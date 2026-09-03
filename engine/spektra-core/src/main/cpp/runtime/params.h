/*
 * Spektrafilm for Android — native engine: minimal digested filming params.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. A tiny subset of spektrafilm/runtime/params_builder.py
 * (init_params + digest_params) sufficient for the filming stage under the
 * parity goldens' toggles (auto-exposure off, spatial+stochastic effects off).
 *
 * Only the constants the filming stage actually consumes are reproduced; the
 * exact numeric values are pulled from the Python oracle's digested params for
 * the scan_portra case.
 */
#ifndef SPK_RUNTIME_PARAMS_H
#define SPK_RUNTIME_PARAMS_H

#include "model/couplers.h"
#include "model/diffusion.h"
#include "model/grain.h"
#include "model/morph_curves.h"

namespace spk {

// kernels/lut3d_cache.h — only a pointer is needed here, so it stays forward
// declared rather than pulling <map>/<mutex>/<list> into every params.h user.
class Lut3DCache;

// Input color space selector for the rgb->raw chromaticity transform. Each maps
// to a baked RGB->XYZ matrix (apply_cctf_decoding=False, CAT02 to the film's
// reference illuminant) inside the filming stage.
enum class InputColorSpace {
    kProPhotoRGB,   // "ProPhoto RGB" (linear) — the spektrafilm default input.
};

// Digested parameters the filming stage needs. Mirrors the relevant fields of
// the Python digested RuntimePhotoParams for the parity toggles.
struct FilmingParams {
    InputColorSpace input_color_space = InputColorSpace::kProPhotoRGB;
    bool input_cctf_decoding = false;

    // Exposure (auto-exposure off, exposure_compensation_ev == 0 under goldens).
    double exposure_compensation_ev = 0.0;

    // Scanner BLACK/WHITE filming exposure correction (color_reference.py::
    // black_white_filming_exposure_correction), applied in expose() as a scalar
    // multiplier on the raw irradiance (raw *= bw_exposure_correction), right
    // where filming.py does `raw *= black_white_filming_exposure_correction()`
    // (after halation, before log10). It is 1.0 (a STRICT no-op) on every parity
    // route EXCEPT scan_film with a POSITIVE film: the oracle returns 1.0 for
    // negative film and for the print route, so all existing goldens (negative
    // film) stay bit-exact. The factor is computed by the caller via
    // color_reference.h::exposure_correction_factor(filming_positive=true).
    double bw_exposure_correction = 1.0;

    // Density-curve gamma (scalar broadcast to all three channels).
    float density_curve_gamma[3] = {1.0f, 1.0f, 1.0f};

    // Spatial-effects toggle. When false (the spatial-OFF parity goldens) the
    // halation pass is skipped in expose() and the DIR-coupler correction is
    // applied pointwise (diffusion_size_um forced to 0). When true the halation
    // pass runs and the coupler correction is spatially diffused.
    bool spatial_effects = false;

    // µm -> pixel conversion driving every spatial sigma. Equals the resize
    // service's pixel_size_um = film_format_mm * 1000 / max(width, height) for the
    // parity image (no crop). Only consumed when spatial_effects is true.
    double pixel_size_um = 0.0;

    DirCouplersParams dir_couplers;  // filled by digest for the film type.
    HalationParams halation;         // filled by digest; active only if spatial.

    // Camera lens blur, in micrometres (camera.lens_blur_um). Applied in expose()
    // on the float64 raw irradiance, AFTER the optical diffusion filter and BEFORE
    // halation — exactly matching filming.py::expose, which calls
    // apply_gaussian_blur_um(raw, camera.lens_blur_um, pixel_size_um) between
    // apply_diffusion_filter_um and apply_halation_um. The blur is a per-channel
    // 2D Gaussian with a single scalar sigma = lens_blur_um / pixel_size_um (the
    // oracle broadcasts the scalar sigma across all three channels). Schema default
    // 0.0 µm => sigma 0 => strict no-op, so default params stay bit-exact. The
    // oracle's digest_params zeroes camera.lens_blur_um under
    // deactivate_spatial_effects=True (params_builder.py), so the lens blur belongs
    // to the spatial branch (gated on spatial_effects in expose()). pixel_size_um
    // drives the µm->pixel conversion.
    double lens_blur_um = 0.0;

    // Camera optical diffusion filter (Black Pro-Mist family). Applied in
    // expose() on the float64 raw irradiance, AFTER the highlight boost and
    // BEFORE the lens blur / halation (mirrors filming.py::expose ordering).
    // Schema default active=false -> strict no-op, so default params stay
    // bit-exact. Runs whenever active (independent of the spatial_effects
    // toggle: the diffusion filter is active even in preview, matching the
    // upstream GUI note "diffusion filters are active" in preview mode).
    DiffusionFilterParams diffusion_filter;

