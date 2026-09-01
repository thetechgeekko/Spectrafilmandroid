# RAW / DNG editing on Android

> **ℹ️ Note (integration paths updated).** The decode *science* below (rawpy == LibRaw, ACES /
> 16-bit / linear / camera-WB settings) is correct and shipped in the **`:lib:libraw`** module
> (`libsfraw.so`, `RawDecoder.kt`) consumed by the standalone **`:app`**. Ignore the references to
> `feature:film-emulation` / a Coil `Decoder.Factory` / ImageToolbox's save pipeline — that host was
> never built. Also note two things this doc predates: the **off-heap decode + half-size/OOM ladder**
> (see `RawDecoder.kt` / `EngineHelpers.kt` and `docs/RESEARCH_BIG_FILES.md`) and the **MotionCam
> `.mcraw`** import path (`McrawContainer.kt`, see `docs/RESEARCH_MCRAW.md`).

Goal: open camera RAW (incl. **DNG**) on device and feed the engine a **linear,
scene-referred RGB** buffer using Spektrafilm's desktop `rawpy` settings. Matching
settings are necessary but not sufficient for byte identity: the rawpy/LibRaw
version, build flags, optional codecs, patchset, thread policy, and input corpus
must also be pinned.

## Key insight: rawpy == LibRaw

spektrafilm reads RAW with `rawpy`, and **`rawpy` is a thin Python binding over LibRaw**.
So "RAW like spektrafilm" = "LibRaw with the same `postprocess` options". We compile LibRaw
with the NDK and call it from a JNI wrapper using the identical settings:

```python
# spektrafilm/utils/raw_file_processor.py (effective settings)
raw.postprocess(
    output_color = rawpy.ColorSpace.ACES,   # linear ACES2065-1 primaries
    output_bps   = 16,                       # 16-bit
    no_auto_bright = True,
    gamma        = (1, 1),                   # LINEAR (gamma 1.0)
    use_camera_wb = True,                    # as-shot; other modes use daylight base
)
```

LibRaw equivalent (C++):

```cpp
LibRaw raw;
raw.open_buffer(bytes, len);            // from a SAF InputStream / fd
raw.imgdata.params.output_color   = 6;  // 6 = ACES (matches rawpy.ColorSpace.ACES)
raw.imgdata.params.output_bps     = 16;
raw.imgdata.params.no_auto_bright = 1;
raw.imgdata.params.gamm[0] = 1.0;       // gamma 1/1 → linear
raw.imgdata.params.gamm[1] = 1.0;
raw.imgdata.params.use_camera_wb  = 1;  // as-shot; 0 selects LibRaw's daylight base
raw.imgdata.params.threshold      = 0.0f; // LibRaw wavelet is outside parity
raw.unpack();
raw.dcraw_process();
libraw_processed_image_t* img = raw.dcraw_make_mem_image();  // 16-bit linear ACES
```

The Android wrapper normalizes that uint16 intermediate to float32, applies any
requested ACES-space white-balance adaptation, and converts ACES2065-1 to linear
ProPhoto RGB before handing it to the engine. LibRaw's raw-memory budget is set
to 128 MiB before `open_buffer`; its mobile-unfriendly 2048 MiB default is not used.

The decoder writes into one uninitialized, malloc-backed float buffer and transfers
that exact allocation into the JNI registry; it does not zero-fill a vector and then
allocate/copy a second full-resolution buffer. The allocation is exposed only through
a `LinearResult` data lease. `close()` prevents new readers immediately but waits for
active leases, and release requires the exact token, base address, and capacity.
Export-scale hand-off transfers a live lease into the engine `LinearImage`; proxy/Coil
consumers copy while leased and release the native result before returning. Cooperative
cancellation is installed as a LibRaw progress handler for its long decode phases and
is also polled around each phase and throughout first-party read, uint16-to-float,
white-balance, and colour loops. A cancellation observed before or after the constant-
time ownership transfer discards or registry-releases the buffer exactly once.
The same production ownership-publication seam is exercised on the host under
ASan/UBSan with injected pre-handoff and post-adopt cancellation, platform
publication failure/exception, successful publication, and stale-token checks;
each rollback ends with zero outstanding registry entries.

The cross-module ownership rules and combined host/JVM/Android verification matrix are canonical
in [JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md).

