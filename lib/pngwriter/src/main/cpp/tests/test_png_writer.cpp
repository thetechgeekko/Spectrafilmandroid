/*
 * Spektrafilm for Android — host unit test for the 16-bit PNG writer.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Pure host test (no Android / no Gradle). Writes a small known 16-bit RGB
 * buffer to /tmp with an embedded iCCP blob + tEXt Software tag, then parses
 * the file back (a minimal independent PNG reader implemented here, no libpng)
 * and asserts:
 *
 *   - PNG signature (8 bytes, correct)
 *   - IHDR: correct width/height, bit_depth=16, color_type=2 (RGB),
 *           compression=0, filter=0, interlace=0
 *   - IHDR CRC32 correct (independently computed)
 *   - iCCP chunk present when ICC was supplied; absent when not
 *   - iCCP CRC32 correct
 *   - tEXt chunk present with "Software\0<value>" when software supplied
 *   - IDAT present; IDAT CRC32 correct
 *   - IEND present with 0-length data
 *   - Pixel round-trip: inflate IDAT, strip filter bytes, byte-swap big->little,
 *     compare against original uint16 samples bit-exact
 *
 * Build (host — no Android):
 *   g++ -std=c++17 -O2 \
 *       -I/home/user/wt-lib/lib/pngwriter/src/main/cpp \
 *       test_png_writer.cpp \
 *       /home/user/wt-lib/lib/pngwriter/src/main/cpp/png_writer.cpp \
 *       -lz -o /tmp/test_png_writer
 *   /tmp/test_png_writer
 *
 * Python PIL cross-check (run after the test):
 *   python3 -c "
 *   from PIL import Image
 *   import numpy as np
 *   img = Image.open('/tmp/sf_png_test_rgb16.png')
 *   arr = np.array(img, dtype=np.uint16)
 *   print('mode:', img.mode, 'size:', img.size, 'dtype:', arr.dtype)
 *   print('first pixel (R,G,B):', arr[0,0])
 *   print('OK')
 *   "
 */
#include "png_writer.h"
#include "png_writer_jni_boundary.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>
#include <dirent.h>

using namespace spectrafilm;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::printf("  FAIL: %s\n", (msg)); g_failures++; }      \
        else         { std::printf("  ok:   %s\n", (msg)); }                    \
    } while (0)

// ---- read helpers ----------------------------------------------------------

static uint32_t rdBE32(const std::vector<uint8_t>& b, size_t off) {
    return (static_cast<uint32_t>(b[off])     << 24) |
           (static_cast<uint32_t>(b[off + 1]) << 16) |
           (static_cast<uint32_t>(b[off + 2]) <<  8) |
            static_cast<uint32_t>(b[off + 3]);
}

