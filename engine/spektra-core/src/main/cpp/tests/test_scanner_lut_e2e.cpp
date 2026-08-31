/*
 * Spektrafilm for Android — end-to-end host test for the OPT-IN scanner 3D-LUT.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Proves the WIRED scanner-LUT path is correct end-to-end (not just the isolated
 * kernel that tests/test_lut_accel.cpp gates). It runs both the scan_film route
 * and the Kodak 2383/2393 print routes through spk_simulate() on the same
 * deterministic fixture:
 *   (A) use_scanner_lut = 0  -> the DEFAULT direct spectral path (bit-exact, the
 *       parity-gate path).
 *   (B) use_scanner_lut = 1  -> the LUT-accelerated path (scan() builds a per-
 *       channel PCHIP 3D LUT over the density domain at lut_resolution and
 *       interpolates density_cmy -> log_xyz instead of the per-pixel spectral
 *       integral).
 *
 * Assertions:
 *  1. The direct (A) output matches each committed oracle final_rgb golden
 *     (max_abs <= 1e-4, rms <= 1e-5). The cinema-print cases therefore lock the
 *     K75P spectral integral + output-white adaptation on the CPU direct route.
 *  2. The K75P LUT (B) output matches a committed, pinned-spektrafilm fixture
 *     generated with the same lut_resolution=17 (max_abs <= 1e-4, rms <= 1e-5).
 *     This is a like-for-like oracle-parity gate: coarse LUT output is
 *     intentionally not conflated with direct spectral integration.
 *  3. The legacy D50 route retains its LUT-vs-direct convergence gates at
 *     resolutions 17 and 64.
 *
 * Build (host) — full source set, from the cpp root:
 *   g++ -std=c++17 -O2 -I <cpp_root> -I <tools/parity> \
 *     tests/test_scanner_lut_e2e.cpp <full SRC set> -o /tmp/test_scanner_lut_e2e
 * Run:
 *   /tmp/test_scanner_lut_e2e <asset_dir> <scan_portra_golden_dir> <input.f64>
 *
 * The Kodak K75P golden directories are resolved as siblings of scan_portra;
 * no oracle generation is performed by this test.
 */
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "spkvec_io.h"
#include "spektra.h"

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/scan_portra";
const char* kInputF64 =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

struct Metrics { double max_abs; double rms; size_t argmax; };

Metrics compare(const float* a, const float* b, size_t n) {
    double max_abs = 0.0, sse = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < n; ++i) {
        double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > max_abs) { max_abs = d; argmax = i; }
        sse += d * d;
    }
    return {max_abs, std::sqrt(sse / static_cast<double>(n)), argmax};
}

// Run the full scan_film pipeline once with the given LUT settings.
bool run(spk_engine* eng, const spk_image& in, const char* print_profile,
         int scan_film, int use_lut, int lut_res,
         std::vector<float>* out_rgb, int* w, int* h) {
    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = print_profile;
    spk_default_params(&p);
    p.exposure_compensation_ev = 0.0f;
    p.auto_exposure = 0;
    p.density_curve_gamma = 1.0f;
    p.grain_active = 0;
    p.halation_active = 0;
    // Spatial effects are per-effect gated (zero = inert); express the
    // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
    p.dir_diffusion_size_um = 0.0f;
    p.scanner_unsharp[0] = 0.0f;
    p.scanner_unsharp[1] = 0.0f;
    p.dir_couplers_active = 1;
    p.glare_active = 0;
    p.scan_film = scan_film;
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    p.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p.preview_max_size = 640;
    p.use_scanner_lut = use_lut;
    p.lut_resolution = lut_res;

    spk_image out{};
    spk_status st = spk_simulate(eng, &in, &p, &out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate failed: %s\n", spk_status_str(st));
        return false;
    }
    *w = out.width; *h = out.height;
    const size_t n = static_cast<size_t>(out.width) * out.height * 3;
    out_rgb->assign(out.data, out.data + n);
    spk_image_free(&out);
    return true;
}

