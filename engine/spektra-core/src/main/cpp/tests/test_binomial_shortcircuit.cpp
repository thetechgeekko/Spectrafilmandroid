/*
 * Spektrafilm for Android — host test: fast_binomial_one's degenerate-CDF
 * short-circuit is EXACTLY the loop it replaces.
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
 * WHY THIS TEST EXISTS. kernels/stats.cpp returns `n` directly when the binomial
 * CDF-inversion branch is entered with pow(1-p,n) underflowed to 0, instead of
 * walking the loop to k = n+1. That is worth ~8.4x on the whole grain stage at
 * export resolution, and it is only safe because of an argument (the accumulator
 * can never advance, and the loop draws no randomness). The two grain tests
 * cannot check that argument — they are STATISTICAL (mean preservation + noise
 * std within 15%), so they would pass just as happily if the sampler were subtly
 * wrong. This test is the gate the optimisation actually needs.
 *
 * WHAT IT CHECKS. Against `reference_binomial` below — a verbatim transcription
 * of fast_binomial_one as it stood BEFORE the short-circuit (same structure as
 * test_fft_convolve's direct-loop reference) — for every case:
 *   1. the returned variate is identical, and
 *   2. the RNG STREAM is identical afterwards, proven by drawing further
 *      uniforms from both engines and comparing them. A short-circuit that
 *      swallowed or added a draw would desynchronise every later pixel, so
 *      checking the return value alone would not be enough.
 *
 * OUTPUT VOCABULARY: the parity runner marks a test failed when its stdout
 * matches /fail/i, so nothing on the SUCCESS path may contain that substring —
 * a summary counter named "failures=0" is enough to fail the gate. Hence
 * "mismatches" below.
 *
 * MUTATION-CHECKED. `return n-1`, an extra RNG draw before returning, and a
 * short-circuit loosened to `prob < 1e-300` are all caught. Two mutations
 * survive, both understood rather than left as mysteries:
 *   - `prob < 1e-320` survives at the shipping flags ONLY. -ffast-math enables
 *     flush-to-zero, so every denormal prob is already exactly 0.0 there and the
 *     two conditions are the same program. It is caught at -O2, which is why the
 *     parity job runs both legs.
 *   - the `u > 0.0` guard, below.
 *
 * NOT COVERED, deliberately and stated rather than papered over: the `u > 0.0`
 * half of the guard. uniform() would have to return exactly 0.0 for it to
 * matter (probability ~2^-53 per draw), and no seed search can be relied on to
 * produce that, so dropping that half of the condition survives this test. The
 * guard is kept anyway because it is what makes the replacement exact rather
 * than merely almost-exact: with u == 0.0 the original loop exits at k = 0 and
 * returns -1, and that is preserved.
 *
 * The sweep deliberately covers the degenerate region (large n with p pinned
 * near 1, which is what a maximum-density pixel produces once `saturation`
 * collapses and the Poisson rate explodes), the ordinary small-p CDF region
 * that must still walk the loop, the normal-approximation and Bernoulli
 * branches, and the exact threshold boundaries between them.
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "kernels/stats.h"

namespace {

// VERBATIM transcription of fast_binomial_one before the short-circuit landed.
// Do not "simplify" this — its value is that it is the original text.
int64_t reference_binomial(int64_t n, double p, spk::StatsRng& rng) {
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
        int64_t a = static_cast<int64_t>(std::llround(approx));
        if (a < 0) a = 0;
        if (a > n) a = n;
        return a;
    }
    double u = rng.uniform();
    double cdf = 0.0;
    double prob = std::pow(1.0 - p, static_cast<double>(n));
    int64_t k = 0;
    while (cdf < u && k <= n) {
        cdf += prob;
        if (k < n)
            prob = prob * (static_cast<double>(n - k) / (k + 1)) * (p / (1.0 - p));
        k += 1;
    }
    return k - 1;
}

int g_fail = 0;
long long g_cases = 0, g_degenerate = 0, g_walked = 0, g_tiny_nonzero = 0;

// One case: identical seeds, one call each, then compare the variate AND the
// next few draws from each engine (proving the streams stayed in lockstep).
void check(int64_t n, double p, uint64_t seed, const char* label) {
    spk::StatsRng ra(seed), rb(seed);
    int64_t a = reference_binomial(n, p, ra);
    int64_t b = spk::fast_binomial_one(n, p, rb);
    ++g_cases;
    // Classify: did the reference take the degenerate walk?
    if (n >= 25 && p > 0.0 && p < 1.0) {
        double var = static_cast<double>(n) * p * (1.0 - p);
        if (var <= 10.0) {
            double pr = std::pow(1.0 - p, static_cast<double>(n));
            if (pr == 0.0) {
                ++g_degenerate;
            } else {
                ++g_walked;
                // The case that separates "prob == 0" from "prob is small":
                // the loop must still run and must NOT return n.
                if (pr < 1e-280) ++g_tiny_nonzero;
            }
        }
    }
    bool ok = (a == b);
    for (int i = 0; i < 8 && ok; ++i) {
        if (ra.uniform() != rb.uniform()) ok = false;
        if (ra.normal() != rb.normal()) ok = false;
    }
    if (!ok) {
        ++g_fail;
        if (g_fail <= 20)
            std::printf("FAIL %-22s n=%lld p=%.17g seed=%llu  ref=%lld new=%lld%s\n",
                        label, static_cast<long long>(n), p,
                        static_cast<unsigned long long>(seed),
                        static_cast<long long>(a), static_cast<long long>(b),
                        a == b ? "  (stream diverged)" : "");
    }
}

}  // namespace

int main() {
    // --- 1. The degenerate region: large n, p pinned near 1. This is what a
    //        maximum-density pixel produces, and it is 99.62% of all CDF-branch
    //        loop iterations at 12.58 MP export geometry.
    const int64_t big_n[] = {25, 26, 31, 64, 100, 257, 1000, 2774, 8192,
                             30877, 41643, 65536, 200000};
    for (int64_t n : big_n)
        for (double q : {1e-9, 1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3, 1.0 / 40000})
            for (uint64_t s : {1u, 7u, 12345u, 99991u})
                check(n, 1.0 - q, s, "degenerate p->1");

    // --- 2. The ordinary small-p CDF region, which must still walk the loop.
    for (int64_t n : big_n)
        for (double p : {1e-9, 1e-7, 1e-5, 1e-4, 1e-3, 0.002, 0.01, 0.05})
            for (uint64_t s : {2u, 8u, 4242u})
                check(n, p, s, "small-p CDF");

    // --- 3. Threshold boundaries: n at 24/25/26 and var straddling 10.
    for (int64_t n : {24, 25, 26}) {
        for (double p : {0.0, 1e-12, 0.001, 0.3, 0.5, 0.7, 0.999999999999, 1.0})
            for (uint64_t s : {3u, 11u}) check(n, p, s, "n threshold");
    }
    for (int64_t n : {100, 1000, 10000}) {
        // p such that n*p*(1-p) sits just either side of 10.
        for (double target : {9.5, 9.99, 10.0, 10.01, 10.5}) {
            double disc = 1.0 - 4.0 * target / static_cast<double>(n);
            if (disc < 0.0) continue;
            double lo = (1.0 - std::sqrt(disc)) / 2.0;
            double hi = (1.0 + std::sqrt(disc)) / 2.0;
            for (uint64_t s : {5u, 13u}) {
                check(n, lo, s, "var boundary lo");
                check(n, hi, s, "var boundary hi");
            }
        }
    }

    // --- 4. THE SHARP EDGE: prob tiny but NOT zero. The short-circuit is exact
    //        only for pow(1-p,n) == exactly 0; if prob is merely very small the
    //        loop still terminates at k ~ n*p, which is NOT n. Without these
    //        cases the sweep cannot tell the two apart — a short-circuit written
    //        as `prob < 1e-300` passed every other section of this test.
    //        Constructed so that n*log10(1/(1-p)) straddles the ~324-decade
    //        underflow edge, with n*(1-p) small enough to keep var <= 10 and so
    //        stay in the CDF branch.
    for (double q : {1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7}) {
        const double decades = -std::log10(q);
        for (double target = 280.0; target <= 350.0; target += 2.0) {
            int64_t n = static_cast<int64_t>(target / decades);
            if (n < 25) continue;
            double p = 1.0 - q;
            if (static_cast<double>(n) * p * (1.0 - p) > 10.0) continue;
            for (uint64_t s : {17u, 23u, 31u}) check(n, p, s, "underflow edge");
        }
    }

    // --- 5. Bulk randomised sweep over the whole (n, p) space, including the
    //        p values grain actually clips to (1e-6 and 1-1e-6).
    {
        spk::StatsRng pick(20260830);
        for (int i = 0; i < 20000; ++i) {
            int64_t n = static_cast<int64_t>(1 + pick.uniform() * 50000);
            double p = pick.uniform();
            if (i % 4 == 0) p = 1.0 - 1e-6;         // grain's upper clip
            else if (i % 4 == 1) p = 1e-6;          // grain's lower clip
            else if (i % 4 == 2) p = 1.0 - pick.uniform() * 1e-3;
            check(n, p, static_cast<uint64_t>(i), "random sweep");
        }
    }

    std::printf("cases=%lld  degenerate-walk=%lld  genuine-walk=%lld  "
                "tiny-but-nonzero-prob=%lld  mismatches=%d\n",
                g_cases, g_degenerate, g_walked, g_tiny_nonzero, g_fail);
    if (g_degenerate == 0) {
        std::printf("FAIL: the sweep never entered the degenerate branch, so it "
                    "does not gate the short-circuit at all\n");
        return 1;
    }
    if (g_walked == 0) {
        std::printf("FAIL: the sweep never took the genuine CDF walk, so it does "
                    "not prove the loop still runs when it must\n");
        return 1;
    }
    if (g_tiny_nonzero == 0) {
        std::printf("FAIL: the sweep never produced a tiny-but-nonzero pow(1-p,n), "
                    "so it cannot distinguish `prob == 0` from `prob is small` — "
                    "which is the entire basis of the short-circuit\n");
        return 1;
    }
    std::printf("%s\n", g_fail ? "FAIL" : "PASS: short-circuit is exact "
                "(variate and RNG stream) on every case");
    return g_fail ? 1 : 0;
}
