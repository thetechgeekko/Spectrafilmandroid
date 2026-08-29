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

// Smallest power of two >= v (v >= 1).
int fft_next_pow2(int v);

}  // namespace spk

#endif  // SPK_KERNELS_FFT_H
