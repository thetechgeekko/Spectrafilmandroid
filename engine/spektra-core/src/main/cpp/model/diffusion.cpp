/*
 * Spektrafilm for Android — native engine: in-emulsion scatter + halation.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements model/diffusion.h (apply_highlight_boost,
 * apply_halation_um, apply_diffusion_filter_um).
 */
#include "model/diffusion.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>   // debug.spektra.fftmax / .fft (tuning_knob)
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "kernels/exponential_filter.h"
#include "kernels/fft_convolve.h"
#include "kernels/parallel.h"
#include "runtime/memory_budget.h"
#include "runtime/stage_timer.h"

// M_PI is not in standard C++ <cmath>; some toolchains gate it behind
// _USE_MATH_DEFINES / _GNU_SOURCE. Provide the IEEE-754 double value (identical
// to numpy.pi) so the PSF normalisation is bit-exact on every compiler.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// NaN / Inf behavior — VERIFIED against the oracle (spektrafilm/model/diffusion.py).
// The oracle's apply_halation_um and apply_diffusion_filter_um do NO NaN/Inf
// sanitization: there is no np.nan_to_num / np.isnan / np.isfinite anywhere on
// this path. They use only np.maximum(..., 1e-6) on the FILTER PARAMETERS
// (sigma/lambda lower bounds) and np.clip on the strength/warmth SCALARS — never
// on the image data — so any NaN/Inf in the input irradiance PROPAGATES through
// the Gaussian/exponential filters and the direct convolution, exactly like
// numpy. This port matches that behavior DELIBERATELY: it is NOT NaN-aware on the
// image data, by design, to stay bit-exact with the oracle. The existing isnan
// guards elsewhere in the engine (filming/printing sensitivity, midgray, scan(),
// conversions) each mirror a SPECIFIC oracle np.nan_to_num/np.isnan call; the
// diffusion path has none, so adding one here would DIVERGE from the oracle and
// is intentionally omitted. Upstream stages already feed finite irradiance here
// (raw = fmax(raw, 0) + 1e-10 happens just after this in filming.expose).
namespace spk {

namespace {
inline double max_eps(double v) { return v > 1e-6 ? v : 1e-6; }
}  // namespace

void apply_halation_um(double* raw, int w, int h, const HalationParams& params,
                       double pixel_size_um) {
    if (!params.active) return;
    if (w <= 0 || h <= 0 || pixel_size_um <= 0.0) return;

    const size_t total = static_cast<size_t>(w) * h * 3;

    // Step 1 (highlight boost) is applied earlier in FilmingStage.expose via the
    // separate apply_highlight_boost pass (matching filming.py, which calls
    // boost_highlights before apply_halation_um). Not repeated here.

    // Step 2: in-emulsion scatter.
    //   scattered = (1 - w_s) * G(sigma_c) * raw  +  w_s * Exp(lambda_t) * raw
    //   raw       = (1 - s) * raw  +  s * scattered
    const double s_amount = params.scatter_amount;
    const double s_scale = params.scatter_spatial_scale;
    double sigma_c_px[3], lambda_t_px[3], w_s[3];
    bool any_scatter_sigma = false;
    for (int c = 0; c < 3; ++c) {
        sigma_c_px[c] = params.scatter_core_um[c] * s_scale / pixel_size_um;
        lambda_t_px[c] = params.scatter_tail_um[c] * s_scale / pixel_size_um;
        w_s[c] = params.scatter_tail_weight[c];
        if (sigma_c_px[c] > 0.0 || lambda_t_px[c] > 0.0) any_scatter_sigma = true;
    }
    if (s_amount > 0.0 && any_scatter_sigma) {
        // core = fast_gaussian_filter(raw, max(sigma_c, 1e-6))
        std::vector<double> core(raw, raw + total);
        double sc[3] = {max_eps(sigma_c_px[0]), max_eps(sigma_c_px[1]),
                        max_eps(sigma_c_px[2])};
        gaussian_blur_per_channel_d(core.data(), w, h, 3, sc);
        // tail = fast_exponential_filter(raw, max(lambda_t, 1e-6))
        std::vector<double> tail(total);
        double lt[3] = {max_eps(lambda_t_px[0]), max_eps(lambda_t_px[1]),
                        max_eps(lambda_t_px[2])};
        exponential_filter_per_channel_d(raw, w, h, 3, lt, tail.data());
        // scattered = (1-w_s)*core + w_s*tail ; raw = (1-s)*raw + s*scattered
        // Per-pixel map, disjoint writes -> deterministic parallel chunks.
        const int plane = w * h;
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p) {
                for (int c = 0; c < 3; ++c) {
                    size_t idx = static_cast<size_t>(p) * 3 + c;
                    double scattered =
                        (1.0 - w_s[c]) * core[idx] + w_s[c] * tail[idx];
                    raw[idx] = (1.0 - s_amount) * raw[idx] + s_amount * scattered;
                }
            }
        });
    }

    // Step 3: back-reflection halation.
    //   a_tot = halation_strength * halation_amount
    //   decay_k = rho^(k-1) / sum_j rho^(j-1)   (k = 1..N)
    //   halation_blur = sum_k decay_k * G(sigma_h * sqrt(k)) * raw
    //   raw = raw + a_tot * halation_blur ; if renormalize: raw /= (1 + a_tot)
    const double h_amount = params.halation_amount;
    const double h_scale = params.halation_spatial_scale;
    double a_tot[3], sigma_h_px[3];
    bool any_a = false, any_sigma_h = false;
    for (int c = 0; c < 3; ++c) {
        a_tot[c] = params.halation_strength[c] * h_amount;
        sigma_h_px[c] = params.halation_first_sigma_um[c] * h_scale / pixel_size_um;
        if (a_tot[c] > 0.0) any_a = true;
        if (sigma_h_px[c] > 0.0) any_sigma_h = true;
    }
    const int N = params.halation_n_bounces;
    const double rho = params.halation_bounce_decay;
    if (N >= 1 && any_a && any_sigma_h) {
        std::vector<double> decay(N);
        double dsum = 0.0;
        for (int k = 1; k <= N; ++k) {
            decay[k - 1] = std::pow(rho, static_cast<double>(k - 1));
            dsum += decay[k - 1];
        }
        for (int k = 0; k < N; ++k) decay[k] /= dsum;

        std::vector<double> halation_blur(total, 0.0);
        std::vector<double> comp(total);
        const int ntotal = static_cast<int>(total);
        for (int k = 1; k <= N; ++k) {
            double sk[3];
            for (int c = 0; c < 3; ++c)
                sk[c] = max_eps(sigma_h_px[c] * std::sqrt(static_cast<double>(k)));
            // Copy / axpy: per-element maps -> deterministic parallel chunks.
            parallel_for(0, ntotal, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) comp[i] = raw[i];
            });
            gaussian_blur_per_channel_d(comp.data(), w, h, 3, sk);
            double wk = decay[k - 1];
            parallel_for(0, ntotal, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) halation_blur[i] += wk * comp[i];
            });
        }
        const int plane = w * h;
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p) {
                for (int c = 0; c < 3; ++c) {
                    size_t idx = static_cast<size_t>(p) * 3 + c;
                    double v = raw[idx] + a_tot[c] * halation_blur[idx];
                    if (params.halation_renormalize) v = v / (1.0 + a_tot[c]);
                    raw[idx] = v;
                }
            }
        });
    }
}

