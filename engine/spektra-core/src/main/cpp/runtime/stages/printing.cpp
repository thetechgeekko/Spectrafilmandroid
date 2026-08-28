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
 * spektrafilm.
 */
#include "runtime/stages/printing.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/vulkan_compute.h"
#include "kernels/exp10.h"
#include "kernels/lut3d.h"
#include "kernels/lut3d_cache.h"
#include "kernels/parallel.h"
#include "runtime/stage_timer.h"
#include "model/density_curves.h"
#include "model/diffusion.h"
#include "model/morph_curves.h"

namespace spk {

namespace {

// Context + adapter for the OPT-IN enlarger 3D-LUT. Samples the SAME
// _film_cmy_to_print_log_raw transform the direct path evaluates: from a film
// CMY density triple, the spectral integral against the (precomputed) print
// sensitivity and filtered illuminant, times the midgray exposure factor, then
// log10. Scalar libm exp10 here (the LUT is an approximation anyway; the direct
// per-pixel path keeps its byte-exact exp10_vec SIMD untouched).
struct EnlargerLutCtx {
    const float* channel_density;     // film dyes, (S*3,) row-major [l*3+k]
    const float* base_density;        // film base, (S,)
    const double* sens;               // print sensitivity, (S*3,) [l*3+k]
    const double* filtered_illuminant; // enlarger illuminant * dichroic, (S,)
    int S;
    double exposure_factor_midgray;
    // Constant per-print-channel preflash raw 3-vector (already scaled by
    // preflash_exposure), added to raw AFTER the midgray factor. {0,0,0} when
    // preflash is off, so the LUT samples the exact same transform as the direct
    // path (printing.py::_film_cmy_to_print_log_raw, raw += _compute_raw_preflash).
    double preflash_raw[3];
};

void cmy_to_print_log_raw_fn(const double in[3], double out[3], void* vctx) {
    const EnlargerLutCtx& c = *static_cast<const EnlargerLutCtx*>(vctx);
    double raw0 = 0.0, raw1 = 0.0, raw2 = 0.0;
    for (int l = 0; l < c.S; ++l) {
        const float* cd = c.channel_density + static_cast<size_t>(l) * 3;
        const double spectral = in[0] * static_cast<double>(cd[0]) +
                                in[1] * static_cast<double>(cd[1]) +
                                in[2] * static_cast<double>(cd[2]) +
                                static_cast<double>(c.base_density[l]);
        double light = std::pow(10.0, -spectral) * c.filtered_illuminant[l];
        if (std::isnan(light)) light = 0.0;
        const double* sl = c.sens + static_cast<size_t>(l) * 3;
        raw0 += light * sl[0];
        raw1 += light * sl[1];
        raw2 += light * sl[2];
    }
    raw0 *= c.exposure_factor_midgray;
    raw1 *= c.exposure_factor_midgray;
    raw2 *= c.exposure_factor_midgray;
    raw0 += c.preflash_raw[0];  // raw += _compute_raw_preflash (0 when off)
    raw1 += c.preflash_raw[1];
    raw2 += c.preflash_raw[2];
    out[0] = std::log10(std::fmax(raw0, 0.0) + 1e-10);
    out[1] = std::log10(std::fmax(raw1, 0.0) + 1e-10);
    out[2] = std::log10(std::fmax(raw2, 0.0) + 1e-10);
}

// =========================================================================
// GPU print-expose offload (perf lab, first rung of #148's full-chain GPU).
//
// The print integral turns out to be the SAME SHAPE as the scan integral the
// LINEAR kernel (gpu/scan_spectral_lin.comp) already runs:
//
//   scan  : out[k] = sum_b 10^-(c . dye[b])            * icmf[b][k]
//   print : raw[k] = sum_b 10^-(c . cd[b] + base[b])   * fi[b] * sens[b][k]
//
// so no new shader is needed — only a different fold of the per-band constants,
// plus an IDENTITY matrix in the kernel's XYZ->RGB slot (the print integral's
// output is already the value we want, not a colour-space conversion of it):
//
//   dye[b][k]  = film.channel_density[b][k]                    (identical to scan)
//   icmf[b][k] = 10^-base_density[b] * filtered_illuminant[b] * sens[b][k]
//
// Reusing the validated kernel is the whole point: the arithmetic the PR #145
// device probe measured inside the oracle bar is the arithmetic that runs here.
//
// PREVIEW/EXPERIMENTAL, exactly like the scan offload: gated by
// params.allow_gpu (fed from spk_params' allow_gpu_scan latch), re-gated per
// frame, validated once on device, and any failure at any point falls straight
// back to the exact CPU integral for that frame.
// =========================================================================
constexpr int kGpuNB = 81;  // the shader's fixed band count

std::atomic<int> g_gpu_print_state{0};      // 0 unchecked, 1 passed, 2 failed
std::atomic<long long> g_gpu_print_frames{0};

// NaN contract, mirroring build_gpu_scan_tables: a band whose base or channel
// density (or filtered illuminant) is NaN makes `light` NaN, which the CPU
// engine zeroes for EVERY pixel, so BOTH table rows are zeroed here. With dye
// zeroed the shader computes D = 0 and 10^0 * 0 == 0 — reproducing the CPU's
// nan_to_num exactly rather than propagating a NaN the shader has no guard for.
// `sens` cannot carry NaN: it is nan_to_num'd where it is built.
bool build_gpu_print_tables(const Profile& film, const PrintingParams& params,
                            const std::vector<double>& sens,
                            std::vector<float>* dye, std::vector<float>* icmf) {
    if (film.n_samples != kGpuNB) return false;
    dye->assign(kGpuNB * 3, 0.0f);
    icmf->assign(kGpuNB * 3, 0.0f);
    for (int l = 0; l < kGpuNB; ++l) {
        const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
        const float base = film.base_density[static_cast<size_t>(l)];
        const double fi = params.filtered_illuminant[l];
        if (std::isnan(base) || std::isnan(cd[0]) || std::isnan(cd[1]) ||
            std::isnan(cd[2]) || std::isnan(fi))
            continue;  // both rows stay 0
        (*dye)[l * 3 + 0] = cd[0];
        (*dye)[l * 3 + 1] = cd[1];
        (*dye)[l * 3 + 2] = cd[2];
        const double w = std::pow(10.0, -static_cast<double>(base)) * fi;
        for (int k = 0; k < 3; ++k)
            (*icmf)[l * 3 + k] =
                static_cast<float>(w * sens[static_cast<size_t>(l) * 3 + k]);
    }
    return true;
}

// The CPU integral, evaluated for the self-check grid with plain libm math.
// Deliberately a separate transcription rather than a call into the per-pixel
// loop: the check must compare the GPU against what the ENGINE computes, and
// inlining it here keeps the two visibly side by side.
void print_raw_cpu_reference(const Profile& film, const PrintingParams& params,
                             const std::vector<double>& sens, const float* cmy,
                             int n, double* raw_out) {
    for (int p = 0; p < n; ++p) {
        const double c0 = static_cast<double>(cmy[p * 3 + 0]);
        const double c1 = static_cast<double>(cmy[p * 3 + 1]);
        const double c2 = static_cast<double>(cmy[p * 3 + 2]);
        double r[3] = {0.0, 0.0, 0.0};
        for (int l = 0; l < kGpuNB; ++l) {
            const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
            const double spectral = c0 * static_cast<double>(cd[0]) +
                                    c1 * static_cast<double>(cd[1]) +
                                    c2 * static_cast<double>(cd[2]) +
                                    static_cast<double>(film.base_density[l]);
            double light = std::pow(10.0, -spectral) * params.filtered_illuminant[l];
            if (std::isnan(light)) light = 0.0;
            for (int k = 0; k < 3; ++k)
                r[k] += light * sens[static_cast<size_t>(l) * 3 + k];
        }
        for (int k = 0; k < 3; ++k) raw_out[static_cast<size_t>(p) * 3 + k] = r[k];
    }
}

// One-time on-device validation, same contract as gpu_scan_self_check: sweep the
// density domain the print route actually sees (plus one NaN pixel, so the
// upload guard is exercised), run both paths, and only trust the GPU if it
// agrees within the oracle band. The verdict is cached process-wide — it
// validates the DEVICE's arithmetic and the fold's shape, not one frame's
// numbers, so a later frame with different enlarger filters reuses it.
//
// Compared in LOG-RAW space, not raw: log_raw is what flows on to the paper
// curves, and raw itself spans orders of magnitude, where an absolute
// tolerance would mean nothing.
bool gpu_print_self_check(const Profile& film, const PrintingParams& params,
                          const std::vector<double>& sens,
                          const std::vector<float>& dye,
                          const std::vector<float>& icmf) {
    int st = g_gpu_print_state.load(std::memory_order_acquire);
    if (st != 0) return st == 1;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    st = g_gpu_print_state.load(std::memory_order_acquire);
    if (st != 0) return st == 1;

    double cmax[3] = {0.0, 0.0, 0.0};
    for (int nrow = 0; nrow < film.n_density_pts; ++nrow) {
        const float* dc = film.density_curves.data() + static_cast<size_t>(nrow) * 3;
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

    std::vector<double> cpu(static_cast<size_t>(n) * 3);
    print_raw_cpu_reference(film, params, sens, in.data(), n, cpu.data());

    static const float kIdentity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                       0.0f, 0.0f, 0.0f, 1.0f};
    std::vector<float> gpu(static_cast<size_t>(n) * 3, -1.0f);
    bool ok = spk::gpu::scan_spectral_linear(in.data(), gpu.data(),
                                             static_cast<uint32_t>(n), dye.data(),
                                             icmf.data(), kIdentity);
    if (ok) {
        double worst = 0.0;
        for (size_t k = 0; k < gpu.size(); ++k) {
            const double a = std::log10(std::fmax(static_cast<double>(gpu[k]), 0.0) + 1e-10);
            const double b = std::log10(std::fmax(cpu[k], 0.0) + 1e-10);
            const double d = std::fabs(a - b);
            if (d > worst) worst = d;
        }
        ok = worst <= 1e-4;
    }
    g_gpu_print_state.store(ok ? 1 : 2, std::memory_order_release);
    return ok;
}

}  // namespace

void print_expose(const Profile& film, const Profile& print_profile,
                  const PrintingParams& params, const float* density_cmy,
                  int width, int height, float* log_raw_print_out) {
    ScopedStage _t(STG_PRINT_EXPOSE);  // diagnostic (#146/#152)
    const int npix = width * height;
    const int S = film.n_samples;  // 81 for bundled profiles.
    const bool diffusion = params.diffusion_filter.active;
    // When the enlarger diffusion filter is active the spatial pass runs on the
    // float64 print irradiance (raw = 10^log_raw * print_exposure * bw) BEFORE
    // the final log10, mirroring printing.py::expose. Otherwise the pointwise
    // round trip below writes directly into log_raw_print_out and `raw_buf`
    // is unused, keeping the default (no-op) path byte-identical.
    std::vector<double> raw_buf;
    if (diffusion) raw_buf.resize(static_cast<size_t>(npix) * 3);

    // print sensitivity = nan_to_num(10**log_sensitivity) on the working shape.
    // Precompute once: print_profile.log_sensitivity is (S*3,) row-major [s*3+k].
    std::vector<double> sens(static_cast<size_t>(S) * 3);
    for (int l = 0; l < S; ++l) {
        for (int k = 0; k < 3; ++k) {
            double v = std::pow(10.0, static_cast<double>(
                          print_profile.log_sensitivity[static_cast<size_t>(l) * 3 + k]));
            if (std::isnan(v)) v = 0.0;  // np.nan_to_num
            sens[static_cast<size_t>(l) * 3 + k] = v;
        }
    }

    // PREFLASH (printing.py::_compute_raw_preflash): a uniform pre-exposure flash
    // of the print paper. When enlarger.preflash_exposure > 0 the print raw gets a
    // constant per-channel term added (after the midgray factor, before log10):
    //   light_preflash[l] = 10^-base_density[l] * preflash_illuminant[l]  (NaN->0)
    //   raw_preflash[k]   = sum_l light_preflash[l] * sens[l,k]
    //   preflash_raw[k]   = raw_preflash[k] * preflash_exposure
    // The term is constant across pixels (it depends only on the film base density,
    // the preflash-filtered enlarger illuminant, and the print sensitivity), so it
    // is computed once here. preflash_exposure == 0 (the default / no-preflash
    // guard `if preflash_exposure > 0`) => preflash_raw stays {0,0,0}, a STRICT
    // no-op that keeps the default path byte-identical.
    double preflash_raw[3] = {0.0, 0.0, 0.0};
    if (params.preflash_exposure > 0.0) {
        for (int l = 0; l < S; ++l) {
            double light = std::pow(10.0, -static_cast<double>(film.base_density[l])) *
                           params.preflash_illuminant[l];
            if (std::isnan(light)) light = 0.0;  // density_to_light NaN -> 0
            const double* sl = sens.data() + static_cast<size_t>(l) * 3;
            preflash_raw[0] += light * sl[0];
            preflash_raw[1] += light * sl[1];
            preflash_raw[2] += light * sl[2];
        }
        preflash_raw[0] *= params.preflash_exposure;
        preflash_raw[1] *= params.preflash_exposure;
        preflash_raw[2] *= params.preflash_exposure;
    }

    // OPT-IN enlarger 3D-LUT acceleration (params.use_enlarger_lut, default
    // false). Mirrors printing.py::expose routing _film_cmy_to_print_log_raw
    // through SpectralLUTService.spectral_compute_enlarger(use_lut=...): when on,
    // a per-channel uniform PCHIP 3D LUT is built over the film-density domain
    // [data_min, data_max] = [-grain.density_min, nanmax(film.density_curves)] at
    // params.lut_resolution steps (utils.lut.compute_with_lut) and the film
    // density image is interpolated to log_raw_print instead of evaluating the
    // spectral integral per pixel. The LUT covers EXACTLY the cmy ->
    // _film_cmy_to_print_log_raw step; the 10^lr * print_exposure * bw tail
    // (+ optional diffusion) below is shared with the direct path. Interpolation
    // is NOT bit-exact vs the direct evaluation (~5e-5), so it is OPT-IN and the
    // default path (use_enlarger_lut == false) never even constructs the LUT,
    // staying byte-identical to the per-pixel exp10_vec integral.
    std::vector<double> lut_lr;  // (npix*3) when use_enlarger_lut, else empty
    // EXPERIMENTAL GPU offload of the spectral integral (perf lab, first rung of
    // #148). Fills the SAME lut_lr plane the opt-in enlarger LUT would, so the
    // whole tail below — midgray factor, preflash, the 10^/log10 round trip, the
    // optional diffusion filter — runs unchanged on the CPU. Any failure at any
    // step leaves lut_lr empty, so the route falls back to the enlarger LUT if
    // the user opted into it, and otherwise to the exact per-pixel integral.
    //
    // ORDER MATTERS, and it is the scan offload's precedent: on success this
    // SKIPS the enlarger LUT build entirely, rather than deferring to it. The
    // direct fp32 integral (~3e-6 vs the f64 chain, measured by
    // tests/test_gpu_host.cpp) is TIGHTER than the LUT's own ~5e-5
    // interpolation error, so preferring the LUT would trade accuracy away for
    // nothing. It also matters practically: spk_simulate_preview force-enables
    // both spectral LUTs, so gating this behind !use_enlarger_lut would mean the
    // offload never engaged on the interactive path at all — which is the one
    // that needs it.
    bool gpu_lr_done = false;
    if (params.allow_gpu && S == kGpuNB && spk::gpu::available()) {
        std::vector<float> dye, icmf;
        if (build_gpu_print_tables(film, params, sens, &dye, &icmf) &&
            gpu_print_self_check(film, params, sens, dye, icmf)) {
            static const float kIdentity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                               0.0f, 0.0f, 0.0f, 1.0f};
            std::vector<float> gpu_raw(static_cast<size_t>(npix) * 3);
            if (spk::gpu::scan_spectral_linear(density_cmy, gpu_raw.data(),
                                               static_cast<uint32_t>(npix),
                                               dye.data(), icmf.data(),
                                               kIdentity)) {
                // The kernel stops at `raw`; the constant tail (midgray factor,
                // preflash 3-vector, log10 floor) is pointwise and stays on the
                // CPU, exactly as the direct path computes it.
                lut_lr.resize(static_cast<size_t>(npix) * 3);
                double* const lr = lut_lr.data();
                const double efm = params.exposure_factor_midgray;
                parallel_for(0, npix, [&](int lo, int hi) {
                    for (int p = lo; p < hi; ++p)
                        for (int k = 0; k < 3; ++k) {
                            const double raw =
                                static_cast<double>(
                                    gpu_raw[static_cast<size_t>(p) * 3 + k]) *
                                    efm +
                                preflash_raw[k];
                            lr[static_cast<size_t>(p) * 3 + k] =
                                std::log10(std::fmax(raw, 0.0) + 1e-10);
                        }
                });
                gpu_lr_done = true;
                g_gpu_print_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (params.use_enlarger_lut && !gpu_lr_done) {
        // Per-channel domain bounds (printing.py::expose):
        //   data_min = -film_render.grain.density_min
        //   data_max =  np.nanmax(film.data.density_curves, axis=0)
        double xmin[3], xmax[3];
        const int N = film.n_density_pts;
        double cmax[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (int nrow = 0; nrow < N; ++nrow) {
            const float* dc =
                film.density_curves.data() + static_cast<size_t>(nrow) * 3;
            for (int c = 0; c < 3; ++c) {
                double v = static_cast<double>(dc[c]);
                if (!std::isnan(v) && v > cmax[c]) cmax[c] = v;  // np.nanmax
            }
        }
        for (int c = 0; c < 3; ++c) {
            xmin[c] = -params.grain_density_min[c];
            xmax[c] = cmax[c];
        }

        int steps = params.lut_resolution;
        if (steps < 2) steps = 2;
        if (steps > 192) steps = 192;

        EnlargerLutCtx ctx;
        ctx.channel_density = film.channel_density.data();
        ctx.base_density = film.base_density.data();
        ctx.sens = sens.data();
        ctx.filtered_illuminant = params.filtered_illuminant;
        ctx.S = S;
        ctx.exposure_factor_midgray = params.exposure_factor_midgray;
        ctx.preflash_raw[0] = preflash_raw[0];
        ctx.preflash_raw[1] = preflash_raw[1];
        ctx.preflash_raw[2] = preflash_raw[2];

        // PERF (kernels/lut3d_cache.h): with an engine cache attached, fetch the
        // memoized build instead of redoing steps^3 spectral integrals every call.
        // The key folds EVERY value cmy_to_print_log_raw_fn reads plus the grid
        // that samples it, as raw IEEE-754 bytes:
        //   - the FILM's channel_density + base_density spectra (the negative's dyes),
        //   - the PRINT paper's sensitivity `sens` (10^log_sensitivity, nan_to_num) —
        //     folding the derived array rather than the paper id captures the paper
        //     exactly, whichever profile produced it,
        //   - the enlarger's dichroic-filtered illuminant (all 81 bands; a superset
        //     of the S read), which carries the neutral CC + y/m filter shifts,
        //   - the midgray exposure factor, which carries exposure_compensation_ev
        //     and the normalize/compensation branch,
        //   - the constant preflash raw 3-vector, which carries preflash_exposure and
        //     its own filter shifts,
        //   - the resolved domain bounds xmin/xmax (nanmax of the film density_curves
        //     and params.grain_density_min) and the clamped step count.
        // Every one of those is a live user param, so none may be omitted. Anything
        // added to EnlargerLutCtx later MUST be added here as well.
        std::shared_ptr<const PreparedLut3D> prepared;
        if (params.lut_cache) {
            std::string key;
            lut_key_append_tag(&key, "enl3d");
            lut_key_append(&key, S);
            lut_key_append(&key, film.channel_density.data(),
                           film.channel_density.size());
            lut_key_append(&key, film.base_density.data(),
                           film.base_density.size());
            lut_key_append(&key, sens.data(), sens.size());
            lut_key_append(&key, params.filtered_illuminant, 81);
            lut_key_append(&key, params.exposure_factor_midgray);
            lut_key_append(&key, preflash_raw, 3);
            lut_key_append(&key, xmin, 3);
            lut_key_append(&key, xmax, 3);
            lut_key_append(&key, steps);
            prepared = params.lut_cache->get_or_build(
                key, xmin, xmax, steps, &cmy_to_print_log_raw_fn, &ctx);
        } else {
            prepared = prepare_lut_3d_pchip(build_lut_3d(
                xmin, xmax, steps, {}, &cmy_to_print_log_raw_fn, &ctx));
        }

        lut_lr.resize(static_cast<size_t>(npix) * 3);
        std::vector<double> dens_d(static_cast<size_t>(npix) * 3);
        for (size_t i = 0; i < dens_d.size(); ++i)
            dens_d[i] = static_cast<double>(density_cmy[i]);
        apply_prepared_lut_3d_pchip(*prepared, dens_d.data(), width, height,
                                    lut_lr.data());
    }

    // True when lut_lr carries a precomputed log_raw plane, from EITHER the
    // opt-in enlarger LUT or the GPU offload above.
    const bool lr_precomputed = params.use_enlarger_lut || gpu_lr_done;

    // The Python reference runs the whole spectral chain in float64 and stores
    // float32 only at the final write. Mirror that exactly.
    parallel_for(0, npix, [&](int lo, int hi) {
    for (int p = lo; p < hi; ++p) {
        // log_raw_print = _film_cmy_to_print_log_raw(cmy). Either interpolated
        // from the opt-in enlarger LUT or evaluated directly (the default,
        // byte-exact path — its exp10_vec SIMD is left untouched).
        double lr0, lr1, lr2;
        if (lr_precomputed) {
            const double* lr = lut_lr.data() + static_cast<size_t>(p) * 3;
            lr0 = lr[0];
            lr1 = lr[1];
            lr2 = lr[2];
        } else {
        const float* dcmy = density_cmy + static_cast<size_t>(p) * 3;
        const double c0 = static_cast<double>(dcmy[0]);
        const double c1 = static_cast<double>(dcmy[1]);
        const double c2 = static_cast<double>(dcmy[2]);

        // raw[k] = sum_l light[l] * sens[l,k], where
        //   spectral[l] = c.channel_density[l] + base_density[l]   (film dyes)
        //   light[l]    = 10^-spectral[l] * filtered_illuminant[l] (NaN -> 0)
        // SIMD: 10^(-spectral) (the dominant per-band cost) is evaluated kExp10Lanes
        // at a time with the vector exp10 (kernels/exp10; <=4 ULP, byte-identical
        // after the float32 cast). The raw accumulation stays in band order.
        double raw0 = 0.0, raw1 = 0.0, raw2 = 0.0;
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
                double light = ev[q] * params.filtered_illuminant[l + q];
                if (std::isnan(light)) light = 0.0;
                const double* sl = sens.data() + static_cast<size_t>(l + q) * 3;
                raw0 += light * sl[0];
                raw1 += light * sl[1];
                raw2 += light * sl[2];
            }
        }
        for (; l < S; ++l) {  // odd-band tail (same exp10 arithmetic, scalar).
            const float* cd =
                film.channel_density.data() + static_cast<size_t>(l) * 3;
            const double spectral = c0 * static_cast<double>(cd[0]) +
                                    c1 * static_cast<double>(cd[1]) +
                                    c2 * static_cast<double>(cd[2]) +
                                    static_cast<double>(film.base_density[l]);
            double light = exp10_scalar(-spectral) * params.filtered_illuminant[l];
            if (std::isnan(light)) light = 0.0;
            const double* sl = sens.data() + static_cast<size_t>(l) * 3;
            raw0 += light * sl[0];
            raw1 += light * sl[1];
            raw2 += light * sl[2];
        }

        // raw *= exposure_factor_midgray (midgray normalisation), then
        // raw += _compute_raw_preflash (the constant preflash 3-vector; {0,0,0}
        // when preflash is off, so this is a strict no-op on the default path).
        raw0 *= params.exposure_factor_midgray;
        raw1 *= params.exposure_factor_midgray;
        raw2 *= params.exposure_factor_midgray;
        raw0 += preflash_raw[0];
        raw1 += preflash_raw[1];
        raw2 += preflash_raw[2];

        // _film_cmy_to_print_log_raw returns log10(max(raw,0) + 1e-10).
        lr0 = std::log10(std::fmax(raw0, 0.0) + 1e-10);
        lr1 = std::log10(std::fmax(raw1, 0.0) + 1e-10);
        lr2 = std::log10(std::fmax(raw2, 0.0) + 1e-10);
        }  // end direct (non-LUT) path

        // expose(): raw = 10^log_raw; raw *= print_exposure * bw_correction;
        // then the optical diffusion filter (if active) runs on `raw`; finally
        // return log10(max(raw,0) + 1e-10). The 10^/log10 round trip is
        // reproduced verbatim so float rounding matches the reference.
        const double mult = params.print_exposure * params.bw_exposure_correction;
        double r0 = std::pow(10.0, lr0) * mult;
        double r1 = std::pow(10.0, lr1) * mult;
        double r2 = std::pow(10.0, lr2) * mult;

        if (diffusion) {
            double* rb = raw_buf.data() + static_cast<size_t>(p) * 3;
            rb[0] = r0; rb[1] = r1; rb[2] = r2;
        } else {
            float* out = log_raw_print_out + static_cast<size_t>(p) * 3;
            out[0] = static_cast<float>(std::log10(std::fmax(r0, 0.0) + 1e-10));
            out[1] = static_cast<float>(std::log10(std::fmax(r1, 0.0) + 1e-10));
            out[2] = static_cast<float>(std::log10(std::fmax(r2, 0.0) + 1e-10));
        }
    }
    });

    if (diffusion) {
        apply_diffusion_filter_um(raw_buf.data(), width, height,
                                  params.diffusion_filter, params.pixel_size_um);
        const size_t total = static_cast<size_t>(npix) * 3;
        for (size_t i = 0; i < total; ++i) {
            log_raw_print_out[i] = static_cast<float>(
                std::log10(std::fmax(raw_buf[i], 0.0) + 1e-10));
        }
    }
}

void print_reference_log_raw(const Profile& film, const Profile& print_profile,
                             const PrintingParams& params, const double cmy_film[3],
                             double log_raw_out[3]) {
    const int S = film.n_samples;
    // print sensitivity = nan_to_num(10**log_sensitivity).
    std::vector<double> sens(static_cast<size_t>(S) * 3);
    for (int l = 0; l < S; ++l) {
        for (int k = 0; k < 3; ++k) {
            double v = std::pow(10.0, static_cast<double>(
                          print_profile.log_sensitivity[static_cast<size_t>(l) * 3 + k]));
            if (std::isnan(v)) v = 0.0;
            sens[static_cast<size_t>(l) * 3 + k] = v;
        }
    }
    // Constant preflash 3-vector (= 0 when preflash off), matching print_expose.
    double preflash_raw[3] = {0.0, 0.0, 0.0};
    if (params.preflash_exposure > 0.0) {
        for (int l = 0; l < S; ++l) {
            double light = std::pow(10.0, -static_cast<double>(film.base_density[l])) *
                           params.preflash_illuminant[l];
            if (std::isnan(light)) light = 0.0;
            const double* sl = sens.data() + static_cast<size_t>(l) * 3;
            preflash_raw[0] += light * sl[0];
            preflash_raw[1] += light * sl[1];
            preflash_raw[2] += light * sl[2];
        }
        preflash_raw[0] *= params.preflash_exposure;
        preflash_raw[1] *= params.preflash_exposure;
        preflash_raw[2] *= params.preflash_exposure;
    }
    // raw[k] = sum_l 10^-(film spectral density) * filtered_illuminant[l] * sens[l,k].
    double raw0 = 0.0, raw1 = 0.0, raw2 = 0.0;
    for (int l = 0; l < S; ++l) {
        const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
        const double spectral = cmy_film[0] * static_cast<double>(cd[0]) +
                                cmy_film[1] * static_cast<double>(cd[1]) +
                                cmy_film[2] * static_cast<double>(cd[2]) +
                                static_cast<double>(film.base_density[l]);
        double light = std::pow(10.0, -spectral) * params.filtered_illuminant[l];
        if (std::isnan(light)) light = 0.0;
        const double* sl = sens.data() + static_cast<size_t>(l) * 3;
        raw0 += light * sl[0];
        raw1 += light * sl[1];
        raw2 += light * sl[2];
    }
    raw0 = raw0 * params.exposure_factor_midgray + preflash_raw[0];
    raw1 = raw1 * params.exposure_factor_midgray + preflash_raw[1];
    raw2 = raw2 * params.exposure_factor_midgray + preflash_raw[2];
    log_raw_out[0] = std::log10(std::fmax(raw0, 0.0) + 1e-10);
    log_raw_out[1] = std::log10(std::fmax(raw1, 0.0) + 1e-10);
    log_raw_out[2] = std::log10(std::fmax(raw2, 0.0) + 1e-10);
}

void print_develop(const Profile& print_profile, const PrintingParams& params,
                   const float* log_raw_print, int npix,
                   float* density_cmy_out) {
    // OPT-IN s023 print-curve morph (develop_print_morph): rebuild the print
    // density table from the paper's parametric density_curves_model, morph it by
    // coupled gamma + developer exhaustion, and interpolate THAT table (the morph
    // folds gamma in, so interp uses gamma 1.0). Default-off, or a profile with no
    // density_curves_model, falls through to the stored-table path below, which is
    // byte-identical to the c1d0e44 print goldens.
    if (params.morph.active && print_profile.dc_model_n_layers > 0) {
        const int n = print_profile.n_density_pts;
        std::vector<float> morphed(static_cast<size_t>(n) * 3);
        apply_print_curves_morph(print_profile.dc_model_centers.data(),
                                 print_profile.dc_model_amplitudes.data(),
                                 print_profile.dc_model_sigmas.data(),
                                 print_profile.dc_model_n_layers, params.morph,
                                 print_profile.is_positive(),
                                 print_profile.log_exposure.data(), n,
                                 morphed.data());
        interpolate_exposure_to_density(log_raw_print, npix, morphed.data(),
                                        print_profile.log_exposure.data(), n,
                                        /*gamma_factor=*/1.0f, density_cmy_out);
        return;
    }
    // develop_simple: interpolate against the RAW print density curves (no nanmin
    // normalisation, no DIR couplers), gamma broadcast to all channels.
    interpolate_exposure_to_density(log_raw_print, npix,
                                    print_profile.density_curves.data(),
                                    print_profile.log_exposure.data(),
                                    print_profile.n_density_pts,
                                    params.density_curve_gamma, density_cmy_out);
}

int gpu_print_expose_state() {
    return g_gpu_print_state.load(std::memory_order_acquire);
}

unsigned long long gpu_print_frames_rendered() {
    return static_cast<unsigned long long>(
        g_gpu_print_frames.load(std::memory_order_relaxed));
}

}  // namespace spk
