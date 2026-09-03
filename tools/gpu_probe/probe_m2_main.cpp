/*
 * Spektrafilm for Android — on-device GPU numeric probe M2 (#147, filming +
 * printing integrals; extends the #135 E3 scan measurement). GPLv3.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Film modeling powered by spektrafilm.
 *
 * --------------------------------------------------------------------------------
 * Standalone arm64 executable (adb push to /data/local/tmp; NOT an app change).
 * Measures the fp32 GPU FILMING and PRINTING per-pixel kernels (probe-local
 * shaders filming.comp / printing.comp — engine sources byte-untouched) against
 * f64 CPU references that mirror each shader 1:1, on real hardware. GPU stays
 * PREVIEW-ONLY regardless of the result.
 *
 * Subcommands:
 *   gpu_probe_m2 film  <film.json> <spectra.npy> <input_rgb.f64> <golden_dir>
 *   gpu_probe_m2 print <film.json> <paper.json> <filters.json> <spectra.npy> <golden_dir>
 *
 * Tables are folded on the host through the ENGINE'S OWN builders
 * (build_filming_tc_lut, normalize_density_curves, compute_dir_couplers_matrix +
 * np_interp_array, digest_printing_params + resolve_neutral_cc +
 * compute_midgray_exposure_factor) and downcast to fp32. Each subcommand also
 * runs the REAL engine stage on the same inputs and prints:
 *   CHAIN  — f64 mirror vs engine output (bounds the fold + fp32-table gap)
 *   SETUP  — engine output vs committed golden (proves the digest matches the
 *            goldens' parameters; must PASS the 1e-4/1e-5 bar)
 *   CASE   — GPU vs f64 mirror (precision isolation), GPU vs engine, GPU vs
 *            golden (the oracle bar), determinism x5 on poisoned host buffers.
 * --------------------------------------------------------------------------------
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "io/npy_lut.h"
#include "model/couplers.h"
#include "model/density_curves.h"
#include "profiles/profile.h"
#include "runtime/params.h"
#include "runtime/print_digest.h"
#include "runtime/stages/filming.h"
#include "runtime/stages/printing.h"
#include "spkvec_io.h"

#include "filming_spv.h"
#include "gpu_dispatch.h"
#include "printing_spv.h"
#include "ref_filming_f64.h"
#include "ref_printing_f64.h"

namespace {

constexpr int kNB = 81;

// D55 standard illuminant (film reference illuminant), 380..780 @5nm, mean-norm.
// Baked at full double precision from the Python oracle (standard_illuminant),
// identical to the constant the host parity tests use (tests/test_filming.cpp).
static const double kD55Illuminant[81] = {
    0.3792826592081565,0.41130471283820924,0.4433384066186183,0.5763969653409493,
    0.7094555240632804,0.7537113757177554,0.7979788675225865,0.8155671347108852,
    0.8331670420495402,0.8118539267472403,0.7905291712945844,0.8934979413460009,
    0.996455071247061,1.0685541625536938,1.1406532538603265,1.1550288395502992,
    1.169404425240272,1.1662033838923025,1.1630023425443328,1.1794498749977185,
    1.1958974074511046,1.1687758571210345,1.141642666640608,1.1567865022540935,
    1.1719303378675792,1.172023459070429,1.1721049401229227,1.167984326896809,
    1.1638637136706955,1.1884360710727462,1.213020068625153,1.2007513501496623,
    1.1884826316741712,1.1935228167784289,1.1985630018826865,1.1812890187540066,
    1.1640150356253267,1.1478119463294223,1.1316088570335177,1.134705137028281,
    1.1378130571734006,1.1010418221979967,1.0642822273729489,1.0816726120051912,
    1.0990513564870772,1.1032534507656848,1.107443904893936,1.1020894357300595,
    1.096734966566183,1.0747816429942894,1.0528283194223955,1.0637817009076298,
    1.0747350823928643,1.054504501073696,1.0342739197545279,1.0427945098153053,
    1.0513034597257263,1.0724419727726824,1.0935921259699946,1.0703467457085567,
    1.047101365447119,0.9872826327663333,0.9274522599351918,0.945855337648428,
    0.9642700555120207,0.9759334861689865,0.9875969168259522,0.9025656184735221,
    0.8175459602714483,0.8703107618363444,0.9230755634012404,0.9562034313151373,
    0.989331299229034,0.9130184734934376,0.8366940076074848,0.72561205275776,
    0.6145184577576788,0.7491600769284603,0.883801696099242,0.8598811871171415,
    0.8359723182853972};

// ── Error stats ─────────────────────────────────────────────────────────────

struct Stats {
    double max_abs = 0.0, rms = 0.0;
    size_t worst_i = 0;
    int nan_a = 0, nan_b = 0;
};

Stats compare_fd(const float* a, const double* b, size_t ncomp) {
    Stats s;
    double sum2 = 0.0;
    for (size_t i = 0; i < ncomp; ++i) {
        const double x = static_cast<double>(a[i]);
        const double y = b[i];
        if (std::isnan(x)) ++s.nan_a;
        if (std::isnan(y)) ++s.nan_b;
        if (std::isnan(x) || std::isnan(y)) continue;
        const double d = std::fabs(x - y);
        sum2 += d * d;
        if (d > s.max_abs) { s.max_abs = d; s.worst_i = i; }
    }
    s.rms = std::sqrt(sum2 / static_cast<double>(ncomp));
    return s;
}

Stats compare_ff(const float* a, const float* b, size_t ncomp) {
    Stats s;
    double sum2 = 0.0;
    for (size_t i = 0; i < ncomp; ++i) {
        const double x = static_cast<double>(a[i]);
        const double y = static_cast<double>(b[i]);
        if (std::isnan(x)) ++s.nan_a;
        if (std::isnan(y)) ++s.nan_b;
        if (std::isnan(x) || std::isnan(y)) continue;
        const double d = std::fabs(x - y);
        sum2 += d * d;
        if (d > s.max_abs) { s.max_abs = d; s.worst_i = i; }
    }
    s.rms = std::sqrt(sum2 / static_cast<double>(ncomp));
    return s;
}

Stats compare_df(const double* a, const float* b, size_t ncomp) {
    return compare_fd(b, a, ncomp);  // symmetric in |a-b|
}

int count_nan_f(const std::vector<float>& v) {
    int c = 0;
    for (float x : v) if (std::isnan(x)) ++c;
    return c;
}

// ── Filming tables (host fold through the engine's own builders) ────────────

struct FilmTables {
    int L = 0, n = 0;
    std::vector<float> tc_lut, dev_axis, dev_curve, dir_axis, dir_curve;
    float m[9];
    float exp_mult = 1.0f, shift = 0.0f;
    spk::NdArray tc_lut_f64;   // for the engine reference run
    spk::FilmingParams params;
};

FilmTables build_film_tables(const spk::Profile& film, const spk::NdArray& spectra_lut) {
    FilmTables T;
    T.params = spk::digest_filming_params(film.is_negative(), /*spatial=*/false,
                                          film.stock.c_str());
    const spk::FilmingParams& fp = T.params;
    // The probe covers exactly the pointwise-fused expose + pointwise develop
    // chain the goldens exercise — bail loudly on anything else.
    if (!film.is_negative() || !fp.dir_couplers.active || fp.grain.active ||
        fp.halation.active || fp.diffusion_filter.active ||
        fp.halation.boost_ev > 0.0 || fp.lens_blur_um > 0.0 ||
        fp.bw_exposure_correction != 1.0) {
        std::fprintf(stderr, "film params outside the probe's pointwise scope\n");
        std::exit(2);
    }

    T.tc_lut_f64 = spk::build_filming_tc_lut(film, spectra_lut, kD55Illuminant);
    T.L = T.tc_lut_f64.shape[0];
    T.tc_lut.resize(T.tc_lut_f64.data.size());
    for (size_t i = 0; i < T.tc_lut.size(); ++i)
        T.tc_lut[i] = static_cast<float>(T.tc_lut_f64.data[i]);

    const int n = film.n_density_pts;
    T.n = n;
    // develop: normalized curves + the fp32 le/gamma axis, exactly as
    // interpolate_exposure_to_density builds them.
    T.dev_curve.resize(static_cast<size_t>(n) * 3);
    spk::normalize_density_curves(film.density_curves.data(), n, T.dev_curve.data());
    T.dev_axis.resize(static_cast<size_t>(n) * 3);
    for (int k = 0; k < n; ++k)
        for (int c = 0; c < 3; ++c) {
            const float g = fp.density_curve_gamma[c];
            T.dev_axis[k * 3 + c] =
                (g != 0.0f) ? (film.log_exposure[k] / g) : film.log_exposure[k];
        }

    // DIR-coupler tables, mirroring apply_density_correction_dir_couplers's
    // per-call setup (negative film) in f64 via the engine's own helpers, then
    // downcast. NOTE: develop() hands the couplers the NORMALIZED curves (ndc),
    // so dc/silver_curve here are the normalized curves, not the raw ones:
    // M, silver_curve = ndc, le0 = le - silver@M,
    // dc0[:,c] = np.interp(le, le0[:,c], ndc[:,c]); axis = le/gamma.
    double M[9];
    spk::compute_dir_couplers_matrix(fp.dir_couplers, M);
    for (int i = 0; i < 9; ++i) T.m[i] = static_cast<float>(M[i]);
    T.shift = static_cast<float>(fp.dir_couplers.high_exposure_couplers_shift);
    T.exp_mult = static_cast<float>(std::pow(2.0, fp.exposure_compensation_ev));

    std::vector<double> le(n), le0(n), ycol(n), vbuf(n);
    for (int k = 0; k < n; ++k)
        le[k] = static_cast<double>(film.log_exposure[k]);
    std::vector<double> dc(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n * 3; ++i)
        dc[i] = static_cast<double>(T.dev_curve[i]);  // ndc, as develop() passes

    T.dir_axis.resize(static_cast<size_t>(n) * 3);
    T.dir_curve.resize(static_cast<size_t>(n) * 3);
    for (int c = 0; c < 3; ++c) {
        for (int j = 0; j < n; ++j) {
            double amt = 0.0;
            for (int k = 0; k < 3; ++k) amt += dc[j * 3 + k] * M[k * 3 + c];
            le0[j] = le[j] - amt;
            ycol[j] = dc[j * 3 + c];
        }
        spk::np_interp_array(le.data(), n, le0.data(), ycol.data(), n, vbuf.data());
        const double g = static_cast<double>(fp.density_curve_gamma[c]);
        for (int j = 0; j < n; ++j) {
            T.dir_axis[j * 3 + c] = static_cast<float>(le[j] / g);
            T.dir_curve[j * 3 + c] = static_cast<float>(vbuf[j]);
        }
    }
    return T;
}

