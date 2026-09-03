/*
 * Spektrafilm for Android — native engine: scanning stage.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports spektrafilm/runtime/stages/scanning.py for the scan_film
 * route (negative film scanned directly, default params, spatial + stochastic
 * effects disabled): density_cmy -> spectral density -> light -> XYZ ->
 * output RGB -> sRGB CCTF encode + clip.
 */
#ifndef SPK_RUNTIME_STAGES_SCANNING_H
#define SPK_RUNTIME_STAGES_SCANNING_H

#include <cstdint>

#include "kernels/tonecurve.h"
#include "model/gamut_compression.h"
#include "profiles/profile.h"
#include "spektra.h"  // spk_color_space

namespace spk {

// kernels/lut3d_cache.h — only a pointer is needed here, so it stays forward
// declared rather than pulling <map>/<mutex>/<list> into every scanning.h user.
class Lut3DCache;

// Scan parameters for the scan_film route. The scan_portra milestone uses the
// defaults below (sRGB output, CCTF on, no glare/blur/unsharp because spatial &
// stochastic effects are deactivated). Kept as fields so callers can later wire
// the full RuntimePhotoParams tree without changing the signature.
struct ScanningParams {
    bool scan_film = true;
    bool output_cctf_encoding = true;
    // io.output_color_space: selects the XYZ->RGB matrix + per-space CCTF in
    // color_output. Defaults to sRGB. LINEAR_SRGB uses sRGB primaries with the
    // CCTF (and the near-identity RGB->RGB round-trip) skipped, matching
    // scanning.py, which only runs colour.RGB_to_RGB when output_cctf_encoding.
    spk_color_space output_color_space = SPK_CS_SRGB;
    // Scanner unsharp mask (scanner.unsharp_mask = (sigma, amount)). Applied in
    // the output (linear sRGB) space after XYZ->RGB and before the CAT02 round
    // trip + CCTF encode, matching scanning.py::_apply_blur_and_unsharp /
    // diffusion.apply_unsharp_mask: rgb += amount * (rgb - G(sigma) * rgb).
    // Both default 0 (spatial OFF); set to (0.7, 0.7) under scan_portra_spatial.
    double unsharp_sigma = 0.0;
    double unsharp_amount = 0.0;
    // Scanner lens blur (scanner.lens_blur, in pixels). Applied in the output
    // (linear) space BEFORE the unsharp mask, matching scanning.py::
    // _apply_blur_and_unsharp, which calls apply_gaussian_blur(rgb, scanner.lens_blur)
    // (a per-channel 2D Gaussian with scalar sigma == lens_blur, NOT a µm value)
    // and only then apply_unsharp_mask. Gated on lens_blur > 0; default 0 (the
    // oracle's digest_params zeroes scanner.lens_blur under
    // deactivate_spatial_effects=True) => strict no-op, so existing goldens stay
    // bit-exact.
    double lens_blur = 0.0;

    // Viewing glare (print_render.glare), applied on the PRINT route only, in XYZ
    // space BEFORE the XYZ->RGB transform — matching scanning.py::_density_to_rgb,
    // which calls add_glare(xyz, illuminant_xyz, glare) right after the black/white
    // XYZ correction and before colour.XYZ_to_RGB. On the scan_film route glare is
    // None (scanning.py sets glare = None when io.scan_film), so it is never applied
    // there.
    //
    // IMPORTANT (parity): glare is a STOCHASTIC effect — model/glare.py draws a
    // per-pixel lognormal field via numpy's np.random.randn (numba). A native RNG
    // cannot reproduce that pixel stream bit-for-bit, so a glare-active result is
    // NOT bit-exact against the oracle (exactly like grain). The committed print
    // goldens were generated with deactivate_stochastic_effects=True, which the
    // oracle's params_builder maps to print_render.glare.active = False, so the
    // default print output has glare OFF and stays bit-exact. glare_active defaults
    // to false here => strict no-op. It is wired so a caller can enable a non-default
    // glare (visual effect), but such a result is held to a visual, not bit-exact,
    // tolerance.
    bool glare_active = false;
    float glare_percent = 0.03f;
    float glare_roughness = 0.7f;
    float glare_blur = 0.5f;
    uint64_t glare_seed = 0;

    // OPT-IN scanner 3D-LUT acceleration (settings.use_scanner_lut +
    // settings.lut_resolution). Mirrors scanning.py::_density_to_rgb, which routes
    // the per-pixel cmy_to_log_xyz spectral integral through
    // SpectralLUTService.spectral_compute_scanner(..., use_lut=use_scanner_lut):
    // when use_lut, a per-channel uniform 3D LUT is built over the density domain
    // [data_min, data_max] at `lut_resolution` steps and the image is interpolated
    // with the PCHIP path (kernels/lut3d) instead of evaluating the spectral
    // integral per pixel.
    //
    // The LUT covers EXACTLY the density_cmy -> log_xyz step (steps 1-4 of scan());
    // the post-LUT 10^log_xyz, optional glare, XYZ->RGB, blur/unsharp and CCTF are
    // identical to the direct path. Interpolation is NOT bit-exact vs the direct
    // spectral evaluation and its error is profile/domain dependent. At LUT17 the
    // locked D50 fixture is <=5e-5, while K75P 2383/2393 are about 0.0040/0.0073.
    // Therefore it is OPT-IN: use_lut defaults to false and the default engine path
    // never constructs the LUT and stays byte-identical to today.
    //
    // Domain bounds (scanning.py::_density_to_rgb):
    //   scan_film : data_min = -film_render.grain.density_min,
    //               data_max =  np.nanmax(film.data.density_curves, axis=0)
    //   print scan: data_min =  np.nanmin(print.data.density_curves, axis=0),
    //               data_max =  np.nanmax(print.data.density_curves, axis=0)
    // The scan_film grain.density_min defaults (0.07, 0.08, 0.12) are carried here
    // so scan() can form xmin = -density_min without pulling the full grain params.
    bool use_lut = false;
    int lut_resolution = 32;
    double grain_density_min[3] = {0.07, 0.08, 0.12};

