/*
 * Spektrafilm for Android — lib:pngwriter 16-bit PNG writer implementation.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Writes a valid 16-bit-per-channel RGB PNG stream. Format details:
 *
 *   PNG signature  (8 bytes, always \x89PNG\r\n\x1a\n)
 *   IHDR chunk     (13 data bytes: width, height, bit_depth=16, color_type=2 RGB,
 *                   compression=0, filter=0, interlace=0)
 *   iCCP chunk     (optional; written when PngMetadata::iccProfile is non-empty;
 *                   profile name "ICC Profile\0", compression method 0 = zlib deflate,
 *                   compressed profile bytes — zlib compress2() or deflate)
 *   tEXt chunk     (optional "Software\0<value>" keyword:value pair)
 *   IDAT chunk     (one chunk containing the zlib-deflated filtered scanline data)
 *   IEND chunk     (zero-length end-of-file marker)
 *
 * 16-bit byte order:
 *   The PNG spec (RFC 2083 §2.3 / ISO 15948:2003 §2.1) requires multi-byte
 *   samples to be stored big-endian. The engine delivers little-endian uint16
 *   (native ARM/x86). We byte-swap each sample before deflating. No libpng is
 *   used; byte-swap is hand-rolled (high = v>>8, low = v&0xFF, emitted in that
 *   order into the filtered-row buffer).
 *
 * CRC32:
 *   Every PNG chunk has a 4-byte CRC32 covering the chunk-type bytes plus the
 *   chunk data bytes. We use zlib's crc32() initialised with crc32(0,Z_NULL,0).
 *   This is correct per the PNG spec which mandates ISO 3309 CRC32 — exactly
 *   the polynomial zlib implements.
 *
 * IDAT deflate:
 *   We use zlib's deflate (compress2 / deflateInit2 with windowBits=15 for zlib
 *   wrapper, level Z_DEFAULT_COMPRESSION). Scanlines are fed to deflate ONE ROW AT
 *   A TIME, so the filtered image is never materialised; the resulting bytes are
 *   identical to compressing the whole buffer in one shot and become a single IDAT
 *   chunk (see IdatDeflater, and the byte-identity case in tests/).
 *
 * Per-scanline filter byte:
 *   Each scanline is prefixed with a 1-byte filter type. We use filter 0 (None)
 *   throughout — the sample bytes are passed through unchanged. Filter 0 is
 *   always correct per the PNG spec; for 16-bit RGB the benefit from Paeth/Sub
 *   is marginal enough that simplicity wins here.
 *
 * iCCP chunk:
 *   The raw ICC bytes are zlib-compressed (compress2) and stored as:
 *     profile_name  NUL  compression_method(0)  compressed_data
 *   Profile name is "ICC Profile" (the conventional value; any name ≤79 bytes
 *   works). Compression method 0 is the only defined value per PNG spec §11.3.3.2.
 *
 * zlib on Android:
 *   The NDK sysroot provides libz.so (dynamically linked into any Android process)
 *   and libz.a for static linking. CMakeLists.txt uses find_library(z-lib z) and
 *   target_link_libraries(sfpng ${z-lib}) — on Android this resolves to the
 *   system-provided /system/lib[64]/libz.so, exactly as libraw does for its own
 *   zlib dependency.
 *
 * Host build:
 *   g++ -std=c++17 -O2 -I<dir> png_writer.cpp -lz  (system zlib1g-dev)
 *   The iCCP / IDAT / CRC paths are 100% portable POSIX/C++17; no Android headers.
 */
#include "png_writer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <zlib.h>

