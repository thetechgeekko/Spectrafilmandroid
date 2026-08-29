/*
 * Spektrafilm for Android — lib:libraw native decoder (implementation).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Uses LibRaw (LGPL-2.1).
 *
 * Reproduces spektrafilm/utils/raw_file_processor.py on-device with LibRaw:
 *   raw.postprocess(output_color=ACES, output_bps=16, no_auto_bright=True,
 *                   gamma=(1,1), use_camera_wb=<as_shot>)
 * then the colour-science white-balance path (Von-Kries adaptation + tint) for
 * the non-as-shot modes, and finally the colourspace conversion to linear
 * ProPhoto RGB (the engine's input space) — i.e. load_and_process_raw_file with
 * output_colorspace="ProPhoto RGB". The decoded result is therefore linear
 * ProPhoto RGB, NOT ACES2065-1 (ACES is only the intermediate working space).
 *
 * The LibRaw include is guarded with __has_include so this translation unit still
 * compiles and links even if LibRaw is unavailable (e.g. a host build without the
 * source). In the normal build, CMake fetches LibRaw and adds its root to this
 * target's include path, so <libraw/libraw.h> resolves and the real decode path
 * (SFRAW_HAVE_LIBRAW) is compiled in.
 */
#include "raw_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#if defined(__has_include)
#  if __has_include(<libraw/libraw.h>)
#    include <libraw/libraw.h>
#    define SFRAW_HAVE_LIBRAW 1
#  endif
#endif

#ifndef SFRAW_HAVE_LIBRAW
#  define SFRAW_HAVE_LIBRAW 0
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
// CRITICAL: lossless-JPEG/LJ92 (Compression 7) and uncompressed (1) and deflate
// (8) all decode NATIVELY (see DecodeStatus doc in raw_decoder.h). LibRaw's
// internal lossless_jpeg_load_raw handles tag 7 with no libjpeg. So tag 7 must
// NOT be classified as a fallback case — only genuinely-unsupported lossy JPEG
// (6 / 0x884C) and JPEG-XL (0xCD42) are.
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
    kDeflate = 8,         // ZIP/DEFLATE (Adobe) -> decodes natively (USE_ZLIB)
    kDeflateAdobe = 0x80B2,
    kLossyJpeg = 0x884C,  // DNG 1.4 lossy baseline JPEG -> needs libjpeg
    kJpegXL = 0xCD42,     // DNG 1.7 JPEG-XL (52546) -> needs libjxl/dngsdk
};

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    bool be = false;
    bool in(size_t off, size_t len) const { return off + len <= n && off <= n; }
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
                    int& bestComp, uint64_t& bestPx, int depth) {
    if (ifdOff == 0 || depth > 4 || !r.in(ifdOff, 2)) return;
    uint16_t entries = r.u16(ifdOff);
    if (entries == 0 || entries > 512) return;

    int comp = kUnknown;
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
    }
    for (int s = 0; s < subCount; ++s)
        scanIfd(r, subifds[s], isDng, bestComp, bestPx, depth + 1);

    uint32_t next = r.u32((size_t)ifdOff + 2 + (size_t)entries * 12);
    if (next > ifdOff) scanIfd(r, next, isDng, bestComp, bestPx, depth);
}

// Returns the primary-image compression; sets *isDng if a DNGVersion tag seen.
inline int compressionOf(const uint8_t* data, size_t len, bool* isDng) {
    *isDng = false;
    if (data == nullptr || len < 8) return kUnknown;
    Reader r;
    r.p = data;
    r.n = len;
    if (data[0] == 'I' && data[1] == 'I') r.be = false;
    else if (data[0] == 'M' && data[1] == 'M') r.be = true;
    else return kUnknown;
    int best = kUnknown;
    uint64_t bestPx = 0;
    bool dng = false;
    scanIfd(r, r.u32(4), dng, best, bestPx, 0);
    *isDng = dng;
    return best;
}