// ---------------------------------------------------------------------------
// Optical diffusion filter (Black Pro-Mist family) — port of
// spektrafilm/model/diffusion.py::apply_diffusion_filter_um and the PSF helpers.
// ---------------------------------------------------------------------------
namespace {

// One {core|halo|bloom} group configuration, mirroring the per-family dicts in
// _DIFFUSION_FILTER_SHAPES.
struct GroupCfg {
    double lambda_um;
    double spread;
    int n_components;
    double alpha;  // only used for bloom (kind == bloom).
};

struct FamilyCfg {
    GroupCfg core;
    GroupCfg halo;
    GroupCfg bloom;
    double w_c, w_h, w_b;
    double halo_warmth_base;
    double total_gain;  // _DIFFUSION_FAMILY_TOTAL_GAIN[family].
};

// _DIFFUSION_FILTER_SHAPES + _DIFFUSION_FAMILY_TOTAL_GAIN, transcribed verbatim.
FamilyCfg family_cfg(DiffusionFamily fam) {
    switch (fam) {
        case DiffusionFamily::kGlimmerglass:
            return {{10.0, 1.5, 2, 0.0}, {50.0, 2.0, 3, 0.0},
                    {260.0, 2.5, 4, 3.2}, 0.60, 0.30, 0.10, 0.0, 0.65};
        case DiffusionFamily::kBlackProMist:
            return {{16.0, 1.5, 2, 0.0}, {95.0, 2.0, 3, 0.0},
                    {380.0, 2.5, 4, 3.5}, 0.40, 0.47, 0.13, 0.65, 0.75};
        case DiffusionFamily::kProMist:
            return {{14.0, 1.5, 2, 0.0}, {150.0, 2.0, 3, 0.0},
                    {650.0, 2.5, 4, 2.9}, 0.28, 0.42, 0.30, 0.40, 1.05};
        case DiffusionFamily::kCinebloom:
        default:
            return {{20.0, 1.5, 2, 0.0}, {200.0, 2.0, 3, 0.0},
                    {1000.0, 2.5, 4, 2.5}, 0.22, 0.30, 0.48, 0.85, 1.00};
    }
}

// _DIFFUSION_STRENGTH_BREAKPOINTS / _DIFFUSION_STRENGTH_TOTAL_FRACTION.
constexpr double kStrengthBreaks[5] = {0.125, 0.25, 0.5, 1.0, 2.0};
constexpr double kStrengthFraction[5] = {0.10, 0.20, 0.35, 0.55, 0.75};

// _HALO_CHANNEL_WARMTH_AXIS (R warm, G mild, B cool).
constexpr double kHaloWarmthAxis[3] = {+1.30, +0.15, -1.45};

// np.interp(x, xp, fp) for monotonically increasing xp, with end clamping
// (numpy returns fp[0] / fp[-1] outside the range).
double np_interp(double x, const double* xp, const double* fp, int n) {
    if (x <= xp[0]) return fp[0];
    if (x >= xp[n - 1]) return fp[n - 1];
    int i = 1;
    while (i < n && x > xp[i]) ++i;
    double t = (x - xp[i - 1]) / (xp[i] - xp[i - 1]);
    return fp[i - 1] + t * (fp[i] - fp[i - 1]);
}

// _strength_to_scatter(strength, family).
double strength_to_scatter(double strength, const FamilyCfg& cfg) {
    if (strength <= 0.0) return 0.0;
    double s = strength < 1e-6 ? 1e-6 : strength;  // np.clip(strength, 1e-6, None)
    double log_strength = std::log2(s);
    double log_breaks[5];
    for (int i = 0; i < 5; ++i) log_breaks[i] = std::log2(kStrengthBreaks[i]);
    double base_total = np_interp(log_strength, log_breaks, kStrengthFraction, 5);
    double v = base_total * cfg.total_gain;
    if (v < 0.0) v = 0.0;
    if (v > 0.99) v = 0.99;  // np.clip(..., 0.0, 0.99)
    return v;
}

// _expand_group: returns (lambdas, weights) summing to 1 inside the group.
void expand_group(const GroupCfg& g, bool is_bloom, std::vector<double>* lambdas,
                  std::vector<double>* weights) {
    double lambda_center = g.lambda_um;
    double spread = g.spread;
    int n = g.n_components < 1 ? 1 : g.n_components;
    if (n == 1 || spread <= 1.0) {
        lambdas->assign(1, lambda_center);
        weights->assign(1, 1.0);
        return;
    }
    double log_lo = std::log(lambda_center / spread);
    double log_hi = std::log(lambda_center * spread);
    lambdas->resize(n);
    weights->resize(n);
    // np.linspace(log_lo, log_hi, n): endpoints exact; interior via step.
    double step = (log_hi - log_lo) / static_cast<double>(n - 1);
    for (int k = 0; k < n; ++k) {
        double lg = (k == n - 1) ? log_hi : (log_lo + step * static_cast<double>(k));
        (*lambdas)[k] = std::exp(lg);
    }
    double wsum = 0.0;
    for (int k = 0; k < n; ++k) {
        double wk = is_bloom ? std::pow((*lambdas)[k], 2.0 - g.alpha) : 1.0;
        (*weights)[k] = wk;
        wsum += wk;
    }
    for (int k = 0; k < n; ++k) (*weights)[k] /= wsum;
}

// _halo_channel_weights: returns a (3, n) per-channel halo weight set,
// energy-conserving (each row sums to sum(weights)).
void halo_channel_weights(const std::vector<double>& weights, double warmth,
                          std::vector<std::vector<double>>* out /* [3][n] */) {
    int n = static_cast<int>(weights.size());
    out->assign(3, std::vector<double>(n));
    if (n < 2) {
        for (int c = 0; c < 3; ++c) (*out)[c] = weights;
        return;
    }
    double w = warmth;
    if (w < -1.5) w = -1.5;
    if (w > 1.5) w = 1.5;  // np.clip(warmth, -1.5, 1.5)
    // g = linspace(-1, 1, n); g -= average(g, weights=weights)
    std::vector<double> gv(n);
    double step = 2.0 / static_cast<double>(n - 1);
    for (int k = 0; k < n; ++k)
        gv[k] = (k == n - 1) ? 1.0 : (-1.0 + step * static_cast<double>(k));
    double wsum = 0.0, gwsum = 0.0;
    for (int k = 0; k < n; ++k) { wsum += weights[k]; gwsum += gv[k] * weights[k]; }
    double gavg = gwsum / wsum;  // np.average(g, weights=weights)
    for (int k = 0; k < n; ++k) gv[k] -= gavg;
    double target_total = wsum;  // np.sum(weights)
    for (int c = 0; c < 3; ++c) {
        std::vector<double> rawv(n);
        double s = 0.0;
        for (int k = 0; k < n; ++k) {
            double r = weights[k] * (1.0 + w * kHaloWarmthAxis[c] * gv[k]);
            if (r < 0.0) r = 0.0;  // np.maximum(raw, 0.0)
            rawv[k] = r;
            s += r;
        }
        if (s > 0.0) {
            for (int k = 0; k < n; ++k) (*out)[c][k] = rawv[k] * (target_total / s);
        } else {
            (*out)[c] = weights;
        }
    }
}

// Resolve the family cfg with the per-group multiplier overrides applied
// (mirrors _resolve_family_cfg). Returns the renormalized w_c/w_h/w_b and the
// scaled group lambda centres.
FamilyCfg resolve_family_cfg(DiffusionFamily fam, const DiffusionFilterParams& p) {
    FamilyCfg base = family_cfg(fam);
    double ci = p.core_intensity, hi = p.halo_intensity, bi = p.bloom_intensity;
    double cs = p.core_size, hs = p.halo_size, bs = p.bloom_size;
    if (ci == 1.0 && hi == 1.0 && bi == 1.0 && cs == 1.0 && hs == 1.0 && bs == 1.0)
        return base;
    double w_c = base.w_c * (ci > 0.0 ? ci : 0.0);
    double w_h = base.w_h * (hi > 0.0 ? hi : 0.0);
    double w_b = base.w_b * (bi > 0.0 ? bi : 0.0);
    double total = w_c + w_h + w_b;
    if (total <= 0.0) return base;
    FamilyCfg cfg = base;
    cfg.core.lambda_um = base.core.lambda_um * (cs > 1e-6 ? cs : 1e-6);
    cfg.halo.lambda_um = base.halo.lambda_um * (hs > 1e-6 ? hs : 1e-6);
    cfg.bloom.lambda_um = base.bloom.lambda_um * (bs > 1e-6 ? bs : 1e-6);
    cfg.w_c = w_c / total;
    cfg.w_h = w_h / total;
    cfg.w_b = w_b / total;
    return cfg;
}

// _bloom_max_lambda_um: largest lambda in the bloom progression (image-plane µm).
double bloom_max_lambda_um(const FamilyCfg& cfg) {
    return cfg.bloom.lambda_um * cfg.bloom.spread;
}

// Pick the convolution implementation by COST, not by a magic kernel size, because
// the crossover moves with the image as well as the kernel.
//
//   direct : w*h*ks^2 multiply-adds.
//   FFT    : per tile, two 2D transforms (the image forward, the product inverse)
//            plus the pointwise multiply. A 2D transform of N x N is 2N line
//            transforms of length N, each ~5*N*log2(N) flops. The kernel's own
//            transform is amortised over the tiles and ignored, which biases the
//            estimate slightly AGAINST the FFT -- the safe direction.
//
// Measured crossover on this model is ks ~= 15 at both preview and export sizes,
// which matches the closed-form estimate in docs/research/perf-lab.md 20. Black
// Pro-Mist runs ks = 273 at the 640px default preview and ks = 1725 at 12 MP, so
// it is 20-120x past the crossover and always takes the FFT.
//
// SPK_DIFFUSION_FFT=0 forces the direct loop (A/B measurement, and an escape hatch
// if a device ever mis-measures); =1 forces the FFT even where direct would win.
// Kept for the stable diagnostics ABI. Cost-selected FFT work no longer falls
// back to the direct O(w*h*ks^2) loop: scratch denial propagates as OOM, so this
// counter must remain zero. A non-zero value is therefore a regression alarm.
thread_local std::atomic<unsigned long long> g_fft_fallbacks{0};

// Transform-size cap, overridable so the trade can be MEASURED rather than assumed.
// Scratch is 2*N*(N/2+1)*2*8 + N*N*8 bytes: 100.7 MB at N = 2048, 402.8 MB at
// N = 4096. (This comment previously said 134 / 537 MB -- the pre-r2c formula.
// The real-to-complex change halved the spectra and nobody updated the number.)
//
// Raising it is worth a lot when the kernel is large -- at 12 MP Black Pro-Mist
// has ks = 1725, so N = 2048 leaves a usable block of only 324 (2.5% of each
// transform) and needs 130 tiles, where N = 4096 gives a block of 2372 and needs
// 4. But 402.8 MB is a lot to ask of a phone mid-export, and if the allocation
// fails, fft_convolve_same throws std::bad_alloc and the render boundary reports
// a controlled OOM. It must never fall through to the direct loop: at 12 MP that
// can turn resource pressure into hours of unbounded work.
// An r2c f32 transform would buy N=4096's block size at roughly N=2048's memory.
// Read a tuning knob that must be settable on a SHIPPING build.
//
// std::getenv alone is not enough on Android: an app process inherits no shell
// environment, and `wrap.<pkg>` (the usual way in) requires a debuggable app, so
// on a release APK -- the only build whose numbers are worth measuring -- these
// knobs were simply unreachable. The device session hit exactly that: the
// SPK_DIFFUSION_FFT_MAX experiment could not be run without a rebuild per value,
// and a rebuild drops the loaded image, so the experiment cost a human every
// time.
//
// So: env var first (host tests, benches, CI), then an Android system property,
// which `adb shell setprop` can set on a release build with no rebuild:
//
//     adb shell setprop debug.spektra.fftmax 4096
//     adb shell setprop debug.spektra.fft 0        # force the direct loop
//
// Same mechanism as debug.spektra.dumpparams, which is already proven to work on
// this project's release builds.
const char* tuning_knob(const char* env_name, const char* prop_name, char* buf,
                        size_t cap) {
    if (const char* env = std::getenv(env_name)) return env;
#if defined(__ANDROID__)
    if (cap >= PROP_VALUE_MAX && __system_property_get(prop_name, buf) > 0 && buf[0])
        return buf;
#else
    (void)prop_name; (void)buf; (void)cap;
#endif
    return nullptr;
}

// Scratch for one N x N transform: two r2c spectra plus one real plane.
double fft_scratch_bytes(int n) {
    const double nd = n;
    return 2.0 * nd * (nd / 2.0 + 1.0) * 2.0 * 8.0 + nd * nd * 8.0;
}

int fft_max_transform() {
    char buf[92] = {0};
    if (const char* v0 = tuning_knob("SPK_DIFFUSION_FFT_MAX", "debug.spektra.fftmax",
                                     buf, sizeof(buf))) {
        const int v = std::atoi(v0);
        if (v >= 16) return v;
    }
    return kFftConvMaxTransform;
}

// Admit the transform's scratch through the process memory budget, stepping the
// ceiling down until one fits.
//
// N = 4096 is 5.2x faster than 2048 on a 12 MP Pro-Mist frame, but it wants
// 402.8 MB, and a 12 MP export already peaks near 1.5 GB. Reserving it (rather
// than guessing a fraction of the headroom) is what keeps that from turning a
// completing render into a controlled OOM: if the budget cannot admit 4096 the
// next candidate down is tried, and the render is slower instead of dead. It
// also makes this scratch VISIBLE in spk.memory_budget.v1, which it was not.
//
// The reservation is held by the caller for exactly the convolution's lifetime,
// so a second render sees the first one's bytes rather than the same headroom
// twice.
memory::MemoryReservation reserve_fft_scratch(int w, int h, int ks, int* chosen_cap) {
    const int ceiling = fft_max_transform();
    memory::MemoryBudget& budget = memory::process_memory_budget();
    for (int cap = ceiling; cap >= 16; cap /= 2) {
        const int n = fft_convolve_transform_size(w, h, ks, cap);
        if (n < ks + 1) break;
        memory::MemoryReservation reservation = budget.try_reserve(
            static_cast<std::uint64_t>(fft_scratch_bytes(n)),
            memory::MemoryDomain::NativeScratch, memory::MemoryStage::Spatial);
        if (reservation) {
            *chosen_cap = cap;
            return reservation;
        }
        // The size the selector picked is independent of the ceiling below some
        // point (it is already choosing the cheapest); once that happens, going
        // lower cannot free anything, so stop rather than spin.
        if (fft_convolve_transform_size(w, h, ks, cap / 2) == n) break;
    }
    *chosen_cap = 0;
    return {};
}

bool use_fft(int w, int h, int ks) {
    char buf[92] = {0};
    if (const char* v0 = tuning_knob("SPK_DIFFUSION_FFT", "debug.spektra.fft",
                                     buf, sizeof(buf))) {
        if (v0[0] == '0') return false;
        if (v0[0] == '1') return true;
    }
    // Estimated against the CEILING, not the transform the budget will actually
    // admit. A clamped run picks a smaller N and is slower, but still orders of
    // magnitude below the direct loop at the kernel sizes that get here, so the
    // FFT-vs-direct verdict does not change.
    const int n = fft_convolve_transform_size(w, h, ks, fft_max_transform());
    if (n < ks + 1) return false;                 // no valid block -> direct
    const int block = n - ks + 1;
    const double tiles = std::ceil(static_cast<double>(h) / block) *
                         std::ceil(static_cast<double>(w) / block);
    const double nn = static_cast<double>(n) * n;
    const double log2n = std::log2(static_cast<double>(n));
    const double fft_flops = tiles * (2.0 * (2.0 * 5.0 * nn * log2n) + 6.0 * nn);
    // Direct MACs are ~2 flops each; require a 2x margin before switching, so a
    // near-tie keeps the bit-exact path.
    const double direct_flops =
        2.0 * static_cast<double>(w) * static_cast<double>(h) * ks * ks;
    return fft_flops * 2.0 < direct_flops;
}

}  // namespace

