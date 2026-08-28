/*
 * Spektrafilm for Android — perf lab: the levers we had never measured.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * A MEASUREMENT TOOL, not an engine change. Nothing here is wired into the render
 * path: each lever is transcribed next to the code it would replace so the answer
 * to "is it worth the parity risk" is a number instead of an argument. Every lever
 * reports BOTH a speedup and a max_abs deviation from the exact path, because a
 * speedup whose error exceeds the oracle band (1e-4 / rms 1e-5) is not a speedup,
 * it is a different renderer.
 *
 * The three levers, and why each was never tried:
 *
 *  A. SPECTRAL INTEGRAL AS BATCHED MATRIX OPS. scanning.cpp evaluates, per pixel,
 *     spectral[l] = c0*cd[l][0] + c1*cd[l][1] + c2*cd[l][2] + base[l] over 81 bands,
 *     then w = exp10(-spectral)*illum, then X/Y/Z = sum_l w[l]*CMF[l][k]. That is a
 *     (N x 3)(3 x 81) product, an elementwise transcendental, and an (N x 81)(81 x 3)
 *     reduction — two GEMMs with a nonlinearity between them. The independent Rust
 *     port of this same engine reports its largest single win from expressing exactly
 *     this as BLAS dgemm. We never measured it. Lever A blocks the loop over pixel
 *     tiles so the band axis is walked contiguously, which is the cache behaviour a
 *     GEMM would give, without taking a BLAS dependency to find out.
 *
 *     PARITY NOTE: the reduction order over l is unchanged here (ascending, as in
 *     the engine), so this variant should be byte-identical; the reported deviation
 *     is the check on that, not a tolerance.
 *
 *  B. DIFFUSION PSF VIA GAUSSIAN MIXTURE INSTEAD OF DIRECT O(ks^2) CONVOLUTION.
 *     model/diffusion.cpp convolves a ks x ks radial PSF directly; PERF_ROADMAP
 *     measured that path at 13.4 s for one 640x480 render even after
 *     parallelisation, and parked it as "replacing the algorithm is a separate,
 *     parity-affecting decision". But the PSF is a weighted sum of 2D isotropic
 *     exponentials, and kernels/exponential_filter.cpp ALREADY approximates that
 *     exact function with a 3-Gaussian mixture — the engine sanctions the
 *     approximation for halation and pays O(ks^2) for the same shape here. Lever B
 *     measures what the mixture costs in accuracy and saves in time.
 *
 *  D. THE IRREGULAR KERNELS, PROFILED AS A CLASS -- the gap a review of this branch
 *     surfaced. Both of our SIMD/codegen efforts landed on REGULAR work: Highway on
 *     the f32 FIR and f64 IIR, Halide on the ks x ks PSF convolution. The genuinely
 *     irregular kernels -- the Poisson/Binomial particle samplers in kernels/stats.cpp
 *     that grain is built on -- got neither, and were never profiled as a class. That
 *     matters because the device round found grain + halation holding the preview
 *     time: halation is now covered (lever 1 in the branch), grain is not.
 *
 *     What makes them irregular is not incidental. fast_poisson_one branches on
 *     lam >= 30 and fast_binomial_one on n < 25 and then on n*p*(1-p) > 10, so the
 *     work per sample depends on the DATA, and each branch consumes a different
 *     number of draws from a serial std::mt19937. Lanes cannot run in lockstep on a
 *     shared RNG stream when each lane wants a different count -- which is why the
 *     usual SIMD answer does not apply here, and why the fixed-block seeding contract
 *     (what makes 12 MP 1-vs-8-worker output memcmp-identical) exists in the first
 *     place. This lever measures the cost per regime, and the penalty when the regime
 *     varies per sample the way it does in a real render.
 *
 *  C. fp16 STORAGE FOR THE SPATIAL PLANES. PERF_ROADMAP item 3, written down and
 *     never attempted, though kernels/half.h has shipped exact IEEE-754 conversions
 *     the whole time. The blur planes are float64: 8 bytes per sample through a
 *     bandwidth-bound filter. Lever C sweeps f64 (today) / f32 / fp16 storage
 *     through the same IIR blur and reports time and error for each.
 *
 * Build (host):
 *   g++ -std=c++17 -O2 -pthread -I<cpp> tools/perf_lab/perf_lab.cpp \
 *       <cpp>/kernels/exponential_filter.cpp <cpp>/kernels/parallel.cpp \
 *       <cpp>/kernels/half.cpp <cpp>/kernels/gaussian_hwy.cpp -o perf_lab
 * Device build + run: tools/perf_lab/build_push_run.sh
 */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "kernels/exponential_filter.h"
