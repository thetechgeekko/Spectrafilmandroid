/*
 * Spektrafilm for Android — host parity test for the scanning stage output
 * color spaces (Adobe RGB, ProPhoto RGB, ITU-R BT.2020, ACES2065-1, linear
 * sRGB, plus sRGB).
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 *
 * Loads the scan_portra golden input (film_density_cmy.spkvec) and the
 * kodak_portra_400 profile, runs the C++ scanning stage once per output color
 * space, and compares to the matching oracle reference
 * (tests/scan_portra_ref_<space>.spkvec, dumped by the Python engine — see
 * tests/gen_output_spaces_ref.py). Reports max_abs / rms and PASS/FAIL against
 * max_abs <= 1e-4, rms <= 1e-5.
 *
 * NaN handling: Adobe RGB's gamma encode produces NaN wherever the linear RGB
 * goes negative (gamut excursions), and np.clip preserves NaN. The C++ mirrors
 * this; the comparison therefore (a) requires the NaN layout to match exactly
 * and (b) measures max_abs/rms over the finite entries only.
 *
 * Build (host):
 *   g++ -std=c++17 -O2 \
 *     -I <cpp_root> -I <tools/parity> \
 *     tests/test_output_spaces.cpp \
 *     runtime/stages/scanning.cpp model/color_output.cpp model/emulsion.cpp \
 *     model/conversions.cpp model/spectral.cpp model/diffusion.cpp \
 *     kernels/gaussian.cpp kernels/exponential_filter.cpp profiles/profile.cpp \
 *     -o /tmp/test_output_spaces
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "spkvec_io.h"

#include "profiles/profile.h"
#include "runtime/stages/scanning.h"
#include "spektra.h"

namespace {

const char* kProfilePath =
    "/home/user/spektrafilm/src/spektrafilm/data/profiles/kodak_portra_400.json";
const char* kGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/scan_portra";
const char* kRefDir =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests";

struct SpaceCase {
    spk_color_space cs;
    bool cctf;
    const char* label;  // matches scan_portra_ref_<label>.spkvec
};

const SpaceCase kCases[] = {
    {SPK_CS_SRGB,        true,  "srgb"},
    {SPK_CS_ADOBE_RGB,   true,  "adobe_rgb"},
    {SPK_CS_PROPHOTO,    true,  "prophoto"},
    {SPK_CS_REC2020,     true,  "rec2020"},
    {SPK_CS_ACES2065_1,  true,  "aces2065_1"},
    {SPK_CS_LINEAR_SRGB, false, "linear_srgb"},
};

}  // namespace

int main(int argc, char** argv) {
    std::string profile_path = argc > 1 ? argv[1] : kProfilePath;
    std::string golden_dir = argc > 2 ? argv[2] : kGoldenDir;
    std::string ref_dir = argc > 3 ? argv[3] : kRefDir;

    spk::Profile film = spk::load_profile_file(profile_path);
    std::printf("Loaded profile: type=%s viewing_illuminant=%s samples=%d\n",
                film.type.c_str(), film.viewing_illuminant.c_str(),
                film.n_samples);

    spkvec::Array in = spkvec::read(golden_dir + "/film_density_cmy.spkvec");
    if (in.shape.size() != 3 || in.shape[2] != 3) {
        std::fprintf(stderr, "unexpected input shape\n");
        return 2;
    }
    const int height = static_cast<int>(in.shape[0]);
    const int width = static_cast<int>(in.shape[1]);
    std::printf("Input image: %dx%d x3 (%zu floats)\n", width, height,
                in.data.size());

    const double tol_max_abs = 1e-4;
    const double tol_rms = 1e-5;
    bool all_pass = true;

    for (const SpaceCase& sc : kCases) {
        spkvec::Array gold =
            spkvec::read(ref_dir + "/scan_portra_ref_" + sc.label + ".spkvec");

        std::vector<float> rgb(in.data.size());
        spk::ScanningParams params;  // scan_film, spatial off (defaults)
        params.output_color_space = sc.cs;
        params.output_cctf_encoding = sc.cctf;
        spk::scan(film, params, in.data.data(), width, height, rgb.data());

        if (rgb.size() != gold.data.size()) {
            std::fprintf(stderr, "%s: size mismatch got %zu golden %zu\n",
                         sc.label, rgb.size(), gold.data.size());
            all_pass = false;
            continue;
        }

        double max_abs = 0.0, sse = 0.0;
        size_t argmax = 0, n_finite = 0, nan_layout_mismatch = 0, nan_count = 0;
        for (size_t i = 0; i < rgb.size(); ++i) {
            bool a_nan = std::isnan(rgb[i]);
            bool b_nan = std::isnan(gold.data[i]);
            if (a_nan != b_nan) ++nan_layout_mismatch;
            if (a_nan) ++nan_count;
            if (a_nan || b_nan) continue;
            double d = std::fabs(static_cast<double>(rgb[i]) -
                                 static_cast<double>(gold.data[i]));
            if (d > max_abs) { max_abs = d; argmax = i; }
            sse += d * d;
            ++n_finite;
        }
        double rms = n_finite ? std::sqrt(sse / static_cast<double>(n_finite))
                              : 0.0;
        bool pass = (nan_layout_mismatch == 0) && (max_abs <= tol_max_abs) &&
                    (rms <= tol_rms);
        all_pass = all_pass && pass;

        std::printf(
            "[%-11s] max_abs=%.6e rms=%.6e nan_count=%zu "
            "nan_layout_mismatch=%zu worst(idx=%zu got=%.8f gold=%.8f) %s\n",
            sc.label, max_abs, rms, nan_count, nan_layout_mismatch, argmax,
            rgb[argmax], gold.data[argmax], pass ? "PASS" : "FAIL");
    }

    // Both cinema-print stocks declare K75P. Gate every output-space matrix on
    // an isolated print-density scan, not only the full-route sRGB fixture. The
    // references are generated by the pinned c1 oracle from the committed
    // float32 print-density taps (gen_viewing_illuminant_visuals.py).
    const std::filesystem::path asset_root =
        std::filesystem::path(profile_path).parent_path().parent_path();
    const std::filesystem::path goldens_root =
        std::filesystem::path(golden_dir).parent_path();
    const char* k75p_stocks[] = {"kodak_2383", "kodak_2393"};
    for (const char* stock : k75p_stocks) {
        spk::Profile print = spk::load_profile_file(
            (asset_root / "profiles" / (std::string(stock) + ".json")).string());
        const std::filesystem::path case_dir =
            goldens_root / (std::string("print_") + stock + "_k75p");
        spkvec::Array density =
            spkvec::read((case_dir / "print_density_cmy.spkvec").string());
        const int kh = static_cast<int>(density.shape[0]);
        const int kw = static_cast<int>(density.shape[1]);

        for (const SpaceCase& sc : kCases) {
            spkvec::Array gold = spkvec::read(
                (case_dir / (std::string("scan_ref_") + sc.label + ".spkvec"))
                    .string());
            std::vector<float> rgb(density.data.size()), repeat(density.data.size());
            spk::ScanningParams params;
            params.scan_film = false;
            params.output_color_space = sc.cs;
            params.output_cctf_encoding = sc.cctf;
            spk::scan(print, params, density.data.data(), kw, kh, rgb.data());
            spk::scan(print, params, density.data.data(), kw, kh, repeat.data());

            double max_abs = 0.0, sse = 0.0;
            size_t argmax = 0, n_finite = 0, nan_layout_mismatch = 0;
            for (size_t i = 0; i < rgb.size(); ++i) {
                const bool a_nan = std::isnan(rgb[i]);
                const bool b_nan = std::isnan(gold.data[i]);
                if (a_nan != b_nan) ++nan_layout_mismatch;
                if (a_nan || b_nan) continue;
                const double d = std::fabs(static_cast<double>(rgb[i]) -
                                           static_cast<double>(gold.data[i]));
                if (d > max_abs) { max_abs = d; argmax = i; }
                sse += d * d;
                ++n_finite;
            }
            const double rms = n_finite
                ? std::sqrt(sse / static_cast<double>(n_finite)) : 0.0;
            const bool deterministic =
                std::memcmp(rgb.data(), repeat.data(),
                            rgb.size() * sizeof(float)) == 0;
            const bool pass = nan_layout_mismatch == 0 &&
                              max_abs <= tol_max_abs && rms <= tol_rms &&
                              deterministic;
            all_pass = all_pass && pass;
            std::printf("[K75P %-10s/%-11s] max_abs=%.6e rms=%.6e "
                        "nan_mismatch=%zu repeat_exact=%d worst=%zu -> %s\n",
                        stock, sc.label, max_abs, rms, nan_layout_mismatch,
                        deterministic ? 1 : 0, argmax, pass ? "PASS" : "FAIL");
        }
    }

    std::printf("%s\n", all_pass ? "ALL PASS" : "SOME FAIL");
    return all_pass ? 0 : 1;
}
