/*
 * Spektrafilm Android -- bounded LibRaw public-API regression support.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#ifndef SFRAW_PUBLIC_API_TEST_SUPPORT_H
#define SFRAW_PUBLIC_API_TEST_SUPPORT_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sfraw::hosttest {

constexpr std::size_t kMaxInputBytes = 16U * 1024U * 1024U;
constexpr unsigned kMaxRawMemoryMb = 128U;
constexpr std::uint64_t kMaxDeclaredPixels = 12U * 1024U * 1024U;

enum class DecodeStage {
  kNotStarted,
  kOpen,
  kUnpack,
  kProcess,
  kDimensionGuard,
};

struct DecodeOutcome {
  int openCode;
  int unpackCode;
  int processCode;
  DecodeStage terminalStage;
  unsigned processingWidth;
  unsigned processingHeight;
};

std::vector<std::uint8_t> readHexFixture(const std::filesystem::path& path);

// Exactly fills LibRaw's 64-byte identify header without a trailing NUL. The
// public seam must reject it without treating the fixed-size byte array as a
// C string and reading into adjacent stack storage.
std::vector<std::uint8_t> makeNonTerminatedIdentifyHead();
std::vector<std::uint8_t> makePxnIdentifyHead();
std::vector<std::uint8_t> makeNonTerminatedMakerNoteTiff();

// Minimal Quattro X3F containers whose eight-byte model signature exactly
// ends LibRaw's 2048-byte probe window. They cover the missing dp selector and
// the bounded sd Quattro search/copy without relying on adjacent stack bytes.
std::vector<std::uint8_t> makeTailDpSignatureX3f();
std::vector<std::uint8_t> makeTailSdSignatureX3f();

// Exercises only LibRaw's supported public seam. Later stages are called iff
// the preceding stage succeeded. The input, raw allocation, and declared pixel
// count are independently bounded before expensive processing begins.
DecodeOutcome exercisePublicDecode(const std::vector<std::uint8_t>& input,
                                   bool exerciseWavelet = false);

// Deterministic project-owned negative DNGs. They advertise lossless-JPEG
// compression but provide a short/invalid strip, so open_buffer may recognize
// the container while unpack/dcraw_process must never produce a valid image.
std::vector<std::uint8_t> makeMalformedLosslessJpegDng(
    const std::vector<std::uint8_t>& stripBytes,
    std::uint32_t declaredStripBytes);

// Minimal valid SOF3/DHT/SOS followed by deliberately short entropy. A
// category above 16 is invalid and must be rejected by the decoder.
std::vector<std::uint8_t> makeTruncatedLosslessJpegStrip(
    std::uint8_t category, std::size_t entropyBytes);

// A complete category-zero lossless-JPEG TIFF whose hostile CR2Slice metadata
// maps decoded columns beyond raw_width. It deliberately has no DNGVersion tag:
// DNG routing uses lossless_dng_load_raw and would never exercise the vulnerable
// lossless_jpeg_load_raw seam. Patched LibRaw skips the invalid stores and
// completes without writing past raw_image (CVE-2026-21413).
std::vector<std::uint8_t> makeHostileCr2SliceTiff();

// Exact generic lossless-JPEG route for the signed CR2Slice multiplication
// overflow: raw 22 x 64000 with slice widths 65535. UBSan must remain quiet.
std::vector<std::uint8_t> makeOverflowingCr2SliceTiff();

// Generic TIFF whose small category-zero entropy advertises roughly 134 million
// SOF3 samples over a tiny raw plane. The work-budget patch must reject before
// lossless_jpeg_load_raw enters its nested decode loops.
std::vector<std::uint8_t> makeOversizedLjpegGeometryTiff();
std::vector<std::uint8_t> makeOversizedLjpegGeometryDng();

// A complete category-zero lossless-JPEG CFA DNG. This is the positive
// compatibility control for the lossless-DNG route hardened by the work and
// tile-count budgets.
std::vector<std::uint8_t> makeValidLosslessJpegDng();
std::vector<std::uint8_t> makeSof1ZrlDng();
std::vector<std::uint8_t> makeSof1DcOverflowDng();
std::vector<std::uint8_t> makeSof1AcOverrunDng();

// A 256 x 256 lossless-JPEG DNG split into 1 x 1 tiles. Its 65,536 decoder
// streams exceed the mobile operational ceiling and must fail before the first
// tile is parsed.
std::vector<std::uint8_t> makeExcessiveTileCountLosslessJpegDng();

// The same hostile 1 x 1 tiling with Compression=1, proving the packed tiled
// decoder cannot bypass the shared stream budget.
std::vector<std::uint8_t> makeExcessiveTileCountUncompressedDng();

// Exact non-DNG SONY Compression=6 controls for sony_ljpeg_load_raw(). The
// valid control uses the decoder's required four components; the malformed
// control proves other component counts reject; the tiled case exercises the
// shared setup-work ceiling.
std::vector<std::uint8_t> makeValidSonyLjpegTiff();
std::vector<std::uint8_t> makeInvalidSonyLjpegComponentsTiff();
std::vector<std::uint8_t> makeExcessiveSonyTileCountTiff();

// Exact Canon/BitsPerSample=15 controls for canon_sraw_load_raw(). The hostile
// variants cover negative-chroma arithmetic, SOF-height exhaustion, and empty
// CR2Slice replay respectively.
std::vector<std::uint8_t> makeValidCanonSrawTiff();
std::vector<std::uint8_t> makeNegativeChromaCanonSrawTiff();
std::vector<std::uint8_t> makeShortSofCanonSrawTiff();
std::vector<std::uint8_t> makeRedundantSlicesCanonSrawTiff();
std::vector<std::uint8_t> makeLargeModelVersionCanonSrawTiff();
std::vector<std::uint8_t> makeValidCanonSrawWhiteBalanceTiff();
std::vector<std::uint8_t> makeOverflowingCanonSrawWhiteBalanceTiff();

// Malformed Sony-routed lossless-JPEG DHT controls: a missing final symbol,
// duplicate table definitions, and the 64 KiB terminal-table shape that reads
// beyond LibRaw 0.22.2's parser buffer without the segment-bounds patch.
std::vector<std::uint8_t> makeTruncatedDhtSonyTiff();
std::vector<std::uint8_t> makeDuplicateDhtSonyTiff();
std::vector<std::uint8_t> makeTerminalDhtOverflowSonyTiff();
std::vector<std::uint8_t> makeOversubscribedDhtSonyTiff();
std::vector<std::uint8_t> makeNegativeEffectiveBitsSonyTiff();

// Exactly 4096 tiny streams at the compatibility floor: the lightweight
// control must remain supported, while expensive DHT setup hits the work cap.
std::vector<std::uint8_t> makeBoundarySonyTileCountTiff();
std::vector<std::uint8_t> makeCumulativeDhtWorkSonyTiff();
std::vector<std::uint8_t> makeCumulativeMarkerWorkSonyTiff();

// Repeated identify-time lossless-JPEG sniffing through scalar StripOffset
// tags. The control scans once; the hostile fixture replays one marker-heavy
// stream until the global open-stage work budget rejects it.
std::vector<std::uint8_t> makeValidIdentifyLjpegTiff();
std::vector<std::uint8_t> makeRepeatedIdentifyLjpegTiff();

// TIFF-compliant 16x16 tiling with more than 4096 decoder streams.
std::vector<std::uint8_t> makeCompliantTiledLosslessJpegDng();

// Valid float/deflate DNG controls for distinct 16x16 tiles and a hostile 1x1
// layout that points all 4096 tile entries at one padded zlib stream.
std::vector<std::uint8_t> makeDistinctTileFloatDeflateDng();
std::vector<std::uint8_t> makeRepeatedTileFloatDeflateDng();
// BitsPerSample=32 and SampleFormat=float with no WhiteLevel. This reaches
// identify()'s default-maximum calculation directly from untrusted metadata.
std::vector<std::uint8_t> makeFloat32DngWithoutWhiteLevel();
// Invalid integer BitsPerSample=31 with no WhiteLevel. identify() must reject
// without evaluating a signed 1 << 31 while copying DNG level defaults.
std::vector<std::uint8_t> makeInteger31DngWithoutWhiteLevel();
// Compression 0x80B2 resembles Adobe deflate but has no decoder route in
// pinned LibRaw 0.22.2. The wrapper must return typed DEFLATE fallback even
// when open_buffer rejects before unpack.
std::vector<std::uint8_t> makeAdobeDeflateDng();

// Non-DNG camera metadata that used to reach negative shift counts while
// identify() derived vendor black levels from hostile BitsPerSample values.
std::vector<std::uint8_t> makeSonyInvalidBlackShiftTiff();
std::vector<std::uint8_t> makeSamsungInvalidBlackShiftTiff();
std::vector<std::uint8_t> makeSonyBoundaryBlackShiftTiff();
std::vector<std::uint8_t> makeSamsungBoundaryBlackShiftTiff();

// Hasselblad's paired predictor requires an even raw width.
std::vector<std::uint8_t> makeHasselbladTiff(
    bool oddWidth, std::uint8_t huffmanCategory = 0U);

// SamplesPerPixel=4 plus repeated category-16 negative differences reaches the
// Hasselblad horizontal predictor's signed-int boundary on the first row.
std::vector<std::uint8_t> makeOverflowingHasselbladPredictorTiff();

// Exact Olympus/OM System 14-bit decoder controls. The boundary pair proves
// tagX641=32 cannot become a native-width shift; the hostile variants cover
// EOF-bounded unary work and negative predictor scaling.
std::vector<std::uint8_t> makeOlympusTag641BoundaryTiff(
    std::uint16_t tagX641);
std::vector<std::uint8_t> makeOlympusUnaryWorkTiff();
std::vector<std::uint8_t> makeOlympusNegativePredictorTiff();

// Panasonic RawFormat=8 controls for required Huffman metadata, every formerly
// undefined shift family, and parallel stripe ownership. The valid controls
// use a bounded ordered five-bit table; non-zero predictors prove actual pixel
// writes. The shadowed-table control preserves real DC-S5M2 first-match rules.
std::vector<std::uint8_t> makePanasonicC8MissingTablesTiff();
std::vector<std::uint8_t> makePanasonicC8Tag40CountTiff(
    std::uint16_t count);
std::vector<std::uint8_t> makePanasonicC8Tag41CountTiff(
    std::uint16_t count);
std::vector<std::uint8_t> makePanasonicC8StripeCountMismatchTiff();
std::vector<std::uint8_t> makePanasonicC8BorrowedBitsTiff();
std::vector<std::uint8_t> makePanasonicC8NegativeShiftTiff();
std::vector<std::uint8_t> makePanasonicC8SignedShiftTiff();
std::vector<std::uint8_t> makePanasonicC8Tag41Shift64Tiff();
std::vector<std::uint8_t> makePanasonicC8NoMatchTiff();
std::vector<std::uint8_t> makePanasonicC8OverrangeTableTiff();
std::vector<std::uint8_t> makePanasonicC8HlowZeroTiff();
std::vector<std::uint8_t> makePanasonicC8Hlow17Tiff();
std::vector<std::uint8_t> makePanasonicC8CodeAtLimitTiff();
std::vector<std::uint8_t> makePanasonicC8CodeAboveStoredDomainTiff();
std::vector<std::uint8_t> makePanasonicC8WideStripeLeftTiff();
std::vector<std::uint8_t> makeValidPanasonicC8Tiff();
std::vector<std::uint8_t> makeValidTwoStripePanasonicC8Tiff();
std::vector<std::uint8_t> makeValidShadowedPanasonicC8Tiff();
std::vector<std::uint8_t> makeOverlappingPanasonicC8Tiff();

// A tiny valid uncompressed CFA DNG used to prove that the regression target
// actually reaches dcraw_process (including the optional wavelet path).
std::vector<std::uint8_t> makeValidUncompressedDng(
    std::uint32_t width = 256U, std::uint32_t height = 256U);

// A structurally valid uncompressed DNG whose declared raw plane exceeds the
// production decoder's immediate in-memory pixel budget. The strip itself is
// intentionally tiny: decodeFromBuffer() must reject from metadata immediately
// after open_buffer(), before unpack() can allocate from the hostile dimensions.
std::vector<std::uint8_t> makeDeclaredOversizeUncompressedDng(
    std::uint32_t width, std::uint32_t height);

// A recognized DNG with an embedded ICC payload of exactly [profileBytes].
// Used to prove LibRaw's compile-time mobile profile ceiling is active during
// open_buffer(), before the production wrapper can inspect dimensions.
std::vector<std::uint8_t> makeEmbeddedProfileDng(std::size_t profileBytes);

// Hostile TIFF metadata tables that reuse one small backing payload. The first
// repeats OpcodeList1; the second declares XMP plus all three opcode lists whose
// aggregate decoded metadata exceeds the patched 16 MiB budget.
std::vector<std::uint8_t> makeDuplicateOpcodeDng();
std::vector<std::uint8_t> makeOverBudgetMetadataDng();
std::vector<std::uint8_t> makeDuplicateStripTablesDng();
std::vector<std::uint8_t> makeValidMultiStripDng();

// Exactly 12 MiPixels before processing, but DefaultScale=1/10 asks LibRaw's
// stretch() stage for ten times as many output rows.
std::vector<std::uint8_t> makeExtremeAspectDng();

// DefaultScale produces values immediately outside LibRaw's open (0.995,
// 1.005) normalization interval and therefore still reaches stretch().
std::vector<std::uint8_t> makeBoundaryAspectDng(bool belowOne);

// Exactly 12 MiPixels in both raw and visible dimensions, but ActiveArea adds
// margins that make LibRaw's adjusted pre-unpack raw-store geometry much larger.
std::vector<std::uint8_t> makeHostileActiveAreaDng();

std::string describe(const DecodeOutcome& outcome);

}  // namespace sfraw::hosttest

#endif  // SFRAW_PUBLIC_API_TEST_SUPPORT_H
