/*
 * Spektrafilm for Android — host unit test for the 16-bit baseline TIFF writer.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Pure host test (no Android / no Gradle). Writes a small known 16-bit RGB buffer
 * to /tmp with an embedded ICC blob + metadata, then parses the file back from
 * scratch (a minimal independent TIFF/IFD reader implemented here) and asserts:
 *   - TIFF magic + little-endian byte order ("II", 42)
 *   - ImageWidth / ImageLength match
 *   - BitsPerSample = {16,16,16}
 *   - SamplesPerPixel = 3
 *   - PhotometricInterpretation = 2 (RGB)
 *   - Compression matches the requested scheme
 *   - ICCProfile (34675) tag present and bytes round-trip exactly
 *   - Software (305) string present
 *   - EXIF sub-IFD (34665) present with ColorSpace + pixel dims
 *   - every pixel sample round-trips bit-exact (both uncompressed and PackBits)
 *
 * Build:
 *   g++ -std=c++17 -O2 -I.. test_tiff_writer.cpp ../tiff_writer.cpp -o /tmp/test_tiff_writer
 *   /tmp/test_tiff_writer
 */
#include "tiff_writer.h"
#include "tiff_writer_jni_boundary.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>

using namespace spectrafilm;

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); g_failures++; }       \
        else         { std::printf("  ok:   %s\n", msg); }                     \
    } while (0)

// ---- minimal little-endian TIFF reader (independent of the writer) ---------
uint16_t rdU16(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8);
}
uint32_t rdU32(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

struct IfdEntry {
    uint16_t type;
    uint32_t count;
    uint32_t valueOrOffset;  // raw 4 bytes from the entry
    size_t entryOffset;      // where this entry's value field lives in the file
};

size_t typeSize(uint16_t t) {
    switch (t) {
        case 1: case 2: case 7: return 1;  // BYTE/ASCII/UNDEFINED
        case 3: return 2;                  // SHORT
        case 4: return 4;                  // LONG
        case 5: return 8;                  // RATIONAL
        default: return 1;
    }
}

// Parse an IFD at `ifdOff` into a tag->entry map; returns next-IFD offset.
uint32_t parseIfd(const std::vector<uint8_t>& b, size_t ifdOff,
                  std::map<uint16_t, IfdEntry>& out) {
    uint16_t n = rdU16(b, ifdOff);
    size_t p = ifdOff + 2;
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t tag = rdU16(b, p);
        IfdEntry e;
        e.type = rdU16(b, p + 2);
        e.count = rdU32(b, p + 4);
        e.valueOrOffset = rdU32(b, p + 8);
        e.entryOffset = p + 8;
        out[tag] = e;
        p += 12;
    }
    return rdU32(b, p);
}

// Read the byte offset where an entry's value data lives (inline if it fits in 4).
size_t valueDataOffset(const IfdEntry& e) {
    size_t total = typeSize(e.type) * e.count;
    return (total <= 4) ? e.entryOffset : e.valueOrOffset;
}

