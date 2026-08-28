/*
 * Spektrafilm for Android — Highway SIMD lanes for the f32 separable-FIR Gaussian.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Film modeling powered by spektrafilm.
 *
 * OPT-IN (CMake SPK_ENABLE_HIGHWAY, default OFF; #124, plan in
 * docs/research/highway-vendoring.md). Without it this whole translation unit is
 * empty and `available()` is false, so the library is byte-identical to today.
 *
 * WHY THESE TWO ROUTINES: the f32 FIR Gaussian (kernels/gaussian.cpp) is the blur
 * behind the STOCHASTIC tier — model/grain.cpp calls it four times per grainy
 * render (dye-cloud blur, clumping, micro-structure, sublayers) and model/glare.cpp
 * once. Grain is ON by default in interactive previews, which is exactly where the
 * device validation (#146) found the time going. The f64 spatial tier
 * (kernels/exponential_filter.cpp) is deliberately NOT ported: Highway gives f64
 * two lanes on arm64 and none on armv7 — precisely what kernels/exp10.h already
 * delivers (research §3).
 *
 * BIT-IDENTITY: both routines vectorise ACROSS OUTPUT PIXELS. Each output's
 * accumulation order over the kernel taps is unchanged from the scalar loop, and
 * neither routine uses a fused multiply-add (plain Mul then Add), so the SIMD path
 * is an exact restatement of the scalar arithmetic rather than an approximation.
 * tests/test_gaussian_hwy.cpp asserts that byte-for-byte.
 */
#ifndef SPK_KERNELS_GAUSSIAN_HWY_H
#define SPK_KERNELS_GAUSSIAN_HWY_H

namespace spk {
namespace hwy_fir {

// True when Highway was compiled in AND not disabled at runtime. Runtime control
// is the env var SPK_SIMD ("0"/"off"/"scalar" disable it), read once, so a single
// binary can A/B the two paths on a device without a rebuild.
bool available();

// The name of the compiled-in Highway target ("NEON_WITHOUT_AES", "SSE4", ...),
// or "none" when not compiled in. For benchmark/diagnostic output.
const char* target_name();

// Vertical FIR accumulate for one tap: row[j] += src[j] * kw, for j in [0, m).
// Scalar-equivalent, no FMA.
void vertical_accum(float* row, const float* src, int m, float kw);

// Horizontal FIR over the INTERIOR span [j0, j1) (no boundary reflection needed):
//   out[j] = sum_{k=-radius..radius} src[j + k] * kernel[k + radius]
// `kernel` has 2*radius+1 taps. Scalar-equivalent tap order, no FMA.
void horizontal_interior(const float* src, const float* kernel, int radius,
                         int j0, int j1, float* out);

}  // namespace hwy_fir
}  // namespace spk

#endif  // SPK_KERNELS_GAUSSIAN_HWY_H
