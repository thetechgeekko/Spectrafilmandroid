/*
 * Spektrafilm for Android — lib:tiffwriter 16-bit baseline TIFF writer.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Self-contained, dependency-free implementation. See tiff_writer.h for the API
 * and the rationale for hand-rolling rather than reusing LibRaw (LibRaw exposes
 * no clean public TIFF-writing entry point and is compiled here NO_LCMS/NO_JPEG
 * as a decoder only).
 *
 * Format produced: baseline TIFF 6.0, little-endian ("II", 0x002A), a single IFD,
 * RGB chunky (PlanarConfiguration=1), 16 bits/sample x 3 samples, optional
 * PackBits (Compression=32773) or uncompressed (Compression=1). 16-bit samples
 * are stored in the file's byte order (little-endian) per the TIFF spec.
 *
 * IFD layout strategy (single pass, deterministic offsets):
 *   [0]              header (8 bytes): II, 42, offset-to-IFD0
 *   [ifd0]           IFD0: entry count + N entries (12 bytes each) + next-IFD (=0)
 *   [after IFD0]     out-of-line value blobs referenced by IFD0 entries
 *                    (BitsPerSample[3], resolutions, strip offsets/counts, strings,
 *                     ICC blob, EXIF IFD + its value blobs)
 *   [...]            image strips (one strip per file here; SAFE for host + app)
 *
 * We compute every blob's final offset up front (a layout pass) so we can emit
 * the bytes in one forward sweep with no back-patching beyond what the layout
 * already fixed.
 *
 * STRIP AND BIGTIFF POLICY (#175), stated because both are easy to get silently
 * wrong:
 *
 *   - ONE STRIP. Classic TIFF allows many; we emit one, because the reason to
 *     split -- bounding the buffer the writer holds -- is better served by not
 *     holding the image at all, and multiple strips would move the file's bytes
 *     for callers who pin the whole-container digest (#126 C4). The remaining
 *     buffer is the strip itself; an uncompressed strip's size is known before
 *     any pixel exists, so it could be streamed row by row WITHOUT changing a
 *     byte or adding a strip. That is the next rung if the ~75 MB at 12.5 MP
 *     ever matters; PackBits cannot take it (its size is data-dependent).
 *
 *   - CLASSIC TIFF ONLY, and it REFUSES rather than truncates. Every offset and
 *     length is accumulated in uint64 and the complete layout is proven to fit
 *     uint32 before a single pixel is read (validateUncompressedLayout); failure
 *     is an error string, never a wrapped offset. BigTIFF (0x002B, 64-bit
 *     offsets) is deliberately NOT implemented: the limit is ~4 GB of image
 *     data, i.e. past 700 MP at 16 bits x 3, which no export path on this
 *     device can reach, and BigTIFF is read by materially fewer tools.
 */
#include "tiff_writer.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <unistd.h>

namespace spectrafilm {
namespace {

constexpr uint64_t kClassicTiffMax = 0xffffffffull;

bool checkedMul(uint64_t a, uint64_t b, uint64_t& out) noexcept {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}

bool checkedAdd(uint64_t a, uint64_t b, uint64_t& out) noexcept {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    out = a + b;
    return true;
}

bool addLayout(uint64_t& cursor, uint64_t bytes) noexcept {
    uint64_t next = 0;
    if (!checkedAdd(cursor, bytes, next) || next > kClassicTiffMax) return false;
    cursor = next;
    return true;
}

bool alignLayout(uint64_t& cursor) noexcept {
    return (cursor & 1u) == 0u || addLayout(cursor, 1u);
}

bool imageLayout(int width, int height, uint64_t bytesPerSample,
                 uint64_t& rowSamples, uint64_t& rowBytes,
                 uint64_t& totalBytes, std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "invalid dimensions";
        return false;
    }
    if (!checkedMul(static_cast<uint64_t>(width), 3u, rowSamples) ||
        !checkedMul(rowSamples, bytesPerSample, rowBytes) ||
        !checkedMul(rowBytes, static_cast<uint64_t>(height), totalBytes) ||
        rowBytes > static_cast<uint64_t>(SIZE_MAX) ||
        totalBytes > static_cast<uint64_t>(SIZE_MAX) ||
        totalBytes > kClassicTiffMax) {
        error = "image too large for classic TIFF";
        return false;
    }
    return true;
}

bool isCancelled(const TiffCancellation* cancellation) noexcept {
    return cancellation != nullptr && cancellation->isCancelled != nullptr &&
           cancellation->isCancelled(cancellation->context);
}

TiffWriteResult cancelledResult() {
    TiffWriteResult result;
    result.cancelled = true;
    result.error = "cancelled";
    return result;
}

bool validateUncompressedLayout(uint64_t rawBytes, const TiffMetadata& meta,
                                std::string& error) {
    const std::string* strings[] = {
        &meta.imageDescription, &meta.software, &meta.dateTime,
        &meta.artist, &meta.copyright,
    };
    uint64_t stringCount = 0;
    for (const std::string* value : strings) {
        if (!value->empty()) ++stringCount;
        if (value->size() >= kClassicTiffMax) {
            error = "TIFF metadata field is too large";
            return false;
        }
    }
    if (meta.iccProfile.size() > kClassicTiffMax) {
        error = "TIFF ICC profile is too large";
        return false;
    }

    const uint64_t ifdCount = 14u + stringCount +
        (meta.iccProfile.empty() ? 0u : 1u) + (meta.writeExifIfd ? 1u : 0u);
    uint64_t ifdEntriesBytes = 0;
    uint64_t ifdBytes = 0;
    if (!checkedMul(ifdCount, 12u, ifdEntriesBytes) ||
        !checkedAdd(ifdEntriesBytes, 6u, ifdBytes)) {
        error = "TIFF IFD layout overflow";
        return false;
    }

    uint64_t cursor = 8u;
    if (!addLayout(cursor, ifdBytes) ||
        !alignLayout(cursor) || !addLayout(cursor, 6u) ||
        !alignLayout(cursor) || !addLayout(cursor, 6u) ||
        !alignLayout(cursor) || !addLayout(cursor, 8u) ||
        !alignLayout(cursor) || !addLayout(cursor, 8u)) {
        error = "TIFF value layout overflow";
        return false;
    }
    for (const std::string* value : strings) {
        if (value->empty() || value->size() + 1u <= 4u) continue;
        if (!alignLayout(cursor) || !addLayout(cursor, value->size() + 1u)) {
            error = "TIFF string layout overflow";
            return false;
        }
    }
    if (!meta.iccProfile.empty() &&
        (!alignLayout(cursor) || !addLayout(cursor, meta.iccProfile.size()))) {
        error = "TIFF ICC layout overflow";
        return false;
    }
    if (meta.writeExifIfd &&
        (!alignLayout(cursor) || !addLayout(cursor, 2u + 4u * 12u + 4u))) {
        error = "TIFF EXIF layout overflow";
        return false;
    }
    if (!alignLayout(cursor) || !addLayout(cursor, rawBytes)) {
        error = "complete classic TIFF layout exceeds uint32 offsets";
        return false;
    }
    return true;
}

class TempOutput final {
public:
    TempOutput() = default;
    TempOutput(const TempOutput&) = delete;
    TempOutput& operator=(const TempOutput&) = delete;

