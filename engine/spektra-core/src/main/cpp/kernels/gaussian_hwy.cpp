/*
 * Spektrafilm for Android — Highway SIMD lanes for the f32 separable-FIR Gaussian.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3 (see gaussian_hwy.h).
 * Film modeling powered by spektrafilm.
 *
 * Compiled only when SPK_ENABLE_HIGHWAY is defined; otherwise this file is an
 * empty translation unit apart from the three stubs below, and the library is
 * byte-identical to a build without Highway.
 */
#include "gaussian_hwy.h"

#ifndef SPK_ENABLE_HIGHWAY

namespace spk {
namespace hwy_fir {
bool available() { return false; }
const char* target_name() { return "none"; }
void vertical_accum(float*, const float*, int, float) {}
void horizontal_interior(const float*, const float*, int, int, int, float*) {}
}  // namespace hwy_fir
}  // namespace spk

#else

#include <cstdlib>
#include <cstring>

#include "hwy/highway.h"

namespace spk {
namespace hwy_fir {
namespace hn = hwy::HWY_NAMESPACE;

// Per-ABI target contract (docs/research/highway-vendoring.md §2). With
// HWY_COMPILE_ONLY_STATIC there is exactly ONE code path per ABI — no CPU
// detection — so every device running a given ABI executes the identical
// instruction sequence. That is what preserves "same APK ABI => same numbers on
// every device"; runtime dispatch would break it (AVX2 lanes on one x86_64
// handset, SSE4 on another). Assert the resolved target so a toolchain change
// that silently moves the baseline fails the BUILD rather than the goldens.
#ifdef __ANDROID__
#if defined(__aarch64__) || defined(__arm__)
static_assert(HWY_TARGET == HWY_NEON_WITHOUT_AES,
              "Android ARM must resolve to HWY_NEON_WITHOUT_AES (research doc §2); "
              "a different target changes the shipped instruction sequence.");
#elif defined(__x86_64__)
static_assert(HWY_TARGET == HWY_SSE4,
              "Android x86_64 must resolve to HWY_SSE4 — this needs "
              "HWY_DISABLE_PCLMUL_AES (the ABI mandates neither AES nor PCLMUL, "
              "so without it Highway silently falls back to SSSE3).");
#endif
#endif  // __ANDROID__

// Runtime kill-switch so one binary can A/B both paths on a device.
// SPK_SIMD=0|off|scalar disables the SIMD lanes. Read once (the value cannot
// change mid-process, and the render path must not pay a getenv per row).
bool available() {
    static const bool enabled = [] {
        const char* v = std::getenv("SPK_SIMD");
        if (!v || !*v) return true;  // compiled in => on unless told otherwise
        return !(std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 ||
                 std::strcmp(v, "scalar") == 0);
    }();
    return enabled;
}

const char* target_name() { return hwy::TargetName(HWY_TARGET); }

void vertical_accum(float* row, const float* src, int m, float kw) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    const auto vkw = hn::Set(d, kw);
    size_t j = 0;
    const size_t mm = static_cast<size_t>(m);
    for (; j + N <= mm; j += N) {
        // row[j] + src[j]*kw with SEPARATE mul and add — a fused multiply-add
        // would round once instead of twice and break the bit-identity claim.
        const auto acc = hn::LoadU(d, row + j);
        const auto v = hn::LoadU(d, src + j);
        hn::StoreU(hn::Add(acc, hn::Mul(v, vkw)), d, row + j);
    }
    for (; j < mm; ++j) row[j] += src[j] * kw;  // scalar tail
}

void horizontal_interior(const float* src, const float* kernel, int radius,
                         int j0, int j1, float* out) {
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    const int taps = 2 * radius + 1;
    int j = j0;
    for (; j + static_cast<int>(N) <= j1; j += static_cast<int>(N)) {
        // Each lane accumulates its own output over the taps in the SAME order
        // as the scalar loop (k ascending), so lane l reproduces out[j+l] exactly.
        auto acc = hn::Zero(d);
        for (int t = 0; t < taps; ++t) {
            const auto v = hn::LoadU(d, src + j + t - radius);
            acc = hn::Add(acc, hn::Mul(v, hn::Set(d, kernel[t])));
        }
        hn::StoreU(acc, d, out + j);
    }
    for (; j < j1; ++j) {  // scalar tail
        float sval = 0.0f;
        for (int t = 0; t < taps; ++t) sval += src[j + t - radius] * kernel[t];
        out[j] = sval;
    }
}

}  // namespace hwy_fir
}  // namespace spk

#endif  // SPK_ENABLE_HIGHWAY