#include "kernels/gaussian.h"
#include "kernels/half.h"
#include "kernels/stats.h"

namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Dev {
    double max_abs = 0.0;
    double rms = 0.0;
};

template <typename A, typename B>
Dev deviation(const A& a, const B& b, size_t n) {
    Dev d;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double e = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (e > d.max_abs) d.max_abs = e;
        acc += e * e;
    }
    d.rms = n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
    return d;
}

// The oracle band the engine is held to (CLAUDE.md): max_abs <= 1e-4, rms <= 1e-5.
const char* verdict(const Dev& d) {
    if (d.max_abs == 0.0) return "EXACT";
    if (d.max_abs <= 1e-4 && d.rms <= 1e-5) return "within oracle band";
    return "OUTSIDE oracle band";
}

// ===========================================================================
// Lever A — spectral integral: per-pixel loop vs pixel-tiled batched form
// ===========================================================================
namespace lever_a {

constexpr int NB = 81;  // bands, as in the engine (kGpuNB / kSpectralSamples)

struct Tables {
    std::vector<double> cd;    // NB*3 channel_density
    std::vector<double> base;  // NB   base_density
    std::vector<double> illum; // NB   illuminant
    std::vector<double> cmf;   // NB*3 CIE 1931
};

// Synthetic tables with the magnitudes real profiles carry: dye densities O(1),
// a smooth illuminant, and CMF-shaped lobes. The band COUNT and the value RANGE
// are what set both the cost and the conditioning of the sum, so the timing is
// representative and the deviation is indicative (a real profile would shift the
// last digits, not the verdict).
Tables make_tables() {
    Tables t;
    t.cd.resize(NB * 3);
    t.base.resize(NB);
    t.illum.resize(NB);
    t.cmf.resize(NB * 3);
    for (int b = 0; b < NB; ++b) {
        const double x = static_cast<double>(b) / (NB - 1);
        t.cd[b * 3 + 0] = 0.15 + 1.10 * std::exp(-40.0 * (x - 0.72) * (x - 0.72));
        t.cd[b * 3 + 1] = 0.12 + 1.05 * std::exp(-40.0 * (x - 0.46) * (x - 0.46));
        t.cd[b * 3 + 2] = 0.10 + 1.00 * std::exp(-40.0 * (x - 0.20) * (x - 0.20));
        t.base[b] = 0.05 + 0.10 * x;
        t.illum[b] = 0.8 + 0.4 * std::sin(3.0 * x);
        t.cmf[b * 3 + 0] = std::exp(-30.0 * (x - 0.62) * (x - 0.62));
        t.cmf[b * 3 + 1] = std::exp(-25.0 * (x - 0.50) * (x - 0.50));
        t.cmf[b * 3 + 2] = std::exp(-45.0 * (x - 0.22) * (x - 0.22));
    }
    return t;
}

// Transcribed from runtime/stages/scanning.cpp: one pixel at a time, all 81 bands
// walked per pixel, X/Y/Z accumulated in band order.
void per_pixel(const double* cmy, int npix, const Tables& t, double* xyz) {
    for (int p = 0; p < npix; ++p) {
        const double c0 = cmy[p * 3], c1 = cmy[p * 3 + 1], c2 = cmy[p * 3 + 2];
        double X = 0.0, Y = 0.0, Z = 0.0;
        for (int l = 0; l < NB; ++l) {
            const double* cd = &t.cd[l * 3];
            const double spectral = c0 * cd[0] + c1 * cd[1] + c2 * cd[2] + t.base[l];
            double w = std::pow(10.0, -spectral) * t.illum[l];
            if (std::isnan(w)) w = 0.0;
            X += w * t.cmf[l * 3 + 0];
            Y += w * t.cmf[l * 3 + 1];
            Z += w * t.cmf[l * 3 + 2];
        }
        xyz[p * 3 + 0] = X;
        xyz[p * 3 + 1] = Y;
        xyz[p * 3 + 2] = Z;
    }
}

// Planar reduction tables: the band-major cmf[l*3+k] layout forces phase 3 to
// stride by 3, which is what stops a compiler (and a BLAS kernel) from using
// multiple accumulators. Transposing to three contiguous band vectors is the
// layout an actual GEMM would consume, so the "would BLAS help" question needs
// this variant to be answered honestly — the tiled-only variant measures cache
// blocking alone.
struct Planar {
    double x[NB], y[NB], z[NB];
};
Planar make_planar(const Tables& t) {
    Planar p;
    for (int l = 0; l < NB; ++l) {
        p.x[l] = t.cmf[l * 3 + 0];
        p.y[l] = t.cmf[l * 3 + 1];
        p.z[l] = t.cmf[l * 3 + 2];
    }
    return p;
}

// Tiled AND planar: phase 3 becomes three unit-stride dot products over 81
// contiguous doubles, which is exactly a (P x 81)(81 x 3) GEMM's inner shape.
// NOTE the four independent accumulators per channel REASSOCIATE the sum, so
// unlike the tiled-only variant this one is NOT expected to be bit-exact — the
// reported max_abs is the parity cost of the technique, which is the number the
// decision actually needs.
void tiled_planar(const double* cmy, int npix, const Tables& t, const Planar& pl,
                  double* xyz, int P) {
    std::vector<double> blk(static_cast<size_t>(P) * NB);
    for (int p0 = 0; p0 < npix; p0 += P) {
        const int np = (p0 + P <= npix) ? P : (npix - p0);
        for (int i = 0; i < np; ++i) {
            const double c0 = cmy[(p0 + i) * 3];
            const double c1 = cmy[(p0 + i) * 3 + 1];
            const double c2 = cmy[(p0 + i) * 3 + 2];
            double* row = &blk[static_cast<size_t>(i) * NB];
            for (int l = 0; l < NB; ++l) {
                const double* cd = &t.cd[l * 3];
                const double spectral = c0 * cd[0] + c1 * cd[1] + c2 * cd[2] + t.base[l];
                double w = std::pow(10.0, -spectral) * t.illum[l];
                if (std::isnan(w)) w = 0.0;
                row[l] = w;
            }
        }
        for (int i = 0; i < np; ++i) {
            const double* row = &blk[static_cast<size_t>(i) * NB];
            double X0 = 0, X1 = 0, Y0 = 0, Y1 = 0, Z0 = 0, Z1 = 0;
            int l = 0;
            for (; l + 2 <= NB; l += 2) {
                X0 += row[l] * pl.x[l];       Y0 += row[l] * pl.y[l];
                Z0 += row[l] * pl.z[l];
                X1 += row[l + 1] * pl.x[l + 1]; Y1 += row[l + 1] * pl.y[l + 1];
                Z1 += row[l + 1] * pl.z[l + 1];
            }
            double X = X0 + X1, Y = Y0 + Y1, Z = Z0 + Z1;
            for (; l < NB; ++l) {
                X += row[l] * pl.x[l];
                Y += row[l] * pl.y[l];
                Z += row[l] * pl.z[l];
            }
            xyz[(p0 + i) * 3 + 0] = X;
            xyz[(p0 + i) * 3 + 1] = Y;
            xyz[(p0 + i) * 3 + 2] = Z;
        }
    }
}

// The batched form: a tile of P pixels at a time. Phase 1 fills a P x NB block
// (the (P x 3)(3 x 81) product plus the base bias); phase 2 is the elementwise
// transcendental over the whole block; phase 3 reduces it against the CMF
// (the (P x 81)(81 x 3) product). Band order within each pixel is unchanged, so
// this must come out EXACT — the deviation line is the proof, not a tolerance.
void tiled(const double* cmy, int npix, const Tables& t, double* xyz, int P) {
    std::vector<double> blk(static_cast<size_t>(P) * NB);
    for (int p0 = 0; p0 < npix; p0 += P) {
        const int np = (p0 + P <= npix) ? P : (npix - p0);
        // Phase 1+2 fused: the transcendental is the dominant cost, and keeping it
        // in the same sweep avoids a second pass over the block.
        for (int i = 0; i < np; ++i) {
            const double c0 = cmy[(p0 + i) * 3];
            const double c1 = cmy[(p0 + i) * 3 + 1];
            const double c2 = cmy[(p0 + i) * 3 + 2];
            double* row = &blk[static_cast<size_t>(i) * NB];
            for (int l = 0; l < NB; ++l) {
                const double* cd = &t.cd[l * 3];
                const double spectral = c0 * cd[0] + c1 * cd[1] + c2 * cd[2] + t.base[l];
                double w = std::pow(10.0, -spectral) * t.illum[l];
                if (std::isnan(w)) w = 0.0;
                row[l] = w;
            }
        }
        // Phase 3: reduction against the CMF, band-ascending per pixel.
        for (int i = 0; i < np; ++i) {
            const double* row = &blk[static_cast<size_t>(i) * NB];
            double X = 0.0, Y = 0.0, Z = 0.0;
            for (int l = 0; l < NB; ++l) {
                X += row[l] * t.cmf[l * 3 + 0];
                Y += row[l] * t.cmf[l * 3 + 1];
                Z += row[l] * t.cmf[l * 3 + 2];
            }
            xyz[(p0 + i) * 3 + 0] = X;
            xyz[(p0 + i) * 3 + 1] = Y;
            xyz[(p0 + i) * 3 + 2] = Z;
        }
    }
}

void run(int npix) {
    const Tables t = make_tables();
    std::mt19937 rng(20260828u);
    std::uniform_real_distribution<double> u(-0.05, 2.4);  // density_cmy range
    std::vector<double> cmy(static_cast<size_t>(npix) * 3);
    for (auto& v : cmy) v = u(rng);
    std::vector<double> ref(static_cast<size_t>(npix) * 3);
    std::vector<double> got(static_cast<size_t>(npix) * 3);

    per_pixel(cmy.data(), npix, t, ref.data());  // warm
    double t0 = now_ms();
    per_pixel(cmy.data(), npix, t, ref.data());
    const double ms_ref = now_ms() - t0;

    std::printf("\n[A] spectral integral, %d px, %d bands (1 thread)\n", npix, NB);
    std::printf("    per-pixel loop (engine today) : %8.2f ms\n", ms_ref);
    for (int P : {32, 128, 512}) {
        tiled(cmy.data(), npix, t, got.data(), P);  // warm
        t0 = now_ms();
        tiled(cmy.data(), npix, t, got.data(), P);
        const double ms = now_ms() - t0;
        const Dev d = deviation(got, ref, got.size());
        std::printf("    tiled P=%-4d                  : %8.2f ms  (%.2fx)  "
                    "max_abs=%.3e  %s\n",
                    P, ms, ms_ref / ms, d.max_abs, verdict(d));
    }
    const Planar pl = make_planar(t);
    for (int P : {32, 128, 512}) {
        tiled_planar(cmy.data(), npix, t, pl, got.data(), P);  // warm
        t0 = now_ms();
        tiled_planar(cmy.data(), npix, t, pl, got.data(), P);
        const double ms = now_ms() - t0;
        const Dev d = deviation(got, ref, got.size());
        std::printf("    tiled+planar P=%-4d (GEMM-like): %8.2f ms  (%.2fx)  "
                    "max_abs=%.3e  %s\n",
                    P, ms, ms_ref / ms, d.max_abs, verdict(d));
    }
    std::printf("    NOTE: the engine's real loop uses the vector exp10 of\n"
                "          kernels/exp10.h, not std::pow, so its transcendental is\n"
                "          cheaper than this reference's and the reduction weighs\n"
                "          MORE there than here. Read the ratios, not the absolutes.\n");
}

}  // namespace lever_a