    ~TempOutput() {
        if (fd_ >= 0) ::close(fd_);
        if (!committed_ && !path_.empty()) {
            while (::unlink(path_.c_str()) != 0 && errno == EINTR) {}
        }
    }

    bool open(const std::string& destination, std::string& error) {
        if (!validatePath(destination, error)) {
            return false;
        }
        std::string pattern = destination + ".tmp.XXXXXX";
        for (;;) {
            std::vector<char> writable(pattern.begin(), pattern.end());
            writable.push_back('\0');
            fd_ = ::mkstemp(writable.data());
            if (fd_ >= 0) {
                path_ = writable.data();
                return true;
            }
            if (errno != EINTR) {
                error = "cannot create temporary output";
                return false;
            }
        }
    }

    bool write(const uint8_t* data, size_t size, std::string& error) {
        size_t offset = 0;
        while (offset < size) {
            const ssize_t written = ::write(fd_, data + offset, size - offset);
            if (written > 0) {
                offset += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            error = written == 0 ? "zero-length write to temporary output"
                                 : "cannot write temporary output";
            return false;
        }
        return true;
    }

    bool commit(const std::string& destination, std::string& error) {
        if (fd_ >= 0) {
            const int descriptor = fd_;
            fd_ = -1;
            if (::close(descriptor) != 0) {
                error = "cannot close temporary output";
                return false;
            }
        }
        int renamed;
        do {
            renamed = std::rename(path_.c_str(), destination.c_str());
        } while (renamed != 0 && errno == EINTR);
        if (renamed != 0) {
            error = "cannot publish output";
            return false;
        }
        committed_ = true;
        return true;
    }

    static bool validatePath(const std::string& destination, std::string& error) {
        if (destination.empty()) {
            error = "empty output path";
            return false;
        }
        if (destination.find('\0') != std::string::npos) {
            error = "output path contains NUL";
            return false;
        }
        return true;
    }

private:
    int fd_ = -1;
    std::string path_;
    bool committed_ = false;
};

class MemoryOutputGuard final {
public:
    explicit MemoryOutputGuard(std::vector<uint8_t>& output) noexcept : output_(output) {}
    MemoryOutputGuard(const MemoryOutputGuard&) = delete;
    MemoryOutputGuard& operator=(const MemoryOutputGuard&) = delete;
    ~MemoryOutputGuard() { if (!published_) output_.clear(); }
    void publish() noexcept { published_ = true; }

private:
    std::vector<uint8_t>& output_;
    bool published_ = false;
};

// ---- TIFF tag + type constants --------------------------------------------
constexpr uint16_t T_IMAGE_WIDTH        = 256;
constexpr uint16_t T_IMAGE_LENGTH       = 257;
constexpr uint16_t T_BITS_PER_SAMPLE    = 258;
constexpr uint16_t T_COMPRESSION        = 259;
constexpr uint16_t T_PHOTOMETRIC        = 262;
constexpr uint16_t T_IMAGE_DESCRIPTION  = 270;
constexpr uint16_t T_STRIP_OFFSETS      = 273;
constexpr uint16_t T_SAMPLES_PER_PIXEL  = 277;
constexpr uint16_t T_ROWS_PER_STRIP     = 278;
constexpr uint16_t T_STRIP_BYTE_COUNTS  = 279;
constexpr uint16_t T_X_RESOLUTION       = 282;
constexpr uint16_t T_Y_RESOLUTION       = 283;
constexpr uint16_t T_PLANAR_CONFIG      = 284;
constexpr uint16_t T_RESOLUTION_UNIT    = 296;
constexpr uint16_t T_SOFTWARE           = 305;
constexpr uint16_t T_DATETIME           = 306;
constexpr uint16_t T_ARTIST             = 315;
constexpr uint16_t T_SAMPLE_FORMAT      = 339;
constexpr uint16_t T_COPYRIGHT          = 33432;
constexpr uint16_t T_EXIF_IFD           = 34665;
constexpr uint16_t T_ICC_PROFILE        = 34675;

// EXIF sub-IFD tags
constexpr uint16_t E_EXIF_VERSION       = 36864;  // UNDEFINED[4], e.g. "0230"
constexpr uint16_t E_COLOR_SPACE        = 40961;  // SHORT
constexpr uint16_t E_PIXEL_X_DIM        = 40962;  // SHORT or LONG
constexpr uint16_t E_PIXEL_Y_DIM        = 40963;  // SHORT or LONG

// Field types
constexpr uint16_t TY_ASCII     = 2;
constexpr uint16_t TY_SHORT     = 3;
constexpr uint16_t TY_LONG      = 4;
constexpr uint16_t TY_RATIONAL  = 5;
constexpr uint16_t TY_UNDEFINED = 7;

constexpr uint16_t COMPRESSION_NONE     = 1;
constexpr uint16_t COMPRESSION_PACKBITS = 32773;
constexpr uint16_t PHOTOMETRIC_RGB      = 2;

// ---- little-endian byte emitters ------------------------------------------
void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
// Convert a positive double to a TIFF RATIONAL (num/den). Uses den=1000 so common
// resolutions (72, 300, 96, 150 dpi …) and fractional values round-trip exactly.
void doubleToRational(double v, uint32_t& num, uint32_t& den) {
    if (!std::isfinite(v) || v < 0) v = 0;
    den = 1000;
    double scaled = v * static_cast<double>(den);
    if (scaled > 4294967295.0) {  // clamp to LONG range
        num = 4294967295u; den = 1; return;
    }
    num = static_cast<uint32_t>(scaled + 0.5);
}

// PackBits RLE encode one row. Stop as soon as the encoded strip cannot beat
// the uncompressed size; the caller will use the original bytes instead.
bool appendPackBitsRow(const uint8_t* src, size_t n,
                       size_t fallbackThreshold,
                       const TiffCancellation* cancellation,
                       bool& cancelled,
                       std::vector<uint8_t>& out) {
    size_t i = 0;
    size_t nextCancellationPoll = 0;
    while (i < n) {
        if (i >= nextCancellationPoll) {
            if (isCancelled(cancellation)) {
                cancelled = true;
                return false;
            }
            nextCancellationPoll = i + 64u * 1024u;
        }
        // Try a run of >= 2 identical bytes.
        size_t runLen = 1;
        while (i + runLen < n && runLen < 128 && src[i + runLen] == src[i]) runLen++;
        if (runLen >= 2) {
            if (out.size() > fallbackThreshold - std::min<size_t>(2u, fallbackThreshold))
                return false;
            out.push_back(static_cast<uint8_t>(257 - runLen));  // -(runLen-1) as int8
            out.push_back(src[i]);
            i += runLen;
        } else {
            // Literal run: collect until a >=3 run begins or 128 reached.
            size_t litStart = i;
            size_t litLen = 0;
            while (i < n && litLen < 128) {
                size_t look = 1;
                while (i + look < n && look < 3 && src[i + look] == src[i]) look++;
                if (look >= 3) break;  // a run is starting; stop the literal
                i++; litLen++;
            }
            if (litLen + 1u > fallbackThreshold - out.size()) return false;
            out.push_back(static_cast<uint8_t>(litLen - 1));
            for (size_t k = 0; k < litLen; ++k) out.push_back(src[litStart + k]);
        }
    }
    return out.size() < fallbackThreshold;
}

enum class PackBitsStatus { Packed, NotBeneficial, Cancelled };

PackBitsStatus packBits(const uint8_t* src, size_t rowBytes, int height,
                        const TiffCancellation* cancellation,
                        std::vector<uint8_t>& out) {
    uint64_t total64 = 0;
    if (!checkedMul(static_cast<uint64_t>(rowBytes),
                    static_cast<uint64_t>(height), total64) ||
        total64 > static_cast<uint64_t>(SIZE_MAX)) {
        return PackBitsStatus::NotBeneficial;
    }
    const size_t total = static_cast<size_t>(total64);
    out.clear();
    out.reserve(std::min<size_t>(total, 1024u * 1024u));
    for (int y = 0; y < height; ++y) {
        if (isCancelled(cancellation)) {
            out.clear();
            return PackBitsStatus::Cancelled;
        }
        const uint8_t* row = src + static_cast<size_t>(y) * rowBytes;
        bool rowCancelled = false;
        if (!appendPackBitsRow(row, rowBytes, total, cancellation,
                               rowCancelled, out)) {
            out.clear();
            if (rowCancelled) return PackBitsStatus::Cancelled;
            return PackBitsStatus::NotBeneficial;
        }
    }
    return out.size() < total ? PackBitsStatus::Packed
                              : PackBitsStatus::NotBeneficial;
}

// One IFD entry described abstractly during the layout pass.
struct Entry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    // Either an inline value (<= 4 bytes packed) OR a pointer into the value pool.
    bool inlineVal = false;
    uint32_t inlineBytes = 0;   // up to 4 bytes, little-endian packed, when inlineVal
    uint32_t valueOffset = 0;   // file offset of out-of-line value, when !inlineVal
};

// Writes [bytes], then [tail] if given, through the temp-file-then-rename protocol.
// The two-span form lets a TIFF write emit its header and its image strip without
// concatenating them into one buffer first (#175).
TiffWriteResult publishAtomically(const std::vector<uint8_t>& bytes,
                                  const std::string& path,
                                  const TiffCancellation* cancellation,
                                  TiffWriteResult result,
                                  const std::vector<uint8_t>* tail = nullptr) {
    TempOutput output;
    if (!output.open(path, result.error)) {
        result.ok = false;
        result.bytesWritten = 0;
        return result;
    }
    constexpr size_t kWriteChunk = 64u * 1024u;
    const std::vector<uint8_t>* spans[2] = {&bytes, tail};
    size_t written = 0;
    for (int span = 0; span < 2; ++span) {
        if (spans[span] == nullptr) continue;
        const std::vector<uint8_t>& data = *spans[span];
        size_t offset = 0;
        while (offset < data.size()) {
            if (isCancelled(cancellation)) return cancelledResult();
            const size_t count = std::min(kWriteChunk, data.size() - offset);
            if (!output.write(data.data() + offset, count, result.error)) {
                result.ok = false;
                result.bytesWritten = 0;
                return result;
            }
            offset += count;
        }
        written += data.size();
    }
    if (isCancelled(cancellation)) return cancelledResult();
    if (!output.commit(path, result.error)) {
        result.ok = false;
        result.bytesWritten = 0;
        return result;
    }
    result.bytesWritten = written;
    return result;
}

}  // namespace

