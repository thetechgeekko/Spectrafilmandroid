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
 * SCOPE: the f32 FIR Gaussian of kernels/gaussian.cpp — the blur behind the
 * STOCHASTIC tier. model/grain.cpp calls it four times per grainy render
 * (dye-cloud blur, clumping, micro-structure, sublayers) and model/glare.cpp
 * once, and grain is ON by default in interactive previews.
 *
 * AN f64 TIER WAS BUILT HERE AND REMOVED. It covered kernels/exponential_filter
 * (the halation path) and was measured on an S26 Ultra: 34.19 ms vs 33.43 ms at
 * 640x480 and 73.50 vs 71.96 at 1024x768 — i.e. ~2% SLOWER with the lanes on,
 * because arm64 NEON gives f64 only two lanes and the IIR is latency-bound on a
 * serial recurrence rather than throughput-bound. It also failed its own
 * byte-equality test under the shipping flags (see below). Slower and unprovable
 * is not a trade worth keeping; docs/research/perf-lab.md §1 records the numbers.
 *
 * NUMERICAL CLAIM — read this before strengthening it again. Both routines
 * vectorise ACROSS OUTPUT PIXELS, keep the scalar tap order, and use plain Mul
 * then Add rather than a fused multiply-add. At -O2 that makes them BYTE-identical
 * to the scalar loop, which tests/test_gaussian_hwy.cpp asserts.
 *
 * Under the flags the engine actually ships with (-O3 -ffast-math, see the
 * engine CMakeLists) that is NOT true and cannot be made true: -ffast-math lets
 * the compiler reassociate and contract the SCALAR reference too, so the two
 * sides differ by one unit in the last place. Measured: max_abs 1.1e-16,
 * max_rel 2.1e-16, on one element per row — about twelve orders of magnitude
 * inside the 1e-4 oracle band, and it does NOT depend on thread count, so the
 * thread-invariance gate is untouched.
 *
 * The original header claimed byte-identity without qualification because the
 * test was only ever run at -O2. It is stated accurately here.
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