struct FilmPush {
    uint32_t npix;
    int32_t L, n;
    float exp_mult, shift;
    float m[9];
};

bool gpu_film(const FilmTables& T, const float* rgb, uint32_t npix, float* out) {
    FilmPush push{};
    push.npix = npix;
    push.L = T.L;
    push.n = T.n;
    push.exp_mult = T.exp_mult;
    push.shift = T.shift;
    std::memcpy(push.m, T.m, sizeof(push.m));
    const size_t img = static_cast<size_t>(npix) * 3 * sizeof(float);
    probe::Buf bufs[7] = {
        {rgb, nullptr, img},
        {nullptr, out, img},
        {T.tc_lut.data(), nullptr, T.tc_lut.size() * sizeof(float)},
        {T.dev_axis.data(), nullptr, T.dev_axis.size() * sizeof(float)},
        {T.dev_curve.data(), nullptr, T.dev_curve.size() * sizeof(float)},
        {T.dir_axis.data(), nullptr, T.dir_axis.size() * sizeof(float)},
        {T.dir_curve.data(), nullptr, T.dir_curve.size() * sizeof(float)},
    };
    return probe::dispatch(kFilmingSpv, sizeof(kFilmingSpv), &push, sizeof(push),
                           bufs, 7, (npix + 63u) / 64u);
}