    // Stochastic grain (the AgX particle model). Active only when the case
    // requests grain (grain_active && stochastic effects on). Off => identity.
    // density_max_curves is filled at apply time from the film's normalized
    // density curves; the rest are the schema defaults (digest_grain_params).
    GrainParams grain;
};

// Build digested filming params for a film type, mirroring
// digest_params -> _apply_film_specifics under the parity toggles.
// `is_negative` selects the negative/positive coupler gamma matrix; `stock`
// (the profile info.stock string, may be null/empty) selects the per-stock DIR
// coupler gamma override (_apply_film_specifics applies the negative/positive
// matrix first, then overwrites it for specific stocks — currently
// fujifilm_provia_100f and fujifilm_velvia_100).
//
// When `spatial_effects` is false (the default, spatial-OFF goldens):
//   dir_couplers.diffusion_size_um == 0 (pointwise), halation.active == false.
// When `spatial_effects` is true (scan_portra_spatial):
//   dir_couplers diffusion (size 20µm, tail 200µm, weight 0.06) is enabled and
//   the digested HalationParams (scatter + back-reflection) are filled.
FilmingParams digest_filming_params(bool is_negative, bool spatial_effects = false,
                                    const char* stock = nullptr);

// Fill p.grain from the schema GrainParams defaults and activate it. Mirrors the
// digest under deactivate_stochastic_effects=False (grain.active stays True) and
// deactivate_spatial_effects=False (grain.blur stays 0.65). The
// density_max_curves are NOT set here — they are filled by develop() from the
// film's normalized density curves (nanmax over the log-exposure axis). BOTH
// grain paths are wired: the non-sublayer path and the sublayer/micro-structure
// path (model/grain.h; sublayers_active/n_sub_layers/micro_structure flow
// through spektra.cpp, gated statistically by tests/test_grain_sublayer.cpp).
void digest_grain_params(FilmingParams& p);

// Fill p.halation from the film's halation preset tags (info.use / info.antihalation
// in the profile), mirroring params_builder._apply_halation_preset +
// _HALATION_PRESETS. Sets sigma_h / strength from the (use, antihalation) preset;
// scatter parameters stay at the params_schema HalationParams defaults. Marks
// halation active only when `spatial_effects` is true (otherwise apply_halation_um
// is skipped). Unknown tag pairs leave halation inactive.
void digest_halation_params(FilmingParams& p, const char* use,
                            const char* antihalation, bool spatial_effects);

// Digested parameters the printing (enlarger) stage needs, under the print_portra
// parity toggles (auto-exposure off, spatial + stochastic effects off, preflash
// off, print_exposure==1, normalize_print_exposure & print_exposure_compensation
// both on so the midgray normalisation reduces to the single midgray factor).
//
// Mirrors the relevant digested RuntimePhotoParams fields:
//   - filtered_illuminant: the enlarger light source after the dichroic Y/M/C
//     filters (color_enlarger with the digested neutral CC values + shifts==0),
//     i.e. enlarger_service.enlarger_filtered_illuminant(standard_illuminant(
//     enlarger.illuminant)). 81 samples on the working shape.
//   - exposure_factor_midgray: the scalar midgray normalisation factor returned
//     by PrintingStage._compute_exposure_factor_midgray (= factor_midgray_comp
//     when both normalize + compensation are on). Computed natively by
//     runtime/print_digest.h::compute_midgray_exposure_factor from the full
//     filming midgray chain (rgb_to_raw of 0.184 gray) — no longer oracle-baked.
//   - print_exposure / bw_exposure_correction: scalar multipliers in expose()
//     (1.0 each under the parity defaults: no black/white correction).
//   - density_curve_gamma: print_render.density_curve_gamma (scalar broadcast).
struct PrintingParams {
    // INTERNAL, never a user param: permission to offload the print-expose
    // spectral integral to the GPU for this render. Set from spk_params'
    // allow_gpu_scan latch (the same one scanning uses), so one toggle governs
    // the whole GPU surface. print_expose re-gates on frame eligibility and the
    // one-time on-device self-check, and any failure falls back to the exact
    // CPU integral for that frame. Default false: an untouched PrintingParams
    // is the CPU path.
    bool allow_gpu = false;

    double filtered_illuminant[81] = {0.0};
    double exposure_factor_midgray = 1.0;
    double print_exposure = 1.0;
    double bw_exposure_correction = 1.0;
    float density_curve_gamma[3] = {1.0f, 1.0f, 1.0f};

    // Enlarger optical diffusion filter. Applied in printing.py::expose on the
    // float64 raw print irradiance (raw = 10^log_raw * print_exposure *
    // bw_correction) BEFORE the final log10. Schema default active=false -> a
    // strict no-op, so default params stay bit-exact. pixel_size_um drives the
    // µm->pixel conversion (only consumed when diffusion_filter.active).
    DiffusionFilterParams diffusion_filter;
    double pixel_size_um = 0.0;

