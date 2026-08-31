/*
 * Spektrafilm Android -- deterministic binary LibRaw fuzz-seed writer.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "public_api_test_support.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > sfraw::hosttest::kMaxInputBytes) {
    throw std::runtime_error("Refusing invalid seed size for " + path.string());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw std::runtime_error("Cannot create seed: " + path.string());
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!stream) throw std::runtime_error("Cannot finish seed: " + path.string());
  std::cout << path.filename().string() << '=' << bytes.size() << " bytes\n";
}

}  // namespace

int main(int argc, char** argv) {
  using sfraw::hosttest::makeDuplicateOpcodeDng;
  using sfraw::hosttest::makeDuplicateStripTablesDng;
  using sfraw::hosttest::makeExtremeAspectDng;
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
  using sfraw::hosttest::makeAdobeDeflateDng;
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
  using sfraw::hosttest::makeMalformedLosslessJpegDng;
  using sfraw::hosttest::makeNonTerminatedIdentifyHead;
  using sfraw::hosttest::makeNonTerminatedMakerNoteTiff;
  using sfraw::hosttest::makeTailDpSignatureX3f;
  using sfraw::hosttest::makeTailSdSignatureX3f;
  using sfraw::hosttest::makeSof1ZrlDng;
  using sfraw::hosttest::makeSof1DcOverflowDng;
  using sfraw::hosttest::makeSof1AcOverrunDng;
  using sfraw::hosttest::makeHostileCr2SliceTiff;
  using sfraw::hosttest::makeOverflowingCr2SliceTiff;
  using sfraw::hosttest::makeHostileActiveAreaDng;
  using sfraw::hosttest::makeOverBudgetMetadataDng;
  using sfraw::hosttest::makeOversizedLjpegGeometryTiff;
  using sfraw::hosttest::makeOversizedLjpegGeometryDng;
  using sfraw::hosttest::makeTruncatedLosslessJpegStrip;
  using sfraw::hosttest::makeValidUncompressedDng;
  using sfraw::hosttest::makeValidLosslessJpegDng;
  using sfraw::hosttest::makeValidSonyLjpegTiff;
  using sfraw::hosttest::makeValidCanonSrawTiff;
  using sfraw::hosttest::makeNegativeChromaCanonSrawTiff;
  using sfraw::hosttest::makeShortSofCanonSrawTiff;
  using sfraw::hosttest::makeRedundantSlicesCanonSrawTiff;
  using sfraw::hosttest::makeLargeModelVersionCanonSrawTiff;
  using sfraw::hosttest::makeValidCanonSrawWhiteBalanceTiff;
  using sfraw::hosttest::makeOverflowingCanonSrawWhiteBalanceTiff;
  using sfraw::hosttest::readHexFixture;

  if (argc != 3) {
    std::cerr << "usage: sfraw_libraw_seed_writer <source-corpus> <output-dir>\n";
    return 2;
  }

  try {
    const std::filesystem::path sourceCorpus(argv[1]);
    const std::filesystem::path output(argv[2]);
    if (!std::filesystem::is_directory(output)) {
      throw std::runtime_error("Output directory does not exist: " +
                               output.string());
    }

    writeBytes(output / "issue-844-newsubfiletype.tiff",
               readHexFixture(sourceCorpus /
                              "tiff-newsubfiletype-float-overflow-844.hex"));
    writeBytes(output / "identify-nonterminated-head.tiff",
               makeNonTerminatedIdentifyHead());
    writeBytes(output / "makernote-nonterminated-head.tiff",
               makeNonTerminatedMakerNoteTiff());
    writeBytes(output / "x3f-tail-dp-signature.x3f",
               makeTailDpSignatureX3f());
    writeBytes(output / "x3f-tail-sd-signature.x3f",
               makeTailSdSignatureX3f());
    writeBytes(output / "valid-uncompressed-cfa.dng",
               makeValidUncompressedDng());
    writeBytes(output / "valid-lossless-jpeg-cfa.dng",
               makeValidLosslessJpegDng());
    writeBytes(output / "hostile-cr2slice-column.tiff",
               makeHostileCr2SliceTiff());
    writeBytes(output / "overflowing-cr2slice-arithmetic.tiff",
               makeOverflowingCr2SliceTiff());
    writeBytes(output / "truncated-ljpeg-entropy.dng",
               makeMalformedLosslessJpegDng(
                   makeTruncatedLosslessJpegStrip(16U, 1U), 131072U));
    writeBytes(output / "invalid-ljpeg-category.dng",
               makeMalformedLosslessJpegDng(
                   makeTruncatedLosslessJpegStrip(17U, 8U), 131072U));
    writeBytes(output / "duplicate-opcode-list.dng",
               makeDuplicateOpcodeDng());
    writeBytes(output / "duplicate-strip-tables.dng",
               makeDuplicateStripTablesDng());
    writeBytes(output / "aggregate-metadata-over-budget.dng",
               makeOverBudgetMetadataDng());
    writeBytes(output / "extreme-default-scale.dng", makeExtremeAspectDng());
    writeBytes(output / "hostile-active-area.dng", makeHostileActiveAreaDng());
    writeBytes(output / "oversized-ljpeg-geometry.tiff",
               makeOversizedLjpegGeometryTiff());
    writeBytes(output / "oversized-ljpeg-geometry.dng",
               makeOversizedLjpegGeometryDng());
    writeBytes(output / "excessive-tile-count.dng",
               makeExcessiveTileCountLosslessJpegDng());
    writeBytes(output / "excessive-packed-tile-count.dng",
               makeExcessiveTileCountUncompressedDng());
    writeBytes(output / "compliant-4225-tile-lossless.dng",
               makeCompliantTiledLosslessJpegDng());
    writeBytes(output / "valid-sony-lossless-jpeg.tiff",
               makeValidSonyLjpegTiff());
    writeBytes(output / "valid-canon-sraw.tiff", makeValidCanonSrawTiff());
    writeBytes(output / "negative-chroma-canon-sraw.tiff",
               makeNegativeChromaCanonSrawTiff());
    writeBytes(output / "short-sof-canon-sraw.tiff",
               makeShortSofCanonSrawTiff());
    writeBytes(output / "redundant-slices-canon-sraw.tiff",
               makeRedundantSlicesCanonSrawTiff());
    writeBytes(output / "large-model-version-canon-sraw.tiff",
               makeLargeModelVersionCanonSrawTiff());
    writeBytes(output / "valid-canon-sraw-white-balance.tiff",
               makeValidCanonSrawWhiteBalanceTiff());
    writeBytes(output / "overflowing-canon-sraw-white-balance.tiff",
               makeOverflowingCanonSrawWhiteBalanceTiff());
    writeBytes(output / "invalid-sony-ljpeg-components.tiff",
               makeInvalidSonyLjpegComponentsTiff());
    writeBytes(output / "excessive-sony-tile-count.tiff",
               makeExcessiveSonyTileCountTiff());
    writeBytes(output / "truncated-dht-leaf.tiff",
               makeTruncatedDhtSonyTiff());
    writeBytes(output / "duplicate-dht-table.tiff",
               makeDuplicateDhtSonyTiff());
    writeBytes(output / "terminal-dht-overflow.tiff",
               makeTerminalDhtOverflowSonyTiff());
    writeBytes(output / "oversubscribed-dht.tiff",
               makeOversubscribedDhtSonyTiff());
    writeBytes(output / "negative-effective-bits.tiff",
               makeNegativeEffectiveBitsSonyTiff());
    writeBytes(output / "boundary-sony-tile-count.tiff",
               makeBoundarySonyTileCountTiff());
    writeBytes(output / "cumulative-dht-work.tiff",
               makeCumulativeDhtWorkSonyTiff());
    writeBytes(output / "cumulative-marker-work.tiff",
               makeCumulativeMarkerWorkSonyTiff());
    writeBytes(output / "valid-identify-ljpeg.tiff",
               makeValidIdentifyLjpegTiff());
    writeBytes(output / "repeated-identify-ljpeg.tiff",
               makeRepeatedIdentifyLjpegTiff());
    writeBytes(output / "distinct-tile-float-deflate.dng",
               makeDistinctTileFloatDeflateDng());
    writeBytes(output / "adobe-deflate-0x80b2.dng",
               makeAdobeDeflateDng());
    writeBytes(output / "float32-no-white-level.dng",
               makeFloat32DngWithoutWhiteLevel());
    writeBytes(output / "integer31-no-white-level.dng",
               makeInteger31DngWithoutWhiteLevel());
    writeBytes(output / "sony-bps11-negative-black-shift.tiff",
               makeSonyInvalidBlackShiftTiff());
    writeBytes(output / "sony-bps12-black-shift-boundary.tiff",
               makeSonyBoundaryBlackShiftTiff());
    writeBytes(output / "samsung-bps6-negative-black-shift.tiff",
               makeSamsungInvalidBlackShiftTiff());
    writeBytes(output / "samsung-bps7-black-shift-boundary.tiff",
               makeSamsungBoundaryBlackShiftTiff());
    writeBytes(output / "repeated-tile-float-deflate.dng",
               makeRepeatedTileFloatDeflateDng());
    writeBytes(output / "odd-width-hasselblad.tiff",
               makeHasselbladTiff(true));
    writeBytes(output / "even-width-hasselblad.tiff",
               makeHasselbladTiff(false));
    writeBytes(output / "invalid-category-hasselblad.tiff",
               makeHasselbladTiff(false, 255U));
    writeBytes(output / "overflowing-predictor-hasselblad.tiff",
               makeOverflowingHasselbladPredictorTiff());
    writeBytes(output / "olympus-tag641-31.tiff",
               makeOlympusTag641BoundaryTiff(31U));
    writeBytes(output / "olympus-tag641-32.tiff",
               makeOlympusTag641BoundaryTiff(32U));
    writeBytes(output / "olympus-unary-work.tiff",
               makeOlympusUnaryWorkTiff());
    writeBytes(output / "olympus-negative-predictor.tiff",
               makeOlympusNegativePredictorTiff());
    writeBytes(output / "panasonic-c8-valid.tiff",
               makeValidPanasonicC8Tiff());
    writeBytes(output / "panasonic-c8-valid-two-stripe.tiff",
               makeValidTwoStripePanasonicC8Tiff());
    writeBytes(output / "panasonic-c8-valid-s5m2-shadowed-table.tiff",
               makeValidShadowedPanasonicC8Tiff());
    writeBytes(output / "panasonic-c8-missing-tables.tiff",
               makePanasonicC8MissingTablesTiff());
    writeBytes(output / "panasonic-c8-tag40-count16.tiff",
               makePanasonicC8Tag40CountTiff(16U));
    writeBytes(output / "panasonic-c8-tag40-count18.tiff",
               makePanasonicC8Tag40CountTiff(18U));
    writeBytes(output / "panasonic-c8-tag41-count16.tiff",
               makePanasonicC8Tag41CountTiff(16U));
    writeBytes(output / "panasonic-c8-tag41-count18.tiff",
               makePanasonicC8Tag41CountTiff(18U));
    writeBytes(output / "panasonic-c8-stripe-count-mismatch.tiff",
               makePanasonicC8StripeCountMismatchTiff());
    writeBytes(output / "panasonic-c8-short-first-stripe.tiff",
               makePanasonicC8BorrowedBitsTiff());
    writeBytes(output / "panasonic-c8-negative-shift.tiff",
               makePanasonicC8NegativeShiftTiff());
    writeBytes(output / "panasonic-c8-signed-shift.tiff",
               makePanasonicC8SignedShiftTiff());
    writeBytes(output / "panasonic-c8-tag41-shift64.tiff",
               makePanasonicC8Tag41Shift64Tiff());
    writeBytes(output / "panasonic-c8-no-match.tiff",
               makePanasonicC8NoMatchTiff());
    writeBytes(output / "panasonic-c8-overrange-table.tiff",
               makePanasonicC8OverrangeTableTiff());
    writeBytes(output / "panasonic-c8-hlow-zero.tiff",
               makePanasonicC8HlowZeroTiff());
    writeBytes(output / "panasonic-c8-hlow17.tiff",
               makePanasonicC8Hlow17Tiff());
    writeBytes(output / "panasonic-c8-code-at-limit.tiff",
               makePanasonicC8CodeAtLimitTiff());
    writeBytes(output / "panasonic-c8-code-above-domain.tiff",
               makePanasonicC8CodeAboveStoredDomainTiff());
    writeBytes(output / "panasonic-c8-wide-left.tiff",
               makePanasonicC8WideStripeLeftTiff());
    writeBytes(output / "panasonic-c8-overlap.tiff",
               makeOverlappingPanasonicC8Tiff());
    writeBytes(output / "sof1-zrl.dng", makeSof1ZrlDng());
    writeBytes(output / "sof1-dc-overflow.dng", makeSof1DcOverflowDng());
    writeBytes(output / "sof1-ac-overrun.dng", makeSof1AcOverrunDng());
  } catch (const std::exception& error) {
    std::cerr << "seed generation failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
