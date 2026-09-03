/*
 * Spektrafilm for Android — native engine: double-precision spatial filters.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Implements kernels/exponential_filter.h (float64 Gaussian + the
 * Gaussian-mixture exponential surrogate), reproducing
 * spektrafilm/utils/fast_gaussian_filter.py bit-for-bit on the math path.
 */
#include "kernels/exponential_filter.h"

#include <cmath>
#include <vector>

#include "kernels/parallel.h"

namespace spk {

namespace {

// _gaussian_kernel_1d(sigma, truncate): radius=int(truncate*sigma+0.5);
// kernel[i]=exp(-0.5*((i-radius)/sigma)^2), normalised. All in float64.
void gaussian_kernel_1d(double sigma, double truncate,
                        std::vector<double>& kernel, int& radius) {
    radius = static_cast<int>(truncate * sigma + 0.5);
    if (radius < 0) radius = 0;
    int size = 2 * radius + 1;
    kernel.assign(size, 0.0);
    double total = 0.0;
    for (int i = 0; i < size; ++i) {
        double x = static_cast<double>(i - radius);
        double val = std::exp(-0.5 * (x / sigma) * (x / sigma));
        kernel[i] = val;
        total += val;
    }
    for (int i = 0; i < size; ++i) kernel[i] /= total;
}

// scipy.ndimage mode='reflect' (d c b a | a b c d | d c b a).
inline int reflect(int i, int n) {
    if (i >= 0 && i < n) return i;
    if (i >= -n && i < 0) return -i - 1;
    if (i >= n && i < 2 * n) return 2 * n - 1 - i;
    int period = 2 * n;
    i = i % period;
    if (i < 0) i += period;
    if (i >= n) i = period - 1 - i;
    return i;
}

// Small-sigma separable FIR (vertical then horizontal), reflect boundaries.
// Mirrors _gaussian_filter_2d_small + _fir_2d_fused (the strip fusion is purely a
// memory-traffic optimisation; numerically identical with float64 accumulation).
void gaussian_fir_plane(double* img, int w, int h, double sigma, double truncate) {
    if (sigma <= 0.0) return;
    std::vector<double> kernel;
    int radius;
    gaussian_kernel_1d(sigma, truncate, kernel, radius);
    if (radius == 0) return;  // kernel == {1.0} -> identity

    const int n = h, m = w;
    std::vector<double> tmp(static_cast<size_t>(n) * m, 0.0);

    // Vertical pass. Each output row accumulates independently (img is
    // read-only here) -> deterministic parallel chunks over rows, weighted by
    // the m pixels each row covers (byte-identical for any worker count).
    parallel_for_weighted(0, n, m, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            double* trow = &tmp[static_cast<size_t>(i) * m];
            for (int k = -radius; k <= radius; ++k) {
                double kw = kernel[k + radius];
                int ii = reflect(i + k, n);
                const double* irow = &img[static_cast<size_t>(ii) * m];
                for (int j = 0; j < m; ++j) trow[j] += irow[j] * kw;
            }
        }
    });
    // Horizontal pass (split reflected edges + reflect-free interior to match the
    // Numba kernel's accumulation order). Row i reads tmp row i, writes img row i.
    parallel_for_weighted(0, n, m, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            const double* trow = &tmp[static_cast<size_t>(i) * m];
            double* orow = &img[static_cast<size_t>(i) * m];
            if (2 * radius >= m) {
                for (int j = 0; j < m; ++j) {
                    double sval = 0.0;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[reflect(j + k, m)] * kernel[k + radius];
                    orow[j] = sval;
                }
            } else {
                for (int j = 0; j < radius; ++j) {
                    double sval = 0.0;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[reflect(j + k, m)] * kernel[k + radius];
                    orow[j] = sval;
                }
                for (int j = radius; j < m - radius; ++j) {
                    double sval = 0.0;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[j + k] * kernel[k + radius];
                    orow[j] = sval;
                }
                for (int j = m - radius; j < m; ++j) {
                    double sval = 0.0;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[reflect(j + k, m)] * kernel[k + radius];
                    orow[j] = sval;
                }
            }
        }
    });
}

struct YvvCoeffs { double B, B1, B2, B3; };

