/*
 * Spektrafilm for Android — lib:libraw native decoder (implementation).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Uses statically included, dual-offered LibRaw; distribution is governed by the
 * bundled decision record and fail-closed release audit.
 *
 * Implements the same processing-option contract as
 * spektrafilm/utils/raw_file_processor.py on-device with LibRaw:
 *   raw.postprocess(output_color=ACES, output_bps=16, no_auto_bright=True,
 *                   gamma=(1,1), use_camera_wb=<as_shot>)
 * then the colour-science white-balance path (Von-Kries adaptation + tint) for
 * the non-as-shot modes, and finally the colourspace conversion to linear
 * ProPhoto RGB (the engine's input space) — i.e. load_and_process_raw_file with
 * output_colorspace="ProPhoto RGB". The decoded result is therefore linear
 * ProPhoto RGB, NOT ACES2065-1 (ACES is only the intermediate working space).
 *
 * The LibRaw include is guarded so the first-party sniffer tests can compile the
 * diagnostic-only no-LibRaw branch. Shipping CMake fails closed unless the
 * verified dependency is present; normal builds fetch it, add its headers, and
 * compile the real SFRAW_HAVE_LIBRAW decode path.
 */
#include "raw_decoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#if !defined(SFRAW_FORCE_NO_LIBRAW) && defined(__has_include)
#  if __has_include(<libraw/libraw.h>)
#    include <libraw/libraw.h>
#    define SFRAW_HAVE_LIBRAW 1
#  endif
#endif

#ifndef SFRAW_HAVE_LIBRAW
#  define SFRAW_HAVE_LIBRAW 0
#endif

#if SFRAW_HAVE_LIBRAW
#  include <unistd.h>
#endif

#if defined(SFRAW_TESTING)
namespace sfraw::test {
namespace {
using LibRawProgressObserver = void (*)(void*);
std::atomic<LibRawProgressObserver> g_libraw_progress_observer{nullptr};
std::atomic<void*> g_libraw_progress_context{nullptr};
using LibRawOpenAttemptObserver = void (*)(void*);
std::atomic<LibRawOpenAttemptObserver> g_libraw_open_attempt_observer{nullptr};
std::atomic<void*> g_libraw_open_attempt_context{nullptr};
}  // namespace

void setLibRawProgressObserverForTest(LibRawProgressObserver observer,
                                      void* context) noexcept {
    g_libraw_progress_context.store(context, std::memory_order_relaxed);
    g_libraw_progress_observer.store(observer, std::memory_order_release);
}

void notifyLibRawProgressForTest() noexcept {
    const auto observer =
        g_libraw_progress_observer.load(std::memory_order_acquire);
    if (observer != nullptr) {
        observer(g_libraw_progress_context.load(std::memory_order_relaxed));
    }
}

void setLibRawOpenAttemptObserverForTest(
        LibRawOpenAttemptObserver observer, void* context) noexcept {
    g_libraw_open_attempt_context.store(context, std::memory_order_relaxed);
    g_libraw_open_attempt_observer.store(observer, std::memory_order_release);
}

void notifyLibRawOpenAttemptForTest() noexcept {
    const auto observer =
        g_libraw_open_attempt_observer.load(std::memory_order_acquire);
    if (observer != nullptr) {
        observer(g_libraw_open_attempt_context.load(std::memory_order_relaxed));
    }
}
}  // namespace sfraw::test
#endif

#if SFRAW_HAVE_LIBRAW
static_assert(LIBRAW_VERSION == LIBRAW_MAKE_VERSION(0, 22, 2),
              "sfraw must compile against the audited LibRaw 0.22.2 source");
#endif

