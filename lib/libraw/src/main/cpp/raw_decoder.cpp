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
// SampleFormat=3 floating-point deflate subset (Compression 8) decode natively.
// Integer/linear deflate is a typed fallback in pinned LibRaw 0.22.2. LibRaw's
// internal lossless_jpeg_load_raw handles tag 7 with no libjpeg, so tag 7 must
// never be classified as an unsupported-codec fallback.
//
// Many mobile/Pixel DNGs put a large JPEG *preview* in IFD0 and the real raw
// plane in a SubIFD; the IFD walk below (SubIFDs + next-IFD chain, picking the
// largest non-reduced image) selects the raw plane so a preview is never
// mistaken for the raw compression. Deliberately small and tolerant: any parse
// trouble -> unknown.
namespace dngsniff {

enum Compression {
    kUnknown = -1,
    kNone = 1,            // uncompressed -> decodes natively
    kLossyJpegOld = 6,    // old-style JPEG (lossy) -> needs libjpeg (fallback)
    kLosslessJpeg = 7,    // lossless JPEG / LJ92 -> decodes natively (internal)
    kDeflate = 8,         // float ZIP/DEFLATE -> native with USE_ZLIB
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

inline void scanIfd(const Reader& r, uint32_t ifdOff, bool& isDng,
                    int& bestComp, int& bestSampleFormat,
                    uint64_t& bestPx, int depth,
                    unsigned& remainingIfds) {
    if (ifdOff == 0 || depth > 4 || remainingIfds == 0 ||
        !r.in(ifdOff, 2)) return;
    --remainingIfds;
    uint16_t entries = r.u16(ifdOff);
    if (entries == 0 || entries > 512) return;

    int comp = kUnknown;
    int sampleFormat = 1;  // TIFF default: unsigned integer.
    uint32_t width = 0, height = 0, newSubfileType = 0;
    uint32_t subifds[16];
    int subCount = 0;

    for (uint16_t i = 0; i < entries; ++i) {
        size_t e = (size_t)ifdOff + 2 + (size_t)i * 12;
        if (!r.in(e, 12)) return;
        uint16_t tag = r.u16(e);
        uint16_t type = r.u16(e + 2);
        uint32_t count = r.u32(e + 4);
        size_t valOff = e + 8;
        auto scalar = [&]() -> uint32_t {
            return (type == 3 /*SHORT*/) ? r.u16(valOff) : r.u32(valOff);
        };
        switch (tag) {
            case 0x00FE: newSubfileType = scalar(); break;  // NewSubfileType
            case 0x0100: width = scalar(); break;           // ImageWidth
            case 0x0101: height = scalar(); break;          // ImageLength
            case 0x0103: comp = (int)scalar(); break;       // Compression
            case 0x0153: sampleFormat = (int)scalar(); break; // SampleFormat
            case 0xC612: isDng = true; break;               // DNGVersion
            case 0x014A:                                    // SubIFDs
                if ((type == 4 || type == 3) && count <= 16) {
                    if (count == 1) {
                        if (subCount < 16) subifds[subCount++] = scalar();
                    } else {
                        uint32_t arr = r.u32(valOff);
                        for (uint32_t k = 0; k < count && subCount < 16; ++k)
                            subifds[subCount++] = r.u32((size_t)arr + k * 4);
                    }
                }
                break;
            default: break;
        }
    }

    uint64_t px = (uint64_t)width * height;
    bool reduced = (newSubfileType & 1) != 0;  // reduced-resolution preview
    if (!reduced && comp != kUnknown && px >= bestPx) {
        bestPx = px;
        bestComp = comp;
        bestSampleFormat = sampleFormat;
    }
    for (int s = 0; s < subCount; ++s)
        scanIfd(r, subifds[s], isDng, bestComp, bestSampleFormat, bestPx,
                depth + 1,
                remainingIfds);

    uint32_t next = r.u32((size_t)ifdOff + 2 + (size_t)entries * 12);
    if (next > ifdOff)
        scanIfd(r, next, isDng, bestComp, bestSampleFormat, bestPx, depth,
                remainingIfds);
}

struct PrimaryImageInfo {
    int compression = kUnknown;
    int sampleFormat = 1;
    bool isDng = false;
};

inline PrimaryImageInfo primaryImageInfoOf(const uint8_t* data, size_t len) {
    PrimaryImageInfo info;
    if (data == nullptr || len < 8) return info;
    Reader r;
    r.p = data;
    r.n = len;
    if (data[0] == 'I' && data[1] == 'I') r.be = false;
    else if (data[0] == 'M' && data[1] == 'M') r.be = true;
    else return info;
    uint64_t bestPx = 0;
    // Bound both recursive SubIFDs and the otherwise attacker-controlled
    // next-IFD chain. This parser is only a fallback diagnostic; 32 IFDs is
    // ample for real DNG metadata and prevents stack/CPU amplification.
    unsigned remainingIfds = 32;
    scanIfd(r, r.u32(4), info.isDng, info.compression,
            info.sampleFormat, bestPx, 0, remainingIfds);
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
    if (isDeflate(comp)) {
#ifndef USE_ZLIB
        return SFRAW_ERR_DEFLATE_DNG;
#else
        if (comp != kDeflate || info.sampleFormat != 3)
            return SFRAW_ERR_DEFLATE_DNG;
#endif
    }
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
// lossless-JPEG/LJ92 (7), and SampleFormat=3 float deflate decode natively;
// integer/linear deflate is intentionally classified as a typed fallback.
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
                          const uint8_t* srcData, size_t srcLen) {
    DecodeResult result;

    if (cancellationRequested(options)) return cancellationFailure();

    // Intra-decode phase timers (#158). `phaseMs()` returns the delta since the last
    // call and re-stamps, so the phases are contiguous by construction and cannot
    // double-count. Cost is a handful of steady_clock reads against a ~3.5 s decode.
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
                            mCopy = 0, mAdapt = 0, mColour = 0;

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
        image->bits != 16) {
        result.status = SFRAW_ERR_FORMAT;
        result.error = "unexpected LibRaw image format (expected 16-bit 3-channel)";
        return result;
    }