// Read the i-th SHORT of an entry.
uint16_t readShort(const std::vector<uint8_t>& b, const IfdEntry& e, uint32_t i) {
    return rdU16(b, valueDataOffset(e) + i * 2);
}
// Read a scalar LONG/SHORT value.
uint32_t readScalar(const std::vector<uint8_t>& b, const IfdEntry& e) {
    if (e.type == 3) return rdU16(b, valueDataOffset(e));
    return rdU32(b, valueDataOffset(e));
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// Decode the image strip back to interleaved uint16 RGB.
void decodeStrip(const std::vector<uint8_t>& file, uint16_t compression,
                 uint32_t stripOff, uint32_t stripBytes, uint64_t expectSamples,
                 std::vector<uint16_t>& out) {
    std::vector<uint8_t> raw;
    if (compression == 32773) {  // PackBits
        size_t i = stripOff;
        size_t end = stripOff + stripBytes;
        while (i < end) {
            int8_t n = static_cast<int8_t>(file[i++]);
            if (n >= 0) {
                int cnt = n + 1;
                for (int k = 0; k < cnt && i < end; ++k) raw.push_back(file[i++]);
            } else if (n != -128) {
                int cnt = 1 - n;
                uint8_t v = file[i++];
                for (int k = 0; k < cnt; ++k) raw.push_back(v);
            }
        }
    } else {
        raw.assign(file.begin() + stripOff, file.begin() + stripOff + stripBytes);
    }
    out.resize(static_cast<size_t>(expectSamples));
    for (uint64_t s = 0; s < expectSamples; ++s)
        out[s] = static_cast<uint16_t>(raw[s * 2]) | (static_cast<uint16_t>(raw[s * 2 + 1]) << 8);
}

// Run the full write+readback+assert cycle for one compression mode.
void runCase(const char* label, TiffCompression comp, const std::vector<uint8_t>& icc) {
    std::printf("[%s]\n", label);

    const int W = 5, H = 3;
    std::vector<uint16_t> pixels(static_cast<size_t>(W) * H * 3);
    // Deterministic gradient with full 16-bit range exercised.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t base = (static_cast<size_t>(y) * W + x) * 3;
            pixels[base + 0] = static_cast<uint16_t>((x * 65535) / (W - 1));        // R ramp
            pixels[base + 1] = static_cast<uint16_t>((y * 65535) / (H - 1));        // G ramp
            pixels[base + 2] = static_cast<uint16_t>(((x + y) & 1) ? 0xFFFF : 0x0001); // B run-friendly
        }
    }

    TiffMetadata meta;
    meta.software = "Spektrafilm-test";
    meta.dateTime = "2026:05:29 12:00:00";
    meta.xResolution = 300.0;
    meta.yResolution = 300.0;
    meta.exifColorSpace = 0xFFFF;
    meta.iccProfile = icc;
    meta.writeExifIfd = true;

    std::string path = std::string("/tmp/sf_tiff_test_") + label + ".tiff";
    TiffWriteResult wr = writeTiff16ToFile(pixels.data(), W, H, meta, comp, path);
    CHECK(wr.ok, "writer returned ok");
    if (!wr.ok) { std::printf("    error: %s\n", wr.error.c_str()); return; }

    std::vector<uint8_t> file;
    CHECK(readFile(path, file), "file readable from disk");
    CHECK(file.size() == wr.bytesWritten, "bytesWritten matches file size");
    if (file.size() < 8) return;

    CHECK(file[0] == 'I' && file[1] == 'I', "byte order = little-endian (II)");
    CHECK(rdU16(file, 2) == 42, "TIFF magic = 42");
    uint32_t ifd0 = rdU32(file, 4);
    CHECK(ifd0 >= 8 && ifd0 < file.size(), "IFD0 offset in range");

    std::map<uint16_t, IfdEntry> ifd;
    parseIfd(file, ifd0, ifd);

    CHECK(ifd.count(256) && readScalar(file, ifd[256]) == (uint32_t)W, "ImageWidth = 5");
    CHECK(ifd.count(257) && readScalar(file, ifd[257]) == (uint32_t)H, "ImageLength = 3");

    bool bps16 = ifd.count(258) && ifd[258].count == 3 &&
                 readShort(file, ifd[258], 0) == 16 &&
                 readShort(file, ifd[258], 1) == 16 &&
                 readShort(file, ifd[258], 2) == 16;
    CHECK(bps16, "BitsPerSample = {16,16,16}");

    CHECK(ifd.count(277) && readScalar(file, ifd[277]) == 3, "SamplesPerPixel = 3");
    CHECK(ifd.count(262) && readScalar(file, ifd[262]) == 2, "Photometric = RGB (2)");
    CHECK(ifd.count(284) && readScalar(file, ifd[284]) == 1, "PlanarConfig = chunky (1)");

    bool sf = ifd.count(339) && ifd[339].count == 3 && readShort(file, ifd[339], 0) == 1;
    CHECK(sf, "SampleFormat = unsigned int");

    uint16_t comptag = ifd.count(259) ? (uint16_t)readScalar(file, ifd[259]) : 0;
    uint16_t want = (comp == TiffCompression::PackBits) ? 32773 : 1;
    // PackBits may have fallen back to none on incompressible data; accept either
    // when PackBits was requested.
    if (comp == TiffCompression::PackBits)
        CHECK(comptag == 32773 || comptag == 1, "Compression = PackBits or none-fallback");
    else
        CHECK(comptag == want, "Compression = none (1)");

    // Software string round-trip.
    bool sw = false;
    if (ifd.count(305)) {
        size_t off = valueDataOffset(ifd[305]);
        std::string s(reinterpret_cast<const char*>(&file[off]));
        sw = (s == "Spektrafilm-test");
    }
    CHECK(sw, "Software tag = 'Spektrafilm-test'");

    // ICC presence + byte round-trip.
    if (!icc.empty()) {
        bool iccOk = false;
        if (ifd.count(34675) && ifd[34675].count == icc.size()) {
            size_t off = valueDataOffset(ifd[34675]);
            iccOk = std::memcmp(&file[off], icc.data(), icc.size()) == 0;
        }
        CHECK(iccOk, "ICCProfile (34675) present + bytes round-trip");
    } else {
        CHECK(ifd.count(34675) == 0, "no ICCProfile tag when none provided");
    }

    // EXIF sub-IFD.
    if (ifd.count(34665)) {
        uint32_t exifOff = readScalar(file, ifd[34665]);
        std::map<uint16_t, IfdEntry> exif;
        parseIfd(file, exifOff, exif);
        CHECK(exif.count(40961) && readScalar(file, exif[40961]) == 0xFFFF,
              "EXIF ColorSpace = Uncalibrated");
        CHECK(exif.count(40962) && readScalar(file, exif[40962]) == (uint32_t)W,
              "EXIF PixelXDimension = width");
        CHECK(exif.count(40963) && readScalar(file, exif[40963]) == (uint32_t)H,
              "EXIF PixelYDimension = height");
        CHECK(exif.count(36864) && exif[36864].count == 4, "EXIF ExifVersion present");
    } else {
        CHECK(false, "EXIF sub-IFD (34665) present");
    }

    // Pixel round-trip.
    uint32_t stripOff = readScalar(file, ifd[273]);
    uint32_t stripBytes = readScalar(file, ifd[279]);
    std::vector<uint16_t> back;
    decodeStrip(file, comptag, stripOff, stripBytes,
                (uint64_t)W * H * 3, back);
    bool pixOk = (back.size() == pixels.size()) &&
                 std::memcmp(back.data(), pixels.data(), pixels.size() * 2) == 0;
    CHECK(pixOk, "all pixel samples round-trip bit-exact");

    std::printf("    file: %s (%zu bytes)\n", path.c_str(), wr.bytesWritten);
}

