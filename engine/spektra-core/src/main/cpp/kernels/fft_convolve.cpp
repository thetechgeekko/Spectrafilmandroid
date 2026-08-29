/*
 * Spektrafilm for Android — native engine: FFT convolution for the diffusion PSF.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3 (see fft_convolve.h).
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * ---------------------------------------------------------------------------
 * THE DERIVATION, because "use an FFT" hides exactly the index bookkeeping that
 * makes this the SAME operator rather than a similar one.
 *
 * The direct loop computes, for y in [0,h) and x in [0,w):
 *
 *     out[y][x] = sum_{i,j in [0,ks)} padded[y+i][x+j] * kern[ks-1-i][ks-1-j]
 *
 * Substituting p = ks-1-i (so i = ks-1-p) in one dimension:
 *
 *     out[y] = sum_p padded[y + ks-1 - p] * kern[p]   =   (padded * kern)[y + ks-1]
 *
 * i.e. the LINEAR convolution of padded with kern, read at offset ks-1. That is
 * the standard "valid" region, and it is why the kernel is placed at the ORIGIN
 * of the transform below rather than centred: centring would shift the answer by
 * radius and silently translate the image.
 *
 * A CIRCULAR convolution of size N equals the linear one at index n whenever the
 * taps it reads, n-ks+1 .. n, stay inside [0, N). For n = ks-1+i with i in [0,B)
 * those are i .. i+ks-1, so B = N - ks + 1 outputs per transform are exact and
 * the rest are wrapped garbage. Discarding the wrapped ones and stepping by B is
 * overlap-save.
 *
 * Note padded already has ph = h + 2*radius = h + ks - 1 rows, so a single
 * transform of size N >= ph needs NO further zero padding -- the reflect padding
 * the caller built is exactly the overlap the convolution wants. Small images
 * therefore take one transform and no tiling at all.
 *
 * ---------------------------------------------------------------------------
 * REAL-TO-COMPLEX. Both the image tile and the kernel are real, so their spectra
 * are Hermitian and only n/2 + 1 of the n columns are independent. Storing and
 * transforming just those halves BOTH the scratch and the work versus a complex
 * transform. That matters more than a plain 2x: scratch is what caps the
 * transform size, and transform size is what makes a large kernel cheap. At
 * n = 2048 the two spectra cost ~67 MB instead of ~134 MB.
 *
 * The row pass is kernels/fft.h's RfftPlan (real n -> n/2+1 complex bins, one
 * half-length complex FFT plus an O(n) fixup); the column pass is an ordinary
 * complex FFT of length n over each of the n/2 + 1 kept columns.
 *
 * Still on the table: packing two of the three channels into one complex
 * transform, which would take three channel-passes down to two.
 */
#include "kernels/fft_convolve.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>

#include "kernels/fft.h"
#include "kernels/parallel.h"