// Shared core: emit a baseline RGB TIFF from already-serialised little-endian sample
// bytes. `raw` holds width*height*3 samples at `bytesPerSample` bytes each (2 = uint16,
// 4 = float32); `sampleFormatValue` is the TIFF SampleFormat (1 = unsigned int, 3 = IEEE
// float). The IFD / EXIF / ICC / strip layout is identical across bit depths — only
// BitsPerSample, SampleFormat and the strip size differ — so both public writers share it.
// [stripOut], when given, receives the image strip instead of it being copied into
// [outBytes]. The strip is placed last in the layout, so a file writer can emit the
// header and then the strip without ever holding a second copy of the image: at
// 12.5 MP that copy is 75 MB, and it existed only to be written out and freed
// (#175).
static TiffWriteResult writeTiffSamplesToMemory(std::vector<uint8_t> raw, int width, int height,
                                                int bytesPerSample, uint16_t sampleFormatValue,
                                                const TiffMetadata& meta, TiffCompression compression,
                                                std::vector<uint8_t>& outBytes,
                                                const TiffCancellation* cancellation,
                                                std::vector<uint8_t>* stripOut = nullptr) {
    TiffWriteResult res;
    outBytes.clear();
    MemoryOutputGuard outputGuard(outBytes);
    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t rawStripBytes = 0;
    if (!imageLayout(width, height, static_cast<uint64_t>(bytesPerSample),
                     rowSamples64, rowBytes64, rawStripBytes, res.error) ||
        rawStripBytes != static_cast<uint64_t>(raw.size())) {
        if (res.error.empty()) res.error = "pixel buffer size does not match TIFF layout";
        return res;
    }
    if (!validateUncompressedLayout(rawStripBytes, meta, res.error)) return res;
    if (isCancelled(cancellation)) return cancelledResult();
    const uint16_t bitsValue = static_cast<uint16_t>(bytesPerSample * 8);

    const bool usePackBits = (compression == TiffCompression::PackBits);
    std::vector<uint8_t> strip;
    bool stripIsPacked = false;
    if (usePackBits) {
        const PackBitsStatus status = packBits(
            raw.data(), static_cast<size_t>(rowBytes64), height,
            cancellation, strip);
        if (status == PackBitsStatus::Cancelled) return cancelledResult();
        stripIsPacked = status == PackBitsStatus::Packed;
    }
    if (!stripIsPacked) strip.swap(raw);
    const uint16_t compTag = stripIsPacked ? COMPRESSION_PACKBITS : COMPRESSION_NONE;
    const uint32_t stripByteCount = static_cast<uint32_t>(strip.size());

    // --- Decide which baseline entries we emit (sorted by tag below) --------
    struct PendingString { uint16_t tag; std::string s; };
    std::vector<PendingString> strings;
    auto addString = [&](uint16_t tag, const std::string& s) {
        if (!s.empty()) strings.push_back({tag, s});
    };
    addString(T_IMAGE_DESCRIPTION, meta.imageDescription);
    addString(T_SOFTWARE, meta.software);
    addString(T_DATETIME, meta.dateTime);
    addString(T_ARTIST, meta.artist);
    addString(T_COPYRIGHT, meta.copyright);

    const bool hasIcc = !meta.iccProfile.empty();
    const bool hasExif = meta.writeExifIfd;

    // Count IFD0 entries.
    // Mandatory: ImageWidth, ImageLength, BitsPerSample, Compression, Photometric,
    //   StripOffsets, SamplesPerPixel, RowsPerStrip, StripByteCounts, XRes, YRes,
    //   PlanarConfig, ResolutionUnit, SampleFormat = 14
    uint16_t ifd0Count = 14;
    ifd0Count += static_cast<uint16_t>(strings.size());
    if (hasIcc)  ifd0Count++;
    if (hasExif) ifd0Count++;

    // --- LAYOUT PASS: compute every offset ----------------------------------
    // header = 8 bytes; IFD0 starts at 8.
    const uint64_t ifd0Offset64 = 8u;
    const uint64_t ifd0Bytes64 = 2u + static_cast<uint64_t>(ifd0Count) * 12u + 4u;
    uint64_t cursor = ifd0Offset64;
    if (!addLayout(cursor, ifd0Bytes64)) {
        res.error = "TIFF IFD layout exceeds uint32 offsets";
        return res;
    }
    auto placeAligned = [&](uint64_t bytes, uint64_t& offset) {
        if (!alignLayout(cursor)) return false;
        offset = cursor;
        return addLayout(cursor, bytes);
    };

    // BitsPerSample[3] and SampleFormat[3] are six-byte arrays; resolutions
    // are eight-byte rationals. Keep every cursor in uint64 until the complete
    // classic-TIFF layout has been proven representable.
    uint64_t bitsOffset64 = 0;
    uint64_t sampleFmtOffset64 = 0;
    uint64_t xresOffset64 = 0;
    uint64_t yresOffset64 = 0;
    if (!placeAligned(6u, bitsOffset64) ||
        !placeAligned(6u, sampleFmtOffset64) ||
        !placeAligned(8u, xresOffset64) ||
        !placeAligned(8u, yresOffset64)) {
        res.error = "TIFF value layout exceeds uint32 offsets";
        return res;
    }

    // Strings (ASCII, NUL-terminated). <=4 bytes incl. NUL go inline.
    struct StringLoc { uint16_t tag; std::string s; uint64_t offset; bool inlineVal; };
    std::vector<StringLoc> strLocs;
    strLocs.reserve(strings.size());
    for (auto& ps : strings) {
        const uint64_t len = static_cast<uint64_t>(ps.s.size()) + 1u;  // include NUL
        StringLoc loc{ps.tag, ps.s, 0, len <= 4};
        if (!loc.inlineVal && !placeAligned(len, loc.offset)) {
            res.error = "TIFF string layout exceeds uint32 offsets";
            return res;
        }
        strLocs.push_back(loc);
    }

    // ICC profile blob.
    uint64_t iccOffset64 = 0;
    if (hasIcc && !placeAligned(meta.iccProfile.size(), iccOffset64)) {
        res.error = "TIFF ICC layout exceeds uint32 offsets";
        return res;
    }

    // EXIF sub-IFD: lay out its own entries + value pool.
    // EXIF entries: ExifVersion(UNDEFINED[4] inline), ColorSpace(SHORT inline),
    //   PixelXDimension(LONG inline), PixelYDimension(LONG inline) = 4 entries,
    // all inline -> no EXIF value pool needed.
    uint64_t exifIfdOffset64 = 0;
    const uint16_t exifCount = 4;
    if (hasExif &&
        !placeAligned(2u + static_cast<uint64_t>(exifCount) * 12u + 4u,
                      exifIfdOffset64)) {
        res.error = "TIFF EXIF layout exceeds uint32 offsets";
        return res;
    }

    // Image strip last (word-aligned).
    uint64_t stripOffset64 = 0;
    if (!placeAligned(stripByteCount, stripOffset64)) {
        res.error = "complete classic TIFF layout exceeds uint32 offsets";
        return res;
    }

    const uint32_t ifd0Offset = static_cast<uint32_t>(ifd0Offset64);
    const uint32_t bitsOffset = static_cast<uint32_t>(bitsOffset64);
    const uint32_t sampleFmtOffset = static_cast<uint32_t>(sampleFmtOffset64);
    const uint32_t xresOffset = static_cast<uint32_t>(xresOffset64);
    const uint32_t yresOffset = static_cast<uint32_t>(yresOffset64);
    const uint32_t iccOffset = static_cast<uint32_t>(iccOffset64);
    const uint32_t exifIfdOffset = static_cast<uint32_t>(exifIfdOffset64);
    const uint32_t stripOffset = static_cast<uint32_t>(stripOffset64);
    const uint32_t fileSize = static_cast<uint32_t>(cursor);

    if (isCancelled(cancellation)) return cancelledResult();
    auto cancelWithoutPublication = [&outBytes]() {
        outBytes.clear();
        return cancelledResult();
    };

    // --- EMIT PASS ----------------------------------------------------------
    outBytes.reserve(fileSize);

    // Header.
    outBytes.push_back('I'); outBytes.push_back('I');  // little-endian
    putU16(outBytes, 42);                              // TIFF magic
    putU32(outBytes, ifd0Offset);

    // Assemble IFD0 entries (must be written in ascending tag order).
    std::vector<Entry> entries;
    auto addInline = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t packed) {
        Entry e; e.tag = tag; e.type = type; e.count = count;
        e.inlineVal = true; e.inlineBytes = packed; entries.push_back(e);
    };
    auto addOutline = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t off) {
        Entry e; e.tag = tag; e.type = type; e.count = count;
        e.inlineVal = false; e.valueOffset = off; entries.push_back(e);
    };

    addInline(T_IMAGE_WIDTH,  TY_LONG, 1, static_cast<uint32_t>(width));
    addInline(T_IMAGE_LENGTH, TY_LONG, 1, static_cast<uint32_t>(height));
    addOutline(T_BITS_PER_SAMPLE, TY_SHORT, 3, bitsOffset);
    addInline(T_COMPRESSION, TY_SHORT, 1, compTag);
    addInline(T_PHOTOMETRIC, TY_SHORT, 1, PHOTOMETRIC_RGB);
    // (ImageDescription tag 270 inserted via strings, sorted later)
    addInline(T_STRIP_OFFSETS, TY_LONG, 1, stripOffset);
    addInline(T_SAMPLES_PER_PIXEL, TY_SHORT, 1, 3);
    addInline(T_ROWS_PER_STRIP, TY_LONG, 1, static_cast<uint32_t>(height));
    addInline(T_STRIP_BYTE_COUNTS, TY_LONG, 1, stripByteCount);
    addOutline(T_X_RESOLUTION, TY_RATIONAL, 1, xresOffset);
    addOutline(T_Y_RESOLUTION, TY_RATIONAL, 1, yresOffset);
    addInline(T_PLANAR_CONFIG, TY_SHORT, 1, 1);  // chunky
    addInline(T_RESOLUTION_UNIT, TY_SHORT, 1, meta.resolutionUnit);
    addOutline(T_SAMPLE_FORMAT, TY_SHORT, 3, sampleFmtOffset);

    for (auto& sl : strLocs) {
        uint32_t len = static_cast<uint32_t>(sl.s.size()) + 1;
        if (sl.inlineVal) {
            uint32_t packed = 0;
            for (uint32_t k = 0; k < sl.s.size(); ++k)
                packed |= static_cast<uint32_t>(static_cast<uint8_t>(sl.s[k])) << (8 * k);
            // NUL terminator already implied by zero-fill of remaining bytes.
            addInline(sl.tag, TY_ASCII, len, packed);
        } else {
            addOutline(sl.tag, TY_ASCII, len, static_cast<uint32_t>(sl.offset));
        }
    }
    if (hasIcc)  addOutline(T_ICC_PROFILE, TY_UNDEFINED,
                            static_cast<uint32_t>(meta.iccProfile.size()), iccOffset);
    if (hasExif) addOutline(T_EXIF_IFD, TY_LONG, 1, exifIfdOffset);

    // Sort by tag (TIFF requires ascending tag order within an IFD).
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    // Emit IFD0.
    putU16(outBytes, static_cast<uint16_t>(entries.size()));
    for (const auto& e : entries) {
        putU16(outBytes, e.tag);
        putU16(outBytes, e.type);
        putU32(outBytes, e.count);
        if (e.inlineVal) putU32(outBytes, e.inlineBytes);
        else             putU32(outBytes, e.valueOffset);
    }
    putU32(outBytes, 0);  // next IFD = none

    // Pad to bitsOffset and emit out-of-line value blobs in offset order.
    auto padTo = [&](uint32_t off) { while (outBytes.size() < off) outBytes.push_back(0); };

    padTo(bitsOffset);
    putU16(outBytes, bitsValue); putU16(outBytes, bitsValue); putU16(outBytes, bitsValue);  // BitsPerSample
    padTo(sampleFmtOffset);
    putU16(outBytes, sampleFormatValue); putU16(outBytes, sampleFormatValue); putU16(outBytes, sampleFormatValue);  // SampleFormat (1=uint, 3=IEEE float)
    padTo(xresOffset);
    { uint32_t n, d; doubleToRational(meta.xResolution, n, d); putU32(outBytes, n); putU32(outBytes, d); }
    padTo(yresOffset);
    { uint32_t n, d; doubleToRational(meta.yResolution, n, d); putU32(outBytes, n); putU32(outBytes, d); }

    for (auto& sl : strLocs) {
        if (sl.inlineVal) continue;
        padTo(static_cast<uint32_t>(sl.offset));
        outBytes.insert(outBytes.end(), sl.s.begin(), sl.s.end());
        outBytes.push_back(0);  // NUL
    }

    if (hasIcc) {
        padTo(iccOffset);
        outBytes.insert(outBytes.end(), meta.iccProfile.begin(), meta.iccProfile.end());
    }

    if (hasExif) {
        padTo(exifIfdOffset);
        // EXIF sub-IFD: 4 inline entries, ascending tag order.
        putU16(outBytes, exifCount);
        // ExifVersion (36864) UNDEFINED[4] = "0230"
        putU16(outBytes, E_EXIF_VERSION); putU16(outBytes, TY_UNDEFINED); putU32(outBytes, 4);
        outBytes.push_back('0'); outBytes.push_back('2'); outBytes.push_back('3'); outBytes.push_back('0');
        // ColorSpace (40961) SHORT
        putU16(outBytes, E_COLOR_SPACE); putU16(outBytes, TY_SHORT); putU32(outBytes, 1);
        putU16(outBytes, meta.exifColorSpace); putU16(outBytes, 0);
        // PixelXDimension (40962) LONG
        putU16(outBytes, E_PIXEL_X_DIM); putU16(outBytes, TY_LONG); putU32(outBytes, 1);
        putU32(outBytes, static_cast<uint32_t>(width));
        // PixelYDimension (40963) LONG
        putU16(outBytes, E_PIXEL_Y_DIM); putU16(outBytes, TY_LONG); putU32(outBytes, 1);
        putU32(outBytes, static_cast<uint32_t>(height));
        putU32(outBytes, 0);  // next IFD = none
    }

    if (isCancelled(cancellation)) return cancelWithoutPublication();
    padTo(stripOffset);
    if (stripOut != nullptr) {
        // The caller writes the strip straight to the file after this header.
        res.bytesWritten = outBytes.size() + strip.size();
        *stripOut = std::move(strip);
        res.ok = true;
        outputGuard.publish();
        return res;
    }
    constexpr size_t kCopyChunk = 64u * 1024u;
    for (size_t offset = 0; offset < strip.size();) {
        if (isCancelled(cancellation)) return cancelWithoutPublication();
        const size_t count = std::min(kCopyChunk, strip.size() - offset);
        outBytes.insert(outBytes.end(), strip.data() + offset,
                        strip.data() + offset + count);
        offset += count;
    }
    if (isCancelled(cancellation)) return cancelWithoutPublication();

    res.ok = true;
    res.bytesWritten = outBytes.size();
    outputGuard.publish();
    return res;
}

