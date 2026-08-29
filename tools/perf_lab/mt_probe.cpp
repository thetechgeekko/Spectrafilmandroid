/*
 * Spektrafilm for Android — probe: can MT19937 be vectorised WITHOUT changing a
 * single output bit? Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * WHY. The device run put grain at 3043 ms, the largest stage of a 13.85 s export.
 * A breakdown of the sampler inside it:
 *
 *     fast_poisson_one(lam=29)    160.92 ns/sample
 *       draws consumed             30.00        (Knuth: lam+1)
 *       30 x 4.11 ns            =  123.3 ns     -> 77% of the sample
 *     StatsRng::uniform()           4.11 ns/draw
 *     raw mt19937 word              1.69 ns/word
 *
 * So 77% of grain's Poisson cost is the random number generator, not the sampler.
 *
 * WHY NOT JUST CHANGE THE SAMPLER. Knuth is O(lambda); Hoermann's PTRS/PTRD (1993)
 * is O(1) and is what NumPy uses above lambda=10 and Boost.Random uses. But
 * kernels/stats.cpp deliberately mirrors the oracle's branch structure
 * (fast_stats.py: lambda<30 Knuth, else normal approximation), so switching
 * algorithms changes which numbers come out — a visible change to the grain and a
 * divergence from the parity reference. Off the table.
 *
 * WHY NOT VMT19937. arXiv:2309.16682 reports order-of-magnitude gains and scales
 * linearly with register width, but it gets there by running SEVERAL MT19937
 * instances de-phased via jump-ahead and polling them round-robin. Its stream is
 * NOT the stream a single std::mt19937 emits. Same period and statistics, different
 * numbers — so it is a different generator, not a faster spelling of ours.
 *
 * WHAT IS ACTUALLY AVAILABLE. MT19937's block twist has a dependency distance of
 * N-M = 227 words:
 *
 *   i in [0, 227)   reads mt[i+1] and mt[i+397] — both untouched this block
 *   i in [227, 623) reads mt[i+1] (untouched) and mt[i-227] (written 227 iterations
 *                   earlier, so already final)
 *
 * Anything up to 227 lanes wide can therefore be computed at once and still emit the
 * IDENTICAL sequence. Tempering is elementwise and trivially so. This probe builds
 * that, proves it word-for-word against std::mt19937, and only then times it.
 *
 * Proof first, number second — the f64 Highway lever on this branch was removed
 * precisely because its correctness claim had only ever been checked at -O2.
 *
 * Build (host):
 *   g++ -std=c++17 -O3 -ffast-math -pthread -DSPK_ENABLE_HIGHWAY \
 *       -DHWY_COMPILE_ONLY_STATIC=1 -DHWY_DISABLE_PCLMUL_AES=1 \
 *       -I<cpp> -I<highway> tools/perf_lab/mt_probe.cpp <highway>/hwy/*.cc -o mt_probe
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef SPK_ENABLE_HIGHWAY
#include "hwy/highway.h"
#include "kernels/stats.h"
#endif

namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int kN = 624;
constexpr int kM = 397;
constexpr uint32_t kMatrixA = 0x9908b0dfu;
constexpr uint32_t kUpper = 0x80000000u;
constexpr uint32_t kLower = 0x7fffffffu;

inline uint32_t temper(uint32_t y) {
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

// Scalar MT19937, written out so the SIMD version has something exact to be
// compared against in the same translation unit. Seeding matches the standard
// (and so libstdc++'s std::mt19937(seed)).
struct MtScalar {
    uint32_t mt[kN];
    int idx = kN;

    explicit MtScalar(uint32_t seed) {
        mt[0] = seed;
        for (int i = 1; i < kN; ++i)
            mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + static_cast<uint32_t>(i);
    }

    void twist() {
        int i = 0;
        for (; i < kN - kM; ++i) {
            const uint32_t y = (mt[i] & kUpper) | (mt[i + 1] & kLower);
            mt[i] = mt[i + kM] ^ (y >> 1) ^ ((0u - (y & 1u)) & kMatrixA);
        }
        for (; i < kN - 1; ++i) {
            const uint32_t y = (mt[i] & kUpper) | (mt[i + 1] & kLower);
            mt[i] = mt[i + kM - kN] ^ (y >> 1) ^ ((0u - (y & 1u)) & kMatrixA);
        }
        const uint32_t y = (mt[kN - 1] & kUpper) | (mt[0] & kLower);
        mt[kN - 1] = mt[kM - 1] ^ (y >> 1) ^ ((0u - (y & 1u)) & kMatrixA);
        idx = 0;
    }

    uint32_t next() {
        if (idx >= kN) twist();
        return temper(mt[idx++]);
    }
};

#ifdef SPK_ENABLE_HIGHWAY
namespace hn = hwy::HWY_NAMESPACE;

// Same state, same seeding, same recurrence — computed a vector at a time.
struct MtSimd {
    uint32_t mt[kN];
    int idx = kN;

    explicit MtSimd(uint32_t seed) {
        mt[0] = seed;
        for (int i = 1; i < kN; ++i)
            mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + static_cast<uint32_t>(i);
    }

    // One span of the twist. `off` is the offset added to i to reach the
    // mt[i+M] / mt[i+M-N] operand — the only thing that differs between the two
    // scalar loops. EVERY load happens before ANY store, because the block writes
    // mt[i..i+L) while reading mt[i+1..i+L], and the scalar loop reads the old
    // value there.
    template <class D>
    void twist_span(D d, int lo, int hi, int off) {
        const int L = static_cast<int>(hn::Lanes(d));
        const auto vUpper = hn::Set(d, kUpper);
        const auto vLower = hn::Set(d, kLower);
        const auto vA = hn::Set(d, kMatrixA);
        const auto vOne = hn::Set(d, 1u);
        int i = lo;
        for (; i + L <= hi; i += L) {
            const auto cur = hn::LoadU(d, mt + i);
            const auto nxt = hn::LoadU(d, mt + i + 1);
            const auto far = hn::LoadU(d, mt + i + off);
            const auto y = hn::Or(hn::And(cur, vUpper), hn::And(nxt, vLower));
            // (0 - (y&1)) & MATRIX_A : 0 or MATRIX_A, branchless, exactly as scalar.
            const auto mag = hn::And(hn::Sub(hn::Zero(d), hn::And(y, vOne)), vA);
            hn::StoreU(hn::Xor(hn::Xor(far, hn::ShiftRight<1>(y)), mag), d, mt + i);
        }
        for (; i < hi; ++i) {  // scalar tail
            const uint32_t y = (mt[i] & kUpper) | (mt[i + 1] & kLower);
            mt[i] = mt[i + off] ^ (y >> 1) ^ ((0u - (y & 1u)) & kMatrixA);
        }
    }

    void twist() {
        const hn::ScalableTag<uint32_t> d;
        // Guard the dependency distance rather than assuming it: the second span
        // reads mt[i-227], so a vector wider than 227 would read a word this same
        // block has not written yet.
        if (static_cast<int>(hn::Lanes(d)) <= kN - kM) {
            twist_span(d, 0, kN - kM, kM);
            twist_span(d, kN - kM, kN - 1, kM - kN);
        } else {
            twist_span(hn::ScalableTag<uint32_t>(), 0, 0, 0);  // unreachable today
        }
        const uint32_t y = (mt[kN - 1] & kUpper) | (mt[0] & kLower);
        mt[kN - 1] = mt[kM - 1] ^ (y >> 1) ^ ((0u - (y & 1u)) & kMatrixA);
        idx = 0;
    }

    // Tempering is elementwise, so a whole block can be tempered at once into a
    // caller buffer. This is where the second half of any win would come from.
    template <class D>
    static void temper_block(D d, const uint32_t* src, uint32_t* dst, int n) {
        const int L = static_cast<int>(hn::Lanes(d));
        const auto m1 = hn::Set(d, 0x9d2c5680u);
        const auto m2 = hn::Set(d, 0xefc60000u);
        int i = 0;
        for (; i + L <= n; i += L) {
            auto y = hn::LoadU(d, src + i);
            y = hn::Xor(y, hn::ShiftRight<11>(y));
            y = hn::Xor(y, hn::And(hn::ShiftLeft<7>(y), m1));
            y = hn::Xor(y, hn::And(hn::ShiftLeft<15>(y), m2));
            y = hn::Xor(y, hn::ShiftRight<18>(y));
            hn::StoreU(y, d, dst + i);
        }
        for (; i < n; ++i) dst[i] = temper(src[i]);
    }

    // Hand words out of a PRE-TEMPERED buffer. The first shape of this probe
    // tempered per word inside next(), which threw the whole win away: the block
    // bench read 1.97x while the sampler read 0.81x, because next() was paying
    // scalar tempering plus more bookkeeping than libstdc++'s operator(). The
    // vectorised temper only counts if the sampler actually goes through it.
    // Hand words out of a PRE-TEMPERED buffer. The first shape of this probe
    // tempered per word inside next(), which threw the whole win away: the block
    // bench read 1.97x while the sampler read 0.81x, because next() was paying
    // scalar tempering plus more bookkeeping than libstdc++'s operator(). The
    // vectorised temper only counts if the sampler actually goes through it.
    uint32_t out_[kN];

    uint32_t next() {
        if (idx >= kN) {
            twist();
            const hn::ScalableTag<uint32_t> d;
            temper_block(d, mt, out_, kN);
            idx = 0;
        }
        return out_[idx++];
    }

    // UniformRandomBitGenerator, so this engine can be handed to the very same
    // std::uniform_real_distribution / std::normal_distribution the engine's
    // StatsRng uses. That is what makes "identical doubles" a property of
    // construction rather than a hope.
    using result_type = uint32_t;
    static constexpr uint32_t min() { return 0u; }
    static constexpr uint32_t max() { return 0xffffffffu; }
    uint32_t operator()() { return next(); }
};

// A drop-in for kernels/stats.h's StatsRng, differing ONLY in the engine
// underneath. Same distributions, same order of use.
struct StatsRngSimd {
    explicit StatsRngSimd(uint64_t seed) : engine_(static_cast<uint32_t>(seed)) {}
    double uniform() { return uni_(engine_); }
    double normal() { return nrm_(engine_); }
   private:
    MtSimd engine_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::normal_distribution<double> nrm_{0.0, 1.0};
};

// Transcribed from kernels/stats.cpp so the probe measures the engine's actual
// branch structure rather than an idealised one. Templated only on the RNG.
template <class Rng>
int64_t poisson_one(double lam, Rng& rng) {
    const double lam_threshold = 30.0;
    if (lam <= 0.0) return 0;
    if (lam < lam_threshold) {
        double L = std::exp(-lam);
        double p = 1.0;
        int64_t k = 0;
        while (p > L) { k += 1; p *= rng.uniform(); }
        return k - 1;
    }
    double z = rng.normal();
    double sample = lam + std::sqrt(lam) * z;
    int64_t s = static_cast<int64_t>(std::llround(sample));
    return s < 0 ? 0 : s;
}
#endif  // SPK_ENABLE_HIGHWAY

}  // namespace

int main() {
    const uint32_t kSeed = 20260829u;
    const int kProof = 10000000;  // ~16k blocks — well past any single-block artefact

#ifdef SPK_ENABLE_HIGHWAY
    std::printf("highway target=%s u32_lanes=%d (twist dependency distance is %d)\n",
                hwy::TargetName(HWY_TARGET),
                static_cast<int>(hn::Lanes(hn::ScalableTag<uint32_t>())), kN - kM);
#else
    std::printf("built WITHOUT Highway — scalar reference only\n");
#endif

    // ---- PROOF 1: the scalar transcription is really std::mt19937 -------------
    {
        std::mt19937 ref(kSeed);
        MtScalar mine(kSeed);
        long long bad = 0;
        for (int i = 0; i < kProof; ++i)
            if (ref() != mine.next()) ++bad;
        std::printf("%s: transcription == std::mt19937 over %d words (%lld diffs)\n",
                    bad == 0 ? "ok" : "FAIL", kProof, bad);
        if (bad) return 1;
    }

#ifdef SPK_ENABLE_HIGHWAY
    // ---- PROOF 2: the SIMD twist emits the identical stream -------------------
    {
        std::mt19937 ref(kSeed);
        MtSimd mine(kSeed);
        long long bad = 0;
        uint32_t first_bad_at = 0;
        for (int i = 0; i < kProof; ++i) {
            const uint32_t a = ref(), b = mine.next();
            if (a != b) { if (!bad) first_bad_at = static_cast<uint32_t>(i); ++bad; }
        }
        std::printf("%s: SIMD twist == std::mt19937 over %d words (%lld diffs%s)\n",
                    bad == 0 ? "ok" : "FAIL", kProof, bad,
                    bad ? (std::string(", first at ") + std::to_string(first_bad_at)).c_str() : "");
        if (bad) {
            std::printf("mt_probe: FAIL — no speed number is worth reporting\n");
            return 1;
        }
    }
#endif

    // ---- Only now, speed ------------------------------------------------------
    const int kBench = 50000000;
    double t, ms_std, ms_scalar;
    {
        std::mt19937 e(kSeed);
        volatile uint32_t sink = 0;
        t = now_ms();
        for (int i = 0; i < kBench; ++i) sink += e();
        ms_std = now_ms() - t;
    }
    {
        MtScalar e(kSeed);
        volatile uint32_t sink = 0;
        t = now_ms();
        for (int i = 0; i < kBench; ++i) sink += e.next();
        ms_scalar = now_ms() - t;
    }
    std::printf("\nstd::mt19937 word          %6.2f ns/word\n", ms_std * 1e6 / kBench);
    std::printf("scalar transcription       %6.2f ns/word  (%.2fx)\n",
                ms_scalar * 1e6 / kBench, ms_std / ms_scalar);

#ifdef SPK_ENABLE_HIGHWAY
    {
        MtSimd e(kSeed);
        volatile uint32_t sink = 0;
        t = now_ms();
        for (int i = 0; i < kBench; ++i) sink += e.next();
        const double ms = now_ms() - t;
        std::printf("SIMD twist, scalar temper  %6.2f ns/word  (%.2fx vs std)\n",
                    ms * 1e6 / kBench, ms_std / ms);
    }
    // Block form: twist AND temper vectorised, words handed out from a buffer.
    // This is the shape a real integration would take, and it isolates how much
    // of the cost is per-word bookkeeping rather than generation.
    {
        const hn::ScalableTag<uint32_t> d;
        MtSimd e(kSeed);
        std::vector<uint32_t> out(kN);
        volatile uint32_t sink = 0;
        t = now_ms();
        for (int done = 0; done < kBench; done += kN) {
            e.twist();
            MtSimd::temper_block(d, e.mt, out.data(), kN);
            for (int i = 0; i < kN; ++i) sink += out[i];
        }
        const double ms = now_ms() - t;
        std::printf("SIMD twist + SIMD temper   %6.2f ns/word  (%.2fx vs std)\n",
                    ms * 1e6 / kBench, ms_std / ms);
    }
#endif
    // ---- The number that decides it: the SAMPLER, not the generator ---------
    // A 2x on words only matters in proportion to how much of a sample is words.
    {
        const int kS = 2000000;
        // Equality first. Same seed, same distributions, same branch structure --
        // so every drawn sample must match exactly, not approximately.
        {
            spk::StatsRng a(4242u);
            StatsRngSimd b(4242u);
            long long bad = 0;
            for (int i = 0; i < 200000; ++i) {
                const double lam = 1.0 + static_cast<double>((i * 37) % 600);
                if (poisson_one(lam, a) != poisson_one(lam, b)) ++bad;
            }
            std::printf("\n%s: poisson samples identical with the SIMD engine "
                        "(%lld diffs over 200000 mixed-lambda draws)\n",
                        bad == 0 ? "ok" : "FAIL", bad);
            if (bad) { std::printf("mt_probe: FAIL\n"); return 1; }
        }
        // Where does a sample's time ACTUALLY go? The earlier breakdown assumed
        // uniform() is essentially two engine words, so a 1.79x on words should
        // have carried most of the way. Measure the layers instead of modelling
        // them.
        {
            const int kD = 20000000;
            double a, b, c;
            { spk::StatsRng r(7); volatile double sink = 0; t = now_ms();
              for (int i = 0; i < kD; ++i) sink += r.uniform(); a = now_ms() - t; }
            { StatsRngSimd r(7); volatile double sink = 0; t = now_ms();
              for (int i = 0; i < kD; ++i) sink += r.uniform(); b = now_ms() - t; }
            { MtSimd e(7); volatile uint32_t sink = 0; t = now_ms();
              for (int i = 0; i < kD; ++i) sink += e.next(); c = now_ms() - t; }
            std::printf("\nuniform(), std engine      %6.2f ns   (= %.2f engine words + wrapper)\n",
                        a * 1e6 / kD, (a * 1e6 / kD) / (ms_std * 1e6 / kBench));
            std::printf("uniform(), SIMD engine     %6.2f ns   (%.2fx)\n",
                        b * 1e6 / kD, a / b);
            std::printf("SIMD engine word alone     %6.2f ns   -> wrapper costs %.2f ns/draw\n",
                        c * 1e6 / kD, b * 1e6 / kD - 2.0 * (c * 1e6 / kD));
        }

        double ms_a, ms_b;
        {
            spk::StatsRng r(7);
            volatile long long sink = 0;
            t = now_ms();
            for (int i = 0; i < kS; ++i) sink += poisson_one(29.0, r);
            ms_a = now_ms() - t;
        }
        {
            StatsRngSimd r(7);
            volatile long long sink = 0;
            t = now_ms();
            for (int i = 0; i < kS; ++i) sink += poisson_one(29.0, r);
            ms_b = now_ms() - t;
        }
        std::printf("fast_poisson_one(lam=29), std engine  %7.2f ns/sample\n",
                    ms_a * 1e6 / kS);
        std::printf("fast_poisson_one(lam=29), SIMD engine %7.2f ns/sample  (%.2fx)\n",
                    ms_b * 1e6 / kS, ms_a / ms_b);
        std::printf("   grain was 3043 ms of a 13.85 s export; scale that ratio\n"
                    "   against it, and remember the sampler is only part of grain.\n");
    }

    std::printf("\nmt_probe: ALL OK\n");
    return 0;
}
