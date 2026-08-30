/*
 * Spektrafilm for Android — native engine: DIR couplers.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements model/couplers.h (no-diffusion DIR-coupler path).
 */
#include "model/couplers.h"

#include <cmath>
#include <vector>

#include "kernels/exponential_filter.h"
#include "kernels/parallel.h"
#include "kernels/uniform_axis.h"

namespace spk {

namespace {

// Faithful port of numpy.interp (numpy _core/src/multiarray/compiled_base.c,
// binary_search_with_guess + arr_interp), used because the DIR-coupler axis
// le0 = le - silver_curve @ M can be NON-MONOTONIC for a steep stock / large
// coupler amount. A plain ascending binary search (std::upper_bound) picks a
// different bracket than numpy on a non-monotonic xp, so it diverged from the
// oracle (max_abs ~0.44 at amount>=2.0). numpy's search is ORDER-DEPENDENT (it
// carries a `guess` across the query array), so the interpolation must be done
// in a single batched pass over the query points in order — a per-element scan
// does NOT reproduce np.interp on a non-monotonic xp. For a monotonic xp (the
// default coupler path) this yields the same bracket as before, within parity
// tolerance of the existing goldens.
constexpr int kLikelyInCache = 8;  // numpy LIKELY_IN_CACHE_SIZE

long long binary_search_with_guess(double key, const double* arr, int len,
                                   long long guess) {
    long long imin = 0, imax = len;
    if (key > arr[len - 1]) return len;
    if (key < arr[0]) return -1;
    if (len <= 4) {  // numpy: linear search for short arrays
        int i = 1;
        for (; i < len && key >= arr[i]; ++i) {}
        return i - 1;
    }
    if (guess > len - 3) guess = len - 3;
    if (guess < 1) guess = 1;
    // Check the most likely values: guess-1, guess, guess+1, then restrict.
    if (key < arr[guess]) {
        if (key < arr[guess - 1]) {
            imax = guess - 1;
            if (guess > kLikelyInCache && key >= arr[guess - kLikelyInCache])
                imin = guess - kLikelyInCache;
        } else {
            return guess - 1;
        }
    } else {
        if (key < arr[guess + 1]) {
            return guess;
        } else if (key < arr[guess + 2]) {
            return guess + 1;
        } else {
            imin = guess + 2;
            if (guess < len - kLikelyInCache - 1 && key < arr[guess + kLikelyInCache])
                imax = guess + kLikelyInCache;
        }
    }
    while (imin < imax) {
        const long long imid = imin + ((imax - imin) >> 1);
        if (key >= arr[imid]) imin = imid + 1;
        else imax = imid;
    }
    return imin - 1;
}

// fast_interp-style interpolation used by interpolate_exposure_to_density:
// per-channel axis xa (n,), clamp to endpoint density values. Matches the
// searchsorted(side='right') indexing in utils/fast_interp.py.
//
// `ua` is the axis's precomputed O(1) lookup hint (EXPORT_FASTPATH item 1),
// built ONCE per apply_* call by detect_uniform_axis, far from the per-pixel
// loop. When the axis qualifies (le/gamma is uniform-to-rounding for every
// bundled profile) the bracket comes from the O(1) estimate + fix-up walk,
// which returns the IDENTICAL `low` this binary search picks (see
// kernels/uniform_axis.h) — so the interpolation arithmetic below is
// unchanged and the result is bit-identical. A non-qualifying axis (e.g. a
// descending one from a negative gamma) keeps the exact search.
//
// NaN guard: np.interp propagates NaN (np_interp_array below checks
// explicitly), but this search takes neither clamp branch on NaN and every
// probe comparison is false, ending at low == -1 — an out-of-bounds xa[-1]
// read. Return NaN up front instead (the previous behavior was undefined, not
// covered by any golden).
double fast_interp_channel(double x, const double* xa, const double* y, int n,
                           const UniformAxis<double>& ua) {
    if (std::isnan(x)) return x;
    if (x <= xa[0]) return y[0];
    if (x >= xa[n - 1]) return y[n - 1];
    int low;
    if (ua.ok) {
        low = uniform_axis_bracket(x, xa, n, 1, ua);
    } else {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (xa[mid] <= x)
                lo = mid + 1;
            else
                hi = mid;
        }
        low = lo - 1;
    }
    double x0 = xa[low];
    double dx = xa[low + 1] - x0;
    double t = dx != 0.0 ? (x - x0) / dx : 0.0;
    return y[low] + t * (y[low + 1] - y[low]);
}

}  // namespace