static TiffWriteResult writeTiff16Impl(const uint16_t* rgb16, int width, int height,
                                       const TiffMetadata& meta, TiffCompression compression,
                                       std::vector<uint8_t>& outBytes,
                                       const TiffCancellation* cancellation,
                                       std::vector<uint8_t>* stripOut) {
    outBytes.clear();
    if (rgb16 == nullptr) { TiffWriteResult res; res.error = "null pixel buffer"; return res; }
    TiffWriteResult res;
    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t rawStripBytes = 0;
    if (!imageLayout(width, height, 2u, rowSamples64, rowBytes64,
                     rawStripBytes, res.error) ||
        !validateUncompressedLayout(rawStripBytes, meta, res.error)) return res;
    if (isCancelled(cancellation)) return cancelledResult();

    // Serialise RGB uint16 little-endian.
    std::vector<uint8_t> raw(static_cast<size_t>(rawStripBytes));
    uint8_t* d = raw.data();
    const size_t rowSamples = static_cast<size_t>(rowSamples64);
    for (int y = 0; y < height; ++y) {
        if (isCancelled(cancellation)) return cancelledResult();
        const uint16_t* row = rgb16 + static_cast<size_t>(y) * rowSamples;
        for (size_t s = 0; s < rowSamples; ++s) {
            if ((s & 0x7fffu) == 0u && isCancelled(cancellation))
                return cancelledResult();
            const uint16_t v = row[s];
            *d++ = static_cast<uint8_t>(v & 0xFF);
            *d++ = static_cast<uint8_t>((v >> 8) & 0xFF);
        }
    }
    return writeTiffSamplesToMemory(std::move(raw), width, height, 2, 1,
                                    meta, compression, outBytes, cancellation, stripOut);

}