// ===========================================================================
// Lever B — radial-exponential PSF: direct O(ks^2) vs Gaussian-mixture separable
// ===========================================================================
namespace lever_b {

// The engine's own 3-Gaussian fit for the 2D isotropic exponential
// (kernels/exponential_filter.cpp, _EXPONENTIAL_GAUSSIAN_FITS[3]).
constexpr double kAmp[3] = {0.1633, 0.6496, 0.1870};
constexpr double kSig[3] = {0.5360, 1.5236, 2.7684};

// Direct convolution with the radial PSF, transcribed from
// model/diffusion.cpp::apply_diffusion_filter_um's inner loop (single channel).
void direct(const std::vector<double>& src, int w, int h, int radius, double lambda_px,
            std::vector<double>* out) {
    const int ks = 2 * radius + 1;
    std::vector<double> psf(static_cast<size_t>(ks) * ks);
    double sum = 0.0;
    const int cy = ks / 2, cx = ks / 2;
    for (int yy = 0; yy < ks; ++yy)
        for (int xx = 0; xx < ks; ++xx) {
            const double r = std::sqrt(static_cast<double>((yy - cy) * (yy - cy) +
                                                           (xx - cx) * (xx - cx)));
            const double lk = lambda_px > 1e-6 ? lambda_px : 1e-6;
            const double v = std::exp(-r / lk) / (2.0 * M_PI * lk * lk);
            psf[static_cast<size_t>(yy) * ks + xx] = v;
            sum += v;
        }
    for (auto& v : psf) v /= sum;

    out->assign(src.size(), 0.0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = 0; i < ks; ++i) {
                int sy = y + i - radius;
                if (sy < 0) sy = -sy;                       // reflect
                if (sy >= h) sy = 2 * h - 2 - sy;
                for (int j = 0; j < ks; ++j) {
                    int sx = x + j - radius;
                    if (sx < 0) sx = -sx;
                    if (sx >= w) sx = 2 * w - 2 - sx;
                    acc += src[static_cast<size_t>(sy) * w + sx] *
                           psf[static_cast<size_t>(ks - 1 - i) * ks + (ks - 1 - j)];
                }
            }
            (*out)[static_cast<size_t>(y) * w + x] = acc;
        }
}