void ref_film(const FilmTables& T, const float* rgb, uint32_t npix, double* out) {
    ref_filming_f64(rgb, out, npix, T.tc_lut.data(), T.L, T.dev_axis.data(),
                    T.dev_curve.data(), T.dir_axis.data(), T.dir_curve.data(), T.n,
                    T.exp_mult, T.shift, T.m);
}

// Run the REAL engine filming stage (expose f64 + develop) on a float64 input.
void engine_film(const FilmTables& T, const spk::Profile& film, const double* rgb,
                 int width, int height, std::vector<float>* out) {
    const int npix = width * height;
    std::vector<float> lr(static_cast<size_t>(npix) * 3);
    spk::expose(rgb, width, height, T.params, T.tc_lut_f64, lr.data());
    out->assign(static_cast<size_t>(npix) * 3, 0.0f);
    spk::develop(lr.data(), width, height, film, T.params, out->data());
}

// ── Printing tables ─────────────────────────────────────────────────────────

struct PrintTables {
    int n = 0, nulled_bands = 0;
    std::vector<float> dye, isens, paper_axis, paper_curve;
    float midgray = 1.0f, mult = 1.0f, pf[3] = {0.0f, 0.0f, 0.0f};
    double cmax[3] = {0.0, 0.0, 0.0};  // sweep domain: nanmax(film density_curves)
    spk::PrintingParams params;
};