// _yvv_coeffs(sigma): Young & van Vliet 2002 3rd-order IIR coefficients.
YvvCoeffs yvv_coeffs(double sigma) {
    double q;
    if (sigma >= 2.5) {
        q = 0.98711 * sigma - 0.96330;
    } else {
        q = 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sigma);
    }
    double q2 = q * q, q3 = q2 * q;
    double b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
    double b1 = 2.44413 * q + 2.85619 * q2 + 1.26661 * q3;
    double b2 = -(1.4281 * q2 + 1.26661 * q3);
    double b3 = 0.422205 * q3;
    double B = 1.0 - (b1 + b2 + b3) / b0;
    return {B, b1 / b0, b2 / b0, b3 / b0};
}

// The IIR recurrence is serial ALONG each row but rows are independent ->
// deterministic parallel chunks over rows (byte-identical for any worker count).
void iir_horizontal(double* img, int w, int h, const YvvCoeffs& c) {
    const int n = h, m = w;
    parallel_for_weighted(0, n, m, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            double* row = &img[static_cast<size_t>(i) * m];
            double w1, w2, w3;
            double x0 = row[0];
            w1 = w2 = w3 = x0;
            for (int j = 0; j < m; ++j) {
                double val = c.B * row[j] + c.B1 * w1 + c.B2 * w2 + c.B3 * w3;
                row[j] = val;
                w3 = w2; w2 = w1; w1 = val;
            }
            double xn = row[m - 1];
            double y1, y2, y3;
            y1 = y2 = y3 = xn;
            for (int j = m - 1; j >= 0; --j) {
                double y = c.B * row[j] + c.B1 * y1 + c.B2 * y2 + c.B3 * y3;
                row[j] = y;
                y3 = y2; y2 = y1; y1 = y;
            }
        }
    });
}

// Serial DOWN each column, columns independent (state slot j never touches
// another column) -> deterministic parallel chunks over columns with
// chunk-local state; every column runs the exact op sequence of the serial
// row-major sweep, byte-identical for any worker count.
void iir_vertical(double* img, int w, int h, const YvvCoeffs& c) {
    const int n = h, m = w;
    parallel_for_weighted(0, m, n, [&](int jb, int je) {
        const int mc = je - jb;
        std::vector<double> s1(mc), s2(mc), s3(mc);
        for (int j = jb; j < je; ++j) {
            double x0 = img[j];
            s1[j - jb] = s2[j - jb] = s3[j - jb] = x0;
        }
        for (int i = 0; i < n; ++i) {
            double* row = &img[static_cast<size_t>(i) * m];
            for (int j = jb; j < je; ++j) {
                double x = row[j];
                double val = c.B * x + c.B1 * s1[j - jb] + c.B2 * s2[j - jb] +
                             c.B3 * s3[j - jb];
                row[j] = val;
                s3[j - jb] = s2[j - jb]; s2[j - jb] = s1[j - jb]; s1[j - jb] = val;
            }
        }
        for (int j = jb; j < je; ++j) {
            double xn = img[static_cast<size_t>(n - 1) * m + j];
            s1[j - jb] = s2[j - jb] = s3[j - jb] = xn;
        }
        for (int i = n - 1; i >= 0; --i) {
            double* row = &img[static_cast<size_t>(i) * m];
            for (int j = jb; j < je; ++j) {
                double x = row[j];
                double y = c.B * x + c.B1 * s1[j - jb] + c.B2 * s2[j - jb] +
                           c.B3 * s3[j - jb];
                row[j] = y;
                s3[j - jb] = s2[j - jb]; s2[j - jb] = s1[j - jb]; s1[j - jb] = y;
            }
        }
    });
}

// _gaussian_filter_2d_large: IIR path; falls back to FIR below sigma 0.5.
void gaussian_iir_plane(double* img, int w, int h, double sigma) {
    if (sigma <= 0.0) return;
    if (sigma < 0.5) {
        gaussian_fir_plane(img, w, h, sigma, 3.0);
        return;
    }
    YvvCoeffs c = yvv_coeffs(sigma);
    iir_horizontal(img, w, h, c);
    iir_vertical(img, w, h, c);
}

// n=3 Gaussian-mixture fit for the 2D isotropic exponential PSF
// (_EXPONENTIAL_GAUSSIAN_FITS[3]); amplitudes sum to 0.9999 by design.
constexpr int kExpN = 3;
constexpr double kExpAmplitude[kExpN] = {0.1633, 0.6496, 0.1870};
constexpr double kExpSigmaRatio[kExpN] = {0.5360, 1.5236, 2.7684};

}  // namespace