inline bool isDeflate(int c) { return c == kDeflate || c == kDeflateAdobe; }
inline bool isLossyJpeg(int c) { return c == kLossyJpeg || c == kLossyJpegOld; }
inline bool isJpegXL(int c) { return c == kJpegXL; }
// Compressions LibRaw decodes natively in this build (no external image libs).
inline bool isNativelySupported(int c) {
    return c == kNone || c == kLosslessJpeg || isDeflate(c);
}

// Classify an unpack() failure for a compressed DNG into a stable status.
// Returns SFRAW_ERR_UNPACK if it isn't a recognizable must-fallback case.
//
// Note: this only runs AFTER unpack() has already failed. Uncompressed (1),
// lossless-JPEG/LJ92 (7) and deflate (8) decode natively, so reaching here with
// one of those means a genuine data error, not an unsupported codec -> we leave
// them as SFRAW_ERR_UNPACK rather than mislabeling them as a fallback case.
inline int classifyUnpackFailure(const uint8_t* data, size_t len) {
    bool isDng = false;
    int comp = compressionOf(data, len, &isDng);
    if (isDng) {
        if (isJpegXL(comp)) return SFRAW_ERR_JPEGXL_DNG;  // needs libjxl/dngsdk
        if (isLossyJpeg(comp)) {
#ifndef USE_JPEG
            return SFRAW_ERR_LOSSY_JPEG_DNG;  // needs libjpeg
#endif
        }
        if (isDeflate(comp)) {
#ifndef USE_ZLIB
            return SFRAW_ERR_DEFLATE_DNG;  // misbuild: rebuild with USE_ZLIB
#endif
        }
        // A DNG of unknown/unreadable compression that still failed unpack: the
        // dominant real-world cause among DNGs LibRaw can't open is an
        // unsupported lossy codec, so hint the platform fallback.
        if (comp == kUnknown) return SFRAW_ERR_LOSSY_JPEG_DNG;
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

// Reference (target) white for the daylight-base modes. raw_file_processor.py:
//   _DAYLIGHT_REFERENCE_TEMPERATURE = 6504.0
constexpr double kDaylightReferenceTemperature = 6504.0;
// raw_file_processor.py: _TUNGSTEN_TEMPERATURE = 2850.0
constexpr double kTungstenTemperature = 2850.0;

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

void mat3MulVec(const double m[9], const double v[3], double out[3]) {
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

// CIE daylight locus chromaticity for T >= 4000 K (matches colour's
// 'CIE Illuminant D Series' path used by _whitepoint_xyz_from_temperature).
void daylightXy(double t, double& x, double& y) {
    const double t1 = 1.0e3 / t;
    const double t2 = 1.0e6 / (t * t);
    const double t3 = 1.0e9 / (t * t * t);
    if (t <= 7000.0) {
        x = 0.244063 + 0.09911 * t1 + 2.9678 * t2 - 4.6070 * t3;
    } else {
        x = 0.237040 + 0.24748 * t1 + 1.9018 * t2 - 2.0064 * t3;
    }
    y = -3.000 * x * x + 2.870 * x - 0.275;
}

// Kang 2002 Planckian approximation for T < 4000 K (matches colour's 'Kang 2002').
void kang2002Xy(double t, double& x, double& y) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    if (t <= 4000.0) {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    }
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
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

void buildAcesWbMultiplier(const DecodeOptions& options, float outMul[3]) {
    outMul[0] = outMul[1] = outMul[2] = 1.0f;

    // as-shot / daylight: no colour-science adaptation in raw_file_processor.py.
    if (options.whiteBalance == WhiteBalanceMode::AsShot ||
        options.whiteBalance == WhiteBalanceMode::Daylight) {
        return;
    }

    double targetTemp = options.temperatureK;
    double tint = options.tint;
    if (options.whiteBalance == WhiteBalanceMode::Tungsten) {
        targetTemp = kTungstenTemperature;  // adapt 2850 K -> reference
        tint = 1.0;
    }

    // Source/target whites, normalized so Y == 1 (raw_file_processor.py divides by [1]).
    double scene[3], reference[3];
    whitepointXyzFromTemperature(targetTemp, scene);
    whitepointXyzFromTemperature(kDaylightReferenceTemperature, reference);

    // Von-Kries adaptation collapses, for a neutral (R=G=B) pixel, to a per-channel
    // scale in ACES RGB. We derive that scale by adapting a unit-white ACES pixel:
    //   rgb=(1,1,1) -> XYZ -> (diag von-Kries scene->reference) -> XYZ -> rgb.
    // For non-neutral pixels the JNI/native path applies the full matrix per pixel;
    // exposing the multiplier here covers the common neutral case + unit tests.
    //
    // Von-Kries here uses XYZ directly (colour 'Von Kries' method with no cone
    // transform == identity sharpening), matching method='Von Kries' in the Python.
    double whiteXyz[3];
    const double unit[3] = {1.0, 1.0, 1.0};
    mat3MulVec(kAcesRgbToXyz, unit, whiteXyz);

    double adapted[3] = {
        whiteXyz[0] * (reference[0] / scene[0]),
        whiteXyz[1] * (reference[1] / scene[1]),
        whiteXyz[2] * (reference[2] / scene[2]),
    };
    double adaptedRgb[3];
    mat3MulVec(kAcesXyzToRgb, adapted, adaptedRgb);

    outMul[0] = static_cast<float>(adaptedRgb[0]);
    outMul[1] = static_cast<float>(adaptedRgb[1] * tint);  // green tint multiplier
    outMul[2] = static_cast<float>(adaptedRgb[2]);
}

void aces2065ToProPhotoRGB(float* rgb, size_t pixelCount) {
    // Pure matrix (kAcesToProPhoto), no transfer function — matching
    // raw_file_processor.py's final colour.RGB_to_RGB step. Accumulate in double
    // for parity with the float64 numpy/colour path, then store float32.
    // Deliberately does NOT clamp: ACES2065-1 (AP0) is wider than ProPhoto, so
    // saturated colours can map slightly out of gamut (negative / >1), exactly as
    // the oracle leaves them — gamut handling is a separate, opt-in pipeline stage.
    for (size_t i = 0; i < pixelCount; ++i) {
        const double in[3] = {rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]};
        double out[3];
        mat3MulVec(kAcesToProPhoto, in, out);
        rgb[i * 3 + 0] = static_cast<float>(out[0]);
        rgb[i * 3 + 1] = static_cast<float>(out[1]);
        rgb[i * 3 + 2] = static_cast<float>(out[2]);
    }
}

#if SFRAW_HAVE_LIBRAW

namespace {

// Apply the rawpy-parity postprocess params (RAW_DNG.md):
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

// Full per-pixel Von-Kries adaptation in ACES RGB (the non-neutral-accurate path),
// mirroring _apply_white_balance_adaptation + _apply_tint_adjustment.
void applyAcesAdaptation(float* rgb, size_t pixelCount, const DecodeOptions& options) {
    if (options.whiteBalance == WhiteBalanceMode::AsShot ||
        options.whiteBalance == WhiteBalanceMode::Daylight) {
        return;
    }

    double targetTemp = options.temperatureK;
    double tint = options.tint;
    if (options.whiteBalance == WhiteBalanceMode::Tungsten) {
        targetTemp = kTungstenTemperature;
        tint = 1.0;
    }

    double scene[3], reference[3];
    whitepointXyzFromTemperature(targetTemp, scene);
    whitepointXyzFromTemperature(kDaylightReferenceTemperature, reference);
    const double sx = reference[0] / scene[0];
    const double sy = reference[1] / scene[1];
    const double sz = reference[2] / scene[2];

    for (size_t i = 0; i < pixelCount; ++i) {
        double in[3] = {rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]};
        double xyz[3];
        mat3MulVec(kAcesRgbToXyz, in, xyz);
        xyz[0] *= sx; xyz[1] *= sy; xyz[2] *= sz;     // diagonal Von-Kries
        double out[3];
        mat3MulVec(kAcesXyzToRgb, xyz, out);
        rgb[i * 3 + 0] = static_cast<float>(out[0]);
        rgb[i * 3 + 1] = static_cast<float>(out[1] * tint);  // green tint
        rgb[i * 3 + 2] = static_cast<float>(out[2]);
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
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        result.status = dngsniff::classifyUnpackFailure(srcData, srcLen);
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
            hint = " [deflate-compressed DNG; rebuild LibRaw with USE_ZLIB]";
        result.error = std::string("LibRaw unpack() failed: ") +
                       libraw_strerror(rc) + where + hint;
        return result;
    }
    applyParityParams(raw, options);
    phaseMs();  // param setup is not decode work — keep it out of `process`
    rc = raw.dcraw_process();
    mProcess = phaseMs();
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        result.status = SFRAW_ERR_PROCESS;
        result.error =
            std::string("LibRaw dcraw_process() failed: ") + libraw_strerror(rc);
        return result;
    }

    int status = LIBRAW_SUCCESS;
    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&status);
    mMemImg = phaseMs();
    if (img == nullptr || status != LIBRAW_SUCCESS) {
        if (img) LibRaw::dcraw_clear_mem(img);
        result.librawCode = status;
        result.status = SFRAW_ERR_PROCESS;
        result.error = std::string("LibRaw dcraw_make_mem_image() failed: ") +
                       libraw_strerror(status);
        return result;
    }
    if (img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3 || img->bits != 16) {
        LibRaw::dcraw_clear_mem(img);
        result.status = SFRAW_ERR_FORMAT;
        result.error = "unexpected LibRaw image format (expected 16-bit 3-channel)";
        return result;
    }

    const int fullW = img->width;
    const int fullH = img->height;
    const auto* src = reinterpret_cast<const uint16_t*>(img->data);
    constexpr float kInv16 = 1.0f / 65535.0f;

    // Cap the longest edge to options.maxLongEdge (proxy bound) DURING the uint16->float
    // copy: subsample img->data straight into the final-sized buffer so we never hold a
    // second full-resolution float image. LibRaw's half_size is honoured for most Bayer
    // DNGs, but some (e.g. certain Samsung Expert-RAW DNGs) decode full-resolution
    // regardless; without this cap result.rgb stays full-res and the JVM-side direct
    // ByteBuffer (a managed byte[] on Android) OOMs the ART heap. Peak native memory here
    // is LibRaw's own image + the (small) capped buffer — not two full-res copies.
    int step = 1;
    {
        const int longest = fullW > fullH ? fullW : fullH;
        if (options.maxLongEdge > 0 && longest > options.maxLongEdge) {
            while (longest / step > options.maxLongEdge) ++step;
        }
    }
    const int ow = step > 1 ? (fullW + step - 1) / step : fullW;
    const int oh = step > 1 ? (fullH + step - 1) / step : fullH;
    result.width = ow;
    result.height = oh;
    result.rgb.resize(static_cast<size_t>(ow) * oh * 3);
    for (int oy = 0; oy < oh; ++oy) {
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
    LibRaw::dcraw_clear_mem(img);

    applyAcesAdaptation(result.rgb.data(), static_cast<size_t>(ow) * oh, options);
    mAdapt = phaseMs();

    // Final colourspace conversion: linear ACES2065-1 -> linear ProPhoto RGB, the
    // engine's input space (raw_file_processor.py's output_colorspace step). Done
    // AFTER the ACES-space white-balance adaptation, matching the oracle order.
    // Without this the buffer was tagged ACES but read as ProPhoto downstream
    // (spektra_jni hardcodes the engine input to ProPhoto), so every native RAW
    // decode ran through the wrong primaries.
    aces2065ToProPhotoRGB(result.rgb.data(), static_cast<size_t>(ow) * oh);
    mColour = phaseMs();

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

DecodeResult decodeFromBuffer(const uint8_t* data, size_t length, const DecodeOptions& options) {
    DecodeResult result;
    if (data == nullptr || length == 0) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "empty input buffer";
        return result;
    }
    LibRaw raw;
    int rc = raw.open_buffer(const_cast<uint8_t*>(data), length);
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        result.status = (rc == LIBRAW_FILE_UNSUPPORTED) ? SFRAW_ERR_FILE_UNSUPPORTED
                                                        : SFRAW_ERR_OPEN;
        result.error = std::string("LibRaw open_buffer() failed: ") +
                       libraw_strerror(rc) + " (unsupported or corrupt RAW)";
        return result;
    }
    return finishDecode(raw, options, data, length);
}

