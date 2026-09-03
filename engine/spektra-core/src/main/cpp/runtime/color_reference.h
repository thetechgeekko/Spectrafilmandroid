/*
 * Spektrafilm for Android — native engine: scanner black/white correction
 * (ColorReferenceService port).
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports spektrafilm/runtime/services/color_reference.py
 * (ColorReferenceService): the scan-time black/white tone-anchor correction
 * wired by the four scanner params (white/black_correction + white/black_level).
 *
 * The correction maps the measured CIE Y at the reference BLACK and WHITE
 * densities to the (sRGB-decoded) target black/white levels via the affine map
 *   m = (white_level - black_level) / (y_white - y_black + 1e-10)
 *   q = black_level - m * y_black
 *   correction_func(y) = clip(m*y + q, 0, 1)
 * and applies it in two places:
 *   1) Scanning XYZ correction (black_white_xyz_correction): per pixel
 *        scale = correction_func(Y) / (Y + 1e-10);  xyz *= scale.
 *   2) An exposure correction back-applied to the print (or, on the scan-film
 *      positive route, the filming) raw to re-anchor midgray.
 *
 * Route / film-type branches (color_reference.py):
 *   - scan_film && film negative : EVERYTHING is a no-op (refs never built, xyz
 *     correction returns xyz unchanged). The default scan goldens are negative,
 *     so they stay bit-exact.
 *   - scan_film && film positive : filming exposure correction + xyz correction
 *     active; references come from the film density curves (cmy_black =
 *     nanmax(film.density_curves), cmy_white = 0) -> cmy_to_log_xyz.
 *   - print route (scan_film==false) && print negative : printing exposure
 *     correction + xyz correction active; references come from develop_simple of
 *     the print reference log_raw (from film black/white cmy through the enlarger)
 *     -> cmy_to_log_xyz on the PRINT profile.
 *
 * Default (white_correction == black_correction == false): a STRICT no-op — m/q
 * are never built, the exposure factors are 1.0 and the xyz scale is identity, so
 * every pre-existing golden stays byte-identical.
 */
#ifndef SPK_RUNTIME_COLOR_REFERENCE_H
#define SPK_RUNTIME_COLOR_REFERENCE_H

#include "profiles/profile.h"

namespace spk {

// The sRGB-decode level transform (color_reference.py::_remove_sRGB_cctf):
//   RGB_to_RGB(level*ones(3), 'sRGB','sRGB', decode=True, encode=False).mean()
// = colour.cctf_decoding_sRGB(level) * MEAN_ROWSUM, where MEAN_ROWSUM is the mean
// row-sum of the (near-identity) matrix_RGB_to_RGB(sRGB, sRGB) numerical residue.
// Reproduced bit-for-bit against the c1d0e44 oracle (see color_reference.cpp).
double remove_srgb_cctf(double level);

// The affine correction the scanning XYZ correction and the exposure corrections
// share (color_reference.py::_correction_fucntion). `active` is false when neither
// correction is enabled => the whole correction is a strict no-op.
struct ColorCorrection {
    bool active = false;     // black_correction || white_correction
    double m = 1.0;          // slope of the y -> y_corrected map
    double q = 0.0;          // intercept
    double midgray_corrected = 0.184;  // (0.184 - q) / m, the re-anchored midgray
    // Apply correction_func to a single CIE Y (clip(m*y+q, 0, 1)).
    double apply_y(double y) const { return active ? clip01(m * y + q) : y; }
    static double clip01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
};

// Build the shared correction from the two measured reference Y values and the
// raw (un-decoded) white/black levels + toggles. Mirrors _correction_fucntion:
// when only ONE of black/white is corrected, the OTHER level is pinned to its
// measured reference Y (white_level=y_white if only black; black_level=y_black if
// only white). When neither is on, returns an inactive (no-op) ColorCorrection.
ColorCorrection build_color_correction(double y_black, double y_white,
                                       double black_level, double white_level,
                                       bool black_correction,
                                       bool white_correction);

// Measure (y_black, y_white) for the SCAN-FILM POSITIVE route
// (_update_cmy_black_white_references, in_print=False):
//   cmy_black = nanmax(film.density_curves, axis=0); cmy_white = 0;
//   y = (10**cmy_to_log_xyz(cmy))[Y].
// `film` supplies channel_density / base_density and owns the exact resolved
// viewing illuminant used by the scan pipeline.
void measure_scanfilm_references(const Profile& film, double* y_black,
                                 double* y_white);

// Measure (y_black, y_white) for the PRINT route (negative paper)
// (_update_cmy_black_white_references, in_print=True):
//   cmy_black = develop_simple(log_raw_print_black, print curves, gamma);
//   cmy_white = develop_simple(log_raw_print_white, print curves, gamma);
//   y = (10**cmy_to_log_xyz_PRINT(cmy))[Y],
// where log_raw_print_black/white are _film_cmy_to_print_log_raw(film black/white
// cmy) (the SAME enlarger transform print_expose evaluates). The two reference
// log_raw 3-vectors are passed in (computed by the caller against the live
// enlarger params). `print_profile` supplies the print curves / dyes / illuminant.
void measure_print_references(const Profile& print_profile,
                              const double log_raw_print_black[3],
                              const double log_raw_print_white[3],
                              float density_curve_gamma,
                              double* y_black, double* y_white);

// The print (or filming) exposure correction factor (black_white_printing/filming
// _exposure_correction): re-anchors midgray on the profile characteristic curve.
//   density_midgray            = -log10(0.184)
//   density_midgray_corrected  = -log10(correction.midgray_corrected)
//   density_curve_av = nanmean(profile.density_curves, axis=1) (averaged over CMY)
//   density_min_av   = nanmean(profile.base_density)
//   exposure_correction = 10 ** (le(density_midgray_corrected) - le(density_midgray))
// where le(d) interpolates the (density_curve_av, log_exposure) characteristic.
//
// `negative_increasing` selects the np.interp orientation:
//   - print route (printing.py): interp(d - density_min_av, density_curve_av,
//                                        log_exposure)  -> returns the factor.
//   - filming positive (filming.py): -interp(-(d - density_min_av),
//                                            -density_curve_av, log_exposure),
//     and the function returns 1/factor.
// Returns 1.0 when the correction is inactive (strict no-op).
double exposure_correction_factor(const Profile& profile,
                                  const ColorCorrection& corr,
                                  bool filming_positive);

}  // namespace spk

#endif  // SPK_RUNTIME_COLOR_REFERENCE_H