// np.interp(x[:nx], xp[:n], fp[:n]) -> out[:nx]. Endpoints clamp to fp[0]/fp[n-1].
// Query points are consumed in order so the carried `guess` (in
// binary_search_with_guess) matches numpy's batched np.interp; computing the
// slope inline is bit-identical to numpy's precomputed-slopes path (same
// operands, same order). Exposed (couplers.h) for direct host parity testing.
void np_interp_array(const double* x, int nx, const double* xp, const double* fp,
                     int n, double* out) {
    const double lval = fp[0], rval = fp[n - 1];
    long long j = 0;
    for (int i = 0; i < nx; ++i) {
        const double xv = x[i];
        if (std::isnan(xv)) { out[i] = xv; continue; }
        j = binary_search_with_guess(xv, xp, n, j);
        if (j == -1) {
            out[i] = lval;
        } else if (j == n) {
            out[i] = rval;
        } else if (j == n - 1) {
            out[i] = fp[j];
        } else if (xp[j] == xv) {
            out[i] = fp[j];  // avoid a non-finite slope at a coincident node
        } else {
            const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
            double res = slope * (xv - xp[j]) + fp[j];
            if (std::isnan(res)) {  // numpy: if nan one way, try the other
                res = slope * (xv - xp[j + 1]) + fp[j + 1];
                if (std::isnan(res) && fp[j] == fp[j + 1]) res = fp[j];
            }
            out[i] = res;
        }
    }
}

void compute_dir_couplers_matrix(const DirCouplersParams& params, double out[9]) {
    // M_self = diag(gamma_samelayer_rgb) * inhibition_samelayer
    // M_inter off-diagonal (donor row, receiver col) * inhibition_interlayer
    for (int i = 0; i < 9; ++i) out[i] = 0.0;
    out[0 * 3 + 0] = params.gamma_samelayer_rgb[0] * params.inhibition_samelayer;
    out[1 * 3 + 1] = params.gamma_samelayer_rgb[1] * params.inhibition_samelayer;
    out[2 * 3 + 2] = params.gamma_samelayer_rgb[2] * params.inhibition_samelayer;

    out[0 * 3 + 1] += params.gamma_interlayer_r_to_gb[0] * params.inhibition_interlayer;
    out[0 * 3 + 2] += params.gamma_interlayer_r_to_gb[1] * params.inhibition_interlayer;
    out[1 * 3 + 0] += params.gamma_interlayer_g_to_rb[0] * params.inhibition_interlayer;
    out[1 * 3 + 2] += params.gamma_interlayer_g_to_rb[1] * params.inhibition_interlayer;
    out[2 * 3 + 0] += params.gamma_interlayer_b_to_rg[0] * params.inhibition_interlayer;
    out[2 * 3 + 1] += params.gamma_interlayer_b_to_rg[1] * params.inhibition_interlayer;

    for (int i = 0; i < 9; ++i) out[i] *= params.amount;
}

