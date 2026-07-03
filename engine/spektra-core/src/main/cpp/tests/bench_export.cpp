/*
 * On-device full-res EXPORT benchmark for Spektrafilm (LOCAL tool, not a gate).
 * GPLv3. Film modeling powered by spektrafilm.
 *
 * Measures the real export pipeline end-to-end minus the JNI/Kotlin shell:
 *   make_gradient (synthetic linear-ProPhoto input)
 *     -> cold spk_simulate  (fresh engine/rep; full recompute)
 *     -> float32 -> uint16 quantize
 *     -> writePng16ToMemory (zlib deflate, the shipping PNG16 writer)
 *     -> writeTiff16ToMemory None + PackBits (the shipping TIFF16 writer)
 *
 * A config matrix isolates each SERIAL spatial/stochastic effect (halation,
 * scanner unsharp, DIR diffusion, camera diffusion, grain) against a pointwise
 * floor, so the delta = the serial cost that the optimization targets. Cold
 * timing (fresh engine/rep) keeps the tc_lut build a constant offset across
 * configs, so the deltas isolate spatial cost cleanly.
 *
 * ---------------------------------------------------------------------------
 * BUILD (arm64 standalone, from the cpp root; NDK r27 clang, git-bash on Windows):
 *   CPP=engine/spektra-core/src/main/cpp ; cd "$CPP"
 *   NDK=.../ndk/27.0.12077973/toolchains/llvm/prebuilt/windows-x86_64/bin
 *   CXX="$NDK/aarch64-linux-android24-clang++.cmd"   # .cmd wrapper on Windows
 *   PNG=../../../../../lib/pngwriter/src/main/cpp ; TIFF=../../../../../lib/tiffwriter/src/main/cpp
 *   "$CXX" -std=c++17 -O3 -ffast-math -fno-finite-math-only -static-libstdc++ \
 *     -Wl,-z,max-page-size=16384 -I. -I"$PNG" -I"$TIFF" \
 *     tests/bench_export.cpp spektra.cpp kernels/*.cpp io/*.cpp model/*.cpp \
 *     profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp \
 *     "$PNG/png_writer.cpp" "$TIFF/tiff_writer.cpp" \
 *     -lz -landroid -o bench_export_arm64
 *   # NOTE: -landroid is REQUIRED (__ANDROID__ pulls in the AAssetManager path).
 *
 * DEPLOY + RUN (use PowerShell for adb on Windows — git-bash mangles /data/... to
 * C:/Program Files/Git/data/...):
 *   adb push bench_export_arm64 /data/local/tmp/
 *   adb push engine/spektra-core/src/main/assets/spektra /data/local/tmp/spektra
 *   adb shell chmod 755 /data/local/tmp/bench_export_arm64
 *   adb shell "cd /data/local/tmp && SPK_NUM_THREADS=8 BENCH_REPS=3 \
 *              ./bench_export_arm64 /data/local/tmp/spektra 4032 3024"
 *   # env: BENCH_LEAN=1 -> only FULL-scan all-ON + minus-grain (feasible at 12MP).
 *   # argv: <asset_dir> [W H].  Measured results: docs/EXPORT_PERF_2026-07-02.md.
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "spektra.h"
#include "kernels/parallel.h"       // spk::parallel_num_threads()
#include "png_writer.h"             // spectrafilm::writePng16ToMemory
#include "tiff_writer.h"            // spectrafilm::writeTiff16ToMemory

using clock_t_ = std::chrono::steady_clock;

static double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// Deterministic synthetic gradient (linear ProPhoto), ~[0.02,0.92]. From bench_stages.
static std::vector<float> make_gradient(int w, int h) {
    std::vector<float> img((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 3;
            float u = (w > 1) ? (float)x / (w - 1) : 0.f;
            float v = (h > 1) ? (float)y / (h - 1) : 0.f;
            img[i + 0] = 0.02f + 0.90f * u;
            img[i + 1] = 0.02f + 0.90f * v;
            img[i + 2] = 0.02f + 0.90f * (1.0f - 0.5f * (u + v));
        }
    return img;
}

// Base params with ALL spatial + stochastic effects OFF => pointwise floor
// (this is the already-parallel/SIMD path; everything below it is the target).
static spk_params pointwise_floor(int scan_film) {
    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);
    p.auto_exposure = 1;             // realistic: AE on (matches real export; AE-off drives
                                     // synthetic densities to extremes -> grain pathology)
    p.grain_active = 0;
    p.halation_active = 0;
    p.glare_active = 0;
    p.print_glare_active = 0;
    p.camera_diffusion_active = 0;
    p.enlarger_diffusion_active = 0;
    p.scanner_unsharp[0] = 0.0f; p.scanner_unsharp[1] = 0.0f;
    p.scanner_lens_blur = 0.0f;
    p.lens_blur_um = 0.0f;
    p.enlarger_lens_blur = 0.0f;
    p.dir_diffusion_size_um = 0.0f; // pointwise DIR only
    p.scan_film = scan_film;
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    return p;
}

// Cold whole-simulate median ms (fresh engine/rep, always a full recompute).
// Leaves the last output in *out_keep (caller frees) for the writer phase.
static double cold_sim_ms(const char* asset, const spk_image* in, const spk_params* p,
                          int reps, spk_image* out_keep) {
    if (out_keep) *out_keep = spk_image{};
    std::vector<double> ts;
    for (int r = -1; r < reps; ++r) {          // r==-1 is an untimed warmup
        spk_engine* e = nullptr;
        if (spk_engine_create(asset, &e) != SPK_OK || !e) continue;
        spk_image o{};
        auto t0 = clock_t_::now();
        spk_status st = spk_simulate(e, in, p, &o);
        auto t1 = clock_t_::now();
        if (r >= 0 && st == SPK_OK)
            ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (out_keep && r == reps - 1 && st == SPK_OK) {
            *out_keep = o;                     // keep last; caller frees
        } else if (o.data) {
            spk_image_free(&o);
        }
        spk_engine_destroy(e);
    }
    return median(ts);
}

template <typename F>
static double warm_med_ms(int reps, F&& fn) {
    fn();                                       // warmup
    std::vector<double> ts;
    for (int r = 0; r < reps; ++r) {
        auto t0 = clock_t_::now();
        fn();
        auto t1 = clock_t_::now();
        ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return median(ts);
}

int main(int argc, char** argv) {
    const char* asset = argc > 1 ? argv[1] : "/data/local/tmp/spektra";
    int W = argc > 2 ? std::atoi(argv[2]) : 4032;   // 12 MP default (S24 main sensor)
    int H = argc > 3 ? std::atoi(argv[3]) : 3024;
    int reps = 3;
    if (const char* e = std::getenv("BENCH_REPS")) { int n = std::atoi(e); if (n >= 1) reps = n; }
    const char* thr = std::getenv("SPK_NUM_THREADS");

    // Validate asset dir up front.
    { spk_engine* pr = nullptr;
      if (spk_engine_create(asset, &pr) != SPK_OK || !pr) {
          std::fprintf(stderr, "engine create failed (asset '%s')\n", asset); return 2; }
      spk_engine_destroy(pr); }

    std::vector<float> rgb = make_gradient(W, H);
    spk_image in{rgb.data(), W, H, (int)SPK_CS_PROPHOTO};
    const double MP = (double)W * H / 1e6;

    std::printf("bench_export — Spektrafilm full-res EXPORT pipeline (LOCAL)\n");
    std::printf("image:   %dx%d = %.1f MP synthetic gradient (linear ProPhoto)\n", W, H, MP);
    std::printf("threads: %d (SPK_NUM_THREADS=%s)   reps: %d (+1 warmup), median ms\n",
                spk::parallel_num_threads(), thr ? thr : "unset", reps);

    // ---- Config matrix: pointwise floor + one serial effect at a time -------
    struct Cfg { const char* name; spk_params p; };
    std::vector<Cfg> cfgs;
    { Cfg c{"pointwise floor (all spatial OFF)", pointwise_floor(1)}; cfgs.push_back(c); }
    { spk_params p = pointwise_floor(1); p.halation_active = 1;
      cfgs.push_back({"+ halation (serial blur)", p}); }
    { spk_params p = pointwise_floor(1); p.scanner_unsharp[0]=0.7f; p.scanner_unsharp[1]=0.7f;
      cfgs.push_back({"+ scanner unsharp (serial blur)", p}); }
    { spk_params p = pointwise_floor(1); p.dir_diffusion_size_um = 20.0f;
      cfgs.push_back({"+ DIR diffusion (serial blur)", p}); }
    { spk_params p = pointwise_floor(1); p.camera_diffusion_active = 1;
      cfgs.push_back({"+ camera diffusion (serial blur)", p}); }
    { spk_params p = pointwise_floor(1); p.grain_active = 1;
      cfgs.push_back({"+ grain (serial, stochastic)", p}); }
    { spk_params p{}; p.film_profile="kodak_portra_400"; p.print_profile="kodak_portra_endura";
      spk_default_params(&p); p.scan_film=1; p.output_color_space=SPK_CS_SRGB; p.output_cctf_encoding=1;
      cfgs.push_back({"FULL default export, scan route (all ON)", p}); }
    { spk_params p{}; p.film_profile="kodak_portra_400"; p.print_profile="kodak_portra_endura";
      spk_default_params(&p); p.scan_film=0; p.output_color_space=SPK_CS_SRGB; p.output_cctf_encoding=1;
      cfgs.push_back({"FULL default export, PRINT route (all ON)", p}); }

    // BENCH_LEAN: only the two FULL-scan configs (all-ON + minus-grain) so a real
    // 12MP run is feasible (skips the pathological camera-diffusion isolation).
    if (std::getenv("BENCH_LEAN")) {
        spk_params pf{}; pf.film_profile="kodak_portra_400"; pf.print_profile="kodak_portra_endura";
        spk_default_params(&pf); pf.scan_film=1; pf.output_color_space=SPK_CS_SRGB; pf.output_cctf_encoding=1;
        spk_params pg = pf; pg.grain_active = 0;
        cfgs.clear();
        cfgs.push_back({"FULL default export, scan route (all ON)", pf});
        cfgs.push_back({"FULL scan MINUS grain", pg});
    }

    std::printf("\n%-44s | %9s\n", "config (cold simulate)", "ms");
    std::printf("---------------------------------------------+----------\n");
    double floor_ms = 0.0;
    spk_image full_out{};   // keep the FULL-scan output for the writer phase
    for (size_t i = 0; i < cfgs.size(); ++i) {
        std::fprintf(stderr, "[sim %zu/%zu] %s ...\n", i+1, cfgs.size(), cfgs[i].name);
        bool keep = (std::strncmp(cfgs[i].name, "FULL default export, scan", 25) == 0);
        double ms = cold_sim_ms(asset, &in, &cfgs[i].p, reps, keep ? &full_out : nullptr);
        if (i == 0) floor_ms = ms;
        char delta[32] = "";
        if (i > 0 && i < 6) std::snprintf(delta, sizeof delta, "  (+%.0f)", ms - floor_ms);
        std::printf("%-44s | %9.1f%s\n", cfgs[i].name, ms, delta);
    }

    // ---- Writer phase: on the real FULL-scan float output --------------------
    if (full_out.data) {
        size_t n = (size_t)full_out.width * full_out.height * 3;
        std::vector<uint16_t> u16(n);
        double q_ms = warm_med_ms(reps, [&]{
            for (size_t k = 0; k < n; ++k) {
                float v = full_out.data[k];
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                u16[k] = (uint16_t)(v * 65535.0f + 0.5f);
            }
        });
        spectrafilm::PngMetadata pmeta;
        spectrafilm::TiffMetadata tmeta;
        std::vector<uint8_t> pbytes, tbytes, tpbytes;
        double png_ms = warm_med_ms(reps, [&]{
            pbytes.clear();
            spectrafilm::writePng16ToMemory(u16.data(), full_out.width, full_out.height, pmeta, pbytes);
        });
        double tiff_ms = warm_med_ms(reps, [&]{
            tbytes.clear();
            spectrafilm::writeTiff16ToMemory(u16.data(), full_out.width, full_out.height, tmeta,
                                             spectrafilm::TiffCompression::None, tbytes);
        });
        double tiffp_ms = warm_med_ms(reps, [&]{
            tpbytes.clear();
            spectrafilm::writeTiff16ToMemory(u16.data(), full_out.width, full_out.height, tmeta,
                                             spectrafilm::TiffCompression::PackBits, tpbytes);
        });
        std::printf("\n%-44s | %9s | %10s\n", "export writer phase (on FULL-scan output)", "ms", "MB out");
        std::printf("---------------------------------------------+----------+-----------\n");
        std::printf("%-44s | %9.1f | %10s\n", "float32 -> uint16 quantize (serial)", q_ms, "-");
        std::printf("%-44s | %9.1f | %10.1f\n", "PNG16 writePng16ToMemory (zlib-6 serial)",
                    png_ms, pbytes.size() / 1048576.0);
        std::printf("%-44s | %9.1f | %10.1f\n", "TIFF16 None (uncompressed)",
                    tiff_ms, tbytes.size() / 1048576.0);
        std::printf("%-44s | %9.1f | %10.1f\n", "TIFF16 PackBits",
                    tiffp_ms, tpbytes.size() / 1048576.0);
        spk_image_free(&full_out);
    } else {
        std::fprintf(stderr, "WARN: no FULL-scan output captured; writer phase skipped\n");
    }
    return 0;
}
