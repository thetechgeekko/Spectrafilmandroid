/*
 * Spektrafilm for Android — lib:pngwriter native 16-bit PNG writer.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * A self-contained 16-bit-per-channel RGB PNG writer for the M2 export path.
 * Writes a valid PNG stream containing:
 *
 *   PNG signature (8 bytes)
 *   IHDR  — bit_depth=16, color_type=2 (RGB), no interlace
 *   iCCP  — optional compressed ICC profile (gated on non-null/non-empty arg)
 *   tEXt  — optional "Software\0Spektrafilm" keyword:value pair
 *   IDAT  — zlib-deflated scanlines; each scanline prefixed with a filter byte
 *             (filter 0 = None; correct output, no sub-pixel filtering)
 *   IEND  — zero-length end marker
 *
 * PNG 16-bit samples are big-endian per spec (RFC 2083 §2.3). The writer
 * byte-swaps from the engine's native little-endian uint16 input.
 *
 * CRC32 per chunk is computed with zlib's crc32() (the same library used for
 * deflate, so no extra dependency). zlib is available in the Android NDK's
 * sysroot (libz.so / libz.a) and on any POSIX host (zlib1g-dev).
 *
 * This header is host-buildable (no Android/JNI deps) so the module can be
 * unit-tested with a plain g++ -std=c++17 -lz build.
 */
#ifndef SPECTRAFILM_PNG_WRITER_H
#define SPECTRAFILM_PNG_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

namespace spectrafilm {

// Metadata embedded in the PNG. All optional fields default to sensible values.
struct PngMetadata {
    // Written as a tEXt chunk "Software\0<value>". Empty => chunk omitted.
    std::string software = "Spektrafilm";

    // Raw ICC profile bytes. Non-empty => iCCP chunk written ("ICC Profile\0" name,
    // compression method 0 = zlib-deflate, compressed profile data).
    // Empty => no iCCP chunk.
    std::vector<uint8_t> iccProfile;
};

struct PngWriteResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;        // populated when ok == false
    size_t bytesWritten = 0;  // size of the produced file / buffer
};

// Optional cooperative cancellation. The callback runs synchronously on the
// writer thread and must not throw. A null callback means "not cancellable".
struct PngCancellation {
    void* context = nullptr;
    bool (*isCancelled)(void* context) noexcept = nullptr;
};

// --- Core writer: 16-bit/sample interleaved RGB ------------------------------
//
// `rgb16` is width*height*3 uint16 samples, row-major, interleaved R,G,B.
// Sample values are the engine's native little-endian uint16; the writer
// byte-swaps to big-endian as required by the PNG spec before deflating.

// Encode to an in-memory byte vector (host-test + unit-test friendly).
PngWriteResult writePng16ToMemory(const uint16_t* rgb16, int width, int height,
                                  const PngMetadata& meta,
                                  std::vector<uint8_t>& outBytes,
                                  const PngCancellation* cancellation = nullptr);

// Encode and write to a filesystem path.
PngWriteResult writePng16ToFile(const uint16_t* rgb16, int width, int height,
                                const PngMetadata& meta,
                                const std::string& path,
                                const PngCancellation* cancellation = nullptr);

// --- Convenience: quantise a float RGB buffer [0,1] -> uint16 ---------------
//
// `rgbFloat` is width*height*3 float samples in [0,1]; values outside are
// clamped. Quantisation is round-to-nearest over [0,65535]. Mirrors the TIFF
// writer's equivalent so callers can use the same float SimResult buffer for
// both formats.
PngWriteResult writePngFloatToFile(const float* rgbFloat, int width, int height,
                                   const PngMetadata& meta,
                                   const std::string& path,
                                   const PngCancellation* cancellation = nullptr);

}  // namespace spectrafilm

#endif  // SPECTRAFILM_PNG_WRITER_H