PrintTables build_print_tables(const spk::Profile& film, const spk::Profile& paper,
                               const std::string& filters_json,
                               const spk::NdArray& spectra_lut, double cc_out[3]) {
    PrintTables T;
    // Native digest, exactly as spektra.cpp's print route resolves it.
    spk::NdArray tc_lut = spk::build_filming_tc_lut(film, spectra_lut, kD55Illuminant);
    const double* enl = spk::enlarger_illuminant("TH-KG3");
    // The resolver is failure-atomic; initialize the schema fallback before a
    // missing/invalid neutral-filter database can leave the triple unchanged.
    double cc[3] = {0.0, 0.0, 0.0};
    spk::resolve_neutral_cc(filters_json, paper.stock, "TH-KG3", film.stock, cc);
    for (int k = 0; k < 3; ++k) cc_out[k] = cc[k];
    T.params = spk::digest_printing_params(cc, enl, /*midgray placeholder=*/1.0,
                                           /*gamma=*/1.0f);
    T.params.exposure_factor_midgray = spk::compute_midgray_exposure_factor(
        film, paper, tc_lut, T.params.filtered_illuminant, 1.0f,
        /*exposure_compensation_ev=*/0.0, /*normalize=*/true, /*compensation=*/true);
    const spk::PrintingParams& pp = T.params;
    if (pp.preflash_exposure > 0.0 || pp.diffusion_filter.active ||
        pp.use_enlarger_lut || pp.morph.active) {
        std::fprintf(stderr, "print params outside the probe's pointwise scope\n");
        std::exit(2);
    }

    const int S = film.n_samples;
    if (S != kNB) { std::fprintf(stderr, "n_samples=%d != %d\n", S, kNB); std::exit(2); }
    // print sensitivity = nan_to_num(10^log_sensitivity), engine print_expose.
    std::vector<double> sens(static_cast<size_t>(S) * 3);
    for (int l = 0; l < S; ++l)
        for (int k = 0; k < 3; ++k) {
            double v = std::pow(10.0, static_cast<double>(
                                          paper.log_sensitivity[static_cast<size_t>(l) * 3 + k]));
            if (std::isnan(v)) v = 0.0;
            sens[static_cast<size_t>(l) * 3 + k] = v;
        }
    // Fold: isens[l,k] = 10^-base[l] * filtered_illuminant[l] * sens[l,k].
    // Bands where channel_density, base_density or the filtered illuminant is NaN
    // produce light = NaN -> 0 in the engine for EVERY pixel — both table rows
    // are zeroed (the same argument as the M1 scan fold).
    T.dye.assign(static_cast<size_t>(S) * 3, 0.0f);
    T.isens.assign(static_cast<size_t>(S) * 3, 0.0f);
    for (int l = 0; l < S; ++l) {
        const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
        const float base = film.base_density[static_cast<size_t>(l)];
        const double fil = pp.filtered_illuminant[l];
        const bool nul = std::isnan(base) || std::isnan(fil) || std::isnan(cd[0]) ||
                         std::isnan(cd[1]) || std::isnan(cd[2]);
        if (nul) { ++T.nulled_bands; continue; }
        T.dye[l * 3 + 0] = cd[0];
        T.dye[l * 3 + 1] = cd[1];
        T.dye[l * 3 + 2] = cd[2];
        const double w = std::pow(10.0, -static_cast<double>(base)) * fil;
        for (int k = 0; k < 3; ++k)
            T.isens[l * 3 + k] = static_cast<float>(w * sens[static_cast<size_t>(l) * 3 + k]);
    }

    // develop_simple tables: RAW paper curves + fp32 le/gamma axis.
    const int n = paper.n_density_pts;
    T.n = n;
    T.paper_curve.assign(paper.density_curves.begin(),
                         paper.density_curves.begin() + static_cast<size_t>(n) * 3);
    T.paper_axis.resize(static_cast<size_t>(n) * 3);
    for (int k = 0; k < n; ++k)
        for (int c = 0; c < 3; ++c) {
            const float g = pp.density_curve_gamma[c];
            T.paper_axis[k * 3 + c] =
                (g != 0.0f) ? (paper.log_exposure[k] / g) : paper.log_exposure[k];
        }

    T.midgray = static_cast<float>(pp.exposure_factor_midgray);
    T.mult = static_cast<float>(pp.print_exposure * pp.bw_exposure_correction);

    // Sweep domain: per-channel nanmax of the FILM density_curves (the film
    // density image the print stage consumes; lower bound -0.1 covers the
    // engine's -grain_density_min domain, as in the M1 scan sweep).
    for (int c = 0; c < 3; ++c) T.cmax[c] = 0.0;
    for (int nrow = 0; nrow < film.n_density_pts; ++nrow) {
        const float* dcv = film.density_curves.data() + static_cast<size_t>(nrow) * 3;
        for (int c = 0; c < 3; ++c) {
            const double v = static_cast<double>(dcv[c]);
            if (!std::isnan(v) && v > T.cmax[c]) T.cmax[c] = v;
        }
    }
    return T;
}

