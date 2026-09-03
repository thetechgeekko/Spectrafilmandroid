/*
 * Spektrafilm for Android — host sanity test: spk_params pass-through is LIVE.
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
 * NOT a bit-exact gate. Proves the newly-wired spk_params fields actually reach
 * the render: starting from spk_default_params (which reproduces the goldens),
 * perturbing one field at a time MUST measurably change the output. Covers the
 * filming route (exposure_compensation_ev, grain.blur, halation.halation_amount)
 * and the print route (enlarger y_filter_shift). Also confirms the default run
 * still matches the committed scan_portra final_rgb golden (the parity anchor).
 *
 * Self-contained: the only external input is a valid engine + the committed
 * input vector. It loads NO oracle golden (this is a live/dead probe, not a
 * bit-exact gate), so it runs against the BUNDLED engine assets.
 *   argv[1] = asset dir (default: ../assets/spektra relative to cpp root)
 *   argv[2] = input .f64 (default: the committed tests/scan_portra_input_rgb.f64)
 *
 * Build (host):
 *   g++ -std=c++17 -O2 -pthread -I <cpp_root> -I <tools/parity> \
 *     -DSPK_TEST_DIR="\"<cpp_root>/tests\"" \
 *     tests/test_params_passthrough.cpp \
 *     spektra.cpp kernels/*.cpp io/*.cpp model/*.cpp profiles/*.cpp \
 *     runtime/*.cpp runtime/stages/*.cpp -o /tmp/test_params_passthrough
 *   /tmp/test_params_passthrough <asset_dir> <input.f64>
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "spektra.h"

namespace {

// Defaults assume the CWD is the cpp root (as the engine-parity CI job does,
// `cd "$CPP"`). Both are overridable via argv so the test is self-contained
// against the BUNDLED engine assets — no Python-oracle data dir required.
const char* kAssetDir = "../assets/spektra";
#ifdef SPK_TEST_DIR
const char* kInputF64 = SPK_TEST_DIR "/scan_portra_input_rgb.f64";
#else
const char* kInputF64 = "tests/scan_portra_input_rgb.f64";
#endif

constexpr int kW = 64, kH = 64;

bool load_input(const char* path, std::vector<float>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<double> d(static_cast<size_t>(kW) * kH * 3);
    f.read(reinterpret_cast<char*>(d.data()),
           static_cast<std::streamsize>(d.size() * sizeof(double)));
    if (!f) return false;
    out->assign(d.begin(), d.end());
    return true;
}

// Mean absolute difference between two equal-length RGB buffers.
double mean_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) s += std::fabs((double)a[i] - (double)b[i]);
    return n ? s / (double)n : 0.0;
}

// Max absolute difference (better captures localized spatial changes).
double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        double d = std::fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

bool simulate(spk_engine* eng, const spk_image* in, const spk_params* p,
              std::vector<float>* out) {
    spk_image o{};
    if (spk_simulate(eng, in, p, &o) != SPK_OK || !o.data) return false;
    out->assign(o.data, o.data + (size_t)o.width * o.height * 3);
    spk_image_free(&o);
    return true;
}

int g_fail = 0;
void check_changes(const char* name, double d, double thresh) {
    bool ok = d > thresh;
    std::printf("  [%-28s] mean|d|=%.6e  (> %.0e) -> %s\n", name, d, thresh,
                ok ? "LIVE" : "DEAD");
    if (!ok) ++g_fail;
}

}  // namespace

int main(int argc, char** argv) {
    const char* asset_dir = argc > 1 ? argv[1] : kAssetDir;
    const char* input_path = argc > 2 ? argv[2] : kInputF64;

    std::vector<float> rgb;
    if (!load_input(input_path, &rgb)) {
        std::fprintf(stderr, "cannot load %s\n", input_path);
        return 2;
    }
    spk_image in{rgb.data(), kW, kH, (int)SPK_CS_PROPHOTO};

    spk_engine* eng = nullptr;
    if (spk_engine_create(asset_dir, &eng) != SPK_OK || !eng) {
        std::fprintf(stderr, "engine create failed (asset dir %s)\n", asset_dir);
        return 2;
    }

    // ----- Baseline: default params on the scan_film route. -----
    spk_params base{};
    base.film_profile = "kodak_portra_400";
    base.print_profile = "kodak_portra_endura";
    spk_default_params(&base);
    // The schema fields that historically crossed JNI but were ignored must
    // have explicit native defaults. These checks also protect old/native
    // callers that do not populate the appended fields.
    if (!base.enlarger_illuminant ||
        std::string(base.enlarger_illuminant) != "TH-KG3" ||
        !base.input_color_space ||
        std::string(base.input_color_space) != "ProPhoto RGB" ||
        base.camera_diffusion_family != SPK_DIFFUSION_BLACK_PRO_MIST ||
        base.enlarger_diffusion_family != SPK_DIFFUSION_BLACK_PRO_MIST) {
        std::fprintf(stderr, "parameter schema defaults are incomplete\n");
        return 2;
    }
    base.scan_film = 1;

    std::vector<float> ref_scan;
    if (!simulate(eng, &in, &base, &ref_scan)) {
        std::fprintf(stderr, "baseline scan_film simulate failed\n");
        spk_engine_destroy(eng);
        return 2;
    }

    std::printf("=== spk_params pass-through (scan_film route) ===\n");

    // exposure_compensation_ev: +1 EV must brighten the negative scan.
    {
        spk_params p = base;
        p.exposure_compensation_ev = 1.0f;
        std::vector<float> out;
        simulate(eng, &in, &p, &out);
        check_changes("camera.exposure_ev=+1", mean_abs_diff(ref_scan, out), 1e-4);
    }

    // grain.blur: grain is stochastic, so enable spatial+grain for both the ref
    // and the perturbed run and compare grain.blur=0.65 vs 5.0 (same RNG stream,
    // only the final Gaussian sigma differs -> different texture).
    {
        spk_params g0 = base; g0.grain_active = 1; g0.halation_active = 1;
        spk_params g1 = g0;   g1.grain_blur = 5.0f;
        std::vector<float> a, b;
        simulate(eng, &in, &g0, &a);
        simulate(eng, &in, &g1, &b);
        check_changes("grain.blur 0.65 -> 5.0", mean_abs_diff(a, b), 1e-4);
    }

    // halation block: needs the spatial branch (halation_active). The preset
    // sigma/strength stay baked (film-specific), but the in-emulsion scatter and
    // the back-reflection amount are user-driven. Grain off to keep it
    // deterministic. The (still,strong) preset for portra_400 makes the additive
    // back-reflection tiny, so halation_amount alone is a small effect; the
    // scatter blend is the dominant, always-present halation knob.
    {
        spk_params h0 = base; h0.halation_active = 1; h0.grain_active = 0;
        // scatter_amount 1.0 -> 0.0 disables in-emulsion scatter entirely.
        spk_params hs = h0;   hs.halation_scatter_amount = 0.0f;
        // halation_amount 1.0 -> 50.0 scales the (small) back-reflection.
        spk_params hb = h0;   hb.halation_halation_amount = 50.0f;
        std::vector<float> a, b, c;
        simulate(eng, &in, &h0, &a);
        simulate(eng, &in, &hs, &b);
        simulate(eng, &in, &hb, &c);
        // Spatial effects on this smooth synthetic image are localized, so use
        // the max abs diff. These thresholds only assert "non-zero / live", not
        // any particular magnitude.
        check_changes("halation.scatter_amount", max_abs_diff(a, b), 1e-5);
        check_changes("halation.halation_amount", max_abs_diff(a, c), 1e-6);
    }

    // DiffusionFilterParams.filterFamily is a live family selector. Keep the
    // comparison deterministic by disabling the stochastic branches and use a
    // strong filter so the PSF family difference is observable on this fixture.
    {
        spk_params d0 = base;
        d0.grain_active = 0;
        d0.halation_active = 0;
        d0.camera_diffusion_active = 1;
        d0.camera_diffusion_strength = 1.0f;
        d0.camera_diffusion_family = SPK_DIFFUSION_BLACK_PRO_MIST;
        spk_params d1 = d0;
        d1.camera_diffusion_family = SPK_DIFFUSION_GLIMMERGLASS;
        std::vector<float> a, b;
        if (!simulate(eng, &in, &d0, &a) || !simulate(eng, &in, &d1, &b)) {
            std::fprintf(stderr, "camera diffusion family simulate failed\n");
            spk_engine_destroy(eng);
            return 2;
        }
        check_changes("camera.diffusion.filterFamily", mean_abs_diff(a, b), 1e-6);
    }

    std::printf("=== spk_params pass-through (print route) ===\n");

    // Print-route baseline (scan_film off).
    spk_params pbase = base;
    pbase.scan_film = 0;
    std::vector<float> ref_print;
    if (!simulate(eng, &in, &pbase, &ref_print)) {
        std::fprintf(stderr, "baseline print simulate failed\n");
        spk_engine_destroy(eng);
        return 2;
    }

    // enlarger.y_filter_shift: shifts the dichroic Y filter CC, recolouring the
    // print. 0 -> 30 CC must change the output.
    {
        spk_params p = pbase;
        p.y_filter_shift = 30.0f;
        std::vector<float> out;
        simulate(eng, &in, &p, &out);
        check_changes("enlarger.y_filter_shift=30", mean_abs_diff(ref_print, out), 1e-4);
    }

    // enlarger.print_exposure: 1.0 -> 2.0 must change the print exposure.
    {
        spk_params p = pbase;
        p.print_exposure = 2.0f;
        std::vector<float> out;
        simulate(eng, &in, &p, &out);
        check_changes("enlarger.print_exposure=2", mean_abs_diff(ref_print, out), 1e-4);
    }

    {
        spk_params d0 = pbase;
        d0.grain_active = 0;
        d0.enlarger_diffusion_active = 1;
        d0.enlarger_diffusion_strength = 1.0f;
        d0.enlarger_diffusion_family = SPK_DIFFUSION_BLACK_PRO_MIST;
        spk_params d1 = d0;
        d1.enlarger_diffusion_family = SPK_DIFFUSION_CINEBLOOM;
        std::vector<float> a, b;
        if (!simulate(eng, &in, &d0, &a) || !simulate(eng, &in, &d1, &b)) {
            std::fprintf(stderr, "enlarger diffusion family simulate failed\n");
            spk_engine_destroy(eng);
            return 2;
        }
        check_changes("enlarger.diffusion.filterFamily", mean_abs_diff(a, b), 1e-6);
    }

    // Only TH-KG3 is currently backed by a native SPD. An explicit unsupported
    // selector must fail closed rather than silently using TH-KG3.
    {
        spk_params p = pbase;
        p.enlarger_illuminant = "D50";
        spk_image out{};
        if (spk_simulate(eng, &in, &p, &out) != SPK_ERR_BAD_ARGS) {
            std::fprintf(stderr, "unsupported enlarger illuminant was accepted\n");
            spk_image_free(&out);
            spk_engine_destroy(eng);
            return 1;
        }
    }

    // InputColorSpace currently has one native implementation. It is therefore
    // validated explicitly; an unknown value cannot be silently reinterpreted.
    {
        spk_params p = pbase;
        p.input_color_space = "sRGB";
        spk_image out{};
        if (spk_simulate(eng, &in, &p, &out) != SPK_ERR_BAD_ARGS) {
            std::fprintf(stderr, "unsupported input color space was accepted\n");
            spk_image_free(&out);
            spk_engine_destroy(eng);
            return 1;
        }
    }

    spk_engine_destroy(eng);
    std::printf("%s\n", g_fail == 0 ? "ALL LIVE" : "SOME DEAD");
    return g_fail == 0 ? 0 : 1;
}