    const int fullW = image->width;
    const int fullH = image->height;
    if (fullW <= 0 || fullH <= 0 ||
        pixelProductExceeds(static_cast<uint64_t>(fullW),
                            static_cast<uint64_t>(fullH),
                            kMaxFullDecodePixels)) {
        return dimensionLimitFailure(options);
    }
    const auto* src = reinterpret_cast<const uint16_t*>(image->data);
    constexpr float kInv16 = 1.0f / 65535.0f;

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
    result.width = ow;
    result.height = oh;
    result.rgb.resize(static_cast<size_t>(ow) * oh * 3);
    for (int oy = 0; oy < oh; ++oy) {
        if (cancellationRequested(options)) return cancellationFailure();
        const size_t srow = static_cast<size_t>(oy) * step * fullW;
        for (int ox = 0; ox < ow; ++ox) {
            const size_t si = (srow + static_cast<size_t>(ox) * step) * 3;
            const size_t di = (static_cast<size_t>(oy) * ow + ox) * 3;
            result.rgb[di]     = static_cast<float>(src[si])     * kInv16;
            result.rgb[di + 1] = static_cast<float>(src[si + 1]) * kInv16;
            result.rgb[di + 2] = static_cast<float>(src[si + 2]) * kInv16;
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
        "decode phases ms: unpack=%.0f process=%.0f memimg=%.0f copy=%.0f "
        "adapt=%.0f colour=%.0f",
        mUnpack, mProcess, mMemImg, mCopy, mAdapt, mColour);
#endif

    result.colorSpace = "ProPhoto RGB";
    result.status = SFRAW_OK;
    result.ok = true;
    return result;
}

}  // namespace

DecodeResult decodeFromBuffer(const uint8_t* data, size_t length,
                              const DecodeOptions& options) try {
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
    LibRaw raw;
    applyInputLimits(raw);
    applyCancellationHandler(raw, options);
    int rc = raw.open_buffer(const_cast<uint8_t*>(data), length);
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
    return finishDecode(raw, options, data, length);
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
    [[maybe_unused]] auto ioAt = std::chrono::steady_clock::now();
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
#ifdef __ANDROID__
    // Separated from the phases above because it happens before LibRaw sees anything.
    // Only the fd path is timed: decodeFromBuffer has no file read, and exports take
    // this one.
    __android_log_print(ANDROID_LOG_INFO, "sfraw", "decode io ms: fileread=%.0f bytes=%zu",
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ioAt).count(),
        bytes.size());
#endif
    if (bytes.empty()) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "failed to read RAW from fd";
        return result;
    }
    int rc = raw.open_buffer(bytes.data(), bytes.size());
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
    return finishDecode(raw, options, bytes.data(), bytes.size());
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