// The same PSF as a 3-Gaussian mixture, each term run through the engine's own
// separable/IIR blur — O(w*h) per term instead of O(w*h*ks^2) once.
void mixture(const std::vector<double>& src, int w, int h, double lambda_px,
             std::vector<double>* out) {
    out->assign(src.size(), 0.0);
    std::vector<double> comp(src.size());
    for (int k = 0; k < 3; ++k) {
        std::memcpy(comp.data(), src.data(), src.size() * sizeof(double));
        spk::gaussian_blur_plane_d(comp.data(), w, h, kSig[k] * lambda_px);
        const double a = kAmp[k];
        for (size_t i = 0; i < comp.size(); ++i) (*out)[i] += a * comp[i];
    }
}

void run(int w, int h, double lambda_px) {
    // radius = ceil(max(8*lambda_px, 5)), as in apply_diffusion_filter_um.
    int radius = static_cast<int>(std::ceil(std::fmax(8.0 * lambda_px, 5.0)));
    const int cap = (h < w ? h : w) / 2 - 1;
    if (radius > cap) radius = cap;

    std::mt19937 rng(7u);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> src(static_cast<size_t>(w) * h);
    for (auto& v : src) v = u(rng);

    std::vector<double> a, b;
    double t0 = now_ms();
    direct(src, w, h, radius, lambda_px, &a);
    const double ms_direct = now_ms() - t0;

    mixture(src, w, h, lambda_px, &b);  // warm
    t0 = now_ms();
    mixture(src, w, h, lambda_px, &b);
    const double ms_mix = now_ms() - t0;

    const Dev d = deviation(b, a, a.size());
    std::printf("\n[B] radial-exponential PSF, %dx%d, lambda=%.1f px, ks=%d\n", w, h,
                lambda_px, 2 * radius + 1);
    std::printf("    direct O(ks^2) (engine today) : %8.2f ms\n", ms_direct);
    std::printf("    3-Gaussian mixture, separable : %8.2f ms  (%.1fx)  "
                "max_abs=%.3e rms=%.3e  %s\n",
                ms_mix, ms_direct / ms_mix, d.max_abs, d.rms, verdict(d));
    std::printf("    NOTE: the mixture is the SAME approximation the engine already\n"
                "          accepts for halation; the number above is what it costs\n"
                "          when applied to the diffusion PSF instead.\n");
}

}  // namespace lever_b

