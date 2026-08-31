# Target Architecture — Spektrafilm for Android

> **⚠️ Aspirational — NOT the shipped architecture.** This document describes an early *target*
> design where the app would be a **fork of ImageToolbox** hosting `feature:film-emulation`. That
> design was **never built**. The real app is a **standalone `:app` module** (`com.spectrafilm.app`)
> — there is no `core/`/`build-logic/` ImageToolbox host, `feature:film-emulation` is **not** in
> `settings.gradle.kts` (uncompiled dead code), the RAW lib produces `libsfraw.so` (not
> `liblibraw.so`), and `targetSdk`/`compileSdk` is **34**. The native-engine rationale below is
> still accurate. For the real layout see `CLAUDE.md` and `engine/spektra-core/README.md`.

## Shipped storage boundary

The standalone app's current persistence/export contract is documented in
[TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md). In short, Android 10+
exports use recoverable app-tagged `MediaStore` pending rows and a verified
staged payload; Android 7-9 uses a same-directory fsynced temporary file and an
atomic no-overwrite rename after the legacy runtime permission is granted.
Process-owned export work survives Activity recreation. Source documents retain
persistable SAF access when the provider supports it; acquire/clear/restore run
through one process FIFO and otherwise enter an explicit reauthorization state.
Recipe/preset/sidecar JSON uses bounded parsing plus atomic replacement, while
mask interchange is defined by [MASK_JSON_SCHEMA.md](MASK_JSON_SCHEMA.md).

These guarantees are process-recoverable rather than a cross-provider database
transaction: a provider can report an indeterminate publish result, in which
case the app blocks another export until reconciliation resolves the row.

## Module map

```
Spectrafilmandroid/                 (forked ImageToolbox host + Spektrafilm additions)
├── app/                            [from ImageToolbox] application entry, navigation host
├── core/                           [from ImageToolbox] data, ui, domain, filters, di, ksp, ...
│   └── data/.../coil/              ← register RawDecoder (full-res, via spektra-core/libraw)
├── feature/                        [from ImageToolbox] 55 existing feature screens
│   └── film-emulation/             ★ NEW — Compose screen that drives the engine
├── lib/                            [from ImageToolbox] opencv-tools, neural-tools, curves, ...
│   └── libraw/                     ★ NEW — NDK: LibRaw → linear 16-bit RGB (rawpy parity)
├── engine/
│   └── spektra-core/               ★ NEW — NDK C++ port of spektrafilm + JNI/Kotlin facade
│       └── src/main/assets/spektra/
│           ├── profiles/           28 film/paper JSON profiles
│           ├── luts/               spectral-upsampling LUT binaries (~10 MB)
│           └── icc/                ICC profiles for output color spaces
└── settings.gradle.kts             + include(":engine:spektra-core", ":lib:libraw",
                                                ":feature:film-emulation")
```

★ = modules we add. Everything else is inherited from ImageToolbox and reused as-is.

## Layered view

```
┌─────────────────────────────────────────────────────────────┐
│ feature:film-emulation (Compose)                             │
│  • profile pickers (film / print)  • param sliders           │
│  • preview (downscaled) / full scan  • export                │
│  • Decompose component + Hilt VM, registered in Screen.kt     │
└───────────────▲───────────────────────────────▲─────────────┘
                │ Bitmap / params               │ Uri (RAW/DNG)
┌───────────────┴───────────┐       ┌────────────┴─────────────┐
│ engine:spektra-core (JVM)  │       │ core:data RawDecoder      │
│  SpektraEngine (Kotlin)    │       │  (Coil Decoder.Factory)   │
│  SpektraParams (Kotlin)    │       └────────────┬─────────────┘
└───────────────┬───────────┘                    │
                │ JNI                             │ JNI
┌───────────────┴───────────┐       ┌────────────┴─────────────┐
│ libspektra.so (C++)        │       │ liblibraw.so (LibRaw C++) │
│  filming/printing/scanning │◄──────│  RAW/DNG → linear RGB16   │
│  density curves, couplers, │ linear│  (gamma 1.0, wide gamut)  │
│  grain, halation, spectral │  RGB  └───────────────────────────┘
│  upsampling, scanner XYZ→RGB                                   │
└────────────────────────────────────────────────────────────┘
```

