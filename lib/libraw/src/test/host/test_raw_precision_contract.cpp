/*
 * Spektrafilm Android -- declared RAW precision/normalization contract.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "public_api_test_support.h"
#include "raw_decoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace sfraw::test {
using LibRawOpenAttemptObserver = void (*)(void*);
void setLibRawOpenAttemptObserverForTest(
    LibRawOpenAttemptObserver observer, void* context) noexcept;
}  // namespace sfraw::test

namespace {

static_assert(spectrafilm::RawPrecisionDescriptor::kJniWordCount == 136U);
static_assert(spectrafilm::RawPrecisionDescriptor::kJniRealCount == 2U);

int failures = 0;

void check(bool condition, const std::string& label,
           const std::string& detail = {}) {
  std::cout << (condition ? "ok   " : "FAIL ") << label;
  if (!detail.empty()) std::cout << " (" << detail << ')';
  std::cout << '\n';
  if (!condition) ++failures;
}

std::string describe(const spectrafilm::DecodeResult& result) {
  return "status=" + std::to_string(result.status) +
      " libraw=" + std::to_string(result.librawCode) +
      " error=" + result.error;
}

std::uint64_t exactDigest(const spectrafilm::DecodeResult& result) {
  constexpr std::uint64_t kOffset = 1469598103934665603ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t digest = kOffset;
  for (std::size_t index = 0U; index < result.rgb.size(); ++index) {
    std::uint32_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(result.rgb[index]));
    std::memcpy(&bits, &result.rgb[index], sizeof(bits));
    for (unsigned byte = 0U; byte < 4U; ++byte) {
      digest ^= (bits >> (byte * 8U)) & 0xffU;
      digest *= kPrime;
    }
  }
  return digest;
}

spectrafilm::DecodeResult decode(
    const sfraw::hosttest::PrecisionDngOptions& options,
    const std::vector<std::uint16_t>* samples = nullptr) {
  sfraw::hosttest::PrecisionDngOptions concrete = options;
  if (samples != nullptr) concrete.samples = *samples;
  const std::vector<std::uint8_t> dng =
      sfraw::hosttest::makePrecisionUncompressedDng(concrete);
  spectrafilm::DecodeOptions decodeOptions;
  decodeOptions.whiteBalance = spectrafilm::WhiteBalanceMode::Daylight;
  return spectrafilm::decodeFromBuffer(dng.data(), dng.size(), decodeOptions);
}

std::uint32_t effectiveBlack(
    const spectrafilm::RawPrecisionDescriptor& descriptor,
    std::size_t channel, std::size_t cell) {
  const std::uint32_t repeat = descriptor.blackPatternCount == 0U
      ? 0U
      : descriptor.blackPattern[cell % descriptor.blackPatternCount];
  return descriptor.blackLevelCommon +
      descriptor.blackLevelChannels.at(channel) + repeat;
}

float sampleAt(const spectrafilm::DecodeResult& result, int x, int y,
               int channel) {
  const std::size_t index =
      (static_cast<std::size_t>(y) * result.width + x) * 3U + channel;
  return result.rgb[index];
}

std::uint16_t little16(const std::vector<std::uint8_t>& bytes,
                       std::size_t at) {
  return static_cast<std::uint16_t>(bytes.at(at)) |
      static_cast<std::uint16_t>(bytes.at(at + 1U) << 8U);
}

void little16(std::vector<std::uint8_t>* bytes, std::size_t at,
              std::uint16_t value) {
  bytes->at(at) = static_cast<std::uint8_t>(value & 0xffU);
  bytes->at(at + 1U) = static_cast<std::uint8_t>(value >> 8U);
}

bool isBigEndian(const std::vector<std::uint8_t>& bytes) {
  return bytes.size() >= 2U && bytes[0] == 'M' && bytes[1] == 'M';
}

std::uint16_t ordered16(const std::vector<std::uint8_t>& bytes,
                        std::size_t at) {
  return isBigEndian(bytes)
      ? static_cast<std::uint16_t>((bytes.at(at) << 8U) | bytes.at(at + 1U))
      : little16(bytes, at);
}

std::uint32_t ordered32(const std::vector<std::uint8_t>& bytes,
                        std::size_t at) {
  if (isBigEndian(bytes)) {
    return (static_cast<std::uint32_t>(bytes.at(at)) << 24U) |
        (static_cast<std::uint32_t>(bytes.at(at + 1U)) << 16U) |
        (static_cast<std::uint32_t>(bytes.at(at + 2U)) << 8U) |
        bytes.at(at + 3U);
  }
  return static_cast<std::uint32_t>(bytes.at(at)) |
      (static_cast<std::uint32_t>(bytes.at(at + 1U)) << 8U) |
      (static_cast<std::uint32_t>(bytes.at(at + 2U)) << 16U) |
      (static_cast<std::uint32_t>(bytes.at(at + 3U)) << 24U);
}

void ordered16(std::vector<std::uint8_t>* bytes, std::size_t at,
               std::uint16_t value) {
  if (isBigEndian(*bytes)) {
    bytes->at(at) = static_cast<std::uint8_t>(value >> 8U);
    bytes->at(at + 1U) = static_cast<std::uint8_t>(value & 0xffU);
  } else {
    little16(bytes, at, value);
  }
}

void ordered32(std::vector<std::uint8_t>* bytes, std::size_t at,
               std::uint32_t value) {
  if (isBigEndian(*bytes)) {
    bytes->at(at) = static_cast<std::uint8_t>(value >> 24U);
    bytes->at(at + 1U) = static_cast<std::uint8_t>(value >> 16U);
    bytes->at(at + 2U) = static_cast<std::uint8_t>(value >> 8U);
    bytes->at(at + 3U) = static_cast<std::uint8_t>(value & 0xffU);
  } else {
    bytes->at(at) = static_cast<std::uint8_t>(value & 0xffU);
    bytes->at(at + 1U) = static_cast<std::uint8_t>(value >> 8U);
    bytes->at(at + 2U) = static_cast<std::uint8_t>(value >> 16U);
    bytes->at(at + 3U) = static_cast<std::uint8_t>(value >> 24U);
  }
}

std::size_t findIfdEntryAt(const std::vector<std::uint8_t>& bytes,
                           std::size_t ifd, std::uint16_t tag) {
  const std::uint16_t count = ordered16(bytes, ifd);
  for (std::uint16_t index = 0U; index < count; ++index) {
    const std::size_t entry = ifd + 2U + index * 12U;
    if (ordered16(bytes, entry) == tag) return entry;
  }
  throw std::logic_error("TIFF fixture tag is absent");
}

std::size_t findIfdEntry(const std::vector<std::uint8_t>& bytes,
                         std::uint16_t tag) {
  return findIfdEntryAt(bytes, ordered32(bytes, 4U), tag);
}

std::size_t shortArrayAt(const std::vector<std::uint8_t>& bytes,
                         std::size_t entry) {
  const std::uint32_t count = ordered32(bytes, entry + 4U);
  if (ordered16(bytes, entry + 2U) != 3U || count == 0U) {
    throw std::logic_error("TIFF fixture tag is not a SHORT array");
  }
  return count * 2U <= 4U ? entry + 8U : ordered32(bytes, entry + 8U);
}

std::size_t entryPayloadAt(const std::vector<std::uint8_t>& bytes,
                           std::size_t entry,
                           std::size_t elementBytes) {
  const std::uint32_t count = ordered32(bytes, entry + 4U);
  if (count == 0U || elementBytes == 0U ||
      count > std::numeric_limits<std::size_t>::max() / elementBytes) {
    throw std::logic_error("TIFF fixture payload shape is invalid");
  }
  const std::size_t payloadBytes =
      static_cast<std::size_t>(count) * elementBytes;
  const std::size_t at = payloadBytes <= 4U
      ? entry + 8U
      : ordered32(bytes, entry + 8U);
  if (at > bytes.size() || payloadBytes > bytes.size() - at) {
    throw std::logic_error("TIFF fixture payload is out of bounds");
  }
  return at;
}

void countLibRawOpenAttempt(void* context) {
  static_cast<std::atomic<unsigned>*>(context)->fetch_add(
      1U, std::memory_order_relaxed);
}

enum class ChildLink {
  kNextIfd,
  kSubIfd,
};

struct ChildFixture {
  std::vector<std::uint8_t> bytes;
  std::size_t rootIfd = 0U;
  std::size_t childIfd = 0U;
};

ChildFixture appendClonedChild(sfraw::hosttest::TiffByteOrder byteOrder,
                               ChildLink link) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  ChildFixture fixture;
  fixture.bytes = sfraw::hosttest::makePrecisionUncompressedDng(options);
  fixture.rootIfd = ordered32(fixture.bytes, 4U);
  const std::size_t ifdBytes = 2U +
      static_cast<std::size_t>(ordered16(fixture.bytes, fixture.rootIfd)) *
          12U +
      4U;
  fixture.childIfd = fixture.bytes.size();
  const std::vector<std::uint8_t> clone(
      fixture.bytes.begin() + fixture.rootIfd,
      fixture.bytes.begin() + fixture.rootIfd + ifdBytes);
  fixture.bytes.insert(fixture.bytes.end(), clone.begin(), clone.end());
  if (fixture.childIfd > std::numeric_limits<std::uint32_t>::max()) {
    throw std::logic_error("child IFD fixture offset overflows TIFF LONG");
  }
  if (link == ChildLink::kNextIfd) {
    ordered32(&fixture.bytes, fixture.rootIfd + ifdBytes - 4U,
              static_cast<std::uint32_t>(fixture.childIfd));
  } else {
    // Reuse root DNGBackwardVersion's inline four bytes as one LONG SubIFD
    // pointer. The already-cloned child retains the original backward version.
    const std::size_t subIfdEntry =
        findIfdEntryAt(fixture.bytes, fixture.rootIfd, 0xc613U);
    ordered16(&fixture.bytes, subIfdEntry, 0x014aU);
    ordered16(&fixture.bytes, subIfdEntry + 2U, 4U);
    ordered32(&fixture.bytes, subIfdEntry + 4U, 1U);
    ordered32(&fixture.bytes, subIfdEntry + 8U,
              static_cast<std::uint32_t>(fixture.childIfd));
  }
  return fixture;
}

void makeIfdReduced(std::vector<std::uint8_t>* bytes, std::size_t ifd) {
  const std::size_t entry = findIfdEntryAt(*bytes, ifd, 0x00feU);
  ordered32(bytes, entry + 8U, 1U);
}

void replaceDngVersionWithSoftware(std::vector<std::uint8_t>* bytes,
                                   std::size_t ifd) {
  const std::size_t entry = findIfdEntryAt(*bytes, ifd, 0xc612U);
  ordered16(bytes, entry, 0x0131U);
  ordered16(bytes, entry + 2U, 2U);
  ordered32(bytes, entry + 4U, 4U);
}

std::vector<std::uint8_t> makeReducedDeltaChild(
    sfraw::hosttest::TiffByteOrder byteOrder, ChildLink link,
    bool malformedEligibility, bool malformedDelta) {
  ChildFixture fixture = appendClonedChild(byteOrder, link);
  makeIfdReduced(&fixture.bytes, fixture.childIfd);
  replaceDngVersionWithSoftware(&fixture.bytes, fixture.childIfd);
  const std::size_t delta =
      findIfdEntryAt(fixture.bytes, fixture.childIfd, 0xc62aU);
  ordered16(&fixture.bytes, delta, 0xc61bU);
  if (malformedDelta) {
    ordered16(&fixture.bytes, delta + 2U, 2U);  // ASCII, not SRATIONAL.
  }
  if (malformedEligibility) {
    const std::size_t width =
        findIfdEntryAt(fixture.bytes, fixture.childIfd, 0x0100U);
    ordered16(&fixture.bytes, width + 2U, 2U);  // Invalid scalar type.
  }
  return fixture.bytes;
}

std::vector<std::uint8_t> makeNonRootDngVersion(
    sfraw::hosttest::TiffByteOrder byteOrder, ChildLink link,
    bool malformedChildVersion) {
  ChildFixture fixture = appendClonedChild(byteOrder, link);
  // Root remains a recognized ordinary TIFF, while the reduced child alone
  // declares DNGVersion. This proves dependency safety is not gated by root
  // semantic DNG identity or by selected-image candidacy.
  replaceDngVersionWithSoftware(&fixture.bytes, fixture.rootIfd);
  makeIfdReduced(&fixture.bytes, fixture.childIfd);
  if (malformedChildVersion) {
    const std::size_t version =
        findIfdEntryAt(fixture.bytes, fixture.childIfd, 0xc612U);
    ordered16(&fixture.bytes, version + 2U, 3U);  // SHORT, not BYTE.
  }
  return fixture.bytes;
}

std::vector<std::uint8_t> makeNegativeBlackChild(
    sfraw::hosttest::TiffByteOrder byteOrder, ChildLink link, bool reduced,
    bool malformedEligibility) {
  ChildFixture fixture = appendClonedChild(byteOrder, link);
  replaceDngVersionWithSoftware(&fixture.bytes, fixture.childIfd);
  if (reduced) makeIfdReduced(&fixture.bytes, fixture.childIfd);
  if (malformedEligibility) {
    const std::size_t width =
        findIfdEntryAt(fixture.bytes, fixture.childIfd, 0x0100U);
    ordered16(&fixture.bytes, width + 2U, 2U);
  }
  const std::size_t black =
      findIfdEntryAt(fixture.bytes, fixture.childIfd, 0xc61aU);
  ordered16(&fixture.bytes, black + 2U, 10U);  // SRATIONAL is invalid here.
  ordered32(&fixture.bytes, black + 4U, 1U);
  const std::uint32_t payload =
      static_cast<std::uint32_t>(fixture.bytes.size());
  fixture.bytes.resize(fixture.bytes.size() + 8U, 0U);
  ordered32(&fixture.bytes, black + 8U, payload);
  ordered32(&fixture.bytes, payload, 0xffffffffU);  // -1
  ordered32(&fixture.bytes, payload + 4U, 1U);
  return fixture.bytes;
}

std::size_t ifdByteSize(const std::vector<std::uint8_t>& bytes,
                        std::size_t ifd) {
  return 2U + static_cast<std::size_t>(ordered16(bytes, ifd)) * 12U + 4U;
}

std::size_t appendIfdClone(std::vector<std::uint8_t>* bytes,
                           std::size_t sourceIfd) {
  const std::size_t byteCount = ifdByteSize(*bytes, sourceIfd);
  const std::size_t cloneAt = bytes->size();
  const std::vector<std::uint8_t> clone(
      bytes->begin() + sourceIfd, bytes->begin() + sourceIfd + byteCount);
  bytes->insert(bytes->end(), clone.begin(), clone.end());
  return cloneAt;
}

void replaceEntryWithSubIfd(std::vector<std::uint8_t>* bytes,
                            std::size_t ifd, std::uint16_t replacedTag,
                            std::uint32_t targetIfd) {
  const std::size_t entry = findIfdEntryAt(*bytes, ifd, replacedTag);
  ordered16(bytes, entry, 0x014aU);
  ordered16(bytes, entry + 2U, 4U);
  ordered32(bytes, entry + 4U, 1U);
  ordered32(bytes, entry + 8U, targetIfd);
}

void replaceEntryWithBlackDelta(std::vector<std::uint8_t>* bytes,
                                std::size_t ifd) {
  const std::size_t entry = findIfdEntryAt(*bytes, ifd, 0xc62aU);
  ordered16(bytes, entry, 0xc61bU);
  ordered16(bytes, entry + 2U, 10U);
  ordered32(bytes, entry + 4U, 1U);
  const std::uint32_t payload = static_cast<std::uint32_t>(bytes->size());
  bytes->resize(bytes->size() + 8U, 0U);
  ordered32(bytes, entry + 8U, payload);
  ordered32(bytes, payload, 0U);
  ordered32(bytes, payload + 4U, 1U);
}

std::vector<std::uint8_t> makeBackwardNextDelta(
    sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  std::vector<std::uint8_t> bytes =
      sfraw::hosttest::makePrecisionUncompressedDng(options);
  const std::size_t childIfd = ordered32(bytes, 4U);
  const std::size_t rootIfd = appendIfdClone(&bytes, childIfd);
  ordered32(&bytes, 4U, static_cast<std::uint32_t>(rootIfd));
  ordered32(&bytes, rootIfd + ifdByteSize(bytes, rootIfd) - 4U,
            static_cast<std::uint32_t>(childIfd));
  replaceDngVersionWithSoftware(&bytes, rootIfd);
  replaceDngVersionWithSoftware(&bytes, childIfd);
  makeIfdReduced(&bytes, childIfd);
  replaceEntryWithBlackDelta(&bytes, childIfd);
  return bytes;
}

std::vector<std::uint8_t> makeDuplicateSubIfdWithHiddenDelta(
    sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  std::vector<std::uint8_t> bytes =
      sfraw::hosttest::makePrecisionUncompressedDng(options);
  const std::size_t rootIfd = ordered32(bytes, 4U);
  const std::size_t firstChild = appendIfdClone(&bytes, rootIfd);
  const std::size_t hiddenChild = appendIfdClone(&bytes, rootIfd);
  replaceEntryWithSubIfd(
      &bytes, rootIfd, 0xc612U, static_cast<std::uint32_t>(firstChild));
  replaceEntryWithSubIfd(
      &bytes, rootIfd, 0xc613U, static_cast<std::uint32_t>(hiddenChild));
  replaceDngVersionWithSoftware(&bytes, firstChild);
  replaceDngVersionWithSoftware(&bytes, hiddenChild);
  makeIfdReduced(&bytes, firstChild);
  makeIfdReduced(&bytes, hiddenChild);
  replaceEntryWithBlackDelta(&bytes, hiddenChild);
  return bytes;
}

std::vector<std::uint8_t> makeSeventeenSubIfdsWithEarlyDelta(
    sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  std::vector<std::uint8_t> bytes =
      sfraw::hosttest::makePrecisionUncompressedDng(options);
  const std::size_t rootIfd = ordered32(bytes, 4U);
  const std::size_t hostileChild = appendIfdClone(&bytes, rootIfd);
  replaceDngVersionWithSoftware(&bytes, hostileChild);
  makeIfdReduced(&bytes, hostileChild);
  replaceEntryWithBlackDelta(&bytes, hostileChild);

  const std::uint32_t offsetsAt = static_cast<std::uint32_t>(bytes.size());
  bytes.resize(bytes.size() + 17U * 4U, 0U);
  for (std::size_t index = 0U; index < 17U; ++index) {
    ordered32(&bytes, offsetsAt + index * 4U,
              static_cast<std::uint32_t>(hostileChild));
  }
  const std::size_t subIfd = findIfdEntryAt(bytes, rootIfd, 0xc612U);
  ordered16(&bytes, subIfd, 0x014aU);
  ordered16(&bytes, subIfd + 2U, 4U);
  ordered32(&bytes, subIfd + 4U, 17U);
  ordered32(&bytes, subIfd + 8U, offsetsAt);
  return bytes;
}

std::vector<std::uint8_t> makeDepthFiveSubIfdDelta(
    sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  std::vector<std::uint8_t> bytes =
      sfraw::hosttest::makePrecisionUncompressedDng(options);
  std::array<std::size_t, 6U> ifds{};
  ifds[0] = ordered32(bytes, 4U);
  for (std::size_t depth = 1U; depth < ifds.size(); ++depth) {
    ifds[depth] = appendIfdClone(&bytes, ifds[0]);
  }
  for (std::size_t depth = 0U; depth + 1U < ifds.size(); ++depth) {
    replaceEntryWithSubIfd(
        &bytes, ifds[depth], 0xc612U,
        static_cast<std::uint32_t>(ifds[depth + 1U]));
  }
  replaceDngVersionWithSoftware(&bytes, ifds.back());
  for (std::size_t depth = 1U; depth < ifds.size(); ++depth) {
    makeIfdReduced(&bytes, ifds[depth]);
  }
  replaceEntryWithBlackDelta(&bytes, ifds.back());
  return bytes;
}

std::vector<std::uint8_t> makeSubIfdCycle(
    sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  std::vector<std::uint8_t> bytes =
      sfraw::hosttest::makePrecisionUncompressedDng(options);
  const std::size_t rootIfd = ordered32(bytes, 4U);
  const std::size_t childIfd = appendIfdClone(&bytes, rootIfd);
  replaceEntryWithSubIfd(
      &bytes, rootIfd, 0xc612U, static_cast<std::uint32_t>(childIfd));
  replaceEntryWithSubIfd(
      &bytes, childIfd, 0xc612U, static_cast<std::uint32_t>(rootIfd));
  return bytes;
}

std::vector<std::uint8_t> makeWrongMagicEndianHeader(
    sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.byteOrder = byteOrder;
  std::vector<std::uint8_t> bytes =
      sfraw::hosttest::makePrecisionUncompressedDng(options);
  replaceDngVersionWithSoftware(&bytes, ordered32(bytes, 4U));
  ordered16(&bytes, 2U, 0x1234U);
  return bytes;
}

spectrafilm::DecodeResult decodeFromTemporaryFd(
    const std::vector<std::uint8_t>& bytes) {
  FILE* file = std::tmpfile();
  if (file == nullptr) throw std::runtime_error("tmpfile failed");
  const std::size_t written =
      std::fwrite(bytes.data(), 1U, bytes.size(), file);
  if (written != bytes.size() || std::fflush(file) != 0 ||
      std::fseek(file, 0L, SEEK_SET) != 0) {
    std::fclose(file);
    throw std::runtime_error("temporary RAW fixture write failed");
  }
  spectrafilm::DecodeResult result =
      spectrafilm::decodeFromFd(::fileno(file), {});
  std::fclose(file);
  return result;
}

void checkHostilePreOpenMatrix(const std::vector<std::uint8_t>& bytes,
                               const std::string& label) {
  for (const bool throughFd : {false, true}) {
    std::atomic<unsigned> openAttempts{0U};
    sfraw::test::setLibRawOpenAttemptObserverForTest(
        countLibRawOpenAttempt, &openAttempts);
    const spectrafilm::DecodeResult result = throughFd
        ? decodeFromTemporaryFd(bytes)
        : spectrafilm::decodeFromBuffer(bytes.data(), bytes.size(), {});
    sfraw::test::setLibRawOpenAttemptObserverForTest(nullptr, nullptr);
    check(!result.ok &&
              result.status == spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              result.error.find("before LibRaw open") != std::string::npos &&
              openAttempts.load(std::memory_order_relaxed) == 0U,
          label + (throughFd ? "/fd" : "/buffer") +
              " is typed and rejects before dependency open",
          describe(result));
  }
}

void checkPrecisionCase(std::uint16_t bits,
                        sfraw::hosttest::TiffByteOrder byteOrder) {
  sfraw::hosttest::PrecisionDngOptions options;
  options.bitsPerSample = bits;
  options.includeSampleFormat = true;
  options.byteOrder = byteOrder;
  const std::uint32_t codeMaximum = bits == 16U
      ? 65535U
      : ((1U << bits) - 1U);
  options.blackLevel = std::max<std::uint32_t>(1U, codeMaximum / 16U);
  options.whiteLevel = codeMaximum - std::max<std::uint32_t>(1U, codeMaximum / 32U);

  const std::size_t pixelCount =
      static_cast<std::size_t>(options.width) * options.height;
  const std::vector<std::uint16_t> blackSamples(
      pixelCount, static_cast<std::uint16_t>(options.blackLevel));
  const std::vector<std::uint16_t> whiteSamples(
      pixelCount, static_cast<std::uint16_t>(options.whiteLevel));
  spectrafilm::DecodeResult black = decode(options, &blackSamples);
  spectrafilm::DecodeResult white = decode(options, &whiteSamples);
  spectrafilm::DecodeResult bands = decode(options);
  spectrafilm::DecodeResult repeat = decode(options);
  const std::uint16_t adjacentCode = static_cast<std::uint16_t>(
      options.blackLevel + (options.whiteLevel - options.blackLevel) / 2U);
  const std::vector<std::uint16_t> adjacentLow(pixelCount, adjacentCode);
  const std::vector<std::uint16_t> adjacentHigh(
      pixelCount, static_cast<std::uint16_t>(adjacentCode + 1U));
  spectrafilm::DecodeResult low = decode(options, &adjacentLow);
  spectrafilm::DecodeResult high = decode(options, &adjacentHigh);

  const std::string prefix = std::to_string(bits) + "-bit/" +
      (byteOrder == sfraw::hosttest::TiffByteOrder::kBigEndian ? "BE" : "LE");
  check(black.ok && white.ok && bands.ok && repeat.ok && low.ok && high.ok,
        prefix + " fixtures decode through the production wrapper",
        describe(bands));
  if (!black.ok || !white.ok || !bands.ok || !repeat.ok || !low.ok || !high.ok) {
    return;
  }

  const auto& descriptor = bands.descriptor;
  check(descriptor.version == spectrafilm::RawPrecisionDescriptor::kVersion &&
            descriptor.sampleFormat ==
                spectrafilm::RawSampleFormat::UnsignedInteger &&
            descriptor.declaredBitsPerSample == bits &&
            descriptor.effectiveBitsPerSample == bits &&
            descriptor.processedBitsPerSample == 16,
        prefix + " preserves declared/effective/processed precision separately");
  check(descriptor.byteOrder ==
            (byteOrder == sfraw::hosttest::TiffByteOrder::kBigEndian
                 ? spectrafilm::RawByteOrder::BigEndian
                 : spectrafilm::RawByteOrder::LittleEndian) &&
            descriptor.packing ==
                (bits == 16U ? spectrafilm::RawPacking::TiffWord16
                             : spectrafilm::RawPacking::TiffPackedBits),
        prefix + " records container byte order and packing");
  check(descriptor.pixelLayout == spectrafilm::RawPixelLayout::Bayer2x2 &&
            descriptor.cfaPatternRows == 2 &&
            descriptor.cfaPatternColumns == 2 &&
            descriptor.cfaPatternCount == 4U &&
            descriptor.colorChannels == 3,
        prefix + " records the bounded Bayer layout");
  check(descriptor.decoderRoute == spectrafilm::RawDecoderRoute::LibRawNative &&
            descriptor.postprocessRoute ==
                spectrafilm::RawPostprocessRoute::LibRawAcesToFloat32ProPhoto &&
            descriptor.linearSpace ==
                spectrafilm::RawLinearSpace::LinearProPhotoRgb &&
            !descriptor.halfSizeRequested &&
            descriptor.requestedMaxLongEdge == 0 &&
            descriptor.outputSubsampleStep == 1,
        prefix + " records the native route, f32 boundary, and non-proxy state");
  check(descriptor.whiteLevelProvenance ==
            spectrafilm::RawLevelProvenance::DngMetadata &&
            descriptor.blackLevelProvenance ==
                spectrafilm::RawLevelProvenance::DngMetadata &&
            std::all_of(descriptor.whiteLevels.begin(),
                        descriptor.whiteLevels.end(),
                        [&](std::uint32_t level) {
                          return level == options.whiteLevel;
                        }) &&
            descriptor.containerCompression == 1,
        prefix + " preserves effective WhiteLevel and provenance");
  bool blackMatches = true;
  const std::size_t cells = descriptor.blackPatternCount == 0U
      ? 1U
      : descriptor.blackPatternCount;
  for (std::size_t channel = 0U; channel < 4U; ++channel) {
    for (std::size_t cell = 0U; cell < cells; ++cell) {
      blackMatches = blackMatches &&
          effectiveBlack(descriptor, channel, cell) == options.blackLevel;
    }
  }
  check(blackMatches, prefix + " preserves effective BlackLevel semantics");
  check(descriptor.baselineExposurePresent &&
            std::fabs(descriptor.baselineExposure + 0.5f) < 1.0e-6f &&
            descriptor.linearResponseLimitPresent &&
            std::fabs(descriptor.linearResponseLimit - 0.75f) < 1.0e-6f,
        prefix + " distinguishes present HDR-relevant DNG metadata");

  check(exactDigest(bands) == exactDigest(repeat),
        prefix + " repeated decode has an exact float32 digest");
  const int adjacentX = low.width / 2;
  const int adjacentY = low.height / 2;
  const float lowLuminance =
      0.2880402f * sampleAt(low, adjacentX, adjacentY, 0) +
      0.7118741f * sampleAt(low, adjacentX, adjacentY, 1) +
      0.0000857f * sampleAt(low, adjacentX, adjacentY, 2);
  const float highLuminance =
      0.2880402f * sampleAt(high, adjacentX, adjacentY, 0) +
      0.7118741f * sampleAt(high, adjacentX, adjacentY, 1) +
      0.0000857f * sampleAt(high, adjacentX, adjacentY, 2);
  check(exactDigest(low) != exactDigest(high) && highLuminance > lowLuminance,
        prefix + " adjacent source codes remain distinguishable and ordered");

  const int y = bands.height / 2;
  for (int channel = 0; channel < 3; ++channel) {
    const float blackValue = sampleAt(black, black.width / 2,
                                      black.height / 2, channel);
    const float whiteValue = sampleAt(white, white.width / 2,
                                      white.height / 2, channel);
    const float denominator = whiteValue - blackValue;
    bool monotonic = denominator > 1.0e-6f;
    float previous = -std::numeric_limits<float>::infinity();
    for (int band = 0; band < 5 && monotonic; ++band) {
      const int x = (2 * band + 1) * bands.width / 10;
      const float value = sampleAt(bands, x, y, channel);
      monotonic = std::isfinite(value) && value + 2.0e-4f >= previous;
      previous = value;
      const std::uint32_t highestBlack = options.blackLevel;
      const std::uint32_t span = options.whiteLevel - highestBlack;
      const std::uint32_t code = band == 0
          ? highestBlack
          : (band == 1 ? highestBlack + 1U
             : (band == 2 ? highestBlack + span / 2U
                : (band == 3 ? options.whiteLevel - 1U
                              : options.whiteLevel)));
      const float expected = static_cast<float>(code - highestBlack) /
          static_cast<float>(span);
      const float observed = (value - blackValue) / denominator;
      monotonic = monotonic && std::fabs(observed - expected) < 0.015f;
    }
    check(monotonic,
          prefix + " channel " + std::to_string(channel) +
              " is monotonic and follows (code-black)/(white-black)");
  }
}

}  // namespace

int main() {
  for (const std::uint16_t bits : {8U, 10U, 12U, 14U, 16U}) {
    checkPrecisionCase(bits, sfraw::hosttest::TiffByteOrder::kLittleEndian);
    checkPrecisionCase(bits, sfraw::hosttest::TiffByteOrder::kBigEndian);
  }

  for (const auto byteOrder : {
           sfraw::hosttest::TiffByteOrder::kLittleEndian,
           sfraw::hosttest::TiffByteOrder::kBigEndian,
       }) {
    const std::string endian = byteOrder ==
            sfraw::hosttest::TiffByteOrder::kBigEndian
        ? "BE"
        : "LE";
    for (const std::uint16_t sampleCount : {3U, 4U}) {
      sfraw::hosttest::PrecisionDngOptions linear;
      linear.linearRaw = true;
      linear.samplesPerPixel = sampleCount;
      linear.includeSampleFormat = true;
      linear.byteOrder = byteOrder;
      for (std::uint16_t channel = 0U; channel < sampleCount; ++channel) {
        linear.blackPattern.push_back(32U + channel * 17U);
        linear.whiteLevels.push_back(3600U + channel * 100U);
      }
      const std::vector<std::uint8_t> linearDng =
          sfraw::hosttest::makePrecisionUncompressedDng(linear);
      const std::size_t orientationEntry = findIfdEntry(linearDng, 0x0112U);
      const std::size_t modelEntry = findIfdEntry(linearDng, 0xc614U);
      const std::size_t matrixEntry = findIfdEntry(linearDng, 0xc621U);
      const std::size_t matrix2Entry = findIfdEntry(linearDng, 0xc622U);
      const std::size_t illuminant1Entry = findIfdEntry(linearDng, 0xc65aU);
      const std::size_t illuminant2Entry = findIfdEntry(linearDng, 0xc65bU);
      const std::size_t modelAt = entryPayloadAt(linearDng, modelEntry, 1U);
      const std::size_t matrixAt = entryPayloadAt(linearDng, matrixEntry, 8U);
      const std::size_t matrix2At = entryPayloadAt(linearDng, matrix2Entry, 8U);
      const std::uint32_t modelCount = ordered32(linearDng, modelEntry + 4U);
      bool matrixDenominatorsValid = true;
      for (std::size_t element = 0U;
           element < static_cast<std::size_t>(sampleCount) * 3U; ++element) {
        matrixDenominatorsValid = matrixDenominatorsValid &&
            ordered32(linearDng, matrixAt + element * 8U + 4U) != 0U &&
            ordered32(linearDng, matrix2At + element * 8U + 4U) != 0U;
      }
      check(ordered16(linearDng, orientationEntry + 2U) == 3U &&
                ordered32(linearDng, orientationEntry + 4U) == 1U &&
                ordered16(linearDng, orientationEntry + 8U) == 1U &&
                ordered16(linearDng, modelEntry + 2U) == 2U &&
                modelCount > 1U &&
                linearDng.at(modelAt + modelCount - 1U) == 0U &&
                ordered16(linearDng, matrixEntry + 2U) == 10U &&
                ordered32(linearDng, matrixEntry + 4U) ==
                    static_cast<std::uint32_t>(sampleCount) * 3U &&
                ordered16(linearDng, matrix2Entry + 2U) == 10U &&
                ordered32(linearDng, matrix2Entry + 4U) ==
                    static_cast<std::uint32_t>(sampleCount) * 3U &&
                ordered16(linearDng, illuminant1Entry + 8U) == 21U &&
                ordered16(linearDng, illuminant2Entry + 8U) == 17U &&
                matrixDenominatorsValid,
            "structurally complete " + std::to_string(sampleCount) +
                "-sample " + endian +
                " LinearRaw has Orientation, UniqueCameraModel, and paired ColorMatrices");
      const spectrafilm::DecodeResult linearResult = decode(linear);
      bool levelsExact = linearResult.ok &&
          linearResult.descriptor.colorChannels == sampleCount;
      for (std::size_t channel = 0U;
           channel < sampleCount && levelsExact; ++channel) {
        levelsExact =
            effectiveBlack(linearResult.descriptor, channel, 0U) ==
                linear.blackPattern[channel] &&
            linearResult.descriptor.whiteLevels[channel] ==
                linear.whiteLevels[channel];
      }
      const std::string sampleLabel = std::to_string(sampleCount) + "-sample ";
      check(linearResult.ok && levelsExact &&
                linearResult.descriptor.pixelLayout ==
                    spectrafilm::RawPixelLayout::Linear &&
                linearResult.descriptor.cfaPatternCount == 0U &&
                linearResult.descriptor.cfaPatternRows == 0 &&
                linearResult.descriptor.cfaPatternColumns == 0,
            "compliant " + sampleLabel + endian +
                " LinearRaw preserves per-channel levels",
            describe(linearResult));

      const std::size_t linearSamples =
          static_cast<std::size_t>(linear.width) * linear.height * sampleCount;
      std::vector<std::uint16_t> adjacentBase(linearSamples);
      for (std::size_t index = 0U; index < linearSamples; ++index) {
        const std::size_t channel = index % sampleCount;
        const std::uint32_t midpoint = linear.blackPattern[channel] +
            (linear.whiteLevels[channel] - linear.blackPattern[channel]) / 2U;
        adjacentBase[index] = static_cast<std::uint16_t>(midpoint);
      }
      const spectrafilm::DecodeResult linearBase = decode(linear, &adjacentBase);
      std::vector<std::uint64_t> perturbedDigests;
      for (std::uint16_t plane = 0U; plane < sampleCount; ++plane) {
        std::vector<std::uint16_t> perturbed = adjacentBase;
        for (std::size_t index = plane; index < perturbed.size();
             index += sampleCount) {
          ++perturbed[index];
        }
        const spectrafilm::DecodeResult changed = decode(linear, &perturbed);
        const bool independentlyVisible = linearBase.ok && changed.ok &&
            exactDigest(linearBase) != exactDigest(changed);
        check(independentlyVisible,
              "compliant " + sampleLabel + endian + " LinearRaw plane " +
                  std::to_string(plane) +
                  " independently preserves an adjacent-code perturbation",
              changed.ok ? describe(linearBase) : describe(changed));
        if (changed.ok) perturbedDigests.push_back(exactDigest(changed));
      }
      std::sort(perturbedDigests.begin(), perturbedDigests.end());
      check(perturbedDigests.size() == sampleCount &&
                std::adjacent_find(perturbedDigests.begin(),
                                   perturbedDigests.end()) ==
                    perturbedDigests.end(),
            "compliant " + sampleLabel + endian +
                " LinearRaw plane perturbations remain distinguishable");

      if (sampleCount == 4U) {
        const auto checkLinearPreOpenRejection =
            [&](const std::vector<std::uint8_t>& bytes,
                const std::string& label) {
              std::atomic<unsigned> openAttempts{0U};
              sfraw::test::setLibRawOpenAttemptObserverForTest(
                  countLibRawOpenAttempt, &openAttempts);
              const spectrafilm::DecodeResult result =
                  spectrafilm::decodeFromBuffer(bytes.data(), bytes.size(), {});
              sfraw::test::setLibRawOpenAttemptObserverForTest(nullptr, nullptr);
              check(!result.ok &&
                        result.status ==
                            spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
                        result.error.find("before LibRaw open") !=
                            std::string::npos &&
                        openAttempts.load() == 0U,
                    endian + " " + label,
                    describe(result));
            };

        sfraw::hosttest::PrecisionDngOptions missingMatrices = linear;
        missingMatrices.includeColorMatrix1 = false;
        missingMatrices.includeColorMatrix2 = false;
        checkLinearPreOpenRejection(
            sfraw::hosttest::makePrecisionUncompressedDng(missingMatrices),
            "four-plane LinearRaw missing ColorMatrices fails closed");

        sfraw::hosttest::PrecisionDngOptions singleMatrix = linear;
        singleMatrix.includeColorMatrix2 = false;
        checkLinearPreOpenRejection(
            sfraw::hosttest::makePrecisionUncompressedDng(singleMatrix),
            "four-plane LinearRaw single ColorMatrix fails closed");

        sfraw::hosttest::PrecisionDngOptions unpairedIlluminant = linear;
        unpairedIlluminant.includeCalibrationIlluminant2 = false;
        checkLinearPreOpenRejection(
            sfraw::hosttest::makePrecisionUncompressedDng(unpairedIlluminant),
            "four-plane LinearRaw unpaired calibration illuminant fails closed");

        sfraw::hosttest::PrecisionDngOptions planar = linear;
        planar.planarConfiguration = 2U;
        checkLinearPreOpenRejection(
            sfraw::hosttest::makePrecisionUncompressedDng(planar),
            "four-plane LinearRaw planar storage fails closed");

        sfraw::hosttest::PrecisionDngOptions extraSample = linear;
        extraSample.includeExtraSamples = true;
        checkLinearPreOpenRejection(
            sfraw::hosttest::makePrecisionUncompressedDng(extraSample),
            "four-plane LinearRaw ExtraSamples ambiguity fails closed");

        std::vector<std::uint8_t> wrongMatrixType =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        ordered16(&wrongMatrixType,
                  findIfdEntry(wrongMatrixType, 0xc621U) + 2U, 5U);
        checkLinearPreOpenRejection(
            wrongMatrixType,
            "four-plane LinearRaw ColorMatrix type fails closed");

        std::vector<std::uint8_t> wrongMatrixCount =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        ordered32(&wrongMatrixCount,
                  findIfdEntry(wrongMatrixCount, 0xc622U) + 4U, 9U);
        checkLinearPreOpenRejection(
            wrongMatrixCount,
            "four-plane LinearRaw ColorMatrix count fails closed");

        std::vector<std::uint8_t> zeroMatrixDenominator =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        const std::size_t matrix1Entry =
            findIfdEntry(zeroMatrixDenominator, 0xc621U);
        const std::size_t matrix1Payload =
            entryPayloadAt(zeroMatrixDenominator, matrix1Entry, 8U);
        ordered32(&zeroMatrixDenominator, matrix1Payload + 4U, 0U);
        checkLinearPreOpenRejection(
            zeroMatrixDenominator,
            "four-plane LinearRaw zero ColorMatrix denominator fails closed");

        for (const std::uint16_t tag : {0xc621U, 0xc622U}) {
          std::vector<std::uint8_t> zeroFourthRow =
              sfraw::hosttest::makePrecisionUncompressedDng(linear);
          const std::size_t matrix = findIfdEntry(zeroFourthRow, tag);
          const std::size_t payload =
              entryPayloadAt(zeroFourthRow, matrix, 8U);
          for (std::size_t xyz = 0U; xyz < 3U; ++xyz) {
            ordered32(&zeroFourthRow,
                      payload + (9U + xyz) * 8U, 0U);
          }
          checkLinearPreOpenRejection(
              zeroFourthRow,
              "four-plane LinearRaw zero fourth row in ColorMatrix" +
                  std::string(tag == 0xc621U ? "1" : "2") +
                  " fails closed");
        }

        std::vector<std::uint8_t> wrongIlluminantType =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        ordered16(&wrongIlluminantType,
                  findIfdEntry(wrongIlluminantType, 0xc65aU) + 2U, 4U);
        checkLinearPreOpenRejection(
            wrongIlluminantType,
            "four-plane LinearRaw calibration illuminant type fails closed");

        std::vector<std::uint8_t> wrongIlluminantCount =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        ordered32(&wrongIlluminantCount,
                  findIfdEntry(wrongIlluminantCount, 0xc65bU) + 4U, 2U);
        checkLinearPreOpenRejection(
            wrongIlluminantCount,
            "four-plane LinearRaw calibration illuminant count fails closed");
      }

      if (sampleCount == 3U) {
        sfraw::hosttest::PrecisionDngOptions externalBoundary = linear;
        externalBoundary.blackPattern = {4092U, 4093U, 4094U};
        externalBoundary.whiteLevels = {4095U, 4095U, 4095U};
        const spectrafilm::DecodeResult externalBoundaryResult =
            decode(externalBoundary);
        check(externalBoundaryResult.ok &&
                  externalBoundaryResult.descriptor.effectiveBitsPerSample == 2,
              endian +
                  " three-plane LinearRaw effective precision ignores the padded fourth carrier slot",
              describe(externalBoundaryResult));

        std::vector<std::uint8_t> externalOverRange =
            sfraw::hosttest::makePrecisionUncompressedDng(externalBoundary);
        const std::size_t externalBlackEntry =
            findIfdEntry(externalOverRange, 0xc61aU);
        const std::size_t externalBlackAt =
            ordered32(externalOverRange, externalBlackEntry + 8U);
        ordered32(&externalOverRange, externalBlackAt + 4U, 4096U);
        const spectrafilm::DecodeResult externalOverRangeResult =
            spectrafilm::decodeFromBuffer(
                externalOverRange.data(), externalOverRange.size(), {});
        check(!externalOverRangeResult.ok &&
                  externalOverRangeResult.status ==
                      spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
                  externalOverRangeResult.error.find("before LibRaw open") !=
                      std::string::npos,
              endian +
                  " external LONG LinearRaw BlackLevel checks every channel before LibRaw open",
              describe(externalOverRangeResult));

        std::vector<std::uint8_t> wrongBlackCount =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        ordered32(&wrongBlackCount,
                  findIfdEntry(wrongBlackCount, 0xc61aU) + 4U, 1U);
        const spectrafilm::DecodeResult wrongBlackCountResult =
            spectrafilm::decodeFromBuffer(
                wrongBlackCount.data(), wrongBlackCount.size(), {});
        check(!wrongBlackCountResult.ok &&
                  wrongBlackCountResult.status ==
                      spectrafilm::SFRAW_ERR_PRECISION_METADATA,
              endian +
                  " LinearRaw BlackLevel count must equal repeat cells times samples",
              describe(wrongBlackCountResult));

        std::vector<std::uint8_t> wrongWhiteCount =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        ordered32(&wrongWhiteCount,
                  findIfdEntry(wrongWhiteCount, 0xc61dU) + 4U, 1U);
        const spectrafilm::DecodeResult wrongWhiteCountResult =
            spectrafilm::decodeFromBuffer(
                wrongWhiteCount.data(), wrongWhiteCount.size(), {});
        check(!wrongWhiteCountResult.ok &&
                  wrongWhiteCountResult.status ==
                      spectrafilm::SFRAW_ERR_PRECISION_METADATA,
              endian + " LinearRaw WhiteLevel count must equal samples",
              describe(wrongWhiteCountResult));

        sfraw::hosttest::PrecisionDngOptions spatialLinear = linear;
        spatialLinear.blackRepeatWidth = 2U;
        spatialLinear.blackPattern = {
            32U, 49U, 66U, 33U, 50U, 67U,
        };
        const spectrafilm::DecodeResult spatialLinearResult =
            decode(spatialLinear);
        check(!spatialLinearResult.ok &&
                  spatialLinearResult.status ==
                      spectrafilm::SFRAW_ERR_PRECISION_METADATA,
              endian +
                  " spatial LinearRaw BlackLevel matrix is explicit v1 unsupported",
              describe(spatialLinearResult));

        std::vector<std::uint8_t> mixedBits =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        const std::size_t bitsEntry = findIfdEntry(mixedBits, 0x0102U);
        ordered16(&mixedBits, shortArrayAt(mixedBits, bitsEntry) + 2U, 10U);
        const spectrafilm::DecodeResult mixedBitsResult =
            spectrafilm::decodeFromBuffer(mixedBits.data(), mixedBits.size(), {});
        check(!mixedBitsResult.ok &&
                  mixedBitsResult.status ==
                      spectrafilm::SFRAW_ERR_PRECISION_METADATA,
              "mixed " + endian + " BitsPerSample array fails closed",
              describe(mixedBitsResult));

        std::vector<std::uint8_t> mixedFormat =
            sfraw::hosttest::makePrecisionUncompressedDng(linear);
        const std::size_t formatEntry = findIfdEntry(mixedFormat, 0x0153U);
        ordered16(&mixedFormat, shortArrayAt(mixedFormat, formatEntry) + 4U, 3U);
        const spectrafilm::DecodeResult mixedFormatResult =
            spectrafilm::decodeFromBuffer(
                mixedFormat.data(), mixedFormat.size(), {});
        check(!mixedFormatResult.ok &&
                  mixedFormatResult.status ==
                      spectrafilm::SFRAW_ERR_PRECISION_METADATA,
              "mixed " + endian + " SampleFormat array fails closed",
              describe(mixedFormatResult));
      }
    }
  }

  for (const auto byteOrder : {
           sfraw::hosttest::TiffByteOrder::kLittleEndian,
           sfraw::hosttest::TiffByteOrder::kBigEndian,
       }) {
    const std::string endian = byteOrder ==
            sfraw::hosttest::TiffByteOrder::kBigEndian
        ? "BE"
        : "LE";
    sfraw::hosttest::PrecisionDngOptions cfa;
    cfa.byteOrder = byteOrder;

    sfraw::hosttest::PrecisionDngOptions defaultCfa = cfa;
    defaultCfa.includeCfaPlaneColor = false;
    defaultCfa.includeCfaLayout = false;
    const spectrafilm::DecodeResult defaultCfaResult = decode(defaultCfa);
    check(defaultCfaResult.ok,
          endian + " CFA accepts the specified RGB/rectangular defaults",
          describe(defaultCfaResult));

    sfraw::hosttest::PrecisionDngOptions sevenBitOptions = cfa;
    sevenBitOptions.bitsPerSample = 8U;
    sevenBitOptions.blackLevel = 1U;
    sevenBitOptions.whiteLevel = 127U;
    std::vector<std::uint8_t> sevenBit =
        sfraw::hosttest::makePrecisionUncompressedDng(sevenBitOptions);
    ordered16(&sevenBit, findIfdEntry(sevenBit, 0x0102U) + 8U, 7U);
    const spectrafilm::DecodeResult sevenBitResult =
        spectrafilm::decodeFromBuffer(sevenBit.data(), sevenBit.size(), {});
    check(!sevenBitResult.ok &&
              sevenBitResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              sevenBitResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " BitsPerSample below the qualified 8-bit floor fails closed",
          describe(sevenBitResult));

    sfraw::hosttest::PrecisionDngOptions remappedCfa = cfa;
    remappedCfa.cfaPlaneColors = {0U, 2U, 1U};
    const spectrafilm::DecodeResult remappedCfaResult = decode(remappedCfa);
    check(!remappedCfaResult.ok &&
              remappedCfaResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              remappedCfaResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " non-identity CFAPlaneColor mapping fails before LibRaw open",
          describe(remappedCfaResult));

    sfraw::hosttest::PrecisionDngOptions nonRgbPlane = cfa;
    nonRgbPlane.cfaPlaneColors = {0U, 1U, 2U, 3U};
    const spectrafilm::DecodeResult nonRgbPlaneResult = decode(nonRgbPlane);
    check(!nonRgbPlaneResult.ok &&
              nonRgbPlaneResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              nonRgbPlaneResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian +
              " four-plane/non-RGB CFAPlaneColor fails before LibRaw open",
          describe(nonRgbPlaneResult));

    sfraw::hosttest::PrecisionDngOptions staggeredCfa = cfa;
    staggeredCfa.cfaLayout = 2U;
    const spectrafilm::DecodeResult staggeredCfaResult = decode(staggeredCfa);
    check(!staggeredCfaResult.ok &&
              staggeredCfaResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA,
          endian + " staggered CFALayout fails closed for descriptor v1",
          describe(staggeredCfaResult));

    sfraw::hosttest::PrecisionDngOptions duplicatePlane = cfa;
    duplicatePlane.duplicateCfaPlaneColor = true;
    const spectrafilm::DecodeResult duplicatePlaneResult = decode(duplicatePlane);
    check(!duplicatePlaneResult.ok &&
              duplicatePlaneResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA,
          endian + " duplicate selected-IFD CFAPlaneColor fails closed",
          describe(duplicatePlaneResult));

    sfraw::hosttest::PrecisionDngOptions duplicateLayout = cfa;
    duplicateLayout.duplicateCfaLayout = true;
    const spectrafilm::DecodeResult duplicateLayoutResult = decode(duplicateLayout);
    check(!duplicateLayoutResult.ok &&
              duplicateLayoutResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA,
          endian + " duplicate selected-IFD CFALayout fails closed",
          describe(duplicateLayoutResult));

    std::vector<std::uint8_t> malformedPlane =
        sfraw::hosttest::makePrecisionUncompressedDng(cfa);
    ordered16(&malformedPlane, findIfdEntry(malformedPlane, 0xc616U) + 2U, 3U);
    const spectrafilm::DecodeResult malformedPlaneResult =
        spectrafilm::decodeFromBuffer(
            malformedPlane.data(), malformedPlane.size(), {});
    check(!malformedPlaneResult.ok &&
              malformedPlaneResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA,
          endian + " malformed selected-IFD CFAPlaneColor type fails closed",
          describe(malformedPlaneResult));

    std::vector<std::uint8_t> malformedLayoutType =
        sfraw::hosttest::makePrecisionUncompressedDng(cfa);
    ordered16(&malformedLayoutType,
              findIfdEntry(malformedLayoutType, 0xc617U) + 2U, 4U);
    const spectrafilm::DecodeResult malformedLayoutTypeResult =
        spectrafilm::decodeFromBuffer(
            malformedLayoutType.data(), malformedLayoutType.size(), {});
    check(!malformedLayoutTypeResult.ok &&
              malformedLayoutTypeResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              malformedLayoutTypeResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " malformed CFALayout type fails before LibRaw open",
          describe(malformedLayoutTypeResult));

    std::vector<std::uint8_t> malformedLayoutCount =
        sfraw::hosttest::makePrecisionUncompressedDng(cfa);
    ordered32(&malformedLayoutCount,
              findIfdEntry(malformedLayoutCount, 0xc617U) + 4U, 2U);
    const spectrafilm::DecodeResult malformedLayoutCountResult =
        spectrafilm::decodeFromBuffer(
            malformedLayoutCount.data(), malformedLayoutCount.size(), {});
    check(!malformedLayoutCountResult.ok &&
              malformedLayoutCountResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              malformedLayoutCountResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " malformed CFALayout count fails before LibRaw open",
          describe(malformedLayoutCountResult));
  }

  const std::vector<std::uint8_t> subIfd =
      sfraw::hosttest::makePrecisionSubIfdDngWithPreviewMetadata();
  const spectrafilm::DecodeResult subIfdResult =
      spectrafilm::decodeFromBuffer(subIfd.data(), subIfd.size(), {});
  check(subIfdResult.ok &&
            subIfdResult.descriptor.declaredBitsPerSample == 12 &&
            subIfdResult.descriptor.blackLevelProvenance ==
                spectrafilm::RawLevelProvenance::DngMetadata &&
            effectiveBlack(subIfdResult.descriptor, 0U, 0U) == 64U &&
            std::all_of(
                subIfdResult.descriptor.whiteLevels.begin(),
                subIfdResult.descriptor.whiteLevels.end(),
                [](std::uint32_t level) { return level == 4095U; }) &&
            subIfdResult.descriptor.baselineExposurePresent &&
            std::fabs(subIfdResult.descriptor.baselineExposure + 0.5f) < 1.0e-6f &&
            subIfdResult.descriptor.linearResponseLimitPresent &&
            std::fabs(subIfdResult.descriptor.linearResponseLimit - 0.75f) <
                1.0e-6f,
        "preview IFD CFA/precision/HDR tags cannot leak into the selected RAW SubIFD",
        describe(subIfdResult));

  sfraw::hosttest::PrecisionDngOptions duplicateGeometryOptions;
  std::vector<std::uint8_t> duplicateGeometry =
      sfraw::hosttest::makePrecisionUncompressedDng(duplicateGeometryOptions);
  const std::size_t rootIfd = ordered32(duplicateGeometry, 4U);
  const std::size_t rootIfdBytes = 2U +
      static_cast<std::size_t>(ordered16(duplicateGeometry, rootIfd)) * 12U + 4U;
  const std::uint32_t duplicateIfdAt =
      static_cast<std::uint32_t>(duplicateGeometry.size());
  const std::vector<std::uint8_t> clonedIfd(
      duplicateGeometry.begin() + rootIfd,
      duplicateGeometry.begin() + rootIfd + rootIfdBytes);
  duplicateGeometry.insert(
      duplicateGeometry.end(), clonedIfd.begin(), clonedIfd.end());
  ordered32(&duplicateGeometry, rootIfd + rootIfdBytes - 4U, duplicateIfdAt);
  const spectrafilm::DecodeResult duplicateGeometryResult =
      spectrafilm::decodeFromBuffer(
          duplicateGeometry.data(), duplicateGeometry.size(), {});
  check(!duplicateGeometryResult.ok &&
            duplicateGeometryResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "multiple RAW IFDs matching LibRaw geometry fail closed",
        describe(duplicateGeometryResult));

  sfraw::hosttest::PrecisionDngOptions multiIfdOptions;
  std::vector<std::uint8_t> unsafeSecondary =
      sfraw::hosttest::makePrecisionUncompressedDng(multiIfdOptions);
  const std::size_t primaryIfd = ordered32(unsafeSecondary, 4U);
  const std::size_t primaryIfdBytes = 2U +
      static_cast<std::size_t>(ordered16(unsafeSecondary, primaryIfd)) * 12U +
      4U;
  const std::uint32_t secondaryIfd =
      static_cast<std::uint32_t>(unsafeSecondary.size());
  const std::vector<std::uint8_t> secondaryClone(
      unsafeSecondary.begin() + primaryIfd,
      unsafeSecondary.begin() + primaryIfd + primaryIfdBytes);
  unsafeSecondary.insert(
      unsafeSecondary.end(), secondaryClone.begin(), secondaryClone.end());
  ordered32(&unsafeSecondary, primaryIfd + primaryIfdBytes - 4U, secondaryIfd);
  ordered32(&unsafeSecondary,
            findIfdEntryAt(unsafeSecondary, secondaryIfd, 0x0100U) + 8U, 32U);
  ordered32(&unsafeSecondary,
            findIfdEntryAt(unsafeSecondary, secondaryIfd, 0x0101U) + 8U, 32U);
  ordered32(&unsafeSecondary,
            findIfdEntryAt(unsafeSecondary, secondaryIfd, 0xc61aU) + 8U,
            4096U);

  std::atomic<unsigned> validOpenAttempts{0U};
  sfraw::test::setLibRawOpenAttemptObserverForTest(
      countLibRawOpenAttempt, &validOpenAttempts);
  const std::vector<std::uint8_t> validOpenControl =
      sfraw::hosttest::makePrecisionUncompressedDng(multiIfdOptions);
  const spectrafilm::DecodeResult validOpenControlResult =
      spectrafilm::decodeFromBuffer(
          validOpenControl.data(), validOpenControl.size(), {});
  sfraw::test::setLibRawOpenAttemptObserverForTest(nullptr, nullptr);
  check(validOpenControlResult.ok && validOpenAttempts.load() == 1U,
        "independent observer sees the valid control enter LibRaw open_buffer",
        describe(validOpenControlResult));

  std::atomic<unsigned> unsafeOpenAttempts{0U};
  sfraw::test::setLibRawOpenAttemptObserverForTest(
      countLibRawOpenAttempt, &unsafeOpenAttempts);
  const spectrafilm::DecodeResult unsafeSecondaryResult =
      spectrafilm::decodeFromBuffer(
          unsafeSecondary.data(), unsafeSecondary.size(), {});
  sfraw::test::setLibRawOpenAttemptObserverForTest(nullptr, nullptr);
  check(!unsafeSecondaryResult.ok &&
            unsafeSecondaryResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
            unsafeSecondaryResult.error.find("before LibRaw open") !=
                std::string::npos &&
            unsafeOpenAttempts.load() == 0U,
        "smaller non-reduced RAW IFD precision violation rejects before any LibRaw open attempt",
        describe(unsafeSecondaryResult));

  for (const sfraw::hosttest::TiffByteOrder byteOrder : {
           sfraw::hosttest::TiffByteOrder::kLittleEndian,
           sfraw::hosttest::TiffByteOrder::kBigEndian}) {
    sfraw::hosttest::PrecisionDngOptions hostileSiblingOptions;
    hostileSiblingOptions.byteOrder = byteOrder;
    std::vector<std::uint8_t> hostileSibling =
        sfraw::hosttest::makePrecisionUncompressedDng(hostileSiblingOptions);
    const std::size_t cleanIfd = ordered32(hostileSibling, 4U);
    const std::size_t cleanIfdBytes = 2U +
        static_cast<std::size_t>(ordered16(hostileSibling, cleanIfd)) * 12U +
        4U;
    const std::uint32_t hostileIfd =
        static_cast<std::uint32_t>(hostileSibling.size());
    const std::vector<std::uint8_t> hostileClone(
        hostileSibling.begin() + cleanIfd,
        hostileSibling.begin() + cleanIfd + cleanIfdBytes);
    hostileSibling.insert(
        hostileSibling.end(), hostileClone.begin(), hostileClone.end());
    ordered32(&hostileSibling, cleanIfd + cleanIfdBytes - 4U, hostileIfd);

    // Preserve a completely valid root DNG while making the next RAW IFD
    // hostile in exactly the fields that used to exempt it from the pre-open
    // aggregate: malformed ImageWidth leaves scanner geometry at zero, and a
    // negative SRATIONAL BlackLevel reaches vendored LibRaw if admission fails.
    const std::size_t hostileWidth =
        findIfdEntryAt(hostileSibling, hostileIfd, 0x0100U);
    ordered16(&hostileSibling, hostileWidth + 2U, 2U);  // ASCII, not SHORT/LONG.
    const std::size_t hostileBlack =
        findIfdEntryAt(hostileSibling, hostileIfd, 0xc61aU);
    ordered16(&hostileSibling, hostileBlack + 2U, 10U);  // SRATIONAL is invalid.
    ordered32(&hostileSibling, hostileBlack + 4U, 1U);
    const std::uint32_t hostileBlackPayload =
        static_cast<std::uint32_t>(hostileSibling.size());
    hostileSibling.resize(hostileSibling.size() + 8U, 0U);
    ordered32(&hostileSibling, hostileBlack + 8U, hostileBlackPayload);
    ordered32(&hostileSibling, hostileBlackPayload, 0xffffffffU);  // -1
    ordered32(&hostileSibling, hostileBlackPayload + 4U, 1U);

    std::atomic<unsigned> hostileSiblingOpenAttempts{0U};
    sfraw::test::setLibRawOpenAttemptObserverForTest(
        countLibRawOpenAttempt, &hostileSiblingOpenAttempts);
    const spectrafilm::DecodeResult hostileSiblingResult =
        spectrafilm::decodeFromBuffer(
            hostileSibling.data(), hostileSibling.size(), {});
    sfraw::test::setLibRawOpenAttemptObserverForTest(nullptr, nullptr);
    const std::string endianLabel = byteOrder ==
            sfraw::hosttest::TiffByteOrder::kBigEndian
        ? "BE"
        : "LE";
    check(!hostileSiblingResult.ok &&
              hostileSiblingResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              hostileSiblingResult.error.find("before LibRaw open") !=
                  std::string::npos &&
              hostileSiblingOpenAttempts.load() == 0U,
          endianLabel +
              " malformed-geometry hostile RAW sibling rejects before LibRaw open",
          describe(hostileSiblingResult));

    std::vector<std::uint8_t> reducedHostileSibling = hostileSibling;
    const std::size_t reducedSubfileType =
        findIfdEntryAt(reducedHostileSibling, hostileIfd, 0x00feU);
    ordered32(&reducedHostileSibling, reducedSubfileType + 8U, 1U);
    std::atomic<unsigned> reducedHostileOpenAttempts{0U};
    sfraw::test::setLibRawOpenAttemptObserverForTest(
        countLibRawOpenAttempt, &reducedHostileOpenAttempts);
    const spectrafilm::DecodeResult reducedHostileResult =
        spectrafilm::decodeFromBuffer(
            reducedHostileSibling.data(), reducedHostileSibling.size(), {});
    sfraw::test::setLibRawOpenAttemptObserverForTest(nullptr, nullptr);
    check(!reducedHostileResult.ok &&
              reducedHostileResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              reducedHostileResult.error.find("before LibRaw open") !=
                  std::string::npos &&
              reducedHostileOpenAttempts.load() == 0U,
          endianLabel +
              " malformed BlackLevel in a reduced sibling rejects before LibRaw open",
          describe(reducedHostileResult));
  }

  // Public-route dependency-safety matrix. Every case is exercised through
  // both production entry points in both TIFF byte orders, and the observer is
  // independent from returned status so a typed-but-too-late rejection fails.
  for (const sfraw::hosttest::TiffByteOrder byteOrder : {
           sfraw::hosttest::TiffByteOrder::kLittleEndian,
           sfraw::hosttest::TiffByteOrder::kBigEndian}) {
    const std::string endian = byteOrder ==
            sfraw::hosttest::TiffByteOrder::kBigEndian
        ? "BE"
        : "LE";
    checkHostilePreOpenMatrix(
        makeReducedDeltaChild(
            byteOrder, ChildLink::kNextIfd, false, false),
        endian + "/reduced-next-IFD BlackLevelDeltaH");
    checkHostilePreOpenMatrix(
        makeReducedDeltaChild(
            byteOrder, ChildLink::kSubIfd, false, false),
        endian + "/reduced-SubIFD BlackLevelDeltaH");
    checkHostilePreOpenMatrix(
        makeReducedDeltaChild(
            byteOrder, ChildLink::kNextIfd, true, false),
        endian + "/malformed-eligibility reduced delta child");
    checkHostilePreOpenMatrix(
        makeReducedDeltaChild(
            byteOrder, ChildLink::kSubIfd, false, true),
        endian + "/malformed reduced delta child");
    checkHostilePreOpenMatrix(
        makeNonRootDngVersion(
            byteOrder, ChildLink::kNextIfd, false),
        endian + "/valid non-root DNGVersion next-IFD");
    checkHostilePreOpenMatrix(
        makeNonRootDngVersion(
            byteOrder, ChildLink::kNextIfd, true),
        endian + "/malformed non-root DNGVersion next-IFD");
    checkHostilePreOpenMatrix(
        makeNonRootDngVersion(
            byteOrder, ChildLink::kSubIfd, false),
        endian + "/valid non-root DNGVersion SubIFD");
    checkHostilePreOpenMatrix(
        makeNonRootDngVersion(
            byteOrder, ChildLink::kSubIfd, true),
        endian + "/malformed non-root DNGVersion SubIFD");
    checkHostilePreOpenMatrix(
        makeNegativeBlackChild(
            byteOrder, ChildLink::kNextIfd, false, true),
        endian + "/malformed-eligibility negative BlackLevel next-IFD");
    checkHostilePreOpenMatrix(
        makeNegativeBlackChild(
            byteOrder, ChildLink::kSubIfd, true, false),
        endian + "/negative BlackLevel reduced SubIFD");

    sfraw::hosttest::PrecisionDngOptions malformedRootOptions;
    malformedRootOptions.byteOrder = byteOrder;
    std::vector<std::uint8_t> malformedRootVersion =
        sfraw::hosttest::makePrecisionUncompressedDng(malformedRootOptions);
    const std::size_t rootVersion =
        findIfdEntry(malformedRootVersion, 0xc612U);
    ordered16(&malformedRootVersion, rootVersion + 2U, 3U);
    checkHostilePreOpenMatrix(
        malformedRootVersion, endian + "/malformed root DNGVersion");
    checkHostilePreOpenMatrix(
        makeBackwardNextDelta(byteOrder),
        endian + "/backward next-IFD hidden BlackLevelDeltaH");
    checkHostilePreOpenMatrix(
        makeDuplicateSubIfdWithHiddenDelta(byteOrder),
        endian + "/duplicate SubIFD hidden BlackLevelDeltaH");
    checkHostilePreOpenMatrix(
        makeSeventeenSubIfdsWithEarlyDelta(byteOrder),
        endian + "/17-count SubIFD early hidden BlackLevelDeltaH");
    checkHostilePreOpenMatrix(
        makeDepthFiveSubIfdDelta(byteOrder),
        endian + "/depth-five SubIFD hidden BlackLevelDeltaH");
    checkHostilePreOpenMatrix(
        makeSubIfdCycle(byteOrder), endian + "/cyclic SubIFD graph");
    checkHostilePreOpenMatrix(
        makeWrongMagicEndianHeader(byteOrder),
        endian + "/wrong-magic endian-header TIFF parser candidate");
  }

  sfraw::hosttest::PrecisionDngOptions duplicateWhite;
  duplicateWhite.duplicateWhiteLevel = true;
  const std::vector<std::uint8_t> duplicateWhiteDng =
      sfraw::hosttest::makePrecisionUncompressedDng(duplicateWhite);
  const spectrafilm::DecodeResult duplicateWhiteResult =
      spectrafilm::decodeFromBuffer(
          duplicateWhiteDng.data(), duplicateWhiteDng.size(), {});
  check(!duplicateWhiteResult.ok &&
            duplicateWhiteResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "conflicting selected-IFD WhiteLevel duplicates fail closed",
        describe(duplicateWhiteResult));

  sfraw::hosttest::PrecisionDngOptions delta;
  delta.includeBlackLevelDeltaH = true;
  const std::vector<std::uint8_t> deltaDng =
      sfraw::hosttest::makePrecisionUncompressedDng(delta);
  const spectrafilm::DecodeResult deltaResult = spectrafilm::decodeFromBuffer(
      deltaDng.data(), deltaDng.size(), {});
  check(!deltaResult.ok &&
            deltaResult.status == spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "BlackLevelDeltaH is a typed unsupported transformation",
        describe(deltaResult));

  sfraw::hosttest::PrecisionDngOptions oversizedRepeat;
  oversizedRepeat.blackRepeatWidth = 64U;
  oversizedRepeat.blackRepeatHeight = 1U;
  oversizedRepeat.blackPattern.assign(64U, oversizedRepeat.blackLevel);
  const std::vector<std::uint8_t> oversizedRepeatDng =
      sfraw::hosttest::makePrecisionUncompressedDng(oversizedRepeat);
  const spectrafilm::DecodeResult oversizedRepeatResult =
      spectrafilm::decodeFromBuffer(
          oversizedRepeatDng.data(), oversizedRepeatDng.size(), {});
  check(!oversizedRepeatResult.ok &&
            oversizedRepeatResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "native and Kotlin both reject a 1x64 black repeat shape",
        describe(oversizedRepeatResult));

  sfraw::hosttest::PrecisionDngOptions customCfa;
  customCfa.cfaPatternRows = 8U;
  customCfa.cfaPatternColumns = 2U;
  customCfa.cfaPattern = {
      0U, 1U, 1U, 2U, 0U, 1U, 1U, 2U,
      1U, 0U, 2U, 1U, 1U, 0U, 2U, 1U,
  };
  const std::vector<std::uint8_t> customCfaDng =
      sfraw::hosttest::makePrecisionUncompressedDng(customCfa);
  const spectrafilm::DecodeResult customCfaResult =
      spectrafilm::decodeFromBuffer(customCfaDng.data(), customCfaDng.size(), {});
  check(!customCfaResult.ok &&
            customCfaResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "unrepresented custom CFA geometry fails closed",
        describe(customCfaResult));

  sfraw::hosttest::PrecisionDngOptions xtrans;
  // LibRaw's production X-Trans interpolation requires both visible dimensions
  // to be at least LIBRAW_AHD_TILE (512). Keep this synthetic plane just above
  // that independent dependency precondition so the fixture exercises the
  // published descriptor and the actual postprocess route.
  xtrans.width = 520U;
  xtrans.height = 520U;
  xtrans.cfaPatternRows = 6U;
  xtrans.cfaPatternColumns = 6U;
  xtrans.cfaPattern = {
      1U, 0U, 1U, 1U, 2U, 1U,
      2U, 1U, 2U, 0U, 1U, 0U,
      1U, 0U, 1U, 1U, 2U, 1U,
      1U, 2U, 1U, 1U, 0U, 1U,
      0U, 1U, 0U, 2U, 1U, 2U,
      1U, 2U, 1U, 1U, 0U, 1U,
  };
  const spectrafilm::DecodeResult xtransResult = decode(xtrans);
  check(xtransResult.ok &&
            xtransResult.descriptor.pixelLayout ==
                spectrafilm::RawPixelLayout::XTrans6x6 &&
            xtransResult.descriptor.cfaPatternRows == 6 &&
            xtransResult.descriptor.cfaPatternColumns == 6 &&
            xtransResult.descriptor.cfaPatternCount == 36U,
        "verified X-Trans 6x6 geometry and full pattern are published",
        describe(xtransResult));
  // Regression pin for the X-Trans route. Patch 0024 rewrites `-i << c` (UB:
  // left shift of a negative value) as `-(i << c)`, which is the value
  // two's-complement gcc/clang produced for the UB form at these small shift
  // magnitudes; pre-fix and patched digests were captured identical in the
  // shipping-serial configuration, proving the rewrite arithmetic-neutral.
  // The float digest is build-configuration-sensitive: the serial leg and the
  // OpenMP leg each produce their own stable value (verified identical across
  // independent CI runs per leg). Pin per configuration; any future drift in
  // the X-Trans decode fails here.
#if defined(SFRAW_HOST_OPENMP_ACTIVE)
  constexpr std::uint64_t kCiXtransDigest = 2892219489530756344ULL;
#else
  constexpr std::uint64_t kCiXtransDigest = 5238915555911424415ULL;
#endif
  check(xtransResult.ok && exactDigest(xtransResult) == kCiXtransDigest,
        "defined X-Trans index arithmetic matches the pinned CI reference digest",
        xtransResult.ok
            ? "digest=" + std::to_string(exactDigest(xtransResult))
            : describe(xtransResult));

  sfraw::hosttest::PrecisionDngOptions malformedCfaPayload;
  malformedCfaPayload.cfaPatternRows = 6U;
  malformedCfaPayload.cfaPatternColumns = 6U;
  malformedCfaPayload.cfaPattern.resize(36U);
  for (std::size_t cell = 0U; cell < malformedCfaPayload.cfaPattern.size(); ++cell) {
    malformedCfaPayload.cfaPattern[cell] =
        static_cast<std::uint8_t>(cell % 3U);
  }
  std::vector<std::uint8_t> malformedCfa =
      sfraw::hosttest::makePrecisionUncompressedDng(malformedCfaPayload);
  ordered32(&malformedCfa, findIfdEntry(malformedCfa, 0x828eU) + 8U,
            static_cast<std::uint32_t>(malformedCfa.size() + 64U));
  const spectrafilm::DecodeResult malformedCfaResult =
      spectrafilm::decodeFromBuffer(
          malformedCfa.data(), malformedCfa.size(), {});
  check(!malformedCfaResult.ok &&
            malformedCfaResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "out-of-bounds selected-IFD CFA payload fails the bounded preflight",
        describe(malformedCfaResult));

  sfraw::hosttest::PrecisionDngOptions malformedBlackPayload;
  malformedBlackPayload.blackRepeatWidth = 2U;
  malformedBlackPayload.blackRepeatHeight = 2U;
  malformedBlackPayload.blackPattern.assign(4U, 64U);
  std::vector<std::uint8_t> malformedBlack =
      sfraw::hosttest::makePrecisionUncompressedDng(malformedBlackPayload);
  ordered32(&malformedBlack, findIfdEntry(malformedBlack, 0xc61aU) + 8U,
            static_cast<std::uint32_t>(malformedBlack.size() + 64U));
  const spectrafilm::DecodeResult malformedBlackResult =
      spectrafilm::decodeFromBuffer(
          malformedBlack.data(), malformedBlack.size(), {});
  check(!malformedBlackResult.ok &&
            malformedBlackResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
            malformedBlackResult.error.find("before LibRaw open") !=
                std::string::npos,
        "invalid external BlackLevel is rejected before LibRaw open_buffer",
        describe(malformedBlackResult));

  for (const auto byteOrder : {
           sfraw::hosttest::TiffByteOrder::kLittleEndian,
           sfraw::hosttest::TiffByteOrder::kBigEndian,
       }) {
    const std::string endian = byteOrder ==
            sfraw::hosttest::TiffByteOrder::kBigEndian
        ? "BE"
        : "LE";
    sfraw::hosttest::PrecisionDngOptions numeric;
    numeric.byteOrder = byteOrder;

    std::vector<std::uint8_t> shortBlack =
        sfraw::hosttest::makePrecisionUncompressedDng(numeric);
    const std::size_t shortEntry = findIfdEntry(shortBlack, 0xc61aU);
    ordered16(&shortBlack, shortEntry + 2U, 3U);
    ordered16(&shortBlack, shortEntry + 8U, 64U);
    ordered16(&shortBlack, shortEntry + 10U, 0U);
    const spectrafilm::DecodeResult shortBlackResult =
        spectrafilm::decodeFromBuffer(shortBlack.data(), shortBlack.size(), {});
    check(shortBlackResult.ok,
          endian + " inline SHORT BlackLevel is value-checked and admitted",
          describe(shortBlackResult));

    std::vector<std::uint8_t> oversizedShort = shortBlack;
    ordered16(&oversizedShort, shortEntry + 8U, 4096U);
    const spectrafilm::DecodeResult oversizedShortResult =
        spectrafilm::decodeFromBuffer(
            oversizedShort.data(), oversizedShort.size(), {});
    check(!oversizedShortResult.ok &&
              oversizedShortResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              oversizedShortResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " above-declared-range SHORT BlackLevel fails before LibRaw open",
          describe(oversizedShortResult));

    sfraw::hosttest::PrecisionDngOptions typedLinear;
    typedLinear.linearRaw = true;
    typedLinear.samplesPerPixel = 3U;
    typedLinear.includeSampleFormat = true;
    typedLinear.byteOrder = byteOrder;
    typedLinear.blackPattern = {64U, 80U, 96U};
    typedLinear.whiteLevels = {4095U, 4095U, 4095U};
    std::vector<std::uint8_t> externalShort =
        sfraw::hosttest::makePrecisionUncompressedDng(typedLinear);
    const std::size_t externalShortEntry =
        findIfdEntry(externalShort, 0xc61aU);
    const std::size_t externalShortAt =
        ordered32(externalShort, externalShortEntry + 8U);
    ordered16(&externalShort, externalShortEntry + 2U, 3U);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
      ordered16(&externalShort, externalShortAt + channel * 2U,
                static_cast<std::uint16_t>(typedLinear.blackPattern[channel]));
    }
    const spectrafilm::DecodeResult externalShortResult =
        spectrafilm::decodeFromBuffer(
            externalShort.data(), externalShort.size(), {});
    check(externalShortResult.ok,
          endian + " external SHORT LinearRaw BlackLevel checks all channels",
          describe(externalShortResult));

    std::vector<std::uint8_t> externalShortBad = externalShort;
    ordered16(&externalShortBad, externalShortAt + 4U, 4096U);
    const spectrafilm::DecodeResult externalShortBadResult =
        spectrafilm::decodeFromBuffer(
            externalShortBad.data(), externalShortBad.size(), {});
    check(!externalShortBadResult.ok &&
              externalShortBadResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              externalShortBadResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian +
              " external SHORT BlackLevel rejects an out-of-range non-first channel",
          describe(externalShortBadResult));

    sfraw::hosttest::PrecisionDngOptions longBoundary = numeric;
    longBoundary.blackLevel = 4094U;
    longBoundary.whiteLevel = 4095U;
    const spectrafilm::DecodeResult longBoundaryResult = decode(longBoundary);
    check(longBoundaryResult.ok &&
              effectiveBlack(longBoundaryResult.descriptor, 0U, 0U) == 4094U,
          endian +
              " inline LONG BlackLevel admits the declared adjacent-to-white boundary",
          describe(longBoundaryResult));

    std::vector<std::uint8_t> oversizedLong =
        sfraw::hosttest::makePrecisionUncompressedDng(numeric);
    const std::size_t longEntry = findIfdEntry(oversizedLong, 0xc61aU);
    ordered32(&oversizedLong, longEntry + 8U, 4096U);
    const spectrafilm::DecodeResult oversizedLongResult =
        spectrafilm::decodeFromBuffer(
            oversizedLong.data(), oversizedLong.size(), {});
    check(!oversizedLongResult.ok &&
              oversizedLongResult.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              oversizedLongResult.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " above-declared-range LONG BlackLevel fails before LibRaw open",
          describe(oversizedLongResult));

    const auto rationalBlack = [&](std::uint32_t numerator,
                                   std::uint32_t denominator) {
      std::vector<std::uint8_t> bytes =
          sfraw::hosttest::makePrecisionUncompressedDng(numeric);
      const std::size_t entry = findIfdEntry(bytes, 0xc61aU);
      const std::uint32_t valuesAt =
          static_cast<std::uint32_t>(bytes.size());
      bytes.resize(bytes.size() + 8U, 0U);
      ordered16(&bytes, entry + 2U, 5U);
      ordered32(&bytes, entry + 4U, 1U);
      ordered32(&bytes, entry + 8U, valuesAt);
      ordered32(&bytes, valuesAt, numerator);
      ordered32(&bytes, valuesAt + 4U, denominator);
      return bytes;
    };
    const auto rationalLinearBlack = [&](bool makeSecondFractional) {
      std::vector<std::uint8_t> bytes =
          sfraw::hosttest::makePrecisionUncompressedDng(typedLinear);
      const std::size_t entry = findIfdEntry(bytes, 0xc61aU);
      const std::uint32_t valuesAt =
          static_cast<std::uint32_t>(bytes.size());
      bytes.resize(bytes.size() + 24U, 0U);
      ordered16(&bytes, entry + 2U, 5U);
      ordered32(&bytes, entry + 4U, 3U);
      ordered32(&bytes, entry + 8U, valuesAt);
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const std::uint32_t numerator = makeSecondFractional && channel == 1U
            ? 161U
            : typedLinear.blackPattern[channel];
        const std::uint32_t denominator = makeSecondFractional && channel == 1U
            ? 2U
            : 1U;
        ordered32(&bytes, valuesAt + channel * 8U, numerator);
        ordered32(&bytes, valuesAt + channel * 8U + 4U, denominator);
      }
      return bytes;
    };
    const auto decodeBytes = [](const std::vector<std::uint8_t>& bytes) {
      return spectrafilm::decodeFromBuffer(bytes.data(), bytes.size(), {});
    };

    const spectrafilm::DecodeResult integralRational =
        decodeBytes(rationalBlack(64U, 1U));
    check(integralRational.ok,
          endian + " integral RATIONAL BlackLevel is admitted",
          describe(integralRational));

    const spectrafilm::DecodeResult integralRationalArray =
        decodeBytes(rationalLinearBlack(false));
    check(integralRationalArray.ok,
          endian +
              " external RATIONAL LinearRaw BlackLevel checks all channels",
          describe(integralRationalArray));

    const spectrafilm::DecodeResult fractionalRationalArray =
        decodeBytes(rationalLinearBlack(true));
    check(!fractionalRationalArray.ok &&
              fractionalRationalArray.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              fractionalRationalArray.error.find("before LibRaw open") !=
                  std::string::npos,
          endian +
              " external RATIONAL BlackLevel rejects a fractional non-first channel",
          describe(fractionalRationalArray));

    const spectrafilm::DecodeResult fractionalRational =
        decodeBytes(rationalBlack(65U, 2U));
    check(!fractionalRational.ok &&
              fractionalRational.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              fractionalRational.error.find("before LibRaw open") !=
                  std::string::npos,
          endian + " fractional RATIONAL BlackLevel fails before LibRaw open",
          describe(fractionalRational));

    const spectrafilm::DecodeResult zeroDenominator =
        decodeBytes(rationalBlack(64U, 0U));
    check(!zeroDenominator.ok &&
              zeroDenominator.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              zeroDenominator.error.find("before LibRaw open") !=
                  std::string::npos,
          endian +
              " zero-denominator RATIONAL BlackLevel fails before LibRaw open",
          describe(zeroDenominator));

    const spectrafilm::DecodeResult oversizedRational =
        decodeBytes(rationalBlack(4096U, 1U));
    check(!oversizedRational.ok &&
              oversizedRational.status ==
                  spectrafilm::SFRAW_ERR_PRECISION_METADATA &&
              oversizedRational.error.find("before LibRaw open") !=
                  std::string::npos,
          endian +
              " above-declared-range RATIONAL BlackLevel fails before LibRaw open",
          describe(oversizedRational));
  }

  const std::vector<std::uint8_t> lj92 =
      sfraw::hosttest::makeValidLosslessJpegDng();
  spectrafilm::DecodeOptions lj92Options;
  lj92Options.whiteBalance = spectrafilm::WhiteBalanceMode::Daylight;
  const spectrafilm::DecodeResult lj92Result = spectrafilm::decodeFromBuffer(
      lj92.data(), lj92.size(), lj92Options);
  check(lj92Result.ok &&
            lj92Result.descriptor.containerCompression == 7 &&
            lj92Result.descriptor.packing ==
                spectrafilm::RawPacking::LosslessCompressed &&
            lj92Result.descriptor.processedBitsPerSample == 16,
        "qualified Compression 7/LJ92 publishes the precision descriptor",
        describe(lj92Result));
  if (lj92Result.ok) {
    // The fixture's SOF3 precision is 16 and every Huffman symbol is category
    // zero, so predictor 1 independently defines every source sample as 32768.
    // The neutral center should therefore remain near its known normalized code
    // after linear demosaic and the adopted ACES-to-ProPhoto transform.
    constexpr float kExpectedLjpegCode = 32768.0f / 65535.0f;
    const int x = lj92Result.width / 2;
    const int y = lj92Result.height / 2;
    const float luminance =
        0.2880402f * sampleAt(lj92Result, x, y, 0) +
        0.7118741f * sampleAt(lj92Result, x, y, 1) +
        0.0000857f * sampleAt(lj92Result, x, y, 2);
    check(std::isfinite(luminance) &&
              std::fabs(luminance - kExpectedLjpegCode) < 0.015f,
          "Compression 7/LJ92 preserves the independently known predictor code",
          "observed=" + std::to_string(luminance) +
              " expected=" + std::to_string(kExpectedLjpegCode));
  }

  sfraw::hosttest::PrecisionDngOptions defaults;
  defaults.includeBlackLevel = false;
  defaults.includeWhiteLevel = false;
  defaults.includeBaselineExposure = false;
  defaults.includeLinearResponseLimit = false;
  const spectrafilm::DecodeResult defaultLevels = decode(defaults);
  check(defaultLevels.ok,
        "absent optional DNG levels decode with declared-bit defaults",
        describe(defaultLevels));
  if (defaultLevels.ok) {
    const auto& descriptor = defaultLevels.descriptor;
    check(descriptor.blackLevelProvenance ==
              spectrafilm::RawLevelProvenance::DeclaredBitsDefault &&
              descriptor.whiteLevelProvenance ==
                  spectrafilm::RawLevelProvenance::DeclaredBitsDefault &&
              descriptor.blackLevelCommon == 0U &&
              std::all_of(descriptor.whiteLevels.begin(),
                          descriptor.whiteLevels.end(),
                          [](std::uint32_t level) { return level == 4095U; }) &&
              !descriptor.baselineExposurePresent &&
              !descriptor.linearResponseLimitPresent,
          "metadata absence stays distinct from explicit zero/default levels");
  }

  sfraw::hosttest::PrecisionDngOptions proxyOptions;
  const std::vector<std::uint8_t> proxyDng =
      sfraw::hosttest::makePrecisionUncompressedDng(proxyOptions);
  spectrafilm::DecodeOptions proxyDecodeOptions;
  proxyDecodeOptions.whiteBalance = spectrafilm::WhiteBalanceMode::Daylight;
  proxyDecodeOptions.maxLongEdge = 20;
  const spectrafilm::DecodeResult proxy = spectrafilm::decodeFromBuffer(
      proxyDng.data(), proxyDng.size(), proxyDecodeOptions);
  check(proxy.ok && proxy.width <= 20 && proxy.height <= 20 &&
            proxy.descriptor.requestedMaxLongEdge == 20 &&
            proxy.descriptor.outputSubsampleStep == 4,
        "descriptor reports requested and actual bounded proxy reduction",
        describe(proxy));

  sfraw::hosttest::PrecisionDngOptions patterned;
  patterned.bitsPerSample = 14U;
  patterned.blackLevel = 256U;
  patterned.whiteLevel = 16000U;
  patterned.blackRepeatWidth = 2U;
  patterned.blackRepeatHeight = 2U;
  patterned.blackPattern = {256U, 257U, 258U, 259U};
  const spectrafilm::DecodeResult patternedResult = decode(patterned);
  check(patternedResult.ok, "per-cell BlackLevel fixture decodes",
        describe(patternedResult));
  if (patternedResult.ok) {
    std::vector<std::uint32_t> reconstructed;
    const auto& descriptor = patternedResult.descriptor;
    const std::size_t cells = descriptor.blackPatternCount == 0U
        ? 1U
        : descriptor.blackPatternCount;
    for (std::size_t channel = 0U; channel < 4U; ++channel) {
      for (std::size_t cell = 0U; cell < cells; ++cell) {
        reconstructed.push_back(effectiveBlack(descriptor, channel, cell));
      }
    }
    for (const std::uint32_t expected : patterned.blackPattern) {
      check(std::find(reconstructed.begin(), reconstructed.end(), expected) !=
                reconstructed.end(),
            "per-cell BlackLevel retains code " + std::to_string(expected));
    }
  }

  const std::vector<std::uint8_t> malformed =
      sfraw::hosttest::makeMalformedPrecisionLevelsDng();
  const spectrafilm::DecodeResult malformedResult =
      spectrafilm::decodeFromBuffer(malformed.data(), malformed.size(), {});
  check(!malformedResult.ok &&
            malformedResult.status == spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "WhiteLevel <= BlackLevel fails closed before processing",
        describe(malformedResult));

  sfraw::hosttest::PrecisionDngOptions malformedTagOptions;
  std::vector<std::uint8_t> malformedTag =
      sfraw::hosttest::makePrecisionUncompressedDng(malformedTagOptions);
  const std::size_t malformedIfd = 8U;
  const std::uint16_t malformedEntryCount = little16(malformedTag, malformedIfd);
  bool baselinePatched = false;
  for (std::uint16_t index = 0U; index < malformedEntryCount; ++index) {
    const std::size_t entry = malformedIfd + 2U + index * 12U;
    if (little16(malformedTag, entry) == 0xc62aU) {
      little16(&malformedTag, entry + 2U, 5U);  // RATIONAL, must be SRATIONAL.
      baselinePatched = true;
      break;
    }
  }
  const spectrafilm::DecodeResult malformedTagResult =
      spectrafilm::decodeFromBuffer(malformedTag.data(), malformedTag.size(), {});
  check(baselinePatched && !malformedTagResult.ok &&
            malformedTagResult.status ==
                spectrafilm::SFRAW_ERR_PRECISION_METADATA,
        "malformed HDR metadata type fails the bounded preflight",
        describe(malformedTagResult));

  const std::vector<std::uint8_t> floatDng =
      sfraw::hosttest::makeDistinctTileFloatDeflateDng();
  const spectrafilm::DecodeResult floatResult =
      spectrafilm::decodeFromBuffer(floatDng.data(), floatDng.size(), {});
  check(!floatResult.ok &&
            floatResult.status == spectrafilm::SFRAW_ERR_DEFLATE_DNG &&
            floatResult.error.find("quantize") != std::string::npos,
        "float DNG is explicitly routed instead of claiming native precision parity",
        describe(floatResult));

  bool overflowRejected = false;
  try {
    sfraw::hosttest::PrecisionDngOptions overflow;
    overflow.width = std::numeric_limits<std::uint32_t>::max();
    (void)sfraw::hosttest::makePrecisionUncompressedDng(overflow);
  } catch (const std::invalid_argument&) {
    overflowRejected = true;
  }
  check(overflowRejected,
        "precision fixture rejects pixel/stride overflow before allocation");

  sfraw::hosttest::PrecisionDngOptions cancellationOptions;
  const std::vector<std::uint8_t> cancellable =
      sfraw::hosttest::makePrecisionUncompressedDng(cancellationOptions);
  std::atomic<bool> cancelled{true};
  spectrafilm::DecodeOptions decodeOptions;
  decodeOptions.cancelFlag = &cancelled;
  const spectrafilm::DecodeResult cancelledResult =
      spectrafilm::decodeFromBuffer(cancellable.data(), cancellable.size(),
                                    decodeOptions);
  check(!cancelledResult.ok &&
            cancelledResult.status == spectrafilm::SFRAW_ERR_CANCELLED,
        "precision descriptor work obeys the pre-cancel budget",
        describe(cancelledResult));

  std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
            << " (" << failures << " failures)\n";
  return failures == 0 ? 0 : 1;
}
