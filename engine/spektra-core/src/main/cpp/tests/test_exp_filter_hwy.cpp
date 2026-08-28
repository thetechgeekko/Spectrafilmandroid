/*
 * Spektrafilm for Android — host test for the OPT-IN Highway f64 SIMD lanes on
 * the halation / scatter spatial tier (perf-lab, #124 follow-on).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * WHY THIS TIER: kernels/exponential_filter.cpp is what halation runs through
 * (model/diffusion.cpp -> exponential_filter_per_channel_d), and halation is ON by
 * default. The earlier Highway round covered only the f32 FIR (grain / glare), so
 * the default-ON spatial cost was never touched. This test is the correctness half
 * of finding out whether covering it is worth shipping.
 *
 * WHAT IT PROVES: each of the four f64 lane routines is a BYTE-IDENTICAL
 * restatement of the scalar loop it replaces — same accumulation order, no fused
 * multiply-add. Not a tolerance: any difference would move halation output on every
 * render and is a bug.
 *
 * The IIR step is the one that matters most and is the least obvious: the Young &
 * van Vliet recurrence runs DOWN rows, so columns are independent and lane-parallel,
 * but the four-term sum must keep C++'s left-to-right association exactly.
 *
 * It also prints a checksum of a full exponential_filter_per_channel_d run plus
 * per-routine throughput, so running the SAME binary twice (once with SPK_SIMD=0)
 * gives both an end-to-end equality check and the speed answer. available() caches
 * its env read, so that A/B has to be two processes — which is what
 * tools/perf_lab/build_push_run.sh does.
 *
 * Build (host, Highway ON):
 *   g++ -std=c++17 -O2 -pthread -DSPK_ENABLE_HIGHWAY -I. -I<highway_src> \
 *       tests/test_exp_filter_hwy.cpp <SRC> -o /tmp/test_exp_filter_hwy
 * Build (host, Highway OFF) — must still pass, exercising the stub path.
 */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "kernels/exponential_filter.h"
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

// ---- Scalar references, transcribed from kernels/exponential_filter.cpp so the
// ---- comparison is against the exact arithmetic the engine performs. ----------
void ref_vertical_accum(double* row, const double* src, int m, double kw) {
    for (int j = 0; j < m; ++j) row[j] += src[j] * kw;
}

void ref_horizontal_interior(const double* src, const double* kernel, int radius,
                             int j0, int j1, double* out) {
    for (int j = j0; j < j1; ++j) {
        double sval = 0.0;
        for (int k = -radius; k <= radius; ++k)
            sval += src[j + k] * kernel[k + radius];
        out[j] = sval;
    }
}

void ref_iir_step(double* row, double* s1, double* s2, double* s3, int mc,
                  double B, double B1, double B2, double B3) {
    for (int j = 0; j < mc; ++j) {
        const double x = row[j];
        const double val = B * x + B1 * s1[j] + B2 * s2[j] + B3 * s3[j];
        row[j] = val;
        s3[j] = s2[j];
        s2[j] = s1[j];
        s1[j] = val;
    }
}

void ref_axpy(double* out, const double* src, int n, double a) {
    for (int i = 0; i < n; ++i) out[i] += a * src[i];
}

bool bytes_eq(const std::vector<double>& a, const std::vector<double>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}

std::vector<double> make_noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = u(rng);
    return v;
}

std::vector<double> make_kernel(int radius, double sigma) {
    std::vector<double> k(2 * radius + 1);
    double total = 0.0;
    for (int i = 0; i < 2 * radius + 1; ++i) {
        const double x = static_cast<double>(i - radius);
        k[i] = std::exp(-0.5 * (x / sigma) * (x / sigma));
        total += k[i];
    }
    for (auto& v : k) v /= total;
    return k;
}

