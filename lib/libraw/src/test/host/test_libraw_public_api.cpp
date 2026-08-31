/*
 * Spektrafilm Android -- LibRaw 0.22.2 hostile-input public-seam regressions.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "public_api_test_support.h"

#include <libraw/libraw.h>
#include <libraw/libraw_version.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& label,
           const std::string& detail = {}) {
  if (condition) {
    std::cout << "ok   " << label;
  } else {
    std::cout << "FAIL " << label;
    ++failures;
  }
  if (!detail.empty()) std::cout << " (" << detail << ')';
  std::cout << '\n';
}

std::uint64_t rawPlaneDigest(const std::uint16_t* pixels,
                             std::size_t count) {
  constexpr std::uint64_t kOffset = 1469598103934665603ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  if (pixels == nullptr) return 0U;
  std::uint64_t digest = kOffset;
  for (std::size_t index = 0U; index < count; ++index) {
    digest ^= pixels[index] & 0xffU;
    digest *= kPrime;
    digest ^= pixels[index] >> 8U;
    digest *= kPrime;
  }
  return digest;
}

std::uint16_t readLittle16(const std::vector<std::uint8_t>& bytes,
                           std::size_t offset) {
  return static_cast<std::uint16_t>(bytes.at(offset)) |
      static_cast<std::uint16_t>(bytes.at(offset + 1U) << 8U);
}

std::uint32_t readLittle32(const std::vector<std::uint8_t>& bytes,
                           std::size_t offset) {
  return static_cast<std::uint32_t>(bytes.at(offset)) |
      (static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U) |
      (static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 16U) |
      (static_cast<std::uint32_t>(bytes.at(offset + 3U)) << 24U);
}

struct PanasonicStripeRange {
  std::size_t begin = 0U;
  std::size_t byteCount = 0U;
  bool valid = false;
};

PanasonicStripeRange firstPanasonicStripeRange(
    const std::vector<std::uint8_t>& bytes) {
  const std::size_t ifd = readLittle32(bytes, 4U);
  const std::uint16_t entryCount = readLittle16(bytes, ifd);
  std::size_t offsets = 0U;
  std::size_t compressedBits = 0U;
  for (std::uint16_t index = 0U; index < entryCount; ++index) {
    const std::size_t entry = ifd + 2U + index * 12U;
    const std::uint16_t tag = readLittle16(bytes, entry);
    if (tag == 0x0044U) offsets = readLittle32(bytes, entry + 8U);
    if (tag == 0x0046U) compressedBits = readLittle32(bytes, entry + 8U);
  }
  if (offsets == 0U || compressedBits == 0U) return {};
  const std::uint32_t bits = readLittle32(bytes, compressedBits + 2U);
  const std::size_t begin = readLittle32(bytes, offsets + 2U);
  const std::size_t byteCount = (static_cast<std::size_t>(bits) + 7U) / 8U;
  return {begin, byteCount,
          begin <= bytes.size() && byteCount <= bytes.size() - begin};
}

class PanasonicRangeDatastream final : public LibRaw_buffer_datastream {
 public:
  PanasonicRangeDatastream(const std::vector<std::uint8_t>& bytes,
                           PanasonicStripeRange watched)
      : LibRaw_buffer_datastream(bytes.data(), bytes.size()), watched_(watched) {}

  int read(void* destination, std::size_t size,
           std::size_t count) override {
    const INT64 signedStart = tell();
    if (watched_.valid && signedStart >= 0 &&
        static_cast<std::size_t>(signedStart) == watched_.begin) {
      sawBoundaryRead_ = true;
      const std::size_t requested = size != 0U && count > SIZE_MAX / size
          ? SIZE_MAX
          : size * count;
      if (requested > largestBoundaryRead_) largestBoundaryRead_ = requested;
      if (requested > watched_.byteCount) crossedBoundary_ = true;
    }
    return LibRaw_buffer_datastream::read(destination, size, count);
  }

  bool sawBoundaryRead() const { return sawBoundaryRead_; }
  bool crossedBoundary() const { return crossedBoundary_; }
  std::size_t largestBoundaryRead() const { return largestBoundaryRead_; }

 private:
  PanasonicStripeRange watched_;
  bool sawBoundaryRead_ = false;
  bool crossedBoundary_ = false;
  std::size_t largestBoundaryRead_ = 0U;
};

}  // namespace

static_assert(LIBRAW_MAJOR_VERSION == 0, "Unexpected LibRaw major version");
static_assert(LIBRAW_MINOR_VERSION == 22, "Host tests require LibRaw 0.22.x");
static_assert(LIBRAW_PATCH_VERSION == 2, "Host tests require LibRaw 0.22.2");

int main(int argc, char** argv) {
  using sfraw::hosttest::DecodeStage;
  using sfraw::hosttest::describe;
  using sfraw::hosttest::exercisePublicDecode;
  using sfraw::hosttest::makeMalformedLosslessJpegDng;
  using sfraw::hosttest::makeNonTerminatedIdentifyHead;
  using sfraw::hosttest::makeNonTerminatedMakerNoteTiff;
  using sfraw::hosttest::makePxnIdentifyHead;
  using sfraw::hosttest::makeTailDpSignatureX3f;
  using sfraw::hosttest::makeTailSdSignatureX3f;
  using sfraw::hosttest::makeExcessiveTileCountLosslessJpegDng;
  using sfraw::hosttest::makeExcessiveTileCountUncompressedDng;
  using sfraw::hosttest::makeCompliantTiledLosslessJpegDng;
  using sfraw::hosttest::makeExcessiveSonyTileCountTiff;
  using sfraw::hosttest::makeInvalidSonyLjpegComponentsTiff;
  using sfraw::hosttest::makeDuplicateDhtSonyTiff;
  using sfraw::hosttest::makeTerminalDhtOverflowSonyTiff;
  using sfraw::hosttest::makeTruncatedDhtSonyTiff;
  using sfraw::hosttest::makeOversubscribedDhtSonyTiff;
  using sfraw::hosttest::makeNegativeEffectiveBitsSonyTiff;
  using sfraw::hosttest::makeBoundarySonyTileCountTiff;
  using sfraw::hosttest::makeCumulativeDhtWorkSonyTiff;
  using sfraw::hosttest::makeCumulativeMarkerWorkSonyTiff;
  using sfraw::hosttest::makeValidIdentifyLjpegTiff;
  using sfraw::hosttest::makeRepeatedIdentifyLjpegTiff;
  using sfraw::hosttest::makeDistinctTileFloatDeflateDng;
  using sfraw::hosttest::makeFloat32DngWithoutWhiteLevel;
  using sfraw::hosttest::makeInteger31DngWithoutWhiteLevel;
  using sfraw::hosttest::makeRepeatedTileFloatDeflateDng;
  using sfraw::hosttest::makeSamsungInvalidBlackShiftTiff;
  using sfraw::hosttest::makeSamsungBoundaryBlackShiftTiff;
  using sfraw::hosttest::makeSonyInvalidBlackShiftTiff;
  using sfraw::hosttest::makeSonyBoundaryBlackShiftTiff;
  using sfraw::hosttest::makeHasselbladTiff;
  using sfraw::hosttest::makeOverflowingHasselbladPredictorTiff;
  using sfraw::hosttest::makeOlympusTag641BoundaryTiff;
  using sfraw::hosttest::makeOlympusUnaryWorkTiff;
  using sfraw::hosttest::makeOlympusNegativePredictorTiff;
  using sfraw::hosttest::makePanasonicC8MissingTablesTiff;
  using sfraw::hosttest::makePanasonicC8Tag40CountTiff;
  using sfraw::hosttest::makePanasonicC8Tag41CountTiff;
  using sfraw::hosttest::makePanasonicC8StripeCountMismatchTiff;
  using sfraw::hosttest::makePanasonicC8BorrowedBitsTiff;
  using sfraw::hosttest::makePanasonicC8NegativeShiftTiff;
  using sfraw::hosttest::makePanasonicC8SignedShiftTiff;
  using sfraw::hosttest::makePanasonicC8Tag41Shift64Tiff;
  using sfraw::hosttest::makePanasonicC8NoMatchTiff;
  using sfraw::hosttest::makePanasonicC8OverrangeTableTiff;
  using sfraw::hosttest::makePanasonicC8HlowZeroTiff;
  using sfraw::hosttest::makePanasonicC8Hlow17Tiff;
  using sfraw::hosttest::makePanasonicC8CodeAtLimitTiff;
  using sfraw::hosttest::makePanasonicC8CodeAboveStoredDomainTiff;
  using sfraw::hosttest::makePanasonicC8WideStripeLeftTiff;
  using sfraw::hosttest::makeValidPanasonicC8Tiff;
  using sfraw::hosttest::makeValidTwoStripePanasonicC8Tiff;
  using sfraw::hosttest::makeValidShadowedPanasonicC8Tiff;
  using sfraw::hosttest::makeOverlappingPanasonicC8Tiff;
  using sfraw::hosttest::makeHostileCr2SliceTiff;
  using sfraw::hosttest::makeOverflowingCr2SliceTiff;
  using sfraw::hosttest::makeOversizedLjpegGeometryTiff;
  using sfraw::hosttest::makeOversizedLjpegGeometryDng;
  using sfraw::hosttest::makeTruncatedLosslessJpegStrip;
  using sfraw::hosttest::makeValidLosslessJpegDng;
  using sfraw::hosttest::makeSof1ZrlDng;
  using sfraw::hosttest::makeSof1DcOverflowDng;
  using sfraw::hosttest::makeSof1AcOverrunDng;
  using sfraw::hosttest::makeValidSonyLjpegTiff;
  using sfraw::hosttest::makeValidCanonSrawTiff;
  using sfraw::hosttest::makeNegativeChromaCanonSrawTiff;
  using sfraw::hosttest::makeShortSofCanonSrawTiff;
  using sfraw::hosttest::makeRedundantSlicesCanonSrawTiff;
  using sfraw::hosttest::makeLargeModelVersionCanonSrawTiff;
  using sfraw::hosttest::makeValidCanonSrawWhiteBalanceTiff;
  using sfraw::hosttest::makeOverflowingCanonSrawWhiteBalanceTiff;
  using sfraw::hosttest::makeValidUncompressedDng;
  using sfraw::hosttest::readHexFixture;

  if (argc != 2) {
    std::cerr << "usage: sfraw_libraw_public_api_test <corpus-directory>\n";
    return 2;
  }

  check(LibRaw::versionNumber() == LIBRAW_MAKE_VERSION(0, 22, 2),
        "runtime LibRaw is exactly 0.22.2", LibRaw::version());

  const auto decoderNameFor = [](const std::vector<std::uint8_t>& input) {
    auto raw = std::make_unique<LibRaw>();
    if (raw->open_buffer(input.data(), input.size()) != LIBRAW_SUCCESS) {
      return std::string{};
    }
    const char* decoder = raw->unpack_function_name();
    return decoder == nullptr ? std::string{} : std::string(decoder);
  };

  // GitHub LibRaw issue #844: a 64-byte TIFF NewSubfileType value is parsed as
  // an out-of-range float-to-int conversion in unpatched 0.22.2. Under UBSan,
  // open_buffer used to trap. The reviewed local patch must reject it cleanly.
  const std::filesystem::path corpus(argv[1]);
  const std::vector<std::uint8_t> issue844 = readHexFixture(
      corpus / "tiff-newsubfiletype-float-overflow-844.hex");
  constexpr std::array<std::uint8_t, 64> kIssue844Expected{{
      0x49U, 0x49U, 0x2aU, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U,
      0x01U, 0x00U, 0xfeU, 0x00U, 0x04U, 0x00U, 0x01U, 0x00U,
      0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0xe3U, 0x07U, 0x1bU,
      0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
      0x00U, 0x00U, 0x00U, 0x17U, 0x00U, 0x00U, 0x00U, 0x00U,
      0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x00U,
      0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
      0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  }};
  check(issue844.size() == kIssue844Expected.size() &&
            std::equal(issue844.begin(), issue844.end(),
                       kIssue844Expected.begin()),
        "#844 fixture is the authoritative 64 bytes");
  const auto issue844Outcome = exercisePublicDecode(issue844);
  check(issue844Outcome.openCode != LIBRAW_SUCCESS,
        "#844 hostile TIFF fails safely in open_buffer",
        describe(issue844Outcome));

  const std::vector<std::uint8_t> pxnHead = makePxnIdentifyHead();
  auto pxnProbe = std::make_unique<LibRaw>();
  const int pxnOpen = pxnProbe->open_buffer(pxnHead.data(), pxnHead.size());
  check(pxnOpen == LIBRAW_SUCCESS &&
            std::string(pxnProbe->imgdata.idata.make) == "Logitech" &&
            std::string(pxnProbe->imgdata.idata.model) == "Fotoman Pixtura",
        "NUL-terminated PXN identify signature remains recognized",
        "open=" + std::to_string(pxnOpen) +
            " make=" + pxnProbe->imgdata.idata.make +
            " model=" + pxnProbe->imgdata.idata.model);

  const auto identifyHeadOutcome =
      exercisePublicDecode(makeNonTerminatedIdentifyHead());
  check(identifyHeadOutcome.openCode != LIBRAW_SUCCESS &&
            identifyHeadOutcome.terminalStage == DecodeStage::kOpen,
        "non-terminated identify header fails without a stack over-read",
        describe(identifyHeadOutcome));

  const auto makerNoteOutcome =
      exercisePublicDecode(makeNonTerminatedMakerNoteTiff());
  check(makerNoteOutcome.openCode != LIBRAW_SUCCESS &&
            makerNoteOutcome.terminalStage == DecodeStage::kOpen,
        "non-terminated MakerNote header fails without a stack over-read",
        describe(makerNoteOutcome));

  const auto checkX3fTailSignature = [](
      const std::vector<std::uint8_t>& input, const std::string& expectedModel,
      const std::string& label) {
    auto probe = std::make_unique<LibRaw>();
    const int openCode = probe->open_buffer(input.data(), input.size());
    check(openCode == LIBRAW_SUCCESS &&
              std::string(probe->imgdata.idata.make) == "Sigma" &&
              std::string(probe->imgdata.idata.model) == expectedModel,
          label,
          "open=" + std::to_string(openCode) +
              " make=" + probe->imgdata.idata.make +
              " model=" + probe->imgdata.idata.model);
  };
  checkX3fTailSignature(makeTailDpSignatureX3f(), "sd Quattro",
                        "X3F tail dp signature uses bounded fallback model");
  checkX3fTailSignature(makeTailSdSignatureX3f(), "sd Quatt",
                        "X3F tail sd signature uses bounded model copy");

  // CVE-2026-21413 / TALOS-2026-2331: CR2Slice can map a decoded JPEG
  // column beyond raw_width. Upstream 0.22.2 deliberately skips the invalid
  // store rather than rejecting the whole image, so successful processing
  // under ASan is the regression oracle; unpatched code writes beyond
  // raw_image before it can return.
  const std::vector<std::uint8_t> hostileColumn = makeHostileCr2SliceTiff();
  auto hostileRouteProbe = std::make_unique<LibRaw>();
  const int hostileRouteOpen =
      hostileRouteProbe->open_buffer(hostileColumn.data(), hostileColumn.size());
  const char* hostileDecoder = hostileRouteOpen == LIBRAW_SUCCESS
      ? hostileRouteProbe->unpack_function_name()
      : nullptr;
  check(hostileRouteOpen == LIBRAW_SUCCESS && hostileDecoder != nullptr &&
            std::string(hostileDecoder) == "lossless_jpeg_load_raw()",
        "hostile CR2Slice fixture selects the vulnerable decoder seam",
        hostileDecoder == nullptr ? "no decoder" : hostileDecoder);
  const auto hostileColumnOutcome = exercisePublicDecode(hostileColumn);
  check(hostileColumnOutcome.openCode == LIBRAW_SUCCESS &&
            hostileColumnOutcome.unpackCode == LIBRAW_SUCCESS &&
            hostileColumnOutcome.processCode == LIBRAW_SUCCESS,
        "hostile CR2Slice column is bounded without an out-of-range store",
        describe(hostileColumnOutcome));

  const std::vector<std::uint8_t> validCanonSraw = makeValidCanonSrawTiff();
  auto validCanonRouteProbe = std::make_unique<LibRaw>();
  const int validCanonOpen = validCanonRouteProbe->open_buffer(
      validCanonSraw.data(), validCanonSraw.size());
  const char* validCanonDecoder = validCanonOpen == LIBRAW_SUCCESS
      ? validCanonRouteProbe->unpack_function_name()
      : nullptr;
  check(validCanonOpen == LIBRAW_SUCCESS && validCanonDecoder != nullptr &&
            std::string(validCanonDecoder) == "canon_sraw_load_raw()",
        "Canon/BPS15 control selects the sRAW decoder seam",
        validCanonDecoder == nullptr ? "no decoder" : validCanonDecoder);
  const auto validCanonOutcome = exercisePublicDecode(validCanonSraw);
  check(validCanonOutcome.openCode == LIBRAW_SUCCESS &&
            validCanonOutcome.unpackCode == LIBRAW_SUCCESS,
        "valid Canon sRAW control remains supported",
        describe(validCanonOutcome));

  const auto negativeChromaOutcome =
      exercisePublicDecode(makeNegativeChromaCanonSrawTiff());
  check(negativeChromaOutcome.openCode == LIBRAW_SUCCESS &&
            negativeChromaOutcome.unpackCode == LIBRAW_SUCCESS,
        "negative Canon sRAW chroma uses defined arithmetic",
        describe(negativeChromaOutcome));

  const auto shortSofOutcome =
      exercisePublicDecode(makeShortSofCanonSrawTiff());
  check(shortSofOutcome.openCode == LIBRAW_SUCCESS &&
            shortSofOutcome.unpackCode != LIBRAW_SUCCESS &&
            shortSofOutcome.terminalStage == DecodeStage::kUnpack,
        "Canon sRAW cannot decode rows beyond the embedded SOF height",
        describe(shortSofOutcome));

  const auto redundantSlicesOutcome =
      exercisePublicDecode(makeRedundantSlicesCanonSrawTiff());
  check(redundantSlicesOutcome.openCode == LIBRAW_SUCCESS &&
            redundantSlicesOutcome.unpackCode == LIBRAW_SUCCESS,
        "empty Canon CR2Slice ranges terminate without replay",
        describe(redundantSlicesOutcome));

  const auto largeModelVersionOutcome =
      exercisePublicDecode(makeLargeModelVersionCanonSrawTiff());
  check(largeModelVersionOutcome.openCode == LIBRAW_SUCCESS &&
            largeModelVersionOutcome.unpackCode == LIBRAW_SUCCESS,
        "large Canon firmware components use bounded version arithmetic",
        describe(largeModelVersionOutcome));

  const std::vector<std::uint8_t> validCanonWhiteBalance =
      makeValidCanonSrawWhiteBalanceTiff();
  auto validCanonWhiteBalanceProbe = std::make_unique<LibRaw>();
  const int validCanonWhiteBalanceOpen =
      validCanonWhiteBalanceProbe->open_buffer(validCanonWhiteBalance.data(),
                                               validCanonWhiteBalance.size());
  const char* validCanonWhiteBalanceDecoder =
      validCanonWhiteBalanceOpen == LIBRAW_SUCCESS
          ? validCanonWhiteBalanceProbe->unpack_function_name()
          : nullptr;
  check(validCanonWhiteBalanceOpen == LIBRAW_SUCCESS &&
            validCanonWhiteBalanceDecoder != nullptr &&
            std::string(validCanonWhiteBalanceDecoder) ==
                "canon_sraw_load_raw()",
        "bounded Canon MakerNote white balance preserves the sRAW route",
        validCanonWhiteBalanceDecoder == nullptr
            ? "no decoder"
            : validCanonWhiteBalanceDecoder);
  const auto overflowingCanonWhiteBalanceOutcome =
      exercisePublicDecode(makeOverflowingCanonSrawWhiteBalanceTiff());
  check(overflowingCanonWhiteBalanceOutcome.openCode != LIBRAW_SUCCESS &&
            overflowingCanonWhiteBalanceOutcome.terminalStage ==
                DecodeStage::kOpen,
        "overflowing Canon MakerNote white balance fails during identify",
        describe(overflowingCanonWhiteBalanceOutcome));

  const std::vector<std::uint8_t> overflowingSlice =
      makeOverflowingCr2SliceTiff();
  auto overflowingSliceRouteProbe = std::make_unique<LibRaw>();
  const int overflowingSliceOpen = overflowingSliceRouteProbe->open_buffer(
      overflowingSlice.data(), overflowingSlice.size());
  const char* overflowingSliceDecoder = overflowingSliceOpen == LIBRAW_SUCCESS
      ? overflowingSliceRouteProbe->unpack_function_name()
      : nullptr;
  check(overflowingSliceOpen == LIBRAW_SUCCESS &&
            overflowingSliceDecoder != nullptr &&
            std::string(overflowingSliceDecoder) ==
                "lossless_jpeg_load_raw()",
        "wide CR2Slice fixture selects generic lossless-JPEG",
        overflowingSliceDecoder == nullptr ? "no decoder"
                                            : overflowingSliceDecoder);
  const auto overflowingSliceOutcome = exercisePublicDecode(overflowingSlice);
  check(overflowingSliceOutcome.openCode == LIBRAW_SUCCESS &&
            overflowingSliceOutcome.unpackCode == LIBRAW_SUCCESS,
        "CR2Slice index arithmetic stays defined at 65535-wide slices",
        describe(overflowingSliceOutcome));

  const std::vector<std::uint8_t> identifyControl =
      makeValidIdentifyLjpegTiff();
  auto identifyControlProbe = std::make_unique<LibRaw>();
  const int identifyControlOpen = identifyControlProbe->open_buffer(
      identifyControl.data(), identifyControl.size());
  const char* identifyControlDecoder = identifyControlOpen == LIBRAW_SUCCESS
      ? identifyControlProbe->unpack_function_name()
      : nullptr;
  check(identifyControlOpen == LIBRAW_SUCCESS &&
            identifyControlDecoder != nullptr &&
            std::string(identifyControlDecoder) == "sony_ljpeg_load_raw()",
        "single marker-heavy identify sniff preserves the Sony route",
        identifyControlDecoder == nullptr ? "no decoder"
                                         : identifyControlDecoder);
  const auto identifyReplayOutcome =
      exercisePublicDecode(makeRepeatedIdentifyLjpegTiff());
  check(identifyReplayOutcome.openCode == LIBRAW_TOO_BIG &&
            identifyReplayOutcome.terminalStage == DecodeStage::kOpen,
        "repeated lossless-JPEG identify work is globally bounded",
        describe(identifyReplayOutcome));

  const std::vector<std::uint8_t> distinctDeflate =
      makeDistinctTileFloatDeflateDng();
  auto distinctDeflateProbe = std::make_unique<LibRaw>();
  const int distinctDeflateOpen = distinctDeflateProbe->open_buffer(
      distinctDeflate.data(), distinctDeflate.size());
  const char* distinctDeflateDecoder = distinctDeflateOpen == LIBRAW_SUCCESS
      ? distinctDeflateProbe->unpack_function_name()
      : nullptr;
  check(distinctDeflateOpen == LIBRAW_SUCCESS &&
            distinctDeflateDecoder != nullptr &&
            std::string(distinctDeflateDecoder) == "deflate_dng_load_raw()",
        "distinct tiled float DNG selects the deflate decoder seam",
        distinctDeflateDecoder == nullptr ? "no decoder"
                                          : distinctDeflateDecoder);
  const auto distinctDeflateOutcome = exercisePublicDecode(distinctDeflate);
  check(distinctDeflateOutcome.openCode == LIBRAW_SUCCESS &&
            distinctDeflateOutcome.unpackCode == LIBRAW_SUCCESS,
        "ordinary distinct float/deflate tiles remain supported",
        describe(distinctDeflateOutcome));

  const std::vector<std::uint8_t> noWhiteLevelFloat =
      makeFloat32DngWithoutWhiteLevel();
  auto noWhiteLevelProbe = std::make_unique<LibRaw>();
  const int noWhiteLevelOpen = noWhiteLevelProbe->open_buffer(
      noWhiteLevelFloat.data(), noWhiteLevelFloat.size());
  check(noWhiteLevelOpen == LIBRAW_SUCCESS &&
            noWhiteLevelProbe->imgdata.color.maximum == 1U,
        "float32 DNG without WhiteLevel defaults maximum without shift UB",
        "open=" + std::to_string(noWhiteLevelOpen) +
            " maximum=" +
            std::to_string(noWhiteLevelProbe->imgdata.color.maximum));

  const std::vector<std::uint8_t> integer31NoWhite =
      makeInteger31DngWithoutWhiteLevel();
  auto integer31Probe = std::make_unique<LibRaw>();
  const int integer31Open = integer31Probe->open_buffer(
      integer31NoWhite.data(), integer31NoWhite.size());
  check(integer31Open != LIBRAW_SUCCESS,
        "integer BitsPerSample=31 rejects without default-WhiteLevel shift UB",
        "open=" + std::to_string(integer31Open));

  const auto sonyShiftOutcome =
      exercisePublicDecode(makeSonyInvalidBlackShiftTiff());
  check(sonyShiftOutcome.openCode != LIBRAW_SUCCESS &&
            sonyShiftOutcome.terminalStage == DecodeStage::kOpen,
        "Sony BPS11 metadata rejects before deriving a negative-shift black level",
        describe(sonyShiftOutcome));

  const auto samsungShiftOutcome =
      exercisePublicDecode(makeSamsungInvalidBlackShiftTiff());
  check(samsungShiftOutcome.openCode != LIBRAW_SUCCESS &&
            samsungShiftOutcome.terminalStage == DecodeStage::kOpen,
        "Samsung BPS6 metadata rejects before deriving a negative-shift black level",
        describe(samsungShiftOutcome));

  const std::vector<std::uint8_t> sonyBoundary =
      makeSonyBoundaryBlackShiftTiff();
  auto sonyBoundaryProbe = std::make_unique<LibRaw>();
  const int sonyBoundaryOpen = sonyBoundaryProbe->open_buffer(
      sonyBoundary.data(), sonyBoundary.size());
  check(sonyBoundaryOpen == LIBRAW_SUCCESS &&
            std::string(sonyBoundaryProbe->imgdata.idata.make) == "Sony" &&
            sonyBoundaryProbe->imgdata.sizes.raw_width == 4000U &&
            sonyBoundaryProbe->imgdata.color.black == 128U &&
            sonyBoundaryProbe->imgdata.color.maximum == 4095U &&
            sonyBoundaryProbe->unpack_function_name() != nullptr &&
            std::string(sonyBoundaryProbe->unpack_function_name()) ==
                "sony_ljpeg_load_raw()",
        "Sony BPS12 boundary preserves vendor identification and black level",
        "open=" + std::to_string(sonyBoundaryOpen) +
            " black=" + std::to_string(sonyBoundaryProbe->imgdata.color.black));

  const std::vector<std::uint8_t> samsungBoundary =
      makeSamsungBoundaryBlackShiftTiff();
  auto samsungBoundaryProbe = std::make_unique<LibRaw>();
  const int samsungBoundaryOpen = samsungBoundaryProbe->open_buffer(
      samsungBoundary.data(), samsungBoundary.size());
  check(samsungBoundaryOpen == LIBRAW_SUCCESS &&
            std::string(samsungBoundaryProbe->imgdata.idata.make) == "Samsung" &&
            samsungBoundaryProbe->imgdata.sizes.raw_width == 6496U &&
            samsungBoundaryProbe->imgdata.color.black == 1U &&
            samsungBoundaryProbe->imgdata.color.maximum == 127U &&
            samsungBoundaryProbe->unpack_function_name() != nullptr &&
            std::string(samsungBoundaryProbe->unpack_function_name()) ==
                "lossless_jpeg_load_raw()",
        "Samsung BPS7 boundary preserves vendor identification and black level",
        "open=" + std::to_string(samsungBoundaryOpen) +
            " black=" +
            std::to_string(samsungBoundaryProbe->imgdata.color.black));

  const auto repeatedDeflateOutcome =
      exercisePublicDecode(makeRepeatedTileFloatDeflateDng());
  check(repeatedDeflateOutcome.openCode == LIBRAW_SUCCESS &&
            repeatedDeflateOutcome.unpackCode == LIBRAW_TOO_BIG &&
            repeatedDeflateOutcome.terminalStage == DecodeStage::kUnpack,
        "repeated float/deflate tile bytes are cumulatively bounded",
        describe(repeatedDeflateOutcome));

  const auto oversizedLjpegOutcome =
      exercisePublicDecode(makeOversizedLjpegGeometryTiff());
  check(oversizedLjpegOutcome.openCode == LIBRAW_SUCCESS &&
            oversizedLjpegOutcome.unpackCode == LIBRAW_TOO_BIG &&
            oversizedLjpegOutcome.terminalStage == DecodeStage::kUnpack,
        "oversized embedded lossless-JPEG work is rejected before decode loops",
        describe(oversizedLjpegOutcome));

  const std::vector<std::uint8_t> oversizedDng =
      makeOversizedLjpegGeometryDng();
  auto oversizedDngRouteProbe = std::make_unique<LibRaw>();
  const int oversizedDngOpen =
      oversizedDngRouteProbe->open_buffer(oversizedDng.data(), oversizedDng.size());
  const char* oversizedDngDecoder = oversizedDngOpen == LIBRAW_SUCCESS
      ? oversizedDngRouteProbe->unpack_function_name()
      : nullptr;
  check(oversizedDngOpen == LIBRAW_SUCCESS && oversizedDngDecoder != nullptr &&
            std::string(oversizedDngDecoder) == "lossless_dng_load_raw()",
        "oversized DNG fixture selects the lossless-DNG decoder seam",
        oversizedDngDecoder == nullptr ? "no decoder" : oversizedDngDecoder);
  const auto oversizedDngOutcome = exercisePublicDecode(oversizedDng);
  check(oversizedDngOutcome.openCode == LIBRAW_SUCCESS &&
            oversizedDngOutcome.unpackCode == LIBRAW_TOO_BIG &&
            oversizedDngOutcome.terminalStage == DecodeStage::kUnpack,
        "oversized lossless-DNG work is rejected before tile decode loops",
        describe(oversizedDngOutcome));

  const std::vector<std::uint8_t> compliantTiles =
      makeCompliantTiledLosslessJpegDng();
  auto compliantTilesRouteProbe = std::make_unique<LibRaw>();
  const int compliantTilesOpen = compliantTilesRouteProbe->open_buffer(
      compliantTiles.data(), compliantTiles.size());
  const char* compliantTilesDecoder = compliantTilesOpen == LIBRAW_SUCCESS
      ? compliantTilesRouteProbe->unpack_function_name()
      : nullptr;
  check(compliantTilesOpen == LIBRAW_SUCCESS &&
            compliantTilesDecoder != nullptr &&
            std::string(compliantTilesDecoder) == "lossless_dng_load_raw()",
        "16x16 tiled fixture selects the lossless-DNG decoder seam",
        compliantTilesDecoder == nullptr ? "no decoder" : compliantTilesDecoder);
  const auto compliantTilesOutcome = exercisePublicDecode(compliantTiles);
  check(compliantTilesOutcome.openCode == LIBRAW_SUCCESS &&
            compliantTilesOutcome.unpackCode == LIBRAW_SUCCESS &&
            compliantTilesOutcome.processCode == LIBRAW_SUCCESS,
        "more than 4096 TIFF-compliant 16x16 tiles remain supported",
        describe(compliantTilesOutcome));

  const std::vector<std::uint8_t> excessiveTiles =
      makeExcessiveTileCountLosslessJpegDng();
  auto excessiveTilesRouteProbe = std::make_unique<LibRaw>();
  const int excessiveTilesOpen = excessiveTilesRouteProbe->open_buffer(
      excessiveTiles.data(), excessiveTiles.size());
  const char* excessiveTilesDecoder = excessiveTilesOpen == LIBRAW_SUCCESS
      ? excessiveTilesRouteProbe->unpack_function_name()
      : nullptr;
  check(excessiveTilesOpen == LIBRAW_SUCCESS &&
            excessiveTilesDecoder != nullptr &&
            std::string(excessiveTilesDecoder) == "lossless_dng_load_raw()",
        "tiny-tile DNG selects the lossless-DNG decoder seam",
        excessiveTilesDecoder == nullptr ? "no decoder" : excessiveTilesDecoder);
  const auto excessiveTilesOutcome = exercisePublicDecode(excessiveTiles);
  check(excessiveTilesOutcome.openCode == LIBRAW_SUCCESS &&
            excessiveTilesOutcome.unpackCode == LIBRAW_TOO_BIG &&
            excessiveTilesOutcome.terminalStage == DecodeStage::kUnpack,
        "excessive DNG tile streams are rejected before decoder setup",
        describe(excessiveTilesOutcome));

  const std::vector<std::uint8_t> excessivePackedTiles =
      makeExcessiveTileCountUncompressedDng();
  auto excessivePackedRouteProbe = std::make_unique<LibRaw>();
  const int excessivePackedOpen = excessivePackedRouteProbe->open_buffer(
      excessivePackedTiles.data(), excessivePackedTiles.size());
  const char* excessivePackedDecoder = excessivePackedOpen == LIBRAW_SUCCESS
      ? excessivePackedRouteProbe->unpack_function_name()
      : nullptr;
  check(excessivePackedOpen == LIBRAW_SUCCESS &&
            excessivePackedDecoder != nullptr &&
            std::string(excessivePackedDecoder) == "packed_dng_load_raw()",
        "tiny-tile uncompressed DNG selects the packed decoder seam",
        excessivePackedDecoder == nullptr ? "no decoder" : excessivePackedDecoder);
  const auto excessivePackedOutcome = exercisePublicDecode(excessivePackedTiles);
  check(excessivePackedOutcome.openCode == LIBRAW_SUCCESS &&
            excessivePackedOutcome.unpackCode == LIBRAW_TOO_BIG &&
            excessivePackedOutcome.terminalStage == DecodeStage::kUnpack,
        "excessive packed-DNG tile streams reject before decoder setup",
        describe(excessivePackedOutcome));

  const std::vector<std::uint8_t> excessiveSonyTiles =
      makeExcessiveSonyTileCountTiff();
  auto excessiveSonyRouteProbe = std::make_unique<LibRaw>();
  const int excessiveSonyOpen = excessiveSonyRouteProbe->open_buffer(
      excessiveSonyTiles.data(), excessiveSonyTiles.size());
  const char* excessiveSonyDecoder = excessiveSonyOpen == LIBRAW_SUCCESS
      ? excessiveSonyRouteProbe->unpack_function_name()
      : nullptr;
  check(excessiveSonyOpen == LIBRAW_SUCCESS && excessiveSonyDecoder != nullptr &&
            std::string(excessiveSonyDecoder) == "sony_ljpeg_load_raw()",
        "tiny-tile SONY TIFF selects the Sony lossless-JPEG seam",
        excessiveSonyDecoder == nullptr ? "no decoder" : excessiveSonyDecoder);
  const auto excessiveSonyOutcome = exercisePublicDecode(excessiveSonyTiles);
  check(excessiveSonyOutcome.openCode == LIBRAW_SUCCESS &&
            excessiveSonyOutcome.unpackCode == LIBRAW_TOO_BIG &&
            excessiveSonyOutcome.terminalStage == DecodeStage::kUnpack,
        "excessive Sony tile streams reject before decoder setup",
        describe(excessiveSonyOutcome));

  const std::vector<std::uint8_t> invalidSonyComponents =
      makeInvalidSonyLjpegComponentsTiff();
  auto invalidSonyRouteProbe = std::make_unique<LibRaw>();
  const int invalidSonyOpen = invalidSonyRouteProbe->open_buffer(
      invalidSonyComponents.data(), invalidSonyComponents.size());
  const char* invalidSonyDecoder = invalidSonyOpen == LIBRAW_SUCCESS
      ? invalidSonyRouteProbe->unpack_function_name()
      : nullptr;
  check(invalidSonyOpen == LIBRAW_SUCCESS && invalidSonyDecoder != nullptr &&
            std::string(invalidSonyDecoder) == "sony_ljpeg_load_raw()",
        "malformed-component SONY TIFF selects the hardened decoder seam",
        invalidSonyDecoder == nullptr ? "no decoder" : invalidSonyDecoder);
  const auto invalidSonyOutcome = exercisePublicDecode(invalidSonyComponents);
  check(invalidSonyOutcome.openCode == LIBRAW_SUCCESS &&
            invalidSonyOutcome.unpackCode != LIBRAW_SUCCESS &&
            invalidSonyOutcome.terminalStage == DecodeStage::kUnpack,
        "Sony decoder rejects a non-four-component lossless JPEG",
        describe(invalidSonyOutcome));

  const std::vector<std::uint8_t> validSony = makeValidSonyLjpegTiff();
  auto validSonyRouteProbe = std::make_unique<LibRaw>();
  const int validSonyOpen =
      validSonyRouteProbe->open_buffer(validSony.data(), validSony.size());
  const char* validSonyDecoder = validSonyOpen == LIBRAW_SUCCESS
      ? validSonyRouteProbe->unpack_function_name()
      : nullptr;
  check(validSonyOpen == LIBRAW_SUCCESS && validSonyDecoder != nullptr &&
            std::string(validSonyDecoder) == "sony_ljpeg_load_raw()",
        "valid SONY TIFF selects the hardened lossless-JPEG seam",
        validSonyDecoder == nullptr ? "no decoder" : validSonyDecoder);
  const auto validSonyOutcome = exercisePublicDecode(validSony);
  check(validSonyOutcome.openCode == LIBRAW_SUCCESS &&
            validSonyOutcome.unpackCode == LIBRAW_SUCCESS &&
            validSonyOutcome.processCode == LIBRAW_SUCCESS,
        "valid four-component Sony lossless JPEG completes processing",
        describe(validSonyOutcome));

  const std::vector<std::uint8_t> terminalDht =
      makeTerminalDhtOverflowSonyTiff();
  auto terminalDhtRouteProbe = std::make_unique<LibRaw>();
  const int terminalDhtOpen = terminalDhtRouteProbe->open_buffer(
      terminalDht.data(), terminalDht.size());
  const char* terminalDhtDecoder = terminalDhtOpen == LIBRAW_SUCCESS
      ? terminalDhtRouteProbe->unpack_function_name()
      : nullptr;
  check(terminalDhtOpen == LIBRAW_SUCCESS && terminalDhtDecoder != nullptr &&
            std::string(terminalDhtDecoder) == "sony_ljpeg_load_raw()",
        "terminal-DHT fixture selects the vulnerable parser seam",
        terminalDhtDecoder == nullptr ? "no decoder" : terminalDhtDecoder);
  const auto terminalDhtOutcome = exercisePublicDecode(terminalDht);
  check(terminalDhtOutcome.openCode == LIBRAW_SUCCESS &&
            terminalDhtOutcome.unpackCode != LIBRAW_SUCCESS &&
            terminalDhtOutcome.terminalStage == DecodeStage::kUnpack,
        "terminal truncated DHT fails without reading beyond its segment",
        describe(terminalDhtOutcome));

  const auto truncatedDhtOutcome = exercisePublicDecode(makeTruncatedDhtSonyTiff());
  check(truncatedDhtOutcome.openCode == LIBRAW_SUCCESS &&
            truncatedDhtOutcome.unpackCode != LIBRAW_SUCCESS &&
            truncatedDhtOutcome.terminalStage == DecodeStage::kUnpack,
        "DHT leaf count cannot exceed the remaining segment payload",
        describe(truncatedDhtOutcome));

  const auto duplicateDhtOutcome = exercisePublicDecode(makeDuplicateDhtSonyTiff());
  check(duplicateDhtOutcome.openCode == LIBRAW_SUCCESS &&
            duplicateDhtOutcome.unpackCode != LIBRAW_SUCCESS &&
            duplicateDhtOutcome.terminalStage == DecodeStage::kUnpack,
        "duplicate Huffman tables cannot amplify decoder allocations",
        describe(duplicateDhtOutcome));

  const std::vector<std::uint8_t> oversubscribedDht =
      makeOversubscribedDhtSonyTiff();
  auto oversubscribedRouteProbe = std::make_unique<LibRaw>();
  const int oversubscribedOpen = oversubscribedRouteProbe->open_buffer(
      oversubscribedDht.data(), oversubscribedDht.size());
  const char* oversubscribedDecoder = oversubscribedOpen == LIBRAW_SUCCESS
      ? oversubscribedRouteProbe->unpack_function_name()
      : nullptr;
  check(oversubscribedOpen == LIBRAW_SUCCESS &&
            oversubscribedDecoder != nullptr &&
            std::string(oversubscribedDecoder) == "sony_ljpeg_load_raw()",
        "oversubscribed-DHT fixture selects the hardened parser seam",
        oversubscribedDecoder == nullptr ? "no decoder" : oversubscribedDecoder);
  const auto oversubscribedOutcome = exercisePublicDecode(oversubscribedDht);
  check(oversubscribedOutcome.openCode == LIBRAW_SUCCESS &&
            oversubscribedOutcome.unpackCode != LIBRAW_SUCCESS &&
            oversubscribedOutcome.terminalStage == DecodeStage::kUnpack,
        "oversubscribed Huffman code space rejects before table expansion",
        describe(oversubscribedOutcome));

  const std::vector<std::uint8_t> negativeBits =
      makeNegativeEffectiveBitsSonyTiff();
  auto negativeBitsRouteProbe = std::make_unique<LibRaw>();
  const int negativeBitsOpen = negativeBitsRouteProbe->open_buffer(
      negativeBits.data(), negativeBits.size());
  const char* negativeBitsDecoder = negativeBitsOpen == LIBRAW_SUCCESS
      ? negativeBitsRouteProbe->unpack_function_name()
      : nullptr;
  check(negativeBitsOpen == LIBRAW_SUCCESS && negativeBitsDecoder != nullptr &&
            std::string(negativeBitsDecoder) == "sony_ljpeg_load_raw()",
        "negative-precision fixture selects the hardened parser seam",
        negativeBitsDecoder == nullptr ? "no decoder" : negativeBitsDecoder);
  const auto negativeBitsOutcome = exercisePublicDecode(negativeBits);
  check(negativeBitsOutcome.openCode == LIBRAW_SUCCESS &&
            negativeBitsOutcome.unpackCode != LIBRAW_SUCCESS &&
            negativeBitsOutcome.terminalStage == DecodeStage::kUnpack,
        "SOS point transform cannot make JPEG precision non-positive",
        describe(negativeBitsOutcome));

  const std::vector<std::uint8_t> boundarySonyTiles =
      makeBoundarySonyTileCountTiff();
  auto boundarySonyRouteProbe = std::make_unique<LibRaw>();
  const int boundarySonyOpen = boundarySonyRouteProbe->open_buffer(
      boundarySonyTiles.data(), boundarySonyTiles.size());
  const char* boundarySonyDecoder = boundarySonyOpen == LIBRAW_SUCCESS
      ? boundarySonyRouteProbe->unpack_function_name()
      : nullptr;
  check(boundarySonyOpen == LIBRAW_SUCCESS && boundarySonyDecoder != nullptr &&
            std::string(boundarySonyDecoder) == "sony_ljpeg_load_raw()",
        "4096-stream boundary fixture selects the Sony decoder seam",
        boundarySonyDecoder == nullptr ? "no decoder" : boundarySonyDecoder);
  const auto boundarySonyOutcome = exercisePublicDecode(boundarySonyTiles);
  check(boundarySonyOutcome.openCode == LIBRAW_SUCCESS &&
            boundarySonyOutcome.unpackCode == LIBRAW_SUCCESS &&
            boundarySonyOutcome.processCode == LIBRAW_SUCCESS,
        "4096 lightweight Sony tile streams remain supported",
        describe(boundarySonyOutcome));

  const std::vector<std::uint8_t> cumulativeDhtWork =
      makeCumulativeDhtWorkSonyTiff();
  auto cumulativeWorkRouteProbe = std::make_unique<LibRaw>();
  const int cumulativeWorkOpen = cumulativeWorkRouteProbe->open_buffer(
      cumulativeDhtWork.data(), cumulativeDhtWork.size());
  const char* cumulativeWorkDecoder = cumulativeWorkOpen == LIBRAW_SUCCESS
      ? cumulativeWorkRouteProbe->unpack_function_name()
      : nullptr;
  check(cumulativeWorkOpen == LIBRAW_SUCCESS &&
            cumulativeWorkDecoder != nullptr &&
            std::string(cumulativeWorkDecoder) == "sony_ljpeg_load_raw()",
        "setup-work fixture selects the Sony lossless-JPEG seam",
        cumulativeWorkDecoder == nullptr ? "no decoder" : cumulativeWorkDecoder);
  const auto cumulativeWorkOutcome = exercisePublicDecode(cumulativeDhtWork);
  check(cumulativeWorkOutcome.openCode == LIBRAW_SUCCESS &&
            cumulativeWorkOutcome.unpackCode == LIBRAW_TOO_BIG &&
            cumulativeWorkOutcome.terminalStage == DecodeStage::kUnpack,
        "cumulative valid Huffman setup work stops within the tile cap",
        describe(cumulativeWorkOutcome));

  const std::vector<std::uint8_t> cumulativeMarkerWork =
      makeCumulativeMarkerWorkSonyTiff();
  auto cumulativeMarkerRouteProbe = std::make_unique<LibRaw>();
  const int cumulativeMarkerOpen = cumulativeMarkerRouteProbe->open_buffer(
      cumulativeMarkerWork.data(), cumulativeMarkerWork.size());
  const char* cumulativeMarkerDecoder =
      cumulativeMarkerOpen == LIBRAW_SUCCESS
          ? cumulativeMarkerRouteProbe->unpack_function_name()
          : nullptr;
  check(cumulativeMarkerOpen == LIBRAW_SUCCESS &&
            cumulativeMarkerDecoder != nullptr &&
            std::string(cumulativeMarkerDecoder) == "sony_ljpeg_load_raw()",
        "repeated-marker fixture selects the Sony lossless-JPEG seam",
        cumulativeMarkerDecoder == nullptr ? "no decoder"
                                           : cumulativeMarkerDecoder);
  const auto cumulativeMarkerOutcome =
      exercisePublicDecode(cumulativeMarkerWork);
  check(cumulativeMarkerOutcome.openCode == LIBRAW_SUCCESS &&
            cumulativeMarkerOutcome.unpackCode == LIBRAW_TOO_BIG &&
            cumulativeMarkerOutcome.terminalStage == DecodeStage::kUnpack,
        "replayed lossless-JPEG marker payload work is cumulative",
        describe(cumulativeMarkerOutcome));

  const std::vector<std::uint8_t> oddHasselblad = makeHasselbladTiff(true);
  auto oddHasselbladRouteProbe = std::make_unique<LibRaw>();
  const int oddHasselbladOpen = oddHasselbladRouteProbe->open_buffer(
      oddHasselblad.data(), oddHasselblad.size());
  const char* oddHasselbladDecoder = oddHasselbladOpen == LIBRAW_SUCCESS
      ? oddHasselbladRouteProbe->unpack_function_name()
      : nullptr;
  check(oddHasselbladOpen == LIBRAW_SUCCESS &&
            oddHasselbladDecoder != nullptr &&
            std::string(oddHasselbladDecoder) == "hasselblad_load_raw()",
        "odd-width TIFF selects the Hasselblad paired-predictor seam",
        oddHasselbladDecoder == nullptr ? "no decoder" : oddHasselbladDecoder);
  const auto oddHasselbladOutcome = exercisePublicDecode(oddHasselblad);
  check(oddHasselbladOutcome.openCode == LIBRAW_SUCCESS &&
            oddHasselbladOutcome.unpackCode != LIBRAW_SUCCESS &&
            oddHasselbladOutcome.terminalStage == DecodeStage::kUnpack,
        "Hasselblad paired predictor rejects an odd raw width",
        describe(oddHasselbladOutcome));

  const auto evenHasselbladOutcome =
      exercisePublicDecode(makeHasselbladTiff(false));
  check(evenHasselbladOutcome.openCode == LIBRAW_SUCCESS &&
            evenHasselbladOutcome.unpackCode == LIBRAW_SUCCESS,
        "even-width Hasselblad paired predictor remains supported",
        describe(evenHasselbladOutcome));

  const std::vector<std::uint8_t> invalidHasselbladCategory =
      makeHasselbladTiff(false, 255U);
  auto invalidHasselbladRouteProbe = std::make_unique<LibRaw>();
  const int invalidHasselbladOpen = invalidHasselbladRouteProbe->open_buffer(
      invalidHasselbladCategory.data(), invalidHasselbladCategory.size());
  const char* invalidHasselbladDecoder =
      invalidHasselbladOpen == LIBRAW_SUCCESS
          ? invalidHasselbladRouteProbe->unpack_function_name()
          : nullptr;
  check(invalidHasselbladOpen == LIBRAW_SUCCESS &&
            invalidHasselbladDecoder != nullptr &&
            std::string(invalidHasselbladDecoder) == "hasselblad_load_raw()",
        "invalid-category TIFF selects the Hasselblad Huffman seam",
        invalidHasselbladDecoder == nullptr ? "no decoder"
                                            : invalidHasselbladDecoder);
  const auto invalidHasselbladOutcome =
      exercisePublicDecode(invalidHasselbladCategory);
  check(invalidHasselbladOutcome.openCode == LIBRAW_SUCCESS &&
            invalidHasselbladOutcome.unpackCode != LIBRAW_SUCCESS &&
            invalidHasselbladOutcome.terminalStage == DecodeStage::kUnpack,
        "Hasselblad rejects a Huffman difference category above 16",
        describe(invalidHasselbladOutcome));

  const std::vector<std::uint8_t> overflowingHasselblad =
      makeOverflowingHasselbladPredictorTiff();
  auto overflowingHasselbladRouteProbe = std::make_unique<LibRaw>();
  const int overflowingHasselbladOpen =
      overflowingHasselbladRouteProbe->open_buffer(
          overflowingHasselblad.data(), overflowingHasselblad.size());
  const char* overflowingHasselbladDecoder =
      overflowingHasselbladOpen == LIBRAW_SUCCESS
          ? overflowingHasselbladRouteProbe->unpack_function_name()
          : nullptr;
  check(overflowingHasselbladOpen == LIBRAW_SUCCESS &&
            overflowingHasselbladDecoder != nullptr &&
            std::string(overflowingHasselbladDecoder) ==
                "hasselblad_load_raw()",
        "category-16 TIFF selects the Hasselblad predictor seam",
        overflowingHasselbladDecoder == nullptr
            ? "no decoder"
            : overflowingHasselbladDecoder);
  const auto overflowingHasselbladOutcome =
      exercisePublicDecode(overflowingHasselblad);
  check(overflowingHasselbladOutcome.openCode == LIBRAW_SUCCESS &&
            overflowingHasselbladOutcome.unpackCode != LIBRAW_SUCCESS &&
            overflowingHasselbladOutcome.terminalStage == DecodeStage::kUnpack,
        "Hasselblad rejects a predictor outside signed-int range",
        describe(overflowingHasselbladOutcome));

  for (const std::uint16_t tagX641 : {std::uint16_t{31},
                                      std::uint16_t{32}}) {
    const std::vector<std::uint8_t> olympusBoundary =
        makeOlympusTag641BoundaryTiff(tagX641);
    const std::string decoder = decoderNameFor(olympusBoundary);
    check(decoder == "olympus_load_raw()",
          "Olympus tagX641 boundary selects the hardened decoder seam",
          "tagX641=" + std::to_string(tagX641) + " decoder=" + decoder);
    const auto outcome = exercisePublicDecode(olympusBoundary);
    check(outcome.openCode == LIBRAW_SUCCESS &&
              outcome.unpackCode == LIBRAW_SUCCESS,
          "Olympus tagX641 boundary remains decodable",
          "tagX641=" + std::to_string(tagX641) + " " + describe(outcome));
  }

  const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>
      olympusHostiles{
          {"bounded unary stream", makeOlympusUnaryWorkTiff()},
          {"negative predictor", makeOlympusNegativePredictorTiff()},
      };
  for (const auto& [label, input] : olympusHostiles) {
    const std::string decoder = decoderNameFor(input);
    check(decoder == "olympus_load_raw()",
          "Olympus hostile fixture selects the hardened decoder seam",
          label + " decoder=" + decoder);
    const auto outcome = exercisePublicDecode(input);
    check(outcome.openCode == LIBRAW_SUCCESS &&
              outcome.unpackCode != LIBRAW_SUCCESS &&
              outcome.terminalStage == DecodeStage::kUnpack,
          "Olympus hostile metadata fails safely during unpack",
          label + " " + describe(outcome));
  }

  const auto checkPanasonicPlane = [&](const std::vector<std::uint8_t>& input,
                                       std::uint16_t expectedPixel,
                                       const std::string& label) {
    auto raw = std::make_unique<LibRaw>();
    const int open = raw->open_buffer(input.data(), input.size());
    const char* decoder =
        open == LIBRAW_SUCCESS ? raw->unpack_function_name() : nullptr;
    const int unpack = open == LIBRAW_SUCCESS ? raw->unpack() : -1;
    const std::size_t pixelCount = open == LIBRAW_SUCCESS
        ? static_cast<std::size_t>(raw->imgdata.sizes.raw_width) *
              raw->imgdata.sizes.raw_height
        : 0U;
    const bool exactPlane = unpack == LIBRAW_SUCCESS &&
        raw->imgdata.rawdata.raw_image != nullptr &&
        std::all_of(raw->imgdata.rawdata.raw_image,
                    raw->imgdata.rawdata.raw_image + pixelCount,
                    [expectedPixel](std::uint16_t pixel) {
                      return pixel == expectedPixel;
                    });
    check(open == LIBRAW_SUCCESS && decoder != nullptr &&
              std::string(decoder) == "panasonicC8_load_raw()" &&
              raw->imgdata.sizes.raw_width == 24U &&
              raw->imgdata.sizes.raw_height == 24U &&
              unpack == LIBRAW_SUCCESS && exactPlane,
          label,
          "open=" + std::to_string(open) +
              " unpack=" + std::to_string(unpack) +
              " expected=" + std::to_string(expectedPixel) +
              " decoder=" + (decoder == nullptr ? "none" : decoder));
  };
  checkPanasonicPlane(makeValidPanasonicC8Tiff(), 100U,
                      "valid Panasonic C8 decodes the exact predictor plane");
  checkPanasonicPlane(
      makeValidTwoStripePanasonicC8Tiff(), 100U,
      "valid two-stripe Panasonic C8 decodes the exact predictor plane");
  checkPanasonicPlane(
      makeValidShadowedPanasonicC8Tiff(), 4095U,
      "real S5M2 shadowed Huffman ordering remains decodable");

  const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>
      panasonicArithmeticBoundaries{
          {"negative extraction shift", makePanasonicC8NegativeShiftTiff()},
          {"signed delta shift", makePanasonicC8SignedShiftTiff()},
      };
  constexpr std::array<std::uint64_t, 2> kArithmeticDigests{{
      17959133156917656579ULL,
      14033653403350148739ULL,
  }};
  for (std::size_t index = 0U;
       index < panasonicArithmeticBoundaries.size(); ++index) {
    const auto& [label, input] = panasonicArithmeticBoundaries.at(index);
    auto raw = std::make_unique<LibRaw>();
    const int open = raw->open_buffer(input.data(), input.size());
    const char* decoder =
        open == LIBRAW_SUCCESS ? raw->unpack_function_name() : nullptr;
    const int unpack = open == LIBRAW_SUCCESS ? raw->unpack() : -1;
    const std::size_t pixelCount = open == LIBRAW_SUCCESS
        ? static_cast<std::size_t>(raw->imgdata.sizes.raw_width) *
              raw->imgdata.sizes.raw_height
        : 0U;
    const std::uint64_t digest = unpack == LIBRAW_SUCCESS
        ? rawPlaneDigest(raw->imgdata.rawdata.raw_image, pixelCount)
        : 0U;
    check(open == LIBRAW_SUCCESS && decoder != nullptr &&
              std::string(decoder) == "panasonicC8_load_raw()" &&
              unpack == LIBRAW_SUCCESS &&
              digest == kArithmeticDigests.at(index),
          "Panasonic arithmetic boundary decodes without shift UB",
          label + " open=" + std::to_string(open) +
              " unpack=" + std::to_string(unpack) +
              " digest=" + std::to_string(digest) +
              " decoder=" + (decoder == nullptr ? "none" : decoder));
  }

  const std::vector<std::uint8_t> borrowedPanasonicBits =
      makePanasonicC8BorrowedBitsTiff();
  const PanasonicStripeRange firstStripe =
      firstPanasonicStripeRange(borrowedPanasonicBits);
  PanasonicRangeDatastream borrowedStream(borrowedPanasonicBits, firstStripe);
  auto borrowedProbe = std::make_unique<LibRaw>();
  const int borrowedOpen = borrowedProbe->open_datastream(&borrowedStream);
  const char* borrowedDecoder = borrowedOpen == LIBRAW_SUCCESS
      ? borrowedProbe->unpack_function_name()
      : nullptr;
  const int borrowedUnpack = borrowedOpen == LIBRAW_SUCCESS
      ? borrowedProbe->unpack()
      : -1;
  check(borrowedOpen == LIBRAW_SUCCESS && borrowedDecoder != nullptr &&
            std::string(borrowedDecoder) == "panasonicC8_load_raw()" &&
            borrowedUnpack == LIBRAW_IO_ERROR,
        "Panasonic short first stripe fails safely at the public seam",
        "open=" + std::to_string(borrowedOpen) +
            " unpack=" + std::to_string(borrowedUnpack) +
            " decoder=" +
            (borrowedDecoder == nullptr ? "none" : borrowedDecoder));
  check(firstStripe.valid && borrowedStream.sawBoundaryRead() &&
            !borrowedStream.crossedBoundary() &&
            borrowedStream.largestBoundaryRead() == firstStripe.byteCount,
        "Panasonic decoder does not read into the neighboring stripe",
        "declared=" + std::to_string(firstStripe.byteCount) +
            " requested=" +
            std::to_string(borrowedStream.largestBoundaryRead()) +
            " crossed=" +
            std::to_string(borrowedStream.crossedBoundary()));

  const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>
      panasonicHostiles{
          {"missing Huffman tables", makePanasonicC8MissingTablesTiff()},
          {"tag40 count 16", makePanasonicC8Tag40CountTiff(16U)},
          {"tag40 count 18", makePanasonicC8Tag40CountTiff(18U)},
          {"tag41 count 16", makePanasonicC8Tag41CountTiff(16U)},
          {"tag41 count 18", makePanasonicC8Tag41CountTiff(18U)},
          {"stripe field count mismatch",
           makePanasonicC8StripeCountMismatchTiff()},
          {"tag41 shift 64", makePanasonicC8Tag41Shift64Tiff()},
          {"no Huffman match", makePanasonicC8NoMatchTiff()},
          {"overrange Huffman width", makePanasonicC8OverrangeTableTiff()},
          {"zero Huffman width", makePanasonicC8HlowZeroTiff()},
          {"Huffman width 17", makePanasonicC8Hlow17Tiff()},
          {"Huffman code equals its width limit",
           makePanasonicC8CodeAtLimitTiff()},
          {"Huffman code exceeds the stored 12-bit domain",
           makePanasonicC8CodeAboveStoredDomainTiff()},
          {"stripe left above uint16", makePanasonicC8WideStripeLeftTiff()},
          {"overlapping stripes", makeOverlappingPanasonicC8Tiff()},
      };
  for (const auto& [label, input] : panasonicHostiles) {
    const std::string decoder = decoderNameFor(input);
    check(decoder == "panasonicC8_load_raw()",
          "Panasonic hostile fixture selects the hardened decoder seam",
          label + " decoder=" + decoder);
    const auto outcome = exercisePublicDecode(input);
    check(outcome.openCode == LIBRAW_SUCCESS &&
              outcome.unpackCode == LIBRAW_IO_ERROR &&
              outcome.terminalStage == DecodeStage::kUnpack,
          "Panasonic hostile metadata fails safely during unpack",
          label + " " + describe(outcome));
  }

  const std::vector<std::uint8_t> sof1Zrl = makeSof1ZrlDng();
  auto sof1RouteProbe = std::make_unique<LibRaw>();
  const int sof1Open =
      sof1RouteProbe->open_buffer(sof1Zrl.data(), sof1Zrl.size());
  const char* sof1Decoder = sof1Open == LIBRAW_SUCCESS
      ? sof1RouteProbe->unpack_function_name()
      : nullptr;
  check(sof1Open == LIBRAW_SUCCESS && sof1Decoder != nullptr &&
            std::string(sof1Decoder) == "lossless_dng_load_raw()",
        "SOF1 fixture selects the lossless-DNG IDCT seam",
        sof1Decoder == nullptr ? "no decoder" : sof1Decoder);
  const auto sof1ZrlOutcome = exercisePublicDecode(sof1Zrl);
  check(sof1ZrlOutcome.openCode == LIBRAW_SUCCESS &&
            sof1ZrlOutcome.unpackCode != LIBRAW_SUCCESS &&
            sof1ZrlOutcome.terminalStage == DecodeStage::kUnpack,
        "SOF1 zero-run cannot shift by a negative coefficient size",
        describe(sof1ZrlOutcome));

  const auto sof1DcOverflowOutcome =
      exercisePublicDecode(makeSof1DcOverflowDng());
  check(sof1DcOverflowOutcome.openCode == LIBRAW_SUCCESS &&
            sof1DcOverflowOutcome.unpackCode != LIBRAW_SUCCESS &&
            sof1DcOverflowOutcome.terminalStage == DecodeStage::kUnpack,
        "SOF1 DC predictor overflow rejects through checked INT64 arithmetic",
        describe(sof1DcOverflowOutcome));

  const auto sof1AcOverrunOutcome =
      exercisePublicDecode(makeSof1AcOverrunDng());
  check(sof1AcOverrunOutcome.openCode == LIBRAW_SUCCESS &&
            sof1AcOverrunOutcome.unpackCode != LIBRAW_SUCCESS &&
            sof1AcOverrunOutcome.terminalStage == DecodeStage::kUnpack,
        "SOF1 AC runs cannot index beyond the 64-value quantization table",
        describe(sof1AcOverrunOutcome));

  const std::vector<std::uint8_t> validLossless =
      makeValidLosslessJpegDng();
  auto validLosslessRouteProbe = std::make_unique<LibRaw>();
  const int validLosslessOpen = validLosslessRouteProbe->open_buffer(
      validLossless.data(), validLossless.size());
  const char* validLosslessDecoder = validLosslessOpen == LIBRAW_SUCCESS
      ? validLosslessRouteProbe->unpack_function_name()
      : nullptr;
  check(validLosslessOpen == LIBRAW_SUCCESS && validLosslessDecoder != nullptr &&
            std::string(validLosslessDecoder) == "lossless_dng_load_raw()",
        "valid lossless DNG selects the hardened decoder seam",
        validLosslessDecoder == nullptr ? "no decoder" : validLosslessDecoder);
  const auto validLosslessOutcome = exercisePublicDecode(validLossless);
  check(validLosslessOutcome.openCode == LIBRAW_SUCCESS &&
            validLosslessOutcome.unpackCode == LIBRAW_SUCCESS &&
            validLosslessOutcome.processCode == LIBRAW_SUCCESS,
        "valid lossless DNG reaches open_buffer -> unpack -> dcraw_process",
        describe(validLosslessOutcome));

  // Positive control: prove this target is not merely an open_buffer parser
  // test. A tiny valid CFA DNG reaches all three public stages. With OpenMP
  // available, the non-zero threshold also executes the patched wavelet path.
  const auto validOutcome = exercisePublicDecode(makeValidUncompressedDng(), true);
  check(validOutcome.openCode == LIBRAW_SUCCESS &&
            validOutcome.unpackCode == LIBRAW_SUCCESS &&
            validOutcome.processCode == LIBRAW_SUCCESS,
        "valid DNG reaches open_buffer -> unpack -> dcraw_process",
        describe(validOutcome));
  check(validOutcome.processingWidth >= 65U &&
            validOutcome.processingHeight >= 65U,
        "wavelet control remains above LibRaw's 65-pixel guard",
        describe(validOutcome));
#if defined(SFRAW_HOST_OPENMP_ACTIVE)
  check(validOutcome.processCode == LIBRAW_SUCCESS,
        "OpenMP wavelet control completes the patched allocation path",
        describe(validOutcome));
#endif

  // Project-owned lossless-JPEG negatives. Both have a complete SOF3/DHT/SOS,
  // so open_buffer recognizes the DNG and unpack enters the lossless-JPEG
  // decoder. Their entropy strips are deliberately truncated. Category 17 also
  // verifies the public decoder rejects an invalid difference width (>16).
  const std::vector<std::uint8_t> shortEntropy =
      makeMalformedLosslessJpegDng(
          makeTruncatedLosslessJpegStrip(16U, 1U), 131072U);
  const auto shortEntropyOutcome = exercisePublicDecode(shortEntropy);
  check(shortEntropyOutcome.openCode == LIBRAW_SUCCESS &&
            shortEntropyOutcome.unpackCode != LIBRAW_SUCCESS,
        "truncated lossless-JPEG entropy fails safely in unpack",
        describe(shortEntropyOutcome));

  const std::vector<std::uint8_t> invalidCategory =
      makeMalformedLosslessJpegDng(
          makeTruncatedLosslessJpegStrip(17U, 8U), 131072U);
  const auto invalidCategoryOutcome = exercisePublicDecode(invalidCategory);
  check(invalidCategoryOutcome.openCode == LIBRAW_SUCCESS &&
            invalidCategoryOutcome.unpackCode != LIBRAW_SUCCESS,
        "hostile lossless-JPEG category fails safely in unpack",
        describe(invalidCategoryOutcome));

  // The hard caps are part of the hostile-input contract, not just fuzzer flags.
  std::vector<std::uint8_t> oversized(sfraw::hosttest::kMaxInputBytes + 1U, 0U);
  const auto oversizedOutcome = exercisePublicDecode(oversized);
  check(oversizedOutcome.terminalStage == DecodeStage::kNotStarted,
        "input larger than 16 MiB is rejected before LibRaw",
        describe(oversizedOutcome));

  std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
            << " (" << failures << " failures)\n";
  return failures == 0 ? 0 : 1;
}
