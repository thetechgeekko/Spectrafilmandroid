# Shipped architecture — Spektrafilm for Android

Status: current-tree architecture, reconciled 2026-08-31. Build files and tests remain executable
truth. Live work starts at [EXECUTION_INDEX.md](EXECUTION_INDEX.md).

Spektrafilm is a standalone Jetpack Compose application backed by a native C++ spectral engine and
native file codecs. It is not an ImageToolbox fork, does not use ImageToolbox's Hilt/Decompose
module graph, and does not compile the dormant `feature/film-emulation` tree.

## Compiled modules

```text
:app
  Compose editor, source/session state, recipes/presets/masks,
  preview/export orchestration, grading, storage and Android services
       |
       +--> :engine:spektra-core  Kotlin facade + libspektra.so
       +--> :lib:libraw          RawDecoder + libsfraw.so
       +--> :lib:pngwriter       PNG16 writer + libsfpng.so
       +--> :lib:tiffwriter      TIFF16/TIFF32F writer + libsftiff.so
```

The configured native ABIs are `arm64-v8a`, `armeabi-v7a`, and `x86_64`. Current Gradle truth is
min SDK 24 and target/compile SDK 34. Android policy/toolchain migrations remain live tickets and
must not be documented as already shipped.

`feature/film-emulation/` is retained source but absent from `settings.gradle.kts`; it is not an
alternate production implementation. The old proposed ImageToolbox integration is preserved as
historical decision input in [DECISION.md](DECISION.md) and
[maps/IMAGETOOLBOX_MAP.md](maps/IMAGETOOLBOX_MAP.md).

## End-to-end data flow

```text
content Uri / demo source
        |
        +-- supported RAW/DNG --> patched LibRaw 0.22.2 --> linear RGB
        |
        +-- supported platform image --------------------> linear RGB
        |
        v
LinearImage: interleaved native-order float32 RGB boundary
        |
        +-- scene-linear TIFF32F: verbatim input primaries, untagged --> publication
        |
        +-- preview: bounded linear proxy
        +-- export: approved full-resolution source
        |
        v
SpektraEngine JNI facade --> libspektra.so
        |
        +-- look bake: synthetic lattice, spatial/stochastic effects forced off --> .cube / CLF
        |
        +-- Strict Exact CPU: adopted mixed-precision graph; f64 where parity requires it
        |
        +-- eligible Fast GPU: resident Vulkan pointwise print graph
        |      (capability/self-test gated; non-cancellation route failure falls back to CPU)
        |
        v
native-owned direct float32 RGB result + explicit lease
        |
        v
post-engine app grade / masks / output conversion
        |
        +-- JPEG / PNG8
        +-- PNG16
        +-- TIFF16 / rendered TIFF32F
        +-- separately gated Ultra HDR container
        |
        v
digest-verified transactional publication
```

Unsupported compressed inputs may take Android's display-referred platform fallback. That path is
not labelled as a native RAW/oracle-identical decode. Source EXIF carry-through currently belongs to
JPEG output; no architecture diagram may imply all-format metadata preservation.

## Native engine graph

The CPU graph follows the pinned upstream model:

1. crop/rescale, exposure and auto-metering;
2. spectral upsampling and film exposure;
3. density curves, DIR couplers, grain, halation/scatter and diffusion;
4. optional enlarger/print-paper exposure and development; and
5. scan/viewing illuminant, XYZ/output RGB, glare, tone curve and output transfer.

Profiles, scientific LUTs and ICC assets live under
`engine/spektra-core/src/main/assets/spektra/`. Scientific response tables are not a baked
whole-look cube. Direct simulation is the normal model; `.cube`/CLF generation is an explicit export
operation, and the legacy GLES LUT loupe is only a preview approximation.

### Profile-driven scan illuminants

Profile loading resolves `info.viewing_illuminant` through one fail-closed registry. D50 and K75P
currently have immutable spectral distribution, XYZ normalization, and adaptation records. Unknown
identifiers fail profile loading; they do not silently become D50.

The resolved record feeds direct CPU scan, scanner-LUT construction/cache keys, Vulkan tables,
viewing glare, and black/white references. Direct and LUT K75P routes have independent oracle
fixtures. Strict Exact CPU keeps the coarse scanner LUT disabled because its interpolation is an
explicit approximation.

## Numeric and exactness boundaries

Sensor/container bit depth, processing representation, and exported file depth are independent:

- decoded/editor/JNI interchange is interleaved linear float32;
- the parity-bearing CPU implementation retains its adopted mixed-precision arithmetic and
  operation order, including float64 where parity requires it;
- the resident Vulkan route uses float32 under a separate tolerance/same-device contract;
- float16 is not a global processing mode and requires a separately measured quality gate before
  any narrow use; and
