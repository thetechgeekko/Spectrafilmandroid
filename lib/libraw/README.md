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

> **DNG SDK add-on** (lossy / non-standard DNGs) is a separate decision tracked in
> M2; baseline DNG + CR2/CR3/NEF/ARW/RAF/ORF/RW2 work without it.

## Mobile / Google Pixel DNG decode (native vs fallback)

Mobile DNG layouts vary by device and camera mode. Support is decided from the
selected full-resolution raw plane, not from the file extension or preview:

| Compression tag        | Decodes here? | How                                                   |
|------------------------|---------------|-------------------------------------------------------|
| 1 — uncompressed       | ✅ native     | plain unpack                                          |
| 7 — lossless JPEG/LJ92 | ✅ native     | LibRaw **internal** lossless-JPEG (`lossless_jpeg_load_raw` / `ljpeg_start` / `ljpeg_row`, `src/decoders/decoders_dcraw.cpp`) — **no libjpeg required** |
| 8 — DEFLATE, float (`SampleFormat=3`) | ✅ native | qualified LibRaw 0.22.2 float path + NDK zlib |
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
> Bayer/linear raw in a **SubIFD**. The `dngsniff` sniffer walks IFD0 + SubIFDs
> + the next-IFD chain and picks the largest **non-reduced** (`NewSubFileType`
> bit 0 clear) image, so a JPEG preview is never mistaken for the raw
> compression. The unpack-failure classifier only flags genuinely-unsupported
> codecs (integer/linear Compression 8 or 0x80B2 → `DEFLATE_DNG`, 6 / 0x884C →
> `LOSSY_JPEG_DNG`, 0xCD42 → `JPEGXL_DNG`). A failed native float-deflate,
> uncompressed, or LJ92 decode stays a genuine data error (`UNPACK`).

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

### White balance (mirrors `raw_file_processor.py`)

| Mode       | Behavior                                                                  |
|------------|---------------------------------------------------------------------------|
| `AS_SHOT`  | LibRaw camera WB (`use_camera_wb`) during demosaic.                        |
| `DAYLIGHT` | LibRaw daylight-balanced base output; no adaptation.                       |
| `TUNGSTEN` | Von-Kries adapt **2850 K → 6504 K** reference, tint = 1.0, in linear ACES. |
| `CUSTOM`   | Von-Kries adapt **`temperature` K → 6504 K**, green/magenta `tint`.        |

Whitepoints come from CCT: **CIE daylight locus** for ≥ 4000 K, **Kang 2002**
Planckian approximation below — matching `_whitepoint_xyz_from_temperature`. The
adaptation is `method='Von Kries'` in CIE XYZ, applied in ACES2065-1, then the
green-channel tint multiplier (`_apply_tint_adjustment`).

## Two integration points (docs/RAW_DNG.md)

1. **Engine input (primary).** `feature:film-emulation` asks `lib:libraw` to decode
   a picked RAW `Uri` → direct float32 linear RGB (+ width/height/primaries) via
   `RawDecoder.decodeToLinear(...)`, then hands the `LinearResult` straight to
   `SpektraEngine.simulate` as a `LinearImage` — full 16-bit precision, no
   intermediate 8-bit bitmap.
2. **Sensor-decoded RAW gallery preview (secondary).** `RawCoilDecoder.Factory` is a Coil 3
   `Decoder.Factory` registered in the host's
   `core/data/.../di/ImageLoaderModule.kt` (`provideComponentRegistry`), alongside
   the existing `NefDecoder.Factory()`. Its current ARGB preview is an unmanaged
   channel-wise approximation; a proper ProPhoto-to-sRGB display transform is
   tracked separately and the engine buffer remains the authoritative path.

## License

Spektrafilm for Android is **GPLv3**. LibRaw is offered under LGPL-2.1-only or
CDDL-1.0. The distribution route for this static integration is `UNRESOLVED`;
including both upstream texts records provenance and does not elect a route.
Release requires a human-reviewed route plus the verified source/relink, notice,
and SBOM package. `compliance/license-decision.json` also keeps the human owner,
rationale, approval reference, and local-patch contribution authorization
fail-closed; changing only `license-route.txt` cannot pass release audit. See
`../../docs/LICENSING.md` and ticket #166.