namespace spectrafilm {

// ---------------------------------------------------------------------------
// Minimal in-memory TIFF/DNG sniffer.
// ---------------------------------------------------------------------------
// We sniff the *compression scheme* of the primary (full-resolution, non-
// reduced) raw image. Two uses:
//   1. To classify an unpack() failure precisely so the app can fall back to
//      the platform ImageDecoder for the compressions LibRaw cannot decode
//      without external image libraries (lossy baseline JPEG -> needs libjpeg;
//      JPEG-XL -> needs libjxl/dngsdk). These are reported distinctly from a
//      generic corrupt-file error.
//   2. For diagnostics: naming the compression in error messages.
//
// CRITICAL: lossless-JPEG/LJ92 (Compression 7), uncompressed (1), and the
// LibRaw can parse SampleFormat=3 floating-point deflate, but the production
// wrapper rejects it before the quantizing dcraw memory-bitmap stage.
// Integer/linear deflate is a typed fallback in pinned LibRaw 0.22.2. LibRaw's
// internal lossless_jpeg_load_raw handles tag 7 with no libjpeg, so tag 7 must
// never be classified as an unsupported-codec fallback.
//
// Many mobile/Pixel DNGs put a large JPEG *preview* in IFD0 and the real raw
// plane in a SubIFD. The bounded walk below retains metadata per IFD; precision
// publication requires one RAW-photometric candidate matching LibRaw geometry.
// Largest-plane selection survives only in the post-failure codec diagnostic,
// where it cannot authorize native precision or level provenance.
namespace dngsniff {

enum Compression {
    kUnknown = -1,
    kNone = 1,            // uncompressed -> decodes natively
    kLossyJpegOld = 6,    // old-style JPEG (lossy) -> needs libjpeg (fallback)
    kLosslessJpeg = 7,    // lossless JPEG / LJ92 -> decodes natively (internal)
    kDeflate = 8,         // float ZIP/DEFLATE parser exists with USE_ZLIB
    kDeflateAdobe = 0x80B2,
    kLossyJpeg = 0x884C,  // DNG 1.4 lossy baseline JPEG -> needs libjpeg
    kJpegXL = 0xCD42,     // DNG 1.7 JPEG-XL (52546) -> needs libjxl/dngsdk
};

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool be = false;
    bool in(size_t off, size_t len) const {
        return off <= n && len <= n - off;
    }
    uint16_t u16(size_t o) const {
        if (!in(o, 2)) return 0;
        return be ? (uint16_t)((p[o] << 8) | p[o + 1])
                  : (uint16_t)((p[o + 1] << 8) | p[o]);
    }
    uint32_t u32(size_t o) const {
        if (!in(o, 4)) return 0;
        return be ? ((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) |
                        ((uint32_t)p[o + 2] << 8) | p[o + 3]
                  : ((uint32_t)p[o + 3] << 24) | ((uint32_t)p[o + 2] << 16) |
                        ((uint32_t)p[o + 1] << 8) | p[o];
    }
};

struct PrimaryImageInfo {
    int compression = kUnknown;
    int sampleFormat = 1;
    int bitsPerSample = 0;
    int samplesPerPixel = 1;
    int photometric = -1;
    int cfaPatternRows = 0;
    int cfaPatternColumns = 0;
    std::array<std::uint8_t,
               RawPrecisionDescriptor::kMaxCfaPatternEntries> cfaPattern{};
    std::size_t cfaPatternCount = 0U;
    std::array<std::uint8_t, 4U> cfaPlaneColors{{0U, 1U, 2U, 0U}};
    std::size_t cfaPlaneColorCount = 3U;
    int cfaLayout = 1;
    int blackRepeatRows = 1;
    int blackRepeatColumns = 1;
    std::size_t blackLevelCount = 0U;
    std::size_t whiteLevelCount = 0U;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t selectedIfdOffset = 0U;
    bool dependencyTiffCandidate = false;
    bool recognizedTiff = false;
    bool isDng = false;
    bool bigEndian = false;
    bool metadataAmbiguous = false;
    bool unsafeForLibRawOpen = false;
    bool dependencyUnsafeForLibRawOpen = false;
    bool hasBlackLevel = false;
    bool hasBlackLevelDelta = false;
    bool hasWhiteLevel = false;
    bool hasBaselineExposure = false;
    bool hasLinearResponseLimit = false;
};

struct IfdCandidate {
    int compression = kUnknown;
    int sampleFormat = 1;
    int bitsPerSample = 0;
    int samplesPerPixel = 1;
    int photometric = -1;
    int cfaPatternRows = 0;
    int cfaPatternColumns = 0;
    std::array<std::uint8_t,
               RawPrecisionDescriptor::kMaxCfaPatternEntries> cfaPattern{};
    std::size_t cfaPatternCount = 0U;
    std::array<std::uint8_t, 4U> cfaPlaneColors{{0U, 1U, 2U, 0U}};
    std::size_t cfaPlaneColorCount = 3U;
    int cfaLayout = 1;
    int blackRepeatRows = 1;
    int blackRepeatColumns = 1;
    std::size_t blackLevelCount = 0U;
    std::size_t whiteLevelCount = 0U;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t ifdOffset = 0U;
    uint32_t newSubfileType = 0;
    bool metadataAmbiguous = false;
    bool hasBlackLevel = false;
    bool hasBlackLevelDelta = false;
    bool hasWhiteLevel = false;
    bool hasBaselineExposure = false;
    bool hasLinearResponseLimit = false;
};

struct ScanState {
    // LibRaw 0.22.2 stores at most LIBRAW_IFD_MAXCOUNT (10) TIFF IFDs across
    // root, next-IFD, and recursive SubIFD traversal. Keep a visited set at that
    // same public dependency boundary: repeated offsets are cycles/aliases, not
    // permission to silently stop scanning dependency-visible metadata.
    static constexpr std::size_t kMaxDependencyIfds = 10U;
    std::array<IfdCandidate, 32U> rawCandidates{};
    std::array<std::uint32_t, kMaxDependencyIfds> visitedIfdOffsets{};
    std::size_t visitedIfdCount = 0U;
    std::size_t rawCandidateCount = 0U;
    bool candidateOverflow = false;
    bool containerIsDng = false;
    bool containerMentionedDng = false;
    bool containerAmbiguous = false;
    bool walkIncomplete = false;
    // Dependency parser safety is broader than descriptor selection. For
    // example, LibRaw converts BlackLevel while walking reduced/non-selected
    // IFDs, so structurally unsafe numeric metadata in any walked IFD must stop
    // both public decode routes before open_buffer().
    bool unsafeIfdMetadataForLibRawOpen = false;
    // LibRaw may inspect more than the image it ultimately selects. Pre-open
    // admission must therefore bind every viable, non-reduced RAW IFD, not the
    // diagnostic largest-plane heuristic used when decoded geometry is absent.
    bool unsafeRawCandidateForLibRawOpen = false;
    // These dependency-wide facts are deliberately independent from root DNG
    // identity and selected-image qualification. LibRaw walks every reachable
    // TIFF IFD before the wrapper can bind one decoded RAW plane, including
    // reduced previews and otherwise malformed/non-RAW-looking children.
    bool dependencyHasBlackLevelDelta = false;
    bool nonRootDngVersion = false;
};

struct IfdDecode {
    IfdCandidate candidate;
    size_t bitsEntry = std::numeric_limits<size_t>::max();
    size_t sampleFormatEntry = std::numeric_limits<size_t>::max();
    size_t blackLevelEntry = std::numeric_limits<size_t>::max();
    size_t whiteLevelEntry = std::numeric_limits<size_t>::max();
    size_t colorMatrix1Entry = std::numeric_limits<size_t>::max();
    size_t colorMatrix2Entry = std::numeric_limits<size_t>::max();
    uint16_t blackLevelType = 0U;
    uint32_t blackLevelCount = 0U;
    uint16_t whiteLevelType = 0U;
    uint32_t whiteLevelCount = 0U;
    uint32_t cfaPatternCount = 0U;
    bool sawWidth = false;
    bool sawHeight = false;
    bool sawNewSubfileType = false;
    bool sawCompression = false;
    bool sawPhotometric = false;
    bool sawSamplesPerPixel = false;
    bool sawBits = false;
    bool sawSampleFormat = false;
    bool sawPlanarConfiguration = false;
    bool sawExtraSamples = false;
    bool sawCfaDimensions = false;
    bool sawCfaPattern = false;
    bool sawCfaPlaneColor = false;
    bool sawCfaLayout = false;
    bool sawBlackRepeatDimensions = false;
    bool sawBlackLevel = false;
    bool sawBlackDeltaH = false;
    bool sawBlackDeltaV = false;
    bool sawWhiteLevel = false;
    bool sawBaselineExposure = false;
    bool sawLinearResponseLimit = false;
    bool sawColorMatrix1 = false;
    bool sawColorMatrix2 = false;
    bool sawCalibrationIlluminant1 = false;
    bool sawCalibrationIlluminant2 = false;
    bool sawSubIfds = false;
    int planarConfiguration = 1;
    int calibrationIlluminant1 = 0;
    int calibrationIlluminant2 = 0;
    std::array<uint32_t, 16U> subIfds{};
    std::size_t subIfdCount = 0U;
};

inline bool markOnce(bool* seen) {
    if (*seen) return false;
    *seen = true;
    return true;
}

inline bool uniformShortArray(const Reader& r, size_t entry,
                              uint32_t expectedCount, int* value) {
    if (!r.in(entry, 12U) || r.u16(entry + 2U) != 3U ||
        expectedCount == 0U || expectedCount > 4U ||
        r.u32(entry + 4U) != expectedCount) {
        return false;
    }
    const size_t bytes = static_cast<size_t>(expectedCount) * 2U;
    const size_t valuesAt = bytes <= 4U
        ? entry + 8U
        : static_cast<size_t>(r.u32(entry + 8U));
    if (!r.in(valuesAt, bytes)) return false;
    const uint16_t first = r.u16(valuesAt);
    for (uint32_t index = 1U; index < expectedCount; ++index) {
        if (r.u16(valuesAt + static_cast<size_t>(index) * 2U) != first) {
            return false;
        }
    }
    *value = static_cast<int>(first);
    return true;
}

inline std::size_t tiffTypeWidth(uint16_t type) {
    switch (type) {
        case 1U: return 1U;  // BYTE
        case 3U: return 2U;  // SHORT
        case 4U: return 4U;  // LONG
        case 5U:             // RATIONAL
        case 10U: return 8U; // SRATIONAL
        default: return 0U;
    }
}

inline bool entryPayloadInBounds(const Reader& r, size_t entry,
                                 uint16_t type, uint32_t count,
                                 uint32_t maximumCount) {
    const std::size_t elementBytes = tiffTypeWidth(type);
    if (!r.in(entry, 12U) || elementBytes == 0U || count == 0U ||
        count > maximumCount ||
        count > std::numeric_limits<std::size_t>::max() / elementBytes) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(count) * elementBytes;
    const std::size_t valuesAt = bytes <= 4U
        ? entry + 8U
        : static_cast<std::size_t>(r.u32(entry + 8U));
    return r.in(valuesAt, bytes);
}

inline bool integralUnsignedValuesInRange(const Reader& r, size_t entry,
                                          uint16_t type, uint32_t count,
                                          uint32_t maximumValue) {
    if ((type != 3U && type != 4U && type != 5U) ||
        !entryPayloadInBounds(
            r, entry, type, count,
            static_cast<uint32_t>(
                RawPrecisionDescriptor::kMaxBlackPatternEntries))) {
        return false;
    }
    const std::size_t elementBytes = tiffTypeWidth(type);
    const std::size_t totalBytes = static_cast<std::size_t>(count) * elementBytes;
    const std::size_t valuesAt = totalBytes <= 4U
        ? entry + 8U
        : static_cast<std::size_t>(r.u32(entry + 8U));
    for (uint32_t index = 0U; index < count; ++index) {
        const std::size_t at =
            valuesAt + static_cast<std::size_t>(index) * elementBytes;
        std::uint32_t value = 0U;
        if (type == 3U) {
            value = r.u16(at);
        } else if (type == 4U) {
            value = r.u32(at);
        } else {
            const uint32_t numerator = r.u32(at);
            const uint32_t denominator = r.u32(at + 4U);
            if (denominator == 0U || numerator % denominator != 0U) {
                return false;
            }
            value = numerator / denominator;
        }
        if (value > maximumValue) {
            return false;
        }
    }
    return true;
}

inline bool signedRationalColorMatrixIsQualified(
        const Reader& r, size_t entry, int colorPlanes) {
    if (colorPlanes < 3 || colorPlanes > 4 || !r.in(entry, 12U) ||
        r.u16(entry + 2U) != 10U) {
        return false;
    }
    const std::uint32_t expectedCount =
        static_cast<std::uint32_t>(colorPlanes) * 3U;
    if (r.u32(entry + 4U) != expectedCount ||
        !entryPayloadInBounds(r, entry, 10U, expectedCount, 12U)) {
        return false;
    }
    const std::size_t valuesAt =
        static_cast<std::size_t>(r.u32(entry + 8U));
    for (int plane = 0; plane < colorPlanes; ++plane) {
        bool rowHasCoefficient = false;
        for (int xyz = 0; xyz < 3; ++xyz) {
            const std::size_t at = valuesAt +
                (static_cast<std::size_t>(plane) * 3U + xyz) * 8U;
            const std::uint32_t numeratorBits = r.u32(at);
            const std::uint32_t denominatorBits = r.u32(at + 4U);
            if (denominatorBits == 0U) return false;
            rowHasCoefficient = rowHasCoefficient || numeratorBits != 0U;
        }
        if (!rowHasCoefficient) return false;
    }
    return true;
}

// Decode exactly one already-bounds-checked TIFF directory entry. This helper
// records syntax and payload locations only; dependency-wide admission and
// selected-image semantics are evaluated in separate passes below.
inline void decodeBoundedIfdEntry(const Reader& r, size_t entry,
                                  bool containerIfd, ScanState& state,
                                  IfdDecode& decoded) {
    IfdCandidate& candidate = decoded.candidate;
    const uint16_t tag = r.u16(entry);
    const uint16_t type = r.u16(entry + 2U);
    const uint32_t count = r.u32(entry + 4U);
    const size_t valueAt = entry + 8U;
    const auto scalar = [&]() -> uint32_t {
        return type == 3U ? r.u16(valueAt) : r.u32(valueAt);
    };

    switch (tag) {
        case 0x00FE:  // NewSubfileType
            if (!markOnce(&decoded.sawNewSubfileType) ||
                (type != 3U && type != 4U) || count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.newSubfileType = scalar();
            }
            break;
        case 0x0100:  // ImageWidth
            if (!markOnce(&decoded.sawWidth) ||
                (type != 3U && type != 4U) || count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.width = scalar();
            }
            break;
        case 0x0101:  // ImageLength
            if (!markOnce(&decoded.sawHeight) ||
                (type != 3U && type != 4U) || count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.height = scalar();
            }
            break;
        case 0x0102:  // BitsPerSample
            if (!markOnce(&decoded.sawBits)) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.bitsEntry = entry;
            }
            break;
        case 0x0103:  // Compression
            if (!markOnce(&decoded.sawCompression) ||
                (type != 3U && type != 4U) || count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.compression = static_cast<int>(scalar());
            }
            break;
        case 0x0106:  // PhotometricInterpretation
            if (!markOnce(&decoded.sawPhotometric) || type != 3U ||
                count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.photometric = static_cast<int>(scalar());
            }
            break;
        case 0x0115:  // SamplesPerPixel
            if (!markOnce(&decoded.sawSamplesPerPixel) || type != 3U ||
                count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.samplesPerPixel = static_cast<int>(scalar());
                if (candidate.samplesPerPixel < 1 ||
                    candidate.samplesPerPixel > 4) {
                    candidate.metadataAmbiguous = true;
                }
            }
            break;
        case 0x011C:  // PlanarConfiguration
            if (!markOnce(&decoded.sawPlanarConfiguration) || type != 3U ||
                count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.planarConfiguration = static_cast<int>(r.u16(valueAt));
            }
            break;
        case 0x0152:  // ExtraSamples
            if (!markOnce(&decoded.sawExtraSamples) || type != 3U ||
                count == 0U || count > 4U ||
                !entryPayloadInBounds(r, entry, type, count, 4U)) {
                candidate.metadataAmbiguous = true;
            }
            // The qualified RAW contract has no alpha/auxiliary samples.
            candidate.metadataAmbiguous = true;
            break;
        case 0x0153:  // SampleFormat
            if (!markOnce(&decoded.sawSampleFormat)) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.sampleFormatEntry = entry;
            }
            break;
        case 0xC612:  // DNGVersion
            if (containerIfd) {
                state.containerMentionedDng = true;
                if (state.containerIsDng || type != 1U || count != 4U) {
                    state.containerAmbiguous = true;
                } else {
                    state.containerIsDng = true;
                }
            } else {
                // A child DNGVersion is hostile even when well-formed and even
                // when root IFD0 does not establish semantic DNG identity.
                state.nonRootDngVersion = true;
                candidate.metadataAmbiguous = true;
            }
            break;
        case 0x828D:  // CFARepeatPatternDim
            if (!markOnce(&decoded.sawCfaDimensions) || type != 3U ||
                count != 2U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.cfaPatternRows = r.u16(valueAt);
                candidate.cfaPatternColumns = r.u16(valueAt + 2U);
            }
            break;
        case 0x828E: {  // CFAPattern
            if (!markOnce(&decoded.sawCfaPattern) || type != 1U ||
                count == 0U ||
                count > RawPrecisionDescriptor::kMaxCfaPatternEntries) {
                candidate.metadataAmbiguous = true;
                break;
            }
            const size_t patternAt = count <= 4U
                ? valueAt
                : static_cast<size_t>(r.u32(valueAt));
            if (!r.in(patternAt, static_cast<size_t>(count))) {
                candidate.metadataAmbiguous = true;
                break;
            }
            decoded.cfaPatternCount = count;
            candidate.cfaPatternCount = count;
            for (uint32_t cell = 0U; cell < count; ++cell) {
                const uint8_t color = r.p[patternAt + cell];
                if (color > 3U) candidate.metadataAmbiguous = true;
                candidate.cfaPattern[cell] = color;
            }
            break;
        }
        case 0xC616: {  // CFAPlaneColor
            if (!markOnce(&decoded.sawCfaPlaneColor) || type != 1U ||
                count == 0U || count > candidate.cfaPlaneColors.size() ||
                !entryPayloadInBounds(
                    r, entry, type, count,
                    static_cast<std::uint32_t>(candidate.cfaPlaneColors.size()))) {
                candidate.metadataAmbiguous = true;
                break;
            }
            const size_t colorsAt = count <= 4U
                ? valueAt
                : static_cast<size_t>(r.u32(valueAt));
            candidate.cfaPlaneColorCount = count;
            for (uint32_t plane = 0U; plane < count; ++plane) {
                candidate.cfaPlaneColors[plane] = r.p[colorsAt + plane];
            }
            break;
        }
        case 0xC617:  // CFALayout
            if (!markOnce(&decoded.sawCfaLayout) || type != 3U ||
                count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.cfaLayout = static_cast<int>(r.u16(valueAt));
            }
            break;
        case 0xC619:  // BlackLevelRepeatDim
            if (!markOnce(&decoded.sawBlackRepeatDimensions) || type != 3U ||
                count != 2U) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.blackRepeatRows = static_cast<int>(r.u16(valueAt));
                candidate.blackRepeatColumns =
                    static_cast<int>(r.u16(valueAt + 2U));
            }
            break;
        case 0xC61A: {  // BlackLevel
            const bool payloadValid = entryPayloadInBounds(
                r, entry, type, count,
                static_cast<uint32_t>(
                    RawPrecisionDescriptor::kMaxBlackPatternEntries));
            if (!markOnce(&decoded.sawBlackLevel) ||
                (type != 3U && type != 4U && type != 5U) || count == 0U ||
                count > RawPrecisionDescriptor::kMaxBlackPatternEntries ||
                !payloadValid) {
                candidate.metadataAmbiguous = true;
                state.unsafeIfdMetadataForLibRawOpen = true;
            } else {
                candidate.hasBlackLevel = true;
                candidate.blackLevelCount = count;
                decoded.blackLevelEntry = entry;
                decoded.blackLevelType = type;
                decoded.blackLevelCount = count;
            }
            break;
        }
        case 0xC61B:  // BlackLevelDeltaH
        case 0xC61C: {  // BlackLevelDeltaV
            state.dependencyHasBlackLevelDelta = true;
            bool* seen = tag == 0xC61B
                ? &decoded.sawBlackDeltaH
                : &decoded.sawBlackDeltaV;
            if (!markOnce(seen) || type != 10U || count == 0U ||
                count > 65535U ||
                !entryPayloadInBounds(r, entry, type, count, 65535U)) {
                candidate.metadataAmbiguous = true;
            }
            candidate.hasBlackLevelDelta = true;
            break;
        }
        case 0xC61D:  // WhiteLevel
            if (!markOnce(&decoded.sawWhiteLevel) ||
                (type != 3U && type != 4U) || count == 0U || count > 4U ||
                !entryPayloadInBounds(r, entry, type, count, 4U)) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.hasWhiteLevel = true;
                candidate.whiteLevelCount = count;
                decoded.whiteLevelEntry = entry;
                decoded.whiteLevelType = type;
                decoded.whiteLevelCount = count;
            }
            break;
        case 0xC62A:  // BaselineExposure
            if (!markOnce(&decoded.sawBaselineExposure) || type != 10U ||
                count != 1U ||
                !entryPayloadInBounds(r, entry, type, count, 1U)) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.hasBaselineExposure = true;
            }
            break;
        case 0xC62E:  // LinearResponseLimit
            if (!markOnce(&decoded.sawLinearResponseLimit) || type != 5U ||
                count != 1U ||
                !entryPayloadInBounds(r, entry, type, count, 1U)) {
                candidate.metadataAmbiguous = true;
            } else {
                candidate.hasLinearResponseLimit = true;
            }
            break;
        case 0xC621:  // ColorMatrix1
            if (!markOnce(&decoded.sawColorMatrix1)) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.colorMatrix1Entry = entry;
            }
            break;
        case 0xC622:  // ColorMatrix2
            if (!markOnce(&decoded.sawColorMatrix2)) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.colorMatrix2Entry = entry;
            }
            break;
        case 0xC65A:  // CalibrationIlluminant1
            if (!markOnce(&decoded.sawCalibrationIlluminant1) || type != 3U ||
                count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.calibrationIlluminant1 =
                    static_cast<int>(r.u16(valueAt));
            }
            break;
        case 0xC65B:  // CalibrationIlluminant2
            if (!markOnce(&decoded.sawCalibrationIlluminant2) || type != 3U ||
                count != 1U) {
                candidate.metadataAmbiguous = true;
            } else {
                decoded.calibrationIlluminant2 =
                    static_cast<int>(r.u16(valueAt));
            }
            break;
        case 0x014A: {  // SubIFDs
            if (!markOnce(&decoded.sawSubIfds) || type != 4U || count == 0U ||
                count > decoded.subIfds.size()) {
                candidate.metadataAmbiguous = true;
                // LibRaw accepts and follows this tag independently of semantic
                // RAW selection (and accepts up to 1000 declared offsets). If
                // this bounded preflight cannot represent the complete link,
                // the dependency walk is not proven safe.
                state.walkIncomplete = true;
                break;
            }
            if (count == 1U) {
                decoded.subIfds[decoded.subIfdCount++] = scalar();
                break;
            }
            const uint32_t offsetsAt = r.u32(valueAt);
            if (!r.in(offsetsAt, static_cast<size_t>(count) * 4U)) {
                candidate.metadataAmbiguous = true;
                state.walkIncomplete = true;
                break;
            }
            for (uint32_t index = 0U; index < count; ++index) {
                decoded.subIfds[decoded.subIfdCount++] =
                    r.u32(static_cast<size_t>(offsetsAt) + index * 4U);
            }
            break;
        }
        default:
            break;
    }
}

