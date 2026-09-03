/*
 * Spektrafilm Android -- production RAW decoder resource-limit regressions.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 */
#include "public_api_test_support.h"
#include "raw_decoder.h"

#include <libraw/libraw.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sfraw::test {
using LibRawProgressObserver = void (*)(void*);
void setLibRawProgressObserverForTest(LibRawProgressObserver observer,
                                      void* context) noexcept;
}  // namespace sfraw::test

namespace {

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

}  // namespace

int main() {
  spectrafilm::DecodeOptions options;

  std::atomic<bool> preCancelled{true};
  options.cancelFlag = &preCancelled;
  const std::uint8_t cancelledSentinel = 0U;
  const spectrafilm::DecodeResult cancelled = spectrafilm::decodeFromBuffer(
      &cancelledSentinel, 1U, options);
  check(!cancelled.ok &&
            cancelled.status == spectrafilm::SFRAW_ERR_CANCELLED,
        "pre-cancelled decode returns the stable cancellation status",
        describe(cancelled));
  options.cancelFlag = nullptr;

  const std::vector<std::uint8_t> cancellableInput =
      sfraw::hosttest::makeValidUncompressedDng(2048U, 2048U);
  std::atomic<bool> cancelDuringDecode{false};
  struct ProgressBarrier {
    std::mutex mutex;
    std::condition_variable enteredCondition;
    std::condition_variable releaseCondition;
    bool entered = false;
    bool release = false;
  } progressBarrier;
  auto blockAtLibRawProgress = [](void* opaque) {
    auto* barrier = static_cast<ProgressBarrier*>(opaque);
    std::unique_lock<std::mutex> lock(barrier->mutex);
    barrier->entered = true;
    barrier->enteredCondition.notify_one();
    barrier->releaseCondition.wait(lock, [&] { return barrier->release; });
  };
  sfraw::test::setLibRawProgressObserverForTest(blockAtLibRawProgress,
                                                &progressBarrier);
  spectrafilm::DecodeResult cancelledDuringDecode;
  options.cancelFlag = &cancelDuringDecode;
  std::thread decodeWorker([&] {
    cancelledDuringDecode = spectrafilm::decodeFromBuffer(
        cancellableInput.data(), cancellableInput.size(), options);
  });
  bool enteredProgress = false;
  {
    std::unique_lock<std::mutex> lock(progressBarrier.mutex);
    enteredProgress = progressBarrier.enteredCondition.wait_for(
        lock, std::chrono::seconds(5), [&] { return progressBarrier.entered; });
  }
  cancelDuringDecode.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(progressBarrier.mutex);
    progressBarrier.release = true;
  }
  progressBarrier.releaseCondition.notify_one();
  decodeWorker.join();
  sfraw::test::setLibRawProgressObserverForTest(nullptr, nullptr);
  check(enteredProgress && !cancelledDuringDecode.ok &&
            cancelledDuringDecode.status == spectrafilm::SFRAW_ERR_CANCELLED &&
            cancelledDuringDecode.librawCode == LIBRAW_CANCELLED_BY_CALLBACK,
        "barrier-proven in-flight decode is interrupted through LibRaw's progress callback",
        describe(cancelledDuringDecode));
  options.cancelFlag = nullptr;

  constexpr std::size_t kEncodedInputLimit = 64U * 1024U * 1024U;
  const std::uint8_t sentinel = 0U;
  const spectrafilm::DecodeResult oversizedBufferResult =
      spectrafilm::decodeFromBuffer(&sentinel, kEncodedInputLimit + 1U,
                                    options);
  check(!oversizedBufferResult.ok &&
            oversizedBufferResult.status == spectrafilm::SFRAW_ERR_INPUT,
        "buffer input above 64 MiB is rejected before dereference",
        describe(oversizedBufferResult));

  FILE* oversizedFile = std::tmpfile();
  check(oversizedFile != nullptr,
        "temporary fd fixture for encoded-input ceiling is created");
  if (oversizedFile != nullptr) {
    const int oversizedFd = ::fileno(oversizedFile);
    const int truncateStatus =
        ::ftruncate(oversizedFd, static_cast<off_t>(kEncodedInputLimit + 1U));
    check(truncateStatus == 0,
          "sparse fd fixture is sized one byte above 64 MiB");
    if (truncateStatus == 0) {
      const spectrafilm::DecodeResult oversizedFdResult =
          spectrafilm::decodeFromFd(oversizedFd, options);
      check(!oversizedFdResult.ok &&
                oversizedFdResult.status == spectrafilm::SFRAW_ERR_INPUT,
            "fd input above 64 MiB is rejected without closing caller fd",
            describe(oversizedFdResult));
      check(::ftruncate(oversizedFd, 1) == 0,
            "decodeFromFd retains caller ownership after limit rejection");
    }
    std::fclose(oversizedFile);
  }

  const std::vector<std::uint8_t> adobeDeflate =
      sfraw::hosttest::makeAdobeDeflateDng();
  const spectrafilm::DecodeResult adobeDeflateResult =
      spectrafilm::decodeFromBuffer(adobeDeflate.data(), adobeDeflate.size(),
                                    options);
  check(!adobeDeflateResult.ok &&
            adobeDeflateResult.status == spectrafilm::SFRAW_ERR_DEFLATE_DNG,
        "0x80B2 open failure maps to typed DEFLATE fallback",
        describe(adobeDeflateResult));

  options.maxLongEdge = 500;
  const std::vector<std::uint8_t> oddLongEdge =
      sfraw::hosttest::makeValidUncompressedDng(1001U, 22U);
  const spectrafilm::DecodeResult oddLongEdgeResult =
      spectrafilm::decodeFromBuffer(oddLongEdge.data(), oddLongEdge.size(),
                                    options);
  check(oddLongEdgeResult.ok && oddLongEdgeResult.width <= 500 &&
            oddLongEdgeResult.height <= 500,
        "odd decoded dimensions obey the exact maxLongEdge ceiling",
        "width=" + std::to_string(oddLongEdgeResult.width) +
            " height=" + std::to_string(oddLongEdgeResult.height) + " " +
            describe(oddLongEdgeResult));
  options.maxLongEdge = 0;

  // 4096 x 3073 is one row above the 12 MiPixel 64-bit ceiling.
  const std::vector<std::uint8_t> fullOversize =
      sfraw::hosttest::makeDeclaredOversizeUncompressedDng(4096U, 3073U);
  const spectrafilm::DecodeResult fullResult = spectrafilm::decodeFromBuffer(
      fullOversize.data(), fullOversize.size(), options);
  check(!fullResult.ok && fullResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY,
        "full-resolution metadata above 12 MiPixels fails before unpack",
        describe(fullResult));

  // Some non-Bayer/linear DNGs ignore half_size, so the metadata ceiling must
  // stay fail-closed even when callers request a proxy.
  options.halfSize = true;
  const std::vector<std::uint8_t> halfOversize =
      sfraw::hosttest::makeDeclaredOversizeUncompressedDng(4096U, 3073U);
  const spectrafilm::DecodeResult halfResult = spectrafilm::decodeFromBuffer(
      halfOversize.data(), halfOversize.size(), options);
  check(!halfResult.ok && halfResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY,
        "half-size metadata above 12 MiPixels still fails before unpack",
        describe(halfResult));

  const std::vector<std::uint8_t> extremeAspect =
      sfraw::hosttest::makeExtremeAspectDng();
  options.halfSize = false;
  const spectrafilm::DecodeResult extremeAspectResult =
      spectrafilm::decodeFromBuffer(extremeAspect.data(), extremeAspect.size(),
                                    options);
  check(!extremeAspectResult.ok &&
            extremeAspectResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY,
        "DefaultScale cannot expand a 12 MiPixel frame beyond the ceiling",
        describe(extremeAspectResult));

  for (const bool belowOne : {true, false}) {
    const std::vector<std::uint8_t> boundaryAspect =
        sfraw::hosttest::makeBoundaryAspectDng(belowOne);
    const spectrafilm::DecodeResult boundaryAspectResult =
        spectrafilm::decodeFromBuffer(boundaryAspect.data(),
                                      boundaryAspect.size(), options);
    check(!boundaryAspectResult.ok &&
              boundaryAspectResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY,
          belowOne
              ? "pixel aspect below 0.995 cannot cross the decode ceiling"
              : "pixel aspect above 1.005 cannot cross the decode ceiling",
          describe(boundaryAspectResult));
  }

  const std::vector<std::uint8_t> hostileActiveArea =
      sfraw::hosttest::makeHostileActiveAreaDng();
  const spectrafilm::DecodeResult hostileActiveAreaResult =
      spectrafilm::decodeFromBuffer(hostileActiveArea.data(),
                                    hostileActiveArea.size(), options);
  check(!hostileActiveAreaResult.ok &&
            hostileActiveAreaResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY,
        "ActiveArea margins cannot amplify pre-unpack raw-store geometry",
        describe(hostileActiveAreaResult));

  const std::vector<std::uint8_t> oversizedLjpeg =
      sfraw::hosttest::makeOversizedLjpegGeometryTiff();
  const spectrafilm::DecodeResult oversizedLjpegResult =
      spectrafilm::decodeFromBuffer(oversizedLjpeg.data(),
                                    oversizedLjpeg.size(), options);
  check(!oversizedLjpegResult.ok &&
            oversizedLjpegResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY &&
            oversizedLjpegResult.librawCode == LIBRAW_TOO_BIG,
        "lossless-JPEG work-budget failure maps to stable NO_MEMORY status",
        describe(oversizedLjpegResult));

  const std::vector<std::uint8_t> oversizedLjpegDng =
      sfraw::hosttest::makeOversizedLjpegGeometryDng();
  const spectrafilm::DecodeResult oversizedLjpegDngResult =
      spectrafilm::decodeFromBuffer(oversizedLjpegDng.data(),
                                    oversizedLjpegDng.size(), options);
  check(!oversizedLjpegDngResult.ok &&
            oversizedLjpegDngResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY &&
            oversizedLjpegDngResult.librawCode == LIBRAW_TOO_BIG,
        "lossless-DNG work-budget failure maps to stable NO_MEMORY status",
        describe(oversizedLjpegDngResult));

  const std::vector<std::uint8_t> excessiveTiles =
      sfraw::hosttest::makeExcessiveTileCountLosslessJpegDng();
  const spectrafilm::DecodeResult excessiveTilesResult =
      spectrafilm::decodeFromBuffer(excessiveTiles.data(),
                                    excessiveTiles.size(), options);
  check(!excessiveTilesResult.ok &&
            excessiveTilesResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY &&
            excessiveTilesResult.librawCode == LIBRAW_TOO_BIG,
        "DNG tile-count budget maps to stable NO_MEMORY status",
        describe(excessiveTilesResult));

  const std::vector<std::uint8_t> excessivePackedTiles =
      sfraw::hosttest::makeExcessiveTileCountUncompressedDng();
  const spectrafilm::DecodeResult excessivePackedTilesResult =
      spectrafilm::decodeFromBuffer(excessivePackedTiles.data(),
                                    excessivePackedTiles.size(), options);
  check(!excessivePackedTilesResult.ok &&
            excessivePackedTilesResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY &&
            excessivePackedTilesResult.librawCode == LIBRAW_TOO_BIG,
        "packed-DNG tile-count budget maps to stable NO_MEMORY status",
        describe(excessivePackedTilesResult));

  const std::vector<std::uint8_t> excessiveSonyTiles =
      sfraw::hosttest::makeExcessiveSonyTileCountTiff();
  const spectrafilm::DecodeResult excessiveSonyTilesResult =
      spectrafilm::decodeFromBuffer(excessiveSonyTiles.data(),
                                    excessiveSonyTiles.size(), options);
  check(!excessiveSonyTilesResult.ok &&
            excessiveSonyTilesResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY &&
            excessiveSonyTilesResult.librawCode == LIBRAW_TOO_BIG,
        "Sony tile-count budget maps to stable NO_MEMORY status",
        describe(excessiveSonyTilesResult));

  // Prove that the native raw-store budget itself is causal, independent of
  // the wrapper's earlier dimension guard. A 12 MiPixel CFA needs more than a
  // deliberately tiny 16 MiB raw store, so LibRaw must reject before allocating.
  {
    LibRaw raw;
    raw.imgdata.rawparams.max_raw_memory_mb = 16U;
    const std::vector<std::uint8_t> rawStoreOverBudget =
        sfraw::hosttest::makeDeclaredOversizeUncompressedDng(4096U, 3072U);
    const int open = raw.open_buffer(rawStoreOverBudget.data(),
                                     rawStoreOverBudget.size());
    const int unpack = open == LIBRAW_SUCCESS ? raw.unpack() : open;
    check(open == LIBRAW_SUCCESS && unpack == LIBRAW_TOO_BIG,
          "LibRaw rejects a declared raw store above its configured budget",
          "open=" + std::to_string(open) +
              " unpack=" + std::to_string(unpack));
  }
  {
    const std::vector<std::uint8_t> overBudget =
        sfraw::hosttest::makeOverBudgetMetadataDng();
    const spectrafilm::DecodeResult overBudgetResult =
        spectrafilm::decodeFromBuffer(overBudget.data(), overBudget.size(),
                                      options);
    check(!overBudgetResult.ok &&
              overBudgetResult.status == spectrafilm::SFRAW_ERR_NO_MEMORY &&
              overBudgetResult.librawCode == LIBRAW_UNSUFFICIENT_MEMORY,
          "identify-time metadata budget maps to stable NO_MEMORY status",
          describe(overBudgetResult));
  }

  // The limit acts during LibRaw::open_buffer(), before the wrapper's metadata
  // guard. A small profile proves the fixture is recognized; a payload exactly
  // at the intended 16 MiB ceiling must not be allocated.
  {
    LibRaw raw;
    const std::vector<std::uint8_t> smallProfile =
        sfraw::hosttest::makeEmbeddedProfileDng(1024U);
    const int open = raw.open_buffer(smallProfile.data(), smallProfile.size());
    check(open == LIBRAW_SUCCESS && raw.imgdata.color.profile != nullptr,
          "embedded profile control is recognized below the ceiling",
          "open=" + std::to_string(open));
  }
  {
    LibRaw raw;
    const std::vector<std::uint8_t> validMultiStrip =
        sfraw::hosttest::makeValidMultiStripDng();
    const int open =
        raw.open_buffer(validMultiStrip.data(), validMultiStrip.size());
    check(open == LIBRAW_SUCCESS,
          "one valid multi-strip table remains accepted",
          "open=" + std::to_string(open));
  }
  {
    LibRaw raw;
    const std::vector<std::uint8_t> duplicateStripTables =
        sfraw::hosttest::makeDuplicateStripTablesDng();
    const int open = raw.open_buffer(duplicateStripTables.data(),
                                     duplicateStripTables.size());
    check(open != LIBRAW_SUCCESS,
          "duplicate allocation-bearing strip tables are rejected",
          "open=" + std::to_string(open));
  }
  {
    LibRaw raw;
    const std::vector<std::uint8_t> duplicateOpcode =
        sfraw::hosttest::makeDuplicateOpcodeDng();
    const int open =
        raw.open_buffer(duplicateOpcode.data(), duplicateOpcode.size());
    check(open != LIBRAW_SUCCESS,
          "duplicate allocation-bearing OpcodeList tag is rejected",
          "open=" + std::to_string(open));
  }
  {
    LibRaw raw;
    const std::vector<std::uint8_t> overBudget =
        sfraw::hosttest::makeOverBudgetMetadataDng();
    const int open = raw.open_buffer(overBudget.data(), overBudget.size());
    check(open != LIBRAW_SUCCESS,
          "aggregate TIFF metadata above 16 MiB is rejected",
          "open=" + std::to_string(open));
  }
  {
    LibRaw raw;
    const std::vector<std::uint8_t> limitProfile =
        sfraw::hosttest::makeEmbeddedProfileDng(16U * 1024U * 1024U);
    const int open = raw.open_buffer(limitProfile.data(), limitProfile.size());
    check(open == LIBRAW_SUCCESS && raw.imgdata.color.profile == nullptr,
          "embedded profile at 16 MiB is not allocated during open",
          "open=" + std::to_string(open));
  }

  std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
            << " (" << failures << " failures)\n";
  return failures == 0 ? 0 : 1;
}
