/*
 * Spektrafilm for Android — host parity test for the JzCzhz output gamut compression
 * (model/gamut_compression.cpp::compress_rgb_jzazbz_chroma). Copyright (C) 2026
 * Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The oracle's JzCzhz chroma reduction (utils/gamut_compression.py::
 * compress_rgb_jzazbz_chroma, dispatched by compress_rgb with algorithm=="jzazbz") IS
 * the spec for the native port. Same binary format and case matrix as the OkLch gate
 * (tools/parity/gen_gamut_jzazbz_golden.py; 48 pixels x 6 spaces x 4 knees,
 * lightness_compression pinned to (0.7,1.0,2.2), oracle at upstream 3bb2c2d): the
 * perceptual space is JzAzBz at Y_w = 100 cd/m^2, the Cz_max grid is
 * linspace(0.002,0.18,64), and the lightness knee is normalized by the output
 * whitepoint's Jz. This gates ONLY the opt-in kJzazbz path; the engine default
 * (kLegacyClip) applies no compression and stays byte-identical.
 *
 * Build (host): full source set per tests/README, e.g.
 *   g++ -std=c++17 -O2 -pthread -I. -I <tools/parity> -DSPK_TEST_DIR=... \
 *     tests/test_gamut_out_jzazbz.cpp spektra.cpp kernels/*.cpp io/*.cpp \
 *     model/*.cpp profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp -o /tmp/t
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "model/gamut_compression.h"

#ifndef SPK_TEST_DIR
#define SPK_TEST_DIR "."
#endif

namespace {

template <typename T>
bool read_n(std::ifstream& in, T* dst, size_t count) {
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(count * sizeof(T)));
    return static_cast<size_t>(in.gcount()) == count * sizeof(T);
}

int g_fail = 0;
void chk(bool c, const char* m) {
    std::printf("  [%s] %s\n", c ? "ok" : "FAIL", m);
    if (!c) g_fail = 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path =
        argc > 1 ? argv[1] : (std::string(SPK_TEST_DIR) + "/gamut_jzazbz_cases.bin");
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    int32_t num_cases = 0;
    if (!read_n(in, &num_cases, 1) || num_cases <= 0) {
        std::fprintf(stderr, "bad case count\n"); return 2;
    }

    double max_abs = 0.0;
    int worst_case = -1;
    size_t total_pixels = 0;
    for (int c = 0; c < num_cases; ++c) {
        int32_t space = 0;
        double t = 0.0, l = 0.0, p = 0.0;
        int32_t npix = 0;
        if (!read_n(in, &space, 1) || !read_n(in, &t, 1) || !read_n(in, &l, 1) ||
            !read_n(in, &p, 1) || !read_n(in, &npix, 1) || npix < 1 ||
            space < 0 || space > 5) {
            std::fprintf(stderr, "truncated/bad header at case %d\n", c); return 2;
        }
        std::vector<double> rgb(static_cast<size_t>(npix) * 3);
        std::vector<double> expected(static_cast<size_t>(npix) * 3);
        if (!read_n(in, rgb.data(), rgb.size()) ||
            !read_n(in, expected.data(), expected.size())) {
            std::fprintf(stderr, "truncated payload at case %d\n", c); return 2;
        }
        // Build the table once (inside the wrapper) and compress in place, exactly as
        // the scanning hook does, then compare against the oracle output.
        spk::compress_rgb_jzazbz_chroma(rgb.data(), npix, space, t, l, p);
        for (size_t i = 0; i < rgb.size(); ++i) {
            double d = std::fabs(rgb[i] - expected[i]);
            if (d > max_abs) { max_abs = d; worst_case = c; }
        }
        total_pixels += static_cast<size_t>(npix);
    }

    // Same double-precision pipeline + bisection as the oracle -> essentially exact.
    // Parity budget is max_abs <= 1e-4; a bit-identical build lands ~1e-12 or better.
    const double tol = 1e-9;
    chk(max_abs <= tol, "compress_rgb_jzazbz_chroma matches the oracle golden");
    std::printf("    %d cases, %zu pixels -> max_abs=%.3e (tol %.0e) worst case=%d\n",
                num_cases, total_pixels, max_abs, tol, worst_case);

    // Independent hand-captured probes (utils/gamut_compression.py at knee (0,1,6),
    // lightness_compression (0.7,1.0,2.2)) across every space, incl. black (anchored to
    // 0) and OOG pixels that reconstruct slightly outside the cube (no clip here — the
    // scanning stage clips afterwards). space: 0 sRGB, 1 Adobe, 2 ProPhoto, 3 Rec2020,
    // 4 ACES2065-1, 5 linear-sRGB (== 0).
    {
        struct { int space; double in[3]; double want[3]; } probes[] = {
            {0, {0.5, 0.5, 0.5},
                {0.4993825719106742, 0.49940257608124161, 0.49938355782529703}},
            {0, {1.2, -0.1, 0.3},
                {0.96520878163598389, 0.015432593526303724, 0.30872237801984403}},
            {0, {1.0, 0.0, 0.0},
                {0.91257002602234283, 0.038567513388536728, 0.020623445134438662}},
            {0, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
            {1, {0.0, 1.1, -0.2},
                {0.20900274922469306, 0.83194999214537235, 0.0011493227360781879}},
            {2, {0.3, 0.6, 0.2},
                {0.30050504933280076, 0.5994203314679275, 0.20127507981134637}},
            {2, {1.5, 1.5, 1.5},
                {0.91078689346323327, 0.90582693593132957, 0.87882673545707124}},
            {3, {1.3, 0.0, 1.2},
                {0.99806797059702279, 0.28176581948882817, 0.92826228019324619}},
            {4, {0.4, 0.4, 0.4},
                {0.3999999999856762, 0.40000000002433467, 0.40000000007776643}},
            {5, {2.0, 0.1, -0.5},
                {0.65113184135063706, 0.41421292041681651, 1.0000277184287523}},
        };
        double probe_max = 0.0;
        for (auto& pr : probes) {
            double buf[3] = {pr.in[0], pr.in[1], pr.in[2]};
            spk::compress_rgb_jzazbz_chroma(buf, 1, pr.space, 0.0, 1.0, 6.0);
            for (int ch = 0; ch < 3; ++ch)
                probe_max = std::fmax(probe_max, std::fabs(buf[ch] - pr.want[ch]));
        }
        chk(probe_max <= 1e-9, "compress_rgb_jzazbz_chroma matches the oracle probes");
        std::printf("    probe max_abs=%.3e\n", probe_max);
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail;
}