White-balance modes mirror upstream: **as-shot** (`use_camera_wb`), **daylight**/**tungsten**
(LibRaw daylight base), and **custom** (temperature + tint). Research has pinned upstream's
generic `method='Von Kries'` call to its actual colour-science 0.4.7 default, **CAT02**, with
tint as a separate float32 step. The native path now reproduces that full CAT02 matrix and
cast order bit-for-bit for every locked host/device vector; as-shot/daylight remain arithmetic
no-ops after LibRaw. Native requests reject non-finite or out-of-product-range temperature/tint
before decoding. See
[`research/raw-wb-chromatic-adaptation.md`](research/raw-wb-chromatic-adaptation.md) for the
reproducible decision, exact goldens, and connected-device evidence.

## Why not Android's built-in DNG API?

`android.hardware.camera2.DngCreator` **writes** DNG from `RAW_SENSOR` buffers; it does not
decode arbitrary RAW into RGB. Android's NDK `ImageDecoder` can decode some DNGs but only via
embedded preview / limited paths and gives no control over demosaic, gamma, or output
primaries. Neither reproduces Spektrafilm's controlled scene-referred linear path.

(ImageToolbox already ships a `NefDecoder` that extracts the embedded **JPEG preview** from
Nikon NEF — useful for fast thumbnails, but it is *not* sensor data. We keep it for previews
and add LibRaw for the real decode.)

## Building LibRaw for Android

- The shared resolver pins the official LibRaw `0.22.2` archive and SHA-256,
  applies the ordered hashed patchset, checks exact version/security markers,
  and fails closed. See [`dependencies/LIBRAW.md`](dependencies/LIBRAW.md).
- ABIs are `armeabi-v7a`, `arm64-v8a`, and `x86_64`; NDK r27, CMake 3.22.1,
  C++17, and the actual release optimization flags are recorded.
- RawSpeed and Adobe DNG SDK integration are disabled. They must not be enabled
  as a broad speed or codec switch without corpus parity, Android build cost,
  security maintenance, and license evidence.
- The exact release path is serial. Debug-only OpenMP reproduced upstream issue
  #845 with five different decoded hashes from five runs on the connected phone.
- Release telemetry separates fd I/O, `open_buffer`, unpack, `dcraw_process`,
  `dcraw_make_mem_image`, output allocation, uint16-to-float copy, CAT/tint,
  ACES-to-ProPhoto conversion, and JNI handoff. The repeat probe can exercise both
  buffer and fd entry points; output payloads are hash-compared, not visually judged.
- A release/R8 qualification run must receive a previously pinned 64-hex
  `ticket158_expected_sha256` and compare every repetition against it. Self-seeding
  is permitted only with explicit `ticket158_exploratory=true`; that mode emits
  `TICKET158_RAW_RELEASE_R8_EXPLORATORY: RESULT (UNQUALIFIED)` and can
  never satisfy the exactness gate. Content-URI probes adopt only the SDK-valid
  media permission (API 33+) or legacy storage permission (API 29-32); below API
  29 they require an already-granted manifest or URI read permission.
- Matched minified release/R8 tests retained the complete Samsung and MotionCam
  float-buffer SHA-256 values while removing a 34.628 ms / 25.730 ms median JNI
  publication copy. The final shipping ELF has no OpenMP dependency or symbols,
  and active device thread inventories showed no LibRaw worker pool.
- NDK `AImageDecoder` is not a replacement for this scene-linear route. Any API-30+
  native-decoder experiment is restricted to supported non-RAW/display-referred
  fallback inputs, requires an OS-version codec corpus, and cannot enter the
  archival-exact tier without separately proving decoded samples.
- LibRaw is offered under LGPL-2.1-only or CDDL-1.0. The intended LGPL static
  distribution route still requires the source/relink/notice bundle owned by
  the release-blocking licensing ticket; see `LICENSING.md`.

## Two integration points

1. **Engine input (shipping path).** The standalone app asks `RawDecoder` to
   decode the picked content `Uri` into a direct float32 linear ProPhoto buffer,
   then constructs `SpektraEngine.LinearImage`. There is no intermediate 8-bit bitmap.
2. **Coil adapter (non-shipping reference).** `RawCoilDecoder` exists in the lib
   module but is not registered by the standalone app. It must not be described
   as live until its ownership/lifetime path is wired and tested.

## Output

The standalone app exports JPEG/PNG8, PNG16, TIFF16, TIFF32F, scene-linear TIFF32F, and a
separately gated Ultra HDR container through its own `ImagePipeline` plus native PNG/TIFF writers.
Output color space/transfer/depth/format is being unified under the live OutputDescriptor contract.
Source EXIF carry-through currently applies to JPEG; do not infer all-format metadata parity.