namespace spectrafilm {
namespace {

constexpr uint64_t kPngMaxChunkLength = 0x7fffffffull;

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

bool imageLayout(int width, int height, uint64_t bytesPerSample,
                 uint64_t& rowSamples, uint64_t& rowBytes,
                 uint64_t& filteredBytes, std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "invalid dimensions";
        return false;
    }
    uint64_t filteredRowBytes = 0;
    if (!checkedMul(static_cast<uint64_t>(width), 3u, rowSamples) ||
        !checkedMul(rowSamples, bytesPerSample, rowBytes) ||
        !checkedAdd(rowBytes, 1u, filteredRowBytes) ||
        !checkedMul(filteredRowBytes, static_cast<uint64_t>(height), filteredBytes) ||
        rowSamples > static_cast<uint64_t>(SIZE_MAX) ||
        rowBytes > static_cast<uint64_t>(SIZE_MAX) ||
        filteredBytes > static_cast<uint64_t>(SIZE_MAX) ||
        filteredBytes > kPngMaxChunkLength) {
        error = "image too large for PNG scanline layout";
        return false;
    }
    return true;
}

bool isCancelled(const PngCancellation* cancellation) noexcept {
    return cancellation != nullptr && cancellation->isCancelled != nullptr &&
           cancellation->isCancelled(cancellation->context);
}

PngWriteResult cancelledResult() {
    PngWriteResult result;
    result.cancelled = true;
    result.error = "cancelled";
    return result;
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

// ---- big-endian emitters ---------------------------------------------------
// PNG is big-endian for all multi-byte integers in chunk headers and IHDR data.

static void putBE32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Append a complete PNG chunk:  length(4) + type(4) + data(n) + crc32(4).
// The CRC covers type bytes + data bytes (per PNG spec §5.3).
enum class AppendStatus { Ok, Failed, Cancelled };

static AppendStatus appendChunk(std::vector<uint8_t>& out,
                                const char type[4],
                                const uint8_t* data, size_t len,
                                std::string& error,
                                const PngCancellation* cancellation) {
    uint64_t added = 0;
    if (len > kPngMaxChunkLength || !checkedAdd(static_cast<uint64_t>(len), 12u, added) ||
        added > static_cast<uint64_t>(SIZE_MAX) - static_cast<uint64_t>(out.size())) {
        error = "PNG chunk or output layout is too large";
        return AppendStatus::Failed;
    }
    if (isCancelled(cancellation)) return AppendStatus::Cancelled;
    putBE32(out, static_cast<uint32_t>(len));
    out.push_back(static_cast<uint8_t>(type[0]));
    out.push_back(static_cast<uint8_t>(type[1]));
    out.push_back(static_cast<uint8_t>(type[2]));
    out.push_back(static_cast<uint8_t>(type[3]));
    constexpr size_t kCopyChunk = 64u * 1024u;
    if (data != nullptr) {
        for (size_t offset = 0; offset < len;) {
            if (isCancelled(cancellation)) return AppendStatus::Cancelled;
            const size_t count = std::min(kCopyChunk, len - offset);
            out.insert(out.end(), data + offset, data + offset + count);
            offset += count;
        }
    }

    // CRC32 over the 4 type bytes + data bytes.
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
    if (data != nullptr) {
        for (size_t offset = 0; offset < len;) {
            if (isCancelled(cancellation)) return AppendStatus::Cancelled;
            const size_t count = std::min(kCopyChunk, len - offset);
            crc = crc32(crc, reinterpret_cast<const Bytef*>(data + offset),
                        static_cast<uInt>(count));
            offset += count;
        }
    }
    putBE32(out, static_cast<uint32_t>(crc));
    return AppendStatus::Ok;
}

// Convenience overload for vector data.
static AppendStatus appendChunk(std::vector<uint8_t>& out,
                                const char type[4],
                                const std::vector<uint8_t>& data,
                                std::string& error,
                                const PngCancellation* cancellation) {
    return appendChunk(out, type, data.empty() ? nullptr : data.data(),
                       data.size(), error, cancellation);
}

// ---- zlib compress (streaming + cancellable) -------------------------------
enum class ZlibStatus { Ok, Failed, Cancelled };

class DeflateGuard final {
public:
    explicit DeflateGuard(z_stream& stream) noexcept : stream_(stream) {}
    DeflateGuard(const DeflateGuard&) = delete;
    DeflateGuard& operator=(const DeflateGuard&) = delete;
    ~DeflateGuard() { deflateEnd(&stream_); }

private:
    z_stream& stream_;
};

static ZlibStatus zlibCompress(const uint8_t* src, size_t srcLen,
                               std::vector<uint8_t>& dst, std::string& errOut,
                               const PngCancellation* cancellation) {
    dst.clear();
    if (srcLen > kPngMaxChunkLength ||
        srcLen > static_cast<size_t>(std::numeric_limits<uLong>::max())) {
        errOut = "zlib input exceeds PNG chunk limits";
        return ZlibStatus::Failed;
    }
    if (isCancelled(cancellation)) return ZlibStatus::Cancelled;

    z_stream stream{};
    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        errOut = "zlib initialisation failed";
        return ZlibStatus::Failed;
    }
    DeflateGuard guard(stream);

    constexpr size_t kZlibChunk = 64u * 1024u;
    std::array<uint8_t, kZlibChunk> output{};
    dst.reserve(static_cast<size_t>(compressBound(static_cast<uLong>(srcLen))));

    size_t inputOffset = 0;
    int rc = Z_OK;
    do {
        if (isCancelled(cancellation)) {
            dst.clear();
            return ZlibStatus::Cancelled;
        }
        const size_t inputSize = std::min(kZlibChunk, srcLen - inputOffset);
        stream.next_in = inputSize == 0
            ? Z_NULL
            : const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src + inputOffset));
        stream.avail_in = static_cast<uInt>(inputSize);
        inputOffset += inputSize;
        const int flush = inputOffset == srcLen ? Z_FINISH : Z_NO_FLUSH;