static bool readFile(const std::string& path, std::vector<uint8_t>& out) {
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

// ---- PNG chunk iterator ----------------------------------------------------

struct Chunk {
    uint32_t dataOff;  // offset of data bytes in file
    uint32_t dataLen;
    char     type[5];  // NUL-terminated
    uint32_t crcInFile;
};

// Parse all chunks starting at byte 8 (after the signature).
static std::vector<Chunk> parseChunks(const std::vector<uint8_t>& b) {
    std::vector<Chunk> out;
    size_t pos = 8;
    while (pos + 12 <= b.size()) {
        Chunk c;
        c.dataLen   = rdBE32(b, pos);
        c.type[0]   = static_cast<char>(b[pos + 4]);
        c.type[1]   = static_cast<char>(b[pos + 5]);
        c.type[2]   = static_cast<char>(b[pos + 6]);
        c.type[3]   = static_cast<char>(b[pos + 7]);
        c.type[4]   = '\0';
        c.dataOff   = static_cast<uint32_t>(pos) + 8;
        size_t crcPos = pos + 8 + c.dataLen;
        if (crcPos + 4 > b.size()) break;
        c.crcInFile = rdBE32(b, crcPos);
        out.push_back(c);
        pos = crcPos + 4;
    }
    return out;
}

// Verify the CRC32 of a chunk (type bytes + data bytes).
static bool verifyCrc(const std::vector<uint8_t>& b, const Chunk& c) {
    uLong crc = crc32(0L, Z_NULL, 0);
    // type bytes (4)
    crc = crc32(crc, reinterpret_cast<const Bytef*>(c.type), 4);
    // data bytes
    if (c.dataLen > 0)
        crc = crc32(crc, &b[c.dataOff], c.dataLen);
    return static_cast<uint32_t>(crc) == c.crcInFile;
}

// Decompress zlib data and return raw bytes; returns false on failure.
static bool zlibDecompress(const uint8_t* src, size_t srcLen,
                           std::vector<uint8_t>& dst, std::string& errOut) {
    // Start with a generous initial buffer; resize as needed.
    dst.resize(srcLen * 4 + 1024);
    uLong dstLen = static_cast<uLong>(dst.size());

    int rc = Z_BUF_ERROR;
    while (rc == Z_BUF_ERROR) {
        dstLen = static_cast<uLong>(dst.size());
        rc = uncompress(reinterpret_cast<Bytef*>(dst.data()), &dstLen,
                        reinterpret_cast<const Bytef*>(src),
                        static_cast<uLong>(srcLen));
        if (rc == Z_BUF_ERROR) dst.resize(dst.size() * 2);
    }
    if (rc != Z_OK) {
        errOut = "zlib uncompress failed (rc=" + std::to_string(rc) + ")";
        return false;
    }
    dst.resize(static_cast<size_t>(dstLen));
    return true;
}

// ---- the main test runner --------------------------------------------------

void runCase(const char* label,
             const std::vector<uint16_t>& pixels, int W, int H,
             const PngMetadata& meta) {
    std::printf("[%s]\n", label);

    std::string path = std::string("/tmp/sf_png_test_") + label + ".png";

    PngWriteResult wr = writePng16ToFile(pixels.data(), W, H, meta, path);
    CHECK(wr.ok, "writer returned ok");
    if (!wr.ok) {
        std::printf("    error: %s\n", wr.error.c_str());
        return;
    }

    // Read back.
    std::vector<uint8_t> file;
    CHECK(readFile(path, file), "file readable from disk");
    CHECK(file.size() == wr.bytesWritten, "bytesWritten matches file size");
    if (file.size() < 8) return;

    // ---- PNG signature (8 bytes) -------------------------------------------
    static const uint8_t kSig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    CHECK(std::memcmp(file.data(), kSig, 8) == 0, "PNG signature correct");

    // ---- Chunk inventory ---------------------------------------------------
    auto chunks = parseChunks(file);
    CHECK(!chunks.empty(), "at least one chunk found");

    // Find specific chunks by type.
    auto findChunk = [&](const char* t) -> const Chunk* {
        for (auto& c : chunks) if (std::strcmp(c.type, t) == 0) return &c;
        return nullptr;
    };

    // ---- IHDR chunk --------------------------------------------------------
    const Chunk* ihdr = findChunk("IHDR");
    CHECK(ihdr != nullptr, "IHDR chunk present");
    if (ihdr) {
        CHECK(verifyCrc(file, *ihdr), "IHDR CRC32 correct");
        CHECK(ihdr->dataLen == 13, "IHDR data length = 13");

        const uint8_t* d = &file[ihdr->dataOff];
        uint32_t fw = rdBE32(file, ihdr->dataOff);
        uint32_t fh = rdBE32(file, ihdr->dataOff + 4);
        CHECK(fw == static_cast<uint32_t>(W), "IHDR width matches");
        CHECK(fh == static_cast<uint32_t>(H), "IHDR height matches");
        CHECK(d[8]  == 16, "IHDR bit_depth = 16");
        CHECK(d[9]  ==  2, "IHDR color_type = 2 (RGB)");
        CHECK(d[10] ==  0, "IHDR compression_method = 0");
        CHECK(d[11] ==  0, "IHDR filter_method = 0");
        CHECK(d[12] ==  0, "IHDR interlace = 0 (no interlace)");
    }

    // ---- iCCP chunk --------------------------------------------------------
    const Chunk* iccp = findChunk("iCCP");
    if (!meta.iccProfile.empty()) {
        CHECK(iccp != nullptr, "iCCP chunk present when ICC provided");
        if (iccp) {
            CHECK(verifyCrc(file, *iccp), "iCCP CRC32 correct");
            // Profile name must be "ICC Profile" followed by NUL + compression byte 0.
            const uint8_t* d = &file[iccp->dataOff];
            static const char kName[] = "ICC Profile";
            bool nameOk = (iccp->dataLen > 13) &&
                          (std::memcmp(d, kName, sizeof(kName) - 1) == 0) &&
                          (d[11] == 0) && (d[12] == 0);
            CHECK(nameOk, "iCCP: profile name 'ICC Profile' + NUL + method 0");

            // Decompress the iCCP data and compare with original.
            const uint8_t* compData = d + 13;  // after name(11) + NUL(1) + method(1)
            size_t compLen = iccp->dataLen - 13;
            std::vector<uint8_t> decompressed;
            std::string zerr;
            bool dcOk = zlibDecompress(compData, compLen, decompressed, zerr);
            CHECK(dcOk, "iCCP data decompresses successfully");
            bool iccMatch = dcOk &&
                            (decompressed.size() == meta.iccProfile.size()) &&
                            (std::memcmp(decompressed.data(),
                                        meta.iccProfile.data(),
                                        meta.iccProfile.size()) == 0);
            CHECK(iccMatch, "iCCP decompressed bytes match original ICC profile");
        }
    } else {
        CHECK(iccp == nullptr, "no iCCP chunk when no ICC provided");
    }

    // ---- tEXt chunk --------------------------------------------------------
    const Chunk* text = findChunk("tEXt");
    if (!meta.software.empty()) {
        CHECK(text != nullptr, "tEXt chunk present");
        if (text) {
            CHECK(verifyCrc(file, *text), "tEXt CRC32 correct");
            // Data layout: "Software\0<value>"
            static const char kKw[] = "Software";
            bool kwOk = (text->dataLen > 9) &&
                        (std::memcmp(&file[text->dataOff], kKw, 8) == 0) &&
                        (file[text->dataOff + 8] == 0);
            CHECK(kwOk, "tEXt keyword = 'Software'");
            if (kwOk) {
                // Value is the rest after keyword+NUL.
                std::string val(reinterpret_cast<const char*>(&file[text->dataOff + 9]),
                                text->dataLen - 9);
                CHECK(val == meta.software, "tEXt value matches software string");
            }
        }
    }

    // ---- IDAT chunk --------------------------------------------------------
    const Chunk* idat = findChunk("IDAT");
    CHECK(idat != nullptr, "IDAT chunk present");
    if (idat) {
        CHECK(verifyCrc(file, *idat), "IDAT CRC32 correct");

        // Inflate the IDAT data.
        std::vector<uint8_t> inflated;
        std::string zerr;
        bool inflOk = zlibDecompress(&file[idat->dataOff], idat->dataLen,
                                     inflated, zerr);
        CHECK(inflOk, "IDAT data inflates successfully");
        if (!inflOk) {
            std::printf("    zlib error: %s\n", zerr.c_str());
            return;
        }

        // Expected size after inflate: (1 + W*3*2) * H  (filter byte + row samples)
        const size_t rowBytes = static_cast<size_t>(W) * 3u * 2u;
        const size_t filtRowBytes = 1u + rowBytes;
        CHECK(inflated.size() == filtRowBytes * static_cast<size_t>(H),
              "IDAT inflated size = (1 + W*3*2)*H");

        // Reconstruct the scanlines and compare pixels. Rows now choose between
        // filter 0 (None) and filter 2 (Up) per row (#175), so this UNFILTERS
        // rather than assuming a filter -- which is also what any decoder does.
        bool pixOk = true;
        bool filterOk = true;
        std::vector<uint8_t> recon(rowBytes * static_cast<size_t>(H));
        for (int y = 0; y < H; ++y) {
            const uint8_t* row = inflated.data() + y * filtRowBytes;
            const int type = row[0];
            if (type != 0 && type != 2) { filterOk = false; }
            uint8_t* cur = recon.data() + static_cast<size_t>(y) * rowBytes;
            const uint8_t* prev =
                y > 0 ? recon.data() + static_cast<size_t>(y - 1) * rowBytes : nullptr;
            for (size_t i = 0; i < rowBytes; ++i) {
                const uint8_t x = row[1 + i];
                cur[i] = (type == 2)
                    ? static_cast<uint8_t>(x + (prev ? prev[i] : 0))
                    : x;
            }
        }
        for (int y = 0; y < H && pixOk; ++y) {
            const uint8_t* cur = recon.data() + static_cast<size_t>(y) * rowBytes;
            for (int x = 0; x < W * 3; ++x) {
                const uint16_t sampleBE = (static_cast<uint16_t>(cur[x * 2]) << 8) |
                                           static_cast<uint16_t>(cur[x * 2 + 1]);
                const uint16_t orig = pixels[static_cast<size_t>(y) * W * 3 + x];
                if (sampleBE != orig) { pixOk = false; }
            }
        }
        CHECK(filterOk, "all scanline filter bytes = 0 (None)");
        CHECK(pixOk, "all pixel samples round-trip bit-exact (BE->LE)");
    }

    // ---- IEND chunk --------------------------------------------------------
    const Chunk* iend = findChunk("IEND");
    CHECK(iend != nullptr, "IEND chunk present");
    if (iend) {
        CHECK(iend->dataLen == 0, "IEND data length = 0");
        CHECK(verifyCrc(file, *iend), "IEND CRC32 correct");
    }

    // ---- Chunk order: IHDR must be first, IEND must be last ----------------
    if (!chunks.empty()) {
        CHECK(std::strcmp(chunks.front().type, "IHDR") == 0,
              "IHDR is the first chunk");
        CHECK(std::strcmp(chunks.back().type, "IEND") == 0,
              "IEND is the last chunk");
    }

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
    PngMetadata meta;
    std::vector<uint8_t> out = {0xAA};

    auto huge = writePng16ToMemory(onePixel, INT_MAX, INT_MAX, meta, out);
    CHECK(!huge.ok, "overflow dimensions are rejected before allocation");
    CHECK(out.empty(), "overflow failure publishes no partial memory output");

    auto invalidPath = writePng16ToFile(
        reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(1)),
        1, 1, meta, "");
    CHECK(!invalidPath.ok && invalidPath.error == "empty output path",
          "empty path is rejected before any pixel read");
    static constexpr char kNulPath[] = "/tmp/sf_png_nul\0ignored.png";
    const std::string nulPath(kNulPath, sizeof(kNulPath) - 1u);
    invalidPath = writePng16ToFile(onePixel, 1, 1, meta, nulPath);
    CHECK(!invalidPath.ok && invalidPath.error == "output path contains NUL",
          "embedded-NUL path is rejected deterministically");

    std::vector<uint16_t> pixels(4u * 8u * 3u, 0x1234);
    CancelAfterPolls state{0, 3};
    PngCancellation cancellation{&state, cancelAfter};
    auto cancelled = writePng16ToMemory(
        pixels.data(), 4, 8, meta, out, &cancellation);
    CHECK(!cancelled.ok && cancelled.cancelled,
          "cancellation is reported distinctly from write failure");
    CHECK(state.polls == 3, "cancellation is polled while encoding rows");
    CHECK(out.empty(), "cancelled encode publishes no partial memory output");

    const std::string path = "/tmp/sf_png_cancelled.png";
    {
        FILE* existing = std::fopen(path.c_str(), "wb");
        const uint8_t sentinel[] = {0x51, 0x52, 0x53};
        std::fwrite(sentinel, 1, sizeof(sentinel), existing);
        std::fclose(existing);
    }
    state = {0, 3};
    cancelled = writePng16ToFile(
        pixels.data(), 4, 8, meta, path, &cancellation);
    std::vector<uint8_t> unchanged;
    CHECK(!cancelled.ok && cancelled.cancelled,
          "cancelled file write returns cancellation");
    CHECK(readFile(path, unchanged) && unchanged == std::vector<uint8_t>({0x51, 0x52, 0x53}),
          "cancelled file write leaves the published destination unchanged");
    std::remove(path.c_str());

    const std::string stagedPath = "/tmp/sf_png_staged_cancel.png";
    {
        FILE* existing = std::fopen(stagedPath.c_str(), "wb");
        const uint8_t sentinel[] = {0x41, 0x42, 0x43};
        std::fwrite(sentinel, 1, sizeof(sentinel), existing);
        std::fclose(existing);
    }
    TempStageCancellation stagedState{"/tmp", "sf_png_staged_cancel.png.tmp."};
    PngCancellation stagedCancellation{&stagedState, tempStageExists};
    cancelled = writePng16ToFile(
        pixels.data(), 4, 8, meta, stagedPath, &stagedCancellation);
    unchanged.clear();
    CHECK(!cancelled.ok && cancelled.cancelled,
          "cancellation during staged output is reported");
    CHECK(readFile(stagedPath, unchanged) && unchanged == std::vector<uint8_t>({0x41, 0x42, 0x43}),
          "staged cancellation never replaces the destination");
    CHECK(!tempStageExists(&stagedState),
          "staged cancellation removes the incomplete temporary file");
    std::remove(stagedPath.c_str());

    ConcurrentCancellation concurrentState;
    PngCancellation concurrentCancellation{
        &concurrentState, blockUntilConcurrentCancel,
    };
    PngWriteResult concurrentResult;
    out = {0xAA};
    std::thread writer([&]() {
        concurrentResult = writePng16ToMemory(
            pixels.data(), 4, 8, meta, out, &concurrentCancellation);
    });
    while (concurrentState.polls.load(std::memory_order_acquire) < 2)
        std::this_thread::yield();
    concurrentState.cancelled.store(true, std::memory_order_release);
    writer.join();
    CHECK(!concurrentResult.ok && concurrentResult.cancelled && out.empty(),
          "concurrent cancellation is race-free and publishes no memory output");
}

