/*
 * Spektrafilm for Android — host test for the OPT-IN Highway SIMD FIR (#124).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Proves the claim the whole opt-in rests on: the Highway lanes are a BYTE-IDENTICAL
 * restatement of the scalar f32 FIR, not an approximation. Both routines vectorise
 * across output pixels with the scalar tap order and no FMA, so every output must
 * match bit-for-bit — anything else would move grain/glare output and is a bug, not
 * a tolerance.
 *
 * Also reports throughput for both paths so the same binary answers "is it worth it"
 * on the owner's device (tools/simd_bench pushes this pattern to the phone).
 *
 * Build (host, Highway ON):
 *   g++ -std=c++17 -O2 -pthread -DSPK_ENABLE_HIGHWAY -I. -I<highway_src> \
 *       tests/test_gaussian_hwy.cpp <SRC> -o /tmp/test_gaussian_hwy
 * Build (host, Highway OFF) — must still pass, exercising the stub path.
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "kernels/gaussian.h"
#include "kernels/gaussian_hwy.h"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail = 1;
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Scalar references — deliberately transcribed from kernels/gaussian.cpp so the
// comparison is against the exact arithmetic the engine performs.
void ref_vertical_accum(float* row, const float* src, int m, float kw) {
    for (int j = 0; j < m; ++j) row[j] += src[j] * kw;
}
void ref_horizontal_interior(const float* src, const float* kernel, int radius,
                             int j0, int j1, float* out) {
    for (int j = j0; j < j1; ++j) {
        float sval = 0.0f;
        for (int k = -radius; k <= radius; ++k)
            sval += src[j + k] * kernel[k + radius];
        out[j] = sval;
    }
}

std::vector<float> make_kernel(int radius, float sigma) {
    std::vector<float> k(2 * radius + 1);
    double total = 0.0;
    for (int i = 0; i < (int)k.size(); ++i) {
        double x = i - radius;
        double v = std::exp(-0.5 * (x / sigma) * (x / sigma));
        k[i] = (float)v;
        total += v;
    }
    for (auto& v : k) v = (float)(v / total);
    return k;
}

}  // namespace

int main() {
    std::printf("highway: available=%d target=%s\n",
                (int)spk::hwy_fir::available(), spk::hwy_fir::target_name());

    if (!spk::hwy_fir::available()) {
        // Stub build (or SPK_SIMD=0): the engine must still be correct, and the
        // stubs must be inert. Nothing to compare — report and pass.
        std::printf("info: Highway not active; the engine runs the scalar FIR "
                    "(this is the default build)\n");
        std::printf("test_gaussian_hwy: ALL OK (scalar path)\n");
        return 0;
    }

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-2.0f, 4.0f);

    // Bit-identity across widths that exercise the vector body, the scalar tail,
    // and the "shorter than one vector" case.
    for (int m : {1, 3, 7, 16, 17, 63, 64, 65, 1024, 4099}) {
        std::vector<float> src(m + 64), a(m + 64), b(m + 64);
        for (auto& v : src) v = dist(rng);
        for (int i = 0; i < m + 64; ++i) a[i] = b[i] = dist(rng);
        const float kw = 0.317f;
        spk::hwy_fir::vertical_accum(a.data(), src.data(), m, kw);
        ref_vertical_accum(b.data(), src.data(), m, kw);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "vertical_accum byte-identical (m=%d)", m);
        check(std::memcmp(a.data(), b.data(), (m + 64) * sizeof(float)) == 0, msg);
    }

    for (int radius : {1, 2, 5, 12}) {
        for (int m : {64, 129, 1024}) {
            if (2 * radius >= m) continue;
            const auto kern = make_kernel(radius, radius / 2.0f + 0.5f);
            std::vector<float> src(m), a(m, -1.f), b(m, -1.f);
            for (auto& v : src) v = dist(rng);
            spk::hwy_fir::horizontal_interior(src.data(), kern.data(), radius,
                                              radius, m - radius, a.data());
            ref_horizontal_interior(src.data(), kern.data(), radius,
                                    radius, m - radius, b.data());
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "horizontal_interior byte-identical (radius=%d m=%d)", radius, m);
            check(std::memcmp(a.data(), b.data(), m * sizeof(float)) == 0, msg);
        }
    }

    // Throughput on a realistic plane, both paths, same data. gaussian_blur_plane
    // itself picks the path via hwy_fir::available(), so time the primitives
    // directly to get a clean per-kernel number.
    {
        const int m = 4096, reps = 400;
        const int radius = 6;
        const auto kern = make_kernel(radius, 3.0f);
        std::vector<float> src(m + 64), dst(m + 64, 0.f);
        for (auto& v : src) v = dist(rng);

        double t0 = now_ms();
        for (int r = 0; r < reps; ++r)
            spk::hwy_fir::vertical_accum(dst.data(), src.data(), m, 0.25f);
        double t_simd_v = now_ms() - t0;
        t0 = now_ms();
        for (int r = 0; r < reps; ++r)
            ref_vertical_accum(dst.data(), src.data(), m, 0.25f);
        double t_scal_v = now_ms() - t0;

        std::vector<float> out(m);
        t0 = now_ms();
        for (int r = 0; r < reps; ++r)
            spk::hwy_fir::horizontal_interior(src.data(), kern.data(), radius,
                                              radius, m - radius, out.data());
        double t_simd_h = now_ms() - t0;
        t0 = now_ms();
        for (int r = 0; r < reps; ++r)
            ref_horizontal_interior(src.data(), kern.data(), radius,
                                    radius, m - radius, out.data());
        double t_scal_h = now_ms() - t0;

        std::printf("bench vertical  : simd %.2f ms  scalar %.2f ms  speedup %.2fx\n",
                    t_simd_v, t_scal_v, t_scal_v / t_simd_v);
        std::printf("bench horizontal: simd %.2f ms  scalar %.2f ms  speedup %.2fx  (radius=%d)\n",
                    t_simd_h, t_scal_h, t_scal_h / t_simd_h, radius);
    }

    std::printf(g_fail ? "test_gaussian_hwy: FAIL\n" : "test_gaussian_hwy: ALL OK\n");
    return g_fail;
}
