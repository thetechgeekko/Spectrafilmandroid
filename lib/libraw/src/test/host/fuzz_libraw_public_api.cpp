/*
 * Spektrafilm Android -- LibRaw public-seam libFuzzer target (GPLv3).
 *
 * Adapted from google/oss-fuzz's official LibRaw harness:
 *   https://github.com/google/oss-fuzz/tree/master/projects/libraw
 * That harness establishes the important public seam and memory cap. This
 * variant keeps Spektrafilm's production order (open_buffer -> unpack ->
 * dcraw_process -> dcraw_make_mem_image) and adds explicit input/pixel limits.
 * The separate full-size CTest positive owns deterministic OpenMP-wavelet
 * coverage.
 */
#include <libraw/libraw.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

namespace {

constexpr std::size_t kMaxFuzzInputBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kMaxFuzzPixels = 12U * 1024U * 1024U;
constexpr unsigned kMaxRawMemoryMb = 128U;

bool exceedsPixelCap(unsigned width, unsigned height) {
  if (width == 0U || height == 0U) return false;
  return static_cast<std::uint64_t>(height) >
         kMaxFuzzPixels / static_cast<std::uint64_t>(width);
}

bool exceedsDecodeCap(const libraw_image_sizes_t& sizes) {
  const std::uint64_t allocationWidth = std::max<std::uint64_t>(
      sizes.raw_width,
      static_cast<std::uint64_t>(sizes.width) + sizes.left_margin);
  const std::uint64_t allocationHeight = std::max<std::uint64_t>(
      sizes.raw_height,
      static_cast<std::uint64_t>(sizes.height) + sizes.top_margin);
  if (exceedsPixelCap(sizes.raw_width, sizes.raw_height) ||
      exceedsPixelCap(sizes.width, sizes.height) ||
      exceedsPixelCap(static_cast<unsigned>(allocationWidth),
                      static_cast<unsigned>(allocationHeight))) {
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
  return exceedsPixelCap(static_cast<unsigned>(projectedWidth),
                         static_cast<unsigned>(projectedHeight));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (data == nullptr || size == 0U || size > kMaxFuzzInputBytes) return 0;

  LibRaw raw;
  raw.imgdata.rawparams.max_raw_memory_mb = kMaxRawMemoryMb;
  raw.imgdata.params.output_bps = 8;
  raw.imgdata.params.output_color = 1;
  raw.imgdata.params.no_auto_bright = 1;
  raw.imgdata.params.half_size = 1;
  raw.imgdata.params.user_qual = 0;
  // Threshold is derived without consuming bytes or changing the TIFF header,
  // varying postprocess behavior where half-size dimensions remain eligible.
  raw.imgdata.params.threshold = (data[size - 1U] & 1U) != 0U ? 8.0F : 0.0F;

  if (raw.open_buffer(data, size) != LIBRAW_SUCCESS) return 0;
  if (exceedsDecodeCap(raw.imgdata.sizes)) {
    return 0;
  }
  if (raw.unpack() != LIBRAW_SUCCESS) return 0;
  if (exceedsDecodeCap(raw.imgdata.sizes)) {
    return 0;
  }
  // Wavelet denoise is O(pixels) work per level and legitimately runs for tens
  // of seconds on multi-megapixel mutants under ASan + coverage + OpenMP, which
  // the 10 s libFuzzer alarm then misreports as a hang (observed in
  // LibRaw::wavelet_denoise from a 12 MP-capped mutant). Keep the denoise path
  // exercised on small images so the alarm keeps measuring hangs, not honest
  // postprocessing throughput. Production never reaches it (threshold is
  // pinned to 0.0f in raw_decoder.cpp).
  constexpr std::uint64_t kMaxDenoisePixels = 1ULL << 20;
  if (raw.imgdata.params.threshold > 0.0F &&
      static_cast<std::uint64_t>(raw.imgdata.sizes.width) *
              static_cast<std::uint64_t>(raw.imgdata.sizes.height) >
          kMaxDenoisePixels) {
    raw.imgdata.params.threshold = 0.0F;
  }
  if (raw.dcraw_process() != LIBRAW_SUCCESS) return 0;
  int status = LIBRAW_SUCCESS;
  libraw_processed_image_t* image = raw.dcraw_make_mem_image(&status);
  if (image != nullptr) LibRaw::dcraw_clear_mem(image);
  return 0;
}
