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

### Precision is a chain of separate contracts

`LinearResult.precisionDescriptor` is the versioned source-of-truth for the
admitted native route. Do not use “16-bit processing” as shorthand for all of the
following:

1. **Sensor/container samples:** supported integer RAW/DNG inputs retain their
   declared 8/10/12/14/16-bit storage, unsigned sample format, endian/packing,
   compression, and CFA/layout identity through LibRaw unpack. Float32 can exactly
   represent every integer code in this range.
2. **Effective normalization range:** v1 records WhiteLevel per channel and the
   full BlackLevel model as `common + channel + repeating cell`, plus whether each
   level came from DNG metadata, a declared-bit default, or decoder metadata.
   Effective precision is the number of bits needed for the admitted code span;
   it is not silently substituted for declared sensor bits.
3. **RGB interchange:** LibRaw's parity configuration emits its 16-bit linear ACES
   bitmap, which is converted once into the owned float32 RGB buffer, followed by
   the adopted CAT02/ACES-to-ProPhoto CPU arithmetic. Some library and colour
   calculations use their existing mixed integer/float/double arithmetic; there
   is no full-frame f64 pipeline and no extra f16 boundary.
4. **Engine and export:** downstream f32/Vulkan compute, encoded PNG/TIFF/JPEG bit
   depth, HDR transfer functions, mastering metadata, and Ultra HDR gain maps are
   separate output contracts. A 14-bit sensor does not imply a 14-bit export, and
   a float32 working buffer does not by itself make an HDR file.

The descriptor also records both proxy mechanisms separately: whether LibRaw's
half-size route was requested, and the requested plus actually-applied longest-edge
subsample step. A successful reduced preview therefore cannot be mistaken for a
full-resolution native result merely because its pixels remain float32.

For DNG, descriptor authority is bound to the decoded RAW IFD, not to the
largest image. The bounded TIFF walk must find exactly one non-reduced CFA or
LinearRaw IFD matching LibRaw's identified and post-unpack raw geometry; zero or
multiple matches are `PRECISION_METADATA`. Root `DNGVersion` is the sole
container-level inheritance; any reachable non-root `DNGVersion` is hostile and
ambiguous even when its BYTE[4] payload is structurally valid. That dependency
failure is enforced for a recognized TIFF independently of whether IFD0 itself
establishes semantic DNG identity. Precision, CFA, BlackLevel/WhiteLevel,
BaselineExposure, and LinearResponseLimit tags are local to the selected IFD, so
preview, parent, and sibling tags cannot supply provenance. Conflicting
duplicates are rejected. Payload types/counts/offsets are bounded before
`LibRaw::open_buffer`, including external BlackLevel data, so malformed precision
metadata does not first traverse dependency parsing.

That pre-open decision is intentionally more conservative than final source
selection: every non-reduced CFA/LinearRaw or RAW-looking IFD in the bounded walk
must pass the same precision, level, and layout checks even when its width,
height, compression, or photometric eligibility field is missing or malformed.
LibRaw can inspect an IFD that it does not ultimately select, so a clean largest
plane cannot mask a malformed smaller RAW candidate. Structurally unsafe or
out-of-range BlackLevel data is rejected in every walked IFD, including reduced
and non-selected images, because the dependency converts that metadata during
its wider TIFF walk. Presence of `BlackLevelDeltaH/V` is likewise aggregated
from every bounded next-IFD and SubIFD, including reduced images and IFDs with
malformed eligibility fields; it cannot disappear by failing RAW selection.
Both buffer and fd decode routes run this check before
`open_buffer`; the sanitizer regression uses a separate open-attempt observer
to prove the hostile containers return with zero dependency-open attempts.
The dependency walk records up to ten distinct IFD offsets, matching LibRaw's
`LIBRAW_IFD_MAXCOUNT` storage limit, and follows non-zero next-IFD offsets in
either direction as well as SubIFD edges. A repeated offset (cycle or alias), a
malformed or duplicate SubIFD tag, more than sixteen declared SubIFDs, an
out-of-bounds edge/payload, or exhaustion of the IFD/depth bound marks the walk
incomplete and fails closed before dependency open. Any `II`/`MM` endian header
is also treated as a dependency TIFF candidate: a wrong or truncated TIFF magic
value is rejected before LibRaw can dispatch its TIFF parser.