    // Enlarger PREFLASH (a uniform pre-exposure flash of the print paper before
    // the image exposure). Mirrors printing.py::_compute_raw_preflash, which adds
    // a constant per-print-channel raw_preflash 3-vector to `raw` AFTER the midgray
    // exposure factor and BEFORE the log10 inside _film_cmy_to_print_log_raw:
    //   light_preflash[l] = 10^-base_density[l] * preflash_illuminant[l]   (NaN->0)
    //   raw_preflash[k]   = sum_l light_preflash[l] * sensitivity[l,k]
    //   raw[k] += raw_preflash[k] * preflash_exposure
    // The preflash illuminant is color_enlarger(enlarger light source, CC =
    // [c_neutral, m_neutral + preflash_m_filter_shift, y_neutral +
    // preflash_y_filter_shift]) — its OWN filter shifts, independent of the image
    // exposure's m/y shifts (filter_enlarger_source.preflash_filtered_illuminant).
    // The 3-vector is constant across pixels, so it is precomputed once in
    // print_expose against the print sensitivity + film base density. Schema
    // default preflash_exposure 0.0 => the term is exactly zero (the oracle's
    // `if preflash_exposure > 0` guard returns a zero 3-vector), a STRICT no-op,
    // so default params stay bit-exact. 81 samples on the working shape.
    double preflash_illuminant[81] = {0.0};
    double preflash_exposure = 0.0;

    // OPT-IN enlarger 3D-LUT acceleration (settings.use_enlarger_lut, default
    // false). Mirrors printing.py::expose routing _film_cmy_to_print_log_raw
    // through SpectralLUTService.spectral_compute_enlarger(use_lut=...): a
    // per-channel uniform PCHIP 3D LUT over the film-density domain
    // [data_min, data_max] = [-grain.density_min, nanmax(film.density_curves)]
    // at lut_resolution steps replaces the per-pixel print-expose spectral
    // integral. OFF by default -> the LUT is never built and print_expose is
    // byte-identical to the direct spectral evaluation (the parity-gate path).
    // The LUT is an interpolation -> NOT bit-exact vs direct (documented ~5e-5),
    // so it is strictly opt-in (the scanner-LUT precedent, scanning.cpp).
    bool use_enlarger_lut = false;
    int lut_resolution = 32;
    double grain_density_min[3] = {0.07, 0.08, 0.12};

    // OPTIONAL engine-level memo for the built LUT (kernels/lut3d_cache.h). PERF
    // only: nullptr (the default — every direct caller and every parity test)
    // builds the LUT inline exactly as before, and a non-null cache returns a LUT
    // built by the same code from the same inputs, so the render is byte-identical
    // either way. Only spk_engine passes one, so a preview does not rebuild the
    // same 3D LUT on every frame.
    Lut3DCache* lut_cache = nullptr;

    // OPT-IN print density-curve morph (s023). When morph.active (default false)
    // the develop step rebuilds the print density table from the paper's
    // density_curves_model and morphs it (model/morph_curves) instead of
    // interpolating the stored density_curves table. Default-off -> the stored
    // table is used and the print parity goldens stay byte-identical; the active
    // path is gated by its own oracle golden (test_print_curves_morph_e2e).
    PrintCurvesMorphParams morph;
};

// Enlarger illuminant on the 81-band working shape for a given illuminant id.
// Currently only the default "TH-KG3" (tungsten-halogen blackbody 3400K through
// the Schott KG3 heat filter, mean-normalised) is baked; returns nullptr for
// unknown ids. Equal to illuminants.standard_illuminant(id).
const double* enlarger_illuminant(const char* illuminant_id);

// Build digested printing params for any (film, paper) pair. `neutral_cc` are the
// neutral C/M/Y filter values (Kodak CC units) resolved natively from
// neutral_print_filters.json by runtime/print_digest.h::resolve_neutral_cc for
// the film/print/illuminant triple; `enl_illum` is enlarger_illuminant(id). The
// filtered illuminant is computed exactly via color_enlarger (shifts==0).
// `exposure_factor_midgray` is the native midgray factor from
// runtime/print_digest.h::compute_midgray_exposure_factor; `gamma` is the print
// density-curve gamma. (Pass exposure_factor_midgray=1.0 to build the filtered
// illuminant first, then set it from the native computation.)
PrintingParams digest_printing_params(const double neutral_cc[3],
                                      const double* enl_illum,
                                      double exposure_factor_midgray,
                                      float gamma);

}  // namespace spk

#endif  // SPK_RUNTIME_PARAMS_H
