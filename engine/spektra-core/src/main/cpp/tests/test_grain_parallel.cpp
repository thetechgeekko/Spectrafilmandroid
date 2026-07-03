/*
 * Spektrafilm for Android — grain thread-count invariance (determinism) test.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The grain stage's RNG walks a seeded mt19937 in raster order with data-dependent
 * draw counts, so it CANNOT be parallelized across pixels bit-exactly. But both
 * grain paths loop over INDEPENDENT (channel × sublayer) seeded streams:
 *   - apply_grain_to_density_layers : 3 channels × 3 sublayers = 9 streams
 *   - apply_grain_to_density        : 3 channels × n_sub_layers streams
 * Those calls now run concurrently (kernels/parallel.h parallel_tasks), each into
 * a private plane, then accumulate serially in canonical (c, sl) order. That makes
 * the output BYTE-IDENTICAL for any worker count. This test asserts exactly that:
 * SPK_NUM_THREADS=1 vs =8 produce bitwise-equal grain output, for BOTH paths.
 *
 * (test_parallel.cpp covers the sublayer path end-to-end through the full engine;
 * the non-sublayer path is only reached with grain.sublayers_active == False, which
 * the engine's default params never select, so it is covered ONLY here.)
 *
 * Build (host) — from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I. \
 *     tests/test_grain_parallel.cpp \
 *     model/grain.cpp kernels/stats.cpp kernels/gaussian.cpp kernels/parallel.cpp \
 *     -o /tmp/test_grain_parallel
 *   /tmp/test_grain_parallel      # prints PASS / FAIL, exit 0 on all-pass
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "model/grain.h"

namespace {

// Deterministic synthetic densities spanning a range so the samplers exercise
// several RNG regimes (Knuth vs normal-approx Poisson, Bernoulli vs CDF binomial).
float synth(int i, int lane, int nlanes) {
    // Cheap hash → [0, ~2.4); varies per pixel and per (sublayer,channel) lane.
    unsigned h = static_cast<unsigned>(i) * 2654435761u +
                 static_cast<unsigned>(lane) * 40503u;
    return static_cast<float>((h % 2400u) / 1000.0);  // 0.000 .. 2.399
}

spk::GrainParams make_grain() {
    spk::GrainParams g;               // schema defaults (sublayers_active = true)
    g.active = true;
    g.seed_offset = 4242;             // arbitrary non-zero realisation
    return g;
}

bool identical(const char* label, const std::vector<float>& a,
               const std::vector<float>& b) {
    if (a.size() != b.size()) {
        std::printf("[%s] size mismatch -> FAIL\n", label);
        return false;
    }
    bool same = std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    double max_abs = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        if (d < 0) d = -d;
        if (d > max_abs) { max_abs = d; argmax = i; }
    }
    std::printf("[%s] 1-thread vs 8-thread: max_abs=%.3e (worst idx=%zu) -> %s\n",
                label, max_abs, argmax, same ? "PASS" : "FAIL");
    return same;
}

void set_threads(int n) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    setenv("SPK_NUM_THREADS", buf, /*overwrite=*/1);
}

}  // namespace

int main() {
    const int width = 128, height = 128, npix = width * height;
    const double pixel_size_um = 5.0;
    const spk::GrainParams grain = make_grain();

    bool ok = true;

    // ---- Sublayer path: apply_grain_to_density_layers (9 streams) ----
    {
        std::vector<float> layers(static_cast<size_t>(npix) * 9);
        for (int i = 0; i < npix; ++i)
            for (int sl = 0; sl < 3; ++sl)
                for (int c = 0; c < 3; ++c)
                    layers[static_cast<size_t>(i) * 9 + sl * 3 + c] =
                        synth(i, sl * 3 + c, 9);
        // density_max_layers[sl,c] (3×3), sums per channel > any pixel density.
        double dmax_layers[9];
        for (int sl = 0; sl < 3; ++sl)
            for (int c = 0; c < 3; ++c)
                dmax_layers[sl * 3 + c] = 0.9;  // total per channel = 2.7

        std::vector<float> r1(static_cast<size_t>(npix) * 3);
        std::vector<float> r8(static_cast<size_t>(npix) * 3);
        set_threads(1);
        spk::apply_grain_to_density_layers(layers.data(), npix, width, height,
                                           dmax_layers, pixel_size_um, grain,
                                           r1.data());
        set_threads(8);
        spk::apply_grain_to_density_layers(layers.data(), npix, width, height,
                                           dmax_layers, pixel_size_um, grain,
                                           r8.data());
        ok &= identical("grain_layers(9 streams)", r1, r8);
    }

    // ---- Non-sublayer path: apply_grain_to_density (3×n_sub streams) ----
    for (int n_sub : {1, 3}) {
        spk::GrainParams g = grain;
        g.sublayers_active = false;
        g.n_sub_layers = n_sub;
        std::vector<float> cmy(static_cast<size_t>(npix) * 3);
        for (int i = 0; i < npix; ++i)
            for (int c = 0; c < 3; ++c)
                cmy[static_cast<size_t>(i) * 3 + c] = synth(i, c, 3);

        std::vector<float> r1(static_cast<size_t>(npix) * 3);
        std::vector<float> r8(static_cast<size_t>(npix) * 3);
        set_threads(1);
        spk::apply_grain_to_density(cmy.data(), npix, width, height,
                                    pixel_size_um, g, r1.data());
        set_threads(8);
        spk::apply_grain_to_density(cmy.data(), npix, width, height,
                                    pixel_size_um, g, r8.data());
        char label[48];
        std::snprintf(label, sizeof(label), "grain_flat(n_sub=%d)", n_sub);
        ok &= identical(label, r1, r8);
    }

    std::printf("%s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
