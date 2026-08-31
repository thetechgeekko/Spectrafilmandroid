/*
 * Spektrafilm for Android — native engine: .npy / .lut binary loaders.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by
 * spektrafilm. Mirrors the binary parsing in
 * spektrafilm/utils/spectral_upsampling.py:
 *   - _load_hanatos2025_spectra_lut (numpy .npy, dtype '<f2', shape 192x192x81)
 *   - _load_coeffs_lut (custom .lut: '=4i' header + '=4f' pixels)
 *
 * Dependency-free and NDK-friendly: only the C++17 standard library, no numpy,
 * no third-party deps. Assumes a little-endian host (all Android ARM/x86 targets
 * and CI hosts are LE), matching spkvec_io.h's assumption.
 */
#ifndef SPK_IO_NPY_LUT_H
#define SPK_IO_NPY_LUT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spk {

// Shared by file-backed and APK-asset callers so hostile input is rejected
// before either path allocates its byte buffer.
inline constexpr size_t kMaxNpyFileBytes = 32u * 1024u * 1024u;

// A dense float64 array decoded from disk: flat C-order (row-major) data plus its
// shape. float64 is used internally so LUT values match the Python oracle, which
// promotes the float16 .npy to np.double and reads the .lut as float32->double.
struct NdArray {
    std::vector<int> shape;     // e.g. {192, 192, 81} or {512, 512, 4}
    std::vector<double> data;   // row-major (C-order)

    size_t count() const {
        size_t n = shape.empty() ? 0 : 1;
        for (int d : shape) n *= static_cast<size_t>(d);
        return n;
    }

    // Index helper for a 3D array (i, j, c) -> flat offset, C-order.
    double at3(int i, int j, int c) const {
        return data[(static_cast<size_t>(i) * shape[1] + j) * shape[2] + c];
    }
};

// Load a NumPy .npy file. Supports version 1.0/2.0 headers and the dtypes the
// Hanatos2025 spectra LUT uses: '<f2' (float16), '<f4' (float32), '<f8'
// (float64), all little-endian, C-order. Values are promoted to double, exactly
// mirroring `np.double(np.load(...))` in _load_hanatos2025_spectra_lut.
//
// Throws std::runtime_error on any malformed/unsupported input.
NdArray load_npy(const std::string& path);

// Parse a NumPy .npy from an in-memory buffer (the bytes that load_npy would have
// read from `path`). Used for asset-backed loading on Android, where the file
// lives in the APK asset store and is read via AAssetManager into memory. `name`
// is only used in error messages. Same dtype/version support as load_npy.
NdArray parse_npy(const char* data, size_t len, const std::string& name);

// Load the Hanatos coefficient .lut file (hanatos_irradiance_xy_coeffs_*.lut).
//
// Binary layout (decoded from _load_coeffs_lut, all native/'=' little-endian):
//   header : 4 x int32  -> (magic0, magic1, width, height)   [16 bytes]
//   pixels : width*height x (4 x float32), iterated as
//                for j in range(height): for i in range(width): read pixel
//            and stored by Python as px[i][j], i.e. np.array(px) has shape
//            (width, height, 4) indexed [i][j][channel].
//   Any trailing bytes after width*height pixels are ignored (the Python loader
//   stops after reading exactly width*height pixels).
//
// The returned NdArray has shape {width, height, 4} indexed [i][j][c], matching
// the Python `np.array(px)`. Throws std::runtime_error on malformed input.
NdArray load_coeffs_lut(const std::string& path);

}  // namespace spk

#endif  // SPK_IO_NPY_LUT_H
