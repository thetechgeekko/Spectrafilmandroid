/*
 * Spektrafilm for Android — Halide pipeline-fusion spike. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * EXPERIMENT, not shipped code. Answers two questions the owner raised via the
 * Lightroom/Halide precedent:
 *
 *   Q1. What does pipeline FUSION actually save on OUR pipeline's shape?
 *   Q2. Does a fused, parallel, vectorised Halide pipeline hold the engine's
 *       byte-identical-across-worker-counts contract?
 *
 * WHY THE STENCIL MATTERS, and why a naive spike would mislead. Fusion is
 * spectacular on a chain of elementwise stages: no full-res temporary is ever
 * written. Our chain is NOT that — it has BLURS in the middle (halation, DIR
 * diffusion, scanner unsharp). Fusion cannot cross a stencil for free: it either
 * recomputes producers redundantly per consumer tile or needs line buffering. So
 * this measures the chain BOTH ways, with and without a stencil, because that
 * difference is the whole answer for us.
 *
 * Stages mirror the shape of filming's O(n) run: per-pixel math, a 1D LUT
 * (density curve), a 3x3 matrix (couplers), optionally a separable blur, a second
 * LUT, and an output map.
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Halide.h"

using namespace Halide;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
}

struct Built { Func out; Buffer<float> lut; };

// mode: 0 = materialised (compute_root between stages, like our engine today)
//       1 = fused (inline; Halide computes each stage per consumer tile)
static Built build(Buffer<float> in, int W, int H, bool with_blur, int mode,
                   const std::string& tag) {
    Var x("x"), y("y"), c("c"), xi("xi"), yi("yi");

    Buffer<float> lut(256);
    for (int i = 0; i < 256; ++i) lut(i) = std::pow(i / 255.0f, 2.2f);

    Func clamped("clamped" + tag);
    clamped(x, y, c) = in(clamp(x, 0, W - 1), clamp(y, 0, H - 1), c);

    // 1. expose: per-pixel math + a 1D LUT fetch
    Func expose("expose" + tag);
    Expr v = clamped(x, y, c) * 1.7f + 0.05f;
    Expr idx = clamp(cast<int>(v * 255.0f), 0, 255);
    expose(x, y, c) = lut(idx) * 0.9f + v * 0.1f;

    // 2. couplers: a 3x3 channel mix
    Func couplers("couplers" + tag);
    Expr r = expose(x, y, 0), g = expose(x, y, 1), b = expose(x, y, 2);
    couplers(x, y, c) = select(c == 0, 0.90f * r + 0.06f * g + 0.04f * b,
                               c == 1, 0.05f * r + 0.88f * g + 0.07f * b,
                                       0.03f * r + 0.09f * g + 0.88f * b);

    // 3. optional separable blur — THE STENCIL
    Func stage3("stage3" + tag);
    if (with_blur) {
        Func bx("bx" + tag);
        bx(x, y, c) = (couplers(x - 2, y, c) + 4 * couplers(x - 1, y, c) +
                       6 * couplers(x, y, c) + 4 * couplers(x + 1, y, c) +
                       couplers(x + 2, y, c)) * (1.0f / 16);
        stage3(x, y, c) = (bx(x, y - 2, c) + 4 * bx(x, y - 1, c) +
                           6 * bx(x, y, c) + 4 * bx(x, y + 1, c) +
                           bx(x, y + 2, c)) * (1.0f / 16);
        if (mode == 0) bx.compute_root();
    } else {
        stage3(x, y, c) = couplers(x, y, c);
    }

    // 4. develop: second LUT
    Func develop("develop" + tag);
    Expr d = clamp(stage3(x, y, c), 0.0f, 1.0f);
    develop(x, y, c) = lut(clamp(cast<int>(d * 255.0f), 0, 255));

    // 5. output map
    Func out("out" + tag);
    out(x, y, c) = develop(x, y, c) * 1.05f - 0.02f;

    if (mode == 0) {
        // Materialised: every stage writes a full-resolution temporary, which is
        // what our engine does today.
        expose.compute_root();
        couplers.compute_root();
        stage3.compute_root();
        develop.compute_root();
    }
    // mode 1 leaves everything inline -> fused into out's loop nest.

    out.reorder(c, x, y).bound(c, 0, 3).unroll(c);
    Var yo("yo");
    out.split(y, yo, yi, 32).parallel(yo).vectorize(x, 8);

    return {out, lut};
}

int main(int argc, char** argv) {
    const int W = argc > 1 ? std::atoi(argv[1]) : 2048;
    const int H = W;
    std::printf("# Halide fusion spike — %dx%d x3, f32\n", W, H);

    Buffer<float> in(W, H, 3);
    for (int cc = 0; cc < 3; ++cc)
        for (int yy = 0; yy < H; ++yy)
            for (int xx = 0; xx < W; ++xx)
                in(xx, yy, cc) = 0.3f + 0.4f * std::sin(0.01f * (xx + 3 * yy + 17 * cc));

    for (int blur = 0; blur < 2; ++blur) {
        std::printf("\n== chain %s ==\n",
                    blur ? "WITH a separable blur (our real shape)"
                         : "elementwise only (the flattering case)");
        double t[2];
        std::vector<float> res[2];
        for (int mode = 0; mode < 2; ++mode) {
            Built b = build(in, W, H, blur, mode,
                            "_" + std::to_string(blur) + std::to_string(mode));
            Buffer<float> o(W, H, 3);
            b.out.realize(o);                       // warm: JIT + first run
            // BEST OF N. This host is shared; single realizations swing by more
            // than the effect being measured (a first pass at 2048 read a 192x
            // fusion win and a 3.6e-03 schedule disagreement, neither of which
            // reproduced). The minimum is the least noise-contaminated estimate.
            t[mode] = 1e300;
            for (int rep = 0; rep < 5; ++rep) {
                auto t0 = Clock::now();
                b.out.realize(o);
                t[mode] = std::fmin(t[mode], ms(t0));
            }
            res[mode].assign(o.data(), o.data() + (size_t)W * H * 3);
            std::printf("  %-14s %8.1f ms\n",
                        mode ? "fused" : "materialised", t[mode]);
        }
        std::printf("  fusion speedup: %.2fx\n", t[0] / t[1]);
        double worst = 0;
        for (size_t i = 0; i < res[0].size(); ++i)
            worst = std::fmax(worst, std::fabs((double)res[0][i] - res[1][i]));
        std::printf("  materialised vs fused: max_abs = %.3e\n", worst);

        // Plain-C++ reference for the elementwise chain, so a disagreement between
        // the two schedules can be attributed rather than merely noted.
        if (!blur) {
            std::vector<float> lutv(256);
            for (int i = 0; i < 256; ++i) lutv[i] = std::pow(i / 255.0f, 2.2f);
            auto expose1 = [&](int xx, int yy, int cc) {
                float v = in(xx, yy, cc) * 1.7f + 0.05f;
                int i = (int)(v * 255.0f); i = i < 0 ? 0 : (i > 255 ? 255 : i);
                return lutv[i] * 0.9f + v * 0.1f;
            };
            double dm[2] = {0, 0};
            for (int yy = 0; yy < H; ++yy)
                for (int xx = 0; xx < W; ++xx) {
                    float e0 = expose1(xx, yy, 0), e1 = expose1(xx, yy, 1),
                          e2 = expose1(xx, yy, 2);
                    float mix[3] = {0.90f*e0 + 0.06f*e1 + 0.04f*e2,
                                    0.05f*e0 + 0.88f*e1 + 0.07f*e2,
                                    0.03f*e0 + 0.09f*e1 + 0.88f*e2};
                    for (int cc = 0; cc < 3; ++cc) {
                        float d = mix[cc] < 0 ? 0 : (mix[cc] > 1 ? 1 : mix[cc]);
                        int i = (int)(d * 255.0f); i = i < 0 ? 0 : (i > 255 ? 255 : i);
                        float ref = lutv[i] * 1.05f - 0.02f;
                        size_t k = ((size_t)cc * H + yy) * W + xx;
                        for (int m = 0; m < 2; ++m)
                            dm[m] = std::fmax(dm[m], std::fabs((double)res[m][k] - ref));
                    }
                }
            std::printf("  vs plain C++: materialised %.3e   fused %.3e\n", dm[0], dm[1]);
            // If the disagreement is a LUT-INDEX CLIFF -- a 1-ULP difference in the
            // index expression flipping to the neighbouring table entry -- then only
            // a tiny fraction of pixels differ, and each differs by about one LUT
            // step. If instead the arithmetic itself diverged, the differences would
            // be widespread and small. Counting separates the two.
            size_t ndiff = 0; double sum = 0;
            for (size_t i = 0; i < res[0].size(); ++i) {
                double d = std::fabs((double)res[0][i] - res[1][i]);
                if (d > 1e-7) { ++ndiff; sum += d; }
            }
            if (ndiff) {
                const double step = (std::pow(129/255.0, 2.2) - std::pow(128/255.0, 2.2)) * 1.05;
                std::printf("  differing samples: %zu / %zu (%.4f%%), mean delta %.3e,"
                            " one LUT step ~ %.3e\n",
                            ndiff, res[0].size(),
                            100.0 * ndiff / res[0].size(), sum / ndiff, step);
            }
        }
    }

    // Q2: determinism across Halide worker counts, on the fused+blur pipeline.
    std::printf("\n== determinism: fused + blur, 1 vs 8 Halide threads ==\n");
    std::vector<float> runs[2];
    int idx = 0;
    for (const char* nt : {"1", "8"}) {
        setenv("HL_NUM_THREADS", nt, 1);
        Internal::JITSharedRuntime::release_all();
        Built b = build(in, W, H, true, 1, std::string("_det") + nt);
        Buffer<float> o(W, H, 3);
        b.out.realize(o);
        runs[idx].assign(o.data(), o.data() + (size_t)W * H * 3);
        ++idx;
    }
    const bool same = std::memcmp(runs[0].data(), runs[1].data(),
                                  runs[0].size() * sizeof(float)) == 0;
    std::printf("  HL_NUM_THREADS 1 vs 8: %s\n",
                same ? "byte-identical" : "DIFFERS");
    return 0;
}
