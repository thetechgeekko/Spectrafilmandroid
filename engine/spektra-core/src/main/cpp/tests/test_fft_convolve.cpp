/*
 * Spektrafilm for Android — host test: FFT convolution == the direct PSF loop.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Gates kernels/fft_convolve.cpp against a reference transcribed VERBATIM from
 * model/diffusion.cpp's direct loop (same flipped-kernel indexing, same f64), on:
 *   - the single-transform case (small image, no tiling),
 *   - the OVERLAP-SAVE case (transform capped below the image, so the tile seams
 *     are exercised -- a wrong block step or a wrong valid-region offset shows up
 *     here and nowhere else),
 *   - non-square and odd sizes, where an off-by-one in the last partial tile lives,
 *   - a wide dynamic range, so cancellation has somewhere to hide.
 * and asserts thread-invariance (SPK_NUM_THREADS 1 vs 8 byte-identical).
 *
 * Tolerance, not byte-equality: reassociating a sum in floating point changes the
 * last bits by construction. The bar is the parity suite's own, max_abs <= 1e-4
 * and rms <= 1e-5, and the measured drift should sit many orders below it.
 *
 * Build (host) — from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I. tests/test_fft_convolve.cpp \
 *     kernels/fft.cpp kernels/fft_convolve.cpp kernels/parallel.cpp \
 *     -o /tmp/test_fft_convolve
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernels/fft_convolve.h"
#include "kernels/parallel.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

void set_test_environment(const char* name, const char* value) {
#if defined(_WIN32)
    const int result = _putenv_s(name, value ? value : "");
#else
    const int result = value ? setenv(name, value, 1) : unsetenv(name);
#endif
    if (result != 0) throw std::runtime_error("failed to mutate test environment");
}

int reflect(int p, int n) {
    if (n == 1) return 0;
    int period = 2 * (n - 1);
    int m = p % period;
    if (m < 0) m += period;
    return m < n ? m : period - m;
}

// Verbatim from model/diffusion.cpp: build the reflect-padded plane.
std::vector<double> make_padded(const std::vector<double>& img, int w, int h,
                                int radius) {
    const int pw = w + 2 * radius, ph = h + 2 * radius;
    std::vector<double> padded(static_cast<size_t>(pw) * ph);
    for (int yy = 0; yy < ph; ++yy) {
        int sy = reflect(yy - radius, h);
        for (int xx = 0; xx < pw; ++xx) {
            int sx = reflect(xx - radius, w);
            padded[static_cast<size_t>(yy) * pw + xx] =
                img[static_cast<size_t>(sy) * w + sx];
        }
    }
    return padded;
}

// Verbatim from model/diffusion.cpp: the O(w*h*ks^2) direct loop.
std::vector<double> direct(const std::vector<double>& padded, int pw,
                           const std::vector<double>& kern, int ks, int w, int h) {
    std::vector<double> out(static_cast<size_t>(w) * h);
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
    return out;
}

// A radially-symmetric sum-of-exponentials PSF, the same SHAPE diffusion.cpp
// builds (sum_k w_k exp(-r/lk) / (2 pi lk^2)), sum-normalised.
std::vector<double> make_psf(int ks) {
    const int c = ks / 2;
    std::vector<double> k(static_cast<size_t>(ks) * ks);
    const double lam[3] = {1.7, 5.5, 14.0};
    const double wgt[3] = {0.40, 0.47, 0.13};
    double sum = 0.0;
    for (int y = 0; y < ks; ++y) {
        for (int x = 0; x < ks; ++x) {
            double dx = x - c, dy = y - c;
            double r = std::sqrt(dx * dx + dy * dy);
            double v = 0.0;
            for (int t = 0; t < 3; ++t)
                v += wgt[t] * std::exp(-r / lam[t]) /
                     (2.0 * kPi * lam[t] * lam[t]);
            k[static_cast<size_t>(y) * ks + x] = v;
            sum += v;
        }
    }
    for (double& v : k) v /= sum;
    return k;
}

struct Case { int w, h, ks, cap; const char* name; };

bool run_case(const Case& cs, std::vector<double>* fft_out) {
    const int radius = (cs.ks - 1) / 2;
    const int pw = cs.w + 2 * radius, ph = cs.h + 2 * radius;

    std::mt19937_64 rng(0xC0FFEEu + cs.w * 7919u + cs.h * 104729u + cs.ks);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> img(static_cast<size_t>(cs.w) * cs.h);
    for (size_t i = 0; i < img.size(); ++i) {
        // ~6 decades of dynamic range plus hard speculars: cancellation has
        // somewhere to hide, which a flat noise field would not give it.
        double v = std::pow(10.0, -3.0 + 4.0 * u(rng));
        if ((i % 977u) == 0) v *= 1000.0;
        img[i] = v;
    }

    const std::vector<double> kern = make_psf(cs.ks);
    const std::vector<double> padded = make_padded(img, cs.w, cs.h, radius);
    const std::vector<double> ref = direct(padded, pw, kern, cs.ks, cs.w, cs.h);

    fft_out->assign(static_cast<size_t>(cs.w) * cs.h, 0.0);
    if (!spk::fft_convolve_same(padded.data(), pw, ph, kern.data(), cs.ks, cs.w,
                                cs.h, fft_out->data(), 1, 0, cs.cap)) {
        std::printf("[%s] fft_convolve_same returned false -> FAIL\n", cs.name);
        return false;
    }

    double max_abs = 0.0, sse = 0.0, ref_max = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        double d = std::fabs((*fft_out)[i] - ref[i]);
        if (d > max_abs) max_abs = d;
        sse += d * d;
        ref_max = std::fmax(ref_max, std::fabs(ref[i]));
    }
    const double rms = std::sqrt(sse / static_cast<double>(ref.size()));
    const int n = spk::fft_convolve_transform_size(cs.w, cs.h, cs.ks, cs.cap);
    const int tiles = ((cs.h + (n - cs.ks + 1) - 1) / (n - cs.ks + 1)) *
                      ((cs.w + (n - cs.ks + 1) - 1) / (n - cs.ks + 1));
    const bool pass = (max_abs <= 1e-4) && (rms <= 1e-5);
    std::printf("[%s] N=%d tiles=%d ref_max=%.3g max_abs=%.3e rms=%.3e -> %s\n",
                cs.name, n, tiles, ref_max, max_abs, rms, pass ? "PASS" : "FAIL");
    return pass;
}

}  // namespace

int main() {
    // Without this the fixtures are below kParallelMinChunk and parallel_for runs
    // everything on one worker -- the 1-vs-8 comparison below would then be
    // comparing two single-threaded runs and would pass no matter what.
    set_test_environment("SPK_PARALLEL_MIN_CHUNK", "64");

    const Case cases[] = {
        {  64,  64,  9, 2048, "single transform, ks=9"      },
        {  96,  64, 17, 2048, "single transform, non-square"},
        { 129,  97, 21, 2048, "single transform, odd sizes" },
        // cap forced below the image => real overlap-save, multiple tiles, and a
        // partial tile at each far edge.
        { 200, 160, 15,   64, "OVERLAP-SAVE, square tiles"  },
        { 173, 111, 25,   64, "OVERLAP-SAVE, partial tiles" },
        {  61,  47, 31,   64, "OVERLAP-SAVE, ks near N"     },
        // A kernel wider than the image, which is the real Pro-Mist regime.
        {  40,  32, 41, 2048, "kernel wider than image"     },
    };

    bool ok = true;
    std::vector<double> tmp;
    for (const Case& cs : cases) ok &= run_case(cs, &tmp);

    // Thread-invariance: the engine's contract is byte-identical output for any
    // worker count. Re-run every case at 1 and at 8 workers and memcmp.
    std::printf("\n-- thread invariance (1 vs 8 workers) --\n");
    for (const Case& cs : cases) {
        std::vector<double> a, b;
        set_test_environment("SPK_NUM_THREADS", "1");
        run_case(cs, &a);
        set_test_environment("SPK_NUM_THREADS", "8");
        run_case(cs, &b);
        const bool same = a.size() == b.size() &&
                          std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
        std::printf("[%s] 1 vs 8 workers -> %s\n", cs.name,
                    same ? "PASS (byte-identical)" : "FAIL (differs)");
        ok &= same;
    }
    set_test_environment("SPK_NUM_THREADS", nullptr);

    // Deterministic scratch-denial seam: a cost-selected FFT must surface OOM,
    // not return false and invite the 10.9-hour direct fallback.
    {
        const int w = 8, h = 8, ks = 3, radius = 1;
        const int pw = w + 2 * radius, ph = h + 2 * radius;
        std::vector<double> image(static_cast<size_t>(w) * h, 0.5);
        const std::vector<double> padded = make_padded(image, w, h, radius);
        const std::vector<double> kernel = make_psf(ks);
        std::vector<double> output(static_cast<size_t>(w) * h, -7.0);
        bool denied = false;
        try {
            (void)spk::fft_convolve_same_denied_scratch_for_test(
                padded.data(), pw, ph, kernel.data(), ks, w, h,
                output.data(), 1, 0, 64);
        } catch (const std::bad_alloc&) {
            denied = true;
        }
        const bool untouched =
            std::all_of(output.begin(), output.end(), [](double v) { return v == -7.0; });
        std::printf("[scratch denial] bad_alloc=%s output_untouched=%s -> %s\n",
                    denied ? "yes" : "no", untouched ? "yes" : "no",
                    (denied && untouched) ? "PASS" : "FAIL");
        ok &= denied && untouched;
    }

    // The transform SIZE is a cost decision, and it was measured (8 workers, -O2,
    // best of two, tools in #160). Pin the decisions rather than the heuristic's
    // internals: whoever changes the model has to re-measure and update these.
    //
    //   12 MP  ks=1725  N=2048 130 tiles 9909 ms | N=4096 4 tiles 1890 ms | N=8192 1 tile 3094 ms
    //   1536   ks=651   N=1024  25 tiles  373 ms | N=2048 4 tiles  386 ms | N=4096 1 tile  767 ms
    //
    // Both directions matter: always taking the LARGEST admissible transform was
    // 5.2x too slow at 12 MP and 2.1x too slow at 1536.
    std::printf("\n-- transform-size choice (measured, not assumed) --\n");
    {
        struct Choice { int w, h, ks, cap, want; const char* why; };
        const Choice choices[] = {
            {4080, 3060, 1725, 4096, 4096, "12 MP Pro-Mist: 4 tiles beat 130"},
            {4080, 3060, 1725, 8192, 4096, "8192 is one tile but measured slower"},
            {1536, 1536,  651, 4096, 1024, "1536 px: many small tiles beat one big transform"},
            { 640,  640,  273, 4096, 1024, "default preview fits a single 1024 transform"},
            {4080, 3060, 1725, 2048, 2048, "a smaller ceiling is still honoured"},
            { 200,  160,   15,   64,   32, "a tiny ceiling still leaves a valid block"},
        };
        for (const Choice& c : choices) {
            const int got = spk::fft_convolve_transform_size(c.w, c.h, c.ks, c.cap);
            const bool pass = got == c.want;
            std::printf("[choice %dx%d ks=%d cap=%d] N=%d want=%d -> %s (%s)\n",
                        c.w, c.h, c.ks, c.cap, got, c.want, pass ? "PASS" : "FAIL", c.why);
            ok &= pass;
        }
    }

    std::printf("\n%s\n", ok ? "ALL PASS" : "FAILURES PRESENT");
    return ok ? 0 : 1;
}
