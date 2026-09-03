/*
 * Spektrafilm for Android — lib:libraw native decoder.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Uses statically included, dual-offered LibRaw; distribution is governed by the
 * bundled decision record and fail-closed release audit.
 *
 * Decodes a camera RAW / DNG buffer into a linear, scene-referred RGB image with
 * the same processing-option contract as spektrafilm's desktop rawpy path:
 *   output_color = ACES (LibRaw code 6, ACES2065-1 primaries)
 *   output_bps   = 16
 *   no_auto_bright = 1
 *   gamm[0] = gamm[1] = 1.0   (linear)
 * White balance mirrors raw_file_processor.py: as-shot (camera WB), daylight
 * (LibRaw daylight base), tungsten / custom (temperature+tint -> CAT02
 * Von-Kries chromatic adaptation in linear ACES + a separate float32 tint step).
 * The result is then converted ACES2065-1 -> linear ProPhoto RGB (the spektrafilm
 * engine's input space), mirroring load_and_process_raw_file's output_colorspace
 * step, so DecodeResult.colorSpace is "ProPhoto RGB" — ACES is only intermediate.
 */
#ifndef SPECTRAFILM_RAW_DECODER_H
#define SPECTRAFILM_RAW_DECODER_H

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace spectrafilm {

// Mirrors RawDecoder.WhiteBalance in Kotlin and raw_file_processor.py modes.
enum class WhiteBalanceMode {
    AsShot,    // 'as_shot'  -> use_camera_wb
    Daylight,  // 'daylight' -> LibRaw daylight base, no adaptation
    Tungsten,  // 'tungsten' -> adapt 2850 K -> 6504 K, tint = 1.0
    Custom,    // 'custom'   -> adapt <temperature> K -> 6504 K, tint = <tint>
};

struct DecodeOptions {
    WhiteBalanceMode whiteBalance = WhiteBalanceMode::AsShot;
    // temperatureK in kelvin; tint multiplies green (1.0 = neutral). Native
    // entry points reject values outside the product contract before touching
    // LibRaw or colour math: temperatureK [1000, 12000], tint [0.2, 1.8], both
    // finite. The values are colour-active only in Custom mode, but validating
    // every request keeps NaN/Inf out of render and cache identities too.
    double temperatureK = 6504.0;
    double tint = 1.0;

    // Half-size (proxy) decode.  When true, LibRaw sets `imgdata.params.half_size = 1`
    // before `dcraw_process()`, producing an image at half the linear dimensions
    // (¼ the pixel count) by averaging each 2×2 Bayer cell into one output pixel
    // instead of running full demosaic interpolation.  Benefits:
    //   * Processed-image memory is ~¼ of a full-res decode for Bayer layouts.
    //   * Decode is substantially faster (no demosaic, smaller copy).
    // Tradeoffs:
    //   * Lower quality: colour at each output pixel is a simple 2×2 average, not
    //     a full-neighbourhood interpolation — fine for a proxy/preview, not for
    //     export or spectral processing.
    //   * `result.width` and `result.height` will be approximately half the values
    //     reported by LibRaw for the full-res image (LibRaw updates imgdata.sizes
    //     accordingly; `dcraw_make_mem_image` reports the post-process dimensions).
    //
    // The current in-memory safety gate is evaluated before unpack and remains
    // 12 MiPixels on 64-bit / 8 MiPixels on 32-bit even in half-size mode;
    // non-Bayer layouts may ignore half_size. Ticket #173 owns tiled high-MP decode.
    // Default false requests full-resolution output within those limits.
    bool halfSize = false;

    // Hard cap on the output's longest edge (pixels). 0 = no cap. When > 0 and the
    // decoded image's longest edge exceeds it, the result is box-downsampled (integer
    // step) to fit BEFORE result.rgb is returned — so the caller's direct-buffer
    // allocation is bounded regardless of whether `halfSize` actually reduced the
    // dimensions. Some DNGs ignore LibRaw's half_size and decode full-resolution; a
    // 4080×3060 result is a ~150 MB float buffer that OOMs the managed heap when wrapped
    // in a Java-backed direct ByteBuffer. Proxy-grade (nearest-step subsample).
    int maxLongEdge = 0;

    // Cooperative cancellation supplied by the JNI token registry. The caller
    // keeps this flag alive for the complete synchronous decode call. LibRaw's
    // unpack/demosaic calls do not expose an interrupt callback, so the wrapper
    // polls before and after those noninterruptible phases and throughout its own
    // fd-read, copy, adaptation, and colour-conversion loops.
    const std::atomic<bool>* cancelFlag = nullptr;
};

// Stable decode status codes. These cross to Kotlin (RawDecoder.DecodeStatus)
// so callers can branch on the failure kind (notably to pick a platform-decoder
// fallback for the DNG compressions LibRaw cannot decode without external image
// libraries). Distinct from LibRaw's own LIBRAW_* codes, which are preserved
// separately in DecodeResult.librawCode.
//
// Values are part of the JNI/Kotlin ABI: do NOT renumber existing entries; only
// append new ones (and add the matching entry to RawDecoder.kt's DecodeStatus).
//
// IMPORTANT — what decodes NATIVELY (returns SFRAW_OK, no fallback needed):
//   * Uncompressed DNG (Compression 1)         — plain mobile/Pixel DNGs
//   * Lossless-JPEG / LJ92 DNG (Compression 7) — common Google Pixel and other
//       computational-RAW DNGs. LibRaw decodes these with its OWN internal
//       lossless-JPEG code (lossless_jpeg_load_raw / ljpeg_start / ljpeg_row in
//       src/decoders/decoders_dcraw.cpp), which is compiled unconditionally and
//       does NOT require USE_JPEG/libjpeg. (USE_JPEG only adds *lossy* baseline
//       JPEG, below.)
//   * Floating-point DEFLATE/ZIP parsing exists in LibRaw via USE_ZLIB, but its
//       dcraw memory-bitmap route quantizes before this module's float boundary.
//       The wrapper therefore routes SampleFormat=3 to a typed display-referred /
//       next-codec fallback rather than labelling it native precision parity.
//   * Mainstream camera RAW (CR2/CR3/NEF/ARW/RAF/ORF/RW2/...).
enum DecodeStatus {
    SFRAW_OK = 0,
    SFRAW_ERR_UNKNOWN = 1,
    SFRAW_ERR_INPUT = 2,            // null/empty/unreadable input
    SFRAW_ERR_OPEN = 3,            // open_buffer/open_file failed
    SFRAW_ERR_FILE_UNSUPPORTED = 4,// not a recognized RAW/DNG
    SFRAW_ERR_UNPACK = 5,         // generic unpack() failure
    SFRAW_ERR_PROCESS = 6,        // dcraw_process / make_mem_image failure
    SFRAW_ERR_NO_MEMORY = 7,
    SFRAW_ERR_FORMAT = 8,         // unexpected processed-image format
    SFRAW_ERR_CANCELLED = 9,      // cooperative caller cancellation

    // ---- DNG compressions that need a platform-decoder fallback ----
    // Integer/linear DEFLATE DNG unsupported by pinned LibRaw 0.22.2, or any
    // floating-point DNG for which no precision-preserving RGB route is
    // qualified (including DEFLATE regardless of the zlib parser being built).
    SFRAW_ERR_DEFLATE_DNG = 10,

    // Lossy-baseline-JPEG-compressed DNG (DNG 1.4 lossy, Compression 0x884C, and
    // old-style JPEG, Compression 6). Needs libjpeg (USE_JPEG), which the NDK
    // does not ship, so this is an expected residual limitation. The app should
    // fall back to the platform ImageDecoder.
    SFRAW_ERR_LOSSY_JPEG_DNG = 11,

    // JPEG-XL-compressed DNG (Compression 0xCD42 / 52546, DNG 1.7+). Needs
    // libjxl / the Adobe DNG SDK, neither of which is vendored. App should fall
    // back to the platform ImageDecoder (Android 14+ decodes JXL).
    SFRAW_ERR_JPEGXL_DNG = 12,

    // Source precision/level/layout metadata was contradictory, unbounded, or
    // insufficient to describe the admitted decode without guessing.
    SFRAW_ERR_PRECISION_METADATA = 13,
};

// Human-readable name for a DNG Compression tag value (for diagnostics / logs).
// Handles values outside the sniffer's enum too.
const char* dngCompressionName(int compressionValue);

enum class RawSampleFormat : int {
    Unknown = 0,
    UnsignedInteger = 1,
    FloatingPoint = 2,
};

enum class RawByteOrder : int {
    Unknown = 0,
    LittleEndian = 1,
    BigEndian = 2,
    NotApplicable = 3,
};

enum class RawPacking : int {
    Unknown = 0,
    TiffPackedBits = 1,
    TiffWord16 = 2,
    TiffFloat = 3,
    LosslessCompressed = 4,
    VendorDefined = 5,
};

enum class RawPixelLayout : int {
    Unknown = 0,
    Bayer2x2 = 1,
    XTrans6x6 = 2,
    Linear = 3,
    Layered = 4,
    CustomCfa = 5,
};

enum class RawDecoderRoute : int {
    LibRawNative = 1,
};

enum class RawPostprocessRoute : int {
    LibRawAcesToFloat32ProPhoto = 1,
};

enum class RawLinearSpace : int {
    LinearProPhotoRgb = 1,
};

enum class RawLevelProvenance : int {
    Unknown = 0,
    DngMetadata = 1,
    DeclaredBitsDefault = 2,
    DecoderMetadata = 3,
};

// Versioned, bounded source contract published with every successful native
// decode. Black semantics are lossless: effective code black at CFA position
// (row,col,channel) is common + channel[channel] + repeatingPattern[row,col].
// DNG LinearRaw v1 is consequently admitted only as a 1x1 repeat with one
// BlackLevel/WhiteLevel per three/four-sample channel; a spatial
// row-column-sample matrix requires a versioned representation.
// White levels are per LibRaw channel after source selection and unpack, but
// before dcraw_process mutates level state. A zero declared/effective bit count means
// unknown/not-applicable, never an inferred zero-bit sensor.
struct RawPrecisionDescriptor {
    static constexpr int kVersion = 1;
    static constexpr std::size_t kMaxBlackPatternEntries = 64U;
    static constexpr std::size_t kMaxCfaPatternEntries = 36U;
    // JNI carrier ABI v1. Kotlin validates these exact sizes before reading any
    // index; changing either requires a descriptor version bump and both-side tests.
    static constexpr std::size_t kJniWordCount = 136U;
    static constexpr std::size_t kJniRealCount = 2U;

    int version = kVersion;
    RawSampleFormat sampleFormat = RawSampleFormat::Unknown;
    int declaredBitsPerSample = 0;
    int effectiveBitsPerSample = 0;
    int processedBitsPerSample = 0;
    RawByteOrder byteOrder = RawByteOrder::Unknown;
    RawPacking packing = RawPacking::Unknown;
    int containerCompression = -1;
    RawPixelLayout pixelLayout = RawPixelLayout::Unknown;
    int colorChannels = 0;
    std::uint32_t cfaFilterCode = 0U;
    int cfaPatternRows = 0;
    int cfaPatternColumns = 0;
    std::array<std::uint8_t, kMaxCfaPatternEntries> cfaPattern{};
    std::size_t cfaPatternCount = 0U;
    RawDecoderRoute decoderRoute = RawDecoderRoute::LibRawNative;
    RawPostprocessRoute postprocessRoute =
        RawPostprocessRoute::LibRawAcesToFloat32ProPhoto;
    RawLinearSpace linearSpace = RawLinearSpace::LinearProPhotoRgb;
    RawLevelProvenance whiteLevelProvenance = RawLevelProvenance::Unknown;
    RawLevelProvenance blackLevelProvenance = RawLevelProvenance::Unknown;
    bool halfSizeRequested = false;

    std::uint32_t blackLevelCommon = 0U;
    std::array<std::uint32_t, 4> blackLevelChannels{};
    int blackPatternRows = 0;
    int blackPatternColumns = 0;
    std::array<std::uint32_t, kMaxBlackPatternEntries> blackPattern{};
    std::size_t blackPatternCount = 0U;
    std::array<std::uint32_t, 4> whiteLevels{};

    bool baselineExposurePresent = false;
    float baselineExposure = 0.0f;
    bool linearResponseLimitPresent = false;
    float linearResponseLimit = 0.0f;

    // Proxy state is explicit and reports both independent reduction routes.
    // `requestedMaxLongEdge == 0` means no post-bitmap cap was requested;
    // `outputSubsampleStep == 1` means that route did not reduce this result.
    int requestedMaxLongEdge = 0;
    int outputSubsampleStep = 1;
};

// Linear scene-referred result. Pixels are interleaved RGB float32, row-major,
// normalized from LibRaw's requested high-precision processed bitmap into
// [0,1], in linear ProPhoto RGB primaries
// (decoded via ACES2065-1, then converted — see the file header / aces2065ToProPhotoRGB).
// Move-only malloc-backed output storage. The decoder writes every element before
// publication, so it deliberately avoids std::vector::resize's full-buffer zero fill.
// release() transfers the exact malloc-compatible base to the JNI ownership registry;
// this permits a direct ByteBuffer handoff without a second full-frame copy.
class DecodedFloatBuffer final {
 public:
    DecodedFloatBuffer() noexcept = default;
    ~DecodedFloatBuffer() { reset(); }

    DecodedFloatBuffer(const DecodedFloatBuffer&) = delete;
    DecodedFloatBuffer& operator=(const DecodedFloatBuffer&) = delete;

    DecodedFloatBuffer(DecodedFloatBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0U)) {}

    DecodedFloatBuffer& operator=(DecodedFloatBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0U);
        }
        return *this;
    }

    void allocateUninitialized(std::size_t count) {
        if (count == 0U) {
            reset();
            return;
        }
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
            throw std::bad_alloc();
        }
        void* allocation = std::malloc(count * sizeof(float));
        if (allocation == nullptr) throw std::bad_alloc();
        reset();
        data_ = static_cast<float*>(allocation);
        size_ = count;
    }

    void reset() noexcept {
        std::free(data_);
        data_ = nullptr;
        size_ = 0U;
    }

    [[nodiscard]] float* release() noexcept {
        size_ = 0U;
        return std::exchange(data_, nullptr);
    }

    [[nodiscard]] float* data() noexcept { return data_; }
    [[nodiscard]] const float* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

    float& operator[](std::size_t index) noexcept { return data_[index]; }
    const float& operator[](std::size_t index) const noexcept {
        return data_[index];
    }

 private:
    float* data_ = nullptr;
    std::size_t size_ = 0U;
};