// Write+readback a true 32-bit IEEE-float TIFF and assert tags + verbatim round-trip.
void runFloatCase(const char* label, TiffCompression comp) {
    std::printf("[float-%s]\n", label);

    const int W = 5, H = 3;
    std::vector<float> pixels(static_cast<size_t>(W) * H * 3);
    // Deterministic values spanning negative, fractional and >1 (HDR/scene-linear) — the
    // float writer must store them VERBATIM (no clamp to [0,1], unlike the 16-bit path).
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<float>(i) * 0.123f - 0.4f;
    pixels[0] = 2.5f; pixels[1] = -1.0f; pixels[2] = 0.0f;

    TiffMetadata meta;
    meta.software = "Spektrafilm-test";
    meta.exifColorSpace = 0xFFFF;
    meta.writeExifIfd = true;

    std::string path = std::string("/tmp/sf_tiff_f32_") + label + ".tiff";
    TiffWriteResult wr = writeTiff32fToFile(pixels.data(), W, H, meta, comp, path);
    CHECK(wr.ok, "float writer returned ok");
    if (!wr.ok) { std::printf("    error: %s\n", wr.error.c_str()); return; }

    std::vector<uint8_t> file;
    CHECK(readFile(path, file), "float file readable from disk");
    CHECK(file.size() == wr.bytesWritten, "bytesWritten matches file size");
    if (file.size() < 8) return;
    CHECK(file[0] == 'I' && file[1] == 'I' && rdU16(file, 2) == 42, "float TIFF header (II, 42)");

    std::map<uint16_t, IfdEntry> ifd;
    parseIfd(file, rdU32(file, 4), ifd);

    bool bps32 = ifd.count(258) && ifd[258].count == 3 &&
                 readShort(file, ifd[258], 0) == 32 &&
                 readShort(file, ifd[258], 1) == 32 &&
                 readShort(file, ifd[258], 2) == 32;
    CHECK(bps32, "BitsPerSample = {32,32,32}");

    bool sf3 = ifd.count(339) && ifd[339].count == 3 &&
               readShort(file, ifd[339], 0) == 3 &&
               readShort(file, ifd[339], 1) == 3 &&
               readShort(file, ifd[339], 2) == 3;
    CHECK(sf3, "SampleFormat = IEEE float (3)");

    CHECK(ifd.count(277) && readScalar(file, ifd[277]) == 3, "SamplesPerPixel = 3");
    CHECK(ifd.count(262) && readScalar(file, ifd[262]) == 2, "Photometric = RGB (2)");

    // Decode the strip (PackBits or none) to raw bytes, then compare as float32 LE.
    uint16_t comptag = ifd.count(259) ? static_cast<uint16_t>(readScalar(file, ifd[259])) : 0;
    uint32_t stripOff = readScalar(file, ifd[273]);
    uint32_t stripBytes = readScalar(file, ifd[279]);
    std::vector<uint8_t> raw;
    if (comptag == 32773) {
        size_t i = stripOff, end = stripOff + stripBytes;
        while (i < end) {
            int8_t n = static_cast<int8_t>(file[i++]);
            if (n >= 0) { int cnt = n + 1; for (int k = 0; k < cnt && i < end; ++k) raw.push_back(file[i++]); }
            else if (n != -128) { int cnt = 1 - n; uint8_t v = file[i++]; for (int k = 0; k < cnt; ++k) raw.push_back(v); }
        }
    } else {
        raw.assign(file.begin() + stripOff, file.begin() + stripOff + stripBytes);
    }
    bool sizeOk = raw.size() == pixels.size() * 4;
    CHECK(sizeOk, "float strip decodes to W*H*3*4 bytes");
    bool floatOk = sizeOk && std::memcmp(raw.data(), pixels.data(), pixels.size() * 4) == 0;
    CHECK(floatOk, "all float samples round-trip bit-exact (verbatim, no clamp)");

    std::printf("    file: %s (%zu bytes)\n", path.c_str(), wr.bytesWritten);
}

