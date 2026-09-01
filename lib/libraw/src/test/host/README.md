# LibRaw host public-seam regressions

This standalone CMake project builds the exact LibRaw source selected by
`lib/libraw/cmake/LibRawVendor.cmake`, rather than a system copy. Its normal test
drives hostile bytes only through LibRaw's supported public sequence:
`open_buffer` -> `unpack` -> `dcraw_process`.

The default build instruments both LibRaw and the tests with AddressSanitizer
and UndefinedBehaviorSanitizer. It caps each input at 16 MiB, LibRaw raw-memory
allocations at 128 MiB, and declared/adjusted/stretched frames at 12 MiPixels.
The regression covers
the exact 64-byte input from upstream issue #844 plus project-owned
lossless-JPEG DNGs whose valid SOF3/DHT/SOS headers cover truncated entropy, an
invalid difference category, and a complete category-zero TIFF with hostile
CR2Slice geometry (CVE-2026-21413). A positive uncompressed DNG control reaches
all three public stages (and the wavelet path when OpenMP is available). A
compile-time and runtime assertion prevents a 0.21.x library from satisfying
the test. The production-wrapper CTest also exercises the 64 MiB encoded-input,
128 MiB raw-store, 12/8 MiPixel, ActiveArea, DefaultScale, ICC, and cumulative
metadata guards with positive controls.

The same CTest project runs `sfraw.raw-wb.cat02-exact-bits`. Its fixture header
is generated in the build directory from
`tools/parity/fixtures/raw_wb_cat_vectors.json` only after the canonical research
digest passes. The executable calls production `raw_decoder.cpp` math for all
56 scenario/patch combinations, compares exact ACES-after-WB and float32
ProPhoto bits, and covers cast-order/skip diagnostics plus invalid native
temperature and tint values. No generated header is committed.

Typical Clang build:

```sh
cmake -S lib/libraw/src/test/host -B build/libraw-host \
  -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DSFRAW_LIBRAW_SOURCE_DIR=/path/to/already-verified-and-patched/LibRaw-0.22.2
cmake --build build/libraw-host --parallel
ctest --test-dir build/libraw-host --output-on-failure
```

The source override is intentionally subject to the shared vendor resolver's
verification policy. For the normal fetched/archive path, omit it.

For the dedicated OpenMP-wavelet gate, install a host OpenMP runtime and add
`-DSFRAW_HOST_REQUIRE_OPENMP=ON`. Configuration fails instead of silently
falling back to serial code. The positive 256 x 256 DNG runs full-size with a
non-zero threshold, asserts LibRaw's processing dimensions stay above its
65-pixel early-return guard, and then completes `dcraw_process`.

To link the libFuzzer-compatible entry point (Clang only):

```sh
cmake -S lib/libraw/src/test/host -B build/libraw-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DSFRAW_HOST_BUILD_FUZZER=ON
cmake --build build/libraw-fuzz --target sfraw_libraw_fuzzer --parallel
cmake -E copy_directory build/libraw-fuzz/fuzz-seeds \
  build/libraw-fuzz/fuzz-corpus
build/libraw-fuzz/sfraw_libraw_fuzzer \
  -max_len=16777216 -rss_limit_mb=1024 -timeout=10 -max_total_time=60 \
  build/libraw-fuzz/fuzz-corpus
```

The harness instruments both its entry point and LibRaw itself for coverage. It
is adapted from the official
[OSS-Fuzz LibRaw project](https://github.com/google/oss-fuzz/tree/master/projects/libraw)
and preserves its public-API and raw-memory-boundary strategy.

Building `sfraw_libraw_fuzzer` first generates 82 deterministic binary seeds
under the build directory. In addition to the original #844, CR2Slice, LJPEG,
metadata, scale, geometry, and work-budget routes, the corpus contains exact
positive and hostile controls for Sony/Samsung shifts, Canon sRAW, Hasselblad,
Olympus 14-bit decoding, Panasonic C8 tables/stripes/bit budgets/signed
predictors, fixed identify and MakerNote buffers, and tail-boundary X3F model
probes. The host dependency deliberately enables X3FTOOLS under sanitizers even
though Android currently compiles its stub, qualifying these boundary fixes.
The full X3F parser still requires a dedicated audit before Android enables it.
Copy those seeds
to a disposable build-directory corpus before fuzzing; libFuzzer writes reduced
and newly discovered inputs into the corpus it receives.

Run both sanitizer configurations: OpenMP off mirrors the shipping Android
decoder, while OpenMP required covers patched parallel branches. The C8 decoder
keeps Panasonic's ordered first-match semantics (needed by S5M2) and deliberately
rejects an unmatched table index instead of using upstream's zero fallback.
