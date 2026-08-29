/*
 * Spektrafilm for Android — native engine: FFT convolution for the diffusion PSF.
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
 * Computes EXACTLY the operator model/diffusion.cpp's direct loop computes:
 *
 *     out[y][x] = sum_{i,j in [0,ks)} padded[y+i][x+j] * kern[ks-1-i][ks-1-j]
 *
 * in O(n log n) instead of O(w*h*ks^2). Not an approximation and not a different
 * filter — the same sum, reassociated. See the derivation in fft_convolve.cpp.
 *
 * WHY: that direct loop is quadratic in pixel count because the kernel radius
 * scales with image width, which costs 30.7 s for one 640px preview and an
 * extrapolated 10.9 hours for a 12 MP export at the app's default Black Pro-Mist
 * settings (#160, docs/research/perf-lab.md 20).
 *
 * NOT BIT-IDENTICAL to the direct loop, and cannot be. Reassociating a sum in
 * floating point changes the last bits; measured drift is ~1e-16 relative, which
 * is twelve orders inside the 1e-4 parity bar, but it is not zero. Callers hold
 * this to a TOLERANCE, never to byte-equality against the direct path.
 *
 * IS deterministic across worker counts: every transform is independent and
 * writes only its own row/column/tile, there is no reduction across threads, and
 * kernels/fft.h fixes the butterfly order. Byte-identical for any thread count is
 * preserved; only equality with the *direct* path is given up.
 */
#ifndef SPK_KERNELS_FFT_CONVOLVE_H
#define SPK_KERNELS_FFT_CONVOLVE_H

namespace spk {

// Default cap on the square transform size. Bounds scratch memory at
// 2 * N^2 * 16 bytes (two interleaved-complex f64 planes): 134 MB at N = 2048.
// Raising it makes large-kernel cases markedly faster (fewer, better-filled
// tiles) at quadratic memory cost -- see fft_convolve.cpp's overlap-save notes.
constexpr int kFftConvMaxTransform = 2048;

// `padded` is the reflect-padded plane, (h + ks - 1) rows by (w + ks - 1) cols
// (i.e. padded by radius = (ks-1)/2 on every side), row-major, stride pw.
// `kern` is ks x ks row-major. ks must be odd and >= 1.
// Writes the h x w output window into out[(y*w + x) * out_stride + out_offset].
//
// Returns false and writes nothing if the arguments are inconsistent or a
// transform buffer could not be allocated -- callers fall back to the direct loop.
bool fft_convolve_same(const double* padded, int pw, int ph,
                       const double* kern, int ks,
                       int w, int h,
                       double* out, int out_stride, int out_offset,
                       int max_transform = kFftConvMaxTransform);

// The transform size fft_convolve_same would choose. Exposed for tests and for
// cost/memory reporting; no side effects.
int fft_convolve_transform_size(int w, int h, int ks, int max_transform);

}  // namespace spk

#endif  // SPK_KERNELS_FFT_CONVOLVE_H
