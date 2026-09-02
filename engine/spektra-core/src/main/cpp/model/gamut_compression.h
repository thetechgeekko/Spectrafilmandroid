/*
 * Spektrafilm for Android — native engine: output gamut compression.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports utils/gamut_compression.py::reinhard_knee and
 * compress_rgb_aces_rgc (ACES Reference Gamut Compression v1.3, the native
 * per-channel form) for the output side.
 *
 * OPT-IN / DEFAULT-OFF. The selector defaults to kLegacyClip — the engine's
 * existing behavior (no compression; scanning's final np.clip(0,1)) — so every
 * pre-existing golden stays byte-identical. kAcesRgc opts into the ACES RGC knee
 * in the linear output space (gated by tests/test_gamut_out_aces.cpp against an
 * upstream gamut_compression.py golden). kOklch opts into the OkLch perceptual
 * chroma reduction toward the output RGB cube (compress_rgb_oklch_chroma, gated by
 * tests/test_gamut_out_oklch.cpp). kOklrab is the same chroma reduction indexed by
 * Ottosson's rebased lightness Lr (compress_rgb_oklrab_chroma, gated by
 * tests/test_gamut_out_oklrab.cpp). reinhard_knee is the shared knee the
 * input/perceptual gamut items reuse. The remaining perceptual algorithms
 * (jzazbz/cam16ucs) are reserved here, not yet ported.
 */
#ifndef SPK_MODEL_GAMUT_COMPRESSION_H
#define SPK_MODEL_GAMUT_COMPRESSION_H