void gaussian_blur_plane_d(double* img, int w, int h, double sigma, double truncate) {
    if (sigma <= 0.0 || w <= 0 || h <= 0) return;
    if (sigma >= kSmallSigmaMaxD) {
        gaussian_iir_plane(img, w, h, sigma);
    } else {
        gaussian_fir_plane(img, w, h, sigma, truncate);
    }
}

void gaussian_blur_per_channel_d(double* img, int w, int h, int channels,
                                 const double* sigmas, double truncate) {
    if (w <= 0 || h <= 0 || channels <= 0) return;
    const int plane = w * h;
    std::vector<double> ch(static_cast<size_t>(plane));
    for (int c = 0; c < channels; ++c) {
        // Deinterleave / re-interleave: per-pixel maps with disjoint writes ->
        // deterministic parallel chunks.
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                ch[p] = img[static_cast<size_t>(p) * channels + c];
        });
        gaussian_blur_plane_d(ch.data(), w, h, sigmas[c], truncate);
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                img[static_cast<size_t>(p) * channels + c] = ch[p];
        });
    }
}

void exponential_filter_per_channel_d(const double* img, int w, int h, int channels,
                                      const double* decay, double* out,
                                      double truncate) {
    if (w <= 0 || h <= 0 || channels <= 0) return;
    const int plane = w * h;
    // result = sum_k amplitude_k * fast_gaussian_filter(img, ratio_k * decay)
    //
    // PLANAR, ONE CHANNEL AT A TIME. The obvious shape — loop over the three
    // mixture components, each copying the whole interleaved image and calling
    // gaussian_blur_per_channel_d — makes the image cross the interleaved/planar
    // boundary far more often than the math needs. gaussian_blur_per_channel_d
    // deinterleaves and reinterleaves internally, per channel, on every call, so
    // three components x three channels is NINE strided gathers and NINE strided
    // scatters, plus three full interleaved copies and three interleaved axpy
    // passes. Deinterleaving ONCE per channel and doing all three components in
    // planar space cuts that to three gathers and three scatters, and turns every
    // remaining pass into a contiguous one.
    //
    // Measured on the host at 12.5 MP (4096x3052), release flags, with internal
    // timers whose parts sum to the directly measured total (an earlier attempt
    // reported 2799 ms for this filter and a 65% zero/copy/axpy share; both were
    // process-state artifacts and are wrong — the reconciled figures are these):
    //
    //   old total                     1293.2 ms
    //     zero out                      20.1    1.6%
    //     3x copy img -> comp           58.2    4.5%
    //     3x gaussian_blur_per_channel 958.5   74.1%
    //     3x axpy                       52.2    4.0%
    //     residual (the 300 MB comp)   204.2   15.8%
    //
    // The cost is inside the blur calls, and inside those the de/re-interleave
    // is 56.5% at the IIR sigmas this filter uses (157.5 ms of 279.1 ms at
    // sigma 34.68) — roughly 42% of the whole filter, spent moving data rather
    // than blurring it, and flat across sigma because it is pure movement.
    //
    // BYTE-IDENTICAL, and that is the whole constraint here. Each component is
    // still gaussian_blur_plane_d over exactly img[:, c] at ratio_k * decay[c],
    // and the accumulation still runs k ascending with the same operands in the
    // same order, so every sum is formed identically. Only the order in which
    // memory is visited changed. Halation and the DIR couplers both call this,
    // so the two heaviest spatial stages share the result.
    //
    // The init / copy / axpy passes are per-element maps -> deterministic
    // parallel chunks (each element's arithmetic is chunk-independent).
    std::vector<double> src(static_cast<size_t>(plane));
    std::vector<double> comp(static_cast<size_t>(plane));
    std::vector<double> acc(static_cast<size_t>(plane));
    for (int c = 0; c < channels; ++c) {
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                src[p] = img[static_cast<size_t>(p) * channels + c];
        });
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p) acc[p] = 0.0;
        });
        for (int k = 0; k < kExpN; ++k) {
            parallel_for(0, plane, [&](int lo, int hi) {
                for (int p = lo; p < hi; ++p) comp[p] = src[p];
            });
            gaussian_blur_plane_d(comp.data(), w, h, kExpSigmaRatio[k] * decay[c],
                                  truncate);
            const double a = kExpAmplitude[k];
            parallel_for(0, plane, [&](int lo, int hi) {
                for (int p = lo; p < hi; ++p) acc[p] += a * comp[p];
            });
        }
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                out[static_cast<size_t>(p) * channels + c] = acc[p];
        });
    }
}

}  // namespace spk
