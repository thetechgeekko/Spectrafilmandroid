# lib:libraw

<!-- libraw-license-route: UNRESOLVED -->

LibRaw Android distribution route: UNRESOLVED.

On-device camera **RAW / DNG** decoding for Spektrafilm for Android, producing a
**linear, scene-referred float32 RGB** buffer with the same processing settings
as Spektrafilm's desktop `rawpy` path. Exact decoder-version parity is tracked
explicitly; it is not inferred from matching option names.

> **Status: security-upgraded, qualification in progress.** The native decoder,
> JNI bridge, and Kotlin facade are in place. The build fetches the official
> LibRaw 0.22.2 archive by SHA-256, applies a hashed local hardening series, and
> fails closed if any source/version/patch check fails. RAW/DNG decode is live;
> final ABI/device qualification is rerun whenever the patch aggregate changes.
> `build.gradle.kts` mirrors
> `engine:spektra-core` (plain AGP `com.android.library` + `kotlin.android` +
> `externalNativeBuild` CMake), so the module configures and builds standalone the
> moment it is added to `settings.gradle.kts`.

## What it does

`rawpy` is a thin Python binding over **LibRaw**, so "RAW like spektrafilm" =
"LibRaw with the same `postprocess` options". This module compiles LibRaw with the
NDK and calls it from a JNI wrapper using the identical settings, then applies the
same white-balance colour science spektrafilm uses (see
`spektrafilm/utils/raw_file_processor.py`).

Output is interleaved RGB **float32**, row-major, normalized from LibRaw's
16-bit linear ACES intermediate and converted to **linear ProPhoto RGB** before
it crosses JNI. It is delivered as a direct `ByteBuffer` with no 8-bit round-trip.

Every successful native result also carries `RawPrecisionDescriptor` v1. It keeps
these contracts separate instead of collapsing them into one “bit depth”:

- **Declared source storage:** integer `BitsPerSample` (or bounded decoder metadata),
  sample format, TIFF byte order/packing/compression, and CFA/layout.
- **Effective sensor/code range:** exact common + per-channel + repeating-cell
  BlackLevel model, per-channel WhiteLevel, provenance, and the distinct-value
  precision between them. Zero is a real code value; absence is represented separately.
- **Interchange/compute:** one float32 RGB ownership boundary. Integer samples up
  to 16 bits are exactly representable before adopted LibRaw/CPU mixed-precision
  demosaic and colour arithmetic. This is not a claim that the whole CPU stack is f64.
- **Downstream/output:** the engine's f32/Vulkan working precision, PNG/TIFF/JPEG
  output depth, and HDR transfer/gain-map metadata are independent contracts; none
  can be inferred from sensor bits or the descriptor's linear float32 buffer.

The JNI carrier is fixed-size and versioned (136 bounded integer words + two
floats), validated during `NativeResult` construction before publication commits.
Contradictory levels, unsupported CFA shapes, or ambiguous precision fail with
`PRECISION_METADATA`. Platform codec fallback uses
`RawPrecisionDescriptor.forPlatformDisplayReferredFallback(...)`; that factory
cannot label its output `LIBRAW_NATIVE` or scene-linear parity.

### Native ownership and cancellation

Each native decode returns an owning `LinearResult`. Call `close()` (normally
with Kotlin `use`) after the last consumer finishes. Close is atomic and
idempotent; the JNI registry releases only an exact allocation-token, base, and
capacity match, so foreign buffers, slices, stale tokens, repeated close, and
concurrent close cannot reach `free(3)`. The legacy buffer-only `freeOffHeap`
entry point is intentionally fail-closed because a `ByteBuffer` alone cannot
prove ownership.

Pixel access is lease-only. `withDataLease` gives every reader an independent,
native-order view of the constructor's captured position/limit window and
defers release until the reader returns. `acquireDataLease` supports an explicit
ownership hand-off: the app closes the consumed `LinearResult` immediately and
transfers its still-active lease to the export-scale `LinearImage`, whose
`close()` finally releases the native allocation. Proxy and Coil paths copy
inside a lease and close the result before returning.

`RawDecoder.newCancellation()` creates an `AutoCloseable` cooperative
cancellation generation that can be supplied to any decode overload. The
bounded stream/fd readers and first-party copy, white-balance, and colour loops
poll it at bounded intervals. The decoder also installs LibRaw 0.22.2's progress
handler before opening the input, so long `open_buffer`, `unpack`,
`dcraw_process`, and `dcraw_make_mem_image` phases return
`LIBRAW_CANCELLED_BY_CALLBACK` and map to the stable `CANCELLED` Kotlin status.
Checks immediately before and after every phase cover paths where LibRaw emits
no progress callback. Closing the generation also cancels any native lease
already in flight.