struct PrintPush {
    uint32_t npix;
    int32_t n;
    float midgray, mult, pf0, pf1, pf2;
};

bool gpu_print(const PrintTables& T, const float* cmy, uint32_t npix, float* out) {
    PrintPush push{};
    push.npix = npix;
    push.n = T.n;
    push.midgray = T.midgray;
    push.mult = T.mult;
    push.pf0 = T.pf[0];
    push.pf1 = T.pf[1];
    push.pf2 = T.pf[2];
    const size_t img = static_cast<size_t>(npix) * 3 * sizeof(float);
    probe::Buf bufs[6] = {
        {cmy, nullptr, img},
        {nullptr, out, img},
        {T.dye.data(), nullptr, T.dye.size() * sizeof(float)},
        {T.isens.data(), nullptr, T.isens.size() * sizeof(float)},
        {T.paper_axis.data(), nullptr, T.paper_axis.size() * sizeof(float)},
        {T.paper_curve.data(), nullptr, T.paper_curve.size() * sizeof(float)},
    };
    return probe::dispatch(kPrintingSpv, sizeof(kPrintingSpv), &push, sizeof(push),
                           bufs, 6, (npix + 63u) / 64u);
}

void ref_print(const PrintTables& T, const float* cmy, uint32_t npix, double* out) {
    ref_printing_f64(cmy, out, npix, T.dye.data(), T.isens.data(),
                     T.paper_axis.data(), T.paper_curve.data(), T.n, T.midgray,
                     T.mult, T.pf);
}

void engine_print(const PrintTables& T, const spk::Profile& film,
                  const spk::Profile& paper, const float* cmy, int width, int height,
                  std::vector<float>* out) {
    const int npix = width * height;
    std::vector<float> lr(static_cast<size_t>(npix) * 3);
    spk::print_expose(film, paper, T.params, cmy, width, height, lr.data());
    out->assign(static_cast<size_t>(npix) * 3, 0.0f);
    spk::print_develop(paper, T.params, lr.data(), npix, out->data());
}

// ── Generic Tier-1 case: dispatch, compare, determinism x5 ──────────────────