// Evaluate only metadata that can make LibRaw's dependency-wide TIFF walk
// unsafe. This runs for every bounded IFD, regardless of reduced status,
// photometric interpretation, geometry, or eventual selection.
inline void evaluateDependencySafety(const Reader& r, IfdDecode& decoded,
                                     ScanState& state) {
    IfdCandidate& candidate = decoded.candidate;
    if (decoded.sawBits && !uniformShortArray(
            r, decoded.bitsEntry,
            static_cast<uint32_t>(candidate.samplesPerPixel),
            &candidate.bitsPerSample)) {
        candidate.metadataAmbiguous = true;
    }
    if (decoded.sawSampleFormat && !uniformShortArray(
            r, decoded.sampleFormatEntry,
            static_cast<uint32_t>(candidate.samplesPerPixel),
            &candidate.sampleFormat)) {
        candidate.metadataAmbiguous = true;
    }
    const bool qualifiedBits = candidate.bitsPerSample >= 8 &&
        candidate.bitsPerSample <= 16;
    const std::uint32_t declaredMaximum = !qualifiedBits
        ? 0U
        : (candidate.bitsPerSample == 16
               ? 65535U
               : ((1U << candidate.bitsPerSample) - 1U));
    const bool blackValuesUnsafe = candidate.hasBlackLevel &&
        (candidate.sampleFormat != 1 || !qualifiedBits ||
         !integralUnsignedValuesInRange(
             r, decoded.blackLevelEntry, decoded.blackLevelType,
             decoded.blackLevelCount, declaredMaximum));
    if (blackValuesUnsafe) {
        state.unsafeIfdMetadataForLibRawOpen = true;
    }
    if (candidate.sampleFormat == 1 &&
        (!qualifiedBits || blackValuesUnsafe ||
         (candidate.hasWhiteLevel &&
          !integralUnsignedValuesInRange(
              r, decoded.whiteLevelEntry, decoded.whiteLevelType,
              decoded.whiteLevelCount, declaredMaximum)))) {
        candidate.metadataAmbiguous = true;
    }
}

// Qualify the bounded per-image descriptor independently from dependency
// safety. No field from a preview or sibling is inherited into this candidate.
inline void qualifySemanticCandidate(const Reader& r, IfdDecode& decoded) {
    IfdCandidate& candidate = decoded.candidate;
    const std::uint64_t blackCells =
        static_cast<std::uint64_t>(candidate.blackRepeatRows) *
        static_cast<std::uint64_t>(candidate.blackRepeatColumns);
    const std::uint64_t requiredBlackCount = blackCells *
        static_cast<std::uint64_t>(candidate.samplesPerPixel);
    if (candidate.blackRepeatRows <= 0 || candidate.blackRepeatColumns <= 0 ||
        blackCells > RawPrecisionDescriptor::kMaxBlackPatternEntries ||
        requiredBlackCount > RawPrecisionDescriptor::kMaxBlackPatternEntries ||
        (candidate.hasBlackLevel &&
         candidate.blackLevelCount != requiredBlackCount) ||
        (candidate.hasWhiteLevel &&
         candidate.whiteLevelCount !=
             static_cast<std::size_t>(candidate.samplesPerPixel))) {
        candidate.metadataAmbiguous = true;
    }
    if (candidate.photometric == 32803) {
        const uint64_t cfaCells =
            static_cast<uint64_t>(candidate.cfaPatternRows) *
            candidate.cfaPatternColumns;
        if (!decoded.sawCfaDimensions || !decoded.sawCfaPattern ||
            cfaCells == 0U || cfaCells != decoded.cfaPatternCount ||
            candidate.cfaLayout != 1 ||
            candidate.cfaPlaneColorCount != 3U ||
            candidate.cfaPlaneColors[0] != 0U ||
            candidate.cfaPlaneColors[1] != 1U ||
            candidate.cfaPlaneColors[2] != 2U ||
            std::any_of(
                candidate.cfaPattern.begin(),
                candidate.cfaPattern.begin() + candidate.cfaPatternCount,
                [](std::uint8_t plane) { return plane > 2U; })) {
            candidate.metadataAmbiguous = true;
        }
        return;
    }
    if (candidate.photometric == 34892 &&
        (decoded.sawCfaDimensions || decoded.sawCfaPattern ||
         decoded.sawCfaPlaneColor || decoded.sawCfaLayout ||
         !decoded.sawPlanarConfiguration ||
         decoded.planarConfiguration != 1 || decoded.sawExtraSamples ||
         !decoded.sawColorMatrix1 || !decoded.sawColorMatrix2 ||
         !decoded.sawCalibrationIlluminant1 ||
         !decoded.sawCalibrationIlluminant2 ||
         decoded.calibrationIlluminant1 == 0 ||
         decoded.calibrationIlluminant2 == 0 ||
         !signedRationalColorMatrixIsQualified(
             r, decoded.colorMatrix1Entry, candidate.samplesPerPixel) ||
         !signedRationalColorMatrixIsQualified(
             r, decoded.colorMatrix2Entry, candidate.samplesPerPixel) ||
         candidate.blackRepeatRows != 1 ||
         candidate.blackRepeatColumns != 1)) {
        candidate.metadataAmbiguous = true;
    }
}

// Aggregate one decoded IFD into the semantic candidate set only after the
// independent dependency-safety pass has observed it.
inline void aggregateSemanticCandidate(IfdDecode& decoded, uint32_t ifdOffset,
                                       ScanState& state) {
    IfdCandidate& candidate = decoded.candidate;
    const bool reduced = (candidate.newSubfileType & 1U) != 0U;
    const bool rawPhotometric = candidate.photometric == 32803 ||
        candidate.photometric == 34892;
    const bool malformedPhotometric =
        decoded.sawPhotometric && candidate.photometric == -1;
    const bool rawSpecificMetadata =
        decoded.sawCfaDimensions || decoded.sawCfaPattern ||
        decoded.sawCfaPlaneColor || decoded.sawCfaLayout ||
        decoded.sawBlackRepeatDimensions || decoded.sawBlackLevel ||
        decoded.sawBlackDeltaH || decoded.sawBlackDeltaV ||
        decoded.sawWhiteLevel || decoded.sawBaselineExposure ||
        decoded.sawLinearResponseLimit || decoded.sawColorMatrix1 ||
        decoded.sawColorMatrix2 || decoded.sawCalibrationIlluminant1 ||
        decoded.sawCalibrationIlluminant2 ||
        decoded.sawPlanarConfiguration || decoded.sawExtraSamples;
    const bool potentialRawCandidate = !reduced &&
        (rawPhotometric || malformedPhotometric ||
         (!decoded.sawPhotometric && rawSpecificMetadata));
    const bool eligibilityAmbiguous = potentialRawCandidate &&
        (!rawPhotometric || !decoded.sawWidth || candidate.width == 0U ||
         !decoded.sawHeight || candidate.height == 0U ||
         !decoded.sawCompression || candidate.compression == kUnknown);
    if (potentialRawCandidate) {
        state.unsafeRawCandidateForLibRawOpen =
            state.unsafeRawCandidateForLibRawOpen ||
            candidate.metadataAmbiguous || eligibilityAmbiguous;
    }
    if (reduced || candidate.compression == kUnknown) return;
    candidate.ifdOffset = ifdOffset;
    if (state.rawCandidateCount == state.rawCandidates.size()) {
        state.candidateOverflow = true;
    } else {
        state.rawCandidates[state.rawCandidateCount++] = candidate;
    }
}

inline void walkIfd(const Reader& r, uint32_t ifdOff, ScanState& state,
                    int depth, unsigned& remainingIfds,
                    bool containerIfd) {
    if (ifdOff == 0U) return;
    if (depth >= static_cast<int>(ScanState::kMaxDependencyIfds) ||
        remainingIfds == 0 || !r.in(ifdOff, 2)) {
        state.walkIncomplete = true;
        return;
    }
    if (std::find(state.visitedIfdOffsets.begin(),
                  state.visitedIfdOffsets.begin() + state.visitedIfdCount,
                  ifdOff) !=
        state.visitedIfdOffsets.begin() + state.visitedIfdCount) {
        state.walkIncomplete = true;
        return;
    }
    if (state.visitedIfdCount == state.visitedIfdOffsets.size()) {
        state.walkIncomplete = true;
        return;
    }
    state.visitedIfdOffsets[state.visitedIfdCount++] = ifdOff;
    --remainingIfds;
    uint16_t entries = r.u16(ifdOff);
    if (entries == 0 || entries > 512) {
        state.walkIncomplete = true;
        return;
    }

    IfdDecode decoded;

    for (uint16_t i = 0; i < entries; ++i) {
        size_t e = (size_t)ifdOff + 2 + (size_t)i * 12;
        if (!r.in(e, 12)) {
            state.walkIncomplete = true;
            return;
        }
        decodeBoundedIfdEntry(r, e, containerIfd, state, decoded);
    }

    evaluateDependencySafety(r, decoded, state);
    qualifySemanticCandidate(r, decoded);
    aggregateSemanticCandidate(decoded, ifdOff, state);
    for (std::size_t s = 0U; s < decoded.subIfdCount; ++s) {
        walkIfd(r, decoded.subIfds[s], state, depth + 1, remainingIfds, false);
    }

    const size_t nextAt = (size_t)ifdOff + 2 + (size_t)entries * 12;
    if (!r.in(nextAt, 4U)) {
        state.walkIncomplete = true;
        return;
    }
    uint32_t next = r.u32(nextAt);
    if (next != 0U) {
        walkIfd(r, next, state, depth, remainingIfds, false);
    }
}

struct CandidateSelection {
    const IfdCandidate* candidate = nullptr;
    std::size_t matches = 0U;
};

inline CandidateSelection selectCandidate(const ScanState& state,
                                          uint32_t decodedRawWidth,
                                          uint32_t decodedRawHeight) {
    CandidateSelection selection;
    if (decodedRawWidth != 0U && decodedRawHeight != 0U) {
        for (std::size_t index = 0U; index < state.rawCandidateCount; ++index) {
            const IfdCandidate& candidate = state.rawCandidates[index];
            const bool rawPhotometric = candidate.photometric == 32803 ||
                candidate.photometric == 34892;
            if (rawPhotometric && candidate.width == decodedRawWidth &&
                candidate.height == decodedRawHeight) {
                selection.candidate = &candidate;
                ++selection.matches;
            }
        }
        return selection;
    }

    // Diagnostic-only calls after open/unpack failure have no decoded geometry.
    // Retain largest-plane codec classification, never descriptor publication.
    uint64_t largestPixels = 0U;
    for (std::size_t index = 0U; index < state.rawCandidateCount; ++index) {
        const IfdCandidate& candidate = state.rawCandidates[index];
        const uint64_t pixels = static_cast<uint64_t>(candidate.width) *
            candidate.height;
        if (selection.candidate == nullptr || pixels > largestPixels) {
            largestPixels = pixels;
            selection.candidate = &candidate;
            selection.matches = 1U;
        } else if (pixels == largestPixels) {
            ++selection.matches;
        }
    }
    return selection;
}

inline PrimaryImageInfo primaryImageInfoOf(const uint8_t* data, size_t len,
                                           uint32_t decodedRawWidth = 0U,
                                           uint32_t decodedRawHeight = 0U) {
    PrimaryImageInfo info;
    if (data == nullptr || len < 2U) return info;
    Reader r;
    r.p = data;
    r.n = len;
    if (data[0] == 'I' && data[1] == 'I') r.be = false;
    else if (data[0] == 'M' && data[1] == 'M') r.be = true;
    else return info;
    // LibRaw identify dispatches on the endian marker before parse_tiff(), and
    // its parser consumes but does not validate the classic TIFF magic. Treat
    // every such header as a dependency-TIFF candidate. Wrong/truncated magic
    // is rejected here rather than being allowed to enter dependency parsing.
    info.dependencyTiffCandidate = true;
    info.bigEndian = r.be;
    if (len < 8U || r.u16(2U) != 42U) {
        info.dependencyUnsafeForLibRawOpen = true;
        return info;
    }
    info.recognizedTiff = true;
    // Bound recursive SubIFDs and the otherwise attacker-controlled next-IFD
    // graph at the same ten stored IFDs as LibRaw. Any additional reachable
    // edge makes the dependency-safety proof incomplete and is rejected above
    // open_buffer(), rather than being silently omitted from preflight.
    unsigned remainingIfds =
        static_cast<unsigned>(ScanState::kMaxDependencyIfds);
    ScanState state;
    walkIfd(r, r.u32(4), state, 0, remainingIfds, true);
    info.isDng = state.containerIsDng || state.containerMentionedDng;
    info.metadataAmbiguous = state.containerAmbiguous || state.candidateOverflow;
    info.unsafeForLibRawOpen = state.containerAmbiguous ||
        state.candidateOverflow || state.walkIncomplete ||
        state.unsafeIfdMetadataForLibRawOpen ||
        state.unsafeRawCandidateForLibRawOpen || state.nonRootDngVersion;
    info.dependencyUnsafeForLibRawOpen =
        state.walkIncomplete || state.unsafeIfdMetadataForLibRawOpen ||
        state.nonRootDngVersion;
    info.hasBlackLevelDelta = state.dependencyHasBlackLevelDelta;

    const CandidateSelection selection =
        selectCandidate(state, decodedRawWidth, decodedRawHeight);
    if (selection.matches != 1U || selection.candidate == nullptr) {
        info.metadataAmbiguous = info.metadataAmbiguous || info.isDng;
        info.unsafeForLibRawOpen = info.unsafeForLibRawOpen || info.isDng;
        return info;
    }
    const IfdCandidate* selected = selection.candidate;

    info.compression = selected->compression;
    info.sampleFormat = selected->sampleFormat;
    info.bitsPerSample = selected->bitsPerSample;
    info.samplesPerPixel = selected->samplesPerPixel;
    info.photometric = selected->photometric;
    info.cfaPatternRows = selected->cfaPatternRows;
    info.cfaPatternColumns = selected->cfaPatternColumns;
    info.cfaPattern = selected->cfaPattern;
    info.cfaPatternCount = selected->cfaPatternCount;
    info.cfaPlaneColors = selected->cfaPlaneColors;
    info.cfaPlaneColorCount = selected->cfaPlaneColorCount;
    info.cfaLayout = selected->cfaLayout;
    info.blackRepeatRows = selected->blackRepeatRows;
    info.blackRepeatColumns = selected->blackRepeatColumns;
    info.blackLevelCount = selected->blackLevelCount;
    info.whiteLevelCount = selected->whiteLevelCount;
    info.width = selected->width;
    info.height = selected->height;
    info.selectedIfdOffset = selected->ifdOffset;
    info.metadataAmbiguous = info.metadataAmbiguous ||
        selected->metadataAmbiguous;
    info.unsafeForLibRawOpen = info.unsafeForLibRawOpen ||
        selected->metadataAmbiguous;
    info.hasBlackLevel = selected->hasBlackLevel;
    info.hasBlackLevelDelta = info.hasBlackLevelDelta ||
        selected->hasBlackLevelDelta;
    info.hasWhiteLevel = selected->hasWhiteLevel;
    info.hasBaselineExposure = selected->hasBaselineExposure;
    info.hasLinearResponseLimit = selected->hasLinearResponseLimit;
    return info;
}

