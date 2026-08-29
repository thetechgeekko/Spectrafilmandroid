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
 * spektrafilm.
 */
#include "runtime/stages/scanning.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/vulkan_compute.h"
#include "kernels/exponential_filter.h"
#include "kernels/exp10.h"
#include "kernels/lut3d.h"
#include "kernels/lut3d_cache.h"
#include "kernels/parallel.h"
#include "runtime/stage_timer.h"
#include "model/color_output.h"
#include "model/conversions.h"
#include "model/emulsion.h"
#include "model/gamut_compression.h"
#include "model/glare.h"
#include "model/spectral.h"

namespace spk {

namespace {

// Context for the cmy_to_log_xyz spectral integral (scanning.py::cmy_to_log_xyz):
// the closure over the profile's channel_density / base_density, the scan
// illuminant and its normalization. Used by the LUT-build callback (the opt-in
// use_lut path). The per-pixel DIRECT path does NOT call cmy_to_log_xyz; it
// inlines the same math with the vector exp10 (kernels/exp10) instead of
// std::pow below, so the LUT samples an equivalent transform to within the
// exp10-vs-pow tolerance (<=4 ULP/band), not a byte-identical one.
struct CmyToLogXyzCtx {
    const float* channel_density;  // (S*3,) row-major [l*3 + k]
    const float* base_density;     // (S,)
    const float* illum;            // (S,) scan illuminant
    const float (*cmf)[3];         // (S,3) CIE 1931 CMFs
    int S;
    double inv_norm;               // 1 / normalization
};

// One cmy triple -> log_xyz (a 3-vector). Mirrors scanning.py::cmy_to_log_xyz for
// a single pixel: spectral density -> 10^-D * illuminant (NaN->0) -> XYZ integral
// / normalization -> log10(fmax(xyz, 0) + 1e-10). This LUT-build routine uses
// std::pow(10,-D); the direct per-pixel path (below) computes steps 1-4 with the
// vector exp10 equivalent (<=4 ULP/band), so the two agree to that tolerance --
// not bit-for-bit. The remaining LUT error is dominated by the PCHIP sampling.
inline void cmy_to_log_xyz(const CmyToLogXyzCtx& ctx, const double cmy[3],
                           double log_xyz[3]) {
    const double c0 = cmy[0], c1 = cmy[1], c2 = cmy[2];
    double X = 0.0, Y = 0.0, Z = 0.0;
    for (int l = 0; l < ctx.S; ++l) {
        const float* cd = ctx.channel_density + static_cast<size_t>(l) * 3;
        double spectral = c0 * static_cast<double>(cd[0]) +
                          c1 * static_cast<double>(cd[1]) +
                          c2 * static_cast<double>(cd[2]) +
                          static_cast<double>(ctx.base_density[l]);
        double w = std::pow(10.0, -spectral) * static_cast<double>(ctx.illum[l]);
        if (std::isnan(w)) w = 0.0;
        X += w * ctx.cmf[l][0];
        Y += w * ctx.cmf[l][1];
        Z += w * ctx.cmf[l][2];
    }
    log_xyz[0] = std::log10(std::fmax(X * ctx.inv_norm, 0.0) + 1e-10);
    log_xyz[1] = std::log10(std::fmax(Y * ctx.inv_norm, 0.0) + 1e-10);
    log_xyz[2] = std::log10(std::fmax(Z * ctx.inv_norm, 0.0) + 1e-10);
}

// build_lut_3d callback adapter.
void cmy_to_log_xyz_fn(const double in[3], double out[3], void* ctx) {
    cmy_to_log_xyz(*static_cast<const CmyToLogXyzCtx*>(ctx), in, out);
}

// ── GPU preview fast-path (GPU M1, #146) ────────────────────────────────────
// The Vulkan scan_spectral kernel computes density_cmy -> 10^-D over 81 bands ->
// XYZ -> 3x3 matrix -> sRGB CCTF -> clamp, in fp32. The PR #145 device probe
// measured it at max_abs <= 2.15e-06 vs the f64 chain (46x inside the oracle
// tolerance) and byte-identical across repeated dispatches. It replaces steps
// 1-6 of the CPU path below for ELIGIBLE preview frames only; everything the
// shader does not model (glare, BW correction, gamut compression, blur/unsharp,
// non-sRGB output) gates the frame back to the CPU.

constexpr int kGpuNB = 81;  // the shader's fixed band count (NB in scan_spectral.comp)

struct GpuScanTables {
    std::vector<float> dye;   // NB*3 band-major (c,m,y)
    std::vector<float> icmf;  // NB*3 band-major (X,Y,Z), illum+base+norm folded
    float m_engine[9];        // Mc.M: CAT02 round-trip composed with XYZ->sRGB (fused kernel)
    float m_space[9];         // plain kXYZ_to_RGB[space] (linear kernel; Mc stays in encode)
};

// Fold the profile tables into the gpu/vulkan_compute.h contract. KEEP IN SYNC
// with tools/gpu_probe/probe_main.cpp::build_tables — the probe validated
// exactly this fold on device (PR #145, fold-vs-engine <= 1.02e-7 in f64):
//   dye[b][k]  = channel_density[b][k]                        (fp32, verbatim)
//   icmf[b][k] = 10^-base_density[b] * illumD50[b] * cmf[b][k] / normD50
// Bands with NaN channel/base density contribute w = NaN -> 0 in the CPU engine
// for EVERY pixel, so both table rows are zeroed. The matrix is Mc.M (the
// engine's CAT02 round-trip composed with XYZ->sRGB, an exact linear
// composition): the raw XYZ->sRGB matrix alone differs from the engine's
// default output path by up to ~1.5e-4 near black — outside tolerance.
bool build_gpu_scan_tables(const Profile& film, spk_color_space space,
                           GpuScanTables* t) {
    if (film.n_samples != kGpuNB) return false;
    t->dye.assign(kGpuNB * 3, 0.0f);
    t->icmf.assign(kGpuNB * 3, 0.0f);
    const double inv_norm = 1.0 / kNormD50;
    for (int l = 0; l < kGpuNB; ++l) {
        const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
        const float base = film.base_density[static_cast<size_t>(l)];
        if (std::isnan(base) || std::isnan(cd[0]) || std::isnan(cd[1]) ||
            std::isnan(cd[2]))
            continue;  // both rows stay 0
        t->dye[l * 3 + 0] = cd[0];
        t->dye[l * 3 + 1] = cd[1];
        t->dye[l * 3 + 2] = cd[2];
        const double w = std::pow(10.0, -static_cast<double>(base)) *
                         static_cast<double>(kIlluminantD50[l]) * inv_norm;
        for (int k = 0; k < 3; ++k)
            t->icmf[l * 3 + k] =
                static_cast<float>(w * static_cast<double>(kCieCmf1931[l][k]));
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double acc = 0.0;
            for (int k = 0; k < 3; ++k)
                acc += kRGB_to_RGB_CCTF[SPK_CS_SRGB][r * 3 + k] *
                       kXYZ_to_RGB[SPK_CS_SRGB][k * 3 + c];
            t->m_engine[r * 3 + c] = static_cast<float>(acc);
        }
    for (int k = 0; k < 9; ++k)
        t->m_space[k] = static_cast<float>(kXYZ_to_RGB[space][k]);
    return true;
}

// Base per-frame GPU gate: only what NEITHER kernel path can model — the BW
// XYZ correction, whose per-pixel scale depends non-linearly on the pixel's own
// Y (it needs XYZ, which neither kernel outputs). Glare is NOT gated here: it
// is linear in XYZ, so the LINEAR path composes it exactly as a post-pass
// (M·(xyz + g·I) = M·xyz + g·(M·I)); only the FUSED kernel (post-CCTF output)
// cannot take it.
bool gpu_scan_frame_ok(const ScanningParams& p) {
    return !p.bw_xyz_correction;
}

// FUSED-kernel eligibility: the shader's fixed chain (sRGB matrix + CCTF +
// clamp) must BE the frame's whole chain. Frames failing only the fused extras
// (glare/unsharp/lens blur/gamut compress/non-sRGB/CCTF-off) fall to the
// LINEAR kernel + CPU tail instead.
bool gpu_scan_eligible(const ScanningParams& p) {
    const bool glare = p.glare_active && !p.scan_film && p.glare_percent > 0.0f;
    const bool unsharp = p.unsharp_sigma > 0.0 && p.unsharp_amount > 0.0;
    return gpu_scan_frame_ok(p) && !glare && p.output_cctf_encoding &&
           p.output_color_space == SPK_CS_SRGB &&
           p.output_gamut_compress == OutputGamutCompress::kLegacyClip &&
           p.lens_blur <= 0.0 && !unsharp;
}

// One-time on-device self-check (#146 mandate): before the first GPU frame,
// render a small density lattice through the REAL CPU scan() (the engine is
// its own oracle-parity reference on device) and through the GPU kernel, and
// require max_abs <= 1e-4 (the oracle tolerance bar). Pass -> GPU stays on for
// the process; any failure (numeric OR dispatch) -> GPU off for the process,
// state readable via gpu_scan_preview_state() so the JNI layer can log it.
std::atomic<int> g_gpu_scan_state{0};  // 0 unchecked, 1 passed, 2 failed
// Frames actually rendered through a GPU kernel this process (observability,
// #146 on-device validation follow-up: self-check state alone cannot prove the
// path ever ENGAGED — silence was indistinguishable from "never ran").
std::atomic<uint64_t> g_gpu_scan_frames{0};

bool gpu_scan_self_check(const Profile& film) {
    int st = g_gpu_scan_state.load(std::memory_order_acquire);
    if (st != 0) return st == 1;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    st = g_gpu_scan_state.load(std::memory_order_acquire);
    if (st != 0) return st == 1;

    bool ok = false;
    do {
        GpuScanTables t;
        if (!build_gpu_scan_tables(film, SPK_CS_SRGB, &t)) break;

        // 8x8x8 lattice over [-0.1, nanmax(density_curves)] per channel (the
        // probe's sweep domain, coarser) + one NaN pixel for the guard.
        double cmax[3] = {0.0, 0.0, 0.0};
        for (int n = 0; n < film.n_density_pts; ++n) {
            const float* dc = film.density_curves.data() + static_cast<size_t>(n) * 3;
            for (int c = 0; c < 3; ++c) {
                const double v = static_cast<double>(dc[c]);
                if (!std::isnan(v) && v > cmax[c]) cmax[c] = v;
            }
        }
        const int G = 8;
        const double kLo = -0.1;
        const int n = G * G * G + 1;
        std::vector<float> in(static_cast<size_t>(n) * 3);
        size_t i = 0;
        for (int a = 0; a < G; ++a)
            for (int b = 0; b < G; ++b)
                for (int c = 0; c < G; ++c) {
                    in[i * 3 + 0] = static_cast<float>(kLo + (cmax[0] - kLo) * a / (G - 1));
                    in[i * 3 + 1] = static_cast<float>(kLo + (cmax[1] - kLo) * b / (G - 1));
                    in[i * 3 + 2] = static_cast<float>(kLo + (cmax[2] - kLo) * c / (G - 1));
                    ++i;
                }
        in[i * 3 + 0] = in[i * 3 + 1] = in[i * 3 + 2] = std::nanf("");

        // CPU reference: the engine's own default fused path (allow_gpu is
        // default-false on this params object, so no recursion).
        ScanningParams ref;
        std::vector<float> cpu(in.size());
        scan(film, ref, in.data(), n, 1, cpu.data());

        std::vector<float> gpu(in.size(), -1.0f);
        if (!spk::gpu::scan_spectral(in.data(), gpu.data(), static_cast<uint32_t>(n),
                                     t.dye.data(), t.icmf.data(), t.m_engine))
            break;

        // NaN-aware compare: `d > max_abs` is false for NaN, so without the
        // explicit check a driver emitting NaN would sail through the gate.
        double max_abs = 0.0;
        bool any_nan = false;
        for (size_t k = 0; k < gpu.size(); ++k) {
            const double g = static_cast<double>(gpu[k]);
            const double r = static_cast<double>(cpu[k]);
            if (std::isnan(g) || std::isnan(r)) { any_nan = true; break; }
            const double d = std::fabs(g - r);
            if (d > max_abs) max_abs = d;
        }
        if (any_nan || max_abs > 1e-4) break;

        // LINEAR-kernel sub-check: same lattice vs an f64 mirror of the linear
        // chain over the SAME folded tables (KEEP IN SYNC with
        // gpu/scan_spectral_lin.comp: D -> 10^-D -> XYZ -> m_space, unclipped;
        // non-finite densities guarded to 1e4 like the host upload). The fused
        // check above anchors the fold against the real engine output; this one
        // anchors the second pipeline's plumbing + this device's codegen for it.
        std::vector<float> glin(in.size(), -1.0f);
        if (!spk::gpu::scan_spectral_linear(in.data(), glin.data(),
                                            static_cast<uint32_t>(n), t.dye.data(),
                                            t.icmf.data(), t.m_space))
            break;
        double lin_max = 0.0;
        for (int p = 0; p < n; ++p) {
            double cc[3];
            for (int k = 0; k < 3; ++k) {
                const float v = in[static_cast<size_t>(p) * 3 + k];
                cc[k] = std::isfinite(v) ? static_cast<double>(v) : 1e4;
            }
            double X = 0.0, Y = 0.0, Z = 0.0;
            for (int b = 0; b < kGpuNB; ++b) {
                const double D = cc[0] * t.dye[b * 3 + 0] + cc[1] * t.dye[b * 3 + 1] +
                                 cc[2] * t.dye[b * 3 + 2];
                const double T = std::pow(10.0, -D);
                X += T * t.icmf[b * 3 + 0];
                Y += T * t.icmf[b * 3 + 1];
                Z += T * t.icmf[b * 3 + 2];
            }
            for (int r = 0; r < 3; ++r) {
                const double ref = t.m_space[r * 3 + 0] * X +
                                   t.m_space[r * 3 + 1] * Y +
                                   t.m_space[r * 3 + 2] * Z;
                const double g =
                    static_cast<double>(glin[static_cast<size_t>(p) * 3 + r]);
                if (std::isnan(g) || std::isnan(ref)) { lin_max = INFINITY; break; }
                const double d = std::fabs(g - ref);
                if (d > lin_max) lin_max = d;
            }
        }
        ok = lin_max <= 1e-4;  // INFINITY (a NaN seen) fails the gate
    } while (false);

    g_gpu_scan_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}

}  // namespace

int gpu_scan_preview_state() {
    return g_gpu_scan_state.load(std::memory_order_acquire);
}

uint64_t gpu_scan_frames_rendered() {
    return g_gpu_scan_frames.load(std::memory_order_relaxed);
}

void scan(const Profile& film, const ScanningParams& params,
          const float* density_cmy, int width, int height, float* rgb_out) {
    ScopedStage _t_scan(STG_SCAN);  // whole stage (diagnostic; #146/#152)
    const int npix = width * height;
    const int S = film.n_samples;  // == kSpectralSamples (81) for bundled profiles

    // GPU preview fast-path (#146; law revision #149: preview-only until option-B
    // ships). Placed before the LUT build so an engaged GPU frame skips the LUT
    // entirely — the fp32 direct integral (~2e-6 vs the CPU chain, PR #145) is
    // both faster and tighter than the preview's PCHIP LUT (~5e-5). allow_gpu is
    // default-false: every parity test and every export render never reaches
    // this block, so the CPU path below stays byte-identical. Any failure
    // (ineligible frame, no device, failed self-check, failed dispatch) falls
    // through to the unchanged CPU path for this frame.
    if (params.allow_gpu && S == kGpuNB && gpu_scan_eligible(params) &&
        spk::gpu::available() && gpu_scan_self_check(film)) {
        GpuScanTables t;
        if (build_gpu_scan_tables(film, SPK_CS_SRGB, &t) &&
            spk::gpu::scan_spectral(density_cmy, rgb_out,
                                    static_cast<uint32_t>(npix), t.dye.data(),
                                    t.icmf.data(), t.m_engine)) {
            g_gpu_scan_frames.fetch_add(1, std::memory_order_relaxed);
            // Tone curve post-pass: the CPU encode applies it on the same
            // display-referred, clipped values the GPU just produced. Inactive
            // (the default) is an identity, skipped.
            if (params.tone_curve.active) {
                parallel_for(0, npix, [&](int lo, int hi) {
                    for (int p = lo; p < hi; ++p) {
                        float* out = rgb_out + static_cast<size_t>(p) * 3;
                        for (int c = 0; c < 3; ++c)
                            out[c] = params.tone_curve.apply(c, out[c]);
                    }
                });
            }
            return;
        }
    }

    // Linear output-space RGB (pre-unsharp, pre-CAT02, pre-CCTF) stays float64
    // to match scanning.py, which carries the whole chain at NumPy double
    // precision and only stores float32 at the very end. A full-resolution
    // float64 plane is materialized ONLY when something operates on it between
    // the per-pixel compute and the per-pixel encode (gamut compression, lens
    // blur, unsharp — each gate below mirrors that op's own activation
    // condition). Otherwise the two passes fuse per pixel — identical
    // arithmetic on identical operands, byte-identical output — and the
    // ~288 MB (12 MP) plane never exists (EXPORT_FASTPATH item 4).
    const bool do_unsharp =
        params.unsharp_sigma > 0.0 && params.unsharp_amount > 0.0;
    const bool needs_lin_plane =
        params.output_gamut_compress == OutputGamutCompress::kAcesRgc ||
        params.output_gamut_compress == OutputGamutCompress::kOklch ||
        params.output_gamut_compress == OutputGamutCompress::kOklrab ||
        params.lens_blur > 0.0 || do_unsharp;

    // Scan illuminant + constants. For the scan_film route the scan illuminant is
    // the film's viewing illuminant (D50 here). These mirror scanning.py:
    //   normalization = sum(scan_illuminant * ybar)
    const float* illum = kIlluminantD50;
    const double norm = kNormD50;

    // Viewing glare (print route only). scanning.py::_density_to_rgb adds
    //   xyz += glare_amount[:,:,None] * illuminant_xyz[None,None,:]
    // in XYZ space, where illuminant_xyz = contract("k,kl->l", scan_illuminant,
    // CMFS) / normalization. We precompute the per-pixel glare field and the
    // illuminant XYZ once; the field is added to each pixel's XYZ before the
    // XYZ->RGB matrix. Glare is stochastic (model/glare.py draws np.random.randn
    // per pixel) so the result is NOT bit-exact vs the oracle; it is OFF by default
    // (glare_active == false), preserving the deterministic goldens.
    const bool do_glare =
        params.glare_active && !params.scan_film && params.glare_percent > 0.0f;
    std::vector<float> glare_field;
    double illuminant_xyz[3] = {0.0, 0.0, 0.0};
    if (do_glare) {
        for (int l = 0; l < S; ++l) {
            double w = static_cast<double>(illum[l]) / norm;
            illuminant_xyz[0] += w * kCieCmf1931[l][0];
            illuminant_xyz[1] += w * kCieCmf1931[l][1];
            illuminant_xyz[2] += w * kCieCmf1931[l][2];
        }
        glare_field.assign(static_cast<size_t>(npix), 0.0f);
        // The field build is glare's expensive half — a stochastic full-resolution
        // field plus a blur. The per-pixel add below is folded into the scan loops and
        // is not separable from them, so this slot measures the build only. Like
        // scan_spatial it is NESTED inside the STG_SCAN bracket, so it must not be
        // added to a stage total. Until now glare had no slot at all and so cost
        // nothing visible even when switched on (perf-lab §18).
        ScopedStage _tg(STG_GLARE);
        compute_random_glare_amount(params.glare_percent, params.glare_roughness,
                                    params.glare_blur, width, height,
                                    params.glare_seed, glare_field.data());
    }

    const double inv_norm = 1.0 / norm;

    // GPU LINEAR-variant attempt (#146): frames the fused kernel cannot model —
    // unsharp (ON at (0.7, 0.7) in the production defaults, so interactive
    // previews land HERE, not in the fused branch above), lens blur, gamut
    // compression, non-sRGB output, CCTF-off — still offload the 81-band
    // integral: the linear kernel fills the same lin plane compute_pixel would
    // have produced (fp32-quantized), and the UNCHANGED CPU plane ops + encode
    // tail run on it. On success the scanner LUT below is skipped entirely (the
    // direct fp32 integral, ~2e-6, is tighter than the LUT's ~5e-5). Any
    // failure falls through to the CPU path for this frame.
    std::unique_ptr<double[]> lin_buf;
    if (params.allow_gpu && S == kGpuNB && gpu_scan_frame_ok(params) &&
        spk::gpu::available() && gpu_scan_self_check(film)) {
        GpuScanTables t;
        if (build_gpu_scan_tables(film, params.output_color_space, &t)) {
            std::vector<float> gpu_lin(static_cast<size_t>(npix) * 3);
            if (spk::gpu::scan_spectral_linear(density_cmy, gpu_lin.data(),
                                               static_cast<uint32_t>(npix),
                                               t.dye.data(), t.icmf.data(),
                                               t.m_space)) {
                lin_buf.reset(new double[static_cast<size_t>(npix) * 3]);
                double* const lin = lin_buf.get();
                // Viewing glare composes linearly past the matrix:
                //   M·(xyz + g·I) = M·xyz + g·(M·I),
                // so the CPU's XYZ-space add becomes an AXPY with the constant
                // 3-vector M·illuminant_xyz on the GPU's linear output — same
                // math as compute_pixel's add_glare, reassociated (fp
                // difference ~1e-7, well inside the preview band). The glare
                // FIELD itself (seeded stochastic + blur) was computed on the
                // CPU above, exactly as for the CPU path. The default print
                // preview (grain on -> glare on) lands here.
                double gI[3] = {0.0, 0.0, 0.0};
                if (do_glare) {
                    const double* M = kXYZ_to_RGB[params.output_color_space];
                    for (int r = 0; r < 3; ++r)
                        gI[r] = M[r * 3 + 0] * illuminant_xyz[0] +
                                M[r * 3 + 1] * illuminant_xyz[1] +
                                M[r * 3 + 2] * illuminant_xyz[2];
                }
                parallel_for(0, npix, [&](int lo, int hi) {
                    if (do_glare) {
                        for (int p = lo; p < hi; ++p) {
                            const double g = static_cast<double>(glare_field[p]);
                            for (int c = 0; c < 3; ++c)
                                lin[static_cast<size_t>(p) * 3 + c] =
                                    static_cast<double>(gpu_lin[static_cast<size_t>(p) * 3 + c]) +
                                    g * gI[c];
                        }
                    } else {
                        for (size_t i = static_cast<size_t>(lo) * 3,
                                    e = static_cast<size_t>(hi) * 3;
                             i < e; ++i)
                            lin[i] = static_cast<double>(gpu_lin[i]);
                    }
                });
            }
        }
    }
    const bool gpu_lin_done = static_cast<bool>(lin_buf);
    if (gpu_lin_done) g_gpu_scan_frames.fetch_add(1, std::memory_order_relaxed);

    // OPT-IN scanner 3D-LUT acceleration (params.use_lut, default false). Mirrors
    // scanning.py::_density_to_rgb routing the per-pixel cmy_to_log_xyz spectral
    // integral through SpectralLUTService.spectral_compute_scanner(use_lut=...):
    // when on, a per-channel uniform 3D LUT is built over [data_min, data_max] at
    // params.lut_resolution steps (utils.lut.compute_with_lut / _create_lut_3d) and
    // the density image is interpolated with the PCHIP path (kernels/lut3d,
    // apply_lut_3d default) instead of evaluating the spectral integral per pixel.
    // The LUT covers EXACTLY the density_cmy -> log_xyz step; everything after
    // (10^log_xyz, glare, XYZ->RGB) is shared with the direct path. Interpolation is
    // NOT bit-exact vs the direct evaluation (documented ~5e-5), so it is OPT-IN and
    // the default path (use_lut == false) never even constructs the LUT.
    //
    // Domain bounds (scanning.py::_density_to_rgb):
    //   scan_film : data_min = -film_render.grain.density_min,
    //               data_max =  np.nanmax(film.data.density_curves, axis=0)
    //   print scan: data_min =  np.nanmin(print.data.density_curves, axis=0),
    //               data_max =  np.nanmax(print.data.density_curves, axis=0)
    std::vector<double> lut_log_xyz;  // (npix*3) when use_lut, else empty
    if (params.use_lut && !gpu_lin_done) {  // a GPU-filled plane needs no LUT
        // Per-channel domain bounds from the (passed) profile's density_curves.
        double xmin[3], xmax[3];
        const int N = film.n_density_pts;
        double cmax[3] = {-INFINITY, -INFINITY, -INFINITY};
        double cmin[3] = {INFINITY, INFINITY, INFINITY};
        for (int nrow = 0; nrow < N; ++nrow) {
            const float* dc = film.density_curves.data() + static_cast<size_t>(nrow) * 3;
            for (int c = 0; c < 3; ++c) {
                double v = static_cast<double>(dc[c]);
                if (!std::isnan(v)) {  // np.nanmax / np.nanmin
                    if (v > cmax[c]) cmax[c] = v;
                    if (v < cmin[c]) cmin[c] = v;
                }
            }
        }
        for (int c = 0; c < 3; ++c) {
            if (params.scan_film) {
                xmin[c] = -params.grain_density_min[c];
                xmax[c] = cmax[c];
            } else {
                xmin[c] = cmin[c];
                xmax[c] = cmax[c];
            }
        }

        // Clamp the resolution to a sane band (compute_with_lut needs steps >= 2 to
        // form a non-degenerate grid; cap to bound build cost steps^3).
        int steps = params.lut_resolution;
        if (steps < 2) steps = 2;
        if (steps > 192) steps = 192;

        // Build the LUT by sampling the SAME cmy_to_log_xyz transform the direct
        // path evaluates (shared via cmy_to_log_xyz_fn) over [xmin, xmax].
        CmyToLogXyzCtx ctx;
        ctx.channel_density = film.channel_density.data();
        ctx.base_density = film.base_density.data();
        ctx.illum = illum;
        ctx.cmf = kCieCmf1931;
        ctx.S = S;
        ctx.inv_norm = inv_norm;

        // PERF (kernels/lut3d_cache.h): with an engine cache attached, fetch the
        // memoized build instead of redoing steps^3 spectral integrals every call.
        // The key folds EVERY value cmy_to_log_xyz reads plus the grid that samples
        // it, as raw IEEE-754 bytes:
        //   - the film's channel_density + base_density spectra (ctx.channel_density
        //     / ctx.base_density, whole vectors — a superset of the S entries read),
        //   - the sample count S,
        //   - the resolved domain bounds xmin/xmax, which already encode the route
        //     (scan_film vs print) and its inputs: nanmin/nanmax of the profile's
        //     density_curves and params.grain_density_min,
        //   - the clamped step count.
        // scan_film itself is folded too, so the two routes can never share a slot
        // even if their bounds coincided. ctx.illum (kIlluminantD50), ctx.cmf
        // (kCieCmf1931) and ctx.inv_norm (1/kNormD50) are compile-time constants —
        // they cannot differ between calls, so they need no fold. Anything added to
        // CmyToLogXyzCtx later MUST be added here as well.
        std::shared_ptr<const PreparedLut3D> prepared;
        if (params.lut_cache) {
            std::string key;
            lut_key_append_tag(&key, "scan3d");
            const uint8_t route = params.scan_film ? 1u : 0u;
            lut_key_append(&key, route);
            lut_key_append(&key, S);
            lut_key_append(&key, film.channel_density.data(),
                           film.channel_density.size());
            lut_key_append(&key, film.base_density.data(),
                           film.base_density.size());
            lut_key_append(&key, xmin, 3);
            lut_key_append(&key, xmax, 3);
            lut_key_append(&key, steps);
            prepared = params.lut_cache->get_or_build(key, xmin, xmax, steps,
                                                      &cmy_to_log_xyz_fn, &ctx);
        } else {
            prepared = prepare_lut_3d_pchip(
                build_lut_3d(xmin, xmax, steps, {}, &cmy_to_log_xyz_fn, &ctx));
        }

        // Interpolate the whole density image (compute_with_lut: normalize by
        // (data - xmin)/(xmax - xmin) then PCHIP-interpolate) -> per-pixel log_xyz.
        lut_log_xyz.resize(static_cast<size_t>(npix) * 3);
        std::vector<double> dens_d(static_cast<size_t>(npix) * 3);
        for (size_t i = 0; i < dens_d.size(); ++i)
            dens_d[i] = static_cast<double>(density_cmy[i]);
        apply_prepared_lut_3d_pchip(*prepared, dens_d.data(), width, height,
                                    lut_log_xyz.data());
    }

    // The Python reference computes the whole chain in float64 (NumPy default for
    // the profile arrays) and only stores float32 at the very end. To reproduce it
    // bit-for-bit we mirror that: spectral density, light, the XYZ integral, the
    // matrix product and the CCTF are all done in double; only the final write is
    // float32.
    //
    // Per-pixel compute: density -> XYZ -> corrections -> linear output RGB into
    // lin[3]. One body, shared verbatim by the fused and plane paths below.
    auto compute_pixel = [&](int p, double* lin) {
        double xyz[3];
        if (params.use_lut) {
            // 1-4 replaced by the LUT-interpolated log_xyz (opt-in path). The
            // 10^log_xyz round-trip below is identical to the direct path.
            const double* lx = lut_log_xyz.data() + static_cast<size_t>(p) * 3;
            xyz[0] = std::pow(10.0, lx[0]);
            xyz[1] = std::pow(10.0, lx[1]);
            xyz[2] = std::pow(10.0, lx[2]);
        } else {
            const float* dcmy = density_cmy + static_cast<size_t>(p) * 3;
            const double c0 = static_cast<double>(dcmy[0]);
            const double c1 = static_cast<double>(dcmy[1]);
            const double c2 = static_cast<double>(dcmy[2]);

            // 1. density_cmy -> spectral density (emulsion.compute_density_spectral):
            //    spectral[l] = sum_k dcmy[k] * channel_density[l,k] + base_density[l].
            //    NaN channel_density / base entries propagate as NaN here.
            // 2. light = density_to_light(spectral, illuminant): 10^(-D) * illuminant,
            //    NaN -> 0 (utils/conversions.density_to_light).
            // 3. xyz = sum_l light[l] * CMF[l] / normalization. scanning.py uses a
            //    plain einsum over wavelengths with NO 5 nm interval factor, so we
            //    integrate without dlambda (the interval cancels against the
            //    normalization's own missing interval).
            // SIMD: the dominant per-band cost is the 10^(-spectral) transcendental.
            // We evaluate it kExp10Lanes (2) bands at a time with the vector exp10
            // (kernels/exp10), which matches std::pow(10,x) to <=4 ULP — byte-identical
            // after the final float32 cast (see kernels/exp10.h). The spectral madd and
            // the X/Y/Z accumulation stay in band order (lane 0 then lane 1, then the
            // odd tail) so the reduction is unchanged and thread-count invariant.
            double X = 0.0, Y = 0.0, Z = 0.0;
            int l = 0;
            for (; l + kExp10Lanes <= S; l += kExp10Lanes) {
                exp10_vd negspec;
                for (int q = 0; q < kExp10Lanes; ++q) {
                    const float* cd =
                        film.channel_density.data() + static_cast<size_t>(l + q) * 3;
                    negspec[q] = -(c0 * static_cast<double>(cd[0]) +
                                   c1 * static_cast<double>(cd[1]) +
                                   c2 * static_cast<double>(cd[2]) +
                                   static_cast<double>(film.base_density[l + q]));
                }
                exp10_vd ev = exp10_vec(negspec);
                for (int q = 0; q < kExp10Lanes; ++q) {
                    double w = ev[q] * static_cast<double>(illum[l + q]);
                    if (std::isnan(w)) w = 0.0;
                    X += w * kCieCmf1931[l + q][0];
                    Y += w * kCieCmf1931[l + q][1];
                    Z += w * kCieCmf1931[l + q][2];
                }
            }
            for (; l < S; ++l) {  // odd-band tail (same exp10 arithmetic, scalar).
                const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
                double spectral = c0 * static_cast<double>(cd[0]) +
                                  c1 * static_cast<double>(cd[1]) +
                                  c2 * static_cast<double>(cd[2]) +
                                  static_cast<double>(film.base_density[l]);
                double w = exp10_scalar(-spectral) * static_cast<double>(illum[l]);
                if (std::isnan(w)) w = 0.0;
                X += w * kCieCmf1931[l][0];
                Y += w * kCieCmf1931[l][1];
                Z += w * kCieCmf1931[l][2];
            }
            // 4. log_xyz = log10(max(xyz,0) + 1e-10); xyz = 10^log_xyz. The log/exp
            //    round-trip just floors at 1e-10 and clamps negatives; reproduce that
            //    exactly so float rounding matches the reference.
            xyz[0] = std::pow(10.0, std::log10(std::fmax(X * inv_norm, 0.0) + 1e-10));
            xyz[1] = std::pow(10.0, std::log10(std::fmax(Y * inv_norm, 0.0) + 1e-10));
            xyz[2] = std::pow(10.0, std::log10(std::fmax(Z * inv_norm, 0.0) + 1e-10));
        }

        // Scanner BLACK/WHITE XYZ correction (color_reference.py::
        // black_white_xyz_correction): y = xyz[Y]; y_corrected = clip(m*y+q, 0, 1);
        // scale = y_corrected / (y + 1e-10); xyz *= scale. Default OFF (the four
        // scanner_*_correction params default false) => skipped, so the negative
        // scan_film route and every pre-existing golden stay bit-exact.
        if (params.bw_xyz_correction) {
            double y = xyz[1];
            double yc = params.bw_xyz_m * y + params.bw_xyz_q;
            if (yc < 0.0) yc = 0.0;
            else if (yc > 1.0) yc = 1.0;
            double scale = yc / (y + 1e-10);
            xyz[0] *= scale;
            xyz[1] *= scale;
            xyz[2] *= scale;
        }

        // add_glare (print route only): xyz += glare[p] * illuminant_xyz. On the
        // scan_film route glare is None (do_glare false) so this is skipped.
        if (do_glare) {
            double g = static_cast<double>(glare_field[p]);
            xyz[0] += g * illuminant_xyz[0];
            xyz[1] += g * illuminant_xyz[1];
            xyz[2] += g * illuminant_xyz[2];
        }

        // 5. XYZ -> output RGB (linear, in io.output_color_space), with CAT02
        //    from the D50 scan whitepoint to the space whitepoint baked into the
        //    matrix (colour.XYZ_to_RGB(..., illuminant=D50_xy)).
        const double* M = kXYZ_to_RGB[params.output_color_space];
        for (int c = 0; c < 3; ++c) {
            lin[c] = M[c * 3 + 0] * xyz[0] +
                     M[c * 3 + 1] * xyz[1] +
                     M[c * 3 + 2] * xyz[2];
        }
    };

    // 6. CCTF encode + clip per pixel (scanning._apply_cctf_encoding_and_clip).
    //    When output_cctf_encoding is on, colour.RGB_to_RGB(cs, cs, "CAT02")
    //    applies the near-identity round-trip matrix *and* the per-space CCTF;
    //    when off, neither is applied (only the clip). The clip preserves NaN
    //    (np.clip semantics), which is load-bearing for Adobe RGB gamut
    //    excursions where the gamma encode yields NaN for negative linear RGB.
    const double* Mc = kRGB_to_RGB_CCTF[params.output_color_space];
    const spk_color_space cs = params.output_color_space;
    auto encode_pixel = [&](int p, const double* lin) {
        float* out = rgb_out + static_cast<size_t>(p) * 3;
        for (int c = 0; c < 3; ++c) {
            double v;
            if (params.output_cctf_encoding) {
                double adapted = Mc[c * 3 + 0] * lin[0] +
                                 Mc[c * 3 + 1] * lin[1] +
                                 Mc[c * 3 + 2] * lin[2];
                v = output_cctf_encode(cs, adapted);
            } else {
                v = lin[c];
            }
            // np.clip(v, 0, 1): preserve NaN, clamp finite to [0, 1].
            if (v < 0.0) v = 0.0;
            else if (v > 1.0) v = 1.0;
            // Optional tone curve on the display-referred value (identity no-op by
            // default; NaN passes through). Applied per channel, in [0,1].
            out[c] = params.tone_curve.apply(c, static_cast<float>(v));
        }
    };

    // Fused path: nothing operates between compute and encode, so each pixel
    // goes straight through — same ops, same operands, byte-identical output —
    // and the full-resolution float64 plane never exists. (A GPU-filled plane
    // — e.g. a non-sRGB frame the fused kernel could not take — continues into
    // the plane path below instead: its ops are all gated off, leaving just the
    // encode pass.)
    if (!needs_lin_plane && !gpu_lin_done) {
        parallel_for(0, npix, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p) {
                double lin[3];
                compute_pixel(p, lin);
                encode_pixel(p, lin);
            }
        });
        return;
    }

    // Plane path: materialize lin_rgb for the ops below (unless the GPU linear
    // kernel already filled it). Allocated UNINITIALIZED — compute_pixel writes
    // every element before anything reads it, so the old vector
    // value-initialization only cost a ~288 MB memset at 12 MP
    // (EXPORT_FASTPATH item 4).
    if (!lin_buf) {
        lin_buf.reset(new double[static_cast<size_t>(npix) * 3]);
        double* const fill = lin_buf.get();
        parallel_for(0, npix, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                compute_pixel(p, fill + static_cast<size_t>(p) * 3);
        });
    }
    double* const lin_rgb = lin_buf.get();

    // Scanner spatial branch (gamut compress + lens blur + unsharp) — a
    // SUB-MEASURE of STG_SCAN. Unsharp is ON by the production defaults, so this
    // is where the scan stage's own interactive cost concentrates (#146).
    ScopedStage _t_spatial(STG_SCAN_SPATIAL);

    // OPT-IN output gamut compression, applied in the linear output space at the
    // oracle's position (scanning.py::_density_to_rgb: right after XYZ->RGB and
    // BEFORE blur/unsharp). Default kLegacyClip => skipped, so lin_rgb is untouched
    // and every pre-existing golden stays byte-identical. kAcesRgc compresses
    // out-of-cube chromaticities toward the achromatic axis with the ACES RGC v1.3
    // per-channel knee (model/gamut_compression.cpp).
    if (params.output_gamut_compress == OutputGamutCompress::kAcesRgc) {
        compress_rgb_aces_rgc(lin_rgb, npix, params.gamut_knee_threshold,
                              params.gamut_knee_limit, params.gamut_knee_power);
    } else if (params.output_gamut_compress == OutputGamutCompress::kOklch) {
        // OkLch perceptual chroma reduction toward the output RGB cube. The
        // spk_color_space enum (0..5) is exactly the golden's space_index, so the
        // per-space RGB<->XYZ matrix + C_max table are selected by the raw index.
        compress_rgb_oklch_chroma(lin_rgb, npix,
                                  static_cast<int>(params.output_color_space),
                                  params.gamut_knee_threshold, params.gamut_knee_limit,
                                  params.gamut_knee_power);
    } else if (params.output_gamut_compress == OutputGamutCompress::kOklrab) {
        // Same chroma reduction as kOklch, but the C_max lookup is indexed by
        // Ottosson's rebased lightness Lr (model/gamut_compression.cpp) for a more
        // perceptually uniform knee across light/dark. Same per-space selection.
        compress_rgb_oklrab_chroma(lin_rgb, npix,
                                   static_cast<int>(params.output_color_space),
                                   params.gamut_knee_threshold, params.gamut_knee_limit,
                                   params.gamut_knee_power);
    }

    // Scanner lens blur (scanner.lens_blur, in pixels): a per-channel 2D Gaussian
    // applied in the linear output space BEFORE the unsharp mask, matching
    // scanning.py::_apply_blur_and_unsharp (apply_gaussian_blur then
    // apply_unsharp_mask). apply_gaussian_blur gates on sigma > 0 and uses a scalar
    // sigma broadcast across the 3 channels. Default lens_blur == 0 => skipped, so
    // the existing goldens stay bit-exact.
    if (params.lens_blur > 0.0) {
        double sg[3] = {params.lens_blur, params.lens_blur, params.lens_blur};
        gaussian_blur_per_channel_d(lin_rgb, width, height, 3, sg);
    }

    // Scanner unsharp mask (spatial branch): rgb += amount * (rgb - G(sigma)*rgb),
    // in the linear output space, after the lens blur and before the CAT02
    // round-trip + CCTF. (apply_gaussian_blur / apply_unsharp_mask in
    // model/diffusion.py.)
    if (do_unsharp) {
        const size_t total = static_cast<size_t>(npix) * 3;
        std::vector<double> blur(lin_rgb, lin_rgb + total);
        double sg[3] = {params.unsharp_sigma, params.unsharp_sigma,
                        params.unsharp_sigma};
        gaussian_blur_per_channel_d(blur.data(), width, height, 3, sg);
        const double amt = params.unsharp_amount;
        for (size_t i = 0; i < total; ++i)
            lin_rgb[i] = lin_rgb[i] + amt * (lin_rgb[i] - blur[i]);
    }

    // Encode the (compressed/blurred/sharpened) plane — the same encode_pixel
    // the fused path runs, on the same values.
    parallel_for(0, npix, [&](int lo, int hi) {
        for (int p = lo; p < hi; ++p)
            encode_pixel(p, lin_rgb + static_cast<size_t>(p) * 3);
    });
}

}  // namespace spk