## Layout

```
lib/libraw/
├── build.gradle.kts                 # convention plugins + externalNativeBuild(CMake); 3 ABIs
├── cmake/LibRawVendor.cmake         # archive/hash/version/patch fail-closed resolver
├── patches/                         # ordered, hashed, reviewable 0.22.2 hardening series
├── src/main/AndroidManifest.xml     # minimal (no components)
└── src/main/
    ├── cpp/
    │   ├── CMakeLists.txt           # builds libsfraw.so from the verified resolver
    │   ├── raw_decoder.h            # decode API + WB math contract
    │   ├── raw_decoder.cpp          # LibRaw params + Von-Kries adaptation (guarded)
    │   └── raw_decoder_jni.cpp      # JNI bridge -> direct float ByteBuffer + w/h/cs
    │       # (LibRaw sources are fetched at configure time, not committed)
    └── kotlin/com/spectrafilm/libraw/
        ├── RawDecoder.kt            # facade: decodeToLinear(bytes/buffer/fd/stream)
        └── RawCoilDecoder.kt        # Coil 3 Decoder.Factory (gallery full-res open)
```

## Vendoring LibRaw

LibRaw is **not** committed. `cmake/LibRawVendor.cmake` fetches the official
archive through CMake **`FetchContent`** and pins immutable constants:

```cmake
SFRAW_PINNED_LIBRAW_VERSION = "0.22.2"
SFRAW_PINNED_LIBRAW_URL     = "https://www.libraw.org/data/LibRaw-0.22.2.tar.gz"
SFRAW_PINNED_LIBRAW_SHA256  = "de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa"
```

A clean checkout therefore builds with a working network and no git submodule or
committed upstream blob. The version/URL/hash are not cache variables, so an old
CMake cache cannot redirect the build to 0.21.4. A local
`-DSFRAW_LIBRAW_SOURCE_DIR=<dir>` override is accepted only for Debug/offline
verification; shipping configurations must use the hashed official archive.
Every build applies and verifies `patches/` idempotently, asserts exact 0.22.2
headers and security guards, and refuses to build a runtime decoder stub.

`CMakeLists.txt` then sorts LibRaw's `src/**/*.cpp` (notably **excluding the
`*_ph.cpp` placeholder TUs**,
which are postprocessing-free stubs that would otherwise shadow the real
`dcraw_process` / `dcraw_make_mem_image`), builds a static `raw` lib (with
`LIBRAW_CALLOC_RAWSTORE/NO_JASPER/NO_JPEG/USE_ZLIB`; LCMS stays disabled because
no `USE_LCMS*` define is supplied), generates the exact
source manifest, and links it into `libsfraw.so`. See
[`docs/dependencies/LIBRAW.md`](../../docs/dependencies/LIBRAW.md) for provenance,
patch hashes, build flags, corpus deltas, OpenMP disposition, and sanitizer evidence.

> **DNG SDK add-on:** there is no current adoption commitment. Evaluate it only after the canonical
> per-format baseline under the numeric, coverage, security, licensing, APK-size, and maintenance
> gates in [`docs/BIT_IDENTICAL_EXPORT_ROADMAP.md`](../../docs/BIT_IDENTICAL_EXPORT_ROADMAP.md).
> Baseline DNG + CR2/CR3/NEF/ARW/RAF/ORF/RW2 support does not depend on it.

## Mobile / Google Pixel DNG decode (native vs fallback)

Mobile DNG layouts vary by device and camera mode. Support is decided from the
selected full-resolution raw plane, not from the file extension or preview:

| Compression tag        | Decodes here? | How                                                   |
|------------------------|---------------|-------------------------------------------------------|
| 1 — uncompressed       | ✅ native     | plain unpack                                          |
| 7 — lossless JPEG/LJ92 | ✅ native     | LibRaw **internal** lossless-JPEG (`lossless_jpeg_load_raw` / `ljpeg_start` / `ljpeg_row`, `src/decoders/decoders_dcraw.cpp`) — **no libjpeg required** |
| 8 — DEFLATE, float (`SampleFormat=3`) | ❌ precision fallback | LibRaw's bitmap handoff would quantize float input before f32; fails closed as `DEFLATE_DNG` |
| 8 — DEFLATE, integer/linear | ❌ fallback | pinned decoder rejects safely → `DEFLATE_DNG` |
| 0x80B2 — Adobe deflate | ❌ fallback | no qualified 0.22.2 route → `DEFLATE_DNG` |
| 6 — old-style JPEG     | ❌ fallback   | needs libjpeg → `LOSSY_JPEG_DNG`                      |
| 0x884C — lossy JPEG    | ❌ fallback   | needs libjpeg → `LOSSY_JPEG_DNG`                      |
| 0xCD42 — JPEG-XL       | ❌ fallback   | needs libjxl/dngsdk → `JPEGXL_DNG`                    |

**Key point:** `USE_JPEG` is intentionally OFF and is not needed for a DNG whose
raw plane uses lossless JPEG/LJ92. It only adds *lossy* baseline-JPEG decode and embedded
JPEG thumbnails; LibRaw's lossless-JPEG (LJ92) raw decoder is compiled
unconditionally. A Pixel/Samsung/other mobile DNG must still be classified from
its actual raw IFD; model-level blanket support is not claimed.

> Mobile DNGs commonly embed a large JPEG **preview** in IFD0 with the real
> Bayer/linear raw in a **SubIFD**. The `dngsniff` precision path walks IFD0,
> bounded SubIFDs, and the next-IFD chain, keeps metadata candidate-local, and
> requires exactly one non-reduced RAW IFD whose width/height match LibRaw's
> identified and finalized raw geometry. Zero or multiple matches fail with
> `PRECISION_METADATA`; the largest-plane heuristic remains diagnostic-only.
> Only root `DNGVersion` is container-inherited; any valid or malformed child
> declaration rejects before dependency open, including when IFD0 is ordinary
> TIFF. Bits/sample format, CFA,
> BlackLevel/WhiteLevel, BaselineExposure, and LinearResponseLimit never inherit
> from a preview, sibling, or parent IFD. The unpack-failure classifier only flags genuinely-unsupported
> codecs/precision routes (all Compression 8 or 0x80B2 → `DEFLATE_DNG`, 6 / 0x884C →
> `LOSSY_JPEG_DNG`, 0xCD42 → `JPEGXL_DNG`). A failed native uncompressed or
> LJ92 decode stays a genuine data error (`UNPACK`). Float
> deflate is parsed by the audited LibRaw build for security coverage but is
> deliberately stopped before its quantizing memory-bitmap output.

The same bounded TIFF walk runs before `LibRaw::open_buffer`. Malformed external
precision payloads (including BlackLevel offsets/counts) are therefore rejected
as `PRECISION_METADATA` before they can reach dependency metadata arithmetic.
This gate covers every viable non-reduced CFA/LinearRaw candidate, including a
smaller plane LibRaw might inspect but not finally select; final geometry binding
is a separate post-open decision. BlackLevel conversion safety and
`BlackLevelDeltaH/V` presence are dependency-wide across every bounded next-IFD
and SubIFD, including reduced and malformed-eligibility children, on both buffer
and fd routes. The walk tracks ten distinct IFD offsets (LibRaw's own IFD storage
cap), follows next links in either direction, and fails closed on repeated,
malformed, over-capacity, out-of-bounds, or otherwise incomplete SubIFD graphs.
Even an `II`/`MM` header with wrong TIFF magic is stopped before dependency parser
dispatch. Declared integer precision is admitted only in
the 8-through-16-bit range.

Every successful native result also carries a bounded v1 precision descriptor:
declared/effective/processed precision, integer sample format, byte order and
packing, compression, explicit CFA rows/columns/pattern, exact
common/channel/repeating black levels,
per-channel white levels and provenance, BaselineExposure/LinearResponseLimit
presence, postprocess/linear-space route, and both requested and actually-applied
proxy reduction. A public fallback factory can create only a display-referred
descriptor; it cannot label platform pixels as native RAW parity.

For v1, CFA admission is deliberately narrow: LibRaw's filter state must prove a
true repeating Bayer 2×2 or X-Trans 6×6 pattern, `CFAPlaneColor` must be the RGB
identity mapping (its specified default), and `CFALayout` must be rectangular
(its specified default). Duplicate, malformed, remapped, or staggered selected-IFD
CFA tags fail before dependency parsing. The `filters == 1` custom table and any
non-repeating encoded filter period also fail closed.