template <typename DispatchFn>
int run_case(const char* name, DispatchFn&& dispatch, const float* in,
             uint32_t npix, const double* ref, const float* engine,
             const float* golden) {
    std::vector<float> gpu(static_cast<size_t>(npix) * 3, -1.0f);
    if (!dispatch(gpu.data())) {
        std::printf("CASE %-8s GPU_FAIL (dispatch returned false)\n", name);
        return 1;
    }
    const size_t ncomp = gpu.size();

    bool det = true;
    std::vector<float> again(ncomp);
    for (int rr = 0; rr < 4 && det; ++rr) {
        std::fill(again.begin(), again.end(), -2.0f);  // poison the host buffer
        if (!dispatch(again.data())) {
            std::printf("CASE %-8s GPU_FAIL on determinism rerun %d\n", name, rr + 2);
            return 1;
        }
        det = std::memcmp(gpu.data(), again.data(), ncomp * sizeof(float)) == 0;
    }

    const Stats s = compare_fd(gpu.data(), ref, ncomp);
    std::printf(
        "CASE %-8s npix=%-8u vs_f64: max_abs=%.9e rms=%.9e det_x5=%s nan_gpu=%d nan_ref=%d\n",
        name, npix, s.max_abs, s.rms, det ? "IDENTICAL" : "DIFFERS", s.nan_a, s.nan_b);
    {
        const size_t px = s.worst_i / 3, ch = s.worst_i % 3;
        std::printf("  worst: px=%zu comp=%zu in=(%.9g, %.9g, %.9g) ref=%.17g gpu=%.9g\n",
                    px, ch, in[px * 3], in[px * 3 + 1], in[px * 3 + 2], ref[s.worst_i],
                    gpu[s.worst_i]);
    }
    if (engine) {
        const Stats se = compare_ff(gpu.data(), engine, ncomp);
        std::printf("  vs_engine: max_abs=%.9e rms=%.9e\n", se.max_abs, se.rms);
    }
    if (golden) {
        const Stats sg = compare_ff(gpu.data(), golden, ncomp);
        std::printf("  vs_golden: max_abs=%.9e rms=%.9e %s\n", sg.max_abs, sg.rms,
                    (sg.max_abs <= 1e-4 && sg.rms <= 1e-5) ? "(inside oracle bar)"
                                                           : "(OUTSIDE oracle bar)");
    }
    return 0;
}

void print_chain_setup(const char* tag, const double* ref, const float* engine,
                       const float* golden, size_t ncomp) {
    const Stats ch = compare_df(ref, engine, ncomp);
    std::printf("CHAIN %s f64_mirror_vs_engine: max_abs=%.9e rms=%.9e\n", tag,
                ch.max_abs, ch.rms);
    const Stats st = compare_ff(engine, golden, ncomp);
    std::printf("SETUP %s engine_vs_golden: max_abs=%.9e rms=%.9e %s\n", tag,
                st.max_abs, st.rms,
                (st.max_abs <= 1e-4 && st.rms <= 1e-5) ? "PASS" : "SETUP_FAIL");
}

// ── film subcommand ─────────────────────────────────────────────────────────

