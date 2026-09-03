/*
 * Spektrafilm for Android — native engine: deterministic radix-2 complex FFT.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm.
 *
 * WHY THIS EXISTS. model/diffusion.cpp's PSF convolution is O(w*h*ks^2) with a
 * kernel radius that scales with image width, so its cost is QUADRATIC in pixel
 * count: 30.7 s for one 640px preview and an extrapolated 10.9 hours for a 12 MP
 * export at the app's own default Black Pro-Mist settings (#160,
 * docs/research/perf-lab.md 20). FFT convolution computes the SAME operator in
 * O(n log n). This is that FFT.
 *
 * DETERMINISM IS A HARD REQUIREMENT, not a nice-to-have. The engine's contract is
 * byte-identical output for any worker count. This FFT is therefore:
 *   - iterative and in-place with a FIXED butterfly order (no recursion, no
 *     work-stealing, no reassociation between runs),
 *   - free of any parallel reduction — callers parallelise across independent
 *     transforms (rows, columns, tiles), never inside one,
 *   - driven by a PRECOMPUTED twiddle table, so a given size always multiplies by
 *     bit-identical constants regardless of how many transforms ran before it.
 * Same input, same output, every time, on every worker count.
 *
 * Sizes are powers of two only. That is sufficient here: fft_convolve.cpp picks
 * its own transform size and pads.
 */
#ifndef SPK_KERNELS_FFT_H
#define SPK_KERNELS_FFT_H

#include <cstddef>
#include <vector>

namespace spk {

// Interleaved complex64: re at [2i], im at [2i+1]. Chosen over std::complex so the
// buffers can be memset/moved as plain doubles and so the layout is explicit.
using CplxBuf = std::vector<double>;

// Precomputed twiddles + bit-reversal permutation for one transform size.
// Build once, reuse across every transform of that size (including across threads
// — it is read-only after construction).
class FftPlan {
public:
    FftPlan() = default;
    // n MUST be a power of two and >= 1.
    explicit FftPlan(int n);

    int size() const { return n_; }

    // In-place forward (sign = -1) / inverse (sign = +1) transform of ONE complex
    // sequence of n_ elements, interleaved re/im, starting at `data`.
    // Neither direction scales; inverse_scale() below is applied by the caller
    // exactly once, which keeps the scaling out of the inner loops.
    void forward(double* data) const;
    void inverse(double* data) const;

    // 1/n, to be applied once after an inverse transform.
    double inverse_scale() const { return inv_n_; }

private:
    void run(double* data, bool inverse) const;

    int n_ = 0;
    int levels_ = 0;
    double inv_n_ = 0.0;
    std::vector<int> rev_;        // bit-reversal permutation
    std::vector<double> tw_;      // interleaved cos/-sin twiddles, n_/2 entries
};

// Real-input transform of length n (n a power of two, n >= 2), built on an
// FftPlan of length n/2.
//
// A real sequence has a Hermitian spectrum, so only n/2 + 1 bins are independent
// and the other half is redundant. Exploiting that halves BOTH the spectrum
// storage and the transform work, which is the whole point: fft_convolve's
// scratch is the binding constraint on how large a transform it can afford, and
// a larger transform is what makes a big kernel cheap (see its overlap-save
// notes).
//
// Method: pack the even and odd samples as the real and imaginary parts of a
// half-length complex sequence, transform that, then untangle the two
// interleaved spectra. Costs one complex FFT of length n/2 plus an O(n) fixup
// instead of a complex FFT of length n.
//
// Deterministic on the same terms as FftPlan: fixed order, precomputed twiddles,
// no reduction. forward() and inverse() are exact inverses up to rounding, with
// inverse() NOT scaled -- apply inverse_scale() once, as with FftPlan.
class RfftPlan {
public:
    RfftPlan() = default;
    explicit RfftPlan(int n);

    int size() const { return n_; }
    // Number of complex bins produced: n/2 + 1.
    int bins() const { return n_ / 2 + 1; }

    // real[n_] -> spectrum[2 * bins()] interleaved re/im. Buffers must not alias.
    void forward(const double* real_in, double* spectrum_out) const;
    // spectrum[2 * bins()] -> real[n_]. Unscaled; multiply by inverse_scale().
    // The input spectrum is READ-ONLY (an internal scratch copy is made), so a
    // caller may reuse a shared kernel spectrum across many inverse transforms.
    void inverse(const double* spectrum_in, double* real_out) const;

    double inverse_scale() const { return half_.size() > 0 ? 1.0 / n_ : 1.0; }

private:
    int n_ = 0;
    FftPlan half_;              // length n_/2
    std::vector<double> tw_;    // exp(-2*pi*i*k/n_) for k in [0, n_/2], interleaved
};

// Smallest power of two >= v (v >= 1).
int fft_next_pow2(int v);

}  // namespace spk

#endif  // SPK_KERNELS_FFT_H