void apply_density_correction_dir_couplers(const float* density_cmy, int npix,
                                           const float* log_raw,
                                           const float* log_exposure,
                                           const float* density_curves, int n,
                                           const DirCouplersParams& params,
                                           bool positive_film,
                                           const float gamma_factor[3],
                                           float* out) {
    if (!params.active) {
        if (out != density_cmy)
            for (int i = 0; i < npix * 3; ++i) out[i] = density_cmy[i];
        return;
    }

    double M[9];
    compute_dir_couplers_matrix(params, M);

    // log_exposure axis in double.
    std::vector<double> le(n);
    for (int k = 0; k < n; ++k) le[k] = static_cast<double>(log_exposure[k]);

    // ---- compute_density_curves_before_dir_couplers (negative path) ----
    // density_curves_silver = density_curves (negative)
    // For positive film: silver = nanmax(curves,axis=0) - curves (not exercised
    // by the parity goldens, but kept for correctness).
    std::vector<double> dc(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n * 3; ++i) dc[i] = static_cast<double>(density_curves[i]);

    std::vector<double> silver_curve(static_cast<size_t>(n) * 3);
    if (positive_film) {
        double dmax[3] = {-1e300, -1e300, -1e300};
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c) {
                double v = dc[k * 3 + c];
                if (!std::isnan(v) && v > dmax[c]) dmax[c] = v;
            }
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c)
                silver_curve[k * 3 + c] = dmax[c] - dc[k * 3 + c];
    } else {
        silver_curve = dc;
    }

    // couplers_amount_curves[j,m] = sum_k silver_curve[j,k] * M[k,m]
    // log_exposure_0[j,m] = le[j] - couplers_amount_curves[j,m]
    // dc0[:,i] = interp(le, log_exposure_0[:,i], (+/-)dc[:,i])
    std::vector<double> dc0(static_cast<size_t>(n) * 3);
    std::vector<double> le0(static_cast<size_t>(n));   // per-channel x axis buffer
    std::vector<double> ycol(static_cast<size_t>(n));  // per-channel y values
    std::vector<double> vbuf(static_cast<size_t>(n));  // np.interp output buffer
    for (int c = 0; c < 3; ++c) {
        for (int j = 0; j < n; ++j) {
            double amt = 0.0;
            for (int k = 0; k < 3; ++k) amt += silver_curve[j * 3 + k] * M[k * 3 + c];
            le0[j] = le[j] - amt;
            // negative: y = dc[:,c]; positive: interp on -dc, then negate.
            ycol[j] = positive_film ? -dc[j * 3 + c] : dc[j * 3 + c];
        }
        np_interp_array(le.data(), n, le0.data(), ycol.data(), n, vbuf.data());
        for (int j = 0; j < n; ++j)
            dc0[j * 3 + c] = positive_film ? -vbuf[j] : vbuf[j];
    }

    // ---- compute_exposure_correction_dir_couplers (diffusion off) ----
    //   density_silver = density_cmy (negative) or density_max - density_cmy
    //                    (positive); += high_exposure_shift * silver^2
    //   log_raw_0 = log_raw - density_silver @ M     (diffusion_size_pixel == 0)
    // For positive film density_max is the per-channel nanmax of density_curves.
    double dmax_cmy[3] = {0.0, 0.0, 0.0};
    if (positive_film) {
        dmax_cmy[0] = dmax_cmy[1] = dmax_cmy[2] = -1e300;
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c) {
                double v = dc[k * 3 + c];
                if (!std::isnan(v) && v > dmax_cmy[c]) dmax_cmy[c] = v;
            }
    }
    const double shift = params.high_exposure_couplers_shift;

    // ---- interpolate_exposure_to_density(log_raw_0, dc0, le, gamma) ----
    // Per-channel x-axis is le / gamma_factor[c]; y is dc0[:,c].
    //
    // NOTE: `out` may alias `density_cmy`, and the inhibitor matmul mixes all
    // three (donor) channels of density_cmy into each (receiver) channel. We
    // therefore snapshot the three silver donor values per pixel BEFORE writing
    // any output channel, matching the vectorised Python (which computes the
    // full correction array first, then interpolates).
    // Per-channel contiguous axis (le/gamma) and curve (dc0[:,c]) arrays.
    std::vector<double> axis_c[3], curve_c[3];
    for (int c = 0; c < 3; ++c) {
        double g = static_cast<double>(gamma_factor[c]);
        axis_c[c].resize(n);
        curve_c[c].resize(n);
        for (int k = 0; k < n; ++k) {
            axis_c[c][k] = le[k] / g;
            curve_c[c][k] = dc0[k * 3 + c];
        }
    }
    // O(1) bracket hints for the three per-channel axes (EXPORT_FASTPATH
    // item 1); a non-qualifying axis keeps the exact binary search.
    UniformAxis<double> ua_c[3];
    for (int c = 0; c < 3; ++c)
        ua_c[c] = detect_uniform_axis(axis_c[c].data(), n, 1);
    // Deterministic parallel chunks: each pixel reads only its own density_cmy /
    // log_raw entries and writes only its own out entries (the per-pixel silver
    // snapshot already handles out aliasing density_cmy WITHIN a pixel), so the
    // result is byte-identical to the serial loop for any thread count.
    parallel_for(0, npix, [&](int lo, int hi) {
        for (int p = lo; p < hi; ++p) {
            double silver[3];
            for (int k = 0; k < 3; ++k) {
                double s = static_cast<double>(density_cmy[p * 3 + k]);
                if (positive_film) s = dmax_cmy[k] - s;
                if (shift != 0.0) s += shift * s * s;
                silver[k] = s;
            }
            for (int c = 0; c < 3; ++c) {
                double correction = 0.0;
                for (int k = 0; k < 3; ++k) correction += silver[k] * M[k * 3 + c];
                double log_raw_0 = static_cast<double>(log_raw[p * 3 + c]) - correction;
                out[p * 3 + c] = static_cast<float>(fast_interp_channel(
                    log_raw_0, axis_c[c].data(), curve_c[c].data(), n, ua_c[c]));
            }
        }
    });
}

