/*
 * Spektrafilm for Android — native engine: separable 2D Gaussian blur kernel.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Ports spektrafilm/utils/fast_gaussian_filter.py:
 *   - _gaussian_kernel_1d / _reflect / _fir_2d_fused  (small-sigma FIR)
 *   - _yvv_coeffs / _iir_horizontal / _iir_vertical   (Young-van Vliet IIR)
 *   - _dispatch_2d / _apply_per_channel               (dispatch + channels)
 */
#include "gaussian.h"

#include "gaussian_hwy.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "parallel.h"

namespace spk {

namespace {

// _gaussian_kernel_1d(sigma, truncate): radius = int(truncate*sigma + 0.5);
// size = 2*radius+1; kernel[i] = exp(-0.5*((i-radius)/sigma)^2), normalised.
void gaussian_kernel_1d(float sigma, float truncate,
                        std::vector<float>& kernel, int& radius) {
    radius = static_cast<int>(truncate * sigma + 0.5f);
    if (radius < 0) radius = 0;
    int size = 2 * radius + 1;
    kernel.assign(size, 0.0f);
    double total = 0.0;
    for (int i = 0; i < size; ++i) {
        double x = static_cast<double>(i - radius);
        double val = std::exp(-0.5 * (x / sigma) * (x / sigma));
        kernel[i] = static_cast<float>(val);
        total += val;
    }
    if (total != 0.0) {
        for (int i = 0; i < size; ++i) {
            kernel[i] = static_cast<float>(kernel[i] / total);
        }
    }
}

// _reflect(i, n): scipy.ndimage mode='reflect' (d c b a | a b c d | d c b a).
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

// Small-sigma separable FIR with reflect boundaries.
// Port of _gaussian_filter_2d_small + _fir_2d_fused. We keep the two-pass
// (vertical then horizontal) structure; the Numba "fused strip" is purely a
// memory-traffic optimisation and is numerically identical here.
void gaussian_fir_plane(float* img, int w, int h, float sigma, float truncate) {
    if (sigma <= 0.0f) return;
    std::vector<float> kernel;
    int radius;
    gaussian_kernel_1d(sigma, truncate, kernel, radius);
    if (radius == 0) return;  // degenerate kernel == {1.0}

    const int n = h;  // rows
    const int m = w;  // cols
    std::vector<float> tmp(static_cast<size_t>(n) * m, 0.0f);

    // Vertical pass: tmp[i,j] = sum_k kernel[k+radius] * img[reflect(i+k), j].
    // Each output row accumulates independently (img is read-only here), so the
    // rows split into deterministic parallel chunks — byte-identical for any
    // worker count. Weighted by the m pixels each row covers.
    parallel_for_weighted(0, n, m, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float* trow = &tmp[static_cast<size_t>(i) * m];
            for (int k = -radius; k <= radius; ++k) {
                float kw = kernel[k + radius];
                int ii = reflect(i + k, n);
                const float* irow = &img[static_cast<size_t>(ii) * m];
                // OPT-IN Highway lanes (#124). Vectorises across j only — every
                // output keeps the scalar accumulation order and no FMA is used —
                // so the two branches are byte-identical (asserted by
                // tests/test_gaussian_hwy.cpp), not an approximation.
                if (hwy_fir::available()) {
                    hwy_fir::vertical_accum(trow, irow, m, kw);
                } else {
                    for (int j = 0; j < m; ++j) {
                        trow[j] += irow[j] * kw;
                    }
                }
            }
        }
    });

    // Horizontal pass: output[i,j] = sum_k kernel[k+radius] * tmp[i, reflect(j+k)].
    // Row i reads only tmp row i and writes only img row i — rows independent.
    parallel_for_weighted(0, n, m, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            const float* trow = &tmp[static_cast<size_t>(i) * m];
            float* orow = &img[static_cast<size_t>(i) * m];
            if (2 * radius >= m) {
                for (int j = 0; j < m; ++j) {
                    float sval = 0.0f;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[reflect(j + k, m)] * kernel[k + radius];
                    orow[j] = sval;
                }
            } else {
                for (int j = 0; j < radius; ++j) {
                    float sval = 0.0f;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[reflect(j + k, m)] * kernel[k + radius];
                    orow[j] = sval;
                }
                // Interior span: no reflection, so the taps are a straight
                // window — the SIMD form (same tap order per output, no FMA) is
                // byte-identical to the scalar loop below it.
                if (hwy_fir::available()) {
                    hwy_fir::horizontal_interior(trow, kernel.data(), radius,
                                                 radius, m - radius, orow);
                } else {
                    for (int j = radius; j < m - radius; ++j) {
                        float sval = 0.0f;
                        for (int k = -radius; k <= radius; ++k)
                            sval += trow[j + k] * kernel[k + radius];
                        orow[j] = sval;
                    }
                }
                for (int j = m - radius; j < m; ++j) {
                    float sval = 0.0f;
                    for (int k = -radius; k <= radius; ++k)
                        sval += trow[reflect(j + k, m)] * kernel[k + radius];
                    orow[j] = sval;
                }
            }
        }
    });
}

struct YvvCoeffs {
    double B, B1, B2, B3;
};

// _yvv_coeffs(sigma): Young & van Vliet 2002 3rd-order IIR coefficients.
YvvCoeffs yvv_coeffs(double sigma) {
    double q;
    if (sigma >= 2.5) {
        q = 0.98711 * sigma - 0.96330;
    } else {
        q = 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sigma);
    }
    double q2 = q * q;
    double q3 = q2 * q;
    double b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
    double b1 = 2.44413 * q + 2.85619 * q2 + 1.26661 * q3;
    double b2 = -(1.4281 * q2 + 1.26661 * q3);
    double b3 = 0.422205 * q3;
    double B = 1.0 - (b1 + b2 + b3) / b0;
    return {B, b1 / b0, b2 / b0, b3 / b0};
}