struct DecodeResult {
    DecodedFloatBuffer rgb;       // size == width * height * 3
    int width = 0;
    int height = 0;
    std::string colorSpace = "ProPhoto RGB";
    bool ok = false;
    std::string error;            // populated when ok == false
    // Stable Spektrafilm status (DecodeStatus). SFRAW_OK on success.
    int status = SFRAW_ERR_UNKNOWN;
    // Underlying LibRaw error code (LIBRAW_*), for diagnostics. 0 if N/A.
    int librawCode = 0;
    RawPrecisionDescriptor descriptor;
};

// Decode from an in-memory RAW/DNG buffer (e.g. a SAF InputStream read fully).
DecodeResult decodeFromBuffer(const uint8_t* data, size_t length, const DecodeOptions& options);

// Decode directly from a file descriptor (e.g. a SAF ParcelFileDescriptor).
// The fd is duplicated internally; the caller retains ownership.
DecodeResult decodeFromFd(int fd, const DecodeOptions& options);

// --- White-balance math (exposed for unit testing / parity checks) ---

// CCT (kelvin) -> normalized XYZ whitepoint (Y == 1), matching
// _whitepoint_xyz_from_temperature in raw_file_processor.py: CIE daylight locus
// for >= 4000 K, Kang 2002 Planckian approximation below.
void whitepointXyzFromTemperature(double temperatureK, double outXyz[3]);