LinearRaw accepts only a losslessly representable v1 subset: three or four
samples, uniform `BitsPerSample`/`SampleFormat` arrays whose count equals
`SamplesPerPixel`, a 1×1 `BlackLevelRepeatDim`, exactly one BlackLevel and one
WhiteLevel per sample channel when those tags are present, and no CFA tags.
Spatial row×column×sample black matrices require a later descriptor version.
Inline and offset arrays in both TIFF byte orders are covered. Every admitted
SHORT, LONG, or RATIONAL BlackLevel value is parsed before `LibRaw::open_buffer`
and bounded by the selected IFD's declared `BitsPerSample` maximum. Fractional,
zero-denominator, or out-of-declared-range rationals fail rather than being
rounded. `BlackLevelDeltaH/V` remains a typed unsupported spatial transform.
The project-owned LinearRaw fixtures include required Orientation and
UniqueCameraModel identity plus a valid paired-illuminant set of
ColorPlanes-by-3 SRATIONAL matrices (including ColorMatrix1). That pairing drives
LibRaw's post-identify field promotion instead of silently zeroing the fourth
matrix row, and the host gate perturbs each of three/four planes independently.
LinearRaw validation, effective precision, and public level lists use exactly its
active three or four sample planes. CFA retains four LibRaw level slots, including
the second-green slot when the mosaic has three colours.

A lightweight host unit test (`src/test/cpp/test_dng_sniffer.cpp`) compiles `raw_decoder.cpp`
with `-include` (no LibRaw needed — the decoder guards its LibRaw include) and
exercises the sniffer + classifier against synthesized uncompressed / LJ92 /
deflate / lossy / old-JPEG / JPEG-XL and Pixel-style preview+SubIFD headers.
31/31 assertions pass:

```
g++ -std=c++17 -I../../main/cpp -DUSE_ZLIB=1 \
    -include ../../main/cpp/raw_decoder.cpp \
    test_dng_sniffer.cpp -o /tmp/test_dng_sniffer && /tmp/test_dng_sniffer
```

The security gate is the separate `src/test/host` CMake project. It builds the
exact patched LibRaw source with Clang ASan/UBSan and exercises hostile TIFF and
lossless-JPEG inputs through `open_buffer -> unpack -> dcraw_process`; its
libFuzzer entry follows the official OSS-Fuzz public-seam strategy.

The physical-device JNI boundary gate is:

```powershell
.\gradlew.bat :lib:libraw:connectedDebugAndroidTest
```

This module deliberately uses `RawDecoderBoundaryInstrumentation`, a small custom
instrumentation runner, instead of exposing JUnit test cases. Android Gradle Plugin's
UTP summary therefore reports `0 tests`; that count is not the acceptance marker.
The generated `testlog/test-results.log` must contain both
`INSTRUMENTATION_RESULT: stream=OK (8 tests)` and `INSTRUMENTATION_CODE: -1`.
Any other stream, code, missing log, install failure, or zero-test summary without
those two exact instrumentation markers is a failed gate.

## Half-size (proxy) decode — memory and performance option

The current decoder is deliberately fail-closed before unpack: encoded input is
limited to 64 MiB, LibRaw's raw store to 128 MiB, and declared/ActiveArea/
DefaultScale geometry to 12 MiPixels on 64-bit or 8 MiPixels on 32-bit. Files
above that geometry require the seekable/tiled design tracked by ticket #173;
`halfSize` does not bypass this gate because some layouts ignore the request.

`RawDecoder.Settings` exposes a `halfSize: Boolean` flag (default `false`):

```kotlin
// Proxy decode — ~¼ the processed pixel count/output storage, usually faster.
val proxy: LinearResult = RawDecoder.decodeToLinear(
    fd,
    RawDecoder.Settings(halfSize = true),
)
// proxy.width  ≈ fullWidth  / 2
// proxy.height ≈ fullHeight / 2
```

| Option        | Value  | Effect                                                      |
|---------------|--------|-------------------------------------------------------------|
| `halfSize`    | false  | Full-resolution output within the in-memory safety limits.   |
| `halfSize`    | true   | LibRaw `imgdata.params.half_size = 1`; each 2×2 Bayer cell  |
|               |        | is averaged into one output pixel (no demosaic).            |