unsigned long long diffusion_fft_fallbacks() {
    return g_fft_fallbacks.load(std::memory_order_relaxed);
}

void diffusion_reset_fft_fallbacks() {
    g_fft_fallbacks.store(0, std::memory_order_relaxed);
}

void apply_diffusion_filter_um(double* raw, int w, int h,
                               const DiffusionFilterParams& params,
                               double pixel_size_um) {
    if (!params.active) return;
    if (params.strength <= 0.0 || params.spatial_scale <= 0.0) return;
    if (w <= 0 || h <= 0 || pixel_size_um <= 0.0) return;

    const FamilyCfg cfg = resolve_family_cfg(params.family, params);
    const double p_s = strength_to_scatter(params.strength, cfg);
    if (p_s <= 0.0) return;

    const double spatial_scale = params.spatial_scale > 1e-6 ? params.spatial_scale : 1e-6;

    // Kernel radius (matches apply_diffusion_filter_um):
    //   radius = ceil(max(8 * bloom_max_lambda_px, 5))
    //   radius = min(radius, max(min(h,w)//2 - 1, 1))
    const double bloom_max_lambda_px =
        bloom_max_lambda_um(cfg) * params.spatial_scale / pixel_size_um;
    double rd = 8.0 * bloom_max_lambda_px;
    if (rd < 5.0) rd = 5.0;
    int radius = static_cast<int>(std::ceil(rd));
    int min_hw = (h < w ? h : w);
    int cap = min_hw / 2 - 1;
    if (cap < 1) cap = 1;
    if (radius > cap) radius = cap;

    const int ks = 2 * radius + 1;
    // effective_warmth = halo_warmth_base + halo_warmth.
    const double effective_warmth = cfg.halo_warmth_base + params.halo_warmth;

    // Expand groups (lambdas in µm -> pixel; the channel-independent core/bloom
    // exp-sums and the per-channel halo weights).
    std::vector<double> core_lambdas, core_weights;
    std::vector<double> halo_lambdas, halo_weights;
    std::vector<double> bloom_lambdas, bloom_weights;
    expand_group(cfg.core, /*is_bloom=*/false, &core_lambdas, &core_weights);
    expand_group(cfg.halo, /*is_bloom=*/false, &halo_lambdas, &halo_weights);
    expand_group(cfg.bloom, /*is_bloom=*/true, &bloom_lambdas, &bloom_weights);

    std::vector<std::vector<double>> halo_per_ch;  // [3][N_halo]
    halo_channel_weights(halo_weights, effective_warmth, &halo_per_ch);

    auto to_px = [&](std::vector<double>& v) {
        for (double& x : v) x = x * spatial_scale / pixel_size_um;
    };
    to_px(core_lambdas);
    to_px(halo_lambdas);
    to_px(bloom_lambdas);

    // _exp_sum: sum_k w_k * exp(-r/lk) / (2*pi*lk^2), lk >= 1e-6.
    auto exp_sum = [](double r, const std::vector<double>& lambdas_px,
                      const std::vector<double>& weights) {
        double total = 0.0;
        for (size_t k = 0; k < lambdas_px.size(); ++k) {
            double lk = lambdas_px[k] > 1e-6 ? lambdas_px[k] : 1e-6;
            total += weights[k] * std::exp(-r / lk) / (2.0 * M_PI * lk * lk);
        }
        return total;
    };

    // Build the per-channel PSF on the (ks x ks) grid. r is measured from the
    // grid centre (cy, cx) = (ks//2, ks//2) (same as np.ogrid + the integer
    // centre in diffusion_filter_psf). Then sum-normalise each channel.
    const int cy = ks / 2, cx = ks / 2;
    std::vector<std::vector<double>> psf(3, std::vector<double>(
                                                static_cast<size_t>(ks) * ks));
    double psf_sum[3] = {0.0, 0.0, 0.0};
    for (int yy = 0; yy < ks; ++yy) {
        for (int xx = 0; xx < ks; ++xx) {
            double dx = static_cast<double>(xx - cx);
            double dy = static_cast<double>(yy - cy);
            double r = std::sqrt(dx * dx + dy * dy);
            double core = cfg.w_c * exp_sum(r, core_lambdas, core_weights);
            double bloom = cfg.w_b * exp_sum(r, bloom_lambdas, bloom_weights);
            size_t idx = static_cast<size_t>(yy) * ks + xx;
            for (int c = 0; c < 3; ++c) {
                double halo = cfg.w_h * exp_sum(r, halo_lambdas, halo_per_ch[c]);
                double v = core + halo + bloom;
                psf[c][idx] = v;
                psf_sum[c] += v;
            }
        }
    }
    for (int c = 0; c < 3; ++c)
        for (size_t i = 0; i < psf[c].size(); ++i) psf[c][i] /= psf_sum[c];

    // Reflect-pad the image (numpy mode='reflect': mirror WITHOUT edge repeat),
    // convolve each channel directly in double precision, slice back to the
    // original window. blurred = K_s * image (mode='same').
    const int pw = w + 2 * radius;
    const int ph = h + 2 * radius;
    // numpy 'reflect' index map for a coordinate p in [-radius, w-1+radius].
    auto reflect = [](int p, int n) {
        if (n == 1) return 0;
        int period = 2 * (n - 1);
        int m = p % period;
        if (m < 0) m += period;
        return m < n ? m : period - m;
    };

    std::vector<double> blurred(static_cast<size_t>(w) * h * 3);
    // Build the padded plane for one channel, convolve, write back. Both passes
    // are per-row maps with disjoint writes (padded is fully built before the
    // convolution reads it, and each channel completes before the next reuses
    // the plane) -> deterministic parallel chunks, byte-identical for any
    // worker count.
    std::vector<double> padded(static_cast<size_t>(pw) * ph);
    for (int c = 0; c < 3; ++c) {
        parallel_for_weighted(0, ph, pw, [&](int lo, int hi) {
            for (int yy = lo; yy < hi; ++yy) {
                int sy = reflect(yy - radius, h);
                for (int xx = 0; xx < pw; ++xx) {
                    int sx = reflect(xx - radius, w);
                    padded[static_cast<size_t>(yy) * pw + xx] =
                        raw[(static_cast<size_t>(sy) * w + sx) * 3 + c];
                }
            }
        });
        const std::vector<double>& kern = psf[c];
        // mode='same' convolution: out[y,x] = sum_{i,j} padded[y+i, x+j]
        // * flip(kern)[i,j], centred. For a symmetric centred kernel the centre
        // offsets (ks-1)/2 == radius, so out[y,x] over the original window maps
        // to padded[y .. y+ks-1, x .. x+ks-1] convolved with the flipped kernel.
        //
        // TWO IMPLEMENTATIONS OF THE SAME SUM. The direct loop below is O(w*h*ks^2),
        // and ks scales with image width (radius is a fixed size on the FILM, so it
        // grows as pixel_size_um shrinks) -- which makes this stage QUADRATIC in
        // pixel count. Measured: 30.7 s for one 640px preview and an extrapolated
        // 10.9 hours for a 12 MP export, at the app's default Black Pro-Mist
        // settings (#160, docs/research/perf-lab.md 20). kernels/fft_convolve
        // computes the identical sum in O(n log n).
        //
        // Reassociating a sum changes its last bits, so the FFT result is NOT
        // byte-identical to the direct loop -- measured drift is ~1e-15 relative,
        // eleven orders inside the 1e-4 parity bar (tests/test_fft_convolve.cpp).
        // Both paths ARE byte-identical across worker counts, which is the contract
        // that matters here.
        //
        // The direct loop stays reachable only when the cost model selects it for
        // a small kernel. Once FFT is selected, memory denial must fail closed.
        if (use_fft(w, h, ks)) {
            int cap = 0;
            const memory::MemoryReservation scratch =
                reserve_fft_scratch(w, h, ks, &cap);
            if (!scratch) {
                // Every candidate was refused, so there is no affordable FFT. The
                // direct loop is not a fallback here -- at 12 MP it is hours -- so
                // this surfaces as OOM exactly like a failed allocation would.
                throw std::bad_alloc{};
            }
            if (!fft_convolve_same(padded.data(), pw, ph, kern.data(), ks, w, h,
                                   blurred.data(), /*out_stride=*/3, /*out_offset=*/c,
                                   cap)) {
                // All dimensions above are constructed from the same validated
                // image/kernel geometry. False therefore means an internal
                // invariant bug, not a recoverable request for the direct path.
                throw std::logic_error("diffusion FFT rejected valid geometry");
            }
            continue;
        }
        // Each output row is an independent O(w*ks^2) accumulation over the
        // read-only padded plane.
        parallel_for_weighted(0, h, w, [&](int lo, int hi) {
            for (int y = lo; y < hi; ++y) {
                for (int x = 0; x < w; ++x) {
                    double acc = 0.0;
                    for (int i = 0; i < ks; ++i) {
                        const double* prow =
                            &padded[static_cast<size_t>(y + i) * pw + x];
                        // flip(kern) row index (ks-1-i), reversed columns.
                        const double* krow =
                            &kern[static_cast<size_t>(ks - 1 - i) * ks];
                        for (int j = 0; j < ks; ++j) {
                            acc += prow[j] * krow[ks - 1 - j];
                        }
                    }
                    blurred[(static_cast<size_t>(y) * w + x) * 3 + c] = acc;
                }
            }
        });
    }

    // E_out = (1 - p_s) * E_in + p_s * blurred. Per-element map.
    const int total = w * h * 3;
    parallel_for(0, total, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i)
            raw[i] = (1.0 - p_s) * raw[i] + p_s * blurred[i];
    });
}