int do_film(const char* film_json, const char* spectra_npy, const char* input_f64,
            const std::string& golden_dir) {
    if (!probe::gpu_available()) {
        std::printf("FILM GPU unavailable\n");
        return 1;
    }
    spk::Profile film = spk::load_profile_file(film_json);
    spk::NdArray spectra_lut = spk::load_npy(spectra_npy);
    FilmTables T = build_film_tables(film, spectra_lut);
    std::printf("TABLES film=%s L=%d n=%d shift=%.9g exp_mult=%.9g nan_tc=%d nan_dev=%d nan_dir=%d\n",
                film.stock.c_str(), T.L, T.n, T.shift, T.exp_mult,
                count_nan_f(T.tc_lut), count_nan_f(T.dev_curve), count_nan_f(T.dir_curve));

    spkvec::Array gold = spkvec::read(golden_dir + "/film_density_cmy.spkvec");
    const int height = static_cast<int>(gold.shape[0]);
    const int width = static_cast<int>(gold.shape[1]);
    const int npix = width * height;

    // Original float64 synthetic input (the engine consumes f64; the GPU + f64
    // mirror consume its fp32 cast — the cast is part of the fp32 story).
    std::vector<double> rgb64(static_cast<size_t>(npix) * 3);
    {
        std::ifstream in(input_f64, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", input_f64); return 2; }
        in.read(reinterpret_cast<char*>(rgb64.data()),
                static_cast<std::streamsize>(rgb64.size() * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(rgb64.size() * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch\n");
            return 2;
        }
    }
    std::vector<float> rgb32(rgb64.size());
    for (size_t i = 0; i < rgb64.size(); ++i) rgb32[i] = static_cast<float>(rgb64[i]);

    std::vector<float> engine_out;
    engine_film(T, film, rgb64.data(), width, height, &engine_out);
    std::vector<double> ref(rgb32.size());
    ref_film(T, rgb32.data(), static_cast<uint32_t>(npix), ref.data());
    print_chain_setup("film ", ref.data(), engine_out.data(), gold.data.data(),
                      ref.size());

    int rc = 0;
    rc |= run_case("golden",
                   [&](float* out) { return gpu_film(T, rgb32.data(),
                                                     static_cast<uint32_t>(npix), out); },
                   rgb32.data(), static_cast<uint32_t>(npix), ref.data(),
                   engine_out.data(), gold.data.data());

    // Sweep: 64^3 linear-ProPhoto lattice over [-0.05, 2.0] per channel —
    // sub-black through 1-stop-over highlights; the chromaticity path clamps
    // beyond that, and b scales linearly. 512x512 pixels.
    {
        const int G = 64;
        const double kLo = -0.05, kHi = 2.0;
        const int sw = 512, shh = 512;
        std::vector<float> sweep(static_cast<size_t>(G) * G * G * 3);
        size_t i = 0;
        for (int a = 0; a < G; ++a)
            for (int b = 0; b < G; ++b)
                for (int c = 0; c < G; ++c) {
                    sweep[i * 3 + 0] = static_cast<float>(kLo + (kHi - kLo) * a / (G - 1));
                    sweep[i * 3 + 1] = static_cast<float>(kLo + (kHi - kLo) * b / (G - 1));
                    sweep[i * 3 + 2] = static_cast<float>(kLo + (kHi - kLo) * c / (G - 1));
                    ++i;
                }
        const uint32_t nsw = static_cast<uint32_t>(G) * G * G;
        std::vector<double> sweep64(sweep.size());
        for (size_t k = 0; k < sweep.size(); ++k)
            sweep64[k] = static_cast<double>(sweep[k]);  // engine sees identical values
        std::vector<float> eng_sw;
        engine_film(T, film, sweep64.data(), sw, shh, &eng_sw);
        std::vector<double> ref_sw(sweep.size());
        ref_film(T, sweep.data(), nsw, ref_sw.data());
        rc |= run_case("sweep",
                       [&](float* out) { return gpu_film(T, sweep.data(), nsw, out); },
                       sweep.data(), nsw, ref_sw.data(), eng_sw.data(), nullptr);
    }

    // NaN input (recorded raw; the engine's own cubic path has no defined NaN
    // input semantics, so no engine column here).
    {
        const float qnan = std::nanf("");
        std::vector<float> nanc = {qnan, qnan, qnan, qnan, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
        std::vector<float> gpu(nanc.size(), -1.0f);
        if (!gpu_film(T, nanc.data(), 3, gpu.data())) {
            std::printf("CASE nan      GPU_FAIL\n");
            rc |= 1;
        } else {
            std::vector<double> rn(nanc.size());
            ref_film(T, nanc.data(), 3, rn.data());
            std::printf("CASE nan      (per-pixel raw values)\n");
            const char* label[3] = {"all-NaN ", "one-NaN ", "control "};
            for (int px = 0; px < 3; ++px)
                std::printf("  %s gpu=(%.9g, %.9g, %.9g) ref_f64=(%.9g, %.9g, %.9g)\n",
                            label[px], gpu[px * 3], gpu[px * 3 + 1], gpu[px * 3 + 2],
                            rn[px * 3], rn[px * 3 + 1], rn[px * 3 + 2]);
        }
    }
    return rc;
}

// ── print subcommand ────────────────────────────────────────────────────────

int do_print(const char* film_json, const char* paper_json, const char* filters_json,
             const char* spectra_npy, const std::string& golden_dir) {
    if (!probe::gpu_available()) {
        std::printf("PRINT GPU unavailable\n");
        return 1;
    }
    spk::Profile film = spk::load_profile_file(film_json);
    spk::Profile paper = spk::load_profile_file(paper_json);
    spk::NdArray spectra_lut = spk::load_npy(spectra_npy);
    double cc[3];
    PrintTables T = build_print_tables(film, paper, filters_json, spectra_lut, cc);
    std::printf("TABLES film=%s paper=%s n=%d nulled_bands=%d cc=(%.9g, %.9g, %.9g) midgray=%.9g cmax=(%.6g, %.6g, %.6g) nan_paper_curve=%d\n",
                film.stock.c_str(), paper.stock.c_str(), T.n, T.nulled_bands, cc[0],
                cc[1], cc[2], static_cast<double>(T.midgray), T.cmax[0], T.cmax[1],
                T.cmax[2], count_nan_f(T.paper_curve));

    spkvec::Array in = spkvec::read(golden_dir + "/film_density_cmy.spkvec");
    spkvec::Array gold = spkvec::read(golden_dir + "/print_density_cmy.spkvec");
    const int height = static_cast<int>(in.shape[0]);
    const int width = static_cast<int>(in.shape[1]);
    const int npix = width * height;

    std::vector<float> engine_out;
    engine_print(T, film, paper, in.data.data(), width, height, &engine_out);
    std::vector<double> ref(static_cast<size_t>(npix) * 3);
    ref_print(T, in.data.data(), static_cast<uint32_t>(npix), ref.data());
    print_chain_setup("print", ref.data(), engine_out.data(), gold.data.data(),
                      ref.size());

    int rc = 0;
    rc |= run_case("golden",
                   [&](float* out) { return gpu_print(T, in.data.data(),
                                                      static_cast<uint32_t>(npix), out); },
                   in.data.data(), static_cast<uint32_t>(npix), ref.data(),
                   engine_out.data(), gold.data.data());

    // Sweep: 64^3 film-density lattice, -0.1..nanmax(density_curves) per channel
    // (the engine's own print-input domain; the same bounds as the M1 scan sweep).
    {
        const int G = 64;
        const double kLo = -0.1;
        const int sw = 512, shh = 512;
        std::vector<float> sweep(static_cast<size_t>(G) * G * G * 3);
        size_t i = 0;
        for (int a = 0; a < G; ++a)
            for (int b = 0; b < G; ++b)
                for (int c = 0; c < G; ++c) {
                    sweep[i * 3 + 0] = static_cast<float>(kLo + (T.cmax[0] - kLo) * a / (G - 1));
                    sweep[i * 3 + 1] = static_cast<float>(kLo + (T.cmax[1] - kLo) * b / (G - 1));
                    sweep[i * 3 + 2] = static_cast<float>(kLo + (T.cmax[2] - kLo) * c / (G - 1));
                    ++i;
                }
        const uint32_t nsw = static_cast<uint32_t>(G) * G * G;
        std::vector<float> eng_sw;
        engine_print(T, film, paper, sweep.data(), sw, shh, &eng_sw);
        std::vector<double> ref_sw(sweep.size());
        ref_print(T, sweep.data(), nsw, ref_sw.data());
        rc |= run_case("sweep",
                       [&](float* out) { return gpu_print(T, sweep.data(), nsw, out); },
                       sweep.data(), nsw, ref_sw.data(), eng_sw.data(), nullptr);
    }

    // NaN density (engine: light = NaN -> 0 per band => a defined near-black
    // output; the shader has NO NaN guard — record what the GPU emits, with the
    // engine column for contrast).
    {
        const float qnan = std::nanf("");
        std::vector<float> nanc = {qnan, qnan, qnan, qnan, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
        std::vector<float> gpu(nanc.size(), -1.0f);
        if (!gpu_print(T, nanc.data(), 3, gpu.data())) {
            std::printf("CASE nan      GPU_FAIL\n");
            rc |= 1;
        } else {
            std::vector<double> rn(nanc.size());
            ref_print(T, nanc.data(), 3, rn.data());
            std::vector<float> en;
            engine_print(T, film, paper, nanc.data(), 3, 1, &en);
            std::printf("CASE nan      (per-pixel raw values; engine defines NaN density as light->0)\n");
            const char* label[3] = {"all-NaN ", "one-NaN ", "control "};
            for (int px = 0; px < 3; ++px)
                std::printf("  %s gpu=(%.9g, %.9g, %.9g) ref_f64=(%.9g, %.9g, %.9g) engine=(%.9g, %.9g, %.9g)\n",
                            label[px], gpu[px * 3], gpu[px * 3 + 1], gpu[px * 3 + 2],
                            rn[px * 3], rn[px * 3 + 1], rn[px * 3 + 2], en[px * 3],
                            en[px * 3 + 1], en[px * 3 + 2]);
        }
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 6 && std::strcmp(argv[1], "film") == 0)
        return do_film(argv[2], argv[3], argv[4], argv[5]);
    if (argc >= 7 && std::strcmp(argv[1], "print") == 0)
        return do_print(argv[2], argv[3], argv[4], argv[5], argv[6]);
    std::fprintf(stderr,
                 "usage: gpu_probe_m2 film  <film.json> <spectra.npy> <input_rgb.f64> <golden_dir>\n"
                 "       gpu_probe_m2 print <film.json> <paper.json> <filters.json> <spectra.npy> <golden_dir>\n");
    return 2;
}