        do {
            if (isCancelled(cancellation)) {
                dst.clear();
                return ZlibStatus::Cancelled;
            }
            stream.next_out = reinterpret_cast<Bytef*>(output.data());
            stream.avail_out = static_cast<uInt>(output.size());
            rc = deflate(&stream, flush);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                dst.clear();
                errOut = "zlib deflate failed";
                return ZlibStatus::Failed;
            }
            const size_t produced = output.size() - stream.avail_out;
            if (produced > kPngMaxChunkLength - dst.size()) {
                dst.clear();
                errOut = "compressed PNG chunk exceeds format limits";
                return ZlibStatus::Failed;
            }
            dst.insert(dst.end(), output.data(), output.data() + produced);
        } while (stream.avail_in != 0 || stream.avail_out == 0 ||
                 (flush == Z_FINISH && rc != Z_STREAM_END));
    } while (rc != Z_STREAM_END);
    return ZlibStatus::Ok;
}

// ---- streaming IDAT --------------------------------------------------------
//
// The filtered image used to be materialised in full before compression: at
// 12.5 MP that is a 75 MB buffer whose only purpose is to be read once, on top of
// the 75 MB of uint16 samples and the ~75 MB of compressed output (#175).
//
// Feeding deflate one scanline at a time removes it. The compressed BYTES are
// unchanged: this pushes exactly the same total stream through exactly the same
// deflate settings with Z_NO_FLUSH, which is what the whole-buffer path above
// already did in 64 KB pieces -- zlib inserts no sync point for Z_NO_FLUSH, so
// where the input is split cannot change the output. tests/test_png_writer.cpp
// asserts that byte-for-byte, because PNG16 container identity is a gated
// contract, not a preference.
class IdatDeflater final {
public:
    IdatDeflater(std::vector<uint8_t>& dst, const PngCancellation* cancellation) noexcept
        : dst_(dst), cancellation_(cancellation) {}

    IdatDeflater(const IdatDeflater&) = delete;
    IdatDeflater& operator=(const IdatDeflater&) = delete;

    ~IdatDeflater() {
        if (open_) deflateEnd(&stream_);
    }

    // [totalInputBytes] is only a sizing hint: without it the output vector grows
    // geometrically and its reallocation transiently holds two copies, which on a
    // 12.5 MP frame costs more than the buffer this class exists to remove.
    bool init(uint64_t totalInputBytes, std::string& errOut) {
        if (deflateInit(&stream_, Z_DEFAULT_COMPRESSION) != Z_OK) {
            errOut = "zlib initialisation failed";
            return false;
        }
        open_ = true;
        if (totalInputBytes > 0 &&
            totalInputBytes <= static_cast<uint64_t>(std::numeric_limits<uLong>::max())) {
            const uLong bound = compressBound(static_cast<uLong>(totalInputBytes));
            if (bound <= kPngMaxChunkLength) dst_.reserve(static_cast<size_t>(bound));
        }
        return true;
    }