namespace spk {

// Output gamut compression algorithm selector (mirrors
// gamut_compression.py::OutputGamutCompressSpec.algorithm, plus the kLegacyClip
// sentinel that is local to this engine — the oracle has no clip, the engine does).
//   kLegacyClip — DEFAULT. No gamut compression; the scanning stage keeps its
//                 existing final np.clip(0,1). Byte-identical to every golden.
//   kOff        — oracle "off". NOTE: today this behaves exactly like
//                 kLegacyClip — scan()'s final np.clip(0,1) is unconditional, so
//                 no branch implements a true pass-through. Making kOff skip the
//                 clip is a behavior change needing its own golden (open item in
//                 docs/AUDIT.md); until then the ordinal is accepted and clips.
//   kAcesRgc    — ACES Reference Gamut Compression v1.3 (per-channel knee on the
//                 achromatic distance), applied in the linear output space.
//   kOklch      — OkLch perceptual chroma reduction toward the output RGB cube
//                 (constant OkLab hue + lightness; one-sided lightness knee then a
//                 chroma knee against a per-space C_max(L,h) table). Opt-in; gated
//                 by tests/test_gamut_out_oklch.cpp.
//   kOklrab     — the same OkLch chroma reduction, but the C_max lookup is indexed by
//                 Ottosson's rebased lightness Lr = f(L) (a monotonic remap of OkLab L
//                 toward CIELAB L*), so the knee response is more perceptually uniform
//                 across light/dark. Opt-in; gated by tests/test_gamut_out_oklrab.cpp.
//   kJzazbz/kCam16ucs — perceptual chroma reduction in JzCzhz / CAM16-UCS (#201);
//                 not implemented yet.
enum class OutputGamutCompress {
    kLegacyClip = 0,
    kOff        = 1,
    kAcesRgc    = 2,
    kOklch      = 3,
    kOklrab     = 4,
    kJzazbz     = 5,
    kCam16ucs   = 6,
};

// Input gamut compression algorithm selector (mirrors
// gamut_compression.py::InputGamutCompressSpec.algorithm, plus the kOff sentinel that
// is the engine default — the oracle spec defaults active=True, the engine defaults
// OFF so every pre-existing golden stays byte-identical, exactly like the output side
// defaults to kLegacyClip).
//   kOff   — DEFAULT. No input compression; the Hanatos filming tc_lut is built and
//            used exactly as before (oracle InputGamutCompressSpec.active == False).
//            Byte-identical to every golden.
//   kXy    — ACES-RGC-style radial compression in CIE 1931 chromaticity from the film
//            reference illuminant toward the visible spectral locus (oracle
//            algorithm="xy", the production default), baked into the tc_lut.
//   kOklch — perceptual chroma reduction. RESERVED (needs the OkLab C_max bisection
//            table); not implemented yet.
enum class InputGamutCompress {
    kOff   = 0,
    kXy    = 1,
    kOklch = 2,
};

// Reinhard knee on a normalized distance d (gamut_compression.py::reinhard_knee):
// identity at/below `threshold`, smoothly asymptotic to `limit` above it.
//   d <= threshold      -> d
//   else  scale = limit - threshold;  x = (d - threshold) / scale;
//         y = x / (1 + x^power)^(1/power);  return threshold + scale * y.
double reinhard_knee(double d, double threshold, double limit, double power);

// ACES RGC v1.3 on one linear-RGB triple
// (gamut_compression.py::compress_rgb_aces_rgc), writing the compressed triple to
// out[3]. ach = max(R,G,B); for ach <= 1e-12 (near-black) the pixel passes through
// unchanged. Otherwise each channel's distance d = (ach - c) / ach is passed through
// reinhard_knee and reconstructed as c' = ach * (1 - d'). The achromatic max is
// preserved; with limit=1 negative channels (d>1) are pulled to exactly 0.
void compress_pixel_aces_rgc(const double rgb[3], double threshold, double limit,
                             double power, double out[3]);

// In-place ACES RGC over an interleaved (npix*3) row-major linear-RGB image.
void compress_rgb_aces_rgc(double* rgb, int npix, double threshold, double limit,
                           double power);

// ---- Output-side: OkLch perceptual chroma reduction to the output RGB cube -------
// (gamut_compression.py::compress_rgb_oklch_chroma, dispatched by compress_rgb with
// algorithm=="oklch".) Perceptual-hue- and lightness-preserving chroma reduction.
// Per pixel: linear output-space RGB -> XYZ (native per-space matrix, illuminant = the
// space's OWN whitepoint, no CAT) -> OkLab -> OkLch(L,C,h); a one-sided reinhard
// lightness knee on L (params (0.7,1.0,2.2), L_white=1 — the OutputGamutCompressSpec
// default the golden pins) runs BEFORE the chroma step, so the C_max lookup and the
// reconstruction both use the corrected L, while C and h come from the ORIGINAL a,b;
// look up the max in-gamut chroma C_max(L,h) for the destination RGB cube (a 64x720
// bisection table) and pass C/C_max through the shared reinhard_knee; reconstruct
// OkLch -> OkLab -> XYZ -> RGB. All math is double precision (NumPy float64). No clip
// (scanning clips afterwards, exactly as the oracle leaves it). `output_space` is
// spk_color_space 0..5 (selects the per-space RGB<->XYZ matrix and the C_max table).
//
// OPT-IN: gated by tests/test_gamut_out_oklch.cpp; the scanning hook runs it only when
// output_gamut_compress == kOklch, so every pre-existing golden stays byte-identical.

// In-place OkLch chroma reduction over an interleaved (npix*3) row-major linear-RGB
// image in `output_space`. Builds the per-space C_max(L,h) table ONCE (locally, no
// static state -> thread-invariant and warm==cold), then loops the pixels.
void compress_rgb_oklch_chroma(double* rgb, int npix, int output_space,
                               double threshold, double limit, double power);

// ---- Output-side: Oklrab perceptual chroma reduction (Lr-indexed) ---------------
// (gamut_compression.py::compress_rgb_oklrab_chroma, dispatched by compress_rgb with
// algorithm=="oklrab".) Byte-for-byte the OkLch reduction above, with ONE change: the
// per-pixel C_max lookup is indexed by Ottosson 2023's rebased lightness
// Lr = _oklab_L_to_oklrab_Lr(L) instead of raw OkLab L, and the per-space C_max(Lr,h)
// bisection table is built over an Lr grid (each grid row's OkLab L is recovered by the
// inverse remap before Oklab->XYZ). The chroma reconstruction still preserves the
// original (lightness-compressed) L — only the boundary lookup moves to the Lr axis, so
// equal knee increments track closer-to-equal perceived lightness. Same double-precision
// pipeline, same 64x720/18-bisection geometry, same one-sided lightness knee (0.7,1.0,2.2)
// on L before the Lr remap. OPT-IN: gated by tests/test_gamut_out_oklrab.cpp; the scanning
// hook runs it only when output_gamut_compress == kOklrab, so every pre-existing golden
// stays byte-identical.
void compress_rgb_oklrab_chroma(double* rgb, int npix, int output_space,
                                double threshold, double limit, double power);

// ---- Output-side: JzCzhz perceptual chroma reduction (Safdar 2017) ---------------
// (gamut_compression.py::compress_rgb_jzazbz_chroma, dispatched by compress_rgb with
// algorithm=="jzazbz".) Same algorithm shape as kOklch, but the perceptual space is
// JzAzBz at an absolute reference white of Y_w = 100 cd/m^2 (linear RGB=1 maps to
// Y=100 cd/m^2 before the forward, undone after the inverse), the Cz_max table's
// lightness grid is linspace(0.002, 0.18, 64) with chroma headroom 0.3, and the
// one-sided lightness knee is normalized by the output whitepoint's Jz. Keeps
// perceived hue stable across the magenta<->cyan arc where OkLch twists. OPT-IN:
// gated by tests/test_gamut_out_jzazbz.cpp; scanning runs it only when
// output_gamut_compress == kJzazbz, so every pre-existing golden stays byte-identical.
void compress_rgb_jzazbz_chroma(double* rgb, int npix, int output_space,
                                double threshold, double limit, double power);

// ---- Output-side: CAM16-UCS perceptual chroma reduction (Li et al. 2017) ---------
// (gamut_compression.py::compress_rgb_cam16ucs_chroma, dispatched by compress_rgb
// with algorithm=="cam16ucs" — the oracle's DEFAULT algorithm at 3bb2c2d.) Same
// algorithm shape in CIECAM16-UCS J'a'b': adapting whitepoint = the output space's
// whitepoint (Y=1, x100 in-model), fixed viewing conditions L_A=64 cd/m^2, Y_b=20,
// Average surround; Cp_max grid linspace(1, 110, 64) with chroma headroom 150; the
// lightness knee is normalized by the whitepoint's Jp (~100). The heaviest of the
// perceptual options (full CAM16 forward+inverse per pixel and per bisection step).
// OPT-IN: gated by tests/test_gamut_out_cam16ucs.cpp; scanning runs it only when
// output_gamut_compress == kCam16ucs, so every pre-existing golden stays
// byte-identical.
void compress_rgb_cam16ucs_chroma(double* rgb, int npix, int output_space,
                                  double threshold, double limit, double power);

// ---- Input-side: radial xy compression toward the visible spectral locus --------
// (gamut_compression.py input path: spectral_locus_xy + compress_xy_radial.) These
// operate on CIE 1931 chromaticity xy (2 components) around a reference white, NOT on
// output RGB. They are pure double math; the spectral-locus polygon is baked as a
// captured constant (see the .cpp) so the result matches the oracle bit-for-bit.

// The closed CIE 1931 2 deg visible spectral locus polygon in xy
// (gamut_compression.py::spectral_locus_xy: CMFs at 380..700 nm @ 5 nm, normalized by
// X+Y+Z, first vertex repeated). Returns the vertex count N (66) and sets *out_xy to
// a static flat array of N*2 doubles (x0,y0,x1,y1,...). The first and last vertices
// are identical (the polygon is closed) so it is directly usable for ray-polygon
// intersection.
int spectral_locus_xy(const double** out_xy);

// ACES-RGC-style radial xy compression of one chromaticity toward the spectral locus
// (gamut_compression.py::compress_xy_radial, per point). `white_xy` is the achromatic
// axis (the film reference illuminant chromaticity). Computes the normalized radial
// distance d = |xy - white| / boundary (boundary = ray-to-locus distance along the
// xy->white direction), passes d through reinhard_knee, and rescales along the same
// ray — preserving hue (dominant wavelength). At-white points (|xy - white| < 1e-9)
// pass through unchanged. Writes the compressed xy to out[2].
void compress_pixel_xy(const double xy[2], const double white_xy[2], double threshold,
                       double limit, double power, double out[2]);

// In-place radial xy compression over an interleaved (npix*2) array of chromaticities.
void compress_xy_radial(double* xy, int npix, const double white_xy[2],
                        double threshold, double limit, double power);

}  // namespace spk

#endif  // SPK_MODEL_GAMUT_COMPRESSION_H
