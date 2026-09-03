# LibRaw hostile-input corpus

`tiff-newsubfiletype-float-overflow-844.hex` is the exact 64-byte reporter input
from [LibRaw issue #844](https://github.com/LibRaw/LibRaw/issues/844). Its bytes
decode from:

```text
SUkqAAgAAAABAP4ABAABAAAAAQAA4wcbAAAAAAAAAAAAAAAXAAAAAAAAAAAAABgAAAAAAAAAAAAAAAAAAAAAAA==
```

The host test verifies both its fixed 64-byte length and clean `open_buffer`
rejection under UBSan. The binary SHA-256 is
`59167be5a9b4c8bdae9b926be2e91c664e8aee207b78971dcadb057eab18aa4c`.

Lossless-JPEG DNG/TIFF fixtures are generated deterministically by
`host/public_api_test_support.cpp` and `host/test_libraw_public_api.cpp`. Keeping
the generator beside the public-seam test makes strip lengths and truncation
points reviewable. Two negative fixtures have complete SOF3/DHT/SOS headers so
`open_buffer` succeeds and `unpack` reaches the decoder; one has truncated
entropy and one decodes an invalid 17-bit difference category. A third complete
category-zero generic TIFF combines a 4096 x 256 JPEG with hostile CR2Slice metadata
over a 256 x 256 raw plane. It reaches processing under ASan/UBSan and proves
LibRaw 0.22.2 bounds the CVE-2026-21413 column store.

The fuzz/public-seam harness enforces a 16 MiB operational input cap, a 128 MiB
LibRaw raw-store cap, and a 12 MiPixel declared/adjusted/stretched dimension cap.
The production wrapper separately enforces 64 MiB encoded input and a 12 MiPixel
64-bit / 8 MiPixel 32-bit policy. This directory intentionally contains no
proprietary camera RAW files.

The source tree stores the #844 bytes as reviewable hex. When the fuzzer target
is built, `host/write_fuzz_seeds.cpp` writes 82 deterministic binary TIFF/DNG/X3F
seeds under `<build>/fuzz-seeds`. They include positive decode controls and
hostile allocation, geometry, LJPEG, Sony/Samsung shift, Hasselblad predictor,
Olympus metadata/arithmetic, Panasonic C8 table/stripe/bit-budget routes,
fixed-header string boundaries, and dormant X3F model probes.
libFuzzer never needs to write mutations into this source directory.
