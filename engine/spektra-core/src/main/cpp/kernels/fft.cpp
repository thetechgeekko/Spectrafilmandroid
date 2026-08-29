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


// ---------------------------------------------------------------------------
// RfftPlan — real-input transform via a half-length complex one.
//
// THE DERIVATION, because the fixup is where this goes wrong silently.
//
// Let M = n/2 and z[k] = x[2k] + i*x[2k+1] for k in [0, M). One complex FFT of
// length M gives Z = FFT_M(z), which holds the two interleaved spectra mixed
// together. Writing E for the DFT of the even samples and O for the odd:
//
//     A = Z[k mod M],  B = conj(Z[(M-k) mod M])
//     E[k] = (A + B) / 2
//     O[k] = (A - B) / (2i)            == (A - B) * (-i/2)
//     X[k] = E[k] + exp(-2*pi*i*k/n) * O[k],   k in [0, M]
//
// Checks that catch a sign slip immediately: at k = 0 the twiddle is 1 and
// X[0] = Re(Z[0]) + Im(Z[0]) = sum(even) + sum(odd) = sum(x); at k = M the
// twiddle is -1 and X[M] = sum(even) - sum(odd), the Nyquist bin. Both are real,
// as they must be.
//
// The inverse runs it backwards. Using X[M-k]'s conjugate and the identity
// conj(W_{M-k}) = -W_k:
//
//     conj(X[M-k]) = E[k] - W_k * O[k]
//  => E[k] = (X[k] + conj(X[M-k])) / 2
//     O[k] = conj(W_k) * (X[k] - conj(X[M-k])) / 2
//     Z[k] = E[k] + i * O[k]
//
// then z = IFFT_M(Z) and x[2k] = Re(z[k]), x[2k+1] = Im(z[k]).
// ---------------------------------------------------------------------------

RfftPlan::RfftPlan(int n) : n_(n) {
    if (n_ < 2) { n_ = 2; }
    const int m = n_ / 2;
    half_ = FftPlan(m);
    tw_.resize(static_cast<size_t>(m + 1) * 2);
    for (int k = 0; k <= m; ++k) {
        const double a = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n_);
        tw_[static_cast<size_t>(k) * 2 + 0] = std::cos(a);
        tw_[static_cast<size_t>(k) * 2 + 1] = std::sin(a);
    }
}

void RfftPlan::forward(const double* real_in, double* spectrum_out) const {
    const int m = n_ / 2;
    // z[k] = x[2k] + i*x[2k+1], transformed in place in the caller's output
    // buffer's first 2*m doubles (it has 2*(m+1), so there is room).
    std::vector<double> z(static_cast<size_t>(m) * 2);
    for (int k = 0; k < m; ++k) {
        z[static_cast<size_t>(k) * 2 + 0] = real_in[2 * k];
        z[static_cast<size_t>(k) * 2 + 1] = real_in[2 * k + 1];
    }
    half_.forward(z.data());

    for (int k = 0; k <= m; ++k) {
        const int ka = k % m;
        const int kb = (m - k) % m;
        const double ar = z[static_cast<size_t>(ka) * 2 + 0];
        const double ai = z[static_cast<size_t>(ka) * 2 + 1];
        const double br =  z[static_cast<size_t>(kb) * 2 + 0];
        const double bi = -z[static_cast<size_t>(kb) * 2 + 1];   // conj

        const double er = 0.5 * (ar + br);
        const double ei = 0.5 * (ai + bi);
        // (A - B) * (-i/2)
        const double dr = ar - br, di = ai - bi;
        const double orr =  0.5 * di;
        const double oi = -0.5 * dr;

        const double wr = tw_[static_cast<size_t>(k) * 2 + 0];
        const double wi = tw_[static_cast<size_t>(k) * 2 + 1];

        spectrum_out[static_cast<size_t>(k) * 2 + 0] = er + (wr * orr - wi * oi);
        spectrum_out[static_cast<size_t>(k) * 2 + 1] = ei + (wr * oi + wi * orr);
    }
}

void RfftPlan::inverse(const double* spectrum_in, double* real_out) const {
    const int m = n_ / 2;
    std::vector<double> z(static_cast<size_t>(m) * 2);
    for (int k = 0; k < m; ++k) {
        const double xr = spectrum_in[static_cast<size_t>(k) * 2 + 0];
        const double xi = spectrum_in[static_cast<size_t>(k) * 2 + 1];
        const double yr =  spectrum_in[static_cast<size_t>(m - k) * 2 + 0];
        const double yi = -spectrum_in[static_cast<size_t>(m - k) * 2 + 1];  // conj

        const double er = 0.5 * (xr + yr);
        const double ei = 0.5 * (xi + yi);
        const double dr = 0.5 * (xr - yr);
        const double di = 0.5 * (xi - yi);

        // O[k] = conj(W_k) * (X[k] - conj(X[M-k]))/2
        const double wr =  tw_[static_cast<size_t>(k) * 2 + 0];
        const double wi = -tw_[static_cast<size_t>(k) * 2 + 1];   // conj(W_k)
        const double orr = wr * dr - wi * di;
        const double oi = wr * di + wi * dr;

        // Z[k] = E[k] + i*O[k]
        z[static_cast<size_t>(k) * 2 + 0] = er - oi;
        z[static_cast<size_t>(k) * 2 + 1] = ei + orr;
    }
    half_.inverse(z.data());
    // half_.inverse is unscaled; the caller applies inverse_scale() == 1/n_, and
    // the half-length transform needs 1/m == 2/n_. Fold the factor-of-2
    // difference in here so the caller's single 1/n_ is correct.
    for (int k = 0; k < m; ++k) {
        real_out[2 * k]     = 2.0 * z[static_cast<size_t>(k) * 2 + 0];
        real_out[2 * k + 1] = 2.0 * z[static_cast<size_t>(k) * 2 + 1];
    }
}

}  // namespace spk