// _iir_horizontal: forward + backward sweep per row, sample-replication edges.
// The recurrence is serial ALONG a row but rows are independent, so the rows
// split into deterministic parallel chunks (byte-identical for any worker
// count — each row's sweep is the same scalar op sequence wherever it runs).
void iir_horizontal(float* img, int w, int h, const YvvCoeffs& c) {
    const int n = h, m = w;
    parallel_for_weighted(0, n, m, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float* row = &img[static_cast<size_t>(i) * m];
            double w1, w2, w3;
            double x0 = row[0];
            w1 = w2 = w3 = x0;
            for (int j = 0; j < m; ++j) {
                double val = c.B * row[j] + c.B1 * w1 + c.B2 * w2 + c.B3 * w3;
                row[j] = static_cast<float>(val);
                w3 = w2; w2 = w1; w1 = val;
            }
            double xn = row[m - 1];
            double y1, y2, y3;
            y1 = y2 = y3 = xn;
            for (int j = m - 1; j >= 0; --j) {
                double y = c.B * row[j] + c.B1 * y1 + c.B2 * y2 + c.B3 * y3;
                row[j] = static_cast<float>(y);
                y3 = y2; y2 = y1; y1 = y;
            }
        }
    });
}

// _iir_vertical: forward + backward sweep per column. The recurrence is serial
// DOWN a column but each column's state (s1/s2/s3 slot j) never touches another
// column, so the columns split into deterministic parallel chunks with
// chunk-local state — every column runs the exact op sequence of the serial
// row-major sweep, byte-identical for any worker count.
void iir_vertical(float* img, int w, int h, const YvvCoeffs& c) {
    const int n = h, m = w;
    parallel_for_weighted(0, m, n, [&](int jb, int je) {
        const int mc = je - jb;
        std::vector<double> s1(mc), s2(mc), s3(mc);
        for (int j = jb; j < je; ++j) {
            double x0 = img[j];
            s1[j - jb] = s2[j - jb] = s3[j - jb] = x0;
        }
        for (int i = 0; i < n; ++i) {
            float* row = &img[static_cast<size_t>(i) * m];
            for (int j = jb; j < je; ++j) {
                double x = row[j];
                double val = c.B * x + c.B1 * s1[j - jb] + c.B2 * s2[j - jb] +
                             c.B3 * s3[j - jb];
                row[j] = static_cast<float>(val);
                s3[j - jb] = s2[j - jb]; s2[j - jb] = s1[j - jb]; s1[j - jb] = val;
            }
        }
        for (int j = jb; j < je; ++j) {
            double xn = img[static_cast<size_t>(n - 1) * m + j];
            s1[j - jb] = s2[j - jb] = s3[j - jb] = xn;
        }
        for (int i = n - 1; i >= 0; --i) {
            float* row = &img[static_cast<size_t>(i) * m];
            for (int j = jb; j < je; ++j) {
                double x = row[j];
                double y = c.B * x + c.B1 * s1[j - jb] + c.B2 * s2[j - jb] +
                           c.B3 * s3[j - jb];
                row[j] = static_cast<float>(y);
                s3[j - jb] = s2[j - jb]; s2[j - jb] = s1[j - jb]; s1[j - jb] = y;
            }
        }
    });
}

// _gaussian_filter_2d_large: IIR path; falls back to FIR below sigma 0.5.
void gaussian_iir_plane(float* img, int w, int h, float sigma) {
    if (sigma <= 0.0f) return;
    if (sigma < 0.5f) {
        gaussian_fir_plane(img, w, h, sigma, 3.0f);
        return;
    }
    YvvCoeffs c = yvv_coeffs(static_cast<double>(sigma));
    iir_horizontal(img, w, h, c);
    iir_vertical(img, w, h, c);
}

}  // namespace

void gaussian_blur_plane(float* img, int w, int h, float sigma, float truncate) {
    if (sigma <= 0.0f || w <= 0 || h <= 0) return;
    // _dispatch_2d: sigma >= SMALL_SIGMA_MAX uses IIR, else FIR.
    if (sigma >= kSmallSigmaMax) {
        gaussian_iir_plane(img, w, h, sigma);
    } else {
        gaussian_fir_plane(img, w, h, sigma, truncate);
    }
}

void gaussian_blur(float* img, int w, int h, int channels, float sigma,
                   float truncate) {
    if (channels == 1) {
        gaussian_blur_plane(img, w, h, sigma, truncate);
        return;
    }
    std::vector<float> sigmas(channels, sigma);
    gaussian_blur_per_channel(img, w, h, channels, sigmas.data(), truncate);
}

void gaussian_blur_per_channel(float* img, int w, int h, int channels,
                               const float* sigmas, float truncate) {
    if (w <= 0 || h <= 0 || channels <= 0) return;
    const int plane = w * h;
    std::vector<float> ch(static_cast<size_t>(plane));
    for (int c = 0; c < channels; ++c) {
        // Deinterleave channel c into a contiguous plane (per-pixel map,
        // disjoint writes -> deterministic parallel chunks).
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                ch[p] = img[static_cast<size_t>(p) * channels + c];
        });
        gaussian_blur_plane(ch.data(), w, h, sigmas[c], truncate);
        // Re-interleave.
        parallel_for(0, plane, [&](int lo, int hi) {
            for (int p = lo; p < hi; ++p)
                img[static_cast<size_t>(p) * channels + c] = ch[p];
        });
    }
}

}  // namespace spk
