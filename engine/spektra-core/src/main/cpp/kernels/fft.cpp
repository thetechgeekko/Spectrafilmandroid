/*
 * Spektrafilm for Android — native engine: deterministic radix-2 complex FFT.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3 (see fft.h).
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Iterative Cooley-Tukey, decimation-in-time, in-place after a bit-reversal
 * permutation. See fft.h for why the fixed order matters.
 */
#include "kernels/fft.h"

#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace spk {

int fft_next_pow2(int v) {
    int n = 1;
    while (n < v) n <<= 1;
    return n;
}

FftPlan::FftPlan(int n) : n_(n) {
    if (n_ < 1) { n_ = 1; }
    levels_ = 0;
    while ((1 << levels_) < n_) ++levels_;
    inv_n_ = 1.0 / static_cast<double>(n_);

    rev_.resize(static_cast<size_t>(n_));
    for (int i = 0; i < n_; ++i) {
        int r = 0;
        for (int b = 0; b < levels_; ++b)
            if (i & (1 << b)) r |= 1 << (levels_ - 1 - b);
        rev_[static_cast<size_t>(i)] = r;
    }

    // Twiddles for the FORWARD transform: w_k = exp(-2*pi*i*k/n), k in [0, n/2).
    // The inverse conjugates them at use, so one table serves both directions and
    // both directions therefore multiply by bit-identical constants.
    const int half = n_ / 2;
    tw_.resize(static_cast<size_t>(half) * 2);
    for (int k = 0; k < half; ++k) {
        double a = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n_);
        tw_[static_cast<size_t>(k) * 2 + 0] = std::cos(a);
        tw_[static_cast<size_t>(k) * 2 + 1] = std::sin(a);
    }
}

void FftPlan::run(double* data, bool inverse) const {
    if (n_ <= 1) return;

    // Bit-reversal permutation (swap each pair once).
    for (int i = 0; i < n_; ++i) {
        int j = rev_[static_cast<size_t>(i)];
        if (j > i) {
            double tr = data[2 * i], ti = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tr;
            data[2 * j + 1] = ti;
        }
    }

    // Butterflies, smallest stage first. `stride` selects the twiddle for this
    // stage from the single full-resolution table, so no per-stage table is built
    // and every stage reuses the same constants.
    const double sgn = inverse ? -1.0 : 1.0;   // conjugate the table when inverse
    for (int len = 2; len <= n_; len <<= 1) {
        const int half = len >> 1;
        const int stride = n_ / len;
        for (int base = 0; base < n_; base += len) {
            for (int k = 0; k < half; ++k) {
                const size_t t = static_cast<size_t>(k) * stride;
                const double wr = tw_[t * 2 + 0];
                const double wi = sgn * tw_[t * 2 + 1];

                const int i0 = base + k;
                const int i1 = i0 + half;
                const double ar = data[2 * i0], ai = data[2 * i0 + 1];
                const double br = data[2 * i1], bi = data[2 * i1 + 1];

                const double tr = br * wr - bi * wi;
                const double ti = br * wi + bi * wr;

                data[2 * i0]     = ar + tr;
                data[2 * i0 + 1] = ai + ti;
                data[2 * i1]     = ar - tr;
                data[2 * i1 + 1] = ai - ti;
            }
        }
    }
}

void FftPlan::forward(double* data) const { run(data, /*inverse=*/false); }
void FftPlan::inverse(double* data) const { run(data, /*inverse=*/true); }

}  // namespace spk