    ZlibStatus push(const uint8_t* data, size_t len, std::string& errOut) {
        return run(data, len, Z_NO_FLUSH, errOut);
    }

    ZlibStatus finish(std::string& errOut) {
        return run(nullptr, 0, Z_FINISH, errOut);
    }

private:
    ZlibStatus run(const uint8_t* data, size_t len, int flush, std::string& errOut) {
        if (isCancelled(cancellation_)) { dst_.clear(); return ZlibStatus::Cancelled; }
        stream_.next_in = len == 0
            ? Z_NULL
            : const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
        stream_.avail_in = static_cast<uInt>(len);
        int rc = Z_OK;
        do {
            if (isCancelled(cancellation_)) { dst_.clear(); return ZlibStatus::Cancelled; }
            stream_.next_out = reinterpret_cast<Bytef*>(output_.data());
            stream_.avail_out = static_cast<uInt>(output_.size());
            rc = deflate(&stream_, flush);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
                dst_.clear();
                errOut = "zlib deflate failed";
                return ZlibStatus::Failed;
            }
            const size_t produced = output_.size() - stream_.avail_out;
            if (produced > kPngMaxChunkLength - dst_.size()) {
                dst_.clear();
                errOut = "compressed PNG chunk exceeds format limits";
                return ZlibStatus::Failed;
            }
            dst_.insert(dst_.end(), output_.data(), output_.data() + produced);
        } while (stream_.avail_in != 0 || stream_.avail_out == 0 ||
                 (flush == Z_FINISH && rc != Z_STREAM_END));
        return ZlibStatus::Ok;
    }

    z_stream stream_{};
    bool open_ = false;
    std::vector<uint8_t>& dst_;
    const PngCancellation* cancellation_ = nullptr;
    std::array<uint8_t, 64u * 1024u> output_{};
};

// One scanline of uint16 samples at a time, so neither the caller's quantized
// image nor a quantized copy of it has to exist in full.
class RowSource {
public:
    virtual ~RowSource() = default;
    // Returns rowSamples samples for row y, or nullptr on failure.
    virtual const uint16_t* row(int y) = 0;
};

class U16RowSource final : public RowSource {
public:
    U16RowSource(const uint16_t* base, size_t rowSamples) noexcept
        : base_(base), rowSamples_(rowSamples) {}
    const uint16_t* row(int y) override {
        return base_ + static_cast<size_t>(y) * rowSamples_;
    }

private:
    const uint16_t* base_;
    size_t rowSamples_;
};

// Quantizes float -> uint16 one row at a time. Identical arithmetic to the
// whole-image loop it replaces, including the not-greater-than-zero test that
// maps NaN to 0.
class FloatRowSource final : public RowSource {
public:
    FloatRowSource(const float* base, size_t rowSamples)
        : base_(base), rowSamples_(rowSamples), scratch_(rowSamples) {}
    const uint16_t* row(int y) override {
        const float* src = base_ + static_cast<size_t>(y) * rowSamples_;
        for (size_t x = 0; x < rowSamples_; ++x) {
            const float v = src[x];
            if (!(v > 0.0f)) { scratch_[x] = 0; continue; }
            if (v >= 1.0f) { scratch_[x] = 65535; continue; }
            scratch_[x] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
        }
        return scratch_.data();
    }

private:
    const float* base_;
    size_t rowSamples_;
    std::vector<uint16_t> scratch_;
};

}  // namespace

// ---- writePng16ToMemory ----------------------------------------------------

