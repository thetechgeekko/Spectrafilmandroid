/*
 * Spektrafilm for Android — host STATISTICAL test for the film-grain model.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 *
 * Why statistical (not element-wise): the AgX particle grain draws Poisson +
 * Binomial variates per pixel. The C++ RNG (std::mt19937) and the Python
 * numpy/Numba RNG produce different streams even with a fixed seed, so the grainy
 * density cannot match element-by-element. What MUST match is the algorithm and
 * its statistics:
 *   1. mean preservation: grain adds developed-particle density and subtracts the
 *      density_min offset; per-channel mean(grainy) ≈ mean(smooth). We require
 *      |Δmean| < 1e-3 for both the C++ output and the oracle.
 *   2. noise magnitude: per-channel std(grainy - smooth) must match the oracle in
 *      magnitude. Tolerance ±15% relative — the empirical std itself has sampling
 *      error at this image size (4096 px, 5 realisations) and the two RNGs visit
 *      different micro-regimes of the same distribution; ±15% is comfortably
 *      tighter than any visible difference yet loose enough to absorb that noise.
 *
 * Inputs:
 *   - smooth (no-grain) density:  goldens/scan_portra/film_density_cmy.spkvec
 *   - oracle grainy realisations: tests/grain_ref_density.spkvec  (S, npix, 3)
 *   (both produced by the Python oracle; the latter via tests/gen_grain_ref.py)
 *
 * Build (host):
 *   g++ -std=c++17 -O2 \
 *     -I <cpp_root> -I <tools/parity> \
 *     tests/test_grain.cpp \
 *     model/grain.cpp kernels/stats.cpp kernels/gaussian.cpp \
 *     -o /tmp/test_grain
 */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "spkvec_io.h"

#include "kernels/stats.h"
#include "model/grain.h"