void apply_density_correction_dir_couplers_spatial(
    const float* density_cmy, int w, int h, const float* log_raw,
    const float* log_exposure, const float* density_curves, int n,
    const DirCouplersParams& params, bool positive_film,
    const float gamma_factor[3], double pixel_size_um, float* out) {
    const int npix = w * h;

    // No spatial diffusion requested -> delegate to the pointwise path.
    if (!params.active || params.diffusion_size_um <= 0.0 || pixel_size_um <= 0.0) {
        apply_density_correction_dir_couplers(density_cmy, npix, log_raw,
                                              log_exposure, density_curves, n,
                                              params, positive_film, gamma_factor,
                                              out);
        return;
    }

    double M[9];
    compute_dir_couplers_matrix(params, M);

    std::vector<double> le(n);
    for (int k = 0; k < n; ++k) le[k] = static_cast<double>(log_exposure[k]);

    // ---- compute_density_curves_before_dir_couplers (same as pointwise) ----
    std::vector<double> dc(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n * 3; ++i) dc[i] = static_cast<double>(density_curves[i]);
    std::vector<double> silver_curve(static_cast<size_t>(n) * 3);
    if (positive_film) {
        double dmax[3] = {-1e300, -1e300, -1e300};
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c) {
                double v = dc[k * 3 + c];
                if (!std::isnan(v) && v > dmax[c]) dmax[c] = v;
            }
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c)
                silver_curve[k * 3 + c] = dmax[c] - dc[k * 3 + c];
    } else {
        silver_curve = dc;
    }
    std::vector<double> dc0(static_cast<size_t>(n) * 3);
    {
        std::vector<double> le0(n), ycol(n), vbuf(n);
        for (int c = 0; c < 3; ++c) {
            for (int j = 0; j < n; ++j) {
                double amt = 0.0;
                for (int k = 0; k < 3; ++k) amt += silver_curve[j * 3 + k] * M[k * 3 + c];
                le0[j] = le[j] - amt;
                ycol[j] = positive_film ? -dc[j * 3 + c] : dc[j * 3 + c];
            }
            np_interp_array(le.data(), n, le0.data(), ycol.data(), n, vbuf.data());
            for (int j = 0; j < n; ++j)
                dc0[j * 3 + c] = positive_film ? -vbuf[j] : vbuf[j];
        }
    }

    // density_max for the positive-film silver computation.
    double dmax_cmy[3] = {0.0, 0.0, 0.0};
    if (positive_film) {
        dmax_cmy[0] = dmax_cmy[1] = dmax_cmy[2] = -1e300;
        for (int k = 0; k < n; ++k)
            for (int c = 0; c < 3; ++c) {
                double v = dc[k * 3 + c];
                if (!std::isnan(v) && v > dmax_cmy[c]) dmax_cmy[c] = v;
            }
    }
    const double shift = params.high_exposure_couplers_shift;

    // ---- compute_exposure_correction_dir_couplers (diffusion ON) ----
    // correction[p,c] = sum_k silver[p,k] * M[k,c]   (full array, float64)
    // Per-pixel independent -> deterministic parallel chunks (byte-identical).
    std::vector<double> correction(static_cast<size_t>(npix) * 3);
    parallel_for(0, npix, [&](int lo, int hi) {
        for (int p = lo; p < hi; ++p) {
            double silver[3];
            for (int k = 0; k < 3; ++k) {
                double s = static_cast<double>(density_cmy[p * 3 + k]);
                if (positive_film) s = dmax_cmy[k] - s;
                if (shift != 0.0) s += shift * s * s;
                silver[k] = s;
            }
            for (int c = 0; c < 3; ++c) {
                double corr = 0.0;
                for (int k = 0; k < 3; ++k) corr += silver[k] * M[k * 3 + c];
                correction[p * 3 + c] = corr;
            }
        }
    });

    // Diffuse the correction:
    //   correction = (1 - tail_w) * G(size_px) * correction
    //              + tail_w       * Exp(tail_px) * correction
    const double size_px = params.diffusion_size_um / pixel_size_um;
    const double tail_px = params.diffusion_tail_um / pixel_size_um;
    const double tail_w = params.diffusion_tail_weight;
    {
        // ORDER MATTERS FOR MEMORY, NOT FOR MATH. The two filters are
        // independent: the exponential tail READS `correction` and WRITES
        // `tail`, while the Gaussian blurs `correction` in place. Running the
        // tail FIRST therefore leaves `correction` intact for the Gaussian, and
        // the full-resolution `gauss` copy — which existed only to preserve
        // `correction` across a blur that came before its other reader — is not
        // needed at all.
        //
        // Both filters are pure functions of their inputs, so the swap changes
        // no value: the blend below consumes exactly the same two arrays it did
        // before. It is a memory change, and the parity suite is the check.
        //
        // Measured on the host at 12.5 MP (4096x3052) at the release flags, two
        // ways that agree. A phase breakdown of the old block attributed 1772 ms
        // of an 8448 ms stage (21%) to this copy's allocation + memcpy — the
        // per-pixel loops around it were 0.4%, so the stage was mostly memory,
        // not arithmetic. A same-process A/B of the two orders then reported
        // 21.9% with the new arm running COLD and the old one warm, 27-50% when
        // the page-fault ordering ran the other way. Take ~22% as the number:
        // it is the conservative arm and it matches the independent phase
        // estimate. The absolute times drift by ~30% across reps as the
        // allocator warms, which is why the reps are alternated rather than
        // averaged.
        //
        // The memory half is unconditional and does not depend on any of that:
        // three full-resolution f64 planes become two, 900 MB -> 600 MB at
        // 12.5 MP, which matters more on a phone than the milliseconds do.
        //
        // Byte-identity is checked directly, not inferred: an A/B running both
        // orders in one process memcmp'd the f64 results over ten filter
        // configurations (FIR-only, IIR-only, the SMALL_SIGMA_MAX=3 dispatch
        // boundary, tail-weight 0 and 1) across two image shapes including a
        // non-power-of-two, plus the 12.5 MP frame. All identical.
        std::vector<double> tail(static_cast<size_t>(npix) * 3);
        double lt[3] = {tail_px, tail_px, tail_px};
        exponential_filter_per_channel_d(correction.data(), w, h, 3, lt, tail.data());
        double sg[3] = {size_px, size_px, size_px};
        gaussian_blur_per_channel_d(correction.data(), w, h, 3, sg);
        // Element-wise blend -> deterministic parallel chunks (byte-identical).
        // `correction` now carries the Gaussian arm, blurred in place.
        parallel_for(0, npix * 3, [&](int lo, int hi) {
            for (int i = lo; i < hi; ++i)
                correction[i] = (1.0 - tail_w) * correction[i] + tail_w * tail[i];
        });
    }

    // ---- interpolate_exposure_to_density(log_raw - correction, dc0, le, gamma) ----
    std::vector<double> axis_c[3], curve_c[3];
    for (int c = 0; c < 3; ++c) {
        double g = static_cast<double>(gamma_factor[c]);
        axis_c[c].resize(n);
        curve_c[c].resize(n);
        for (int k = 0; k < n; ++k) {
            axis_c[c][k] = le[k] / g;
            curve_c[c][k] = dc0[k * 3 + c];
        }
    }
    // O(1) bracket hints (EXPORT_FASTPATH item 1); fallback = exact search.
    UniformAxis<double> ua_c[3];
    for (int c = 0; c < 3; ++c)
        ua_c[c] = detect_uniform_axis(axis_c[c].data(), n, 1);
    // Per-pixel independent interp -> deterministic parallel chunks.
    parallel_for(0, npix, [&](int lo, int hi) {
        for (int p = lo; p < hi; ++p) {
            for (int c = 0; c < 3; ++c) {
                double log_raw_0 =
                    static_cast<double>(log_raw[p * 3 + c]) - correction[p * 3 + c];
                out[p * 3 + c] = static_cast<float>(fast_interp_channel(
                    log_raw_0, axis_c[c].data(), curve_c[c].data(), n, ua_c[c]));
            }
        }
    });
}

}  // namespace spk
