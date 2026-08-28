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
 * TWO LANE TIERS live here.
 *
 * f32 (`hwy_fir`) — the FIR Gaussian of kernels/gaussian.cpp, the blur behind the
 * STOCHASTIC tier: model/grain.cpp calls it four times per grainy render (dye-cloud
 * blur, clumping, micro-structure, sublayers) and model/glare.cpp once. Grain is ON
 * by default in interactive previews.
 *
 * f64 (`hwy_f64`) — kernels/exponential_filter.cpp, which is the HALATION path
 * (model/diffusion.cpp:84 and model/couplers.cpp:402 call
 * exponential_filter_per_channel_d). `halation_active` defaults to 1, so this runs
 * on essentially every render, and the on-device validation round (#146) found the
 * filming stage — grain plus halation — holding the preview time that the GPU scan
 * offload could not touch. An earlier revision of this header argued the f64 tier
 * was not worth porting because arm64 gives it only two lanes; that reasoning
 * weighed lane count alone and ignored which stage the device said was hot. Two
 * lanes over a stage that actually runs beats eight over one that does not — and
 * the IIR column sweep below is 4 multiplies and 3 adds per element, so it is
 * arithmetic-bound rather than load-bound. armv7 has no f64 SIMD; there
 * `available()` is still true but the f64 lane count is 1, which costs nothing.
 * MEASURE, then keep or drop — that is what this branch is for.
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

// ---------------------------------------------------------------------------
// f64 lanes for kernels/exponential_filter.cpp (the halation / scatter tier).
//
// Every routine keeps the scalar accumulation ORDER and uses separate Mul and
// Add (never a fused multiply-add), so each is an exact restatement of the loop
// it replaces rather than an approximation. tests/test_exp_filter_hwy.cpp
// asserts that byte-for-byte against the scalar bodies.
// ---------------------------------------------------------------------------
namespace hwy_f64 {

// Number of f64 lanes in the compiled-in target (1 when not compiled in, or on
// an ABI whose SIMD has no f64 — armv7 NEON). Diagnostic only.
int lanes();

// FIR vertical accumulate for one tap: row[j] += src[j] * kw, j in [0, m).
void vertical_accum(double* row, const double* src, int m, double kw);

// FIR horizontal over the reflect-free interior [j0, j1):
//   out[j] = sum_{t=0..2*radius} src[j + t - radius] * kernel[t]
void horizontal_interior(const double* src, const double* kernel, int radius,
                         int j0, int j1, double* out);

// One row-step of the Young & van Vliet vertical IIR sweep, across `mc`
// independent columns. For each column j:
//     val   = B*row[j] + B1*s1[j] + B2*s2[j] + B3*s3[j]   (left-to-right)
//     row[j]= val;  s3[j]=s2[j];  s2[j]=s1[j];  s1[j]=val
// The recurrence runs DOWN rows, so columns are independent and lane-parallel.
// This is the hot loop of the halation blur at the sigmas halation uses.
void iir_step(double* row, double* s1, double* s2, double* s3, int mc, double B,
              double B1, double B2, double B3);

// out[i] += a * src[i], i in [0, n) — the per-component mix of the
// exponential filter's three-Gaussian fit.
void axpy(double* out, const double* src, int n, double a);

}  // namespace hwy_f64
}  // namespace spk

#endif  // SPK_KERNELS_GAUSSIAN_HWY_H