DecodeResult decodeFromFd(int fd, const DecodeOptions& options) {
    DecodeResult result;
    if (fd < 0) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "invalid file descriptor";
        return result;
    }
    LibRaw raw;
    // LibRaw can read from a FILE*; we dup the fd so the caller keeps ownership.
    int dup_fd = ::dup(fd);
    if (dup_fd < 0) {
        result.status = SFRAW_ERR_INPUT;
        result.error = "dup(fd) failed";
        return result;
    }
    FILE* fp = ::fdopen(dup_fd, "rb");
    if (fp == nullptr) {
        ::close(dup_fd);
        result.status = SFRAW_ERR_INPUT;
        result.error = "fdopen() failed";
        return result;
    }
    // open_datastream owns the FILE* lifecycle once the buffer is attached; simplest
    // robust path is to slurp into memory and reuse open_buffer.
    // TODO(libraw): for very large RAWs, switch to a LibRaw_abstract_datastream
    // backed by the fd to avoid the full read into RAM.
    std::vector<uint8_t> bytes;
    [[maybe_unused]] auto ioAt = std::chrono::steady_clock::now();
    {
        std::fseek(fp, 0, SEEK_END);
        long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            bytes.resize(static_cast<size_t>(size));
            size_t read = std::fread(bytes.data(), 1, bytes.size(), fp);
            bytes.resize(read);
        }
    }
    std::fclose(fp);  // closes dup_fd
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
    if (rc != LIBRAW_SUCCESS) {
        result.librawCode = rc;
        result.status = (rc == LIBRAW_FILE_UNSUPPORTED) ? SFRAW_ERR_FILE_UNSUPPORTED
                                                        : SFRAW_ERR_OPEN;
        result.error = std::string("LibRaw open_buffer() failed: ") +
                       libraw_strerror(rc) + " (unsupported or corrupt RAW)";
        return result;
    }
    return finishDecode(raw, options, bytes.data(), bytes.size());
}

#else  // !SFRAW_HAVE_LIBRAW

// Fallback: LibRaw headers were not found at compile time (only happens if the
// FetchContent step was skipped without -DSFRAW_LIBRAW_SOURCE_DIR). The module
// still compiles and the .so links so the Kotlin facade can be wired; decode()
// returns a clear error instead of crashing.

DecodeResult decodeFromBuffer(const uint8_t*, size_t, const DecodeOptions&) {
    DecodeResult result;
    result.status = SFRAW_ERR_UNKNOWN;
    result.error = "LibRaw unavailable: configure with network (FetchContent) or -DSFRAW_LIBRAW_SOURCE_DIR";
    return result;
}

DecodeResult decodeFromFd(int, const DecodeOptions&) {
    DecodeResult result;
    result.status = SFRAW_ERR_UNKNOWN;
    result.error = "LibRaw unavailable: configure with network (FetchContent) or -DSFRAW_LIBRAW_SOURCE_DIR";
    return result;
}

#endif  // SFRAW_HAVE_LIBRAW

}  // namespace spectrafilm
