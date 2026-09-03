/*
 * Spektrafilm for Android — native engine: Poisson + Binomial samplers.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements kernels/stats.h, mirroring fast_stats.py's
 * fast_poisson / fast_binomial per-element branch structure.
 */
#include "kernels/stats.h"

#include <cmath>
#include <limits>

namespace spk {

namespace {

// 2^63 is exactly representable as double, unlike INT64_MAX. Guarding against
// this exclusive upper bound before llround avoids a range-error/unspecified
// conversion result while leaving every representable finite sample unchanged.
constexpr double kInt64UpperExclusive = 0x1p63;

int64_t round_nonnegative_i64(double sample) {
    if (!(sample > 0.0)) return 0;  // non-positive and NaN
    if (!std::isfinite(sample) || sample >= kInt64UpperExclusive)
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(std::llround(sample));
}

}  // namespace

int64_t fast_poisson_one(double lam, StatsRng& rng) {
    // fast_poisson: lam <= 0 -> 0; lam < 30 -> Knuth; else Normal approx.
    const double lam_threshold = 30.0;
    if (!(lam > 0.0)) return 0;  // non-positive and NaN
    if (!std::isfinite(lam) || lam >= kInt64UpperExclusive)
        return std::numeric_limits<int64_t>::max();
    if (lam < lam_threshold) {
        // Knuth: multiply uniforms until the product drops below exp(-lam).
        double L = std::exp(-lam);
        double p = 1.0;
        int64_t k = 0;
        while (p > L) {
            k += 1;
            p *= rng.uniform();
        }
        return k - 1;
    }
    // Normal approximation N(lam, sqrt(lam)), rounded, clamped >= 0.
    double z = rng.normal();
    double sample = lam + std::sqrt(lam) * z;
    return round_nonnegative_i64(sample);
}

int64_t fast_binomial_one(int64_t n, double p, StatsRng& rng) {
    // fast_binomial: p<=0 -> 0; p>=1 -> n; n<25 -> Bernoulli trials; else
    // var>10 -> Normal approx; else CDF inversion.
    if (p <= 0.0) return 0;
    if (p >= 1.0) return n;
    const int64_t n_threshold = 25;
    if (n < n_threshold) {
        int64_t count = 0;
        for (int64_t k = 0; k < n; ++k)
            if (rng.uniform() < p) count += 1;
        return count;
    }
    double mean = static_cast<double>(n) * p;
    double var = static_cast<double>(n) * p * (1.0 - p);
    if (var > 10.0) {
        double z = rng.normal();
        double approx = mean + std::sqrt(var) * z;
        int64_t a = round_nonnegative_i64(approx);
        if (a > n) a = n;
        return a;
    }
    // CDF inversion (matches the Numba while-loop exactly).
    double u = rng.uniform();
    double cdf = 0.0;
    double prob = std::pow(1.0 - p, static_cast<double>(n));
    // DEGENERATE WALK, SHORT-CIRCUITED. If P(X=0) underflowed to exactly 0 the
    // recurrence stays 0 for every k, so `cdf += prob` can never advance and the
    // loop below is GUARANTEED to run to k = n+1 and return n — after up to n
    // iterations that compute nothing. The uniform above is already drawn, so
    // returning n here leaves both the variate and the RNG stream unchanged.
    //
    // Not a corner case: at 12.58 MP export geometry (pixel_size_um 8.545) 55.47%
    // of the calls reaching here have prob == 0, and they account for
    // 40,767,030,234 iterations — 99.62% of every iteration this branch runs,
    // averaging 1892 each (the other 0.38% averages 8.9). The sampler is 94.6% of
    // the sublayer grain stage, and grain is the largest item in an export
    // (perf-lab.md §22: 4340-4540 ms of ~10 s). Coarser pixels are worse, not
    // better — n scales with pixel area, so at 35 um/px the census reads 100% of
    // calls, avg 30877 iterations, max n 41643. Measured 8.4x on the stage (§25).
    //
    // The `u > 0.0` half of the guard is load-bearing and must not be dropped:
    // uniform() can return exactly 0.0, and then `cdf < u` is false at k = 0 so
    // the original returns k-1 = -1. Without the guard this returns n instead —
    // a behaviour change. tests/test_binomial_shortcircuit.cpp gates the rest of
    // this equivalence element-wise, and its mutation check records that this
    // particular half is not reachable by any sweep (~2^-53 per draw), which is
    // exactly why it is written down rather than trusted to a test.
    if (prob == 0.0 && u > 0.0) return n;
    int64_t k = 0;
    while (cdf < u && k <= n) {
        cdf += prob;
        if (k < n)
            prob = prob * (static_cast<double>(n - k) / (k + 1)) * (p / (1.0 - p));
        k += 1;
    }
    return k - 1;
}

double fast_lognormal_one(double mu, double sigma, StatsRng& rng) {
    // fast_lognormal: sigma < 1e-6 -> exp(mu) (no normal consumed); else
    // exp(mu + sigma * z).
    const double sigma_threshold = 1e-6;
    if (sigma < sigma_threshold) return std::exp(mu);
    double z = rng.normal();
    return std::exp(mu + sigma * z);
}

double fast_lognormal_from_mean_std_one(double m, double s, StatsRng& rng) {
    // fast_lognormal_from_mean_std: compute (mu, sigma) from linear-space (m, s),
    // then dispatch to fast_lognormal_one.
    double mu, sigma;
    if (m <= 0.0) {
        mu = 0.0;
        sigma = 0.0;
    } else {
        double sigma2 = std::log(1.0 + (s * s) / (m * m));
        sigma = std::sqrt(sigma2);
        mu = std::log(m) - sigma2 / 2.0;
    }
    return fast_lognormal_one(mu, sigma, rng);
}

}  // namespace spk