V1 publishes only verified repeating Bayer 2×2 and X-Trans 6×6 geometry (with
explicit rows, columns, and bounded pattern), the specified RGB-identity
`CFAPlaneColor` mapping, and rectangular `CFALayout`. Absent CFA mapping/layout
tags use those specification defaults. Duplicate, malformed, remapped, or
staggered selected-IFD tags fail closed; reduced-preview values remain local to
that preview. LibRaw custom CFA/filter-table layouts, including `filters == 1`,
also fail until a later descriptor can represent them without loss.

Qualified LinearRaw is the precise lossless v1 subset: three or four samples,
uniform `BitsPerSample` and `SampleFormat` arrays whose count equals
`SamplesPerPixel`, chunky `PlanarConfiguration=1`, no `ExtraSamples`, a 1×1
BlackLevel repeat, exactly one BlackLevel and WhiteLevel per sample channel when
present, and no CFA tags. It also requires local ColorMatrix1/2 plus paired,
non-zero CalibrationIlluminant1/2. Each matrix is an exact 3×3 or 4×3
`SRATIONAL` array with bounded payload, no zero denominator, and a non-zero row
for every admitted sensor plane. After LibRaw identify, both promoted matrices
and its actual RGB coefficient table must still contain a finite active row or
column for every plane. A spatial
row×column×sample BlackLevel matrix is valid DNG but explicitly requires a later
descriptor version. Every admitted SHORT, LONG, or RATIONAL BlackLevel element,
inline or external and in either TIFF byte order, is parsed before
`LibRaw::open_buffer` and bounded by the selected IFD's declared sample maximum.
Zero denominators, fractional rationals, and any out-of-declared-range element
fail rather than being rounded. `BlackLevelDeltaH/V` remains a typed unsupported
spatial transform.

For LinearRaw, level provenance, level validation, usable-span calculation, and
the public black/white lists use exactly the active three or four sample planes.
No padded carrier slot can inflate effective precision. CFA descriptors continue
to expose LibRaw's four physical level slots, including the second-green slot for
a three-colour Bayer mosaic.

Declared integer `BitsPerSample` below 8 or above 16 is outside the qualified
native contract and fails before dependency open. Effective precision can still
be below eight bits when valid black/white metadata narrows the usable code span;
that value is recorded separately and does not relabel the source.

The qualified wrapper does **not** publish floating-point DEFLATE DNG through
`dcraw_make_mem_image`: that route would quantize a `SampleFormat=3` source before
the app's float32 boundary. It fails before unpack/process with the typed
`DEFLATE_DNG` fallback status. Any platform result must use
`RawPrecisionDescriptor.forPlatformDisplayReferredFallback(...)`, which is
permanently labelled display-referred and cannot claim native RAW parity.

Project-owned host fixtures generate both-endian, row-packed 8/10/12/14-bit and
TIFF-word 16-bit DNGs with boundary bands, explicit BlackLevel/WhiteLevel,
BaselineExposure, and LinearResponseLimit. Compliant three- and four-sample
LinearRaw fixtures additionally carry Orientation, a null-terminated
UniqueCameraModel, and a valid two-illuminant pair of correctly dimensioned
signed-rational ColorMatrices (3x3 or 4x3, including ColorMatrix1). The pair
exercises LibRaw's post-identify DNG promotion so a fourth sensor plane cannot
silently receive a zero colour coefficient. Each plane is perturbed independently
by one adjacent code. The vendored ICC writer also rounds signed s15Fixed16
coefficients before their unsigned word encoding, avoiding undefined conversion
for the negative entries in the generated ACES profile. The
sanitizer gate asserts strict adjacent-code distinguishability/order,
`(code-black)/(white-black)` monotonicity, exact repeat float digests, every
SHORT/LONG/RATIONAL BlackLevel element and bound, CFA mapping/layout defaults and
rejections (including non-RGB plane maps and malformed CFALayout type/count in
both byte orders), every LinearRaw matrix/illuminant/planar/extra-sample negative,
zero fourth rows in each matrix independently, malformed-geometry and reduced
hostile siblings with zero dependency opens, bounded metadata/stride/allocation,
selected-IFD preview isolation, independently known LJ92 predictor output,
malformed-level rejection, and cancellation. The dependency-focused matrix adds
17 hostile classes in each byte order through both public routes (68 assertions):
valid/malformed reduced BlackLevelDelta children on next/SubIFD paths, malformed
eligibility, valid/malformed non-root DNGVersion with root DNG identity absent,
malformed root DNGVersion, negative BlackLevel siblings, a backward next-IFD,
duplicate and 17-entry SubIFD arrays with hidden unsafe children, a depth-five
child, a cyclic SubIFD graph, and an endian header with wrong TIFF magic. Every
case requires typed `PRECISION_METADATA` plus an independent zero-open
observation.

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