    // OPTIONAL engine-level memo for the built LUT (kernels/lut3d_cache.h). PERF
    // only: nullptr (the default — every direct caller and every parity test)
    // builds the LUT inline exactly as before, and a non-null cache returns a LUT
    // built by the same code from the same inputs, so the render is byte-identical
    // either way. Only spk_engine passes one, so a preview does not rebuild the
    // same 3D LUT on every frame.
    Lut3DCache* lut_cache = nullptr;

    // Scanner BLACK/WHITE XYZ correction (color_reference.py::
    // black_white_xyz_correction): a scan-time tone anchor that maps the measured
    // CIE Y at the reference black/white densities to the (sRGB-decoded) target
    // levels via the affine y_corrected = clip(m*Y + q, 0, 1), then rescales the
    // whole XYZ triple by y_corrected / (Y + 1e-10). Applied per pixel right after
    // the density->XYZ step and BEFORE add_glare + XYZ->RGB, exactly matching
    // scanning.py::_density_to_rgb. m/q are built by the caller from the route's
    // reference Y values (color_reference.h). bw_xyz_correction defaults OFF (the
    // four scanner_*_correction params default to false), a STRICT no-op, so every
    // pre-existing golden stays bit-exact. The oracle skips this correction on the
    // scan_film negative route (and for negative scans the caller leaves it off).
    bool bw_xyz_correction = false;
    double bw_xyz_m = 1.0;
    double bw_xyz_q = 0.0;

    // OPTIONAL Lightroom-style tone curve on the final display-referred RGB, applied
    // per channel right after CCTF encode + clip (kernels/tonecurve). Default
    // inactive => strict no-op (apply() returns the input unchanged), so existing
    // parity goldens — which carry no curve — stay bit-exact. This is a NEW look
    // control beyond the spektrafilm oracle, gated like grain/glare.
    ToneCurveSet tone_curve;

    // OPT-IN output gamut compression (gamut_compression.py::compress_rgb, applied in
    // scanning.py::_density_to_rgb right after XYZ->RGB and before blur/unsharp).
    // Default kLegacyClip => the block is SKIPPED and the engine's existing clip path
    // is unchanged, so every pre-existing golden stays byte-identical. kAcesRgc applies
    // the ACES RGC v1.3 per-channel knee in the linear output space; its math is gated
    // by tests/test_gamut_out_aces.cpp against an upstream gamut_compression.py golden.
    // The knee triple defaults to the oracle's production default (0, 1, 6).
    OutputGamutCompress output_gamut_compress = OutputGamutCompress::kLegacyClip;
    double gamut_knee_threshold = 0.0;
    double gamut_knee_limit = 1.0;
    double gamut_knee_power = 6.0;

    // GPU preview fast-path (GPU M1, #146; PREVIEW-ONLY BY LAW REVISION #149:
    // export stays CPU until the option-B machinery ships). Default false — every
    // parity test and every export render keeps the CPU path byte-identical. Set
    // ONLY by spk_simulate_preview (from spk_params.gpu_preview, the user's
    // Settings toggle). When true AND the frame is eligible (sRGB + CCTF on, no
    // gamut compress / glare / BW correction / blur / unsharp, 81-band profile)
    // AND the one-time on-device self-check passes, scan() runs the density->RGB
    // chain through the Vulkan scan_spectral kernel (fp32, ~2e-6 vs the CPU
    // chain per the PR #145 device probe — tighter than every locked LUT17
    // scanner case) and skips the LUT entirely. ANY failure falls through to the
    // unchanged CPU path.
    bool allow_gpu = false;
};

// One-time GPU scan self-check state: 0 = not yet run, 1 = passed, 2 = failed
// (GPU disabled for the session). For diagnostics/logging (JNI reads it via
// spk_gpu_scan_state); scan() consults it internally.
int gpu_scan_preview_state();

// Frames actually rendered through a GPU kernel this process (0 = the path
// never engaged). Observability only — see spk_gpu_scan_frames.
uint64_t gpu_scan_frames_rendered();

// scan(): run the scanning stage on an (h x w x 3) row-major density_cmy image.
//   in  : density_cmy, length w*h*3.
//   out : final RGB (sRGB, CCTF-encoded, clipped to [0,1]), length w*h*3.
// `film` supplies channel_density / base_density / viewing illuminant. The scan
// illuminant, normalization and XYZ->RGB matrix come from the exact registry key
// in color_output.h (D50 and K75P are currently oracle-locked).
//
// Mirrors scanning.py exactly for the negative scan_film route:
//   - black/white XYZ correction is skipped (negative film scan).
//   - glare is None (scan_film) and blur/unsharp are off (spatial deactivated).
void scan(const Profile& film, const ScanningParams& params,
          const float* density_cmy, int width, int height, float* rgb_out);

}  // namespace spk

#endif  // SPK_RUNTIME_STAGES_SCANNING_H