// Returns the primary-image compression; sets *isDng if a DNGVersion tag seen.
inline int compressionOf(const uint8_t* data, size_t len, bool* isDng) {
    const PrimaryImageInfo info = primaryImageInfoOf(data, len);
    *isDng = info.isDng;
    return info.compression;
}

inline bool isDeflate(int c) { return c == kDeflate || c == kDeflateAdobe; }
inline bool isLossyJpeg(int c) { return c == kLossyJpeg || c == kLossyJpegOld; }
inline bool isJpegXL(int c) { return c == kJpegXL; }

inline int classifyKnownCodecFallback(const PrimaryImageInfo& info) {
    if (!info.isDng) return SFRAW_ERR_UNPACK;
    const int comp = info.compression;
    if (isJpegXL(comp)) return SFRAW_ERR_JPEGXL_DNG;
    if (isLossyJpeg(comp)) {
#ifndef USE_JPEG
        return SFRAW_ERR_LOSSY_JPEG_DNG;
#endif
    }
    // Parser availability is not publication qualification. Every deflate
    // layout uses the typed fallback: integer layouts are unsupported and the
    // float bitmap path quantizes before the module's f32 boundary.
    if (isDeflate(comp)) return SFRAW_ERR_DEFLATE_DNG;
    return SFRAW_ERR_UNPACK;
}

// Some unsupported compression tags are rejected by LibRaw during
// open_buffer(), before unpack classification is reachable. Keep those typed.
inline int classifyOpenFailure(const uint8_t* data, size_t len) {
    return classifyKnownCodecFallback(primaryImageInfoOf(data, len));
}

// Classify an unpack() failure for a compressed DNG into a stable status.
// Returns SFRAW_ERR_UNPACK if it isn't a recognizable must-fallback case.
//
// Note: this only runs AFTER unpack() has already failed. Uncompressed (1),
// lossless-JPEG/LJ92 (7) decode natively. LibRaw itself can unpack SampleFormat=3
// float deflate, but finishDecode's earlier precision gate routes it before this
// failure classifier; integer/linear deflate remains a typed fallback here.
inline int classifyUnpackFailure(const uint8_t* data, size_t len) {
    const PrimaryImageInfo info = primaryImageInfoOf(data, len);
    const int knownFallback = classifyKnownCodecFallback(info);
    if (knownFallback != SFRAW_ERR_UNPACK) return knownFallback;
    if (info.isDng) {
        // A DNG of unknown/unreadable compression that still failed unpack: the
        // dominant real-world cause among DNGs LibRaw can't open is an
        // unsupported lossy codec, so hint the platform fallback.
        if (info.compression == kUnknown) return SFRAW_ERR_LOSSY_JPEG_DNG;
    }
    return SFRAW_ERR_UNPACK;
}

}  // namespace dngsniff

// Human-readable DNG Compression name (free function; declared in the header).
const char* dngCompressionName(int v) {
    switch (v) {
        case 1: return "uncompressed (1)";
        case 6: return "old-style JPEG (6, lossy)";
        case 7: return "lossless JPEG / LJ92 (7)";
        case 8: return "deflate / ZIP (8)";
        case 0x80B2: return "deflate / Adobe (0x80B2)";
        case 0x884C: return "lossy baseline JPEG (0x884C)";
        case 0xCD42: return "JPEG-XL (0xCD42)";
        case -1: return "unknown/none";
        default: return "other";
    }
}

