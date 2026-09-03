/*
 * Spektrafilm for Android — native engine: scan color output (XYZ->RGB + CCTF).
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Provides the constants and helpers the scanning stage needs to
 * mirror colour.XYZ_to_RGB(..., illuminant=scan_illuminant_xy) + the sRGB CCTF
 * encode in spektrafilm/runtime/stages/scanning.py.
 *
 * The XYZ->output-RGB matrix in colour bakes in chromatic adaptation from the
 * scan illuminant's xy to the output colourspace whitepoint. For every route,
 * the scan illuminant is the scanned profile's viewing illuminant. The
 * effective matrices below were extracted from
 * colour-science via the parity oracle so they match bit-for-bit.
 */
#ifndef SPK_MODEL_COLOR_OUTPUT_H
#define SPK_MODEL_COLOR_OUTPUT_H

#include <string_view>

#include "spektra.h"  // spk_color_space

namespace spk {

// --- D50 viewing illuminant (the film's viewing_illuminant) ------------------
// standard_illuminant("D50") on SpectralShape(380,780,5): colour's SDS_ILLUMINANTS
// "D50" aligned to the working shape, then normalised so mean(values) == 1
// (illuminants.py divides by sum/len). 81 samples. NaN-free.
extern const float kIlluminantD50[81];

// normalization = sum(illuminant * ybar) over the 81 samples, for D50.
// Used as the XYZ denominator in scanning's cmy_to_log_xyz.
extern const double kNormD50;

// --- K75P cinema-projector viewing illuminant -------------------------------
// standard_illuminant("K75P") in upstream resolves to colour-science's
// SDS_LIGHT_SOURCES["Kinoton 75P"], aligned to 380..780 @ 5 nm and normalized
// to mean 1. Kodak 2383 and 2393 declare this exact identifier.
extern const float kIlluminantK75P[81];
extern const double kNormK75P;

// Effective XYZ -> sRGB(linear) matrix from colour.XYZ_to_RGB with
// illuminant = D50 xy (CAT02 to sRGB's D65 whitepoint baked in).
// Row-major 3x3: rgb = M . xyz.
extern const float kXYZ_to_sRGB_D50[9];

// Near-identity round-trip matrix colour applies inside RGB_to_RGB(sRGB, sRGB,
// CAT02) before the CCTF encode in scanning's _apply_cctf_encoding_and_clip.
// matrix_RGB_to_RGB(sRGB, sRGB, "CAT02") is NOT exactly identity (RGB->XYZ->RGB
// round-trip + CAT02 adaptation between identical whitepoints leaves ~1e-5
// residuals); the goldens were generated with it applied, so it must be matched.
// Row-major 3x3: rgb_out = M . rgb_in.
extern const double kSRGB_to_sRGB_CAT02[9];

// --- sRGB CCTF encode (IEC 61966-2-1) ---------------------------------------
// V = (L <= 0.0031308) ? 12.92*L : 1.055*L^(1/2.4) - 0.055.
// Matches colour.models.rgb.transfer_functions.srgb.eotf_inverse_sRGB.
float srgb_cctf_encode(float linear);

// --- Per-output-space transforms --------------------------------------------
// The scanning stage emits RGB in io.output_color_space. Each space has:
//   * an effective XYZ->RGB matrix from colour.XYZ_to_RGB(..., illuminant=scan_xy)
//     (CAT02 from the selected scan whitepoint to the space's whitepoint + the
//     space's primaries baked in), and
//   * a near-identity RGB->RGB matrix from colour.RGB_to_RGB(cs, cs, "CAT02")
//     that colour applies inside _apply_cctf_encoding_and_clip *before* the CCTF
//     encode (only when output_cctf_encoding is on), and
//   * a CCTF encode (see output_cctf_encode below).
// LINEAR_SRGB shares sRGB's primaries/matrix but carries NO CCTF and (because
// scanning.py only runs RGB_to_RGB when output_cctf_encoding is True) no
// near-identity round-trip either.
//
// All matrices are row-major 3x3 (out = M . in), extracted from colour-science
// via the parity oracle so they match bit-for-bit. Indexed by spk_color_space.
extern const double kXYZ_to_RGB[6][9];       // D50 (legacy public name)
extern const double kXYZ_to_RGB_K75P[6][9];
extern const double kRGB_to_RGB_CCTF[6][9];

// A profile viewing white is resolved exactly once and then passed unchanged
// through every scan consumer: the spectral integral, glare/reference white,
// output-space adaptation and GPU/LUT fast paths. `xyz_to_rgb` points at six
// row-major 3x3 matrices indexed by spk_color_space.
struct ViewingIlluminant {
    const char* identifier;
    const float* spectrum;
    double normalization;
    const double (*xyz_to_rgb)[9];
};

// Exact, case-sensitive registry lookup. Only identifiers whose spectrum,
// normalization and adaptation matrices are oracle-locked are accepted.
const ViewingIlluminant* find_viewing_illuminant(
    std::string_view identifier) noexcept;

// Fail-closed form used by render paths and profile validation. The diagnostic
// names both the metadata field and the rejected identifier.
const ViewingIlluminant& require_viewing_illuminant(
    std::string_view identifier);

// output_cctf_encode(): the per-space encode CCTF, applied component-wise after
// the near-identity matrix. Mirrors colour's cctf_encoding for each colourspace:
//   SRGB / LINEAR_SRGB : sRGB piecewise (LINEAR_SRGB never calls this; CCTF off)
//   ADOBE_RGB          : pow(L, 1/2.19921875) == pow(L, 0.4547069271758437),
//                        "Indeterminate" negative handling -> NaN for L<0.
//   PROPHOTO           : ROMM: L<E_t ? 16*L : spow(L, 1/1.8), E_t=16^(1.8/(1-1.8)).
//   REC2020            : L<0.018 ? 4.5*L : 1.099*spow(L,0.45) - 0.099.
//   ACES2065_1         : linear (identity).
// `spow` is sign-preserving (sign(L)*pow(|L|,e)); Adobe's plain pow is NOT.
double output_cctf_encode(spk_color_space cs, double linear);

}  // namespace spk

#endif  // SPK_MODEL_COLOR_OUTPUT_H