namespace spk {
namespace {

// Column pass over the kept n/2+1 columns of an r2c spectrum. Each column is an
// independent complex transform of length n, gathered into a contiguous scratch
// line and scattered back -- striding the transform through the spectrum directly
// is dramatically slower. Independent per column, so the parallel split cannot
// change any result and the determinism contract survives for any worker count.
void columns(double* spec, int n, int bins, const FftPlan& plan, bool inverse) {
    parallel_for(0, bins, [&](int lo, int hi) {
        std::vector<double> col(static_cast<size_t>(n) * 2);
        for (int c = lo; c < hi; ++c) {
            for (int r = 0; r < n; ++r) {
                const size_t s = (static_cast<size_t>(r) * bins + c) * 2;
                col[static_cast<size_t>(r) * 2 + 0] = spec[s + 0];
                col[static_cast<size_t>(r) * 2 + 1] = spec[s + 1];
            }
            if (inverse) plan.inverse(col.data()); else plan.forward(col.data());
            for (int r = 0; r < n; ++r) {
                const size_t s = (static_cast<size_t>(r) * bins + c) * 2;
                spec[s + 0] = col[static_cast<size_t>(r) * 2 + 0];
                spec[s + 1] = col[static_cast<size_t>(r) * 2 + 1];
            }
        }
    });
}

// Real n x n plane -> r2c spectrum (n rows x bins complex).
void forward2(const double* plane, double* spec, int n, int bins,
              const RfftPlan& rp, const FftPlan& cp) {
    parallel_for(0, n, [&](int lo, int hi) {
        for (int r = lo; r < hi; ++r)
            rp.forward(plane + static_cast<size_t>(r) * n,
                       spec + static_cast<size_t>(r) * bins * 2);
    });
    columns(spec, n, bins, cp, /*inverse=*/false);
}

// r2c spectrum -> real n x n plane. Unscaled; the caller applies 1/(n*n).
void inverse2(double* spec, double* plane, int n, int bins,
              const RfftPlan& rp, const FftPlan& cp) {
    columns(spec, n, bins, cp, /*inverse=*/true);
    parallel_for(0, n, [&](int lo, int hi) {
        for (int r = lo; r < hi; ++r)
            rp.inverse(spec + static_cast<size_t>(r) * bins * 2,
                       plane + static_cast<size_t>(r) * n);
    });
}

}  // namespace

int fft_convolve_transform_size(int w, int h, int ks, int max_transform) {
    if (ks < 1 || w < 1 || h < 1) return 0;
    const int ph = h + ks - 1;
    const int pw = w + ks - 1;
    // Never transform larger than the padded plane -- that is the single-tile case
    // and any more would be wasted work.
    const int ideal = fft_next_pow2(std::max(ph, pw));
    // ...but never so small that B = N - ks + 1 <= 0.
    const int floor_n = fft_next_pow2(ks + 1);
    int cap = fft_next_pow2(std::max(max_transform, 1));
    if (cap < floor_n) cap = floor_n;
    return std::min(ideal, cap);
}

bool fft_convolve_same(const double* padded, int pw, int ph,
                       const double* kern, int ks,
                       int w, int h,
                       double* out, int out_stride, int out_offset,
                       int max_transform) {
    if (!padded || !kern || !out) return false;
    if (ks < 1 || (ks % 2) == 0) return false;          // odd kernels only
    if (w < 1 || h < 1) return false;
    if (pw != w + ks - 1 || ph != h + ks - 1) return false;
    if (out_stride < 1 || out_offset < 0 || out_offset >= out_stride) return false;

    const int n = fft_convolve_transform_size(w, h, ks, max_transform);
    if (n < ks + 1) return false;
    const int block = n - ks + 1;                        // exact outputs per tile
    const int bins  = n / 2 + 1;                         // kept Hermitian columns
    const size_t spec_sz  = static_cast<size_t>(n) * bins * 2;
    const size_t plane_sz = static_cast<size_t>(n) * n;

    std::vector<double> kspec, tspec, plane;
    try {
        kspec.assign(spec_sz, 0.0);
        tspec.assign(spec_sz, 0.0);
        plane.assign(plane_sz, 0.0);
    } catch (const std::bad_alloc&) {
        return false;                                    // caller falls back
    }

    const RfftPlan rp(n);
    const FftPlan  cp(n);

    // Kernel spectrum: kern at the ORIGIN (see the derivation above), zero elsewhere.
    for (int i = 0; i < ks; ++i)
        for (int j = 0; j < ks; ++j)
            plane[static_cast<size_t>(i) * n + j] =
                kern[static_cast<size_t>(i) * ks + j];
    forward2(plane.data(), kspec.data(), n, bins, rp, cp);

    // Two unscaled inverse transforms are applied per tile (columns, then rows),
    // each wanting 1/n.
    const double scale = 1.0 / (static_cast<double>(n) * static_cast<double>(n));

    for (int y0 = 0; y0 < h; y0 += block) {
        for (int x0 = 0; x0 < w; x0 += block) {
            // Load the n x n input window at (y0, x0). Rows/cols past the padded
            // plane are zero; they only ever feed outputs beyond (h, w), which are
            // discarded below.
            std::fill(plane.begin(), plane.end(), 0.0);
            const int rows = std::min(n, ph - y0);
            const int cols = std::min(n, pw - x0);
            parallel_for(0, rows, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) {
                    const double* src =
                        padded + static_cast<size_t>(y0 + i) * pw + x0;
                    double* dst = plane.data() + static_cast<size_t>(i) * n;
                    for (int j = 0; j < cols; ++j) dst[j] = src[j];
                }
            });

            forward2(plane.data(), tspec.data(), n, bins, rp, cp);

            // Pointwise complex multiply by the kernel spectrum.
            parallel_for(0, n, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) {
                    double* t = tspec.data() + static_cast<size_t>(i) * bins * 2;
                    const double* k = kspec.data() + static_cast<size_t>(i) * bins * 2;
                    for (int j = 0; j < bins; ++j) {
                        const double ar = t[j * 2], ai = t[j * 2 + 1];
                        const double br = k[j * 2], bi = k[j * 2 + 1];
                        t[j * 2]     = ar * br - ai * bi;
                        t[j * 2 + 1] = ar * bi + ai * br;
                    }
                }
            });

            inverse2(tspec.data(), plane.data(), n, bins, rp, cp);

            // Exact outputs live at [ks-1 + i][ks-1 + j] for i,j in [0, block).
            const int ny = std::min(block, h - y0);
            const int nx = std::min(block, w - x0);
            parallel_for(0, ny, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) {
                    const double* src = plane.data() +
                        static_cast<size_t>(ks - 1 + i) * n + (ks - 1);
                    double* dst = out + (static_cast<size_t>(y0 + i) * w + x0) *
                                            out_stride + out_offset;
                    for (int j = 0; j < nx; ++j)
                        dst[static_cast<size_t>(j) * out_stride] = src[j] * scale;
                }
            });
        }
    }
    return true;
}

}  // namespace spk