namespace {

// LibRaw defaults its unpack/raw-store allocation budget to 2048 MiB, which is
// unsuitable for an Android process. Patch 0004 separately bounds identify-time
// metadata at compile time. Ticket #173 owns a future device-class/tiled policy.
constexpr unsigned kMaxRawMemoryMb = 128;
// Immediate safety ceiling for the current in-memory decoder. Ticket #173
// replaces the fd slurp with a seekable/tiled datastream before larger future
// RAWs can be admitted without making encoded bytes an unbounded allocation.
constexpr size_t kMaxEncodedInputBytes = 64U * 1024U * 1024U;
// LibRaw's raw-store budget does not include every later dcraw_process(),
// dcraw_make_mem_image(), and float-result allocation. Bound metadata-declared
// pixels before unpack() as an immediate mobile-safe policy. Do not relax this
// for half_size: non-Bayer/linear DNG layouts may ignore that request and still
// allocate at full resolution. Ticket #173 replaces the fixed limit with tiled
// decode and device-class budgeting.
constexpr uint64_t kMaxFullDecodePixels =
    sizeof(void*) >= 8 ? 12ULL * 1024ULL * 1024ULL
                       : 8ULL * 1024ULL * 1024ULL;

bool cancellationRequested(const DecodeOptions& options) {
    return options.cancelFlag != nullptr &&
           options.cancelFlag->load(std::memory_order_acquire);
}

#if SFRAW_HAVE_LIBRAW
void applyInputLimits(LibRaw& raw) {
    raw.imgdata.rawparams.max_raw_memory_mb = kMaxRawMemoryMb;
}

bool isMemoryLimitFailure(int code) {
    return code == LIBRAW_UNSUFFICIENT_MEMORY || code == LIBRAW_TOO_BIG ||
           code == ENOMEM;
}

int cancelLibRawProgress(void* context, LibRaw_progress, int, int) {
#if defined(SFRAW_TESTING)
    sfraw::test::notifyLibRawProgressForTest();
#endif
    const auto* flag = static_cast<const std::atomic<bool>*>(context);
    return flag != nullptr && flag->load(std::memory_order_acquire) ? 1 : 0;
}

void applyCancellationHandler(LibRaw& raw, const DecodeOptions& options) {
    if (options.cancelFlag != nullptr) {
        // LibRaw owns neither the callback context nor the flag. The DecodeOptions
        // cancellation lease outlives this synchronous decode, and the callback
        // only reads the atomic even though LibRaw's API accepts an untyped void*.
        raw.set_progress_handler(cancelLibRawProgress,
                                 const_cast<std::atomic<bool>*>(options.cancelFlag));
    }
}

DecodeResult cancellationFailure(int librawCode = 0) {
    DecodeResult result;
    result.librawCode = librawCode;
    result.status = SFRAW_ERR_CANCELLED;
    result.error = "RAW decode cancelled";
    return result;
}

DecodeResult invalidWhiteBalanceFailure() {
    DecodeResult result;
    result.status = SFRAW_ERR_INPUT;
    result.error =
        "invalid RAW white balance: temperature must be finite and in "
        "[1000,12000] K; tint must be finite and in [0.2,1.8]";
    return result;
}

bool pixelProductExceeds(uint64_t width, uint64_t height, uint64_t limit) {
    return width != 0U && (height > limit / width);
}

bool stretchedDimensionsExceedLimit(const libraw_image_sizes_t& sizes,
                                    uint64_t limit) {
    const double aspect = sizes.pixel_aspect;
    if (!std::isfinite(aspect) || aspect <= 0.0) return true;

    const uint64_t width = sizes.width;
    const uint64_t height = sizes.height;
    if (width == 0U || height == 0U) return false;

    double projectedWidth = static_cast<double>(width);
    double projectedHeight = static_cast<double>(height);
    // TIFF identify() normalizes only values strictly inside (0.995, 1.005).
    // Mirror stretch() for every surviving value on either side of 1.0,
    // including the exact 0.995/1.005 boundary values.
    if (aspect < 1.0) {
        projectedHeight = projectedHeight / aspect + 0.5;
    } else if (aspect > 1.0) {
        projectedWidth = projectedWidth * aspect + 0.5;
    }
    const double maxUshort =
        static_cast<double>(std::numeric_limits<uint16_t>::max());
    if (!std::isfinite(projectedWidth) || !std::isfinite(projectedHeight) ||
        projectedWidth > maxUshort || projectedHeight > maxUshort) {
        return true;
    }
    return pixelProductExceeds(static_cast<uint64_t>(projectedWidth),
                               static_cast<uint64_t>(projectedHeight), limit);
}

bool declaredDecodeDimensionsExceedLimit(const LibRaw& raw,
                                         const DecodeOptions&) {
    const auto& sizes = raw.imgdata.sizes;
    // LibRaw::unpack() may expand its raw-store dimensions to include active
    // area margins even when raw_width*raw_height and width*height are each
    // individually below the limit. Mirror that allocation geometry with
    // widened arithmetic so hostile ActiveArea values cannot amplify memory.
    const uint64_t allocationWidth = std::max<uint64_t>(
        sizes.raw_width,
        static_cast<uint64_t>(sizes.width) + sizes.left_margin);
    const uint64_t allocationHeight = std::max<uint64_t>(
        sizes.raw_height,
        static_cast<uint64_t>(sizes.height) + sizes.top_margin);
    return pixelProductExceeds(sizes.raw_width, sizes.raw_height,
                               kMaxFullDecodePixels) ||
           pixelProductExceeds(sizes.width, sizes.height,
                               kMaxFullDecodePixels) ||
           pixelProductExceeds(allocationWidth, allocationHeight,
                               kMaxFullDecodePixels) ||
           stretchedDimensionsExceedLimit(sizes, kMaxFullDecodePixels);
}

DecodeResult dimensionLimitFailure(const DecodeOptions&) {
    DecodeResult result;
    result.status = SFRAW_ERR_NO_MEMORY;
    result.error = sizeof(void*) >= 8
        ? "RAW geometry exceeds 12 MiPixel in-memory safety limit"
        : "RAW geometry exceeds 8 MiPixel in-memory safety limit";
    return result;
}

DecodeResult precisionMetadataFailure(const char* detail,
                                      int status = SFRAW_ERR_PRECISION_METADATA) {
    DecodeResult result;
    result.status = status;
    result.error = detail != nullptr ? detail : "unsupported RAW precision metadata";
    return result;
}

bool preflightLibRawOpen(const uint8_t* data, size_t length,
                         DecodeResult* failure) {
    const dngsniff::PrimaryImageInfo safety =
        dngsniff::primaryImageInfoOf(data, length);
    if (safety.recognizedTiff && safety.hasBlackLevelDelta) {
        *failure = precisionMetadataFailure(
            "TIFF/DNG BlackLevelDeltaH/V is unqualified on a dependency-walked IFD before LibRaw open");
        return false;
    }
    if (safety.dependencyTiffCandidate &&
        safety.dependencyUnsafeForLibRawOpen) {
        *failure = precisionMetadataFailure(
            "TIFF/DNG dependency metadata is hostile or ambiguous before LibRaw open");
        return false;
    }
    if (safety.isDng && safety.unsafeForLibRawOpen) {
        *failure = precisionMetadataFailure(
            "DNG metadata is malformed or ambiguous before LibRaw open");
        return false;
    }
    return true;
}

int bitsRequired(std::uint64_t distinctValues) {
    if (distinctValues <= 1U) return distinctValues == 0U ? 0 : 1;
    int bits = 0;
    --distinctValues;
    while (distinctValues != 0U) {
        ++bits;
        distinctValues >>= 1U;
    }
    return bits;
}

bool addLevels(std::uint32_t first, std::uint32_t second,
               std::uint32_t third, std::uint32_t* total) {
    const std::uint64_t widened = static_cast<std::uint64_t>(first) + second + third;
    if (widened > std::numeric_limits<std::uint32_t>::max()) return false;
    *total = static_cast<std::uint32_t>(widened);
    return true;
}

bool preflightSourcePrecision(
        const dngsniff::PrimaryImageInfo& source, const char** failure,
        int* failureStatus) {
    if (!source.isDng) return true;
    if (source.metadataAmbiguous || source.bitsPerSample <= 0 ||
        source.compression == dngsniff::kUnknown) {
        *failure =
            "DNG primary image has ambiguous precision/compression metadata";
        *failureStatus = SFRAW_ERR_PRECISION_METADATA;
        return false;
    }
    if (source.sampleFormat == 3) {
        *failure =
            "floating-point DNG requires a precision-preserving codec; "
            "the qualified LibRaw bitmap route would quantize before float32";
        *failureStatus = SFRAW_ERR_DEFLATE_DNG;
        return false;
    }
    if (source.sampleFormat != 1 || source.bitsPerSample < 8 ||
        source.bitsPerSample > 16) {
        *failure =
            "DNG integer sample format/precision is outside the qualified 8-16-bit route";
        *failureStatus = SFRAW_ERR_PRECISION_METADATA;
        return false;
    }
    if (source.hasBlackLevelDelta) {
        *failure =
            "DNG BlackLevelDeltaH/V requires an unqualified spatial level transform";
        *failureStatus = SFRAW_ERR_PRECISION_METADATA;
        return false;
    }
    if (source.photometric == 32803) {
        if (source.samplesPerPixel != 1 ||
            source.cfaLayout != 1 ||
            source.cfaPlaneColorCount != 3U ||
            source.cfaPlaneColors[0] != 0U ||
            source.cfaPlaneColors[1] != 1U ||
            source.cfaPlaneColors[2] != 2U ||
            !((source.cfaPatternRows == 2 && source.cfaPatternColumns == 2) ||
              (source.cfaPatternRows == 6 && source.cfaPatternColumns == 6))) {
            *failure =
                "DNG CFA mapping/layout is outside the qualified rectangular RGB 2x2/6x6 subset";
            *failureStatus = SFRAW_ERR_PRECISION_METADATA;
            return false;
        }
    } else if (source.photometric == 34892) {
        if (source.samplesPerPixel < 3 || source.samplesPerPixel > 4 ||
            source.blackRepeatRows != 1 || source.blackRepeatColumns != 1 ||
            (source.hasBlackLevel &&
             source.blackLevelCount !=
                 static_cast<std::size_t>(source.samplesPerPixel)) ||
            (source.hasWhiteLevel &&
             source.whiteLevelCount !=
                 static_cast<std::size_t>(source.samplesPerPixel))) {
            *failure =
                "DNG LinearRaw descriptor v1 requires a 1x1 per-channel three/four-sample level model";
            *failureStatus = SFRAW_ERR_PRECISION_METADATA;
            return false;
        }
    } else {
        *failure = "DNG selected image is not CFA or LinearRaw";
        *failureStatus = SFRAW_ERR_PRECISION_METADATA;
        return false;
    }
    return true;
}

bool postIdentifyLinearRawColorIsQualified(
        const LibRaw& raw, const dngsniff::PrimaryImageInfo& source) {
    if (!source.isDng || source.photometric != 34892) return true;
    if (source.samplesPerPixel < 3 || source.samplesPerPixel > 4 ||
        raw.imgdata.idata.colors != source.samplesPerPixel) {
        return false;
    }
    for (int matrix = 0; matrix < 2; ++matrix) {
        const auto& dngColor = raw.imgdata.color.dng_color[matrix];
        if (dngColor.illuminant == 0U) return false;
        for (int plane = 0; plane < source.samplesPerPixel; ++plane) {
            bool rowActive = false;
            for (int xyz = 0; xyz < 3; ++xyz) {
                const float coefficient = dngColor.colormatrix[plane][xyz];
                if (!std::isfinite(coefficient)) return false;
                rowActive = rowActive || coefficient != 0.0f;
            }
            if (!rowActive) return false;
        }
    }
    // `rgb_cam` is the post-identify camera-to-output coefficient table used by
    // LibRaw processing. Requiring one finite non-zero coefficient per admitted
    // sensor plane proves that a structurally present fourth matrix row was not
    // discarded while the selected IFD was promoted into decoder state.
    for (int plane = 0; plane < source.samplesPerPixel; ++plane) {
        bool planeActive = false;
        for (int output = 0; output < 3; ++output) {
            const float coefficient = raw.imgdata.color.rgb_cam[output][plane];
            if (!std::isfinite(coefficient)) return false;
            planeActive = planeActive || coefficient != 0.0f;
        }
        if (!planeActive) return false;
    }
    return true;
}

bool buildPrecisionDescriptor(
        LibRaw& raw, const dngsniff::PrimaryImageInfo& source,
        const DecodeOptions& options, RawPrecisionDescriptor* descriptor,
        const char** failure) {
    RawPrecisionDescriptor value;
    value.halfSizeRequested = options.halfSize;
    value.requestedMaxLongEdge = std::max(0, options.maxLongEdge);
    value.colorChannels = raw.imgdata.idata.colors;
    value.cfaFilterCode = raw.imgdata.idata.filters;

    if (value.colorChannels < 1 || value.colorChannels > 4) {
        *failure = "RAW precision metadata has unsupported color-channel count";
        return false;
    }

    if (source.isDng) {
        if (source.metadataAmbiguous || source.bitsPerSample <= 0 ||
            source.compression == dngsniff::kUnknown) {
            *failure =
                "DNG primary image has ambiguous precision/compression metadata";
            return false;
        }
        if (source.sampleFormat == 3) {
            *failure =
                "floating-point DNG requires a precision-preserving codec; "
                "the qualified LibRaw bitmap route would quantize before float32";
            return false;
        }
        if (source.sampleFormat != 1 || source.bitsPerSample < 8 ||
            source.bitsPerSample > 16) {
            *failure =
                "DNG integer sample format/precision is outside the qualified 8-16-bit route";
            return false;
        }
        if (source.hasBlackLevelDelta) {
            *failure =
                "DNG BlackLevelDeltaH/V requires an unqualified spatial level transform";
            return false;
        }
        const unsigned decoderBits = raw.imgdata.color.raw_bps;
        if (decoderBits != 0U && decoderBits != source.bitsPerSample) {
            *failure =
                "DNG container precision disagrees with decoder metadata";
            return false;
        }
        value.sampleFormat = RawSampleFormat::UnsignedInteger;
        value.declaredBitsPerSample = source.bitsPerSample;
        value.byteOrder = source.bigEndian ? RawByteOrder::BigEndian
                                           : RawByteOrder::LittleEndian;
        value.containerCompression = source.compression;
        if (source.compression == dngsniff::kNone) {
            value.packing = source.bitsPerSample == 16
                ? RawPacking::TiffWord16
                : RawPacking::TiffPackedBits;
        } else {
            value.packing = RawPacking::LosslessCompressed;
        }
        value.whiteLevelProvenance = source.hasWhiteLevel
            ? RawLevelProvenance::DngMetadata
            : RawLevelProvenance::Unknown;
        value.blackLevelProvenance = source.hasBlackLevel
            ? RawLevelProvenance::DngMetadata
            : RawLevelProvenance::Unknown;
    } else {
        value.sampleFormat = RawSampleFormat::UnsignedInteger;
        const unsigned rawBits = raw.imgdata.color.raw_bps;
        if (rawBits != 0U && (rawBits < 8U || rawBits > 16U)) {
            *failure =
                "decoder-reported RAW precision is outside the qualified 8-16-bit integer route";
            return false;
        }
        value.declaredBitsPerSample = rawBits >= 8U && rawBits <= 16U
            ? static_cast<int>(rawBits)
            : 0;
        value.byteOrder = RawByteOrder::Unknown;
        value.packing = RawPacking::VendorDefined;
        value.whiteLevelProvenance = RawLevelProvenance::DecoderMetadata;
        value.blackLevelProvenance = RawLevelProvenance::DecoderMetadata;
    }

    const unsigned filters = raw.imgdata.idata.filters;
    if (raw.imgdata.idata.is_foveon != 0U) {
        value.pixelLayout = RawPixelLayout::Layered;
    } else if (filters == 0U) {
        if (source.isDng && source.photometric != 34892) {
            *failure = "DNG CFA metadata produced an unverified linear decoder layout";
            return false;
        }
        if (source.isDng && value.colorChannels != source.samplesPerPixel) {
            *failure =
                "DNG LinearRaw sample count disagrees with decoder color planes";
            return false;
        }
        value.pixelLayout = RawPixelLayout::Linear;
    } else if (filters == 9U) {
        if (source.isDng &&
            (source.photometric != 32803 || source.cfaPatternRows != 6 ||
             source.cfaPatternColumns != 6)) {
            *failure = "DNG X-Trans geometry disagrees with decoder metadata";
            return false;
        }
        value.pixelLayout = RawPixelLayout::XTrans6x6;
        value.cfaPatternRows = 6;
        value.cfaPatternColumns = 6;
        value.cfaPatternCount = 36U;
    } else if (filters > 1000U) {
        if (source.isDng &&
            (source.photometric != 32803 || source.cfaPatternRows != 2 ||
             source.cfaPatternColumns != 2)) {
            *failure = "DNG Bayer geometry disagrees with decoder metadata";
            return false;
        }
        // LibRaw's packed `filters` word describes up to an 8x2 FC period. Only
        // call it Bayer2x2 after proving every encoded row repeats the first 2x2;
        // otherwise serializing four cells would silently lose sensor geometry.
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 2; ++column) {
                if (raw.COLOR(row, column) !=
                    raw.COLOR(row % 2, column % 2)) {
                    *failure =
                        "RAW CFA filter word is not a verified 2x2 Bayer period";
                    return false;
                }
            }
        }
        value.pixelLayout = RawPixelLayout::Bayer2x2;
        value.cfaPatternRows = 2;
        value.cfaPatternColumns = 2;
        value.cfaPatternCount = 4U;
    } else {
        // `filters == 1` uses LibRaw's 16x16/custom color table; other small
        // encodings are likewise not representable by the bounded v1 layouts.
        *failure = "RAW custom CFA geometry is not qualified by descriptor v1";
        return false;
    }
    // LibRaw exposes four fixed level slots because CFA decoders may use slot
    // three for the second green even when `colors == 3`. LinearRaw is
    // different: SamplesPerPixel names the complete sensor-plane set, so its
    // unused carrier padding must never affect provenance, validation, or the
    // effective code-span calculation.
    const std::size_t activeLevelChannels =
        value.pixelLayout == RawPixelLayout::Linear
            ? static_cast<std::size_t>(value.colorChannels)
            : 4U;
    if (value.cfaPatternCount != 0U) {
        if (value.cfaPatternCount > value.cfaPattern.size()) {
            *failure = "RAW CFA pattern exceeds descriptor bound";
            return false;
        }
        for (int row = 0; row < value.cfaPatternRows; ++row) {
            for (int column = 0; column < value.cfaPatternColumns; ++column) {
                const int channel = raw.COLOR(row, column);
                // LibRaw may encode the second green plane as CFA index 3 even
                // when `colors == 3`; its cdesc maps that plane back to green.
                if (channel < 0 || channel > 3) {
                    *failure = "RAW CFA pattern references an invalid color channel";
                    return false;
                }
                value.cfaPattern[static_cast<std::size_t>(
                    row * value.cfaPatternColumns + column)] =
                    static_cast<std::uint8_t>(channel);
            }
        }
        if (source.isDng) {
            bool cfaMatches = source.cfaPatternCount == value.cfaPatternCount;
            for (std::size_t cell = 0U;
                 cell < value.cfaPatternCount && cfaMatches; ++cell) {
                const std::uint8_t decoded = value.cfaPattern[cell];
                const std::uint8_t declared = source.cfaPattern[cell];
                // LibRaw uses slot 3 to keep the second green plane distinct in
                // a three-colour Bayer mosaic. DNG's default CFA plane table
                // names both greens as plane 1, so canonicalize only that
                // documented alias while comparing selected-IFD provenance.
                cfaMatches = decoded == declared ||
                    (value.colorChannels == 3 && decoded == 3U &&
                     declared == 1U);
            }
            if (!cfaMatches) {
                *failure =
                    "DNG selected-IFD CFA pattern disagrees with decoder metadata";
                return false;
            }
        }
    }

    const auto& color = raw.imgdata.color;
    value.blackLevelCommon = color.black;
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        value.blackLevelChannels[channel] = color.cblack[channel];
    }
    const std::uint64_t blackRows = color.cblack[4];
    const std::uint64_t blackColumns = color.cblack[5];
    if ((blackRows == 0U) != (blackColumns == 0U) ||
        blackRows > 8U || blackColumns > 8U ||
        (blackRows != 0U &&
         blackColumns > RawPrecisionDescriptor::kMaxBlackPatternEntries /
                            blackRows)) {
        *failure = "RAW black-level repeat pattern exceeds descriptor bound";
        return false;
    }
    value.blackPatternCount = static_cast<std::size_t>(blackRows * blackColumns);
    value.blackPatternRows = static_cast<int>(blackRows);
    value.blackPatternColumns = static_cast<int>(blackColumns);
    for (std::size_t index = 0U; index < value.blackPatternCount; ++index) {
        value.blackPattern[index] = color.cblack[6U + index];
    }

    const std::uint32_t defaultWhite = color.maximum;
    if (defaultWhite == 0U) {
        *failure = "RAW effective WhiteLevel is absent";
        return false;
    }
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
        const std::uint32_t dngWhite =
            color.dng_levels.dng_whitelevel[channel];
        value.whiteLevels[channel] = source.isDng && dngWhite != 0U
            ? dngWhite
            : defaultWhite;
    }

    const std::uint32_t declaredMaximum =
        value.declaredBitsPerSample == 0
            ? std::numeric_limits<std::uint32_t>::max()
            : (value.declaredBitsPerSample == 16
                   ? 65535U
                   : ((1U << value.declaredBitsPerSample) - 1U));
    if (source.isDng && !source.hasBlackLevel) {
        const bool defaultBlack = value.blackLevelCommon == 0U &&
            std::all_of(value.blackLevelChannels.begin(),
                        value.blackLevelChannels.begin() + activeLevelChannels,
                        [](std::uint32_t level) { return level == 0U; }) &&
            std::all_of(value.blackPattern.begin(),
                        value.blackPattern.begin() + value.blackPatternCount,
                        [](std::uint32_t level) { return level == 0U; });
        value.blackLevelProvenance = defaultBlack
            ? RawLevelProvenance::DeclaredBitsDefault
            : RawLevelProvenance::DecoderMetadata;
    }
    if (source.isDng && !source.hasWhiteLevel) {
        const bool defaultWhiteLevel = std::all_of(
            value.whiteLevels.begin(),
            value.whiteLevels.begin() + activeLevelChannels,
            [declaredMaximum](std::uint32_t level) {
                return level == declaredMaximum;
            });
        value.whiteLevelProvenance = defaultWhiteLevel
            ? RawLevelProvenance::DeclaredBitsDefault
            : RawLevelProvenance::DecoderMetadata;
    }
    std::uint64_t largestSpan = 0U;
    const std::size_t repeatCount =
        value.blackPatternCount == 0U ? 1U : value.blackPatternCount;
    for (std::size_t channel = 0U; channel < activeLevelChannels; ++channel) {
        if (value.whiteLevels[channel] > declaredMaximum) {
            *failure = "RAW WhiteLevel exceeds declared sample precision";
            return false;
        }
        for (std::size_t cell = 0U; cell < repeatCount; ++cell) {
            const std::uint32_t repeated = value.blackPatternCount == 0U
                ? 0U
                : value.blackPattern[cell];
            std::uint32_t black = 0U;
            if (!addLevels(value.blackLevelCommon,
                           value.blackLevelChannels[channel], repeated, &black) ||
                black >= value.whiteLevels[channel]) {
                *failure = "RAW effective BlackLevel is not below WhiteLevel";
                return false;
            }
            largestSpan = std::max<std::uint64_t>(
                largestSpan,
                static_cast<std::uint64_t>(value.whiteLevels[channel]) - black + 1U);
        }
    }
    value.effectiveBitsPerSample = bitsRequired(largestSpan);
    if (value.effectiveBitsPerSample <= 0 ||
        (value.declaredBitsPerSample != 0 &&
         value.effectiveBitsPerSample > value.declaredBitsPerSample)) {
        *failure = "RAW effective precision contradicts declared precision";
        return false;
    }

    value.baselineExposurePresent =
        source.isDng && source.hasBaselineExposure;
    if (value.baselineExposurePresent) {
        value.baselineExposure = color.dng_levels.baseline_exposure;
        if (!std::isfinite(value.baselineExposure) ||
            value.baselineExposure < -32.0f || value.baselineExposure > 32.0f) {
            *failure = "DNG BaselineExposure is outside the supported bound";
            return false;
        }
    }
    value.linearResponseLimitPresent =
        source.isDng && source.hasLinearResponseLimit;
    if (value.linearResponseLimitPresent) {
        value.linearResponseLimit = color.dng_levels.LinearResponseLimit;
        if (!std::isfinite(value.linearResponseLimit) ||
            value.linearResponseLimit <= 0.0f ||
            value.linearResponseLimit > 1.0f) {
            *failure = "DNG LinearResponseLimit is outside (0,1]";
            return false;
        }
    }

    *descriptor = value;
    return true;
}
#endif

