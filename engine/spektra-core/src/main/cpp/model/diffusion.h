/*
 * Spektrafilm for Android — native engine: in-emulsion scatter + halation +
 * the optical diffusion-filter (Black Pro-Mist etc.) PSF stage.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports spektrafilm/model/diffusion.py:
 *   - apply_halation_um (in-emulsion scatter + back-reflection halation), and
 *   - apply_diffusion_filter_um (the optical diffusion-filter PSF: a per-family
 *     core/halo/bloom sum of 2D isotropic exponentials, energy-conserving convex
 *     blend (1-p_s)*image + p_s*(K_s * image)). The camera/enlarger
 *     diffusion_filter params drive it (issue #6 exposed-but-inert params).
 *
 * apply_halation_um / apply_diffusion_filter_um run on the float64 pre-log
 * irradiance `raw` (the spektrafilm pipeline carries the image as float64).
 *
 * Diffusion-filter convolution note: the oracle uses scipy.signal.fftconvolve
 * (mode='same') on a numpy-'reflect'-padded image. fftconvolve is a plain linear
 * convolution computed in the frequency domain; a direct double-precision spatial
 * convolution is numerically equivalent (verified |Δ| ~ 1e-16 vs the oracle, far
 * under the 1e-4 / 1e-5 parity tolerance), so this port uses a direct convolution
 * in double precision and needs no FFT infrastructure.
 */
#ifndef SPK_MODEL_DIFFUSION_H
#define SPK_MODEL_DIFFUSION_H

namespace spk {

// Halation parameters, mirroring the digested
// spektrafilm.runtime.params_schema.HalationParams fields consumed by
// apply_halation_um. Per-channel arrays are RGB-ordered.
struct HalationParams {
    bool active = false;

    // Highlight boost (consumed by apply_highlight_boost; boost_ev==0 default -> no-op).
    double boost_ev = 0.0;
    double boost_range = 0.3;
    double protect_ev = 4.0;

    // In-emulsion scatter (core Gaussian + exponential tail).
    double scatter_amount = 1.0;
    double scatter_spatial_scale = 1.0;
    double scatter_core_um[3] = {0.0, 0.0, 0.0};
    double scatter_tail_um[3] = {0.0, 0.0, 0.0};
    double scatter_tail_weight[3] = {0.0, 0.0, 0.0};

    // Back-reflection halation (multi-bounce Gaussian sum).
    double halation_amount = 1.0;
    double halation_spatial_scale = 1.0;
    double halation_strength[3] = {0.0, 0.0, 0.0};
    double halation_first_sigma_um[3] = {0.0, 0.0, 0.0};
    int halation_n_bounces = 0;
    double halation_bounce_decay = 0.5;
    bool halation_renormalize = true;
};

// apply_highlight_boost: in-place on the float64 raw irradiance image `raw`, shape
// (h, w, 3) row-major channel-interleaved. Ports
// spektrafilm/utils/numba_boost_hightlights.py::boost_highlights, which
// FilmingStage.expose calls on `raw` right after the exposure-compensation scale and
// BEFORE the diffusion filter / lens blur / halation (filming.py:58-60), with the
// default midgray = 0.184. The curve lifts highlights above
// raw_x0 = clip(midgray * 2^protect_ev, 0, max(raw)) by an exponential set so the
// brightest pixel gains `boost_ev` stops:
//   a = 28^(1 - boost_range);  x0 = raw_x0 / max_raw
//   k = (2^boost_ev - 1) / (exp(a(1-x0)) - a(1-x0) - 1)
//   raw > raw_x0:  raw += k*max_raw*(exp(a*dx) - a*dx - 1),  dx = (raw - raw_x0)/max_raw
// It is NOT part of the spatial branch (the oracle's digest does not zero boost_ev
// under deactivate_spatial_effects), so it fires whenever boost_ev > 0. boost_ev <= 0
// (schema/UI default 0) is a strict identity, so default params stay bit-exact. The
// global max(raw) reduction is order-independent, so the result is thread-invariant.
void apply_highlight_boost(double* raw, int w, int h, const HalationParams& params);

// apply_halation_um: in-place on the float64 raw irradiance image `raw`, shape
// (h, w, 3) row-major channel-interleaved. `pixel_size_um` converts the µm
// spatial parameters to pixel sigmas (sigma_px = um * spatial_scale /
// pixel_size_um). If `params.active` is false the image is left untouched. The
// highlight boost is a separate, earlier pass (apply_highlight_boost) in
// FilmingStage.expose; apply_halation_um covers Step 2 (scatter) + Step 3 (halation).
void apply_halation_um(double* raw, int w, int h, const HalationParams& params,
                       double pixel_size_um);

// Diffusion-filter family selector. Values mirror the keys of
// _DIFFUSION_FILTER_SHAPES in spektrafilm/model/diffusion.py.
enum class DiffusionFamily {
    kGlimmerglass = 0,
    kBlackProMist = 1,   // spektrafilm DiffusionFilterParams default family.
    kProMist = 2,
    kCinebloom = 3,
};

// Optical diffusion-filter params, mirroring the consumed fields of
// spektrafilm.runtime.params_schema.DiffusionFilterParams. Defaults match the
// schema so a default-constructed instance is a strict no-op:
//   active=false  -> apply_diffusion_filter_um returns the image untouched.
struct DiffusionFilterParams {
    bool active = false;                                  // schema default false.
    DiffusionFamily family = DiffusionFamily::kBlackProMist;  // "black_pro_mist".
    double strength = 0.5;        // commercial filter stop (interpolated).
    double spatial_scale = 1.0;   // multiplier on image-plane PSF widths.
    double halo_warmth = 0.0;     // additive bias to the family halo-warmth axis.
    // Per-group fine-tune multipliers (advanced). 1.0 = use the family preset.
    double core_intensity = 1.0;
    double core_size = 1.0;
    double halo_intensity = 1.0;
    double halo_size = 1.0;
    double bloom_intensity = 1.0;
    double bloom_size = 1.0;
};

// apply_diffusion_filter_um: in-place on the float64 raw irradiance image `raw`,
// shape (h, w, 3) row-major channel-interleaved. Implements the energy-conserving
// convex combination
//     E_out = (1 - p_s) * E_in  +  p_s * (K_s * E_in)
// with p_s derived from strength+family and K_s the per-channel diffusion-filter
// PSF (the halo is colour-tinted via an energy-conserving warmth redistribution).
// `pixel_size_um` converts the image-plane µm widths to pixels. No-op (image left
// untouched) when params.active is false, strength<=0, spatial_scale<=0, or the
// derived p_s<=0 — exactly mirroring spektrafilm.model.diffusion.apply_diffusion_filter_um.
// Observability for the FFT convolution path (see diffusion.cpp). Counts the
// renders where the cost model chose the FFT but the transform buffers could not
// be allocated, so the stage silently ran the direct O(w*h*ks^2) loop instead --
// correct output, but potentially ~100x slower with nothing in the timing to say
// so. Read it after any measurement of this stage, and especially after changing
// SPK_DIFFUSION_FFT_MAX: a nonzero count means the number you just measured is
// the DIRECT path, not the transform size you thought you were testing.
// Process-global and monotonic; reset it yourself around a measurement.
unsigned long long diffusion_fft_fallbacks();
void diffusion_reset_fft_fallbacks();

void apply_diffusion_filter_um(double* raw, int w, int h,
                               const DiffusionFilterParams& params,
                               double pixel_size_um);

}  // namespace spk

#endif  // SPK_MODEL_DIFFUSION_H