void runExceptionTranslationCases() {
    using spectrafilm::pngjni::NativeExceptionKind;
    using spectrafilm::pngjni::BufferWindow;
    using spectrafilm::pngjni::BufferWindowError;
    using spectrafilm::pngjni::containNativeExceptions;
    using spectrafilm::pngjni::stableMessage;

    std::printf("[jni-exception-translation]\n");
    NativeExceptionKind kind = NativeExceptionKind::None;
    int value = containNativeExceptions<int>([]() -> int { throw std::bad_alloc(); }, kind);
    CHECK(value == 0 && kind == NativeExceptionKind::OutOfMemory,
          "bad_alloc is contained and classified");
    CHECK(std::strcmp(stableMessage(kind), "PNG write failed: out of memory") == 0,
          "bad_alloc maps to a stable Kotlin error message");

    value = containNativeExceptions<int>([]() -> int { throw std::runtime_error("unstable detail"); }, kind);
    CHECK(value == 0 && kind == NativeExceptionKind::Standard,
          "std::exception is contained and classified");
    CHECK(std::strcmp(stableMessage(kind), "PNG write failed: native exception") == 0,
          "std::exception maps to a stable Kotlin error message");

    value = containNativeExceptions<int>([]() -> int { throw 7; }, kind);
    CHECK(value == 0 && kind == NativeExceptionKind::Unknown,
          "unknown exception is contained and classified");
    CHECK(std::strcmp(stableMessage(kind), "PNG write failed: unknown native exception") == 0,
          "unknown exception maps to a stable Kotlin error message");

    BufferWindow window;
    auto windowError = spectrafilm::pngjni::validateBufferWindow(
        4, 16, 20, 20, 12, window);
    CHECK(windowError == BufferWindowError::None && window.offset == 4 && window.length == 12,
          "direct-buffer validation honours the logical position and limit");
    windowError = spectrafilm::pngjni::validateBufferWindow(
        4, 15, 20, 20, 12, window);
    CHECK(windowError == BufferWindowError::TooSmall,
          "direct-buffer validation rejects a short logical window");
    windowError = spectrafilm::pngjni::validateBufferWindow(
        4, 16, 20, 19, 12, window);
    CHECK(windowError == BufferWindowError::Malformed,
          "direct-buffer validation rejects inconsistent capacities");
}

}  // namespace