TiffWriteResult writeTiff16ToMemory(const uint16_t* rgb16, int width, int height,
                                    const TiffMetadata& meta, TiffCompression compression,
                                    std::vector<uint8_t>& outBytes,
                                    const TiffCancellation* cancellation) {
    return writeTiff16Impl(rgb16, width, height, meta, compression, outBytes,
                           cancellation, nullptr);
}

// True 32-bit IEEE-float TIFF (SampleFormat=3, BitsPerSample=32): the engine's float
// samples are written verbatim, no quantisation — a high-bit-depth / scene-linear export.
TiffWriteResult writeTiff32fToMemory(const float* rgbFloat, int width, int height,
                                     const TiffMetadata& meta, TiffCompression compression,
                                     std::vector<uint8_t>& outBytes,
                                     const TiffCancellation* cancellation) {
    outBytes.clear();
    if (rgbFloat == nullptr) { TiffWriteResult res; res.error = "null float buffer"; return res; }
    TiffWriteResult res;
    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t rawStripBytes = 0;
    if (!imageLayout(width, height, 4u, rowSamples64, rowBytes64,
                     rawStripBytes, res.error) ||
        !validateUncompressedLayout(rawStripBytes, meta, res.error)) return res;
    if (isCancelled(cancellation)) return cancelledResult();

    // float32 little-endian: our targets (arm64/x86_64) and the test host are all
    // little-endian, matching the "II" file byte order, so the raw float bytes copy verbatim.
    std::vector<uint8_t> raw(static_cast<size_t>(rawStripBytes));
    const size_t rowBytes = static_cast<size_t>(rowBytes64);
    for (int y = 0; y < height; ++y) {
        const size_t rowStart = static_cast<size_t>(y) * rowBytes;
        for (size_t offset = 0; offset < rowBytes;) {
            if (isCancelled(cancellation)) return cancelledResult();
            const size_t count = std::min<size_t>(64u * 1024u, rowBytes - offset);
            std::memcpy(raw.data() + rowStart + offset,
                        reinterpret_cast<const uint8_t*>(rgbFloat) + rowStart + offset,
                        count);
            offset += count;
        }
    }
    return writeTiffSamplesToMemory(std::move(raw), width, height, 4, 3,
                                    meta, compression, outBytes, cancellation);
}