## Data flow (full pipeline)

```
RAW/DNG file ─► libraw ─► linear scene-referred RGB (ProPhoto/ACES, 16-bit float)
            │
            ▼  (or: load prepared linear TIFF/EXR/PNG via host I/O)
spektra-core.simulate(linearRgb, params):
   1. Camera / preprocess     exposure, auto-exposure, UV/IR filters, lens blur, diffusion
   2. Filming stage           RGB → spectral (Hanatos2025 LUT) → camera raw via film sensitivities
   3. Develop negative        log-exposure → CMY density (characteristic curves), DIR couplers,
                              grain (Poisson-binomial), halation/scatter
   4. Printing stage          film CMY → spectral transmittance → enlarger illuminant + dichroic
                              Y/M filters → print paper exposure → print density curves
   5. Scanning stage          density → profile viewing illuminant → CIE XYZ (2° observer)
                              → output RGB (sRGB/AdobeRGB/ProPhoto/Rec2020/ACES)
                              + glare, unsharp, CCTF
   ▼
Bitmap (display-referred) ─► host preview / export with EXIF+ICC
```

### Profile-driven scan illuminants

The shipped native scanner resolves `info.viewing_illuminant` while loading a
profile. Its exact, fail-closed registry currently contains D50 and K75P
(Kinoton 75P, selected by Kodak 2383 and 2393). One immutable resolved record
owns the spectral distribution, XYZ normalization, and output-space adaptation
matrices. Unknown identifiers fail profile loading with the rejected metadata
value in the diagnostic; there is no D50 fallback.

That record flows unchanged through the direct CPU spectral integral, scanner
3D-LUT construction/cache key, Vulkan linear and fused preview/export tables,
viewing glare, and black/white reference measurement. The two K75P full-route
goldens lock direct mode and LUT17 mode independently against matching pinned
oracle outputs; the LUT-vs-direct delta is diagnostic because upstream's coarse
17³ PCHIP is intentionally approximate. Strict exact export therefore keeps the
scanner LUT off. The GPU test separately compares both K75P routes with CPU export
and runs K75P first so the process-global self-check cannot be armed by D50 alone.
A scan route therefore cannot substitute its own illuminant policy unnoticed.

## Why the engine is native (C++/NDK), not Kotlin

| Driver | Detail |
|--------|--------|
| Performance | Spectral integration runs over an 81-band shape (380–780 nm, 5 nm). Per-pixel einsum-style contractions and several 2D Gaussian/exponential convolutions dominate. C++ with SIMD-friendly loops is the right tool. |
| Existing shape | spektrafilm's hot paths are already Numba-JIT kernels (`fast_gaussian_filter`, `fast_interp_lut`, `fast_stats`) — these translate near-mechanically to C++. |
| Co-location | LibRaw is C++. Sharing one native boundary (and linear-RGB buffers) with the decoder avoids extra copies. |
| Determinism | A C++ port lets us match spektrafilm's `.npz` regression baselines for numerical parity. |

## Threading & memory

- Decode + simulate run off the main thread (Kotlin coroutines `Dispatchers.Default`),
  results posted back to Compose state. Mirrors ImageToolbox's existing async image ops.
- Two quality modes, same as upstream: **preview** (downscaled to `settings.preview_max_size`,
  default 640) for interactive parameter tuning, and **scan** (full resolution) for export.
- Native side works on contiguous float buffers passed as direct `ByteBuffer`/`float[]` across
  JNI to avoid per-pixel JNI calls.

## Build / DI conventions (inherited from ImageToolbox)

- Gradle KTS + version catalog (`gradle/libs.versions.toml`), convention plugins in
  `build-logic/convention/` (`image.toolbox.library/feature/hilt/compose`).
- Hilt (Dagger) for DI; Decompose for navigation; Coil 3 for decode; min SDK 24, target 37.
- New feature module follows the standard pattern (see `IMAGETOOLBOX_MAP.md` §5): a
  `*Content.kt` Composable, a `*Component.kt` (Decompose) logic holder, optional `di/`, and a
  4-line `build.gradle.kts` applying the convention plugins.
- New screen registered in `core/ui/.../navigation/Screen.kt` and surfaced on the home grid.
