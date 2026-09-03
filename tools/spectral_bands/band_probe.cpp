/*
 * Spektrafilm for Android — spectral band-width probe (host experiment).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Renders the SAME fixture through two asset trees and reports the error
 * DISTRIBUTION in 8-bit display codes, for both the scan route and the print
 * (enlarger) route. Used to size what a lower spectral band count costs.
 *
 * Output is SPK_CS_SRGB with cctf encoding on, so a delta of d maps to d*255
 * display codes directly.
 *
 *   band_probe <asset_dir_A> <asset_dir_B> <input.f64> <W> <H>
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "spektra.h"

namespace {

spk_params base_params(int scan_film, const char* film, const char* paper) {
    spk_params p{};
    p.film_profile = film;
    p.print_profile = paper;
    spk_default_params(&p);
    p.exposure_compensation_ev = 0.0f;
    p.auto_exposure = 0;
    p.density_curve_gamma = 1.0f;
    p.grain_active = 0;
    p.halation_active = 0;
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
    return p;
}

bool render(const char* asset_dir, const spk_params& p, const spk_image& in,
            std::vector<float>* out_buf) {
    spk_engine* eng = nullptr;
    if (spk_engine_create(asset_dir, &eng) != SPK_OK) {
        std::fprintf(stderr, "engine create failed for %s\n", asset_dir);
        return false;
    }
    spk_image out{};
    spk_params q = p;
    spk_status st = spk_simulate(eng, &in, &q, &out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "simulate failed: %s\n", spk_status_str(st));
        spk_engine_destroy(eng);
        return false;
    }
    size_t n = static_cast<size_t>(out.width) * out.height * 3;
    out_buf->assign(out.data, out.data + n);
    spk_image_free(&out);
    spk_engine_destroy(eng);
    return true;
}

void report(const char* label, const std::vector<float>& a,
            const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        std::printf("[%s] SIZE MISMATCH\n", label);
        return;
    }
    std::vector<double> d;
    d.reserve(a.size());
    double sse = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double x = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        d.push_back(x);
        sse += x * x;
    }
    std::sort(d.begin(), d.end());
    auto pct = [&](double q) { return d[static_cast<size_t>(q * (d.size() - 1))]; };
    double rms = std::sqrt(sse / d.size());
    auto over = [&](double thr) {
        size_t c = static_cast<size_t>(
            d.end() - std::lower_bound(d.begin(), d.end(), thr));
        return 100.0 * c / d.size();
    };
    const double C = 255.0;
    std::printf("[%s]  n=%zu channel samples\n", label, d.size());
    std::printf("   codes/255:  median %.2f   p90 %.2f   p99 %.2f   p99.9 %.2f   max %.2f   rms %.2f\n",
                pct(0.50) * C, pct(0.90) * C, pct(0.99) * C, pct(0.999) * C,
                d.back() * C, rms * C);
    std::printf("   share of channel samples off by >= 1 code: %.2f%%   >= 2: %.2f%%   >= 5: %.2f%%\n",
                over(1.0 / C), over(2.0 / C), over(5.0 / C));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s <asset_A> <asset_B> <input.f64> <W> <H>\n", argv[0]);
        return 2;
    }
    const char* aA = argv[1];
    const char* aB = argv[2];
    const int W = std::atoi(argv[4]);
    const int H = std::atoi(argv[5]);
    const size_t npix = static_cast<size_t>(W) * H;

    std::vector<double> rgb64(npix * 3);
    std::ifstream in(argv[3], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[3]); return 2; }
    in.read(reinterpret_cast<char*>(rgb64.data()),
            static_cast<std::streamsize>(rgb64.size() * sizeof(double)));
    if (in.gcount() != static_cast<std::streamsize>(rgb64.size() * sizeof(double))) {
        std::fprintf(stderr, "input size mismatch\n");
        return 2;
    }
    std::vector<float> rgb32(rgb64.begin(), rgb64.end());
    spk_image in_img{rgb32.data(), W, H, SPK_CS_PROPHOTO};

    struct Case { const char* name; int scan; const char* film; const char* paper; };
    const Case cases[] = {
        {"scan  kodak_portra_400",              1, "kodak_portra_400", "kodak_portra_endura"},
        {"print kodak_portra_400 -> endura",    0, "kodak_portra_400", "kodak_portra_endura"},
        {"print kodak_ektar_100  -> supra",     0, "kodak_ektar_100",  "kodak_supra_endura"},
        {"scan  fujifilm_provia_100f",          1, "fujifilm_provia_100f", "kodak_portra_endura"},
    };

    for (const Case& c : cases) {
        spk_params p = base_params(c.scan, c.film, c.paper);
        std::vector<float> ra, rb;
        if (!render(aA, p, in_img, &ra)) return 2;
        if (!render(aB, p, in_img, &rb)) return 2;
        report(c.name, ra, rb);
    }
    return 0;
}