- output may be 8/16-bit integer or 32-bit float, while Ultra HDR additionally needs an honest
  gain-map/transfer contract.

“Strict Exact” currently means oracle tolerance (`max_abs <= 1e-4`, `rms <= 1e-5`) plus byte
identity across worker counts for the same build. It does not imply cross-build/cross-ABI bytes,
CPU/GPU equality, or complete container SHA identity. The authoritative levels are in
[BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md).

## CPU and Vulkan routes

### Strict Exact CPU

The C++ route is the parity-bearing implementation and universal fallback. Its 39-case host matrix
runs at O2 and the shipping `-O3 -ffast-math -fno-finite-math-only` flags. Deterministic fixed-chunk
parallelism proves worker-count invariance in covered scenarios. Spatial scratch, memory budgets,
writer streaming, and cold latency remain explicit work rather than reasons to change arithmetic
silently.

### Fast GPU

The current eligible print route keeps pointwise filming, printing, and scan in one persistent
Vulkan chain with one upload, three dispatches, and one readback. Full-byte table keys, explicit
NaN/bounds handling, cancellation, route counters, and a CPU comparison self-test gate exposure.
GPU output remains private until the whole route succeeds. Unsupported devices and
non-cancellation availability, validation, allocation, or dispatch failures take the Strict Exact
CPU route; cancellation terminates the render and must not restart work on CPU.

This is not yet a full resident graph. Grain, halation, diffusion, Pro-Mist and other
spatial/stochastic work remain outside the qualified slice, and no functional device pass proves a
1–2 second SLO.

## Ownership, threading and cancellation

- Kotlin coroutines keep decode, render and export off the main thread; UI state receives only
  bounded progress and terminal results.
- The native engine owns its worker scheduler and render-local stage timings. LibRaw shipping builds
  remain serial because the repeated compressed-Fuji OpenMP corpus changed output bytes;
  the current patched release has no exact OpenMP qualification, avoiding silent
  oversubscription and decode drift.
- Native allocations have explicit owner tokens and leases. A direct `ByteBuffer` is a view, never
  release authority. Geometry and logical buffer windows are checked at each JNI boundary.
- Render/close, cancellation, foreground-service generations, and exactly-once result publication
  follow [JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md).

## Persistence and publication

The storage contract is defined in [TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md):

- Android 10+ exports use app-tagged pending MediaStore rows and publish only a verified staged
  payload;
- Android 7–9 uses a same-directory fsynced temporary file and atomic no-overwrite rename after the
  legacy permission gate;
- process-owned work survives Activity recreation and journals recoverable state;
- source documents retain persistable SAF access when supported and otherwise enter explicit
  reauthorization; and
- recipe/preset/sidecar/mask documents use bounded, versioned parsing and atomic replacement.

These are process-recoverable guarantees, not a cross-provider database transaction. An
indeterminate provider result blocks another export until reconciliation resolves ownership.

### Two export caches (issue #179)

Both are keyed by content, live in `cacheDir`, and treat any doubt as a miss:

- **`ExportCache`** stores finished **container bytes** under a key covering the source digest,
  every engine parameter, the grade, the whole `OutputDescriptor`, geometry, quality and the build
  contract. A hit publishes the exact bytes a previous export produced — no decode, engine, grade
  or encode. It pays off on a repeated export.
- **`RenderPayloadCache`** stores the **engine's float output**, written by an idle pre-render
  after the editor sits still for 5 s, keyed on the engine inputs only (so one payload serves
  every container). A hit skips decode and the engine — ~86% of a first export — while the export
  still runs the real encoder, which is what makes the published bytes identical to an uncached
  export's.

Neither may ever return a stale or partial entry: metadata is committed after the payload, the
build contract folds in the install time, and a length or key mismatch discards the entry.

## Why native C++ remains the core

| Driver | Reason |
|---|---|
| Model fit | The pipeline performs 81-band spectral contractions plus large spatial filters and maps naturally to the upstream numeric graph. |
| Parity control | C++ exposes arithmetic order, compiler flags, NaN behavior, and deterministic scheduling to executable gates. |
| Boundary cost | LibRaw and output writers are native; explicit direct buffers avoid per-pixel JNI calls and accidental managed-heap copies. |
| GPU evolution | The owned C++ graph can add bounded Vulkan stages while preserving one CPU fallback and one parameter/profile implementation. |

Wholesale replacement with vkdt, Halide, RawSpeed, Adobe DNG SDK, or another framework is not an
architecture shortcut. Each may contribute a proven technique or narrow dependency only after its
numeric, format, licensing, memory, APK-size, security, and device-performance gates pass.