// The whole encoder, driven one scanline at a time. Both public entry points are
// thin wrappers: writePng16ToMemory hands it rows of the caller's buffer, and the
// float entry hands it rows it quantizes on the way past, so neither a filtered
// image nor a quantized copy is ever allocated in full (#175).
static PngWriteResult writePngRows(RowSource& rows, int width, int height,
                                   const PngMetadata& meta,
                                   std::vector<uint8_t>& outBytes,
                                   const PngCancellation* cancellation) {
    PngWriteResult res;
    outBytes.clear();
    MemoryOutputGuard outputGuard(outBytes);

    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t filtBufSize64 = 0;
    if (!imageLayout(width, height, 2u, rowSamples64, rowBytes64,
                     filtBufSize64, res.error)) return res;
    if (isCancelled(cancellation)) return cancelledResult();

    auto cancelWithoutPublication = [&outBytes]() {
        outBytes.clear();
        return cancelledResult();
    };

    // ---- 1. PNG signature --------------------------------------------------
    // \x89 P N G \r \n \x1a \n  (RFC 2083 §5.2)
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    outBytes.insert(outBytes.end(), kSig, kSig + 8);

    // ---- 2. IHDR chunk (13 data bytes) -------------------------------------
    // width(4BE) height(4BE) bit_depth(1) color_type(1) compression(1) filter(1) interlace(1)
    // bit_depth=16, color_type=2 (RGB), compression=0, filter=0, interlace=0
    {
        uint8_t ihdr[13];
        // width big-endian
        ihdr[0] = static_cast<uint8_t>((static_cast<uint32_t>(width) >> 24) & 0xFF);
        ihdr[1] = static_cast<uint8_t>((static_cast<uint32_t>(width) >> 16) & 0xFF);
        ihdr[2] = static_cast<uint8_t>((static_cast<uint32_t>(width) >>  8) & 0xFF);
        ihdr[3] = static_cast<uint8_t>( static_cast<uint32_t>(width)        & 0xFF);
        // height big-endian
        ihdr[4] = static_cast<uint8_t>((static_cast<uint32_t>(height) >> 24) & 0xFF);
        ihdr[5] = static_cast<uint8_t>((static_cast<uint32_t>(height) >> 16) & 0xFF);
        ihdr[6] = static_cast<uint8_t>((static_cast<uint32_t>(height) >>  8) & 0xFF);
        ihdr[7] = static_cast<uint8_t>( static_cast<uint32_t>(height)        & 0xFF);
        ihdr[8]  = 16;  // bit_depth
        ihdr[9]  = 2;   // color_type = RGB (no alpha)
        ihdr[10] = 0;   // compression method 0 = deflate (only defined value)
        ihdr[11] = 0;   // filter method 0 (only defined value)
        ihdr[12] = 0;   // interlace = 0 (no interlace)
        const AppendStatus status = appendChunk(
            outBytes, "IHDR", ihdr, 13, res.error, cancellation);
        if (status == AppendStatus::Cancelled) return cancelWithoutPublication();
        if (status == AppendStatus::Failed) return res;
    }

    // ---- 3. iCCP chunk (optional: non-empty iccProfile) --------------------
    // Format: profile_name NUL compression_method(0) compressed_profile_data
    if (!meta.iccProfile.empty()) {
        // Compress the raw ICC bytes with zlib.
        std::vector<uint8_t> compressedIcc;
        std::string zlibErr;
        const ZlibStatus iccStatus = zlibCompress(
            meta.iccProfile.data(), meta.iccProfile.size(), compressedIcc,
            zlibErr, cancellation);
        if (iccStatus == ZlibStatus::Cancelled) return cancelWithoutPublication();
        if (iccStatus == ZlibStatus::Failed) {
            res.error = "iCCP: " + zlibErr;
            return res;
        }
        if (isCancelled(cancellation)) return cancelWithoutPublication();

        // Assemble iCCP chunk data.
        static const char kProfileName[] = "ICC Profile";  // including NUL
        const size_t nameLen = sizeof(kProfileName);       // strlen + NUL = 12
        std::vector<uint8_t> iccpData;
        iccpData.reserve(nameLen + 1 + compressedIcc.size());
        iccpData.insert(iccpData.end(),
                        reinterpret_cast<const uint8_t*>(kProfileName),
                        reinterpret_cast<const uint8_t*>(kProfileName) + nameLen);
        iccpData.push_back(0);  // compression method = 0 (zlib)
        iccpData.insert(iccpData.end(), compressedIcc.begin(), compressedIcc.end());
        const AppendStatus status = appendChunk(
            outBytes, "iCCP", iccpData, res.error, cancellation);
        if (status == AppendStatus::Cancelled) return cancelWithoutPublication();
        if (status == AppendStatus::Failed) return res;
    }

    // ---- 4. tEXt chunk (optional: non-empty software) ----------------------
    // Format: keyword NUL value  (no compression for tEXt; use iTXt for UTF-8)
    if (!meta.software.empty()) {
        static const char kKeyword[] = "Software";  // including NUL = 9 bytes
        std::vector<uint8_t> txtData;
        txtData.reserve(sizeof(kKeyword) + meta.software.size());
        txtData.insert(txtData.end(),
                       reinterpret_cast<const uint8_t*>(kKeyword),
                       reinterpret_cast<const uint8_t*>(kKeyword) + sizeof(kKeyword));
        txtData.insert(txtData.end(), meta.software.begin(), meta.software.end());
        const AppendStatus status = appendChunk(
            outBytes, "tEXt", txtData, res.error, cancellation);
        if (status == AppendStatus::Cancelled) return cancelWithoutPublication();
        if (status == AppendStatus::Failed) return res;
    }

    // ---- 5. IDAT chunk: build filtered scanline buffer, then deflate -------
    //
    // For a 16-bit RGB image with filter 0 (None):
    //   Each scanline is: 1 filter-type byte (0x00) followed by
    //   width * 3 * 2 raw big-endian sample bytes.
    // All scanlines are concatenated into one buffer, then zlib-compressed.
    // The compressed bytes form a single IDAT chunk.
    //
    // Big-endian byte-swap: PNG samples are stored high-byte first.
    // Input rgb16 samples are native uint16 (little-endian machine words on
    // ARM/x86); we emit (v>>8) then (v&0xFF) to get big-endian.
    {
        const size_t rowSamples = static_cast<size_t>(rowSamples64);

        // One scanline at a time: the filtered image is never materialised, so the
        // 75 MB it cost at 12.5 MP is gone (#175). The compressed bytes are
        // identical -- same stream, same settings, only the input chunking differs.
        std::vector<uint8_t> idatData;
        std::vector<uint8_t> filteredRow(static_cast<size_t>(rowBytes64) + 1u);
        std::string zlibErr;
        {
            IdatDeflater deflater(idatData, cancellation);
            if (!deflater.init(filtBufSize64, res.error)) return res;
            for (int y = 0; y < height; ++y) {
                if (isCancelled(cancellation)) return cancelWithoutPublication();
                const uint16_t* src = rows.row(y);
                if (src == nullptr) {
                    res.error = "IDAT: row source failed";
                    return res;
                }
                uint8_t* dst = filteredRow.data();
                *dst++ = 0;  // filter byte = 0 (None)
                for (size_t x = 0; x < rowSamples; ++x) {
                    const uint16_t v = src[x];
                    *dst++ = static_cast<uint8_t>(v >> 8);    // high byte first (big-endian)
                    *dst++ = static_cast<uint8_t>(v & 0xFF);  // low byte second
                }
                const ZlibStatus rowStatus =
                    deflater.push(filteredRow.data(), filteredRow.size(), zlibErr);
                if (rowStatus == ZlibStatus::Cancelled) return cancelWithoutPublication();
                if (rowStatus == ZlibStatus::Failed) {
                    res.error = "IDAT: " + zlibErr;
                    return res;
                }
            }
            const ZlibStatus endStatus = deflater.finish(zlibErr);
            if (endStatus == ZlibStatus::Cancelled) return cancelWithoutPublication();
            if (endStatus == ZlibStatus::Failed) {
                res.error = "IDAT: " + zlibErr;
                return res;
            }
        }
        if (isCancelled(cancellation)) return cancelWithoutPublication();

        const AppendStatus status = appendChunk(
            outBytes, "IDAT", idatData, res.error, cancellation);
        if (status == AppendStatus::Cancelled) return cancelWithoutPublication();
        if (status == AppendStatus::Failed) return res;
    }

    // ---- 6. IEND chunk (zero-length) ---------------------------------------
    const AppendStatus endStatus = appendChunk(
        outBytes, "IEND", nullptr, 0, res.error, cancellation);
    if (endStatus == AppendStatus::Cancelled) return cancelWithoutPublication();
    if (endStatus == AppendStatus::Failed) return res;
    if (isCancelled(cancellation)) return cancelWithoutPublication();

    res.ok = true;
    res.bytesWritten = outBytes.size();
    outputGuard.publish();
    return res;
}