void apply_highlight_boost(double* raw, int w, int h, const HalationParams& params) {
    // Port of spektrafilm/utils/numba_boost_hightlights.py::boost_highlights, called
    // by filming.py::expose as boost_highlights(raw, boost_ev, boost_range,
    // protect_ev) with the default midgray = 0.184. boost_ev <= 0 is a strict
    // identity (the oracle's `if boost_ev == 0: return x` no-op).
    const double boost_ev = params.boost_ev;
    if (boost_ev <= 0.0) return;
    if (w <= 0 || h <= 0) return;
    const double boost_range = params.boost_range;
    const double protect_ev = params.protect_ev;
    const double midgray = 0.184;  // boost_highlights default; filming.py passes none.
    const size_t total = static_cast<size_t>(w) * h * 3;

    // max_raw = np.max(x). A plain reduction; max is order-independent, so this is
    // byte-identical for any worker count (the boost stays thread-invariant).
    double max_raw = raw[0];
    for (size_t i = 1; i < total; ++i)
        if (raw[i] > max_raw) max_raw = raw[i];
    if (max_raw == 0.0) {
        for (size_t i = 0; i < total; ++i) raw[i] = 0.0;
        return;
    }

    // raw_x0 = clip(midgray * 2^protect_ev, 0, max_raw). raw_x0 == max_raw -> identity.
    double raw_x0 = midgray * std::pow(2.0, protect_ev);
    if (raw_x0 < 0.0) raw_x0 = 0.0;
    if (raw_x0 > max_raw) raw_x0 = max_raw;
    if (raw_x0 == max_raw) return;

    const double a = std::pow(28.0, 1.0 - boost_range);
    const double x0 = raw_x0 / max_raw;
    const double denom = std::exp(a * (1.0 - x0)) - a * (1.0 - x0) - 1.0;
    // denom > 0 for all valid params (exp(t) > 1 + t for t = a(1-x0) > 0); the oracle
    // raises here, but the engine path must never throw, so guard defensively.
    if (denom <= 0.0) return;
    const double k = (std::pow(2.0, boost_ev) - 1.0) / denom;
    const double inv_max_raw = 1.0 / max_raw;
    const double boost_scale = k * max_raw;

    // Per-element map, disjoint writes -> deterministic parallel chunks
    // (byte-identical for any worker count; 1-vs-8 gated by
    // test_highlight_boost_e2e).
    parallel_for(0, static_cast<int>(total), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            const double xv = raw[i];
            if (xv > raw_x0) {
                const double dx = (xv - raw_x0) * inv_max_raw;
                raw[i] = xv + boost_scale * (std::exp(a * dx) - a * dx - 1.0);
            }
        }
    });
}

}  // namespace spk
