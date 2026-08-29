/*
 * Spektrafilm for Android — host per-stage split of the engine. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * PURE LOCAL TOOL. Not a parity gate, not in CI.
 *
 * WHY IT EXISTS. Every per-stage number we have ("grain is 42% of the engine",
 * "halation is 23%") was read off a DEBUG APK. The engine was the one module
 * already compiling at -O2 there, so the SHARES were meaningful -- but the
 * release APK builds the engine at -O3 -ffast-math -fno-finite-math-only, and
 * nobody had checked whether that speeds every stage by the same factor. If it
 * does not, the shares move, and the GPU target list is drawn from the wrong
 * ranking. This measures it directly: same binary source, two flag sets.
 *
 * It also prints the split with the OPTIONAL EFFECTS ON, which no export profile
 * has ever covered (stage_timings_format skips zero slots, so a gated-off filter
 * is invisible rather than shown as zero -- see stage_timer.h).
 *
 * The absolute milliseconds are HOST numbers on x86_64 and do not transfer to the
 * phone. The per-stage RATIO between the two flag sets is a compiler question and
 * largely does; the SHARES are what this is for.
 *
 * Build (host) -- run from engine/spektra-core/src/main/cpp:
 *   g++ -std=c++17 -O2 -pthread -I. \
 *     ../../../../../tools/stage_split/stage_split.cpp \
 *     spektra.cpp gpu/ *.cpp kernels/ *.cpp io/ *.cpp model/ *.cpp \
 *     profiles/ *.cpp runtime/ *.cpp runtime/stages/ *.cpp -o /tmp/stage_split
 *   (and again with -O3 -ffast-math -fno-finite-math-only)
 * Run:
 *   /tmp/stage_split <asset_dir> [side_px] [reps]
 *
 * Use tools/stage_split/run_stage_split.sh to do both legs and diff them.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "spektra.h"

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Deterministic synthetic scene: a smooth gradient with a few hard edges, so the
// spatial stages (halation, diffusion, lens blur, unsharp) have something to do
// and the density curves see the full exposure range.
// SPK_SCENE=wide (default) spans ~8 stops with lum=8.0 specular squares.
// SPK_SCENE=tame spans ~2 stops around midgray with no speculars. The pair exists
// because grain's cost turned out NOT to scale with pixel count, and the first
// suspect was that a wide density range blows up the particle model's work.
std::vector<float> make_scene(int w, int h) {
    const char* mode = std::getenv("SPK_SCENE");
    const bool tame = mode && std::strcmp(mode, "tame") == 0;
    std::vector<float> v(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double fx = static_cast<double>(x) / (w - 1);
            double fy = static_cast<double>(y) / (h - 1);
            double base = tame
                ? 0.184 * std::pow(2.0, -1.0 + 2.0 * fx) * (0.9 + 0.2 * fy)
                : std::pow(2.0, -6.0 + 8.0 * fx) * (0.25 + 0.75 * fy);
            bool spec = !tame && ((x / 64) % 5 == 0) && ((y / 64) % 5 == 0);
            double lum = spec ? 8.0 : base;
            size_t i = (static_cast<size_t>(y) * w + x) * 3;
            v[i + 0] = static_cast<float>(lum * 1.00);
            v[i + 1] = static_cast<float>(lum * (0.55 + 0.40 * fy));
            v[i + 2] = static_cast<float>(lum * (0.30 + 0.55 * fx));
        }
    }
    return v;
}

struct Config {
    const char* name;
    int scan_film;
    int grain;
    int halation;
    int glare;
    float diffusion;     // Black Pro-Mist strength
    float lens_blur_um;
    int highlight_boost;
};

void run(const char* asset_dir, const Config& c, const std::vector<float>& scene,
         int w, int h, int reps) {
    spk_engine* eng = nullptr;
    if (spk_engine_create(asset_dir, &eng) != SPK_OK) {
        std::fprintf(stderr, "engine create failed\n");
        std::exit(2);
    }
    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);
    p.auto_exposure = 0;
    p.exposure_compensation_ev = 0.0f;
    p.density_curve_gamma = 1.0f;
    p.scan_film = c.scan_film;
    p.grain_active = c.grain;
    p.halation_active = c.halation;
    p.glare_active = c.glare;
    p.dir_couplers_active = 1;
    // Optional effects. Each defaults off, which is exactly why none of them has
    // ever appeared in an export profile (stage_timings_format skips zero slots).
    if (c.diffusion > 0.0f) {
        p.camera_diffusion_active = 1;
        p.camera_diffusion_strength = c.diffusion;
    }
    p.lens_blur_um = c.lens_blur_um;
    if (c.highlight_boost) {
        p.halation_boost_ev = 1.0f;   // any nonzero fires the pre-clip boost
    }
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    p.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p.preview_max_size = 0;   // full resolution -- this is the export case

    spk_image in_img{const_cast<float*>(scene.data()), w, h, SPK_CS_PROPHOTO};

    double best = 1e300;
    char tbuf[1024] = {0};
    for (int r = 0; r < reps + 1; ++r) {   // rep 0 is a warmup
        spk_image out{};
        auto t0 = Clock::now();
        spk_status st = spk_simulate(eng, &in_img, &p, &out);
        double dt = ms_since(t0);
        if (st != SPK_OK) {
            std::fprintf(stderr, "simulate failed (%s): %s\n", c.name,
                         spk_status_str(st));
            spk_engine_destroy(eng);
            std::exit(2);
        }
        if (r > 0 && dt < best) {
            best = dt;
            spk_stage_timings(tbuf, sizeof(tbuf));
        }
        spk_image_free(&out);
    }
    std::printf("CASE %s\n", c.name);
    std::printf("  wall_ms %.1f\n", best);
    std::printf("  stages %s\n", tbuf);
    spk_engine_destroy(eng);
}

}  // namespace

int main(int argc, char** argv) {
    const char* asset_dir = argc > 1 ? argv[1] : "../assets/spektra";
    const int side = argc > 2 ? std::atoi(argv[2]) : 1024;
    const int reps = argc > 3 ? std::atoi(argv[3]) : 3;

    std::printf("# stage_split  side=%d reps=%d  assets=%s\n", side, reps, asset_dir);
#if defined(__FAST_MATH__)
    std::printf("# built with -ffast-math\n");
#else
    std::printf("# built WITHOUT -ffast-math\n");
#endif
    const char* nt = std::getenv("SPK_NUM_THREADS");
    std::printf("# SPK_NUM_THREADS=%s\n", nt ? nt : "(unset)");

    std::vector<float> scene = make_scene(side, side);

    const Config cases[] = {
        // The config every export profile so far was taken at.
        {"print  defaults (grain+halation ON, effects off)", 0, 1, 1, 0, 0.0f, 0.0f, 0},
        {"scan   defaults (grain+halation ON, effects off)", 1, 1, 1, 0, 0.0f, 0.0f, 0},
        // The never-measured ones: each optional effect alone, on the print route.
        {"print  + glare",                                   0, 1, 1, 1, 0.0f, 0.0f, 0},
        {"print  + Black Pro-Mist",                          0, 1, 1, 0, 0.5f, 0.0f, 0},
        {"print  + lens blur",                               0, 1, 1, 0, 0.0f, 30.0f, 0},
        {"print  ALL ON",                                    0, 1, 1, 1, 0.5f, 30.0f, 1},
        // Grain isolated: the largest stage, and the GPU candidate.
        {"print  grain OFF (subtract for grain's cost)",     0, 0, 1, 0, 0.0f, 0.0f, 0},
        {"print  halation OFF",                              0, 1, 0, 0, 0.0f, 0.0f, 0},
    };

    for (const Config& c : cases) run(asset_dir, c, scene, side, side, reps);
    return 0;
}