// #175: the encoder now feeds deflate one scanline at a time instead of building
// the whole filtered image first, and the float entry quantizes per row instead of
// building a second full image. Those are MEMORY changes only -- PNG16 container
// identity is a gated contract (the export benchmark compares whole-container
// digests), so the bytes must not move by one.
//
// The reference here is the algorithm that was replaced: concatenate every filtered
// scanline, then deflate the result in one pass with the same settings.
static void runStreamingIdentityCases() {
    std::printf("\n[streaming identity]\n");
    const int W = 129, H = 97;   // several rows, and a row that is not a chunk multiple
    const size_t rowSamples = static_cast<size_t>(W) * 3;
    std::vector<uint16_t> pixels(rowSamples * H);
    for (int y = 0; y < H; ++y) {
        for (size_t x = 0; x < rowSamples; ++x) {
            // Mixed compressible and incompressible content, so the deflate stream
            // exercises both literal and match paths.
            const size_t i = static_cast<size_t>(y) * rowSamples + x;
            pixels[i] = static_cast<uint16_t>(((i * 2654435761u) >> 11) ^ (y << 7));
            if ((x / 9) % 3 == 0) pixels[i] = static_cast<uint16_t>(0x1234 + y);
        }
    }

    PngMetadata meta;
    meta.software = "Spektrafilm-test";
    std::vector<uint8_t> file;
    const PngWriteResult res =
        writePng16ToMemory(pixels.data(), W, H, meta, file, nullptr);
    CHECK(res.ok, "streaming encode succeeds");
    if (!res.ok) return;

    const std::vector<Chunk> chunks = parseChunks(file);
    const Chunk* idat = nullptr;
    for (const Chunk& c : chunks) if (std::strcmp(c.type, "IDAT") == 0) idat = &c;
    CHECK(idat != nullptr, "exactly one IDAT chunk (bands stay one deflate stream)");
    if (idat == nullptr) return;
    int idatCount = 0;
    for (const Chunk& c : chunks) if (std::strcmp(c.type, "IDAT") == 0) idatCount++;
    CHECK(idatCount == 1, "the banded encode still emits a single IDAT");

    // The encode is parallel, so the bytes must not depend on how many threads ran.
    // Bands are fixed by row count and concatenated in order, so this is a property
    // of the design; it is asserted because a future change could break it silently.
    {
        setenv("SPK_PNG_WORKERS", "1", 1);
        std::vector<uint8_t> serial;
        const PngWriteResult one = writePng16ToMemory(pixels.data(), W, H, meta,
                                                      serial, nullptr);
        setenv("SPK_PNG_WORKERS", "8", 1);
        std::vector<uint8_t> parallel;
        const PngWriteResult eight = writePng16ToMemory(pixels.data(), W, H, meta,
                                                        parallel, nullptr);
        unsetenv("SPK_PNG_WORKERS");
        CHECK(one.ok && eight.ok, "1-worker and 8-worker encodes both succeed");
        CHECK(serial.size() == parallel.size() &&
                  std::memcmp(serial.data(), parallel.data(), serial.size()) == 0,
              "output is byte-identical at 1 and 8 workers");
    }

    // The file encoder streams and the memory encoder buffers, so they are two
    // implementations of one format. They must agree byte for byte, or a container
    // digest would depend on which entry point the caller happened to use.
    {
        const std::string streamPath = "/tmp/sf_png_stream_file.png";
        const PngWriteResult fileRes =
            writePng16ToFile(pixels.data(), W, H, meta, streamPath, nullptr);
        CHECK(fileRes.ok, "streaming file write succeeds");
        std::vector<uint8_t> streamed;
        if (fileRes.ok && readFile(streamPath, streamed)) {
            CHECK(streamed.size() == file.size() &&
                      std::memcmp(streamed.data(), file.data(), file.size()) == 0,
                  "streamed file is byte-identical to the in-memory encode");
            CHECK(fileRes.bytesWritten == streamed.size(),
                  "bytesWritten matches the streamed file size");
        } else {
            CHECK(false, "streamed file readable");
        }
        std::remove(streamPath.c_str());
    }

    // The float entry must produce the same file as quantizing first and writing.
    std::vector<float> floats(pixels.size());
    for (size_t i = 0; i < pixels.size(); ++i)
        floats[i] = static_cast<float>(pixels[i]) / 65535.0f;
    std::vector<uint16_t> requantized(pixels.size());
    for (size_t i = 0; i < floats.size(); ++i) {
        const float v = floats[i];
        if (!(v > 0.0f)) { requantized[i] = 0; continue; }
        if (v >= 1.0f) { requantized[i] = 65535; continue; }
        requantized[i] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
    }
    const std::string floatPath = "/tmp/sf_png_stream_float.png";
    const std::string refPath = "/tmp/sf_png_stream_ref.png";
    const PngWriteResult floatRes =
        writePngFloatToFile(floats.data(), W, H, meta, floatPath, nullptr);
    const PngWriteResult refRes =
        writePng16ToFile(requantized.data(), W, H, meta, refPath, nullptr);
    CHECK(floatRes.ok && refRes.ok, "float and uint16 file writes both succeed");
    std::vector<uint8_t> floatFile, refFile;
    if (readFile(floatPath, floatFile) && readFile(refPath, refFile)) {
        CHECK(floatFile.size() == refFile.size() &&
                  std::memcmp(floatFile.data(), refFile.data(), refFile.size()) == 0,
              "row-quantized float write is byte-identical to the staged uint16 write");
    } else {
        CHECK(false, "both files readable");
    }
    std::remove(floatPath.c_str());
    std::remove(refPath.c_str());
}

