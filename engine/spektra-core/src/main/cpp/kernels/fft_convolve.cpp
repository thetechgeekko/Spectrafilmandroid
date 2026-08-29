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
 * WHY NOT REAL-TO-COMPLEX. A r2c transform would halve both memory and time, and
 * this is the obvious next optimisation. It is deliberately not done here: the
 * first landing is a correctness-and-determinism exercise held to a tolerance
 * against the direct loop, and a c2c transform keeps the index algebra above
 * plainly readable. The same applies to packing two of the three channels into
 * one complex transform.
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

// 2D in-place transform of an N x N interleaved-complex plane: all rows, then all
// columns. Each row/column transform is independent and touches only its own
// line, so the parallel split cannot change any result -- the determinism
// contract survives for any worker count.
void fft2(double* buf, int n, const FftPlan& plan, bool inverse) {
    parallel_for(0, n, [&](int lo, int hi) {
        for (int y = lo; y < hi; ++y) {
            double* row = buf + static_cast<size_t>(y) * n * 2;
            if (inverse) plan.inverse(row); else plan.forward(row);
        }
    });
    // Columns: gather into a contiguous scratch line, transform, scatter back.
    // The gather/scatter is what makes the column pass cache-tolerable; a
    // transform striding by n*2 doubles directly is dramatically slower.
    parallel_for(0, n, [&](int lo, int hi) {
        std::vector<double> col(static_cast<size_t>(n) * 2);
        for (int x = lo; x < hi; ++x) {
            for (int y = 0; y < n; ++y) {
                const size_t s = (static_cast<size_t>(y) * n + x) * 2;
                col[static_cast<size_t>(y) * 2 + 0] = buf[s + 0];
                col[static_cast<size_t>(y) * 2 + 1] = buf[s + 1];
            }
            if (inverse) plan.inverse(col.data()); else plan.forward(col.data());
            for (int y = 0; y < n; ++y) {
                const size_t s = (static_cast<size_t>(y) * n + x) * 2;
                buf[s + 0] = col[static_cast<size_t>(y) * 2 + 0];
                buf[s + 1] = col[static_cast<size_t>(y) * 2 + 1];
            }
        }
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
    const size_t plane = static_cast<size_t>(n) * n * 2;

    std::vector<double> kbuf, tbuf;
    try {
        kbuf.assign(plane, 0.0);
        tbuf.assign(plane, 0.0);
    } catch (const std::bad_alloc&) {
        return false;                                    // caller falls back
    }

    const FftPlan plan(n);

    // Kernel spectrum: kern at the ORIGIN (see the derivation above), zero elsewhere.
    for (int i = 0; i < ks; ++i)
        for (int j = 0; j < ks; ++j)
            kbuf[(static_cast<size_t>(i) * n + j) * 2] =
                kern[static_cast<size_t>(i) * ks + j];
    fft2(kbuf.data(), n, plan, /*inverse=*/false);

    const double scale = plan.inverse_scale() * plan.inverse_scale();  // 1/(n*n)

    for (int y0 = 0; y0 < h; y0 += block) {
        for (int x0 = 0; x0 < w; x0 += block) {
            // Load the n x n input window at (y0, x0). Rows/cols past the padded
            // plane are zero; they only ever feed outputs beyond (h, w), which are
            // discarded below.
            std::fill(tbuf.begin(), tbuf.end(), 0.0);
            const int rows = std::min(n, ph - y0);
            const int cols = std::min(n, pw - x0);
            parallel_for(0, rows, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) {
                    const double* src =
                        padded + static_cast<size_t>(y0 + i) * pw + x0;
                    double* dst = tbuf.data() + static_cast<size_t>(i) * n * 2;
                    for (int j = 0; j < cols; ++j) dst[j * 2] = src[j];
                }
            });

            fft2(tbuf.data(), n, plan, /*inverse=*/false);

            // Pointwise complex multiply by the kernel spectrum.
            parallel_for(0, n, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) {
                    double* t = tbuf.data() + static_cast<size_t>(i) * n * 2;
                    const double* k = kbuf.data() + static_cast<size_t>(i) * n * 2;
                    for (int j = 0; j < n; ++j) {
                        const double ar = t[j * 2], ai = t[j * 2 + 1];
                        const double br = k[j * 2], bi = k[j * 2 + 1];
                        t[j * 2]     = ar * br - ai * bi;
                        t[j * 2 + 1] = ar * bi + ai * br;
                    }
                }
            });

            fft2(tbuf.data(), n, plan, /*inverse=*/true);

            // Exact outputs live at [ks-1 + i][ks-1 + j] for i,j in [0, block).
            const int ny = std::min(block, h - y0);
            const int nx = std::min(block, w - x0);
            parallel_for(0, ny, [&](int lo, int hi) {
                for (int i = lo; i < hi; ++i) {
                    const double* src =
                        tbuf.data() +
                        (static_cast<size_t>(ks - 1 + i) * n + (ks - 1)) * 2;
                    double* dst = out + (static_cast<size_t>(y0 + i) * w + x0) *
                                            out_stride + out_offset;
                    for (int j = 0; j < nx; ++j)
                        dst[static_cast<size_t>(j) * out_stride] =
                            src[static_cast<size_t>(j) * 2] * scale;
                }
            });
        }
    }
    return true;
}

}  // namespace spk
