/*
 * Spektrafilm for Android — native engine: print-route digest implementation.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements runtime/print_digest.h.
 */
#include "runtime/print_digest.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#include "kernels/spectral_upsampling.h"
#include "model/density_curves.h"
#include "profiles/json_min.h"

namespace spk {

namespace {

// sRGB (linear) -> XYZ with CAT02 chromatic adaptation to the film's reference
// illuminant (D55), apply_cctf_decoding=False. This is the transform
// _simple_rgb_to_density_spectral takes for the midgray gray: it calls
// _rgb_to_film_raw(rgb) with the method-default color_space='sRGB' (NOT the
// io.input_color_space used by the main expose path, which is ProPhoto RGB).
// Baked at full double precision from colour.RGB_to_XYZ (the transform is exactly
// linear). Row-major 3x3: xyz[r] = sum_c M[r][c] * rgb[c].
constexpr double kSRGBToXyzD55[3][3] = {
    {0.42623421365907455, 0.37571635923964258, 0.15488745710403823},
    {0.21812651506394931, 0.71977726742782511, 0.062097551400824694},
    {0.015618316763516405, 0.099341902396424583, 0.8063581952787553},
};

inline double clip01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// _rgb_to_tc_b for color_space='sRGB', apply_cctf_decoding=False,
// reference_illuminant='D55'.
void srgb_rgb_to_tc_b(const double rgb[3], Vec2* out_tc, double* out_b) {
    double xyz[3];
    for (int r = 0; r < 3; ++r) {
        xyz[r] = kSRGBToXyzD55[r][0] * rgb[0] + kSRGBToXyzD55[r][1] * rgb[1] +
                 kSRGBToXyzD55[r][2] * rgb[2];
    }
    double b = xyz[0] + xyz[1] + xyz[2];
    double denom = std::fmax(b, 1e-10);
    Vec2 xy{clip01(xyz[0] / denom), clip01(xyz[1] / denom)};
    *out_tc = tri2quad(xy);
    *out_b = std::isnan(b) ? 0.0 : b;
}

}  // namespace

bool resolve_neutral_cc_string(const std::string& json_text,
                               const std::string& print_stock,
                               const std::string& illuminant,
                               const std::string& film_stock,
                               double cc_out[3]) {
    if (cc_out == nullptr) return false;
    try {
        const json::ValuePtr root = json::parse(json_text);
        const json::Value& r = *root;

        // filters.get(print_stock, {}).get(illuminant, {}).get(film_stock)
        if (!r.has(print_stock)) return false;
        const json::Value& by_illum = r.at(print_stock);
        if (!by_illum.has(illuminant)) return false;
        const json::Value& by_film = by_illum.at(illuminant);
        if (!by_film.has(film_stock)) return false;
        const json::Value& triple = by_film.at(film_stock);
        if (!triple.is_array() || triple.size() != 3) return false;

        // Validate into private storage first. No syntax/schema/allocation error
        // may partially publish a replacement triple to the caller.
        std::array<double, 3> parsed{};
        for (size_t i = 0; i < parsed.size(); ++i) {
            if (!triple[i].is_number() || triple[i].is_null_number) return false;
            parsed[i] = triple[i].number;
            if (!std::isfinite(parsed[i])) return false;
        }
        for (size_t i = 0; i < parsed.size(); ++i) cc_out[i] = parsed[i];
        return true;
    } catch (...) {
        // Existing bool API: malformed input and allocation failures are
        // represented as false, with caller storage left byte-for-byte alone.
        return false;
    }
}

bool resolve_neutral_cc(const std::string& json_path,
                        const std::string& print_stock,
                        const std::string& illuminant,
                        const std::string& film_stock,
                        double cc_out[3]) {
    if (cc_out == nullptr) return false;
    try {
        std::ifstream in(json_path, std::ios::binary);
        if (!in) return false;
        in.seekg(0, std::ios::end);
        const std::streamoff end = in.tellg();
        if (end < 0 || static_cast<uintmax_t>(end) > json::kMaxInputBytes)
            return false;
        const size_t size = static_cast<size_t>(end);
        std::string text(size, '\0');
        in.seekg(0, std::ios::beg);
        if (size != 0) {
            in.read(text.data(), static_cast<std::streamsize>(size));
            if (!in || static_cast<size_t>(in.gcount()) != size) return false;
        }
        if (in.peek() != std::char_traits<char>::eof()) return false;
        return resolve_neutral_cc_string(text, print_stock, illuminant, film_stock,
                                         cc_out);
    } catch (...) {
        return false;
    }
}

namespace {

// _exposure_factor for a single neutral gray value `gray` (FilmingStage's
// rgb=[gray]*3), mirroring filming.py::_simple_rgb_to_density_spectral fed through
// printing.py::_exposure_factor. Returns 1 / geomean_k(raw_midgray_k). All NaN
// dye/illuminant/sensitivity entries are zeroed exactly as the Python pipeline.
double exposure_factor_for_gray(const Profile& film, const Profile& print_profile,
                                const NdArray& filming_tc_lut,
                                const double* filtered_illuminant, float gamma,
                                double gray) {
    const int S = film.n_samples;  // 81

    // 1) Filming midgray: rgb=[gray]*3 -> film raw (sRGB tc transform) -> log10.
    const double rgb_mid[3] = {gray, gray, gray};
    Vec2 tc;
    double b;
    srgb_rgb_to_tc_b(rgb_mid, &tc, &b);
    const int L = filming_tc_lut.shape[0];
    const double scale = static_cast<double>(L - 1);
    double raw[3];
    cubic_interp_lut_at_2d(filming_tc_lut, tc.x * scale, tc.y * scale, raw);
    float log_raw[3];
    for (int c = 0; c < 3; ++c) {
        double r = raw[c] * b;
        log_raw[c] = static_cast<float>(std::log10(r + 1e-10));
    }

    // 2) develop_simple against the RAW film density curves (no nanmin
    //    normalisation, no DIR couplers), gamma broadcast to all channels.
    float density_cmy[3];
    interpolate_exposure_to_density(log_raw, /*npix=*/1,
                                    film.density_curves.data(),
                                    film.log_exposure.data(),
                                    film.n_density_pts, gamma, density_cmy);
    const double c0 = static_cast<double>(density_cmy[0]);
    const double c1 = static_cast<double>(density_cmy[1]);
    const double c2 = static_cast<double>(density_cmy[2]);

    // 3) _exposure_factor against the print sensitivity + filtered illuminant.
    //    density_spectral[l] = c . channel_density[l] + base_density[l]
    //    light[l]            = 10^-density_spectral[l] * filtered_illum[l]  (NaN->0)
    //    raw_k               = sum_l light[l] * print_sens[l,k]
    //    raw_k               = fmax(raw_k, 1e-10)
    //    geomean             = exp(mean_k log(raw_k))
    //    factor              = 1 / geomean
    double rawk[3] = {0.0, 0.0, 0.0};
    for (int l = 0; l < S; ++l) {
        const float* cd =
            film.channel_density.data() + static_cast<size_t>(l) * 3;
        const double spectral = c0 * static_cast<double>(cd[0]) +
                                c1 * static_cast<double>(cd[1]) +
                                c2 * static_cast<double>(cd[2]) +
                                static_cast<double>(film.base_density[l]);
        double light = std::pow(10.0, -spectral) * filtered_illuminant[l];
        if (std::isnan(light)) light = 0.0;
        const float* ls =
            print_profile.log_sensitivity.data() + static_cast<size_t>(l) * 3;
        for (int k = 0; k < 3; ++k) {
            double sens = std::pow(10.0, static_cast<double>(ls[k]));
            if (std::isnan(sens)) sens = 0.0;  // np.nan_to_num
            rawk[k] += light * sens;
        }
    }
    double log_sum = 0.0;
    for (int k = 0; k < 3; ++k) {
        double v = std::fmax(rawk[k], 1e-10);
        log_sum += std::log(v);
    }
    double geomean = std::exp(log_sum / 3.0);
    return 1.0 / geomean;
}

}  // namespace

double compute_midgray_exposure_factor(const Profile& film,
                                       const Profile& print_profile,
                                       const NdArray& filming_tc_lut,
                                       const double* filtered_illuminant,
                                       float gamma, double exposure_compensation_ev,
                                       bool normalize_print_exposure,
                                       bool print_exposure_compensation) {
    // Mirror PrintingStage._compute_exposure_factor_midgray (printing.py
    // c1d0e44 L104-118), with the two midgray densities from
    // FilmingStage._compute_density_spectral_midgray_to_balance_print
    // (filming.py c1d0e44 L125-134):
    //   density_spectral_midgray      <- rgb = 0.184 gray (always)
    //   density_spectral_midgray_comp <- rgb = 0.184 * 2^exposure_compensation_ev
    //                                    (only when print_exposure_compensation)
    // factor_midgray      = _exposure_factor(...density_spectral_midgray)
    // factor_midgray_comp = _exposure_factor(...density_spectral_midgray_comp)
    //                       (else 1.0)
    // and the 4-case branch on (print_exposure_compensation, normalize_print_exposure):
    //   comp && !normalize : factor_midgray_comp / factor_midgray
    //   normalize && comp  : factor_midgray_comp
    //   normalize && !comp : factor_midgray
    //   else               : 1.0
    // CRITICAL: at exposure_compensation_ev == 0 the compensated gray equals the
    // uncompensated gray, so factor_midgray_comp == factor_midgray bit-for-bit and
    // every existing (EV=0) print golden is byte-identical to before.
    const double factor_midgray = exposure_factor_for_gray(
        film, print_profile, filming_tc_lut, filtered_illuminant, gamma, 0.184);

    double factor_midgray_comp = 1.0;
    if (print_exposure_compensation) {
        const double gray_comp = 0.184 * std::pow(2.0, exposure_compensation_ev);
        factor_midgray_comp = exposure_factor_for_gray(
            film, print_profile, filming_tc_lut, filtered_illuminant, gamma,
            gray_comp);
    }

    if (print_exposure_compensation && !normalize_print_exposure) {
        return factor_midgray_comp / factor_midgray;
    } else if (normalize_print_exposure && print_exposure_compensation) {
        return factor_midgray_comp;
    } else if (normalize_print_exposure && !print_exposure_compensation) {
        return factor_midgray;
    } else {
        return 1.0;
    }
}

}  // namespace spk