PngWriteResult writePng16ToMemory(const uint16_t* rgb16, int width, int height,
                                  const PngMetadata& meta,
                                  std::vector<uint8_t>& outBytes,
                                  const PngCancellation* cancellation) {
    PngWriteResult res;
    outBytes.clear();
    if (rgb16 == nullptr) { res.error = "null pixel buffer"; return res; }
    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t filtBufSize64 = 0;
    if (!imageLayout(width, height, 2u, rowSamples64, rowBytes64,
                     filtBufSize64, res.error)) return res;
    U16RowSource rows(rgb16, static_cast<size_t>(rowSamples64));
    return writePngRows(rows, width, height, meta, outBytes, cancellation);
}

// ---- writePng16ToFile ------------------------------------------------------

// Publish encoded bytes through the same write-to-temp-then-rename protocol both
// file entry points use, so a failed or cancelled write leaves no partial file.
static PngWriteResult writeBytesToPath(const std::vector<uint8_t>& bytes,
                                       const std::string& path,
                                       PngWriteResult res,
                                       const PngCancellation* cancellation) {
    TempOutput output;
    if (!output.open(path, res.error)) {
        res.ok = false;
        res.bytesWritten = 0;
        return res;
    }
    constexpr size_t kWriteChunk = 64u * 1024u;
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (isCancelled(cancellation)) return cancelledResult();
        const size_t count = std::min(kWriteChunk, bytes.size() - offset);
        if (!output.write(bytes.data() + offset, count, res.error)) {
            res.ok = false;
            res.bytesWritten = 0;
            return res;
        }
        offset += count;
    }
    if (isCancelled(cancellation)) return cancelledResult();
    if (!output.commit(path, res.error)) {
        res.ok = false;
        res.bytesWritten = 0;
        return res;
    }
    res.bytesWritten = bytes.size();
    return res;
}