// ===========================================================================
// Lever C — storage precision for the spatial planes: f64 vs f32 vs fp16
// ===========================================================================
namespace lever_c {

void run(int w, int h, double sigma) {
    const size_t n = static_cast<size_t>(w) * h;
    std::mt19937 rng(99u);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> base(n);
    for (auto& v : base) v = u(rng);

    // f64 plane through the f64 filter — what the halation tier does today.
    std::vector<double> f64buf = base;
    spk::gaussian_blur_plane_d(f64buf.data(), w, h, sigma);  // warm
    f64buf = base;
    double t0 = now_ms();
    spk::gaussian_blur_plane_d(f64buf.data(), w, h, sigma);
    const double ms64 = now_ms() - t0;

    // f32 plane through the ENGINE'S OWN f32 filter (kernels/gaussian.cpp) — the
    // real bandwidth comparison, not a widened round-trip. Half the bytes moved
    // and half the arithmetic width, end to end.
    std::vector<float> f32base(n);
    for (size_t i = 0; i < n; ++i) f32base[i] = static_cast<float>(base[i]);
    std::vector<float> f32buf = f32base;
    spk::gaussian_blur_plane(f32buf.data(), w, h, static_cast<float>(sigma));  // warm
    f32buf = f32base;
    t0 = now_ms();
    spk::gaussian_blur_plane(f32buf.data(), w, h, static_cast<float>(sigma));
    const double ms32 = now_ms() - t0;
    const Dev d32 = deviation(f32buf, f64buf, n);

    // fp16 STORAGE around the f32 filter: quantise the plane to binary16, widen,
    // filter at f32. This is the storage lever on its own; the conversion cost is
    // included because shipping it would pay that cost too.
    std::vector<uint16_t> h16(n);
    std::vector<float> f16buf(n);
    t0 = now_ms();
    spk::f32_to_f16(f32base.data(), h16.data(), n);
    spk::f16_to_f32(h16.data(), f16buf.data(), n);
    spk::gaussian_blur_plane(f16buf.data(), w, h, static_cast<float>(sigma));
    const double ms16 = now_ms() - t0;
    const Dev d16 = deviation(f16buf, f64buf, n);

    std::printf("\n[C] spatial-plane precision, %dx%d, sigma=%.1f (half_simd=%s)\n",
                w, h, sigma, spk::half_is_simd() ? "yes" : "no");
    std::printf("    f64 plane + f64 filter (today): %8.2f ms   (reference)\n", ms64);
    std::printf("    f32 plane + f32 filter        : %8.2f ms  (%.2fx)  "
                "max_abs=%.3e rms=%.3e  %s\n",
                ms32, ms64 / ms32, d32.max_abs, d32.rms, verdict(d32));
    std::printf("    fp16 storage + f32 filter     : %8.2f ms  (%.2fx)  "
                "max_abs=%.3e rms=%.3e  %s\n",
                ms16, ms64 / ms16, d16.max_abs, d16.rms, verdict(d16));
    std::printf("    NOTE: deviation is measured against the f64 result, so it is\n"
                "          the FULL cost of the precision change (storage + filter\n"
                "          arithmetic), which is what shipping it would incur.\n");
}

}  // namespace lever_c