TiffWriteResult writeTiff16ToFile(const uint16_t* rgb16, int width, int height,
                                  const TiffMetadata& meta, TiffCompression compression,
                                  const std::string& path,
                                  const TiffCancellation* cancellation) {
    TiffWriteResult pathResult;
    if (!TempOutput::validatePath(path, pathResult.error)) return pathResult;
    // Header and strip are written as two spans, so the whole file is never
    // assembled in memory: that copy was 75 MB at 12.5 MP (#175).
    std::vector<uint8_t> header;
    std::vector<uint8_t> strip;
    TiffWriteResult res = writeTiff16Impl(
        rgb16, width, height, meta, compression, header, cancellation, &strip);
    if (!res.ok) return res;
    return publishAtomically(header, path, cancellation, std::move(res), &strip);
}

TiffWriteResult writeTiff32fToFile(const float* rgbFloat, int width, int height,
                                   const TiffMetadata& meta, TiffCompression compression,
                                   const std::string& path,
                                   const TiffCancellation* cancellation) {
    TiffWriteResult pathResult;
    if (!TempOutput::validatePath(path, pathResult.error)) return pathResult;
    std::vector<uint8_t> bytes;
    TiffWriteResult res = writeTiff32fToMemory(
        rgbFloat, width, height, meta, compression, bytes, cancellation);
    if (!res.ok) return res;
    return publishAtomically(bytes, path, cancellation, std::move(res));
}

