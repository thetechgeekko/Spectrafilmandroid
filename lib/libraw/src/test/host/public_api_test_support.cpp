/*
 * Spektrafilm Android -- bounded LibRaw public-API regression support.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "public_api_test_support.h"

#include <libraw/libraw.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace sfraw::hosttest {
namespace {

constexpr int kNotCalled = std::numeric_limits<int>::min();

void put16(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint16_t value) {
  bytes.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
  bytes.at(offset + 1U) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value) {
  bytes.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
  bytes.at(offset + 1U) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes.at(offset + 2U) = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes.at(offset + 3U) = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

std::uint16_t get16(const std::vector<std::uint8_t>& bytes,
                    std::size_t offset) {
  return static_cast<std::uint16_t>(bytes.at(offset)) |
         static_cast<std::uint16_t>(bytes.at(offset + 1U) << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes,
                    std::size_t offset) {
  return static_cast<std::uint32_t>(bytes.at(offset)) |
         (static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U) |
         (static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 16U) |
         (static_cast<std::uint32_t>(bytes.at(offset + 3U)) << 24U);
}

struct TiffEntry {
  std::uint16_t tag;
  std::uint16_t type;
  std::uint32_t count;
  std::uint32_t value;
};

enum class MetadataFixture {
  kNone,
  kDuplicateOpcode,
  kAggregateOverBudget,
  kDuplicateStripTables,
  kValidMultiStrip,
  kExtremeAspect,
  kBoundaryAspectBelow,
  kBoundaryAspectAbove,
  kHostileActiveArea,
  kExcessiveTiles,
  kSonyTiff,
  kExcessiveSonyTiles,
  kHasselbladTiff,
  kCompliantDngTiles,
};

int hexNibble(char value) {
  const unsigned char c = static_cast<unsigned char>(value);
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

const char* stageName(DecodeStage stage) {
  switch (stage) {
    case DecodeStage::kNotStarted:
      return "not-started";
    case DecodeStage::kOpen:
      return "open_buffer";
    case DecodeStage::kUnpack:
      return "unpack";
    case DecodeStage::kProcess:
      return "dcraw_process";
    case DecodeStage::kDimensionGuard:
      return "dimension-guard";
  }
  return "unknown";
}

bool pixelProductExceeds(std::uint64_t width, std::uint64_t height) {
  return width != 0U && height > kMaxDeclaredPixels / width;
}

bool declaredDimensionsExceedLimit(const libraw_image_sizes_t& sizes) {
  const std::uint64_t allocationWidth = std::max<std::uint64_t>(
      sizes.raw_width,
      static_cast<std::uint64_t>(sizes.width) + sizes.left_margin);
  const std::uint64_t allocationHeight = std::max<std::uint64_t>(
      sizes.raw_height,
      static_cast<std::uint64_t>(sizes.height) + sizes.top_margin);
  if (pixelProductExceeds(sizes.raw_width, sizes.raw_height) ||
      pixelProductExceeds(sizes.width, sizes.height) ||
      pixelProductExceeds(allocationWidth, allocationHeight)) {
    return true;
  }

  const double aspect = sizes.pixel_aspect;
  if (!std::isfinite(aspect) || aspect <= 0.0) return true;
  if (sizes.width == 0U || sizes.height == 0U) return false;

  double projectedWidth = sizes.width;
  double projectedHeight = sizes.height;
  if (aspect < 1.0) {
    projectedHeight = projectedHeight / aspect + 0.5;
  } else if (aspect > 1.0) {
    projectedWidth = projectedWidth * aspect + 0.5;
  }
  const double maxDimension =
      static_cast<double>(std::numeric_limits<std::uint16_t>::max());
  if (!std::isfinite(projectedWidth) || !std::isfinite(projectedHeight) ||
      projectedWidth > maxDimension || projectedHeight > maxDimension) {
    return true;
  }
  return pixelProductExceeds(static_cast<std::uint64_t>(projectedWidth),
                             static_cast<std::uint64_t>(projectedHeight));
}

std::vector<std::uint8_t> makeCfaDng(
    std::uint16_t compression, const std::vector<std::uint8_t>& stripBytes,
    std::uint32_t declaredStripBytes, bool addHostileCr2Slice = false,
    std::uint32_t width = 256U, std::uint32_t height = 256U,
    std::size_t profileBytes = 0U,
    MetadataFixture metadataFixture = MetadataFixture::kNone) {
  // Little-endian TIFF/DNG. All IFD entries are deliberately small/in-line;
  // the only external data is the CFA strip.
  constexpr std::uint32_t kIfdOffset = 8U;
  // LibRaw intentionally rejects RAW planes smaller than 22 x 22 during
  // identify(). Keep synthetic DNGs large enough to reach the decoder while
  // still tiny enough for sanitizer/fuzzer iteration.
  std::vector<TiffEntry> entries{
      {0x00fe, 4, 1, 0},            // NewSubFileType: primary image
      {0x0100, 4, 1, width},        // ImageWidth
      {0x0101, 4, 1, height},       // ImageLength
      {0x0102, 3, 1, 16},           // BitsPerSample
      {0x0103, 3, 1, compression},  // Compression
      {0x0106, 3, 1, 32803},        // PhotometricInterpretation: CFA
      {0x0111, 4, 1, 0},            // StripOffsets (patched below)
      {0x0115, 3, 1, 1},            // SamplesPerPixel
      {0x0116, 4, 1, height},        // RowsPerStrip
      {0x0117, 4, 1, declaredStripBytes},
      {0x011c, 3, 1, 1},            // PlanarConfiguration
      {0x828d, 3, 2, 0x00020002U},  // CFARepeatPatternDim: 2 x 2
      {0x828e, 1, 4, 0x02010100U},  // CFAPattern: R G G B
      {0xc612, 1, 4, 0x00000401U},  // DNGVersion: 1.4.0.0
      {0xc613, 1, 4, 0x00000101U},  // DNGBackwardVersion: 1.1.0.0
      {0xc61d, 4, 1, 65535U},       // WhiteLevel
      {0xc65a, 3, 1, 21U},          // CalibrationIlluminant1: D65
  };
  if (profileBytes > 0U) {
    if (profileBytes > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("embedded profile is too large for TIFF LONG");
    }
    entries.push_back({0x8773, 1,
                       static_cast<std::uint32_t>(profileBytes), 0U});
  }
  std::size_t metadataBytes = 0U;
  if (metadataFixture == MetadataFixture::kDuplicateOpcode) {
    metadataBytes = 1024U;
    entries.push_back({0xc740, 1, 1024U, 0U});
    entries.push_back({0xc740, 1, 1024U, 0U});
  } else if (metadataFixture == MetadataFixture::kAggregateOverBudget) {
    constexpr std::uint32_t kXmpBytes = 5'000'000U;
    constexpr std::uint32_t kOpcodeBytes = 4U * 1024U * 1024U - 1U;
    metadataBytes = kXmpBytes;
    entries.push_back({0x02bc, 1, kXmpBytes, 0U});
    entries.push_back({0xc740, 1, kOpcodeBytes, 0U});
    entries.push_back({0xc741, 1, kOpcodeBytes, 0U});
    entries.push_back({0xc74e, 1, kOpcodeBytes, 0U});
  } else if (metadataFixture == MetadataFixture::kDuplicateStripTables) {
    metadataBytes = 8U;
    entries.push_back({0x0111, 4, 2U, 0U});
    entries.push_back({0x0111, 4, 2U, 0U});
    entries.push_back({0x0117, 4, 2U, 0U});
    entries.push_back({0x0117, 4, 2U, 0U});
  } else if (metadataFixture == MetadataFixture::kValidMultiStrip) {
    metadataBytes = 16U;
    for (TiffEntry& entry : entries) {
      if (entry.tag == 0x0111U || entry.tag == 0x0117U) {
        entry.count = 2U;
        entry.value = 0U;
      }
    }
  } else if (metadataFixture == MetadataFixture::kExtremeAspect ||
             metadataFixture == MetadataFixture::kBoundaryAspectBelow ||
             metadataFixture == MetadataFixture::kBoundaryAspectAbove) {
    metadataBytes = 16U;
    entries.push_back({0xc61e, 5, 2U, 0U});  // DefaultScale rationals
  } else if (metadataFixture == MetadataFixture::kHostileActiveArea) {
    metadataBytes = 16U;
    entries.push_back({0xc68d, 4, 4U, 0U});  // ActiveArea LONG[4]
  } else if (metadataFixture == MetadataFixture::kExcessiveTiles ||
             metadataFixture == MetadataFixture::kExcessiveSonyTiles ||
             metadataFixture == MetadataFixture::kCompliantDngTiles) {
    const bool compliantTiles =
        metadataFixture == MetadataFixture::kCompliantDngTiles;
    const std::uint32_t fixtureTileWidth = compliantTiles ? 16U : 1U;
    const std::uint32_t fixtureTileHeight = compliantTiles ? 16U : 1U;
    const std::uint64_t tileCount64 = compliantTiles
        ? ((static_cast<std::uint64_t>(width) + 15U) / 16U) *
              ((static_cast<std::uint64_t>(height) + 15U) / 16U)
        : static_cast<std::uint64_t>(width) * height;
    if (tileCount64 > std::numeric_limits<std::uint32_t>::max() ||
        tileCount64 * 8U > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("tiled fixture is too large for classic TIFF");
    }
    const std::uint32_t tileCount = static_cast<std::uint32_t>(tileCount64);
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [](const TiffEntry& entry) {
          return entry.tag == 0x0111U || entry.tag == 0x0116U ||
                 entry.tag == 0x0117U;
        }),
        entries.end());
    metadataBytes = static_cast<std::size_t>(tileCount) * 8U;
    entries.push_back({0x0142, 4, 1U, fixtureTileWidth});
    entries.push_back({0x0143, 4, 1U, fixtureTileHeight});
    entries.push_back({0x0144, 4, tileCount, 0U});    // TileOffsets
    entries.push_back({0x0145, 4, tileCount, 0U});    // TileByteCounts
  }
  if (addHostileCr2Slice) {
    // Three SHORTs live out-of-line. Values mirror the vulnerable geometry in
    // TALOS-2026-2331: a 4096-column slice applied to a 256-column raw plane.
    entries.push_back({0xc640, 3, 3, 0});  // CR2Slice (offset patched below)
  }
  const bool makeSonyTiff = metadataFixture == MetadataFixture::kSonyTiff ||
      metadataFixture == MetadataFixture::kExcessiveSonyTiles;
  const bool makeHasselbladTiff =
      metadataFixture == MetadataFixture::kHasselbladTiff;
  const bool makeVendorTiff = makeSonyTiff || makeHasselbladTiff;
  if (addHostileCr2Slice || makeVendorTiff) {
    // DNGVersion forces LibRaw onto a DNG loader. Replace those two tags while
    // preserving the entry count so generic RAW fixtures select their exact
    // vendor decoder seams.
    for (TiffEntry& entry : entries) {
      if (entry.tag == 0xc612U) {
        entry = makeVendorTiff
            ? TiffEntry{0x010f, 2,
                        makeHasselbladTiff ? 11U : 5U, 0U}
            : TiffEntry{0x010f, 2, 4, 0x00747354U};  // Make: "Tst\0"
      } else if (entry.tag == 0xc613U) {
        entry = {0x0110, 2, 4, 0x00776152U};  // Model: "Raw\0"
      }
    }
  }
  // TIFF 6.0 requires ascending tag order. LibRaw tolerates many unsorted IFDs,
  // but security fixtures must not depend on that tolerance or silently skip a
  // later private tag.
  std::sort(entries.begin(), entries.end(),
            [](const TiffEntry& left, const TiffEntry& right) {
              return left.tag < right.tag;
            });

  constexpr std::size_t kHeaderBytes = 8U;
  const std::size_t ifdBytes = 2U + entries.size() * 12U + 4U;
  const std::uint32_t sliceOffset =
      static_cast<std::uint32_t>(kHeaderBytes + ifdBytes);
  const std::uint32_t vendorMakeOffset =
      sliceOffset + (addHostileCr2Slice ? 6U : 0U);
  const std::uint32_t profileOffset =
      vendorMakeOffset + (makeSonyTiff ? 5U :
                          (makeHasselbladTiff ? 11U : 0U));
  const std::uint32_t metadataOffset = profileOffset +
      static_cast<std::uint32_t>(profileBytes);
  const std::uint32_t stripOffset = metadataOffset +
      static_cast<std::uint32_t>(metadataBytes);
  std::vector<std::uint8_t> bytes(stripOffset + stripBytes.size(), 0U);

  bytes[0] = 'I';
  bytes[1] = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, static_cast<std::uint16_t>(entries.size()));

  std::size_t cursor = kIfdOffset + 2U;
  for (const TiffEntry& original : entries) {
    TiffEntry entry = original;
    if (entry.tag == 0x0111U) entry.value = stripOffset;
    if (entry.tag == 0x0111U && entry.count > 1U) {
      entry.value = metadataOffset;
    }
    if (entry.tag == 0x0117U && entry.count > 1U) {
      entry.value = metadataFixture == MetadataFixture::kValidMultiStrip
          ? metadataOffset + 8U
          : metadataOffset;
    }
    if (entry.tag == 0x8773U) entry.value = profileOffset;
    if (entry.tag == 0x02bcU || entry.tag == 0xc740U ||
        entry.tag == 0xc741U || entry.tag == 0xc74eU) {
      entry.value = metadataOffset;
    }
    if (entry.tag == 0xc61eU) entry.value = metadataOffset;
    if (entry.tag == 0xc68dU) entry.value = metadataOffset;
    if (entry.tag == 0xc640U) entry.value = sliceOffset;
    if (entry.tag == 0x010fU && entry.count > 4U) {
      entry.value = vendorMakeOffset;
    }
    if (entry.tag == 0x0144U) entry.value = metadataOffset;
    if (entry.tag == 0x0145U) {
      entry.value = metadataOffset +
          static_cast<std::uint32_t>(metadataBytes / 2U);
    }
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);

  if (addHostileCr2Slice) {
    put16(bytes, sliceOffset, 1U);       // one full-width slice
    put16(bytes, sliceOffset + 2U, 4096U);
    put16(bytes, sliceOffset + 4U, 4096U);
  }
  if (makeSonyTiff) {
    bytes.at(vendorMakeOffset) = 'S';
    bytes.at(vendorMakeOffset + 1U) = 'O';
    bytes.at(vendorMakeOffset + 2U) = 'N';
    bytes.at(vendorMakeOffset + 3U) = 'Y';
  } else if (makeHasselbladTiff) {
    constexpr std::array<char, 10> kMake{{'H', 'a', 's', 's', 'e',
                                          'l', 'b', 'l', 'a', 'd'}};
    std::copy(kMake.begin(), kMake.end(),
              bytes.begin() + vendorMakeOffset);
  }

  if (metadataFixture == MetadataFixture::kValidMultiStrip) {
    put32(bytes, metadataOffset, stripOffset);
    put32(bytes, metadataOffset + 4U, stripOffset + 1U);
    put32(bytes, metadataOffset + 8U, 1U);
    put32(bytes, metadataOffset + 12U, 1U);
  } else if (metadataFixture == MetadataFixture::kExtremeAspect ||
             metadataFixture == MetadataFixture::kBoundaryAspectBelow ||
             metadataFixture == MetadataFixture::kBoundaryAspectAbove) {
    const std::uint32_t horizontalNumerator =
        metadataFixture == MetadataFixture::kExtremeAspect
            ? 1U
            : (metadataFixture == MetadataFixture::kBoundaryAspectBelow
                   ? 994'999U
                   : 1'005'001U);
    const std::uint32_t verticalNumerator =
        metadataFixture == MetadataFixture::kExtremeAspect ? 10U : 1'000'000U;
    put32(bytes, metadataOffset, horizontalNumerator);
    put32(bytes, metadataOffset + 4U, 1U);
    put32(bytes, metadataOffset + 8U, verticalNumerator);
    put32(bytes, metadataOffset + 12U, 1U);
  } else if (metadataFixture == MetadataFixture::kHostileActiveArea) {
    put32(bytes, metadataOffset, 2048U);        // top
    put32(bytes, metadataOffset + 4U, 2048U);   // left
    put32(bytes, metadataOffset + 8U, 5120U);   // bottom: height 3072
    put32(bytes, metadataOffset + 12U, 6144U);  // right: width 4096
  } else if (metadataFixture == MetadataFixture::kExcessiveTiles ||
             metadataFixture == MetadataFixture::kExcessiveSonyTiles ||
             metadataFixture == MetadataFixture::kCompliantDngTiles) {
    const std::uint32_t tileCount =
        static_cast<std::uint32_t>(metadataBytes / 8U);
    const std::size_t byteCountsOffset =
        metadataOffset + static_cast<std::size_t>(tileCount) * 4U;
    for (std::uint32_t tile = 0; tile < tileCount; ++tile) {
      put32(bytes, metadataOffset + static_cast<std::size_t>(tile) * 4U,
            stripOffset);
      put32(bytes, byteCountsOffset + static_cast<std::size_t>(tile) * 4U,
            static_cast<std::uint32_t>(stripBytes.size()));
    }
  }

  // The profile payload is already zero-initialized; leave it inert and copy
  // only the raw strip that follows it.

  for (std::size_t index = 0; index < stripBytes.size(); ++index) {
    bytes[stripOffset + index] = stripBytes[index];
  }
  return bytes;
}

std::vector<std::uint8_t> makeSonyLosslessJpegStrip(
    std::uint16_t width, std::uint16_t height, std::uint8_t components,
    std::size_t entropyBytes) {
  if (components == 0U || components > 6U) {
    throw std::invalid_argument("invalid synthetic JPEG component count");
  }
  const std::uint16_t sofLength =
      static_cast<std::uint16_t>(8U + 3U * components);
  std::vector<std::uint8_t> bytes{
      0xffU, 0xd8U, 0xffU, 0xc3U,
      static_cast<std::uint8_t>(sofLength >> 8U),
      static_cast<std::uint8_t>(sofLength & 0xffU),
      0x10U,
      static_cast<std::uint8_t>(height >> 8U),
      static_cast<std::uint8_t>(height & 0xffU),
      static_cast<std::uint8_t>(width >> 8U),
      static_cast<std::uint8_t>(width & 0xffU),
      components,
  };
  for (std::uint8_t component = 1U; component <= components; ++component) {
    bytes.push_back(component);
    bytes.push_back(0x11U);
    bytes.push_back(0x00U);
  }
  const std::array<std::uint8_t, 5> dht{{0xffU, 0xc4U, 0x00U, 0x14U, 0x00U}};
  bytes.insert(bytes.end(), dht.begin(), dht.end());
  bytes.push_back(0x01U);
  bytes.insert(bytes.end(), 15U, 0x00U);
  bytes.push_back(0x00U);  // the only Huffman symbol is category zero

  const std::uint16_t sosLength =
      static_cast<std::uint16_t>(6U + 2U * components);
  bytes.push_back(0xffU);
  bytes.push_back(0xdaU);
  bytes.push_back(static_cast<std::uint8_t>(sosLength >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(sosLength & 0xffU));
  bytes.push_back(components);
  for (std::uint8_t component = 1U; component <= components; ++component) {
    bytes.push_back(component);
    bytes.push_back(0x00U);
  }
  bytes.push_back(0x01U);
  bytes.push_back(0x00U);
  bytes.push_back(0x00U);
  bytes.insert(bytes.end(), entropyBytes, 0x00U);
  bytes.push_back(0xffU);
  bytes.push_back(0xd9U);
  return bytes;
}

enum class Sof1Fixture { kZrl, kDcOverflow, kAcOverrun };

std::vector<std::uint8_t> makeSof1Dng(Sof1Fixture fixture) {
  std::vector<std::uint8_t> bytes = makeValidLosslessJpegDng();
  const std::array<std::uint8_t, 2> sof3{{0xffU, 0xc3U}};
  const auto sof = std::search(bytes.begin(), bytes.end(),
                               sof3.begin(), sof3.end());
  if (sof == bytes.end()) throw std::logic_error("synthetic SOF3 is absent");
  sof[1] = 0xc1U;

  const std::array<std::uint8_t, 2> dhtMarker{{0xffU, 0xc4U}};
  const auto dcDht = std::search(bytes.begin(), bytes.end(),
                                 dhtMarker.begin(), dhtMarker.end());
  if (dcDht == bytes.end()) throw std::logic_error("synthetic DHT is absent");
  if (fixture == Sof1Fixture::kDcOverflow) dcDht[21] = 16U;

  const std::array<std::uint8_t, 2> sosMarker{{0xffU, 0xdaU}};
  const auto sos = std::search(bytes.begin(), bytes.end(),
                               sosMarker.begin(), sosMarker.end());
  if (sos == bytes.end()) throw std::logic_error("synthetic SOS is absent");

  std::vector<std::uint8_t> extra;
  if (fixture == Sof1Fixture::kDcOverflow) {
    extra.insert(extra.end(), {0xffU, 0xdbU, 0x00U, 0x83U, 0x10U,
                               0xffU, 0xffU});
    for (int coefficient = 1; coefficient < 64; ++coefficient) {
      extra.push_back(0x00U);
      extra.push_back(0x01U);
    }
  }
  extra.insert(extra.end(), {0xffU, 0xc4U, 0x00U, 0x14U, 0x10U, 0x01U});
  extra.insert(extra.end(), 15U, 0x00U);
  extra.push_back(fixture == Sof1Fixture::kDcOverflow
                      ? 0x00U
                      : (fixture == Sof1Fixture::kAcOverrun ? 0xf1U : 0xf0U));
  bytes.insert(sos, extra.begin(), extra.end());
  return bytes;
}

std::vector<std::uint8_t> makeIdentifyLjpegTiff(unsigned scans) {
  if (scans < 1U || scans > 165U) {
    throw std::invalid_argument("invalid identify-scan count");
  }
  std::vector<std::uint8_t> jpeg =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  std::vector<std::uint8_t> appMarkers;
  for (unsigned marker = 0U; marker < 4U; ++marker) {
    appMarkers.insert(appMarkers.end(), {0xffU, 0xe1U, 0xffU, 0xffU});
    appMarkers.insert(appMarkers.end(), 65533U, 0U);
  }
  jpeg.insert(jpeg.begin() + 2, appMarkers.begin(), appMarkers.end());

  std::vector<TiffEntry> entries{
      {0x00feU, 4U, 1U, 0U},
      {0x0100U, 4U, 1U, 256U},
      {0x0101U, 4U, 1U, 256U},
      {0x0106U, 3U, 1U, 32803U},
      {0x010fU, 2U, 5U, 0U},
      {0x0110U, 2U, 4U, 0x00776152U},
      {0x0115U, 3U, 1U, 1U},
      {0x0116U, 4U, 1U, 256U},
      {0x0117U, 4U, 1U, static_cast<std::uint32_t>(jpeg.size())},
      {0x011cU, 3U, 1U, 1U},
      {0x828dU, 3U, 2U, 0x00020002U},
      {0x828eU, 1U, 4U, 0x02010100U},
      {0xc61dU, 4U, 1U, 65535U},
      {0xc65aU, 3U, 1U, 21U},
  };
  for (unsigned scan = 0U; scan < scans; ++scan) {
    entries.push_back({0x0102U, 3U, 1U, 0U});
    entries.push_back({0x0103U, 3U, 1U, 7U});
    entries.push_back({0x0111U, 4U, 1U, 0U});
  }
  entries.push_back({0x0102U, 3U, 1U, 16U});
  entries.push_back({0x0103U, 3U, 1U, 6U});
  entries.push_back({0x0111U, 4U, 1U, 0U});

  constexpr std::uint32_t kIfdOffset = 8U;
  const std::uint32_t ifdBytes =
      2U + static_cast<std::uint32_t>(entries.size()) * 12U + 4U;
  const std::uint32_t makeOffset = kIfdOffset + ifdBytes;
  const std::uint32_t jpegOffset = makeOffset + 5U;
  std::vector<std::uint8_t> bytes(jpegOffset + jpeg.size(), 0U);
  bytes[0] = 'I';
  bytes[1] = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, static_cast<std::uint16_t>(entries.size()));
  std::size_t cursor = kIfdOffset + 2U;
  for (TiffEntry entry : entries) {
    if (entry.tag == 0x010fU) entry.value = makeOffset;
    if (entry.tag == 0x0111U) entry.value = jpegOffset;
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);
  constexpr std::array<char, 4> kSony{{'S', 'O', 'N', 'Y'}};
  std::copy(kSony.begin(), kSony.end(), bytes.begin() + makeOffset);
  std::copy(jpeg.begin(), jpeg.end(), bytes.begin() + jpegOffset);
  return bytes;
}

std::vector<std::uint8_t> makeFloatDeflateDng(bool repeatedOffsets,
                                              bool omitWhiteLevel,
                                              std::uint16_t bitsPerSample = 32U,
                                              std::uint16_t sampleFormat = 3U,
                                              std::uint16_t compression = 8U) {
  constexpr std::uint32_t kWidth = 64U;
  constexpr std::uint32_t kHeight = 64U;
  const std::uint32_t tileEdge = repeatedOffsets ? 1U : 16U;
  const std::uint32_t tileCount =
      (kWidth / tileEdge) * (kHeight / tileEdge);
  const std::uint32_t paddedBlobBytes = repeatedOffsets ? 65536U : 64U;
  std::vector<TiffEntry> entries{
      {0x00feU, 4U, 1U, 0U},
      {0x0100U, 4U, 1U, kWidth},
      {0x0101U, 4U, 1U, kHeight},
      {0x0102U, 3U, 1U, bitsPerSample},
      {0x0103U, 3U, 1U, compression},
      {0x0106U, 3U, 1U, 32803U},
      {0x0115U, 3U, 1U, 1U},
      {0x011cU, 3U, 1U, 1U},
      {0x0142U, 4U, 1U, tileEdge},
      {0x0143U, 4U, 1U, tileEdge},
      {0x0144U, 4U, tileCount, 0U},
      {0x0145U, 4U, tileCount, 0U},
      {0x0153U, 3U, 1U, sampleFormat},
      {0x828dU, 3U, 2U, 0x00020002U},
      {0x828eU, 1U, 4U, 0x02010100U},
      {0xc612U, 1U, 4U, 0x00000401U},
      {0xc613U, 1U, 4U, 0x00000101U},
      {0xc61dU, 4U, 1U, 1U},
      {0xc65aU, 3U, 1U, 21U},
  };
  if (omitWhiteLevel) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [](const TiffEntry& entry) {
                         return entry.tag == 0xc61dU;
                       }),
        entries.end());
  }
  std::sort(entries.begin(), entries.end(),
            [](const TiffEntry& left, const TiffEntry& right) {
              return left.tag < right.tag;
            });

  std::vector<std::uint8_t> rawTile(
      static_cast<std::size_t>(tileEdge) * tileEdge * sizeof(float), 0U);
  uLongf compressedSize = compressBound(static_cast<uLong>(rawTile.size()));
  std::vector<std::uint8_t> compressed(compressedSize, 0U);
  if (compress2(compressed.data(), &compressedSize, rawTile.data(),
                static_cast<uLong>(rawTile.size()), Z_BEST_SPEED) != Z_OK ||
      compressedSize > paddedBlobBytes) {
    throw std::logic_error("cannot create synthetic deflate tile");
  }
  compressed.resize(compressedSize);

  constexpr std::uint32_t kIfdOffset = 8U;
  const std::uint32_t ifdBytes =
      2U + static_cast<std::uint32_t>(entries.size()) * 12U + 4U;
  const std::uint32_t offsetsAt = kIfdOffset + ifdBytes;
  const std::uint32_t countsAt = offsetsAt + tileCount * 4U;
  const std::uint32_t blobsAt = countsAt + tileCount * 4U;
  const std::uint32_t storedBlobCount = repeatedOffsets ? 1U : tileCount;
  std::vector<std::uint8_t> bytes(
      static_cast<std::size_t>(blobsAt) +
          static_cast<std::size_t>(storedBlobCount) * paddedBlobBytes,
      0U);
  bytes[0] = 'I';
  bytes[1] = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, static_cast<std::uint16_t>(entries.size()));
  std::size_t cursor = kIfdOffset + 2U;
  for (TiffEntry entry : entries) {
    if (entry.tag == 0x0144U) entry.value = offsetsAt;
    if (entry.tag == 0x0145U) entry.value = countsAt;
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);
  for (std::uint32_t tile = 0U; tile < tileCount; ++tile) {
    const std::uint32_t blobOffset = blobsAt +
        (repeatedOffsets ? 0U : tile * paddedBlobBytes);
    put32(bytes, offsetsAt + static_cast<std::size_t>(tile) * 4U, blobOffset);
    put32(bytes, countsAt + static_cast<std::size_t>(tile) * 4U,
          paddedBlobBytes);
    if (!repeatedOffsets || tile == 0U) {
      std::copy(compressed.begin(), compressed.end(),
                bytes.begin() + blobOffset);
    }
  }
  return bytes;
}

}  // namespace

std::vector<std::uint8_t> readHexFixture(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("Cannot open fixture: " + path.string());
  }

  std::vector<std::uint8_t> result;
  int high = -1;
  char c = '\0';
  while (stream.get(c)) {
    if (c == '#') {
      stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) continue;

    const int nibble = hexNibble(c);
    if (nibble < 0) {
      throw std::runtime_error("Non-hex character in fixture: " + path.string());
    }
    if (high < 0) {
      high = nibble;
    } else {
      result.push_back(static_cast<std::uint8_t>((high << 4) | nibble));
      high = -1;
    }
  }

  if (high >= 0) {
    throw std::runtime_error("Odd number of hex nibbles in fixture: " +
                             path.string());
  }
  if (result.size() > kMaxInputBytes) {
    throw std::runtime_error("Fixture exceeds the public-seam input cap: " +
                             path.string());
  }
  return result;
}

std::vector<std::uint8_t> makeNonTerminatedIdentifyHead() {
  std::vector<std::uint8_t> bytes(64U, 0xffU);
  bytes.front() = 0x49U;
  return bytes;
}

std::vector<std::uint8_t> makePxnIdentifyHead() {
  std::vector<std::uint8_t> bytes(64U, 0U);
  std::copy_n("PXN", 3U, bytes.begin());
  return bytes;
}

DecodeOutcome exercisePublicDecode(const std::vector<std::uint8_t>& input,
                                   bool exerciseWavelet) {
  DecodeOutcome outcome{kNotCalled, kNotCalled, kNotCalled,
                        DecodeStage::kNotStarted, 0U, 0U};
  if (input.empty() || input.size() > kMaxInputBytes) return outcome;

  LibRaw raw;
  raw.imgdata.rawparams.max_raw_memory_mb = kMaxRawMemoryMb;
  raw.imgdata.params.output_bps = 8;
  raw.imgdata.params.output_color = 1;
  raw.imgdata.params.no_auto_bright = 1;
  // The general hostile-input path stays half-size. The positive wavelet
  // control must retain >=65 pixels on each axis or LibRaw intentionally skips
  // wavelet_denoise before touching the patched OpenMP allocation.
  raw.imgdata.params.half_size = exerciseWavelet ? 0 : 1;
  raw.imgdata.params.user_qual = 0;
  // A non-zero threshold reaches LibRaw's wavelet denoise path on decodable
  // inputs. The 0.22.2 OpenMP allocation initialization is verified by the
  // shared vendor resolver before this test target can configure.
  raw.imgdata.params.threshold = exerciseWavelet ? 8.0F : 0.0F;

  outcome.openCode = raw.open_buffer(input.data(), input.size());
  outcome.terminalStage = DecodeStage::kOpen;
  if (outcome.openCode != LIBRAW_SUCCESS) return outcome;

  if (declaredDimensionsExceedLimit(raw.imgdata.sizes)) {
    outcome.terminalStage = DecodeStage::kDimensionGuard;
    return outcome;
  }

  outcome.unpackCode = raw.unpack();
  outcome.terminalStage = DecodeStage::kUnpack;
  if (outcome.unpackCode != LIBRAW_SUCCESS) return outcome;

  outcome.processingWidth = raw.imgdata.sizes.iwidth;
  outcome.processingHeight = raw.imgdata.sizes.iheight;

  if (declaredDimensionsExceedLimit(raw.imgdata.sizes)) {
    outcome.terminalStage = DecodeStage::kDimensionGuard;
    return outcome;
  }

  outcome.processCode = raw.dcraw_process();
  outcome.terminalStage = DecodeStage::kProcess;
  return outcome;
}

std::vector<std::uint8_t> makeMalformedLosslessJpegDng(
    const std::vector<std::uint8_t>& stripBytes,
    std::uint32_t declaredStripBytes) {
  return makeCfaDng(7U, stripBytes, declaredStripBytes);
}

std::vector<std::uint8_t> makeTruncatedLosslessJpegStrip(
    std::uint8_t category, std::size_t entropyBytes) {
  // Minimal one-component, 16-bit, 256 x 256 lossless-JPEG interchange header.
  // The DHT has one one-bit code whose decoded difference category is supplied
  // by the caller. Entropy is then intentionally cut short and has no EOI.
  std::vector<std::uint8_t> bytes{
      0xffU, 0xd8U,                          // SOI
      0xffU, 0xc3U, 0x00U, 0x0bU,           // SOF3, 11 bytes
      0x10U, 0x01U, 0x00U, 0x01U, 0x00U,    // 16 bit, 256 x 256
      0x01U, 0x01U, 0x11U, 0x00U,           // one component
      0xffU, 0xc4U, 0x00U, 0x14U, 0x00U,    // DHT, table 0
      0x01U,                                 // one code of length 1
  };
  bytes.insert(bytes.end(), 15U, 0x00U);     // remaining code lengths
  bytes.push_back(category);                 // one Huffman symbol
  const std::array<std::uint8_t, 10> scan{{
      0xffU, 0xdaU, 0x00U, 0x08U,           // SOS, 8 bytes
      0x01U, 0x01U, 0x00U,                   // component 1, table 0
      0x01U, 0x00U, 0x00U,                   // predictor 1, no point transform
  }};
  bytes.insert(bytes.end(), scan.begin(), scan.end());
  bytes.insert(bytes.end(), entropyBytes, 0x00U);
  return bytes;
}

std::vector<std::uint8_t> makeValidUncompressedDng(
    std::uint32_t width, std::uint32_t height) {
  const std::size_t pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (width == 0U || height == 0U ||
      pixelCount > std::numeric_limits<std::uint32_t>::max() / 2U) {
    throw std::invalid_argument("synthetic uncompressed DNG is too large");
  }
  std::vector<std::uint8_t> strip(pixelCount * 2U, 0U);
  for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
    const std::uint16_t sample =
        static_cast<std::uint16_t>(1024U + (pixel % width) * 512U);
    strip[pixel * 2U] = static_cast<std::uint8_t>(sample & 0xffU);
    strip[pixel * 2U + 1U] =
        static_cast<std::uint8_t>((sample >> 8U) & 0xffU);
  }
  return makeCfaDng(1U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, width, height);
}

std::vector<std::uint8_t> makeDeclaredOversizeUncompressedDng(
    std::uint32_t width, std::uint32_t height) {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    width, height);
}

std::vector<std::uint8_t> makeEmbeddedProfileDng(std::size_t profileBytes) {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    256U, 256U, profileBytes);
}

std::vector<std::uint8_t> makeDuplicateOpcodeDng() {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    256U, 256U, 0U, MetadataFixture::kDuplicateOpcode);
}

std::vector<std::uint8_t> makeOverBudgetMetadataDng() {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    256U, 256U, 0U, MetadataFixture::kAggregateOverBudget);
}

std::vector<std::uint8_t> makeDuplicateStripTablesDng() {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    256U, 256U, 0U, MetadataFixture::kDuplicateStripTables);
}

std::vector<std::uint8_t> makeValidMultiStripDng() {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    256U, 256U, 0U, MetadataFixture::kValidMultiStrip);
}

std::vector<std::uint8_t> makeExtremeAspectDng() {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    4096U, 3072U, 0U, MetadataFixture::kExtremeAspect);
}

std::vector<std::uint8_t> makeBoundaryAspectDng(bool belowOne) {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(
      1U, tinyStrip, static_cast<std::uint32_t>(tinyStrip.size()), false,
      4096U, 3072U, 0U,
      belowOne ? MetadataFixture::kBoundaryAspectBelow
               : MetadataFixture::kBoundaryAspectAbove);
}

std::vector<std::uint8_t> makeHostileActiveAreaDng() {
  const std::vector<std::uint8_t> tinyStrip{0U, 0U};
  return makeCfaDng(1U, tinyStrip,
                    static_cast<std::uint32_t>(tinyStrip.size()), false,
                    4096U, 3072U, 0U, MetadataFixture::kHostileActiveArea);
}

std::vector<std::uint8_t> makeHostileCr2SliceTiff() {
  // One length-1 Huffman code maps every sample to category zero. Exactly one
  // bit per 4096 x 256 JPEG sample plus padding gives lossless_jpeg_load_raw
  // columns far enough beyond the 256 x 256 TIFF raw allocation (including
  // LibRaw's eight guard rows) to reproduce TALOS-2026-2331 without the 0.22.2
  // column check. EOI makes this complete, not an early-truncation test.
  std::vector<std::uint8_t> strip =
      makeTruncatedLosslessJpegStrip(0U, 131072U + 16U);
  // The generic (non-DNG) ljpeg_start path consumes one pad byte after the
  // nine-byte SOF3 payload. Without it the next 0xff DHT marker is skipped and
  // this security fixture never reaches lossless_jpeg_load_raw.
  strip.insert(strip.begin() + 15, 0x00U);
  // SOF3 stores height then width as big-endian uint16. The common helper uses
  // 256 x 256; widen only this security fixture's JPEG stream to 4096 x 256.
  strip.at(9U) = 0x10U;
  strip.at(10U) = 0x00U;
  strip.push_back(0xffU);
  strip.push_back(0xd9U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()), true);
}

std::vector<std::uint8_t> makeOverflowingCr2SliceTiff() {
  std::vector<std::uint8_t> bytes = makeHostileCr2SliceTiff();
  const std::uint32_t ifdOffset = get32(bytes, 4U);
  const std::uint16_t entryCount = get16(bytes, ifdOffset);
  std::uint32_t sliceOffset = 0U;
  for (std::uint16_t index = 0U; index < entryCount; ++index) {
    const std::size_t entry = ifdOffset + 2U +
        static_cast<std::size_t>(index) * 12U;
    const std::uint16_t tag = get16(bytes, entry);
    if (tag == 0x0100U) put32(bytes, entry + 8U, 22U);
    if (tag == 0x0101U || tag == 0x0116U) {
      put32(bytes, entry + 8U, 64000U);
    }
    if (tag == 0xc640U) sliceOffset = get32(bytes, entry + 8U);
  }
  if (sliceOffset == 0U) {
    throw std::logic_error("synthetic CR2Slice metadata is absent");
  }
  put16(bytes, sliceOffset, 1U);
  put16(bytes, sliceOffset + 2U, 65535U);
  put16(bytes, sliceOffset + 4U, 65535U);
  return bytes;
}

std::vector<std::uint8_t> makeOversizedLjpegGeometryTiff() {
  std::vector<std::uint8_t> strip =
      makeTruncatedLosslessJpegStrip(0U, 16U);
  strip.insert(strip.begin() + 15, 0x00U);
  // SOF3 height=2047, width=65535, one component: 134,150,145 decoded
  // samples from a sub-kilobyte container without the pre-loop work guard.
  strip.at(7U) = 0x07U;
  strip.at(8U) = 0xffU;
  strip.at(9U) = 0xffU;
  strip.at(10U) = 0xffU;
  strip.push_back(0xffU);
  strip.push_back(0xd9U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()), true);
}

std::vector<std::uint8_t> makeOversizedLjpegGeometryDng() {
  std::vector<std::uint8_t> strip =
      makeTruncatedLosslessJpegStrip(0U, 16U);
  strip.at(7U) = 0x07U;
  strip.at(8U) = 0xffU;
  strip.at(9U) = 0xffU;
  strip.at(10U) = 0xffU;
  strip.push_back(0xffU);
  strip.push_back(0xd9U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()));
}

std::vector<std::uint8_t> makeValidLosslessJpegDng() {
  // Category zero needs one Huffman bit per sample. The extra bytes cover
  // LibRaw's bounded bit-reader lookahead; EOI makes the stream complete.
  std::vector<std::uint8_t> strip =
      makeTruncatedLosslessJpegStrip(0U, 8192U + 16U);
  strip.push_back(0xffU);
  strip.push_back(0xd9U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()));
}

std::vector<std::uint8_t> makeSof1ZrlDng() {
  return makeSof1Dng(Sof1Fixture::kZrl);
}

std::vector<std::uint8_t> makeSof1DcOverflowDng() {
  return makeSof1Dng(Sof1Fixture::kDcOverflow);
}

std::vector<std::uint8_t> makeSof1AcOverrunDng() {
  return makeSof1Dng(Sof1Fixture::kAcOverrun);
}

std::vector<std::uint8_t> makeValidIdentifyLjpegTiff() {
  return makeIdentifyLjpegTiff(1U);
}

std::vector<std::uint8_t> makeRepeatedIdentifyLjpegTiff() {
  return makeIdentifyLjpegTiff(165U);
}

std::vector<std::uint8_t> makeDistinctTileFloatDeflateDng() {
  return makeFloatDeflateDng(false, false);
}

std::vector<std::uint8_t> makeRepeatedTileFloatDeflateDng() {
  return makeFloatDeflateDng(true, false);
}

std::vector<std::uint8_t> makeFloat32DngWithoutWhiteLevel() {
  return makeFloatDeflateDng(false, true);
}

std::vector<std::uint8_t> makeInteger31DngWithoutWhiteLevel() {
  return makeFloatDeflateDng(false, true, 31U, 1U);
}

std::vector<std::uint8_t> makeAdobeDeflateDng() {
  return makeFloatDeflateDng(false, false, 32U, 3U, 0x80b2U);
}

std::vector<std::uint8_t> makeExcessiveTileCountLosslessJpegDng() {
  std::vector<std::uint8_t> strip =
      makeTruncatedLosslessJpegStrip(0U, 16U);
  // One decoded sample per 1 x 1 tile keeps this regression independent of the
  // cumulative sample budget: only the number of decoder initializations is
  // excessive.
  strip.at(7U) = 0x00U;
  strip.at(8U) = 0x01U;
  strip.at(9U) = 0x00U;
  strip.at(10U) = 0x01U;
  strip.push_back(0xffU);
  strip.push_back(0xd9U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U,
                    MetadataFixture::kExcessiveTiles);
}

std::vector<std::uint8_t> makeExcessiveTileCountUncompressedDng() {
  const std::vector<std::uint8_t> strip{0U, 0U};
  return makeCfaDng(1U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U,
                    MetadataFixture::kExcessiveTiles);
}

std::vector<std::uint8_t> makeCompliantTiledLosslessJpegDng() {
  std::vector<std::uint8_t> strip =
      makeTruncatedLosslessJpegStrip(0U, 48U);
  strip.at(7U) = 0x00U;
  strip.at(8U) = 0x10U;
  strip.at(9U) = 0x00U;
  strip.at(10U) = 0x10U;
  strip.push_back(0xffU);
  strip.push_back(0xd9U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 1040U, 1040U, 0U,
                    MetadataFixture::kCompliantDngTiles);
}

std::vector<std::uint8_t> makeValidSonyLjpegTiff() {
  const std::vector<std::uint8_t> strip =
      makeSonyLosslessJpegStrip(128U, 128U, 4U, 8192U + 16U);
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

namespace {

enum class CanonSrawFixture {
  kValid,
  kNegativeChroma,
  kShortSof,
  kRedundantSlices,
  kLargeModelVersion,
};

std::vector<std::uint8_t> makeCanonSrawTiff(CanonSrawFixture fixture) {
  std::vector<std::uint8_t> bytes = makeValidSonyLjpegTiff();
  const bool negativeChroma = fixture == CanonSrawFixture::kNegativeChroma;
  if (negativeChroma) {
    const std::array<std::uint8_t, 2> dhtMarker{{0xffU, 0xc4U}};
    const auto dht = std::search(bytes.begin(), bytes.end(),
                                 dhtMarker.begin(), dhtMarker.end());
    if (dht == bytes.end()) {
      throw std::logic_error("synthetic Canon DHT is absent");
    }
    dht[21] = 16U;
  }

  const std::array<std::uint8_t, 2> eoiMarker{{0xffU, 0xd9U}};
  auto eoi = std::find_end(bytes.begin(), bytes.end(),
                           eoiMarker.begin(), eoiMarker.end());
  if (eoi == bytes.end()) {
    throw std::logic_error("synthetic Canon EOI is absent");
  }
  constexpr std::size_t kCategory16EntropyBytes = 32U * 1024U;
  const std::size_t extraEntropy = negativeChroma
      ? kCategory16EntropyBytes
      : 0U;
  bytes.insert(eoi, extraEntropy, 0U);

  const std::uint32_t canonOffset = static_cast<std::uint32_t>(bytes.size());
  constexpr std::array<std::uint8_t, 6> kCanon{{'C', 'a', 'n', 'o', 'n', 0}};
  bytes.insert(bytes.end(), kCanon.begin(), kCanon.end());
  const bool largeModelVersion =
      fixture == CanonSrawFixture::kLargeModelVersion;
  const std::uint32_t modelOffset = static_cast<std::uint32_t>(bytes.size());
  constexpr std::array<std::uint8_t, 12> kLargeModel{{
      '2', '1', '4', '7', '4', '8', '3', '.', '0', '.', '0', 0}};
  if (largeModelVersion) {
    bytes.insert(bytes.end(), kLargeModel.begin(), kLargeModel.end());
  }
  const std::uint32_t sliceOffset = static_cast<std::uint32_t>(bytes.size());
  bytes.resize(bytes.size() + 6U, 0U);
  put16(bytes, sliceOffset,
        fixture == CanonSrawFixture::kRedundantSlices ? 65535U : 0U);
  put16(bytes, sliceOffset + 2U, 65535U);
  put16(bytes, sliceOffset + 4U, 65535U);

  const std::uint32_t width = 256U;
  const std::uint32_t height =
      fixture == CanonSrawFixture::kShortSof ? 65U : 23U;
  const std::uint32_t ifdOffset = get32(bytes, 4U);
  const std::uint16_t entryCount = get16(bytes, ifdOffset);
  bool replacedWhiteLevel = false;
  for (std::uint16_t index = 0U; index < entryCount; ++index) {
    const std::size_t entry = ifdOffset + 2U +
        static_cast<std::size_t>(index) * 12U;
    const std::uint16_t tag = get16(bytes, entry);
    if (tag == 0x0100U) put32(bytes, entry + 8U, width);
    if (tag == 0x0101U || tag == 0x0116U) {
      put32(bytes, entry + 8U, height);
    }
    if (tag == 0x0102U) put16(bytes, entry + 8U, 15U);
    if (tag == 0x010fU) {
      put32(bytes, entry + 4U, 6U);
      put32(bytes, entry + 8U, canonOffset);
    }
    if (tag == 0x0110U && largeModelVersion) {
      put32(bytes, entry + 4U, static_cast<std::uint32_t>(kLargeModel.size()));
      put32(bytes, entry + 8U, modelOffset);
    }
    if (tag == 0x0117U) {
      put32(bytes, entry + 8U,
            get32(bytes, entry + 8U) +
                static_cast<std::uint32_t>(extraEntropy));
    }
    if (tag == 0xc61dU) {
      put16(bytes, entry, 0xc640U);
      put16(bytes, entry + 2U, 3U);
      put32(bytes, entry + 4U, 3U);
      put32(bytes, entry + 8U, sliceOffset);
      replacedWhiteLevel = true;
    }
  }
  if (!replacedWhiteLevel) {
    throw std::logic_error("synthetic Canon WhiteLevel tag is absent");
  }
  return bytes;
}

std::vector<std::uint8_t> makeCanonSrawWhiteBalanceTiff(bool hostile) {
  std::vector<std::uint8_t> bytes = makeValidSonyLjpegTiff();
  const std::uint32_t canonOffset = static_cast<std::uint32_t>(bytes.size());
  constexpr std::array<std::uint8_t, 6> kCanon{{'C', 'a', 'n', 'o', 'n', 0}};
  bytes.insert(bytes.end(), kCanon.begin(), kCanon.end());

  const std::uint32_t exifOffset = static_cast<std::uint32_t>(bytes.size());
  constexpr std::uint32_t kIfdBytes = 18U;
  constexpr std::uint32_t kColorWords = 1250U;
  constexpr std::uint32_t kColorBytes = kColorWords * 2U;
  const std::uint32_t makerOffset = exifOffset + kIfdBytes;
  const std::uint32_t colorOffset = makerOffset + kIfdBytes;
  const std::uint32_t makerBytes = kIfdBytes + kColorBytes;
  bytes.resize(static_cast<std::size_t>(colorOffset) + kColorBytes, 0U);

  put16(bytes, exifOffset, 1U);
  put16(bytes, exifOffset + 2U, 0x927cU);
  put16(bytes, exifOffset + 4U, 7U);
  put32(bytes, exifOffset + 6U, makerBytes);
  put32(bytes, exifOffset + 10U, makerOffset);
  put32(bytes, exifOffset + 14U, 0U);

  put16(bytes, makerOffset, 1U);
  put16(bytes, makerOffset + 2U, 0x4001U);
  put16(bytes, makerOffset + 4U, 3U);
  put32(bytes, makerOffset + 6U, kColorWords);
  put32(bytes, makerOffset + 10U, colorOffset);
  put32(bytes, makerOffset + 14U, 0U);

  constexpr std::size_t kSrawWordOffset = 0x004eU;
  const std::uint16_t multiplier = hostile ? 65535U : 1024U;
  for (std::size_t channel = 0U; channel < 4U; ++channel) {
    put16(bytes, colorOffset + (kSrawWordOffset + channel) * 2U,
          multiplier);
  }

  const std::uint32_t ifdOffset = get32(bytes, 4U);
  const std::uint16_t entryCount = get16(bytes, ifdOffset);
  bool installedExif = false;
  for (std::uint16_t index = 0U; index < entryCount; ++index) {
    const std::size_t entry = ifdOffset + 2U +
        static_cast<std::size_t>(index) * 12U;
    const std::uint16_t tag = get16(bytes, entry);
    if (tag == 0x0102U) put16(bytes, entry + 8U, 15U);
    if (tag == 0x010fU) {
      put32(bytes, entry + 4U, 6U);
      put32(bytes, entry + 8U, canonOffset);
    }
    if (tag == 0xc65aU) {
      put16(bytes, entry, 0x8769U);
      put16(bytes, entry + 2U, 4U);
      put32(bytes, entry + 4U, 1U);
      put32(bytes, entry + 8U, exifOffset);
      installedExif = true;
    }
  }
  if (!installedExif) {
    throw std::logic_error("synthetic Canon Exif pointer is absent");
  }
  return bytes;
}

}  // namespace

std::vector<std::uint8_t> makeValidCanonSrawTiff() {
  return makeCanonSrawTiff(CanonSrawFixture::kValid);
}

std::vector<std::uint8_t> makeNegativeChromaCanonSrawTiff() {
  return makeCanonSrawTiff(CanonSrawFixture::kNegativeChroma);
}

std::vector<std::uint8_t> makeShortSofCanonSrawTiff() {
  return makeCanonSrawTiff(CanonSrawFixture::kShortSof);
}

std::vector<std::uint8_t> makeRedundantSlicesCanonSrawTiff() {
  return makeCanonSrawTiff(CanonSrawFixture::kRedundantSlices);
}

std::vector<std::uint8_t> makeLargeModelVersionCanonSrawTiff() {
  return makeCanonSrawTiff(CanonSrawFixture::kLargeModelVersion);
}

std::vector<std::uint8_t> makeValidCanonSrawWhiteBalanceTiff() {
  return makeCanonSrawWhiteBalanceTiff(false);
}

std::vector<std::uint8_t> makeOverflowingCanonSrawWhiteBalanceTiff() {
  return makeCanonSrawWhiteBalanceTiff(true);
}

std::vector<std::uint8_t> makeInvalidSonyLjpegComponentsTiff() {
  const std::vector<std::uint8_t> strip =
      makeSonyLosslessJpegStrip(128U, 128U, 1U, 2048U + 16U);
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

std::vector<std::uint8_t> makeExcessiveSonyTileCountTiff() {
  const std::vector<std::uint8_t> strip =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U,
                    MetadataFixture::kExcessiveSonyTiles);
}

std::vector<std::uint8_t> makeTruncatedDhtSonyTiff() {
  const std::vector<std::uint8_t> complete =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  const std::array<std::uint8_t, 2> marker{{0xffU, 0xc4U}};
  const auto dht = std::search(complete.begin(), complete.end(),
                               marker.begin(), marker.end());
  if (dht == complete.end()) throw std::logic_error("synthetic DHT is absent");
  std::vector<std::uint8_t> strip(complete.begin(), dht);
  strip.insert(strip.end(), marker.begin(), marker.end());
  strip.push_back(0x00U);
  strip.push_back(0x13U);  // 17-byte payload: id + counts, but no one leaf
  strip.push_back(0x00U);
  strip.push_back(0x01U);
  strip.insert(strip.end(), 15U, 0x00U);
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

std::vector<std::uint8_t> makeDuplicateDhtSonyTiff() {
  const std::vector<std::uint8_t> complete =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  const std::array<std::uint8_t, 2> dhtMarker{{0xffU, 0xc4U}};
  const std::array<std::uint8_t, 2> sosMarker{{0xffU, 0xdaU}};
  const auto dht = std::search(complete.begin(), complete.end(),
                               dhtMarker.begin(), dhtMarker.end());
  const auto sos = std::search(complete.begin(), complete.end(),
                               sosMarker.begin(), sosMarker.end());
  if (dht == complete.end() || sos == complete.end() || sos <= dht) {
    throw std::logic_error("synthetic JPEG markers are absent");
  }
  std::vector<std::uint8_t> strip(complete.begin(), dht);
  strip.insert(strip.end(), dhtMarker.begin(), dhtMarker.end());
  strip.push_back(0x00U);
  strip.push_back(0x26U);  // two 18-byte table records plus length field
  for (int table = 0; table < 2; ++table) {
    strip.push_back(0x00U);
    strip.push_back(0x01U);
    strip.insert(strip.end(), 15U, 0x00U);
    strip.push_back(0x00U);
  }
  strip.insert(strip.end(), sos, complete.end());
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

std::vector<std::uint8_t> makeTerminalDhtOverflowSonyTiff() {
  const std::vector<std::uint8_t> complete =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  const std::array<std::uint8_t, 2> marker{{0xffU, 0xc4U}};
  const auto dht = std::search(complete.begin(), complete.end(),
                               marker.begin(), marker.end());
  if (dht == complete.end()) throw std::logic_error("synthetic DHT is absent");
  std::vector<std::uint8_t> strip(complete.begin(), dht);
  strip.insert(strip.end(), marker.begin(), marker.end());
  strip.push_back(0xffU);
  strip.push_back(0xffU);  // maximum JPEG segment length: payload=65,533
  for (int table = 0; table < 15; ++table) {
    strip.push_back(0x00U);
    strip.insert(strip.end(), 16U, 0xffU);
    strip.insert(strip.end(), 4080U, 0x00U);
  }
  strip.push_back(0x00U);
  strip.insert(strip.end(), 15U, 0xffU);
  strip.push_back(223U);
  strip.insert(strip.end(), 4048U, 0x00U);
  // A final permitted table id with only 12 count bytes. Unpatched
  // make_decoder_ref() consumes all 16 and reads exactly beyond data_buffer.
  strip.push_back(0x00U);
  strip.insert(strip.end(), 12U, 0x00U);
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

std::vector<std::uint8_t> makeOversubscribedDhtSonyTiff() {
  const std::vector<std::uint8_t> complete =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  const std::array<std::uint8_t, 2> dhtMarker{{0xffU, 0xc4U}};
  const std::array<std::uint8_t, 2> sosMarker{{0xffU, 0xdaU}};
  const auto dht = std::search(complete.begin(), complete.end(),
                               dhtMarker.begin(), dhtMarker.end());
  const auto sos = std::search(complete.begin(), complete.end(),
                               sosMarker.begin(), sosMarker.end());
  if (dht == complete.end() || sos == complete.end() || sos <= dht) {
    throw std::logic_error("synthetic JPEG markers are absent");
  }
  std::vector<std::uint8_t> strip(complete.begin(), dht);
  strip.insert(strip.end(), dhtMarker.begin(), dhtMarker.end());
  strip.push_back(0x00U);
  strip.push_back(0x16U);  // id + 16 counts + three symbols + length field
  strip.push_back(0x00U);
  strip.push_back(0x03U);  // three length-one codes cannot fit two code slots
  strip.insert(strip.end(), 15U, 0x00U);
  strip.insert(strip.end(), 3U, 0x00U);
  strip.insert(strip.end(), sos, complete.end());
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

std::vector<std::uint8_t> makeNegativeEffectiveBitsSonyTiff() {
  std::vector<std::uint8_t> strip =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  strip.at(6U) = 0x01U;  // one-bit SOF precision
  const std::array<std::uint8_t, 2> marker{{0xffU, 0xdaU}};
  const auto sos = std::search(strip.begin(), strip.end(),
                               marker.begin(), marker.end());
  if (sos == strip.end()) throw std::logic_error("synthetic SOS is absent");
  const std::size_t sosOffset =
      static_cast<std::size_t>(std::distance(strip.begin(), sos));
  const std::uint16_t sosLength = static_cast<std::uint16_t>(
      (strip.at(sosOffset + 2U) << 8U) | strip.at(sosOffset + 3U));
  strip.at(sosOffset + 1U + sosLength) = 0x0fU;  // point transform 15
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 256U, 256U, 0U, MetadataFixture::kSonyTiff);
}

std::vector<std::uint8_t> makeBoundarySonyTileCountTiff() {
  const std::vector<std::uint8_t> strip =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 64U, 64U, 0U,
                    MetadataFixture::kExcessiveSonyTiles);
}

std::vector<std::uint8_t> makeCumulativeDhtWorkSonyTiff() {
  const std::vector<std::uint8_t> complete =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  const std::array<std::uint8_t, 2> dhtMarker{{0xffU, 0xc4U}};
  const std::array<std::uint8_t, 2> sosMarker{{0xffU, 0xdaU}};
  const auto dht = std::search(complete.begin(), complete.end(),
                               dhtMarker.begin(), dhtMarker.end());
  const auto sos = std::search(complete.begin(), complete.end(),
                               sosMarker.begin(), sosMarker.end());
  if (dht == complete.end() || sos == complete.end() || sos <= dht) {
    throw std::logic_error("synthetic JPEG markers are absent");
  }
  std::vector<std::uint8_t> strip(complete.begin(), dht);
  strip.insert(strip.end(), dhtMarker.begin(), dhtMarker.end());
  strip.push_back(0x00U);
  strip.push_back(0x92U);  // eight 18-byte tables plus the length field
  const std::array<std::uint8_t, 8> ids{{0U, 1U, 2U, 3U,
                                        16U, 17U, 18U, 19U}};
  for (const std::uint8_t id : ids) {
    strip.push_back(id);
    strip.insert(strip.end(), 15U, 0x00U);
    strip.push_back(0x01U);  // one valid code at maximum length 16
    strip.push_back(0x00U);
  }
  strip.insert(strip.end(), sos, complete.end());
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 64U, 64U, 0U,
                    MetadataFixture::kExcessiveSonyTiles);
}

std::vector<std::uint8_t> makeCumulativeMarkerWorkSonyTiff() {
  std::vector<std::uint8_t> strip{0xffU, 0xd8U};
  for (int segment = 0; segment < 16; ++segment) {
    strip.push_back(0xffU);
    strip.push_back(0xe1U);  // ignored APP1 marker
    strip.push_back(0xffU);
    strip.push_back(0xffU);  // maximum 65,535-byte segment length
    strip.insert(strip.end(), 65'533U, 0x00U);
  }
  const std::vector<std::uint8_t> body =
      makeSonyLosslessJpegStrip(1U, 1U, 4U, 16U);
  strip.insert(strip.end(), body.begin() + 2, body.end());
  return makeCfaDng(6U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, 64U, 64U, 0U,
                    MetadataFixture::kExcessiveSonyTiles);
}

std::vector<std::uint8_t> makeHasselbladTiff(
    bool oddWidth, std::uint8_t huffmanCategory) {
  std::vector<std::uint8_t> strip =
      makeSonyLosslessJpegStrip(1U, 1U, 1U, 4096U);
  const std::array<std::uint8_t, 2> marker{{0xffU, 0xc4U}};
  const auto dht = std::search(strip.begin(), strip.end(),
                               marker.begin(), marker.end());
  if (dht == strip.end()) throw std::logic_error("synthetic DHT is absent");
  const std::size_t dhtOffset =
      static_cast<std::size_t>(std::distance(strip.begin(), dht));
  strip.at(dhtOffset + 21U) = huffmanCategory;
  // Non-DNG ljpeg_start consumes one pad byte after a nine-byte SOF3 payload.
  strip.insert(strip.begin() + static_cast<std::ptrdiff_t>(dhtOffset), 0x00U);
  return makeCfaDng(7U, strip, static_cast<std::uint32_t>(strip.size()),
                    false, oddWidth ? 23U : 22U, 22U, 0U,
                     MetadataFixture::kHasselbladTiff);
}

namespace {

enum class OlympusFixture { kTag641, kUnaryWork, kNegativePredictor };

std::vector<std::uint8_t> makeOlympusTiff(OlympusFixture fixture,
                                          std::uint16_t value) {
  constexpr std::uint32_t kIfdOffset = 8U;
  constexpr std::uint32_t kEntryCount = 16U;
  constexpr std::uint32_t kMakeOffset =
      kIfdOffset + 2U + kEntryCount * 12U + 4U;
  constexpr std::uint32_t kModelOffset = kMakeOffset + 8U;
  constexpr std::uint32_t kExifOffset = kModelOffset + 5U;
  constexpr std::uint32_t kMakerOffset = kExifOffset + 18U;
  constexpr std::uint32_t kNestedIfdOffset = kMakerOffset + 30U;
  constexpr std::uint32_t kStripOffset = kMakerOffset + 84U;
  const bool unaryWork = fixture == OlympusFixture::kUnaryWork;
  const bool negativePredictor =
      fixture == OlympusFixture::kNegativePredictor;
  const std::uint32_t width = unaryWork ? 24U : 256U;
  const std::uint32_t height = unaryWork ? 24U : 256U;
  const std::uint32_t stripBytes =
      unaryWork ? 40U * 1024U : width * height * 2U;
  const std::vector<TiffEntry> topEntries{
      {0x00feU, 4U, 1U, 0U},
      {0x0100U, 4U, 1U, width},
      {0x0101U, 4U, 1U, height},
      {0x0102U, 3U, 1U, 14U},
      {0x0103U, 3U, 1U, 1U},
      {0x0106U, 3U, 1U, 32803U},
      {0x010fU, 2U, 8U, kMakeOffset},
      {0x0110U, 2U, 5U, kModelOffset},
      {0x0111U, 4U, 1U, kStripOffset},
      {0x0115U, 3U, 1U, 1U},
      {0x0116U, 4U, 1U, height},
      {0x0117U, 4U, 1U, stripBytes},
      {0x011cU, 3U, 1U, 1U},
      {0x828dU, 3U, 2U, 0x00020002U},
      {0x828eU, 1U, 4U, 0x02010100U},
      {0x8769U, 4U, 1U, kExifOffset},
  };

  std::vector<std::uint8_t> bytes(kStripOffset + stripBytes, 0U);
  bytes.at(0U) = 'I';
  bytes.at(1U) = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, kEntryCount);
  std::size_t cursor = kIfdOffset + 2U;
  for (const TiffEntry& entry : topEntries) {
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);
  std::copy_n("OLYMPUS", 8U, bytes.begin() + kMakeOffset);
  std::copy_n("E-M1", 5U, bytes.begin() + kModelOffset);

  put16(bytes, kExifOffset, 1U);
  put16(bytes, kExifOffset + 2U, 0x927cU);
  put16(bytes, kExifOffset + 4U, 7U);
  put32(bytes, kExifOffset + 6U, 84U);
  put32(bytes, kExifOffset + 10U, kMakerOffset);
  put32(bytes, kExifOffset + 14U, 0U);

  std::copy_n("OLYMPUS", 8U, bytes.begin() + kMakerOffset);
  bytes.at(kMakerOffset + 8U) = 'I';
  bytes.at(kMakerOffset + 9U) = 'I';
  put16(bytes, kMakerOffset + 10U, 3U);
  put16(bytes, kMakerOffset + 12U, 1U);
  cursor = kMakerOffset + 14U;
  put16(bytes, cursor, 0x2040U);
  put16(bytes, cursor + 2U, 4U);
  put32(bytes, cursor + 4U, 1U);
  put32(bytes, cursor + 8U, kNestedIfdOffset - kMakerOffset);
  put32(bytes, cursor + 12U, 0U);

  std::vector<TiffEntry> makerEntries{
      {0x0611U, 3U, 1U, 14U},
      {0x0641U, 3U, 1U,
       fixture == OlympusFixture::kTag641 ? value : 1U},
      {static_cast<std::uint16_t>(
           unaryWork ? 0x0652U : (negativePredictor ? 0x0640U : 0x0645U)),
       3U, 1U,
       fixture == OlympusFixture::kTag641 ? 1U : value},
      {0x0653U, 3U, 1U, 1U},
  };
  std::sort(makerEntries.begin(), makerEntries.end(),
            [](const TiffEntry& left, const TiffEntry& right) {
              return left.tag < right.tag;
            });
  put16(bytes, kNestedIfdOffset,
        static_cast<std::uint16_t>(makerEntries.size()));
  cursor = kNestedIfdOffset + 2U;
  for (const TiffEntry& entry : makerEntries) {
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);
  bytes.at(kStripOffset) = 1U;
  if (fixture == OlympusFixture::kTag641) {
    std::fill(bytes.begin() + kStripOffset + 3U, bytes.end(), 0x55U);
  } else if (negativePredictor) {
    std::fill(bytes.begin() + kStripOffset + 3U, bytes.end(), 0xffU);
  }
  return bytes;
}

enum class PanasonicC8Fixture {
  kMissingTables,
  kNegativeShift,
  kSignedShift,
  kTag41Shift64,
  kNoMatch,
  kOverrangeTable,
  kHlowZero,
  kHlow17,
  kCodeAtLimit,
  kCodeAboveStoredDomain,
  kWideStripeLeft,
  kTag40Count16,
  kTag40Count18,
  kTag41Count16,
  kTag41Count18,
  kStripeCountMismatch,
  kBorrowedBits,
  kValid,
  kValidTwoStripe,
  kValidShadowed,
  kOverlap,
};

std::vector<std::uint8_t> makePanasonicC8Tiff(
    PanasonicC8Fixture fixture) {
  const bool hasTables = fixture != PanasonicC8Fixture::kMissingTables;
  const bool twoStripes = fixture == PanasonicC8Fixture::kValidTwoStripe ||
      fixture == PanasonicC8Fixture::kStripeCountMismatch ||
      fixture == PanasonicC8Fixture::kBorrowedBits ||
      fixture == PanasonicC8Fixture::kOverlap;
  constexpr std::uint32_t kIfdOffset = 8U;
  const std::uint32_t entryCount = hasTables ? 23U : 21U;
  const std::uint32_t payloadOffset =
      kIfdOffset + 2U + entryCount * 12U + 4U;
  const std::uint32_t signatureOffset = payloadOffset;
  const std::uint32_t tag40Offset = signatureOffset + 16U;
  const std::uint32_t tag41Offset =
      tag40Offset + (hasTables ? 70U : 0U);
  const std::uint32_t tag44Offset =
      tag41Offset + (hasTables ? 36U : 0U);
  const std::uint32_t tag45Offset = tag44Offset + 50U;
  const std::uint32_t tag46Offset = tag45Offset + 50U;
  const std::uint32_t tag47Offset = tag46Offset + 50U;
  const std::uint32_t tag48Offset = tag47Offset + 26U;
  const std::uint32_t makeOffset = tag48Offset + 26U;
  const std::uint32_t modelOffset = makeOffset + 10U;
  const std::uint32_t stripeOffset = modelOffset + 7U;
  const std::uint32_t defaultStripeBytes =
      fixture == PanasonicC8Fixture::kOverrangeTable ||
          fixture == PanasonicC8Fixture::kValidShadowed ||
          fixture == PanasonicC8Fixture::kNegativeShift ||
          fixture == PanasonicC8Fixture::kSignedShift
      ? 2048U
      : (twoStripes ? 192U : 368U);
  const std::uint32_t firstStripeBytes =
      fixture == PanasonicC8Fixture::kBorrowedBits
          ? 177U
          : defaultStripeBytes;
  const std::uint32_t secondStripeBytes =
      fixture == PanasonicC8Fixture::kBorrowedBits
          ? 180U
          : defaultStripeBytes;
  const std::uint32_t stripeCount = twoStripes ? 2U : 1U;
  const std::uint32_t secondStripeOffset = stripeOffset + firstStripeBytes;

  std::vector<TiffEntry> entries{
      {0x0001U, 1U, 4U, 1U},
      {0x0002U, 4U, 1U, 24U},
      {0x0003U, 4U, 1U, 24U},
      {0x0009U, 3U, 1U, 1U},
      {0x000aU, 3U, 1U, 12U},
      {0x000bU, 3U, 1U, 1U},
      {0x002dU, 3U, 1U, 8U},
      {0x003bU, 3U, 1U, 4095U},
      {0x003cU, 3U, 1U, 100U},
      {0x003dU, 3U, 1U, 100U},
      {0x003eU, 3U, 1U, 100U},
      {0x003fU, 3U, 1U, 100U},
  };
  if (hasTables) {
    entries.push_back({0x0040U, 7U, 70U, tag40Offset});
    entries.push_back({0x0041U, 7U, 36U, tag41Offset});
  }
  const std::array<TiffEntry, 9> tailEntries{{
      {0x0042U, 3U, 1U, stripeCount},
      {0x0044U, 7U, 50U, tag44Offset},
      {0x0045U, 7U, 50U, tag45Offset},
      {0x0046U, 7U, 50U, tag46Offset},
      {0x0047U, 7U, 26U, tag47Offset},
      {0x0048U, 7U, 26U, tag48Offset},
      {0x010fU, 2U, 10U, makeOffset},
      {0x0110U, 2U, 7U, modelOffset},
      {0x0118U, 4U, 1U, stripeOffset},
  }};
  entries.insert(entries.end(), tailEntries.begin(), tailEntries.end());
  if (entries.size() != entryCount) {
    throw std::logic_error("synthetic Panasonic C8 IFD count changed");
  }

  std::vector<std::uint8_t> bytes(
      stripeOffset + firstStripeBytes +
          (twoStripes ? secondStripeBytes : 0U),
      0U);
  bytes.at(0U) = 'I';
  bytes.at(1U) = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, static_cast<std::uint16_t>(entryCount));
  std::size_t cursor = kIfdOffset + 2U;
  for (const TiffEntry& entry : entries) {
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);
  put32(bytes, signatureOffset, 1U);

  if (hasTables) {
    const std::uint16_t tag40Count =
        fixture == PanasonicC8Fixture::kTag40Count16
            ? 16U
            : (fixture == PanasonicC8Fixture::kTag40Count18 ? 18U : 17U);
    const std::uint16_t tag41Count =
        fixture == PanasonicC8Fixture::kTag41Count16
            ? 16U
            : (fixture == PanasonicC8Fixture::kTag41Count18 ? 18U : 17U);
    put16(bytes, tag40Offset, tag40Count);
    put16(bytes, tag41Offset, tag41Count);
    constexpr std::array<std::uint16_t, 17> kS5M2Lengths{{
        6U, 7U, 6U, 5U, 4U, 3U, 3U, 2U, 3U,
        3U, 4U, 5U, 6U, 8U, 8U, 12U, 12U,
    }};
    constexpr std::array<std::uint16_t, 17> kS5M2Codes{{
        62U, 126U, 61U, 28U, 12U, 4U, 2U, 0U, 3U,
        5U, 13U, 29U, 60U, 254U, 255U, 4094U, 4095U,
    }};
    for (std::size_t index = 0U; index < 17U; ++index) {
      std::uint16_t hlow = fixture == PanasonicC8Fixture::kValidShadowed
          ? kS5M2Lengths.at(index)
          : 5U;
      std::uint16_t code = fixture == PanasonicC8Fixture::kValidShadowed
          ? kS5M2Codes.at(index)
          : static_cast<std::uint16_t>(index);
      if (index == 0U) {
        if (fixture == PanasonicC8Fixture::kOverrangeTable) hlow = 65535U;
        if (fixture == PanasonicC8Fixture::kHlowZero) hlow = 0U;
        if (fixture == PanasonicC8Fixture::kHlow17) hlow = 17U;
        if (fixture == PanasonicC8Fixture::kCodeAtLimit) code = 32U;
        if (fixture == PanasonicC8Fixture::kCodeAboveStoredDomain) {
          hlow = 16U;
          code = 4096U;
        }
      }
      put16(bytes, tag40Offset + 2U + index * 4U, hlow);
      put16(bytes, tag40Offset + 4U + index * 4U, code);
      const std::uint16_t tag41 =
          fixture == PanasonicC8Fixture::kTag41Shift64
              ? 64U
              : (fixture == PanasonicC8Fixture::kSignedShift ? 1U : 0U);
      put16(bytes, tag41Offset + 2U + index * 2U, tag41);
    }
  }

  put16(bytes, tag44Offset,
        static_cast<std::uint16_t>(
            fixture == PanasonicC8Fixture::kStripeCountMismatch
                ? 1U
                : stripeCount));
  put32(bytes, tag44Offset + 2U, stripeOffset);
  if (twoStripes) put32(bytes, tag44Offset + 6U, secondStripeOffset);
  put16(bytes, tag45Offset, static_cast<std::uint16_t>(stripeCount));
  put32(bytes, tag45Offset + 2U,
        fixture == PanasonicC8Fixture::kValidTwoStripe ||
                fixture == PanasonicC8Fixture::kBorrowedBits ||
                fixture == PanasonicC8Fixture::kStripeCountMismatch
            ? 12U
            : (fixture == PanasonicC8Fixture::kWideStripeLeft
                   ? 0x10000U
                   : 0U));
  if (twoStripes) put32(bytes, tag45Offset + 6U, 0U);
  put16(bytes, tag46Offset, static_cast<std::uint16_t>(stripeCount));
  put32(bytes, tag46Offset + 2U,
        fixture == PanasonicC8Fixture::kBorrowedBits
            ? 1416U
            : firstStripeBytes * 8U);
  if (twoStripes) put32(bytes, tag46Offset + 6U, secondStripeBytes * 8U);
  put16(bytes, tag47Offset, static_cast<std::uint16_t>(stripeCount));
  put16(bytes, tag47Offset + 2U, twoStripes ? 12U : 24U);
  if (twoStripes) put16(bytes, tag47Offset + 4U, 12U);
  put16(bytes, tag48Offset, static_cast<std::uint16_t>(stripeCount));
  put16(bytes, tag48Offset + 2U, 24U);
  if (twoStripes) put16(bytes, tag48Offset + 4U, 24U);
  std::copy_n("Panasonic", 10U, bytes.begin() + makeOffset);
  std::copy_n("DC-GH6", 7U, bytes.begin() + modelOffset);
  if (fixture == PanasonicC8Fixture::kNegativeShift) {
    bytes.at(stripeOffset) = 0x10U;
  } else if (fixture == PanasonicC8Fixture::kSignedShift) {
    bytes.at(stripeOffset) = 0x30U;
  } else if (fixture == PanasonicC8Fixture::kNoMatch) {
    bytes.at(stripeOffset) = 0x1fU;
  } else if (fixture == PanasonicC8Fixture::kValidShadowed) {
    std::fill(bytes.begin() + stripeOffset, bytes.end(), 0xffU);
  }
  return bytes;
}

std::vector<std::uint8_t> makeTailSignatureX3f(
    const std::array<std::uint8_t, 8>& signature) {
  constexpr std::uint32_t kRawSectionOffset = 64U;
  constexpr std::uint32_t kTailSignatureOffset = 2040U;
  constexpr std::uint32_t kDirectoryOffset = 2064U;
  constexpr std::size_t kFileSize = kDirectoryOffset + 28U;
  std::vector<std::uint8_t> bytes(kFileSize, 0U);

  // X3F 4.0 header. Only the raw directory entry drives parse_x3f(); omitting
  // a property section selects the fixed 2048-byte model-probe path.
  put32(bytes, 0U, 0x62564f46U);  // FOVb
  put32(bytes, 4U, 0x00040000U);
  put32(bytes, 40U, 2944U);
  put32(bytes, 44U, 1888U);
  put32(bytes, 48U, 0U);

  put32(bytes, kRawSectionOffset, 0x69434553U);  // SECi
  put32(bytes, kRawSectionOffset + 4U, 0x00020000U);
  put32(bytes, kRawSectionOffset + 8U, 1U);
  put32(bytes, kRawSectionOffset + 12U, 0x25U);  // SD Quattro RAW
  put32(bytes, kRawSectionOffset + 16U, 2944U);
  put32(bytes, kRawSectionOffset + 20U, 1888U);
  put32(bytes, kRawSectionOffset + 24U, 2944U * 6U);

  std::copy(signature.begin(), signature.end(),
            bytes.begin() + kTailSignatureOffset);

  put32(bytes, kDirectoryOffset, 0x64434553U);  // SECd
  put32(bytes, kDirectoryOffset + 4U, 0x00020000U);
  put32(bytes, kDirectoryOffset + 8U, 1U);
  put32(bytes, kDirectoryOffset + 12U, kRawSectionOffset);
  put32(bytes, kDirectoryOffset + 16U, 28U);
  put32(bytes, kDirectoryOffset + 20U, 0x32414d49U);  // IMA2
  put32(bytes, kDirectoryOffset + 24U, kDirectoryOffset);
  return bytes;
}

}  // namespace

std::vector<std::uint8_t> makeOlympusTag641BoundaryTiff(
    std::uint16_t tagX641) {
  return makeOlympusTiff(OlympusFixture::kTag641, tagX641);
}

std::vector<std::uint8_t> makeOlympusUnaryWorkTiff() {
  return makeOlympusTiff(OlympusFixture::kUnaryWork, 512U);
}

std::vector<std::uint8_t> makeOlympusNegativePredictorTiff() {
  return makeOlympusTiff(OlympusFixture::kNegativePredictor, 0U);
}

std::vector<std::uint8_t> makeNonTerminatedMakerNoteTiff() {
  constexpr std::uint32_t kIfdOffset = 8U;
  constexpr std::uint32_t kExifOffset = 26U;
  constexpr std::uint32_t kMakerOffset = 44U;
  std::vector<std::uint8_t> bytes(64U, 0U);
  bytes.at(0U) = 'I';
  bytes.at(1U) = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, 1U);
  put16(bytes, kIfdOffset + 2U, 0x8769U);
  put16(bytes, kIfdOffset + 4U, 4U);
  put32(bytes, kIfdOffset + 6U, 1U);
  put32(bytes, kIfdOffset + 10U, kExifOffset);
  put32(bytes, kIfdOffset + 14U, 0U);
  put16(bytes, kExifOffset, 1U);
  put16(bytes, kExifOffset + 2U, 0x927cU);
  put16(bytes, kExifOffset + 4U, 7U);
  put32(bytes, kExifOffset + 6U, 10U);
  put32(bytes, kExifOffset + 10U, kMakerOffset);
  put32(bytes, kExifOffset + 14U, 0U);
  std::fill_n(bytes.begin() + kMakerOffset, 10U, 'A');
  return bytes;
}

std::vector<std::uint8_t> makeTailDpSignatureX3f() {
  constexpr std::array<std::uint8_t, 8> kSignature{
      'S', 'I', 'G', 'M', 'A', ' ', 'd', 'p'};
  return makeTailSignatureX3f(kSignature);
}

std::vector<std::uint8_t> makeTailSdSignatureX3f() {
  constexpr std::array<std::uint8_t, 8> kSignature{
      's', 'd', ' ', 'Q', 'u', 'a', 't', 't'};
  return makeTailSignatureX3f(kSignature);
}

std::vector<std::uint8_t> makePanasonicC8MissingTablesTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kMissingTables);
}

std::vector<std::uint8_t> makePanasonicC8Tag40CountTiff(
    std::uint16_t count) {
  if (count == 16U) {
    return makePanasonicC8Tiff(PanasonicC8Fixture::kTag40Count16);
  }
  if (count == 18U) {
    return makePanasonicC8Tiff(PanasonicC8Fixture::kTag40Count18);
  }
  throw std::invalid_argument("Panasonic tag40 count fixture expects 16 or 18");
}

std::vector<std::uint8_t> makePanasonicC8Tag41CountTiff(
    std::uint16_t count) {
  if (count == 16U) {
    return makePanasonicC8Tiff(PanasonicC8Fixture::kTag41Count16);
  }
  if (count == 18U) {
    return makePanasonicC8Tiff(PanasonicC8Fixture::kTag41Count18);
  }
  throw std::invalid_argument("Panasonic tag41 count fixture expects 16 or 18");
}

std::vector<std::uint8_t> makePanasonicC8StripeCountMismatchTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kStripeCountMismatch);
}

std::vector<std::uint8_t> makePanasonicC8BorrowedBitsTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kBorrowedBits);
}

std::vector<std::uint8_t> makePanasonicC8NegativeShiftTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kNegativeShift);
}

std::vector<std::uint8_t> makePanasonicC8SignedShiftTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kSignedShift);
}

std::vector<std::uint8_t> makePanasonicC8Tag41Shift64Tiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kTag41Shift64);
}

std::vector<std::uint8_t> makePanasonicC8NoMatchTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kNoMatch);
}

std::vector<std::uint8_t> makePanasonicC8OverrangeTableTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kOverrangeTable);
}

std::vector<std::uint8_t> makePanasonicC8HlowZeroTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kHlowZero);
}

std::vector<std::uint8_t> makePanasonicC8Hlow17Tiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kHlow17);
}

std::vector<std::uint8_t> makePanasonicC8CodeAtLimitTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kCodeAtLimit);
}

std::vector<std::uint8_t> makePanasonicC8CodeAboveStoredDomainTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kCodeAboveStoredDomain);
}

std::vector<std::uint8_t> makePanasonicC8WideStripeLeftTiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kWideStripeLeft);
}

std::vector<std::uint8_t> makeValidPanasonicC8Tiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kValid);
}

std::vector<std::uint8_t> makeValidTwoStripePanasonicC8Tiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kValidTwoStripe);
}

std::vector<std::uint8_t> makeValidShadowedPanasonicC8Tiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kValidShadowed);
}

std::vector<std::uint8_t> makeOverlappingPanasonicC8Tiff() {
  return makePanasonicC8Tiff(PanasonicC8Fixture::kOverlap);
}

namespace {

std::vector<std::uint8_t> makeVendorBlackShiftTiff(
    bool sony, std::uint16_t bitsPerSample) {
  const std::string make = sony ? std::string("Sony\0", 5U)
                                : std::string("Samsung\0", 8U);
  const std::string model = sony ? std::string("ILCE-1\0", 7U)
                                 : std::string("NX500\0", 6U);
  const std::uint32_t width = sony ? 4000U : 6496U;
  const std::uint32_t height = sony ? 32U : 24U;
  const std::uint16_t jpegWidth = sony ? 2000U : 6496U;
  const std::uint16_t jpegHeight = sony ? 16U : 24U;
  const std::uint8_t components = sony ? 4U : 1U;
  const std::size_t samples = static_cast<std::size_t>(jpegWidth) *
      jpegHeight * components;
  std::vector<std::uint8_t> jpeg = makeSonyLosslessJpegStrip(
      jpegWidth, jpegHeight, components, (samples + 7U) / 8U + 32U);
  jpeg.at(6U) = static_cast<std::uint8_t>(bitsPerSample);
  constexpr std::uint32_t kIfdOffset = 8U;
  constexpr std::uint32_t kEntryCount = 15U;
  constexpr std::uint32_t kIfdBytes = 2U + kEntryCount * 12U + 4U;
  const std::uint32_t makeOffset = kIfdOffset + kIfdBytes;
  const std::uint32_t modelOffset =
      makeOffset + static_cast<std::uint32_t>(make.size());
  const std::uint32_t stripOffset =
      modelOffset + static_cast<std::uint32_t>(model.size());
  const std::array<TiffEntry, kEntryCount> entries{{
      {0x00feU, 4U, 1U, 0U},
      {0x0100U, 4U, 1U, width},
      {0x0101U, 4U, 1U, height},
      {0x0102U, 3U, 1U, bitsPerSample},
      {0x0103U, 3U, 1U, sony ? 6U : 7U},
      {0x0106U, 3U, 1U, 32803U},
      {0x010fU, 2U, static_cast<std::uint32_t>(make.size()), makeOffset},
      {0x0110U, 2U, static_cast<std::uint32_t>(model.size()), modelOffset},
      {0x0111U, 4U, 1U, stripOffset},
      {0x0115U, 3U, 1U, 1U},
      {0x0116U, 4U, 1U, height},
      {0x0117U, 4U, 1U, static_cast<std::uint32_t>(jpeg.size())},
      {0x011cU, 3U, 1U, 1U},
      {0x828dU, 3U, 2U, 0x00020002U},
      {0x828eU, 1U, 4U, 0x02010100U},
  }};

  std::vector<std::uint8_t> bytes(stripOffset + jpeg.size(), 0U);
  bytes.at(0U) = 'I';
  bytes.at(1U) = 'I';
  put16(bytes, 2U, 42U);
  put32(bytes, 4U, kIfdOffset);
  put16(bytes, kIfdOffset, kEntryCount);
  std::size_t cursor = kIfdOffset + 2U;
  std::uint16_t previousTag = 0U;
  for (const TiffEntry& entry : entries) {
    if (entry.tag < previousTag) {
      throw std::logic_error("synthetic vendor TIFF IFD is not sorted");
    }
    previousTag = entry.tag;
    put16(bytes, cursor, entry.tag);
    put16(bytes, cursor + 2U, entry.type);
    put32(bytes, cursor + 4U, entry.count);
    put32(bytes, cursor + 8U, entry.value);
    cursor += 12U;
  }
  put32(bytes, cursor, 0U);
  std::copy(make.begin(), make.end(), bytes.begin() + makeOffset);
  std::copy(model.begin(), model.end(), bytes.begin() + modelOffset);
  std::copy(jpeg.begin(), jpeg.end(), bytes.begin() + stripOffset);
  return bytes;
}

}  // namespace

std::vector<std::uint8_t> makeSonyInvalidBlackShiftTiff() {
  return makeVendorBlackShiftTiff(true, 11U);
}

std::vector<std::uint8_t> makeSamsungInvalidBlackShiftTiff() {
  return makeVendorBlackShiftTiff(false, 6U);
}

std::vector<std::uint8_t> makeSonyBoundaryBlackShiftTiff() {
  return makeVendorBlackShiftTiff(true, 12U);
}

std::vector<std::uint8_t> makeSamsungBoundaryBlackShiftTiff() {
  return makeVendorBlackShiftTiff(false, 7U);
}

std::vector<std::uint8_t> makeOverflowingHasselbladPredictorTiff() {
  std::vector<std::uint8_t> bytes = makeHasselbladTiff(false, 16U);
  const std::uint32_t ifdOffset = get32(bytes, 4U);
  const std::uint16_t entryCount = get16(bytes, ifdOffset);
  bool changedWidth = false;
  bool changedSamples = false;
  bool changedByteCount = false;
  for (std::uint16_t index = 0U; index < entryCount; ++index) {
    const std::size_t entry = ifdOffset + 2U +
        static_cast<std::size_t>(index) * 12U;
    const std::uint16_t tag = get16(bytes, entry);
    if (tag == 0x0100U) {
      put32(bytes, entry + 8U, 16'386U);
      changedWidth = true;
    } else if (tag == 0x0115U) {
      put16(bytes, entry + 8U, 4U);
      changedSamples = true;
    } else if (tag == 0x0117U) {
      put32(bytes, entry + 8U, get32(bytes, entry + 8U) + 200'000U);
      changedByteCount = true;
    }
  }
  if (!changedWidth || !changedSamples || !changedByteCount ||
      bytes.size() < 2U || bytes[bytes.size() - 2U] != 0xffU ||
      bytes.back() != 0xd9U) {
    throw std::logic_error("synthetic Hasselblad TIFF layout changed");
  }
  bytes.insert(bytes.end() - 2, 200'000U, 0x00U);
  return bytes;
}

std::string describe(const DecodeOutcome& outcome) {
  std::ostringstream out;
  out << "stage=" << stageName(outcome.terminalStage)
      << " open=" << outcome.openCode << " unpack=" << outcome.unpackCode
      << " process=" << outcome.processCode << " iwidth="
      << outcome.processingWidth << " iheight=" << outcome.processingHeight;
  return out.str();
}

}  // namespace sfraw::hosttest