namespace {

const char* kSmoothGolden =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/scan_portra/"
    "film_density_cmy.spkvec";
const char* kOracleGrainy =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "grain_ref_density.spkvec";

// Digested kodak_portra_400 grain params for the non-sublayer path (from
// gen_grain_ref.py / the Python digest). density_max_curves are the per-channel
// nanmax of the normalized density curves.
constexpr int kSize = 64;
constexpr double kPixelSizeUm = 35.0 * 1000.0 / kSize;  // 546.875
const double kDensityMaxCurves[3] = {1.93501957, 1.75090728, 2.11231201};

struct ChanStats {
    double mean[3];
    double noise_std[3];  // std over space of (x - smooth)
};

// Per-channel mean of an interleaved (npix,3) buffer.
void channel_mean(const std::vector<float>& x, int npix, double out[3]) {
    double s[3] = {0, 0, 0};
    for (int i = 0; i < npix; ++i)
        for (int c = 0; c < 3; ++c) s[c] += x[i * 3 + c];
    for (int c = 0; c < 3; ++c) out[c] = s[c] / npix;
}

// Per-channel std of (x - smooth) over space.
void channel_noise_std(const std::vector<float>& x,
                       const std::vector<float>& smooth, int npix,
                       double out[3]) {
    double s[3] = {0, 0, 0}, s2[3] = {0, 0, 0};
    for (int i = 0; i < npix; ++i)
        for (int c = 0; c < 3; ++c) {
            double d = static_cast<double>(x[i * 3 + c]) - smooth[i * 3 + c];
            s[c] += d;
            s2[c] += d * d;
        }
    for (int c = 0; c < 3; ++c) {
        double m = s[c] / npix;
        out[c] = std::sqrt(std::fmax(s2[c] / npix - m * m, 0.0));
    }
}

// Resource-amplification regression at the sampler seam. When the initial
// binomial probability underflows to exactly zero, the existing inversion walk
// can only return n; walking hundreds of millions of zero-probability terms is
// therefore wasted work. The shortcut must still consume the inversion branch's
// one uniform draw so the following grain samples remain byte-reproducible.
bool sampler_extreme_input_regression() {
    bool pass = true;

    constexpr int64_t kHugeN = 250'000'000;
    constexpr double kNearOneP = 1.0 - 1e-12;
    spk::StatsRng actual_rng(0x170u);
    spk::StatsRng reference_rng(0x170u);
    (void)reference_rng.uniform();  // draw consumed by CDF inversion

    const auto started = std::chrono::steady_clock::now();
    const int64_t underflow_sample =
        spk::fast_binomial_one(kHugeN, kNearOneP, actual_rng);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const bool underflow_ok =
        underflow_sample == kHugeN &&
        actual_rng.uniform() == reference_rng.uniform() &&
        elapsed < std::chrono::milliseconds(500);
    pass = pass && underflow_ok;
    std::printf("Sampler zero-probability shortcut: %s\n",
                underflow_ok ? "PASS" : "FAIL");

    // Invalid or unrepresentable Poisson rates have a deterministic saturated
    // result and must never reach llround(NaN/Inf). Rejected rates consume no RNG.
    spk::StatsRng invalid_rng(0x171u);
    spk::StatsRng invalid_reference_rng(0x171u);
    const bool nonfinite_ok =
        spk::fast_poisson_one(std::numeric_limits<double>::quiet_NaN(), invalid_rng) == 0 &&
        spk::fast_poisson_one(std::numeric_limits<double>::infinity(), invalid_rng) ==
            std::numeric_limits<int64_t>::max() &&
        spk::fast_poisson_one(std::numeric_limits<double>::max(), invalid_rng) ==
            std::numeric_limits<int64_t>::max() &&
        invalid_rng.uniform() == invalid_reference_rng.uniform();
    pass = pass && nonfinite_ok;
    std::printf("Sampler non-finite/overflow rates: %s\n",
                nonfinite_ok ? "PASS" : "FAIL");

    // A finite lambda immediately below 2^63 can still overflow after the
    // normal perturbation. Across several deterministic streams, every result
    // must remain representable and at least one positive perturbation saturates.
    const double near_i64_limit =
        std::nextafter(0x1p63, 0.0);
    bool saw_saturation = false;
    bool finite_overflow_ok = true;
    for (uint64_t seed = 0; seed < 64; ++seed) {
        spk::StatsRng rng(seed);
        const int64_t sample = spk::fast_poisson_one(near_i64_limit, rng);
        finite_overflow_ok = finite_overflow_ok && sample >= 0;
        saw_saturation = saw_saturation ||
                         sample == std::numeric_limits<int64_t>::max();
    }
    finite_overflow_ok = finite_overflow_ok && saw_saturation;
    pass = pass && finite_overflow_ok;
    std::printf("Sampler finite rounded overflow: %s\n",
                finite_overflow_ok ? "PASS" : "FAIL");

    // The public particle-layer seam must degrade to an exact no-op if an
    // invalid particle area/pixel size produced a non-positive or non-finite
    // particles-per-pixel count. This prevents invalid imported parameters from
    // entering the samplers while retaining the original density plane.
    const float density[] = {0.1f, 0.2f, 0.3f, 0.4f};
    constexpr double kInvalidCounts[] = {
        0.0,
        -1.0,
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    bool derived_count_ok = true;
    for (double invalid_count : kInvalidCounts) {
        float output[] = {-1.0f, -1.0f, -1.0f, -1.0f};
        spk::layer_particle_model(density, 4, 2, 2, 2.0, invalid_count,
                                  0.97, 123u, output);
        for (int i = 0; i < 4; ++i)
            derived_count_ok = derived_count_ok && output[i] == density[i];
    }
    pass = pass && derived_count_ok;
    std::printf("Grain invalid derived-count no-op: %s\n",
                derived_count_ok ? "PASS" : "FAIL");

    return pass;
}

}  // namespace

int main(int argc, char** argv) {
    if (!sampler_extreme_input_regression()) return 1;

    std::string smooth_path = argc > 1 ? argv[1] : kSmoothGolden;
    std::string oracle_path = argc > 2 ? argv[2] : kOracleGrainy;

    spkvec::Array smooth_a = spkvec::read(smooth_path);
    spkvec::Array oracle_a = spkvec::read(oracle_path);

    const int npix = static_cast<int>(smooth_a.count() / 3);
    std::vector<float> smooth = smooth_a.data;  // (npix,3) row-major

    // Oracle: (S, npix, 3). Derive per-channel mean + noise std averaged over S.
    const int S = static_cast<int>(oracle_a.shape[0]);
    std::printf("Image: %d px, oracle realisations S=%d, pixel_size_um=%.4f\n",
                npix, S, kPixelSizeUm);

    double oracle_mean[3] = {0, 0, 0}, oracle_nstd[3] = {0, 0, 0};
    for (int s = 0; s < S; ++s) {
        std::vector<float> g(oracle_a.data.begin() + static_cast<size_t>(s) * npix * 3,
                             oracle_a.data.begin() + static_cast<size_t>(s + 1) * npix * 3);
        double m[3], nstd[3];
        channel_mean(g, npix, m);
        channel_noise_std(g, smooth, npix, nstd);
        for (int c = 0; c < 3; ++c) { oracle_mean[c] += m[c]; oracle_nstd[c] += nstd[c]; }
    }
    for (int c = 0; c < 3; ++c) { oracle_mean[c] /= S; oracle_nstd[c] /= S; }

    double smooth_mean[3];
    channel_mean(smooth, npix, smooth_mean);

    // C++ grain: same params, run S realisations with distinct seed offsets so
    // the empirical std is estimated on the same footing as the oracle.
    spk::GrainParams grain;
    grain.active = true;
    grain.agx_particle_area_um2 = 0.2;
    grain.agx_particle_scale[0] = 0.8; grain.agx_particle_scale[1] = 1.0; grain.agx_particle_scale[2] = 2.0;
    grain.density_min[0] = 0.07; grain.density_min[1] = 0.08; grain.density_min[2] = 0.12;
    grain.uniformity[0] = 0.97; grain.uniformity[1] = 0.97; grain.uniformity[2] = 0.99;
    grain.blur = 0.65;
    grain.n_sub_layers = 1;
    for (int c = 0; c < 3; ++c) grain.density_max_curves[c] = kDensityMaxCurves[c];

    double cpp_mean[3] = {0, 0, 0}, cpp_nstd[3] = {0, 0, 0};
    std::vector<float> out(static_cast<size_t>(npix) * 3);
    for (int s = 0; s < S; ++s) {
        grain.seed_offset = 1000 + s * 7;  // distinct deterministic streams
        spk::apply_grain_to_density(smooth.data(), npix, kSize, kSize,
                                    kPixelSizeUm, grain, out.data());
        double m[3], nstd[3];
        channel_mean(out, npix, m);
        channel_noise_std(out, smooth, npix, nstd);
        for (int c = 0; c < 3; ++c) { cpp_mean[c] += m[c]; cpp_nstd[c] += nstd[c]; }
    }
    for (int c = 0; c < 3; ++c) { cpp_mean[c] /= S; cpp_nstd[c] /= S; }

    const char* chan[3] = {"C", "M", "Y"};
    const double tol_mean = 1e-3;       // mean preservation (absolute)
    const double tol_std_rel = 0.15;    // noise magnitude (relative)

    bool pass = true;
    std::printf("\n=== mean preservation (|grainy_mean - smooth_mean| < %.0e) ===\n",
                tol_mean);
    for (int c = 0; c < 3; ++c) {
        double d_cpp = std::fabs(cpp_mean[c] - smooth_mean[c]);
        double d_ora = std::fabs(oracle_mean[c] - smooth_mean[c]);
        bool ok = (d_cpp < tol_mean) && (d_ora < tol_mean);
        pass = pass && ok;
        std::printf("  [%s] smooth=%.6f  cpp=%.6f (|d|=%.2e)  oracle=%.6f (|d|=%.2e)  %s\n",
                    chan[c], smooth_mean[c], cpp_mean[c], d_cpp, oracle_mean[c],
                    d_ora, ok ? "PASS" : "FAIL");
    }

    std::printf("\n=== noise magnitude (|cpp_std/oracle_std - 1| < %.0f%%) ===\n",
                tol_std_rel * 100.0);
    for (int c = 0; c < 3; ++c) {
        double rel = (oracle_nstd[c] > 0.0)
                         ? std::fabs(cpp_nstd[c] / oracle_nstd[c] - 1.0)
                         : (cpp_nstd[c] > 0 ? 1.0 : 0.0);
        bool ok = rel < tol_std_rel;
        pass = pass && ok;
        std::printf("  [%s] cpp_std=%.6f  oracle_std=%.6f  rel_err=%.1f%%  %s\n",
                    chan[c], cpp_nstd[c], oracle_nstd[c], rel * 100.0,
                    ok ? "PASS" : "FAIL");
    }

    // Locality / blur sanity: the lag-1 spatial autocorrelation of the injected
    // noise (grainy - smooth) is a fingerprint of the final cloud-blur sigma
    // (0.65 px here). It must MATCH the oracle's lag-1 autocorrelation: if the
    // C++ ran a different sigma (or skipped the blur), the autocorrelation would
    // diverge. Note the absolute value is small/negative here because the smooth
    // field has hard patch edges and sigma=0.65 barely correlates neighbours —
    // so the meaningful test is C++ vs oracle agreement, not a fixed threshold.
    auto lag1 = [&](const std::vector<float>& field, int c) {
        double m = 0.0;
        for (int i = 0; i < npix; ++i) m += field[i * 3 + c] - smooth[i * 3 + c];
        m /= npix;
        double num = 0.0, den = 0.0;
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x) {
                int i = y * kSize + x;
                double d = (field[i * 3 + c] - smooth[i * 3 + c]) - m;
                den += d * d;
                if (x + 1 < kSize) {
                    int j = y * kSize + (x + 1);
                    double dn = (field[j * 3 + c] - smooth[j * 3 + c]) - m;
                    num += d * dn;
                }
            }
        return den > 0 ? num / den : 0.0;
    };
    std::printf("\n=== blur locality (lag-1 noise autocorr matches oracle, |d|<0.05) ===\n");
    const double tol_ac = 0.05;
    for (int c = 0; c < 3; ++c) {
        double ac_cpp = lag1(out, c);
        // Oracle autocorr averaged over its S realisations.
        double ac_ora = 0.0;
        for (int s = 0; s < S; ++s) {
            std::vector<float> g(oracle_a.data.begin() + static_cast<size_t>(s) * npix * 3,
                                 oracle_a.data.begin() + static_cast<size_t>(s + 1) * npix * 3);
            ac_ora += lag1(g, c);
        }
        ac_ora /= S;
        bool ok = std::fabs(ac_cpp - ac_ora) < tol_ac;
        pass = pass && ok;
        std::printf("  [%s] cpp=%.3f  oracle=%.3f  |d|=%.3f  %s\n", chan[c], ac_cpp,
                    ac_ora, std::fabs(ac_cpp - ac_ora), ok ? "PASS" : "FAIL");
    }

    std::printf("\n%s\n", pass ? "ALL PASS" : "FAILED");
    return pass ? 0 : 1;
}