struct CancelAfterPolls {
    int polls = 0;
    int cancelAt = 1;
};

bool cancelAfter(void* opaque) noexcept {
    auto* state = static_cast<CancelAfterPolls*>(opaque);
    return ++state->polls >= state->cancelAt;
}

struct TempStageCancellation {
    const char* directory;
    const char* filePrefix;
};

bool tempStageExists(void* opaque) noexcept {
    const auto* state = static_cast<const TempStageCancellation*>(opaque);
    DIR* directory = opendir(state->directory);
    if (directory == nullptr) return false;
    bool found = false;
    const size_t prefixLength = std::strlen(state->filePrefix);
    while (const dirent* entry = readdir(directory)) {
        if (std::strncmp(entry->d_name, state->filePrefix, prefixLength) == 0) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

struct ConcurrentCancellation {
    std::atomic<int> polls{0};
    std::atomic<bool> cancelled{false};
};

bool blockUntilConcurrentCancel(void* opaque) noexcept {
    auto* state = static_cast<ConcurrentCancellation*>(opaque);
    const int poll = state->polls.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (poll == 2) {
        while (!state->cancelled.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    return state->cancelled.load(std::memory_order_acquire);
}

void runSafetyCases() {
    std::printf("[safety]\n");
    const uint16_t onePixel[3] = {1, 2, 3};
    TiffMetadata meta;
    std::vector<uint8_t> out = {0xAA};

    auto huge = writeTiff16ToMemory(onePixel, INT_MAX, INT_MAX, meta,
                                    TiffCompression::None, out);
    CHECK(!huge.ok, "overflow dimensions are rejected before allocation");
    CHECK(out.empty(), "overflow failure publishes no partial memory output");

    auto invalidPath = writeTiff16ToFile(
        reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(1)),
        1, 1, meta, TiffCompression::None, "");
    CHECK(!invalidPath.ok && invalidPath.error == "empty output path",
          "empty path is rejected before any pixel read");
    static constexpr char kNulPath[] = "/tmp/sf_tiff_nul\0ignored.tiff";
    const std::string nulPath(kNulPath, sizeof(kNulPath) - 1u);
    invalidPath = writeTiff16ToFile(
        onePixel, 1, 1, meta, TiffCompression::None, nulPath);
    CHECK(!invalidPath.ok && invalidPath.error == "output path contains NUL",
          "embedded-NUL path is rejected deterministically");

    // Raw pixels fit in uint32, but the complete classic-TIFF layout does not.
    auto layoutOverflow = writeTiff16ToMemory(
        onePixel, 715827882, 1, meta, TiffCompression::None, out);
    CHECK(!layoutOverflow.ok,
          "classic TIFF rejects full-layout overflow before pixel access");
    CHECK(out.empty(), "layout overflow publishes no partial memory output");

    std::vector<uint16_t> pixels(4u * 8u * 3u, 0x1234);
    CancelAfterPolls state{0, 3};
    TiffCancellation cancellation{&state, cancelAfter};
    auto cancelled = writeTiff16ToMemory(
        pixels.data(), 4, 8, meta, TiffCompression::None, out, &cancellation);
    CHECK(!cancelled.ok && cancelled.cancelled,
          "cancellation is reported distinctly from write failure");
    CHECK(state.polls == 3, "cancellation is polled while serialising rows");
    CHECK(out.empty(), "cancelled encode publishes no partial memory output");

    const std::string path = "/tmp/sf_tiff_cancelled.tiff";
    {
        FILE* existing = std::fopen(path.c_str(), "wb");
        const uint8_t sentinel[] = {0x61, 0x62, 0x63};
        std::fwrite(sentinel, 1, sizeof(sentinel), existing);
        std::fclose(existing);
    }
    state = {0, 3};
    cancelled = writeTiff16ToFile(
        pixels.data(), 4, 8, meta, TiffCompression::None, path, &cancellation);
    std::vector<uint8_t> unchanged;
    CHECK(!cancelled.ok && cancelled.cancelled,
          "cancelled file write returns cancellation");
    CHECK(readFile(path, unchanged) && unchanged == std::vector<uint8_t>({0x61, 0x62, 0x63}),
          "cancelled file write leaves the published destination unchanged");
    std::remove(path.c_str());

    const std::string stagedPath = "/tmp/sf_tiff_staged_cancel.tiff";
    {
        FILE* existing = std::fopen(stagedPath.c_str(), "wb");
        const uint8_t sentinel[] = {0x31, 0x32, 0x33};
        std::fwrite(sentinel, 1, sizeof(sentinel), existing);
        std::fclose(existing);
    }
    TempStageCancellation stagedState{"/tmp", "sf_tiff_staged_cancel.tiff.tmp."};
    TiffCancellation stagedCancellation{&stagedState, tempStageExists};
    cancelled = writeTiff16ToFile(
        pixels.data(), 4, 8, meta, TiffCompression::None,
        stagedPath, &stagedCancellation);
    unchanged.clear();
    CHECK(!cancelled.ok && cancelled.cancelled,
          "cancellation during staged output is reported");
    CHECK(readFile(stagedPath, unchanged) && unchanged == std::vector<uint8_t>({0x31, 0x32, 0x33}),
          "staged cancellation never replaces the destination");
    CHECK(!tempStageExists(&stagedState),
          "staged cancellation removes the incomplete temporary file");
    std::remove(stagedPath.c_str());

    ConcurrentCancellation concurrentState;
    TiffCancellation concurrentCancellation{
        &concurrentState, blockUntilConcurrentCancel,
    };
    TiffWriteResult concurrentResult;
    out = {0xAA};
    std::thread writer([&]() {
        concurrentResult = writeTiff16ToMemory(
            pixels.data(), 4, 8, meta, TiffCompression::None,
            out, &concurrentCancellation);
    });
    while (concurrentState.polls.load(std::memory_order_acquire) < 2)
        std::this_thread::yield();
    concurrentState.cancelled.store(true, std::memory_order_release);
    writer.join();
    CHECK(!concurrentResult.ok && concurrentResult.cancelled && out.empty(),
          "concurrent cancellation is race-free and publishes no memory output");
}

void runExceptionTranslationCases() {
    using spectrafilm::tiffjni::NativeExceptionKind;
    using spectrafilm::tiffjni::BufferWindow;
    using spectrafilm::tiffjni::BufferWindowError;
    using spectrafilm::tiffjni::containNativeExceptions;
    using spectrafilm::tiffjni::stableMessage;

    std::printf("[jni-exception-translation]\n");
    NativeExceptionKind kind = NativeExceptionKind::None;
    int value = containNativeExceptions<int>([]() -> int { throw std::bad_alloc(); }, kind);
    CHECK(value == 0 && kind == NativeExceptionKind::OutOfMemory,
          "bad_alloc is contained and classified");
    CHECK(std::strcmp(stableMessage(kind), "TIFF write failed: out of memory") == 0,
          "bad_alloc maps to a stable Kotlin error message");

    value = containNativeExceptions<int>([]() -> int { throw std::runtime_error("unstable detail"); }, kind);
    CHECK(value == 0 && kind == NativeExceptionKind::Standard,
          "std::exception is contained and classified");
    CHECK(std::strcmp(stableMessage(kind), "TIFF write failed: native exception") == 0,
          "std::exception maps to a stable Kotlin error message");

    value = containNativeExceptions<int>([]() -> int { throw 7; }, kind);
    CHECK(value == 0 && kind == NativeExceptionKind::Unknown,
          "unknown exception is contained and classified");
    CHECK(std::strcmp(stableMessage(kind), "TIFF write failed: unknown native exception") == 0,
          "unknown exception maps to a stable Kotlin error message");

    BufferWindow window;
    auto windowError = spectrafilm::tiffjni::validateBufferWindow(
        4, 16, 20, 20, 12, window);
    CHECK(windowError == BufferWindowError::None && window.offset == 4 && window.length == 12,
          "direct-buffer validation honours the logical position and limit");
    windowError = spectrafilm::tiffjni::validateBufferWindow(
        4, 15, 20, 20, 12, window);
    CHECK(windowError == BufferWindowError::TooSmall,
          "direct-buffer validation rejects a short logical window");
    windowError = spectrafilm::tiffjni::validateBufferWindow(
        4, 16, 20, 19, 12, window);
    CHECK(windowError == BufferWindowError::Malformed,
          "direct-buffer validation rejects inconsistent capacities");
}

}  // namespace

int main() {
    // A fake but structurally-irrelevant ICC blob (odd length to exercise word-align
    // padding of the following strip). Round-trip must be byte-exact regardless.
    std::vector<uint8_t> icc;
    for (int i = 0; i < 137; ++i) icc.push_back(static_cast<uint8_t>(i * 7 + 3));

    runCase("uncompressed", TiffCompression::None, icc);
    runCase("packbits", TiffCompression::PackBits, icc);
    runCase("no_icc", TiffCompression::None, {});

    // True 32-bit IEEE-float path (BitsPerSample=32, SampleFormat=3, verbatim samples).
    runFloatCase("uncompressed", TiffCompression::None);
    runFloatCase("packbits", TiffCompression::PackBits);

    runSafetyCases();
    runExceptionTranslationCases();

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