// Quantise float [0,1] -> LE uint16 bytes in ONE pass, straight into the strip.
//
// The old shape cost two full images on top of the engine's float buffer: a
// uint16 plane, then the byte serialisation of it. At 12.5 MP that is 75 MB
// twice, and a THIRD 75 MB before this existed, because the caller quantised in
// Kotlin and handed the writer a uint16 buffer (#175). Rounding is unchanged --
// same clamp, same +0.5 -- so the file is byte-identical to the uint16 entry
// point and #126's container-identity level is untouched.
TiffWriteResult writeTiffFloatToFile(const float* rgbFloat, int width, int height,
                                     const TiffMetadata& meta, TiffCompression compression,
                                     const std::string& path,
                                     const TiffCancellation* cancellation) {
    TiffWriteResult res;
    if (!TempOutput::validatePath(path, res.error)) return res;
    if (rgbFloat == nullptr) { res.error = "null float buffer"; return res; }
    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t rawStripBytes = 0;
    if (!imageLayout(width, height, 2u, rowSamples64, rowBytes64,
                     rawStripBytes, res.error) ||
        !validateUncompressedLayout(rawStripBytes, meta, res.error)) return res;
    if (isCancelled(cancellation)) return cancelledResult();

    const size_t rowSamples = static_cast<size_t>(rowSamples64);
    std::vector<uint8_t> raw(static_cast<size_t>(rawStripBytes));
    uint8_t* d = raw.data();
    for (int y = 0; y < height; ++y) {
        if (isCancelled(cancellation)) return cancelledResult();
        const float* row = rgbFloat + static_cast<size_t>(y) * rowSamples;
        for (size_t x = 0; x < rowSamples; ++x) {
            if ((x & 0x7fffu) == 0u && isCancelled(cancellation))
                return cancelledResult();
            const float v = row[x];
            // NaN takes the first branch (every comparison with NaN is false),
            // which is the same 0 the uint16 path produces for it.
            uint16_t q;
            if (!(v > 0.0f)) q = 0;
            else if (v >= 1.0f) q = 65535;
            else q = static_cast<uint16_t>(v * 65535.0f + 0.5f);
            *d++ = static_cast<uint8_t>(q & 0xFF);
            *d++ = static_cast<uint8_t>((q >> 8) & 0xFF);
        }
    }

    std::vector<uint8_t> header;
    std::vector<uint8_t> strip;
    res = writeTiffSamplesToMemory(std::move(raw), width, height, 2, 1, meta,
                                   compression, header, cancellation, &strip);
    if (!res.ok) return res;
    return publishAtomically(header, path, cancellation, std::move(res), &strip);
}

}  // namespace spectrafilm