PngWriteResult writePng16ToFile(const uint16_t* rgb16, int width, int height,
                                const PngMetadata& meta,
                                const std::string& path,
                                const PngCancellation* cancellation) {
    PngWriteResult pathResult;
    if (!TempOutput::validatePath(path, pathResult.error)) return pathResult;
    std::vector<uint8_t> bytes;
    PngWriteResult res = writePng16ToMemory(
        rgb16, width, height, meta, bytes, cancellation);
    if (!res.ok) return res;
    return writeBytesToPath(bytes, path, res, cancellation);
}

// ---- writePngFloatToFile ---------------------------------------------------

PngWriteResult writePngFloatToFile(const float* rgbFloat, int width, int height,
                                   const PngMetadata& meta,
                                   const std::string& path,
                                   const PngCancellation* cancellation) {
    PngWriteResult res;
    if (rgbFloat == nullptr) { res.error = "null float buffer"; return res; }
    uint64_t rowSamples64 = 0;
    uint64_t rowBytes64 = 0;
    uint64_t filteredBytes64 = 0;
    if (!imageLayout(width, height, 2u, rowSamples64, rowBytes64,
                     filteredBytes64, res.error)) return res;
    uint64_t sampleCount64 = 0;
    if (!checkedMul(rowSamples64, static_cast<uint64_t>(height), sampleCount64) ||
        sampleCount64 > static_cast<uint64_t>(SIZE_MAX)) {
        res.error = "image sample count is too large";
        return res;
    }
    if (isCancelled(cancellation)) return cancelledResult();

    // Quantize a row at a time on the way into the encoder. This used to build a
    // whole second image -- 75 MB at 12.5 MP, on top of the caller's float buffer
    // and everything the encoder itself staged (#175). Same arithmetic, same
    // output bytes; only the lifetime of the intermediate changed.
    const size_t rowSamples = static_cast<size_t>(rowSamples64);
    if (!TempOutput::validatePath(path, res.error)) return res;
    FloatRowSource rows(rgbFloat, rowSamples);
    std::vector<uint8_t> bytes;
    PngWriteResult encoded =
        writePngRows(rows, width, height, meta, bytes, cancellation);
    if (!encoded.ok) return encoded;
    return writeBytesToPath(bytes, path, encoded, cancellation);
}

}  // namespace spectrafilm
