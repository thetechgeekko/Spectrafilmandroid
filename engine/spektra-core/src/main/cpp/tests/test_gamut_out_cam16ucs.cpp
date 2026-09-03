/*
 * Spektrafilm for Android — host parity test for the CAM16-UCS output gamut compression
 * (model/gamut_compression.cpp::compress_rgb_cam16ucs_chroma). Copyright (C) 2026
 * Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The oracle's CAM16-UCS chroma reduction (utils/gamut_compression.py::
 * compress_rgb_cam16ucs_chroma, dispatched by compress_rgb with algorithm=="cam16ucs") IS
 * the spec for the native port. Same binary format and case matrix as the OkLch gate
 * (tools/parity/gen_gamut_cam16ucs_golden.py; 48 pixels x 6 spaces x 4 knees,
 * lightness_compression pinned to (0.7,1.0,2.2), oracle at upstream 3bb2c2d): the
 * perceptual space is CIECAM16-UCS J'a'b' (adapting white = the output space's
 * whitepoint, L_A=64, Y_b=20, Average surround), the Cp_max grid is
 * linspace(1,110,64), and the lightness knee is normalized by the whitepoint's Jp. This gates ONLY the opt-in kCam16ucs path; the engine default
 * (kLegacyClip) applies no compression and stays byte-identical.
 *
 * Build (host): full source set per tests/README, e.g.
 *   g++ -std=c++17 -O2 -pthread -I. -I <tools/parity> -DSPK_TEST_DIR=... \
 *     tests/test_gamut_out_cam16ucs.cpp spektra.cpp kernels/*.cpp io/*.cpp \
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
        argc > 1 ? argv[1] : (std::string(SPK_TEST_DIR) + "/gamut_cam16ucs_cases.bin");
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
        spk::compress_rgb_cam16ucs_chroma(rgb.data(), npix, space, t, l, p);
        for (size_t i = 0; i < rgb.size(); ++i) {
            double d = std::fabs(rgb[i] - expected[i]);
            if (d > max_abs) { max_abs = d; worst_case = c; }
        }
        total_pixels += static_cast<size_t>(npix);
    }

    // Same double-precision pipeline + bisection as the oracle -> essentially exact.
    // Parity budget is max_abs <= 1e-4; a bit-identical build lands ~1e-12 or better.
    const double tol = 1e-9;
    chk(max_abs <= tol, "compress_rgb_cam16ucs_chroma matches the oracle golden");
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
                {0.49559917482019639, 0.49564736894270844, 0.49563391523108785}},
            {0, {1.2, -0.1, 0.3},
                {0.87163677256272476, 0.017824106478284148, 0.28299665096153742}},
            {0, {1.0, 0.0, 0.0},
                {0.88119355192366455, 0.038810329766961436, 0.019256362216892239}},
            {0, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
            {1, {0.0, 1.1, -0.2},
                {0.36876766186095083, 0.76491399977120256, 0.001463530852515567}},
            {2, {0.3, 0.6, 0.2},
                {0.29976156796019304, 0.59556941618878467, 0.20068929426810592}},
            {2, {1.5, 1.5, 1.5},
                {0.86243550449610817, 0.86305808063350531, 0.8605038891832073}},
            {3, {1.3, 0.0, 1.2},
                {0.9660789779525305, 0.20458795018006623, 0.85503975754374262}},
            {4, {0.4, 0.4, 0.4},
                {0.39993593646166553, 0.39993606036676133, 0.39993603410818956}},
            {5, {2.0, 0.1, -0.5},
                {0.92358549959314795, 0.39284052725989654, 0.00012385849236076275}},
        };
        double probe_max = 0.0;
        for (auto& pr : probes) {
            double buf[3] = {pr.in[0], pr.in[1], pr.in[2]};
            spk::compress_rgb_cam16ucs_chroma(buf, 1, pr.space, 0.0, 1.0, 6.0);
            for (int ch = 0; ch < 3; ++ch)
                probe_max = std::fmax(probe_max, std::fabs(buf[ch] - pr.want[ch]));
        }
        chk(probe_max <= 1e-9, "compress_rgb_cam16ucs_chroma matches the oracle probes");
        std::printf("    probe max_abs=%.3e\n", probe_max);
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail;
}