// FNV-1a over the raw bytes — a stable cross-process fingerprint of a buffer.
uint64_t checksum(const std::vector<double>& v) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const unsigned char*>(v.data());
    for (size_t i = 0; i < v.size() * sizeof(double); ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Lengths chosen to straddle every lane boundary Highway can pick (1, 2, 4, 8)
// and to include prime-ish tails that exercise the scalar remainder loop.
const int kLens[] = {1, 2, 3, 5, 8, 9, 15, 16, 17, 63, 64, 65, 1023, 4099};

void test_vertical_accum() {
    bool all_eq = true;
    for (int m : kLens) {
        auto src = make_noise(static_cast<size_t>(m), 11u + m);
        auto base = make_noise(static_cast<size_t>(m), 77u + m);
        std::vector<double> a = base, b = base;
        const double kw = 0.31830988618379067;
        ref_vertical_accum(a.data(), src.data(), m, kw);
        spk::hwy_f64::vertical_accum(b.data(), src.data(), m, kw);
        if (!spk::hwy_fir::available()) b = a;  // stub build: nothing to compare
        if (!bytes_eq(a, b)) {
            std::printf("  mismatch at m=%d\n", m);
            all_eq = false;
        }
    }
    check(all_eq, "f64 vertical_accum byte-identical to scalar (all lengths)");
}

void test_horizontal_interior() {
    bool all_eq = true;
    for (int radius : {1, 2, 3, 5, 12, 31}) {
        for (int m : kLens) {
            const int j0 = radius, j1 = m - radius;
            if (j1 <= j0) continue;  // span exists only when 2*radius < m
            auto src = make_noise(static_cast<size_t>(m), 101u + m + radius);
            auto kern = make_kernel(radius, 0.4 * radius + 0.5);
            std::vector<double> a(m, 0.0), b(m, 0.0);
            ref_horizontal_interior(src.data(), kern.data(), radius, j0, j1, a.data());
            spk::hwy_f64::horizontal_interior(src.data(), kern.data(), radius, j0, j1,
                                              b.data());
            if (!spk::hwy_fir::available()) b = a;
            if (!bytes_eq(a, b)) {
                std::printf("  mismatch at m=%d radius=%d\n", m, radius);
                all_eq = false;
            }
        }
    }
    check(all_eq, "f64 horizontal_interior byte-identical to scalar (all lengths)");
}

void test_iir_step() {
    // Coefficients from yvv_coeffs(sigma) for a halation-scale sigma; the exact
    // values do not matter to the equality claim, only that all four are live.
    const double B = 0.0838, B1 = 1.5537, B2 = -0.7995, B3 = 0.1620;
    bool all_eq = true;
    for (int mc : kLens) {
        auto row0 = make_noise(static_cast<size_t>(mc), 5u + mc);
        auto st = make_noise(static_cast<size_t>(mc) * 3, 909u + mc);
        std::vector<double> ra = row0, rb = row0;
        std::vector<double> a1(st.begin(), st.begin() + mc), a2(st.begin() + mc, st.begin() + 2 * mc),
            a3(st.begin() + 2 * mc, st.end());
        std::vector<double> b1 = a1, b2 = a2, b3 = a3;
        // Several steps in sequence: the state carries forward, so a single
        // divergent lane would compound rather than cancel.
        for (int step = 0; step < 4; ++step) {
            ref_iir_step(ra.data(), a1.data(), a2.data(), a3.data(), mc, B, B1, B2, B3);
            spk::hwy_f64::iir_step(rb.data(), b1.data(), b2.data(), b3.data(), mc, B,
                                   B1, B2, B3);
        }
        if (!spk::hwy_fir::available()) { rb = ra; b1 = a1; b2 = a2; b3 = a3; }
        if (!bytes_eq(ra, rb) || !bytes_eq(a1, b1) || !bytes_eq(a2, b2) ||
            !bytes_eq(a3, b3)) {
            std::printf("  mismatch at mc=%d\n", mc);
            all_eq = false;
        }
    }
    check(all_eq, "f64 iir_step byte-identical to scalar, row+state (all lengths)");
}

void test_axpy() {
    bool all_eq = true;
    for (int n : kLens) {
        auto src = make_noise(static_cast<size_t>(n), 3u + n);
        auto base = make_noise(static_cast<size_t>(n), 44u + n);
        std::vector<double> a = base, b = base;
        const double amp = 0.6496;  // kExpAmplitude[1]
        ref_axpy(a.data(), src.data(), n, amp);
        spk::hwy_f64::axpy(b.data(), src.data(), n, amp);
        if (!spk::hwy_fir::available()) b = a;
        if (!bytes_eq(a, b)) {
            std::printf("  mismatch at n=%d\n", n);
            all_eq = false;
        }
    }
    check(all_eq, "f64 axpy byte-identical to scalar (all lengths)");
}

// End-to-end: a real halation-shaped call. The checksum is the cross-process
// equality check (run the binary again with SPK_SIMD=0 and compare); the timing
// is the answer to "is the f64 tier worth shipping".
void bench_e2e(int w, int h) {
    const int channels = 3;
    auto img = make_noise(static_cast<size_t>(w) * h * channels, 20260828u);
    std::vector<double> out(img.size(), 0.0);
    // Halation decay in pixels, per channel — red scatters furthest, which is
    // what makes the red halo. Large enough that the IIR branch is the one taken.
    const double decay[3] = {14.0, 9.0, 6.0};

    spk::exponential_filter_per_channel_d(img.data(), w, h, channels, decay,
                                          out.data());  // warm
    const double t0 = now_ms();
    const int reps = 3;
    for (int r = 0; r < reps; ++r)
        spk::exponential_filter_per_channel_d(img.data(), w, h, channels, decay,
                                              out.data());
    const double t1 = now_ms();
    std::printf("bench exponential_filter %dx%d x%d: %.2f ms/call  checksum=%016llx\n",
                w, h, channels, (t1 - t0) / reps,
                static_cast<unsigned long long>(checksum(out)));
}

}  // namespace

int main() {
    std::printf("highway: compiled_in=%s target=%s f64_lanes=%d\n",
                spk::hwy_fir::available() ? "yes" : "no/disabled",
                spk::hwy_fir::target_name(), spk::hwy_f64::lanes());

    test_vertical_accum();
    test_horizontal_interior();
    test_iir_step();
    test_axpy();

    bench_e2e(640, 480);
    bench_e2e(1024, 768);

    std::printf(g_fail ? "test_exp_filter_hwy: FAIL\n" : "test_exp_filter_hwy: ALL OK\n");
    return g_fail;
}