// Reference (target) white for the daylight-base modes. raw_file_processor.py:
//   _DAYLIGHT_REFERENCE_TEMPERATURE = 6504.0
constexpr double kDaylightReferenceTemperature = 6504.0;
// raw_file_processor.py: _TUNGSTEN_TEMPERATURE = 2850.0
constexpr double kTungstenTemperature = 2850.0;
// Public product bounds. 1000 K is intentionally retained for upstream/UI
// compatibility even though it extrapolates below Kang 2002's documented
// physical domain; changing that policy requires a new colour decision.
constexpr double kMinTemperature = 1000.0;
constexpr double kMaxTemperature = 12000.0;
constexpr double kMinTint = 0.2;
constexpr double kMaxTint = 1.8;

// ACES2065-1 (AP0) <-> CIE XYZ (D60-adapted, the colour-science default for this
// colourspace). These are the standard AP0 RGB->XYZ and XYZ->RGB matrices used by
// `colour.RGB_COLOURSPACES["ACES2065-1"]`, which is what raw_file_processor.py uses
// for RGB_to_XYZ / XYZ_to_RGB with chromatic_adaptation_transform=None.
//
// Row-major 3x3.
constexpr double kAcesRgbToXyz[9] = {
    0.9525523959, 0.0000000000,  0.0000936786,
    0.3439664498, 0.7281660966, -0.0721325464,
    0.0000000000, 0.0000000000,  1.0088251844,
};
constexpr double kAcesXyzToRgb[9] = {
     1.0498110175, 0.0000000000, -0.0000974845,
    -0.4959030231, 1.3733130458,  0.0982400361,
     0.0000000000, 0.0000000000,  0.9912520182,
};

// colour-science 0.4.7's default Von-Kries cone transform. The upstream call
// specifies method="Von Kries" without a transform, which resolves to CAT02.
// Row-major 3x3; these constants are locked by raw_wb_cat_vectors.json.
constexpr double kCat02[9] = {
     0.7328, 0.4296, -0.1624,
    -0.7036, 1.6975,  0.0061,
     0.0030, 0.0136,  0.9834,
};

// Linear ACES2065-1 -> linear ProPhoto RGB, the final colourspace step of
// raw_file_processor.py::load_and_process_raw_file (output_colorspace="ProPhoto
// RGB"):
//   colour.RGB_to_RGB(rgb, ACES2065-1, ProPhoto RGB,
//                     apply_cctf_decoding=False, apply_cctf_encoding=False)
// i.e. a pure 3x3 with the default CAT02 chromatic adaptation (ACES white -> D50)
// baked in — NO transfer function. The spektrafilm engine's input space is linear
// ProPhoto RGB, so the decoder emits ProPhoto directly instead of leaving ACES
// pixels to be mis-read as ProPhoto by the engine.
//
// Computed with colour-science 0.4.7 (the oracle's pinned dependency); reproduces
// colour.matrix_RGB_to_RGB to < 5e-11. NB the row sums are intentionally NOT 1
// (1.00018 / 0.99996 / 1.00027) — that is exactly what colour's CAT02 produces, so
// matching the oracle means using these values verbatim. Row-major 3x3.
constexpr double kAcesToProPhoto[9] = {
     1.2393803417847302,  -0.16396782280140051,  -0.075233383798369968,
     0.0036113618663812341, 1.0896136492217019,   -0.093265792081978632,
    -0.00205967931567552,  -0.0022515883414713734, 1.0045855773288515,
};

// Keep every accumulation step explicit. The production target and the host
// golden target also compile this translation unit with FP contraction disabled;
// together those choices preserve the Python/NumPy double-operation order before
// the declared float32 cast boundaries.
double dot3(const double* lhs, const double* rhs, size_t rhsStride) {
    double sum = 0.0;
    sum += lhs[0] * rhs[0 * rhsStride];
    sum += lhs[1] * rhs[1 * rhsStride];
    sum += lhs[2] * rhs[2 * rhsStride];
    return sum;
}

void mat3MulVec(const double m[9], const double v[3], double out[3]) {
    out[0] = dot3(m + 0, v, 1);
    out[1] = dot3(m + 3, v, 1);
    out[2] = dot3(m + 6, v, 1);
}

void mat3Mul(const double lhs[9], const double rhs[9], double out[9]) {
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            out[row * 3 + column] =
                dot3(lhs + row * 3, rhs + column, 3);
        }
    }
}

void mat3Inverse(const double m[9], double out[9]) {
    const double a = m[0], b = m[1], c = m[2];
    const double d = m[3], e = m[4], f = m[5];
    const double g = m[6], h = m[7], i = m[8];
    const double determinant =
        a * (e * i - f * h) - b * (d * i - f * g) +
        c * (d * h - e * g);
    const double inverseDeterminant = 1.0 / determinant;
    out[0] = (e * i - f * h) * inverseDeterminant;
    out[1] = (c * h - b * i) * inverseDeterminant;
    out[2] = (b * f - c * e) * inverseDeterminant;
    out[3] = (f * g - d * i) * inverseDeterminant;
    out[4] = (a * i - c * g) * inverseDeterminant;
    out[5] = (c * d - a * f) * inverseDeterminant;
    out[6] = (d * h - e * g) * inverseDeterminant;
    out[7] = (b * g - a * h) * inverseDeterminant;
    out[8] = (a * e - b * d) * inverseDeterminant;
}

bool numpyAllclose(const double lhs[3], const double rhs[3]) {
    for (size_t i = 0; i < 3; ++i) {
        // NumPy defaults: atol=1e-8, rtol=1e-5. rtol is applied to the
        // second operand, so the argument order is part of the oracle contract.
        if (std::fabs(lhs[i] - rhs[i]) >
            1.0e-8 + 1.0e-5 * std::fabs(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool numpyIsclose(double lhs, double rhs) {
    return std::fabs(lhs - rhs) <=
           1.0e-8 + 1.0e-5 * std::fabs(rhs);
}

void buildCat02Matrix(const double sourceWhite[3],
                      const double targetWhite[3], double out[9]) {
    double sourceCone[3], targetCone[3];
    mat3MulVec(kCat02, sourceWhite, sourceCone);
    mat3MulVec(kCat02, targetWhite, targetCone);

    const double diagonal[9] = {
        targetCone[0] / sourceCone[0], 0.0, 0.0,
        0.0, targetCone[1] / sourceCone[1], 0.0,
        0.0, 0.0, targetCone[2] / sourceCone[2],
    };
    double inverse[9], inverseTimesDiagonal[9];
    mat3Inverse(kCat02, inverse);
    mat3Mul(inverse, diagonal, inverseTimesDiagonal);
    mat3Mul(inverseTimesDiagonal, kCat02, out);
}

bool aces2065ToProPhotoRGBImpl(float* rgb, size_t pixelCount,
                               const std::atomic<bool>* cancelFlag) {
    // Pure matrix (kAcesToProPhoto), no transfer function. Accumulate in double
    // for parity with the float64 numpy/colour path, then store float32.
    // Deliberately do not clamp: gamut handling is a separate pipeline stage.
    for (size_t i = 0; i < pixelCount; ++i) {
        if ((i & 4095U) == 0U && cancelFlag != nullptr &&
            cancelFlag->load(std::memory_order_acquire)) {
            return false;
        }
        const double in[3] = {rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]};
        double out[3];
        mat3MulVec(kAcesToProPhoto, in, out);
        rgb[i * 3 + 0] = static_cast<float>(out[0]);
        rgb[i * 3 + 1] = static_cast<float>(out[1]);
        rgb[i * 3 + 2] = static_cast<float>(out[2]);
    }
    return cancelFlag == nullptr ||
           !cancelFlag->load(std::memory_order_acquire);
}

// CIE daylight locus chromaticity for T >= 4000 K (matches colour's
// 'CIE Illuminant D Series' path used by _whitepoint_xyz_from_temperature).
void daylightXy(double t, double& x, double& y) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    if (t <= 7000.0) {
        x = -4.607e9 / t3;
        x += 2.9678e6 / t2;
        x += 0.09911e3 / t;
        x += 0.244063;
    } else {
        x = -2.0064e9 / t3;
        x += 1.9018e6 / t2;
        x += 0.24748e3 / t;
        x += 0.237040;
    }
    const double x2 = x * x;
    y = -3.0 * x2;
    y += 2.87 * x;
    y -= 0.275;
}

// Kang 2002 Planckian approximation for T < 4000 K (matches colour's 'Kang 2002').
void kang2002Xy(double t, double& x, double& y) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    if (t <= 4000.0) {
        x = -0.2661239e9 / t3;
        x -= 0.2343589e6 / t2;
        x += 0.8776956e3 / t;
        x += 0.179910;
    } else {
        x = -3.0258469e9 / t3;
        x += 2.1070379e6 / t2;
        x += 0.2226347e3 / t;
        x += 0.240390;
    }
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (t <= 2222.0) {
        y = -1.1063814 * x3;
        y -= 1.34811020 * x2;
        y += 2.18555832 * x;
        y -= 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3;
        y -= 1.37418593 * x2;
        y += 2.09137015 * x;
        y -= 0.16748867;
    } else {
        y = 3.0817580 * x3;
        y -= 5.87338670 * x2;
        y += 3.75112997 * x;
        y -= 0.37001483;
    }
}

}  // namespace

void whitepointXyzFromTemperature(double temperatureK, double outXyz[3]) {
    double x, y;
    // raw_file_processor.py: 'CIE Illuminant D Series' if T >= 4000 else 'Kang 2002'.
    if (temperatureK >= 4000.0) {
        daylightXy(temperatureK, x, y);
    } else {
        kang2002Xy(temperatureK, x, y);
    }
    // xy -> XYZ with Y = 1 (colour.xy_to_XYZ default).
    outXyz[0] = x / y;
    outXyz[1] = 1.0;
    outXyz[2] = (1.0 - x - y) / y;
}

bool rawWhiteBalanceOptionsValid(const DecodeOptions& options) noexcept {
    switch (options.whiteBalance) {
        case WhiteBalanceMode::AsShot:
        case WhiteBalanceMode::Daylight:
        case WhiteBalanceMode::Tungsten:
        case WhiteBalanceMode::Custom:
            break;
        default:
            return false;
    }
    return std::isfinite(options.temperatureK) &&
           options.temperatureK >= kMinTemperature &&
           options.temperatureK <= kMaxTemperature &&
           std::isfinite(options.tint) &&
           options.tint >= kMinTint && options.tint <= kMaxTint;
}