// ===========================================================================
// Lever D -- the irregular kernels (grain's Poisson/Binomial samplers)
// ===========================================================================
namespace lever_d {

// Each case names the branch it lands in, so the output reads as a regime map
// rather than a wall of numbers.
struct Regime {
    const char* name;
    double lam;      // Poisson rate, or -1 for a binomial case
    long long n;     // Binomial n
    double p;        // Binomial p
};

const Regime kPoisson[] = {
    {"lam=2      (small, inversion)", 2.0, 0, 0.0},
    {"lam=15     (mid, inversion)", 15.0, 0, 0.0},
    {"lam=29     (just under lam>=30)", 29.0, 0, 0.0},
    {"lam=31     (just over  lam>=30)", 31.0, 0, 0.0},
    {"lam=500    (normal approx)", 500.0, 0, 0.0},
};

const Regime kBinomial[] = {
    {"n=8   p=0.5  (n<25 Bernoulli)", -1.0, 8, 0.5},
    {"n=24  p=0.5  (just under n<25)", -1.0, 24, 0.5},
    {"n=26  p=0.5  (npq=6.5, inversion)", -1.0, 26, 0.5},
    {"n=200 p=0.5  (npq=50, normal)", -1.0, 200, 0.5},
    {"n=200 p=0.01 (npq=1.98, inversion)", -1.0, 200, 0.01},
};

double bench_poisson(double lam, int n_samples) {
    spk::StatsRng rng(20260828u);
    volatile int64_t sink = 0;
    const double t0 = now_ms();
    for (int i = 0; i < n_samples; ++i) sink += spk::fast_poisson_one(lam, rng);
    const double t1 = now_ms();
    (void)sink;
    return (t1 - t0) * 1e6 / n_samples;  // ns per sample
}

double bench_binomial(long long n, double p, int n_samples) {
    spk::StatsRng rng(20260828u);
    volatile int64_t sink = 0;
    const double t0 = now_ms();
    for (int i = 0; i < n_samples; ++i) sink += spk::fast_binomial_one(n, p, rng);
    const double t1 = now_ms();
    (void)sink;
    return (t1 - t0) * 1e6 / n_samples;
}

// The realistic case: a real render hands these a DIFFERENT parameter per pixel,
// so the branch predictor cannot settle and every sample may take a different
// path. The gap between this and the best fixed regime is the cost of
// irregularity itself, which is the number this lever exists to produce.
double bench_poisson_mixed(int n_samples) {
    spk::StatsRng rng(20260828u);
    volatile int64_t sink = 0;
    // Sweep lam across both sides of the lam>=30 threshold, deterministically.
    const double t0 = now_ms();
    for (int i = 0; i < n_samples; ++i) {
        const double lam = 1.0 + static_cast<double>((i * 37) % 600);
        sink += spk::fast_poisson_one(lam, rng);
    }
    const double t1 = now_ms();
    (void)sink;
    return (t1 - t0) * 1e6 / n_samples;
}

double bench_binomial_mixed(int n_samples) {
    spk::StatsRng rng(20260828u);
    volatile int64_t sink = 0;
    const double t0 = now_ms();
    for (int i = 0; i < n_samples; ++i) {
        const long long n = 1 + (i * 17) % 400;       // straddles n<25 and npq>10
        const double p = 0.02 + 0.96 * ((i * 29) % 50) / 50.0;
        sink += spk::fast_binomial_one(n, p, rng);
    }
    const double t1 = now_ms();
    (void)sink;
    return (t1 - t0) * 1e6 / n_samples;
}

void run(int n_samples) {
    std::printf("\n[D] irregular kernels -- grain's samplers (kernels/stats.cpp), "
                "%d samples each, 1 thread\n", n_samples);

    std::printf("    fast_poisson_one:\n");
    double best_p = 1e18, worst_p = 0.0;
    for (const Regime& r : kPoisson) {
        const double ns = bench_poisson(r.lam, n_samples);
        if (ns < best_p) best_p = ns;
        if (ns > worst_p) worst_p = ns;
        std::printf("      %-34s %8.1f ns/sample\n", r.name, ns);
    }
    const double mixed_p = bench_poisson_mixed(n_samples);
    std::printf("      %-34s %8.1f ns/sample   <-- realistic\n",
                "lam varies per sample", mixed_p);
    std::printf("      regime spread %.1fx (best %.1f, worst %.1f); "
                "mixed vs best %.2fx\n",
                worst_p / best_p, best_p, worst_p, mixed_p / best_p);

    std::printf("    fast_binomial_one:\n");
    double best_b = 1e18, worst_b = 0.0;
    for (const Regime& r : kBinomial) {
        const double ns = bench_binomial(r.n, r.p, n_samples);
        if (ns < best_b) best_b = ns;
        if (ns > worst_b) worst_b = ns;
        std::printf("      %-34s %8.1f ns/sample\n", r.name, ns);
    }
    const double mixed_b = bench_binomial_mixed(n_samples);
    std::printf("      %-34s %8.1f ns/sample   <-- realistic\n",
                "n,p vary per sample", mixed_b);
    std::printf("      regime spread %.1fx (best %.1f, worst %.1f); "
                "mixed vs best %.2fx\n",
                worst_b / best_b, best_b, worst_b, mixed_b / best_b);

    // Scale the per-sample cost to a real render so the number means something.
    // apply_grain_to_density draws per pixel per channel.
    const double mp12 = 12.0e6 * 3.0;
    std::printf("    at 12 MP x 3 channels, one draw each: poisson %.2f s, "
                "binomial %.2f s (1 thread)\n",
                mixed_p * mp12 * 1e-9, mixed_b * mp12 * 1e-9);
    std::printf("    READ IT THIS WAY: a large regime spread or a large mixed-vs-best\n"
                "      ratio means the cost is DATA-dependent, which is exactly what\n"
                "      lane-parallel SIMD cannot absorb -- each lane would want a\n"
                "      different number of draws from one serial RNG stream. A small\n"
                "      spread would mean the branches are cheap and the real cost is\n"
                "      elsewhere (the RNG itself), which WOULD be attackable.\n");
}

}  // namespace lever_d

}  // namespace

int main(int argc, char** argv) {
    const bool quick = (argc > 1 && std::strcmp(argv[1], "--quick") == 0);
    std::printf("=== spektrafilm perf lab — untried levers, measured ===\n");
    std::printf("oracle band: max_abs <= 1e-4 AND rms <= 1e-5\n");

    lever_a::run(quick ? 200000 : 1000000);
    lever_b::run(quick ? 256 : 512, quick ? 256 : 512, 4.0);
    if (!quick) lever_b::run(512, 512, 12.0);
    lever_c::run(quick ? 512 : 1024, quick ? 512 : 1024, 8.0);
    lever_d::run(quick ? 200000 : 1000000);

    std::printf("\n=== end perf lab ===\n");
    return 0;
}
