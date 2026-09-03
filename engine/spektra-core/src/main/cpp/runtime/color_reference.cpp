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
 * spektrafilm.
 */
#include "runtime/color_reference.h"

#include <cmath>
#include <vector>

#include "model/color_output.h"
#include "model/spectral.h"

namespace spk {

namespace {

// CIE Y at a single CMY density triple, on the given profile's dyes + resolved
// viewing illuminant. Mirrors scanning.py::cmy_to_log_xyz for the Y channel only, then
// the (10**log_xyz)[Y] round-trip the references take:
//   spectral[l] = sum_k cmy[k]*channel_density[l,k] + base_density[l]
//   light[l]    = 10^-spectral[l] * illum[l]   (NaN -> 0)
//   Y           = sum_l light[l] * ybar[l] / normalization
//   y           = 10 ** log10(max(Y,0) + 1e-10)
// (The black/white references only use the Y channel, so X/Z are skipped.)
double cmy_to_y(const Profile& prof, const double cmy[3]) {
    const int S = prof.n_samples;
    const ViewingIlluminant& viewing = prof.resolved_viewing_illuminant
        ? *prof.resolved_viewing_illuminant
        : require_viewing_illuminant(prof.viewing_illuminant);
    const float* illum = viewing.spectrum;
    const double inv_norm = 1.0 / viewing.normalization;
    double Y = 0.0;
    for (int l = 0; l < S; ++l) {
        const float* cd = prof.channel_density.data() + static_cast<size_t>(l) * 3;
        double spectral = cmy[0] * static_cast<double>(cd[0]) +
                          cmy[1] * static_cast<double>(cd[1]) +
                          cmy[2] * static_cast<double>(cd[2]) +
                          static_cast<double>(prof.base_density[l]);
        double w = std::pow(10.0, -spectral) * static_cast<double>(illum[l]);
        if (std::isnan(w)) w = 0.0;
        Y += w * kCieCmf1931[l][1];
    }
    // log10(max(Y,0)+1e-10) round-trip (matches the reference's 10**log_xyz).
    return std::pow(10.0, std::log10(std::fmax(Y * inv_norm, 0.0) + 1e-10));
}

// develop_simple for a single log-exposure 3-vector against the print density
// curves (emulsion.develop_simple): per channel, interp the log_exposure against
// (log_exposure / gamma, density_curves[:,k]) with endpoint clamping. Mirrors
// model/density_curves.cpp::interpolate_exposure_to_density for one pixel.
void develop_simple_one(const Profile& print_profile, const double log_raw[3],
                        float gamma, double cmy_out[3]) {
    const int N = print_profile.n_density_pts;
    const float* dc = print_profile.density_curves.data();   // (N*3,) [n*3+k]
    const float* le = print_profile.log_exposure.data();     // (N,)
    const double g = (gamma != 0.0f) ? static_cast<double>(gamma) : 1.0;
    for (int k = 0; k < 3; ++k) {
        const double x = log_raw[k];
        // axis[n] = log_exposure[n] / gamma (monotonic increasing). np.interp /
        // fast_interp clamps to the first/last density outside the axis range.
        if (x <= static_cast<double>(le[0]) / g) {
            cmy_out[k] = static_cast<double>(dc[0 * 3 + k]);
            continue;
        }
        if (x >= static_cast<double>(le[N - 1]) / g) {
            cmy_out[k] = static_cast<double>(dc[(N - 1) * 3 + k]);
            continue;
        }
        int lo = 0;
        for (int n = 1; n < N; ++n) {
            if (static_cast<double>(le[n]) / g >= x) { lo = n - 1; break; }
        }
        const double x0 = static_cast<double>(le[lo]) / g;
        const double x1 = static_cast<double>(le[lo + 1]) / g;
        const double y0 = static_cast<double>(dc[lo * 3 + k]);
        const double y1 = static_cast<double>(dc[(lo + 1) * 3 + k]);
        const double t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0;
        cmy_out[k] = y0 + t * (y1 - y0);
    }
}

// np.interp(x, xp, fp) with xp strictly increasing, clamped at the endpoints.
double np_interp(double x, const std::vector<double>& xp,
                 const std::vector<double>& fp) {
    const int n = static_cast<int>(xp.size());
    if (x <= xp[0]) return fp[0];
    if (x >= xp[n - 1]) return fp[n - 1];
    int lo = 0;
    for (int i = 1; i < n; ++i) {
        if (xp[i] >= x) { lo = i - 1; break; }
    }
    const double x0 = xp[lo], x1 = xp[lo + 1];
    const double f0 = fp[lo], f1 = fp[lo + 1];
    const double t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0;
    return f0 + t * (f1 - f0);
}

}  // namespace

double remove_srgb_cctf(double level) {
    // colour.cctf_decoding_sRGB (eotf_sRGB):
    //   threshold = eotf_inverse_sRGB(0.0031308) = 1.055*0.0031308^(1/2.4) - 0.055
    //   L = (V <= threshold) ? V/12.92 : ((V+0.055)/1.055)^2.4
    // then RGB_to_RGB(sRGB,sRGB) applies the (near-identity) matrix whose mean
    // row-sum is the constant below (numerical residue of the colourspace's own
    // RGB<->XYZ matrices); since the input is L*ones(3) and we take .mean(), the
    // net is L * MEAN_ROWSUM. MEAN_ROWSUM reproduces the c1d0e44 oracle exactly:
    //   _remove_sRGB_cctf(0.98) = 0.955131596857, (0.01) = 0.000774015686275.
    const double kMeanRowSum = 1.0000282666666667;
    const double threshold = 1.055 * std::pow(0.0031308, 1.0 / 2.4) - 0.055;
    double L = (level <= threshold) ? level / 12.92
                                    : std::pow((level + 0.055) / 1.055, 2.4);
    return L * kMeanRowSum;
}

ColorCorrection build_color_correction(double y_black, double y_white,
                                       double black_level, double white_level,
                                       bool black_correction,
                                       bool white_correction) {
    ColorCorrection c;
    c.active = black_correction || white_correction;
    if (!c.active) return c;  // strict no-op
    // _correction_fucntion: when only ONE side is corrected, the OTHER level is
    // pinned to its measured reference Y.
    if (black_correction && !white_correction) white_level = y_white;
    if (white_correction && !black_correction) black_level = y_black;
    c.m = (white_level - black_level) / (y_white - y_black + 1e-10);
    c.q = black_level - c.m * y_black;
    c.midgray_corrected = (0.184 - c.q) / c.m;
    return c;
}

void measure_scanfilm_references(const Profile& film, double* y_black,
                                 double* y_white) {
    // cmy_black = nanmax(film.density_curves, axis=0); cmy_white = 0.
    const int N = film.n_density_pts;
    double cmy_black[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (int n = 0; n < N; ++n) {
        const float* dc = film.density_curves.data() + static_cast<size_t>(n) * 3;
        for (int k = 0; k < 3; ++k) {
            double v = static_cast<double>(dc[k]);
            if (!std::isnan(v) && v > cmy_black[k]) cmy_black[k] = v;  // np.nanmax
        }
    }
    const double cmy_white[3] = {0.0, 0.0, 0.0};
    *y_black = cmy_to_y(film, cmy_black);
    *y_white = cmy_to_y(film, cmy_white);
}

void measure_print_references(const Profile& print_profile,
                              const double log_raw_print_black[3],
                              const double log_raw_print_white[3],
                              float density_curve_gamma,
                              double* y_black, double* y_white) {
    double cmy_black[3], cmy_white[3];
    develop_simple_one(print_profile, log_raw_print_black, density_curve_gamma,
                       cmy_black);
    develop_simple_one(print_profile, log_raw_print_white, density_curve_gamma,
                       cmy_white);
    *y_black = cmy_to_y(print_profile, cmy_black);
    *y_white = cmy_to_y(print_profile, cmy_white);
}

double exposure_correction_factor(const Profile& profile,
                                  const ColorCorrection& corr,
                                  bool filming_positive) {
    if (!corr.active) return 1.0;  // strict no-op
    const double density_midgray = -std::log10(0.184);
    const double density_midgray_corrected = -std::log10(corr.midgray_corrected);

    // density_curve_av = nanmean(density_curves, axis=1) (over the 3 CMY channels);
    // density_min_av   = nanmean(base_density).
    const int N = profile.n_density_pts;
    std::vector<double> density_curve_av(N);
    for (int n = 0; n < N; ++n) {
        const float* dc = profile.density_curves.data() + static_cast<size_t>(n) * 3;
        double sum = 0.0;
        int cnt = 0;
        for (int k = 0; k < 3; ++k) {
            double v = static_cast<double>(dc[k]);
            if (!std::isnan(v)) { sum += v; ++cnt; }
        }
        density_curve_av[n] = cnt > 0 ? sum / cnt : NAN;
    }
    double base_sum = 0.0;
    int base_cnt = 0;
    for (size_t i = 0; i < profile.base_density.size(); ++i) {
        double v = static_cast<double>(profile.base_density[i]);
        if (!std::isnan(v)) { base_sum += v; ++base_cnt; }
    }
    const double density_min_av = base_cnt > 0 ? base_sum / base_cnt : 0.0;

    std::vector<double> log_exposure(N);
    for (int n = 0; n < N; ++n)
        log_exposure[n] = static_cast<double>(profile.log_exposure[n]);

    if (filming_positive) {
        // filming.py (positive): le(d) = -interp(-(d - density_min_av),
        //   -density_curve_av, log_exposure); return 1 / 10**(le_corr - le_mid).
        std::vector<double> neg_curve_av(N);
        for (int n = 0; n < N; ++n) neg_curve_av[n] = -density_curve_av[n];
        // np.interp needs increasing xp; -density_curve_av is increasing for the
        // (decreasing-with-exposure) positive characteristic.
        double le_corr = -np_interp(-(density_midgray_corrected - density_min_av),
                                    neg_curve_av, log_exposure);
        double le_mid = -np_interp(-(density_midgray - density_min_av),
                                   neg_curve_av, log_exposure);
        double exposure_correction = std::pow(10.0, le_corr - le_mid);
        return 1.0 / exposure_correction;
    }
    // printing.py (negative paper): le(d) = interp(d - density_min_av,
    //   density_curve_av, log_exposure); return 10**(le_corr - le_mid).
    double le_corr = np_interp(density_midgray_corrected - density_min_av,
                               density_curve_av, log_exposure);
    double le_mid = np_interp(density_midgray - density_min_av,
                              density_curve_av, log_exposure);
    return std::pow(10.0, le_corr - le_mid);
}

}  // namespace spk
