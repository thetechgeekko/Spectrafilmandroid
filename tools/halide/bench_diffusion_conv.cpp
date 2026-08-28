/*
 * Spektrafilm for Android — A/B bench: Halide vs the engine's scalar diffusion
 * PSF convolution (experiment #155). GPLv3. Film modeling powered by spektrafilm.
 *
 * The reference below is transcribed verbatim from model/diffusion.cpp's inner
 * convolution (including the flipped-kernel indexing), so the comparison is
 * against the exact arithmetic the engine performs today. Halide reassociates the
 * reduction when it vectorises, so outputs differ in the last bits BY DESIGN —
 * the bench reports max_abs so the drift can be judged against the oracle
 * tolerance (1e-4) rather than assumed.
 *
 * Build (host):
 *   python3 tools/halide/gen_diffusion_conv.py /tmp/halide_out host
 *   g++ -std=c++17 -O2 -pthread -I/tmp/halide_out \
 *       -I$(python3 -c "import halide,os;print(os.path.dirname(halide.__file__))")/include \
 *       tools/halide/bench_diffusion_conv.cpp /tmp/halide_out/spk_diffusion_conv_host.a \
 *       -o /tmp/bench_diffusion_conv -ldl
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "HalideBuffer.h"
// The AOT header is target-specific (host vs arm-64-android); the runner passes
// -DSPK_HALIDE_HEADER=... so one bench source serves both.
#ifndef SPK_HALIDE_HEADER
#define SPK_HALIDE_HEADER "spk_diffusion_conv_host.h"
#endif
#include SPK_HALIDE_HEADER

namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Verbatim from model/diffusion.cpp (single-threaded, so the comparison isolates
// the KERNEL, not the threading — the engine parallelises rows around this).
void ref_conv(const std::vector<double>& padded, int pw,
              const std::vector<double>& kern, int ks,
              int w, int h, std::vector<double>& out) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = 0; i < ks; ++i) {
                const double* prow = &padded[static_cast<size_t>(y + i) * pw + x];
                const double* krow = &kern[static_cast<size_t>(ks - 1 - i) * ks];
                for (int j = 0; j < ks; ++j) acc += prow[j] * krow[ks - 1 - j];
            }
            out[static_cast<size_t>(y) * w + x] = acc;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int reps = argc > 1 ? std::atoi(argv[1]) : 3;
    std::printf("%-22s %10s %10s %9s %12s\n",
                "case", "scalar_ms", "halide_ms", "speedup", "max_abs");

    struct Case { int w, h, ks; const char* name; };
    const Case cases[] = {
        {640, 480, 9,  "640x480  ks=9"},
        {640, 480, 17, "640x480  ks=17"},
        {640, 480, 33, "640x480  ks=33"},
        {1024, 768, 17, "1024x768 ks=17"},
    };

    for (const Case& cs : cases) {
        const int radius = (cs.ks - 1) / 2;
        const int pw = cs.w + 2 * radius, ph = cs.h + 2 * radius;
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> dist(0.0, 1.5);

        std::vector<double> padded(static_cast<size_t>(pw) * ph);
        for (auto& v : padded) v = dist(rng);
        // PSF shape from diffusion.cpp: sum of exp(-r/lambda), sum-normalised.
        std::vector<double> kern(static_cast<size_t>(cs.ks) * cs.ks);
        double ksum = 0.0;
        for (int yy = 0; yy < cs.ks; ++yy)
            for (int xx = 0; xx < cs.ks; ++xx) {
                double dx = xx - cs.ks / 2, dy = yy - cs.ks / 2;
                double r = std::sqrt(dx * dx + dy * dy);
                double v = 0.6 * std::exp(-r / 1.7) + 0.4 * std::exp(-r / 6.0);
                kern[static_cast<size_t>(yy) * cs.ks + xx] = v;
                ksum += v;
            }
        for (auto& v : kern) v /= ksum;

        std::vector<double> out_ref(static_cast<size_t>(cs.w) * cs.h, 0.0);
        std::vector<double> out_hl(static_cast<size_t>(cs.w) * cs.h, 0.0);

        double t_ref = 1e30, t_hl = 1e30;
        for (int r = 0; r < reps; ++r) {
            double t0 = now_ms();
            ref_conv(padded, pw, kern, cs.ks, cs.w, cs.h, out_ref);
            t_ref = std::min(t_ref, now_ms() - t0);
        }

        Halide::Runtime::Buffer<double> b_padded(padded.data(), pw, ph);
        Halide::Runtime::Buffer<double> b_kern(kern.data(), cs.ks, cs.ks);
        Halide::Runtime::Buffer<double> b_out(out_hl.data(), cs.w, cs.h);
        for (int r = 0; r < reps; ++r) {
            double t0 = now_ms();
            int rc = spk_diffusion_conv(b_padded, b_kern, cs.ks, b_out);
            if (rc != 0) { std::printf("halide rc=%d\n", rc); return 1; }
            t_hl = std::min(t_hl, now_ms() - t0);
        }

        double max_abs = 0.0;
        for (size_t i = 0; i < out_ref.size(); ++i)
            max_abs = std::max(max_abs, std::fabs(out_ref[i] - out_hl[i]));

        std::printf("%-22s %10.2f %10.2f %8.2fx %12.3e\n",
                    cs.name, t_ref, t_hl, t_ref / t_hl, max_abs);
    }
    return 0;
}