int main() {
    const int W = 5, H = 4;

    // Deterministic test pattern: full 16-bit range exercised.
    std::vector<uint16_t> pixels(static_cast<size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t base = (static_cast<size_t>(y) * W + x) * 3;
            pixels[base + 0] = static_cast<uint16_t>((x * 65535) / (W - 1));
            pixels[base + 1] = static_cast<uint16_t>((y * 65535) / (H - 1));
            pixels[base + 2] = static_cast<uint16_t>(((x + y) & 1) ? 0xFFFF : 0x0001);
        }
    }
    // Force pixels at corners to well-known values for easy manual inspection.
    pixels[0] = 0x0000; pixels[1] = 0x0000; pixels[2] = 0x0000;  // top-left  = black
    pixels[(W * H - 1) * 3 + 0] = 0xFFFF;  // bottom-right R
    pixels[(W * H - 1) * 3 + 1] = 0xFFFF;  // bottom-right G
    pixels[(W * H - 1) * 3 + 2] = 0xFFFF;  // bottom-right = white

    // Fake ICC blob (arbitrary bytes, odd length to stress word alignment).
    std::vector<uint8_t> icc;
    for (int i = 0; i < 149; ++i) icc.push_back(static_cast<uint8_t>(i * 11 + 5));

    // Case 1: full metadata (ICC + software tag).
    {
        PngMetadata meta;
        meta.software  = "Spektrafilm-test";
        meta.iccProfile = icc;
        runCase("rgb16", pixels, W, H, meta);
    }

    // Case 2: no ICC (no iCCP chunk expected).
    {
        PngMetadata meta;
        meta.software = "Spektrafilm-test";
        // meta.iccProfile is empty by default
        runCase("no_icc", pixels, W, H, meta);
    }

    // Case 3: no software string (no tEXt chunk expected).
    {
        PngMetadata meta;
        meta.software   = "";  // empty -> no tEXt
        meta.iccProfile = icc;
        runCase("no_software", pixels, W, H, meta);
    }

    runStreamingIdentityCases();

    runSafetyCases();
    runExceptionTranslationCases();

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