std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir = argc > 1 ? argv[1] : kAssetDir;
    std::string golden_dir = argc > 2 ? argv[2] : kGoldenDir;
    std::string input_path = argc > 3 ? argv[3] : kInputF64;

    spk_engine* eng = nullptr;
    if (spk_engine_create(asset_dir.c_str(), &eng) != SPK_OK) {
        std::fprintf(stderr, "engine create failed\n");
        return 2;
    }

    spkvec::Array scan_gold =
        spkvec::read(golden_dir + "/final_rgb.spkvec");
    const int height = static_cast<int>(scan_gold.shape[0]);
    const int width = static_cast<int>(scan_gold.shape[1]);
    const int npix = width * height;
    const size_t n = static_cast<size_t>(npix) * 3;

    std::vector<double> rgb64(n);
    {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", input_path.c_str()); return 2; }
        in.read(reinterpret_cast<char*>(rgb64.data()),
                static_cast<std::streamsize>(n * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(n * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch\n"); return 2;
        }
    }
    std::vector<float> rgb32(rgb64.begin(), rgb64.end());
    spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};

    std::printf("Image: %dx%dx3\n", width, height);

    const std::string goldens_root = parent_dir(golden_dir);
    auto check_route = [&](const char* label, const char* print_profile,
                           int scan_film, const std::string& route_golden_dir,
                           bool has_lut_oracle) {
        spkvec::Array gold =
            spkvec::read(route_golden_dir + "/final_rgb.spkvec");
        if (gold.data.size() != n || static_cast<int>(gold.shape[0]) != height ||
            static_cast<int>(gold.shape[1]) != width) {
            std::fprintf(stderr, "%s golden shape mismatch\n", label);
            return false;
        }

        // (A) Direct path (use_scanner_lut = 0): the default oracle path.
        std::vector<float> direct;
        int dw = 0, dh = 0;
        if (!run(eng, in_img, print_profile, scan_film, /*use_lut=*/0,
                 /*res=*/17, &direct, &dw, &dh)) {
            return false;
        }
        if (dw != width || dh != height || direct.size() != n) {
            std::fprintf(stderr, "%s direct output shape mismatch\n", label);
            return false;
        }

        Metrics m_gold = compare(direct.data(), gold.data.data(), n);
        const double tol_max_abs = 1e-4, tol_rms = 1e-5;
        bool pass_direct =
            (m_gold.max_abs <= tol_max_abs) && (m_gold.rms <= tol_rms);
        std::printf("[%s direct vs golden] max_abs=%.6e (tol %.0e) "
                    "rms=%.6e (tol %.0e) -> %s\n",
                    label, m_gold.max_abs, tol_max_abs, m_gold.rms, tol_rms,
                    pass_direct ? "PASS" : "FAIL");

        // (B) Default preview grid for every route. K75P compares like-for-like
        // with the pinned oracle's LUT output. The legacy D50 fixture predates a
        // route-level LUT oracle and retains its tight direct-convergence gate.
        bool pass_lut = true;
        const struct { int res; double direct_band; } cfgs[] = {
            {17, 5e-5}, {64, 5e-6}};
        const int cfg_count = has_lut_oracle ? 1 : 2;
        for (int i = 0; i < cfg_count; ++i) {
            const auto& cfg = cfgs[i];
            std::vector<float> lut;
            int lw = 0, lh = 0;
            if (!run(eng, in_img, print_profile, scan_film, /*use_lut=*/1,
                     cfg.res, &lut, &lw, &lh)) {
                return false;
            }
            if (lw != width || lh != height || lut.size() != n) return false;
            Metrics versus_direct = compare(lut.data(), direct.data(), n);
            if (has_lut_oracle) {
                spkvec::Array lut_gold = spkvec::read(
                    route_golden_dir + "/final_rgb_scanner_lut_17.spkvec");
                if (lut_gold.data.size() != n || lut_gold.shape != gold.shape) {
                    std::fprintf(stderr, "%s LUT oracle shape mismatch\n", label);
                    return false;
                }
                Metrics oracle = compare(lut.data(), lut_gold.data.data(), n);
                const bool within = oracle.max_abs <= 1e-4 && oracle.rms <= 1e-5;
                std::printf("[%s LUT(res=17) vs pinned LUT oracle] "
                            "max_abs=%.6e (tol 1e-4) rms=%.6e (tol 1e-5); "
                            "vs-direct max_abs=%.6e (diagnostic) -> %s\n",
                            label, oracle.max_abs, oracle.rms,
                            versus_direct.max_abs, within ? "PASS" : "FAIL");
                pass_lut = pass_lut && within;
            } else {
                const bool within =
                    versus_direct.max_abs <= cfg.direct_band;
                std::printf("[%s LUT(res=%d) vs direct] max_abs=%.6e "
                            "(accel band %.0e, NOT bit-exact by design) "
                            "rms=%.6e -> %s\n",
                            label, cfg.res, versus_direct.max_abs,
                            cfg.direct_band, versus_direct.rms,
                            within ? "WITHIN BAND" : "OUT OF BAND");
                pass_lut = pass_lut && within;
            }
        }
        return pass_direct && pass_lut;
    };

    // Exercise both affected K75P profiles, then retain the legacy D50 route.
    // Viewing-only LUT-cache isolation is gated separately by test_lut_cache_e2e.
    bool all = true;
    all &= check_route("kodak_2383/K75P", "kodak_2383", /*scan_film=*/0,
                       goldens_root + "/print_kodak_2383_k75p", true);
    all &= check_route("kodak_2393/K75P", "kodak_2393", /*scan_film=*/0,
                       goldens_root + "/print_kodak_2393_k75p", true);
    all &= check_route("scan_portra/D50", "kodak_portra_endura",
                       /*scan_film=*/1, golden_dir, false);

    spk_engine_destroy(eng);
    std::printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}
