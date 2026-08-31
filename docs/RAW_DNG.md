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

The returned native allocation is exposed only through a `LinearResult` data
lease. `close()` prevents new readers immediately but waits for active leases,
and the JNI allocation registry requires the exact token, base address, and
capacity before freeing. Export-scale hand-off transfers a live lease into the
engine `LinearImage`; proxy/Coil consumers copy while leased and release the
native result before returning. Cooperative cancellation is installed as a
LibRaw progress handler for its long decode phases and is also polled around
each phase and throughout first-party read/copy/colour loops.

The cross-module ownership rules and combined host/JVM/Android verification matrix are canonical
in [JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md).

White-balance modes mirror upstream: **as-shot** (`use_camera_wb`), **daylight**/**tungsten**
(LibRaw daylight base), and **custom** (temperature + tint). Research has pinned upstream's
generic `method='Von Kries'` call to its actual colour-science 0.4.7 default, **CAT02**, with
tint as a separate float32 step. The currently shipped native path still uses a direct-XYZ
scaling approximation and therefore is not exact for tungsten/custom modes; see
[`research/raw-wb-chromatic-adaptation.md`](research/raw-wb-chromatic-adaptation.md) for the
reproducible decision and follow-up contract. As-shot and daylight receive no extra CAT.

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

Editing results are exported through ImageToolbox's existing save pipeline: 8/16-bit
PNG/TIFF/JPEG with EXIF (`ExifInterface`) and embedded ICC matching the chosen output color
space (sRGB / Adobe RGB / ProPhoto / Rec.2020 / ACES), reproducing spektrafilm's `io.py`
behavior on the formats Android supports natively.