bool applyAcesWhiteBalance(float* rgb, size_t pixelCount,
                           const DecodeOptions& options) {
    if (!rawWhiteBalanceOptionsValid(options) ||
        (rgb == nullptr && pixelCount != 0U)) {
        return false;
    }
    if (cancellationRequested(options)) return false;

    // As-shot/daylight ownership stays entirely with LibRaw. Do not even run an
    // identity matrix: these modes must remain byte-for-byte unchanged.
    if (options.whiteBalance == WhiteBalanceMode::AsShot ||
        options.whiteBalance == WhiteBalanceMode::Daylight) {
        return true;
    }

    double temperatureK = options.temperatureK;
    double tint = options.tint;
    if (options.whiteBalance == WhiteBalanceMode::Tungsten) {
        temperatureK = kTungstenTemperature;
        tint = 1.0;
    }

    double sourceWhite[3], targetWhite[3];
    whitepointXyzFromTemperature(temperatureK, sourceWhite);
    whitepointXyzFromTemperature(kDaylightReferenceTemperature, targetWhite);

    // Match np.allclose(target_white, source_white) exactly, including operand
    // order. A near-reference source skips the CAT but can still receive tint.
    const bool skipCat = numpyAllclose(targetWhite, sourceWhite);
    const bool skipTint = numpyIsclose(tint, 1.0);
    double cat02[9]{};
    if (!skipCat) buildCat02Matrix(sourceWhite, targetWhite, cat02);
    const float tint32 = static_cast<float>(tint);

    for (size_t i = 0; i < pixelCount; ++i) {
        if ((i & 4095U) == 0U && cancellationRequested(options)) return false;
        float adapted[3] = {
            rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2],
        };
        if (!skipCat) {
            const double in[3] = {
                static_cast<double>(adapted[0]),
                static_cast<double>(adapted[1]),
                static_cast<double>(adapted[2]),
            };
            double xyz[3], adaptedXyz[3], out[3];
            mat3MulVec(kAcesRgbToXyz, in, xyz);
            mat3MulVec(cat02, xyz, adaptedXyz);
            mat3MulVec(kAcesXyzToRgb, adaptedXyz, out);

            // First declared oracle boundary: CAT output is float32.
            adapted[0] = static_cast<float>(out[0]);
            adapted[1] = static_cast<float>(out[1]);
            adapted[2] = static_cast<float>(out[2]);
        }
        if (!skipTint) {
            // Second declared oracle boundary: multiply float32 CAT output by a
            // float32 tint vector, then round the product back to float32.
            adapted[1] = static_cast<float>(adapted[1] * tint32);
        }
        rgb[i * 3 + 0] = adapted[0];
        rgb[i * 3 + 1] = adapted[1];
        rgb[i * 3 + 2] = adapted[2];
    }
    return !cancellationRequested(options);
}

bool buildAcesWbMultiplier(const DecodeOptions& options, float outMul[3]) {
    outMul[0] = outMul[1] = outMul[2] = 1.0f;
    return applyAcesWhiteBalance(outMul, 1U, options);
}

void aces2065ToProPhotoRGB(float* rgb, size_t pixelCount) {
    // Pure matrix (kAcesToProPhoto), no transfer function — matching
    // raw_file_processor.py's final colour.RGB_to_RGB step. Accumulate in double
    // for parity with the float64 numpy/colour path, then store float32.
    // Deliberately does NOT clamp: ACES2065-1 (AP0) is wider than ProPhoto, so
    // saturated colours can map slightly out of gamut (negative / >1), exactly as
    // the oracle leaves them — gamut handling is a separate, opt-in pipeline stage.
    (void)aces2065ToProPhotoRGBImpl(rgb, pixelCount, nullptr);
}

#if SFRAW_HAVE_LIBRAW

namespace {

// Apply the desktop-compatible postprocess option contract (RAW_DNG.md).
// Exact output parity also requires the same qualified LibRaw version (#189).
//   output_color=6 (ACES), output_bps=16, no_auto_bright=1, gamm[0]=gamm[1]=1.0,
//   use_camera_wb for as-shot.
// When options.halfSize is true, also sets half_size=1 so LibRaw averages each
// 2x2 Bayer cell into one pixel instead of running full demosaic interpolation.
// This produces an image at ~half the linear dimensions (quarter the pixel count),
// reducing peak memory by ~75% and substantially cutting decode time — intended
// for fast proxy/preview decodes of large RAW/DNG files. half_size=0 is the
// LibRaw default; explicitly setting it here keeps the full-res path unchanged
// even if imgdata.params was not zero-initialized by the caller.
void applyParityParams(LibRaw& raw, const DecodeOptions& options) {
    auto& p = raw.imgdata.params;
    p.output_color   = 6;     // 6 == ACES, matches rawpy.ColorSpace.ACES
    p.output_bps     = 16;
    p.no_auto_bright = 1;
    p.gamm[0]        = 1.0;   // gamma (1,1) -> linear
    p.gamm[1]        = 1.0;
    // Built-in wavelet denoise is outside Spektrafilm's parity contract. Pinning
    // zero makes the default explicit and keeps that postprocess path unreachable.
    p.threshold      = 0.0f;

    // Half-size proxy decode: set to 1 for fast low-memory decode, 0 for full-res.
    // Explicitly writing 0 in the full-res path is defensive — LibRaw default-
    // constructs params.half_size = 0, but being explicit ensures correctness if
    // the LibRaw instance is ever reused or partially re-initialized by a caller.
    p.half_size = options.halfSize ? 1 : 0;

    if (options.whiteBalance == WhiteBalanceMode::AsShot) {
        p.use_camera_wb = 1;
    } else {
        // daylight / tungsten / custom: LibRaw daylight-balanced base output, then
        // the colour-science adaptation is applied below (matches the Python, which
        // leaves use_camera_wb off and adapts in ACES afterwards).
        p.use_camera_wb = 0;
        // TODO(libraw): if a vendored LibRaw build defaults to auto-WB, force the
        // daylight multipliers explicitly via p.user_mul / raw.imgdata.color.pre_mul
        // so the base matches rawpy's daylight default exactly.
    }
}

// Run unpack + dcraw_process + dcraw_make_mem_image and copy the 16-bit linear RGB
// into a normalized float result (value / 65535), matching the Python:
//   rgb = raw.postprocess(...).astype(float32) / 65535.0
//
// `srcData`/`srcLen` point at the still-readable source bytes (the same buffer
// LibRaw opened) so a failed unpack() of a compressed DNG can be classified
// (deflate vs lossy-JPEG) for a precise, actionable error code.
DecodeResult finishDecode(LibRaw& raw, const DecodeOptions& options,
                          const uint8_t* srcData, size_t srcLen,
                          std::chrono::steady_clock::time_point decodeStartedAt,
                          double inputIoMs, double openMs) {
    DecodeResult result;

    if (cancellationRequested(options)) return cancellationFailure();

    const dngsniff::PrimaryImageInfo source = dngsniff::primaryImageInfoOf(
        srcData, srcLen, raw.imgdata.sizes.raw_width,
        raw.imgdata.sizes.raw_height);
    if (source.isDng &&
        (raw.imgdata.sizes.raw_width == 0U ||
         raw.imgdata.sizes.raw_height == 0U)) {
        return precisionMetadataFailure(
            "DNG has no identified RAW geometry for exact IFD binding");
    }
    const char* precisionFailure = nullptr;
    int precisionFailureStatus = SFRAW_ERR_PRECISION_METADATA;
    if (!preflightSourcePrecision(source, &precisionFailure,
                                  &precisionFailureStatus)) {
        return precisionMetadataFailure(precisionFailure,
                                        precisionFailureStatus);
    }
    if (!postIdentifyLinearRawColorIsQualified(raw, source)) {
        return precisionMetadataFailure(
            "DNG LinearRaw color matrices did not retain every admitted sensor plane after identify");
    }

    // Intra-decode phase timers (#158). `phaseMs()` returns the delta since the last
    // call and re-stamps, so the phases are contiguous by construction and cannot
    // double-count. Cost is a handful of steady_clock reads against a full decode.
    // maybe_unused because the line they feed is Android-only and this file also
    // compiles for the host test.
    using phase_clk = std::chrono::steady_clock;
    auto phaseAt = phase_clk::now();
    auto phaseMs = [&phaseAt]() {
        auto now = phase_clk::now();
        double d = std::chrono::duration<double, std::milli>(now - phaseAt).count();
        phaseAt = now;
        return d;
    };
    [[maybe_unused]] double mUnpack = 0, mProcess = 0, mMemImg = 0,
                            mAllocate = 0, mCopy = 0, mAdapt = 0, mColour = 0;

    int rc = raw.unpack();
    mUnpack = phaseMs();
    if (rc == LIBRAW_CANCELLED_BY_CALLBACK) return cancellationFailure(rc);
    if (cancellationRequested(options)) return cancellationFailure();
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        result.status = isMemoryLimitFailure(rc)
            ? SFRAW_ERR_NO_MEMORY
            : dngsniff::classifyUnpackFailure(srcData, srcLen);
        // Name the specific compression in the message for diagnosability.
        bool isDng = false;
        int comp = dngsniff::compressionOf(srcData, srcLen, &isDng);
        std::string where = isDng ? std::string(" [DNG compression: ") +
                                        dngCompressionName(comp) + "]"
                                  : std::string();
        const char* hint = "";
        if (result.status == SFRAW_ERR_LOSSY_JPEG_DNG)
            hint = " [lossy-JPEG DNG (e.g. Samsung Expert RAW); this build has no "
                   "libjpeg — fall back to platform ImageDecoder]";
        else if (result.status == SFRAW_ERR_JPEGXL_DNG)
            hint = " [JPEG-XL DNG; this build has no libjxl/dngsdk — fall back to "
                   "platform ImageDecoder]";
        else if (result.status == SFRAW_ERR_DEFLATE_DNG)
            hint = " [deflate DNG layout unsupported by pinned LibRaw (integer "
                   "samples) or zlib is disabled; use the platform fallback]";
        result.error = std::string("LibRaw unpack() failed: ") +
                       libraw_strerror(rc) + where + hint;
        return result;
    }
    // Some loaders finalize or replace image geometry during unpack(). Re-run
    // the same overflow-safe raw/visible/margin/aspect guard before any
    // dcraw_process() allocation instead of trusting identify-time dimensions.
    if (declaredDecodeDimensionsExceedLimit(raw, options)) {
        return dimensionLimitFailure(options);
    }
    const dngsniff::PrimaryImageInfo finalizedSource =
        dngsniff::primaryImageInfoOf(srcData, srcLen,
                                     raw.imgdata.sizes.raw_width,
                                     raw.imgdata.sizes.raw_height);
    if (source.isDng &&
        (finalizedSource.metadataAmbiguous || !finalizedSource.isDng ||
         finalizedSource.selectedIfdOffset != source.selectedIfdOffset)) {
        return precisionMetadataFailure(
            "DNG decoded RAW IFD identity changed or became ambiguous after unpack");
    }
    if (!preflightSourcePrecision(finalizedSource, &precisionFailure,
                                  &precisionFailureStatus)) {
        return precisionMetadataFailure(precisionFailure,
                                        precisionFailureStatus);
    }
    // Several vendor loaders finalize raw_bps/black/maximum/CFA metadata only
    // while unpacking. Publish from that finalized state, but still before
    // dcraw_process mutates levels or quantizes into its processed bitmap.
    if (!buildPrecisionDescriptor(raw, finalizedSource, options,
                                  &result.descriptor,
                                  &precisionFailure)) {
        return precisionMetadataFailure(precisionFailure);
    }
    applyParityParams(raw, options);
    if (cancellationRequested(options)) return cancellationFailure();
    phaseMs();  // param setup is not decode work — keep it out of `process`
    rc = raw.dcraw_process();
    mProcess = phaseMs();
    if (rc == LIBRAW_CANCELLED_BY_CALLBACK) return cancellationFailure(rc);
    if (cancellationRequested(options)) return cancellationFailure();
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        result.status = isMemoryLimitFailure(rc)
            ? SFRAW_ERR_NO_MEMORY
            : SFRAW_ERR_PROCESS;
        result.error =
            std::string("LibRaw dcraw_process() failed: ") + libraw_strerror(rc);
        return result;
    }

    int status = LIBRAW_SUCCESS;
    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&status);
    mMemImg = phaseMs();
    if (status == LIBRAW_CANCELLED_BY_CALLBACK) {
        if (img) LibRaw::dcraw_clear_mem(img);
        return cancellationFailure(status);
    }
    if (img == nullptr || status != LIBRAW_SUCCESS) {
        if (img) LibRaw::dcraw_clear_mem(img);
        result.librawCode = status;
        result.status = isMemoryLimitFailure(status)
            ? SFRAW_ERR_NO_MEMORY
            : SFRAW_ERR_PROCESS;
        result.error = std::string("LibRaw dcraw_make_mem_image() failed: ") +
                       libraw_strerror(status);
        return result;
    }
    auto clearImage = [](libraw_processed_image_t* owned) {
        if (owned != nullptr) LibRaw::dcraw_clear_mem(owned);
    };
    std::unique_ptr<libraw_processed_image_t, decltype(clearImage)> image(
        img, clearImage);
    if (cancellationRequested(options)) return cancellationFailure();
    if (image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3 ||
        (image->bits != 8 && image->bits != 16)) {
        result.status = SFRAW_ERR_FORMAT;
        result.error =
            "unexpected LibRaw image format (expected 8/16-bit 3-channel bitmap)";
        return result;
    }
    if (image->bits < result.descriptor.effectiveBitsPerSample) {
        result.status = SFRAW_ERR_PRECISION_METADATA;
        result.error =
            "LibRaw processed bitmap precision would truncate the effective RAW range";
        return result;
    }
    result.descriptor.processedBitsPerSample = image->bits;

    const int fullW = image->width;
    const int fullH = image->height;
    if (fullW <= 0 || fullH <= 0 ||
        pixelProductExceeds(static_cast<uint64_t>(fullW),
                            static_cast<uint64_t>(fullH),
                            kMaxFullDecodePixels)) {
        return dimensionLimitFailure(options);
    }
    const std::size_t fullPixels =
        static_cast<std::size_t>(fullW) * static_cast<std::size_t>(fullH);
    const std::size_t bytesPerSample =
        image->bits == 16 ? sizeof(std::uint16_t) : sizeof(std::uint8_t);
    if (fullPixels > std::numeric_limits<std::size_t>::max() / 3U ||
        fullPixels * 3U > std::numeric_limits<std::size_t>::max() / bytesPerSample ||
        static_cast<std::size_t>(image->data_size) <
            fullPixels * 3U * bytesPerSample) {
        result.status = SFRAW_ERR_FORMAT;
        result.error = "LibRaw processed bitmap has a truncated/overflowing stride";
        return result;
    }
    const auto* src16 = reinterpret_cast<const uint16_t*>(image->data);
    const auto* src8 = reinterpret_cast<const uint8_t*>(image->data);
    const float processedScale = image->bits == 16
        ? (1.0f / 65535.0f)
        : (1.0f / 255.0f);

    // Cap the longest edge to options.maxLongEdge (proxy bound) DURING the uint16->float
    // copy: subsample img->data straight into the final-sized buffer so we never hold a
    // second full-resolution float image. LibRaw's half_size is honoured for most Bayer
    // DNGs, but some (e.g. certain Samsung Expert-RAW DNGs) decode full-resolution
    // regardless; without this cap result.rgb stays full-res and the returned direct
    // buffer plus downstream copies can exhaust the process memory budget. Peak native
    // memory here is LibRaw's own image + the capped buffer, not two full-res copies.
    int step = 1;
    {
        const int longest = fullW > fullH ? fullW : fullH;
        if (options.maxLongEdge > 0 && longest > options.maxLongEdge) {
            step = static_cast<int>(
                (static_cast<int64_t>(longest) + options.maxLongEdge - 1) /
                options.maxLongEdge);
        }
    }
    const int ow = step > 1 ? (fullW + step - 1) / step : fullW;
    const int oh = step > 1 ? (fullH + step - 1) / step : fullH;
    result.descriptor.outputSubsampleStep = step;
    result.width = ow;
    result.height = oh;
    result.rgb.allocateUninitialized(static_cast<size_t>(ow) * oh * 3U);
    mAllocate = phaseMs();
    for (int oy = 0; oy < oh; ++oy) {
        if (cancellationRequested(options)) return cancellationFailure();
        const size_t srow = static_cast<size_t>(oy) * step * fullW;
        for (int ox = 0; ox < ow; ++ox) {
            const size_t si = (srow + static_cast<size_t>(ox) * step) * 3;
            const size_t di = (static_cast<size_t>(oy) * ow + ox) * 3;
            if (image->bits == 16) {
                result.rgb[di] = static_cast<float>(src16[si]) * processedScale;
                result.rgb[di + 1] =
                    static_cast<float>(src16[si + 1]) * processedScale;
                result.rgb[di + 2] =
                    static_cast<float>(src16[si + 2]) * processedScale;
            } else {
                result.rgb[di] = static_cast<float>(src8[si]) * processedScale;
                result.rgb[di + 1] =
                    static_cast<float>(src8[si + 1]) * processedScale;
                result.rgb[di + 2] =
                    static_cast<float>(src8[si + 2]) * processedScale;
            }
        }
    }
    mCopy = phaseMs();
    image.reset();

    if (!applyAcesWhiteBalance(result.rgb.data(), static_cast<size_t>(ow) * oh,
                               options)) {
        return cancellationFailure();
    }
    mAdapt = phaseMs();

    // Final colourspace conversion: linear ACES2065-1 -> linear ProPhoto RGB, the
    // engine's input space (raw_file_processor.py's output_colorspace step). Done
    // AFTER the ACES-space white-balance adaptation, matching the oracle order.
    // Without this the buffer was tagged ACES but read as ProPhoto downstream
    // (spektra_jni hardcodes the engine input to ProPhoto), so every native RAW
    // decode ran through the wrong primaries.
    if (!aces2065ToProPhotoRGBImpl(result.rgb.data(),
                                   static_cast<size_t>(ow) * oh,
                                   options.cancelFlag)) {
        return cancellationFailure();
    }
    mColour = phaseMs();
    if (cancellationRequested(options)) return cancellationFailure();

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "sfraw",
        "decoded %dx%d (halfSize=%d) -> %dx%d (maxLongEdge=%d, step=%d)",
        fullW, fullH, options.halfSize ? 1 : 0, ow, oh, options.maxLongEdge, step);
    __android_log_print(ANDROID_LOG_INFO, "sfraw",
        "decode phases ms: io=%.3f open=%.3f unpack=%.3f process=%.3f "
        "memimg=%.3f output_alloc=%.3f copy=%.3f adapt=%.3f colour=%.3f "
        "total=%.3f",
        inputIoMs, openMs, mUnpack, mProcess, mMemImg, mAllocate, mCopy,
        mAdapt, mColour,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decodeStartedAt).count());
