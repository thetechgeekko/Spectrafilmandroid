/*
 * Spektrafilm for Android — lib:tiffwriter native 16-bit TIFF writer.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * A self-contained, dependency-free baseline TIFF writer for the M2 export path:
 * writes an RGB, 16-bit-per-sample, single-IFD baseline TIFF (little-endian, "II")
 * with optional PackBits compression and embedded colour/metadata:
 *   - ICCProfile      (tag 34675)  — raw ICC byte blob (sRGB / Display-P3 / ProPhoto …)
 *   - Software        (tag 305)    — producer string
 *   - X/YResolution   (tags 282/283), ResolutionUnit (296)
 *   - DateTime        (tag 306)    — "YYYY:MM:DD HH:MM:SS"
 *   - ImageDescription(tag 270), Artist (315), Copyright (33432) — optional strings
 *   - an EXIF sub-IFD (tag 34665) carrying ExifVersion + ColorSpace + pixel dims
 *
 * The writer is deliberately independent of LibRaw: LibRaw is vendored here only as
 * a decoder (built with NO_JPEG/NO_LCMS) and exposes no clean public TIFF-writing
 * entry point, so hand-rolling the baseline writer is both smaller and gives full
 * control over the IFD tags we need (ICC + EXIF). The format produced is plain
 * baseline TIFF 6.0 and round-trips through libtiff, ImageMagick, exiftool, etc.
 *
 * This header is host-buildable (no Android/JNI deps) so it can be unit-tested with
 * a plain g++ -std=c++17 build (see tests/test_tiff_writer.cpp).
 */
#ifndef SPECTRAFILM_TIFF_WRITER_H
#define SPECTRAFILM_TIFF_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

namespace spectrafilm {

// TIFF compression schemes we support.
enum class TiffCompression {
    None = 1,      // baseline uncompressed (TIFF Compression = 1)
    PackBits = 2,  // baseline PackBits RLE   (TIFF Compression = 32773)
};

// Metadata embedded into the TIFF. All string fields are optional ("" => skipped).
struct TiffMetadata {
    std::string software = "Spektrafilm";  // tag 305
    std::string imageDescription;          // tag 270
    std::string artist;                    // tag 315
    std::string copyright;                 // tag 33432
    // "YYYY:MM:DD HH:MM:SS"; empty => DateTime tag omitted (EXIF 2.x format).
    std::string dateTime;

    // Resolution (pixels per ResolutionUnit). Defaults to 72 dpi (unit = inch).
    double xResolution = 72.0;
    double yResolution = 72.0;
    uint16_t resolutionUnit = 2;  // 1=none, 2=inch, 3=cm

    // EXIF ColorSpace tag value written into the EXIF sub-IFD:
    //   1 = sRGB, 0xFFFF = Uncalibrated (any non-sRGB / wide-gamut space).
    // The authoritative colour definition is the embedded ICC profile below.
    uint16_t exifColorSpace = 0xFFFF;

    // Raw ICC profile bytes (e.g. an Elle Stone / sauce-control .icc). Empty => no
    // ICCProfile tag written.
    std::vector<uint8_t> iccProfile;

    // Write an EXIF sub-IFD (tag 34665). When false, only baseline tags are written.
    bool writeExifIfd = true;
};

struct TiffWriteResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;       // populated when ok == false
    size_t bytesWritten = 0; // size of the produced file / buffer
};

// Optional cooperative cancellation. The callback runs synchronously on the
// writer thread and must not throw. A null callback means "not cancellable".
struct TiffCancellation {
    void* context = nullptr;
    bool (*isCancelled)(void* context) noexcept = nullptr;
};

// --- Core writer: 16-bit/sample interleaved RGB -----------------------------
//
// `rgb16` is width*height*3 uint16 samples, row-major, interleaved R,G,B. Sample
// values are written verbatim (native quantisation is the caller's job). Pixels
// are stored most-significant-byte-first per-sample within the little-endian file
// (TIFF stores 16-bit samples in the file's byte order; we use "II"/little-endian).

// Encode to an in-memory byte vector (host-test friendly; no filesystem).
TiffWriteResult writeTiff16ToMemory(const uint16_t* rgb16, int width, int height,
                                    const TiffMetadata& meta, TiffCompression compression,
                                    std::vector<uint8_t>& outBytes,
                                    const TiffCancellation* cancellation = nullptr);

// Encode and write to a filesystem path.
TiffWriteResult writeTiff16ToFile(const uint16_t* rgb16, int width, int height,
                                  const TiffMetadata& meta, TiffCompression compression,
                                  const std::string& path,
                                  const TiffCancellation* cancellation = nullptr);

// --- True 32-bit IEEE-float writer (SampleFormat=3, BitsPerSample=32) ---------
//
// `rgbFloat` is width*height*3 float samples, row-major, interleaved R,G,B, written
// VERBATIM (no quantisation, no clamp) as 32-bit IEEE little-endian. For a high-bit-depth
// output export or a scene-linear export of the engine's input. Same IFD / metadata shape
// as the 16-bit writer; round-trips through libtiff / Photoshop / darktable / Resolve.
TiffWriteResult writeTiff32fToMemory(const float* rgbFloat, int width, int height,
                                     const TiffMetadata& meta, TiffCompression compression,
                                     std::vector<uint8_t>& outBytes,
                                     const TiffCancellation* cancellation = nullptr);

TiffWriteResult writeTiff32fToFile(const float* rgbFloat, int width, int height,
                                   const TiffMetadata& meta, TiffCompression compression,
                                   const std::string& path,
                                   const TiffCancellation* cancellation = nullptr);

// --- Convenience: quantise a linear/encoded float RGB buffer to 16-bit --------
//
// `rgbFloat` is width*height*3 float samples in [0,1] (values outside are clamped).
// Quantisation is round-to-nearest over [0,65535]. This matches the engine's
// display-referred float output (SimResult) so callers can write that buffer
// directly without an intermediate copy step in Kotlin.
TiffWriteResult writeTiffFloatToFile(const float* rgbFloat, int width, int height,
                                     const TiffMetadata& meta, TiffCompression compression,
                                     const std::string& path,
                                     const TiffCancellation* cancellation = nullptr);

}  // namespace spectrafilm

#endif  // SPECTRAFILM_TIFF_WRITER_H