**How it works (LibRaw `half_size`):**
LibRaw's `half_size` parameter skips the full Bayer demosaic interpolation and
instead merges each **2×2 Bayer cell** (one R + two G + one B sample) into a
**single RGB output pixel** using a simple average.  Because the output is
half the linear dimensions in each axis, the total pixel count is **¼ of the
full-res decode**. LibRaw's raw sensor store can remain full-sized, so this is
not a blanket quarter-peak-memory guarantee. LibRaw updates
`imgdata.sizes` (specifically `S.width` / `S.height`) after `dcraw_process()`,
and `dcraw_make_mem_image` reports the post-process dimensions, so
`LinearResult.width * LinearResult.height * 3 == rgb.size()` is always satisfied.

**When to use:**
- Fast proxy preview of an admitted RAW on a low-RAM device.
- "Does this file decode?" health checks where full quality is not needed.
- App-side thumbnail generation before handing the full-res job to a background
  worker.

**When NOT to use:**
- Export (TIFF / PNG / Ultra HDR) — lower quality, wrong dimensions.
- Engine spectral film simulation — all processing must be at full resolution.

The app module decides when to request `halfSize = true`; the lib only exposes
the capability. Existing call sites default to `halfSize = false`, which requests
full-resolution output subject to the same safety limits.

## rawpy ↔ LibRaw parity

| `rawpy.postprocess`          | value                  | LibRaw (`imgdata.params`)        |
|------------------------------|------------------------|----------------------------------|
| `output_color`               | `ColorSpace.ACES`      | `output_color = 6` (ACES2065-1)  |
| `output_bps`                 | `16`                   | `output_bps = 16`                |
| `no_auto_bright`             | `True`                 | `no_auto_bright = 1`             |
| `gamma`                      | `(1, 1)` → linear      | `gamm[0] = gamm[1] = 1.0`        |
| `use_camera_wb` (as-shot)    | `True`                 | `use_camera_wb = 1`              |
| normalization                | `/ 65535.0` → float32  | done in `raw_decoder.cpp`        |

### White balance (oracle-locked compatibility state)

| Mode       | Behavior                                                                  |
|------------|---------------------------------------------------------------------------|
| `AS_SHOT`  | LibRaw camera WB (`use_camera_wb`) during demosaic.                        |
| `DAYLIGHT` | LibRaw daylight-balanced base output; no adaptation.                       |
| `TUNGSTEN` | One CAT02 **2850 K → 6504 K** adaptation in linear ACES; tint is neutral.   |
| `CUSTOM`   | One CAT02 **`temperature` K → 6504 K** adaptation, then float32 tint.      |

Whitepoints come from CCT: **CIE daylight locus** for ≥ 4000 K, **Kang 2002**
Planckian approximation below. Upstream's generic Von-Kries call resolves to CAT02. Production
uses its full cone-response matrix, preserves NumPy's `allclose`/`isclose` skips, and rounds CAT
output to float32 before the separate float32 green-channel tint multiply. The digest-generated
C++ gate reproduces all 56 research vectors at both declared bit boundaries; connected Samsung,
MotionCam, and third-cohort DNG repeat digests are recorded in
[`docs/research/raw-wb-chromatic-adaptation.md`](../../docs/research/raw-wb-chromatic-adaptation.md)
and [Implement oracle-locked CAT02 RAW white balance and exact cast-order goldens](https://github.com/thetechgeekko/Spektrafilm-android/issues/192). Native entry points reject non-finite
or out-of-range `[1000,12000] K` / `[0.2,1.8]` settings before decode or colour math.

## Integration points (docs/RAW_DNG.md)

1. **Shipping engine input.** The standalone `:app` asks `RawDecoder` to decode a picked content
   `Uri` to direct float32 linear RGB (+ width/height/primaries), then constructs a
   `LinearImage`. There is no intermediate 8-bit bitmap. Source sample depth and the
   float32 working boundary are separate contracts.
2. **Non-shipping reference.** `RawCoilDecoder.Factory` exists in this library but is not registered
   by the standalone app. The never-built ImageToolbox host path and dormant
   `feature:film-emulation` module are not production integration points.

## License

Spektrafilm for Android is **GPLv3**. LibRaw is offered under LGPL-2.1-only or
CDDL-1.0. The distribution route for this static integration is `UNRESOLVED`;
including both upstream texts records provenance and does not elect a route.
Release requires a human-reviewed route plus the verified source/relink, notice,
and SBOM package. `compliance/license-decision.json` also keeps the human owner,
rationale, approval reference, and local-patch contribution authorization
fail-closed; changing only `license-route.txt` cannot pass release audit. See
`../../docs/LICENSING.md` and ticket #166.