#endif

    result.colorSpace = "ProPhoto RGB";
    result.status = SFRAW_OK;
    result.ok = true;
    return result;
}

}  // namespace

DecodeResult decodeFromBuffer(const uint8_t* data, size_t length,
                              const DecodeOptions& options) try {
    const auto decodeStartedAt = std::chrono::steady_clock::now();
    DecodeResult result;
    if (!rawWhiteBalanceOptionsValid(options)) {
        return invalidWhiteBalanceFailure();
    }
    if (cancellationRequested(options)) return cancellationFailure();
    if (data == nullptr || length == 0) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "empty input buffer";
        return result;
    }
    if (length > kMaxEncodedInputBytes) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "RAW input exceeds 64 MiB safety limit";
        return result;
    }
    if (!preflightLibRawOpen(data, length, &result)) return result;
    LibRaw raw;
    applyInputLimits(raw);
    applyCancellationHandler(raw, options);
    const auto openAt = std::chrono::steady_clock::now();
#if defined(SFRAW_TESTING)
    sfraw::test::notifyLibRawOpenAttemptForTest();
#endif
    int rc = raw.open_buffer(const_cast<uint8_t*>(data), length);
    const double openMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - openAt).count();
    if (rc == LIBRAW_CANCELLED_BY_CALLBACK) return cancellationFailure(rc);
    if (cancellationRequested(options)) return cancellationFailure();
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        const int codecFallback = dngsniff::classifyOpenFailure(data, length);
        result.status = isMemoryLimitFailure(rc) ? SFRAW_ERR_NO_MEMORY
            : (codecFallback != SFRAW_ERR_UNPACK ? codecFallback
            : (rc == LIBRAW_FILE_UNSUPPORTED ? SFRAW_ERR_FILE_UNSUPPORTED
                                              : SFRAW_ERR_OPEN));
        result.error = std::string("LibRaw open_buffer() failed: ") +
                       libraw_strerror(rc) + " (unsupported or corrupt RAW)";
        return result;
    }
    if (declaredDecodeDimensionsExceedLimit(raw, options)) {
        return dimensionLimitFailure(options);
    }
    return finishDecode(raw, options, data, length, decodeStartedAt, 0.0,
                        openMs);
} catch (const std::bad_alloc&) {
    DecodeResult result;
    result.status = SFRAW_ERR_NO_MEMORY;
    result.error = "out of memory";
    return result;
} catch (...) {
    DecodeResult result;
    result.status = SFRAW_ERR_UNKNOWN;
    result.error = "decode failed";
    return result;
}

DecodeResult decodeFromFd(int fd, const DecodeOptions& options) try {
    const auto decodeStartedAt = std::chrono::steady_clock::now();
    DecodeResult result;
    if (!rawWhiteBalanceOptionsValid(options)) {
        return invalidWhiteBalanceFailure();
    }
    if (cancellationRequested(options)) return cancellationFailure();
    if (fd < 0) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "invalid file descriptor";
        return result;
    }
    LibRaw raw;
    applyInputLimits(raw);
    applyCancellationHandler(raw, options);
    // LibRaw can read from a FILE*; we dup the fd so the caller keeps ownership.
    int dup_fd = ::dup(fd);
    if (dup_fd < 0) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "dup(fd) failed";
        return result;
    }
    FILE* openedFile = ::fdopen(dup_fd, "rb");
    if (openedFile == nullptr) {
        ::close(dup_fd);
        result.status = SFRAW_ERR_INPUT;
        result.error = "fdopen() failed";
        return result;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> fp(openedFile, &std::fclose);
    // Keep the existing open_buffer parity path for this ticket, but read in
    // bounded chunks. This supports descriptors without a trustworthy stat size
    // and rejects over-limit files before growing the vector past the ceiling.
    std::vector<uint8_t> bytes;
    const auto ioAt = std::chrono::steady_clock::now();
    std::array<uint8_t, 64U * 1024U> chunk{};
    for (;;) {
        if (cancellationRequested(options)) return cancellationFailure();
        const size_t read =
            std::fread(chunk.data(), 1, chunk.size(), fp.get());
        if (cancellationRequested(options)) return cancellationFailure();
        if (read > 0) {
            if (read > kMaxEncodedInputBytes - bytes.size()) {
                result.status = SFRAW_ERR_INPUT;
                result.error = "RAW input exceeds 64 MiB safety limit";
                return result;
            }
            bytes.insert(bytes.end(), chunk.data(), chunk.data() + read);
        }
        if (read < chunk.size()) {
            if (std::ferror(fp.get())) {
                result.status = SFRAW_ERR_INPUT;
                result.error = "failed to read RAW from fd";
                return result;
            }
            break;
        }
    }
    fp.reset();  // closes only the duplicate; caller retains ownership
    // Only the fd path has input I/O: decodeFromBuffer receives caller-owned bytes.
    const double inputIoMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - ioAt).count();
    if (bytes.empty()) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "failed to read RAW from fd";
        return result;
    }
    if (!preflightLibRawOpen(bytes.data(), bytes.size(), &result)) return result;
    const auto openAt = std::chrono::steady_clock::now();
#if defined(SFRAW_TESTING)
    sfraw::test::notifyLibRawOpenAttemptForTest();
#endif
    int rc = raw.open_buffer(bytes.data(), bytes.size());
    const double openMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - openAt).count();
    if (rc == LIBRAW_CANCELLED_BY_CALLBACK) return cancellationFailure(rc);
    if (cancellationRequested(options)) return cancellationFailure();
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        const int codecFallback =
            dngsniff::classifyOpenFailure(bytes.data(), bytes.size());
        result.status = isMemoryLimitFailure(rc) ? SFRAW_ERR_NO_MEMORY
            : (codecFallback != SFRAW_ERR_UNPACK ? codecFallback
            : (rc == LIBRAW_FILE_UNSUPPORTED ? SFRAW_ERR_FILE_UNSUPPORTED
                                              : SFRAW_ERR_OPEN));
        result.error = std::string("LibRaw open_buffer() failed: ") +
                       libraw_strerror(rc) + " (unsupported or corrupt RAW)";
        return result;
    }
    if (declaredDecodeDimensionsExceedLimit(raw, options)) {
        return dimensionLimitFailure(options);
    }
    return finishDecode(raw, options, bytes.data(), bytes.size(),
                        decodeStartedAt, inputIoMs, openMs);
} catch (const std::bad_alloc&) {
    DecodeResult result;
    result.status = SFRAW_ERR_NO_MEMORY;
    result.error = "out of memory";
    return result;
} catch (...) {
    DecodeResult result;
    result.status = SFRAW_ERR_UNKNOWN;
    result.error = "decode failed";
    return result;
}

#else  // !SFRAW_HAVE_LIBRAW

// Headerless fallback retained only for lightweight source-level tests that
// intentionally compile this file without the native dependency. Production
// CMake fails during configure before a libsfraw.so can take this branch.

DecodeResult decodeFromBuffer(const uint8_t*, size_t,
                              const DecodeOptions& options) {
    DecodeResult result;
    if (!rawWhiteBalanceOptionsValid(options)) {
        result.status = SFRAW_ERR_INPUT;
        result.error =
            "invalid RAW white balance: temperature must be finite and in "
            "[1000,12000] K; tint must be finite and in [0.2,1.8]";
        return result;
    }
    result.status = SFRAW_ERR_UNKNOWN;
    result.error = "LibRaw unavailable in this non-production test build";
    return result;
}

DecodeResult decodeFromFd(int, const DecodeOptions& options) {
    DecodeResult result;
    if (!rawWhiteBalanceOptionsValid(options)) {
        result.status = SFRAW_ERR_INPUT;
        result.error =
            "invalid RAW white balance: temperature must be finite and in "
            "[1000,12000] K; tint must be finite and in [0.2,1.8]";
        return result;
    }
    result.status = SFRAW_ERR_UNKNOWN;
    result.error = "LibRaw unavailable in this non-production test build";
    return result;
}

#endif  // SFRAW_HAVE_LIBRAW

}  // namespace spectrafilm
