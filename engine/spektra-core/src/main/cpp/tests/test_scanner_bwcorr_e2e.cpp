/*
 * Spektrafilm for Android — END-TO-END host parity test for the SCANNER
 * BLACK/WHITE corrections (scanner.black_correction / white_correction +
 * black_level / white_level), run through the WHOLE pipeline on BOTH routes that
 * fire them.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The four scanner params drive runtime/services/color_reference.py
 * (ColorReferenceService): a scan-time tone anchor that maps the measured CIE Y at
 * the reference BLACK and WHITE densities to the (sRGB-decoded) target levels via
 * the affine y_corrected = clip(m*Y + q, 0, 1), and applies it in two ways:
 *   - an exposure correction back-applied to the print (or filming) raw, and
 *   - a per-pixel XYZ rescale in scanning (xyz *= y_corrected / (Y + 1e-10)).
 *
 * The correction is route-gated, so this test covers BOTH non-trivial routes:
 *
 *   CASE A — PRINT route (portra -> endura, NEGATIVE paper) vs the
 *   print_portra_bwcorr golden. Fires the PRINTING exposure correction (changes
 *   print_density_cmy) + the SCANNING XYZ correction (changes final_rgb). The
 *   FILM taps are UNTOUCHED (the filming exposure correction is 1.0 for negative
 *   film), verified against the no-correction print_portra golden.
 *
 *   CASE B — scan_film route on a POSITIVE film (provia) vs the scan_provia_bwcorr
 *   golden. Fires the FILMING exposure correction (changes film_log_raw /
 *   film_density_cmy) + the SCANNING XYZ correction (changes final_rgb).
 *
 * Each case also asserts that turning the corrections OFF (everything else equal)
 * changes the output, proving they are genuinely active end to end. Both cases use
 * the c1d0e44 goldens (max_abs <= 1e-4, rms <= 1e-5).
 *
 *   CASES C/D — Kodak 2383 and 2393 print routes. Their committed no-correction
 *   K75P goldens anchor the input/baseline; enabling both corrections must change
 *   print density and final RGB, stay finite, and be exactly deterministic. This
 *   exercises the K75P reference-measurement seam without inventing a new oracle
 *   for corrected pixels.
 *
 * Build (host) — full source set, run from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I <cpp_root> -I <tools/parity> \
 *     tests/test_scanner_bwcorr_e2e.cpp spektra.cpp \
 *     model/*.cpp kernels/*.cpp io/*.cpp profiles/*.cpp \
 *     runtime/*.cpp runtime/stages/*.cpp \
 *     -o /tmp/test_scanner_bwcorr_e2e
 * Run (argv[4] optionally overrides the goldens ROOT for a git worktree):
 *   /tmp/test_scanner_bwcorr_e2e <asset_dir> <print_portra_bwcorr_golden_dir> \
 *     <input.f64> [goldens_root]
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "spkvec_io.h"
#include "model/color_output.h"
#include "profiles/profile.h"
#include "runtime/color_reference.h"
#include "spektra.h"

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kGoldensRoot = "/home/user/Spectrafilmandroid/tools/parity/goldens";
const char* kInputF64 =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

bool check(const char* label, const float* got, const std::vector<float>& gold) {
    double max_abs = 0.0, sse = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < gold.size(); ++i) {
        double d = std::fabs(static_cast<double>(got[i]) -
                             static_cast<double>(gold[i]));
        if (d > max_abs) { max_abs = d; argmax = i; }
        sse += d * d;
    }
    double rms = std::sqrt(sse / static_cast<double>(gold.size()));
    const double tol_max_abs = 1e-4, tol_rms = 1e-5;
    bool pass = (max_abs <= tol_max_abs) && (rms <= tol_rms);
    std::printf("[%s] max_abs=%.6e (tol %.0e) rms=%.6e (tol %.0e) "
                "worst idx=%zu got=%.8f gold=%.8f -> %s\n",
                label, max_abs, tol_max_abs, rms, tol_rms, argmax,
                got[argmax], gold[argmax], pass ? "PASS" : "FAIL");
    return pass;
}

double max_abs_diff(const float* a, const float* b, size_t n) {
    double m = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > m) m = d;
    }
    return m;
}

bool all_finite(const float* a, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(a[i])) return false;
    }
    return true;
}

void set_base(spk_params* p, const char* film, const char* print, int scan_film) {
    p->film_profile = film;
    p->print_profile = print;
    spk_default_params(p);
    p->exposure_compensation_ev = 0.0f;
    p->auto_exposure = 0;
    p->density_curve_gamma = 1.0f;
    p->grain_active = 0;
    p->halation_active = 0;     // spatial branch OFF (deactivate_spatial_effects=True)
    // Spatial effects are per-effect gated (zero = inert); express the
    // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
    p->dir_diffusion_size_um = 0.0f;
    p->scanner_unsharp[0] = 0.0f;
    p->scanner_unsharp[1] = 0.0f;
    p->dir_couplers_active = 1;
    p->glare_active = 0;
    p->scan_film = scan_film;
    p->output_color_space = SPK_CS_SRGB;
    p->output_cctf_encoding = 1;
    p->rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p->preview_max_size = 640;
}

// Read the committed input image, sized to a golden's shape.
std::vector<double> load_input(const std::string& path, int npix) {
    std::vector<double> rgb64(static_cast<size_t>(npix) * 3);
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    in.read(reinterpret_cast<char*>(rgb64.data()),
            static_cast<std::streamsize>(rgb64.size() * sizeof(double)));
    if (in.gcount() != static_cast<std::streamsize>(rgb64.size() * sizeof(double))) {
        std::fprintf(stderr, "input size mismatch\n"); std::exit(2);
    }
    return rgb64;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir = argc > 1 ? argv[1] : kAssetDir;
    std::string input_path = argc > 3 ? argv[3] : kInputF64;
    std::string goldens_root = kGoldensRoot;
    // argv[2] is the print_portra_bwcorr golden dir (CI convention); argv[4] (if
    // present) overrides the goldens ROOT for a git worktree. Derive both case dirs
    // from the root so a single argv covers both.
    if (argc > 4) goldens_root = argv[4];
    else if (argc > 2) {
        std::string g2 = argv[2];
        size_t slash = g2.find_last_of('/');
        if (slash != std::string::npos) goldens_root = g2.substr(0, slash);
    }
    const std::string print_dir = goldens_root + "/print_portra_bwcorr";
    const std::string print_ref_dir = goldens_root + "/print_portra";  // corr OFF
    const std::string scan_dir = goldens_root + "/scan_provia_bwcorr";
    const std::string print_2383_dir =
        goldens_root + "/print_kodak_2383_k75p";
    const std::string print_2393_dir =
        goldens_root + "/print_kodak_2393_k75p";

    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(asset_dir.c_str(), &eng);
    if (st != SPK_OK) {
        std::fprintf(stderr, "engine create failed: %s\n", spk_status_str(st));
        return 2;
    }

    bool all = true;

    // Reference-service discriminator: hold every profile table and reference
    // log-raw sample fixed, changing only the resolved viewing illuminant. This
    // fails if measure_print_references silently substitutes D50 for K75P.
    auto check_reference_illuminant = [&](const char* stock) {
        spk::Profile k75p = spk::load_profile_file(
            asset_dir + "/profiles/" + stock + ".json");
        spk::Profile d50 = k75p;
        d50.viewing_illuminant = "D50";
        d50.resolved_viewing_illuminant =
            spk::find_viewing_illuminant("D50");
        const double log_raw_black[3] = {2.0, 2.0, 2.0};
        const double log_raw_white[3] = {-4.0, -4.0, -4.0};
        double kb = 0.0, kw = 0.0, kb_repeat = 0.0, kw_repeat = 0.0;
        double db = 0.0, dw = 0.0;
        spk::measure_print_references(k75p, log_raw_black, log_raw_white,
                                      1.0f, &kb, &kw);
        spk::measure_print_references(k75p, log_raw_black, log_raw_white,
                                      1.0f, &kb_repeat, &kw_repeat);
        spk::measure_print_references(d50, log_raw_black, log_raw_white,
                                      1.0f, &db, &dw);
        const double delta = std::fmax(std::fabs(kb - db), std::fabs(kw - dw));
        const bool repeat = kb == kb_repeat && kw == kw_repeat;
        const bool finite = std::isfinite(kb) && std::isfinite(kw) &&
                            std::isfinite(db) && std::isfinite(dw);
        const bool discriminating = delta > 1e-4;
        std::printf("[%s reference viewing-only K75P vs D50] max_delta=%.6e "
                    "repeat=%s finite=%s -> %s\n",
                    stock, delta, repeat ? "yes" : "NO",
                    finite ? "yes" : "NO",
                    (repeat && finite && discriminating) ? "PASS" : "FAIL");
        return repeat && finite && discriminating;
    };
    all &= check_reference_illuminant("kodak_2383");
    all &= check_reference_illuminant("kodak_2393");

    // =======================================================================
    // CASE A — PRINT route (portra -> endura), corrections ON.
    // =======================================================================
    {
        spkvec::Array gold_rgb = spkvec::read(print_dir + "/final_rgb.spkvec");
        spkvec::Array gold_pcmy = spkvec::read(print_dir + "/print_density_cmy.spkvec");
        // Film taps are UNCHANGED vs the no-correction print_portra golden (the
        // filming exposure correction is 1.0 for negative film).
        spkvec::Array gold_logr = spkvec::read(print_ref_dir + "/film_log_raw.spkvec");
        spkvec::Array gold_fcmy = spkvec::read(print_ref_dir + "/film_density_cmy.spkvec");
        const int height = static_cast<int>(gold_rgb.shape[0]);
        const int width = static_cast<int>(gold_rgb.shape[1]);
        const int npix = width * height;
        std::printf("CASE A (print route) %dx%dx3\n", width, height);
        std::vector<double> rgb64 = load_input(input_path, npix);
        std::vector<float> rgb32(rgb64.begin(), rgb64.end());
        spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};

        spk_params p{};
        set_base(&p, "kodak_portra_400", "kodak_portra_endura", /*scan_film=*/0);
        p.scanner_white_correction = 1;
        p.scanner_black_correction = 1;
        p.scanner_white_level = 0.98f;
        p.scanner_black_level = 0.01f;

        spk_image tap_pcmy{};
        st = spk_simulate_tap(eng, &in_img, &p, "print_density_cmy", &tap_pcmy);
        if (st == SPK_OK) {
            all &= check("A print_density_cmy", tap_pcmy.data, gold_pcmy.data);
            spk_image_free(&tap_pcmy);
        } else { std::fprintf(stderr, "tap pcmy failed\n"); all = false; }

        spk_image tap_logr{}, tap_fcmy{};
        if (spk_simulate_tap(eng, &in_img, &p, "film_log_raw", &tap_logr) == SPK_OK) {
            all &= check("A film_log_raw (unchanged)", tap_logr.data, gold_logr.data);
            spk_image_free(&tap_logr);
        } else { all = false; }
        if (spk_simulate_tap(eng, &in_img, &p, "film_density_cmy", &tap_fcmy) == SPK_OK) {
            all &= check("A film_density_cmy (unchanged)", tap_fcmy.data, gold_fcmy.data);
            spk_image_free(&tap_fcmy);
        } else { all = false; }

        spk_image out{};
        if (spk_simulate(eng, &in_img, &p, &out) != SPK_OK) { all = false; }
        else {
            all &= check("A final_rgb", out.data, gold_rgb.data);
            // Corrections OFF must change the output.
            spk_params p_off = p;
            p_off.scanner_white_correction = 0;
            p_off.scanner_black_correction = 0;
            spk_image out_off{};
            if (spk_simulate(eng, &in_img, &p_off, &out_off) == SPK_OK) {
                double d = max_abs_diff(out.data, out_off.data,
                                        static_cast<size_t>(npix) * 3);
                bool active = d > 1e-4;
                std::printf("[A on vs off] max_abs=%.6e -> %s\n", d,
                            active ? "ACTIVE" : "INERT?!");
                all &= active;
                spk_image_free(&out_off);
            } else all = false;
            spk_image_free(&out);
        }
    }

    // =======================================================================
    // CASE B — scan_film route on a POSITIVE film (provia), corrections ON.
    // =======================================================================
    {
        spkvec::Array gold_rgb = spkvec::read(scan_dir + "/final_rgb.spkvec");
        spkvec::Array gold_logr = spkvec::read(scan_dir + "/film_log_raw.spkvec");
        spkvec::Array gold_fcmy = spkvec::read(scan_dir + "/film_density_cmy.spkvec");
        const int height = static_cast<int>(gold_rgb.shape[0]);
        const int width = static_cast<int>(gold_rgb.shape[1]);
        const int npix = width * height;
        std::printf("CASE B (scan_film positive) %dx%dx3\n", width, height);
        std::vector<double> rgb64 = load_input(input_path, npix);
        std::vector<float> rgb32(rgb64.begin(), rgb64.end());
        spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};

        spk_params p{};
        set_base(&p, "fujifilm_provia_100f", "kodak_portra_endura", /*scan_film=*/1);
        // DIR couplers OFF: the positive-film coupler branch is a separate, un-gated
        // native code path; keeping it off isolates the scanner correction under test
        // (matches the scan_provia_bwcorr golden, which was generated couplers-off).
        p.dir_couplers_active = 0;
        p.scanner_white_correction = 1;
        p.scanner_black_correction = 1;
        p.scanner_white_level = 0.98f;
        p.scanner_black_level = 0.01f;

        // The FILMING exposure correction changes the film taps on this route.
        spk_image tap_logr{}, tap_fcmy{};
        if (spk_simulate_tap(eng, &in_img, &p, "film_log_raw", &tap_logr) == SPK_OK) {
            all &= check("B film_log_raw", tap_logr.data, gold_logr.data);
            spk_image_free(&tap_logr);
        } else { all = false; }
        if (spk_simulate_tap(eng, &in_img, &p, "film_density_cmy", &tap_fcmy) == SPK_OK) {
            all &= check("B film_density_cmy", tap_fcmy.data, gold_fcmy.data);
            spk_image_free(&tap_fcmy);
        } else { all = false; }

        spk_image out{};
        if (spk_simulate(eng, &in_img, &p, &out) != SPK_OK) { all = false; }
        else {
            all &= check("B final_rgb", out.data, gold_rgb.data);
            spk_params p_off = p;
            p_off.scanner_white_correction = 0;
            p_off.scanner_black_correction = 0;
            spk_image out_off{};
            if (spk_simulate(eng, &in_img, &p_off, &out_off) == SPK_OK) {
                double d = max_abs_diff(out.data, out_off.data,
                                        static_cast<size_t>(npix) * 3);
                bool active = d > 1e-4;
                std::printf("[B on vs off] max_abs=%.6e -> %s\n", d,
                            active ? "ACTIVE" : "INERT?!");
                all &= active;
                spk_image_free(&out_off);
            } else all = false;
            spk_image_free(&out);
        }
    }

    // =======================================================================
    // CASES C/D — K75P print-reference measurement on Kodak 2383 and 2393.
    // =======================================================================
    auto check_k75p_reference = [&](const char* case_label, const char* stock,
                                    const std::string& golden_dir) {
        spkvec::Array gold_rgb =
            spkvec::read(golden_dir + "/final_rgb.spkvec");
        spkvec::Array gold_pcmy =
            spkvec::read(golden_dir + "/print_density_cmy.spkvec");
        const int height = static_cast<int>(gold_rgb.shape[0]);
        const int width = static_cast<int>(gold_rgb.shape[1]);
        const int npix = width * height;
        const size_t n = static_cast<size_t>(npix) * 3;
        if (gold_rgb.data.size() != n || gold_pcmy.data.size() != n ||
            gold_pcmy.shape != gold_rgb.shape) {
            std::fprintf(stderr, "%s K75P golden shape mismatch\n", case_label);
            return false;
        }
        std::printf("CASE %s (print route %s / K75P) %dx%dx3\n", case_label,
                    stock, width, height);
        std::vector<double> rgb64 = load_input(input_path, npix);
        std::vector<float> rgb32(rgb64.begin(), rgb64.end());
        spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};

        spk_params p_off{};
        set_base(&p_off, "kodak_portra_400", stock, /*scan_film=*/0);

        bool ok = true;
        spk_image pcmy_off{};
        if (spk_simulate_tap(eng, &in_img, &p_off, "print_density_cmy",
                             &pcmy_off) == SPK_OK) {
            if (pcmy_off.width != width || pcmy_off.height != height) {
                spk_image_free(&pcmy_off);
                return false;
            }
            std::string label =
                std::string(case_label) + " no-correction print_density_cmy";
            ok &= check(label.c_str(), pcmy_off.data, gold_pcmy.data);
            spk_image_free(&pcmy_off);
        } else {
            std::fprintf(stderr, "%s baseline print-density render failed\n",
                         case_label);
            ok = false;
        }

        spk_image rgb_off{};
        if (spk_simulate(eng, &in_img, &p_off, &rgb_off) == SPK_OK) {
            if (rgb_off.width != width || rgb_off.height != height) {
                spk_image_free(&rgb_off);
                return false;
            }
            std::string label =
                std::string(case_label) + " no-correction K75P final_rgb";
            ok &= check(label.c_str(), rgb_off.data, gold_rgb.data);
        } else {
            std::fprintf(stderr, "%s baseline RGB render failed\n", case_label);
            return false;
        }

        spk_params p_on = p_off;
        p_on.scanner_white_correction = 1;
        p_on.scanner_black_correction = 1;
        p_on.scanner_white_level = 0.98f;
        p_on.scanner_black_level = 0.01f;

        spk_image pcmy_on{};
        if (spk_simulate_tap(eng, &in_img, &p_on, "print_density_cmy",
                             &pcmy_on) == SPK_OK) {
            if (pcmy_on.width != width || pcmy_on.height != height) {
                spk_image_free(&pcmy_on);
                spk_image_free(&rgb_off);
                return false;
            }
            const double d =
                max_abs_diff(pcmy_on.data, gold_pcmy.data.data(), n);
            const bool active = d > 1e-4;
            const bool finite = all_finite(pcmy_on.data, n);
            std::printf("[%s K75P reference print exposure] max_abs vs "
                        "no-correction golden=%.6e finite=%s -> %s\n",
                        case_label, d, finite ? "yes" : "NO",
                        (active && finite) ? "ACTIVE" : "FAIL");
            ok &= active && finite;
            spk_image_free(&pcmy_on);
        } else {
            std::fprintf(stderr, "%s corrected print-density render failed\n",
                         case_label);
            ok = false;
        }

        spk_image rgb_on_a{}, rgb_on_b{};
        if (spk_simulate(eng, &in_img, &p_on, &rgb_on_a) == SPK_OK &&
            spk_simulate(eng, &in_img, &p_on, &rgb_on_b) == SPK_OK) {
            if (rgb_on_a.width != width || rgb_on_a.height != height ||
                rgb_on_b.width != width || rgb_on_b.height != height) {
                spk_image_free(&rgb_on_a);
                spk_image_free(&rgb_on_b);
                spk_image_free(&rgb_off);
                return false;
            }
            const double active_delta =
                max_abs_diff(rgb_on_a.data, rgb_off.data, n);
            const double repeat_delta =
                max_abs_diff(rgb_on_a.data, rgb_on_b.data, n);
            const bool active = active_delta > 1e-4;
            const bool finite = all_finite(rgb_on_a.data, n) &&
                                all_finite(rgb_on_b.data, n);
            const bool deterministic = repeat_delta == 0.0;
            std::printf("[%s K75P reference final RGB] on-vs-off=%.6e "
                        "repeat=%.6e finite=%s -> %s\n",
                        case_label, active_delta, repeat_delta,
                        finite ? "yes" : "NO",
                        (active && finite && deterministic) ? "ACTIVE" : "FAIL");
            ok &= active && finite && deterministic;
            spk_image_free(&rgb_on_a);
            spk_image_free(&rgb_on_b);
        } else {
            std::fprintf(stderr, "%s corrected RGB render failed\n", case_label);
            if (rgb_on_a.data) spk_image_free(&rgb_on_a);
            if (rgb_on_b.data) spk_image_free(&rgb_on_b);
            ok = false;
        }
        spk_image_free(&rgb_off);
        return ok;
    };

    all &= check_k75p_reference("C", "kodak_2383", print_2383_dir);
    all &= check_k75p_reference("D", "kodak_2393", print_2393_dir);

    spk_engine_destroy(eng);
    std::printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}
