/*
 * Spektrafilm for Android — native engine: printing (enlarger) stage.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports spektrafilm/runtime/stages/printing.py's expose + develop
 * for the print_portra parity toggles (auto-exposure off, spatial + stochastic
 * off, preflash off, use_enlarger_lut=False so the exact non-LUT spectral path
 * is the gate):
 *
 *   expose:  for each pixel, take the developed *film* CMY density ->
 *            spectral density (film channel_density / base_density) ->
 *            transmittance 10^-D * enlarger filtered illuminant (NaN -> 0) ->
 *            integrate against the *print* paper sensitivity (10**log_sensitivity,
 *            NaN -> 0) -> multiply by the midgray exposure factor -> log10 floor;
 *            then raw = 10^that * print_exposure * bw_correction -> log10 floor.
 *            (diffusion filter is off; preflash is 0.)
 *   develop: develop_simple(log_raw, print.log_exposure, print.density_curves,
 *            gamma) — interpolate against the print density curves WITHOUT the
 *            nanmin normalisation (print uses the raw curves, no DIR couplers).
 */
#ifndef SPK_RUNTIME_STAGES_PRINTING_H
#define SPK_RUNTIME_STAGES_PRINTING_H

#include "profiles/profile.h"
#include "runtime/params.h"

namespace spk {

// expose(): developed film density_cmy (npix,3) -> log_raw_print (npix,3).
//   `film`  supplies channel_density / base_density (the negative's dyes).
//   `print_profile` supplies log_sensitivity (the paper's spectral response).
//   `params` supplies the enlarger filtered illuminant, midgray factor, etc.
// `width`/`height` (width*height == npix, row-major) are needed only by the
// enlarger optical diffusion filter (params.diffusion_filter); when it is
// inactive (the schema default) the result is bit-identical to the pointwise
// path regardless of the width/height split.
void print_expose(const Profile& film, const Profile& print_profile,
                  const PrintingParams& params, const float* density_cmy,
                  int width, int height, float* log_raw_print_out);

// develop(): log_raw_print (npix,3) -> print density_cmy (npix,3) via the print
// paper's density curves (no normalisation, no couplers).
void print_develop(const Profile& print_profile, const PrintingParams& params,
                   const float* log_raw_print, int npix,
                   float* density_cmy_out);

// _film_cmy_to_print_log_raw for a SINGLE film CMY density triple (printing.py:
// _film_cmy_to_print_log_raw): the spectral integral of the film transmittance
// against the enlarger filtered illuminant + print sensitivity, times the midgray
// factor, plus the constant preflash term, then log10 floor. Used to build the
// scanner black/white reference log_raw vectors (color_reference_service's
// log_raw_print_black/white) from the film black/white reference CMY densities.
// Evaluates with the same scalar libm exp10/pow math as the LUT-build path (the
// references are scalars, not the per-pixel SIMD path), giving the byte-exact
// reference the oracle's _film_cmy_to_print_log_raw produces to <=4 ULP.
void print_reference_log_raw(const Profile& film, const Profile& print_profile,
                             const PrintingParams& params, const double cmy_film[3],
                             double log_raw_out[3]);

// EXPERIMENTAL GPU print-expose offload, observability only (perf lab / #148).
// State: 0 = the one-time on-device self-check has not run, 1 = it passed and
// the offload may engage, 2 = it failed and the CPU integral is permanent for
// this process. print_expose consults it internally.
int gpu_print_expose_state();

// Frames whose print-expose integral actually ran on the GPU. 0 with state == 1
// means the path was eligible but never taken (no frame reached it).
unsigned long long gpu_print_frames_rendered();

}  // namespace spk

#endif  // SPK_RUNTIME_STAGES_PRINTING_H