// True only when the full native WB option contract is valid. Validation is
// deliberately independent of LibRaw so host and Android entry points share it.
bool rawWhiteBalanceOptionsValid(const DecodeOptions& options) noexcept;

// Apply the oracle-locked RAW white balance in place at the float32 ACES2065-1
// boundary. Tungsten/custom receive exactly one CAT02 scene-white adaptation;
// as-shot/daylight are byte-preserving no-ops. CAT output is rounded to float32
// before a separate float32 tint multiply, matching colour-science/NumPy cast
// order. Invalid options and a cancellation already set at entry leave pixels
// untouched; a cancellation observed during a large buffer may leave a partial
// internal result, which the decode owner discards.
bool applyAcesWhiteBalance(float* rgb, size_t pixelCount,
                           const DecodeOptions& options);

// Build the transformed unit-neutral ACES value. This compatibility helper is
// retained for small math probes; non-neutral pixels must use the full CAT02
// matrix through applyAcesWhiteBalance(), not a per-channel multiplier. Returns
// false and leaves the identity value in `outMul` for invalid/cancelled input.
bool buildAcesWbMultiplier(const DecodeOptions& options, float outMul[3]);

// Convert an interleaved RGB float32 buffer (pixelCount pixels) from linear
// ACES2065-1 to linear ProPhoto RGB in place, mirroring raw_file_processor.py's
// final colour.RGB_to_RGB(ACES2065-1 -> ProPhoto RGB, no cctf) step (CAT02). This
// is applied unconditionally by the decode path so the result is linear ProPhoto
// RGB. Exposed for unit testing / parity checks. Does not clamp out-of-gamut values.
void aces2065ToProPhotoRGB(float* rgb, size_t pixelCount);

}  // namespace spectrafilm

#endif  // SPECTRAFILM_RAW_DECODER_H
