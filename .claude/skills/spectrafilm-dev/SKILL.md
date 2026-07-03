---
name: spectrafilm-dev
description: Use when working on the Spectrafilmandroid spektrafilm film-emulation engine, its parity-gated native C++ spectral simulation pipeline (filming/printing/scanning), or its Jetpack Compose Lightroom-style RAW editor — covers engine parity rules, spectral film physics, and Compose editor patterns.
---

# Spectrafilmandroid development

## What this skill is

Prescriptive engineering laws and domain reference for the **Spectrafilmandroid** repo: a
native-C++ Android port of the GPLv3 [spektrafilm](https://github.com/andreavolpato/spektrafilm)
(a.k.a. `agx-emulsion`) spectral film-simulation engine, driven by a Jetpack Compose
Lightroom-style editor. The engine reconstructs a per-pixel spectrum from RGB and runs a
physically-based **negative -> enlarger -> print -> scan** pipeline on 81 spectral bands.

This skill encodes the project's hard laws plus honest-engineering rules so changes stay
parity-correct, deterministic, legally compliant, and truthful.

## When to use it

Load this when a task touches any of:
- `engine/spektra-core/src/main/cpp/**` (the C++ engine and its parity tests),
- the `SpektraEngine`/`SpektraParams` Kotlin facade,
- the Compose editor in `app/src/main/java/com/spectrafilm/app/**`,
- RAW/DNG import (`:lib:libraw`), TIFF/PNG export (`:lib:tiffwriter`/`:lib:pngwriter`),
- build/CI/release for this app.

Keywords: spektrafilm, agx-emulsion, film emulation, spectral, parity, engine, pipeline,
filming/printing/scanning, Compose editor, RAW, DNG, presets, recipes.

---

## BRUTAL TRUTH — non-negotiable coding rules

These override convenience. If a change cannot satisfy them, it is not done.

### Parity is the prime directive
1. **Bit-exact parity with the upstream spektrafilm Python oracle.** "Bit-exact" =
   within `max_abs <= 1e-4` and `rms <= 1e-5` of the oracle (float32 rounding) **and**
   byte-identical across thread counts. It is **not** required to be byte-identical across
   CPU architectures — `-ffast-math` FMA contraction differs by arch. (CLAUDE.md 14-17)
2. **ANY change under `engine/spektra-core/src/main/cpp/**` must keep the host-parity suite
   green before it is "done."** Run the gate (see below). A change is incomplete until you
   have run the parity tests and seen no `FAIL` line.
3. **Default-no-op / opt-in for non-parity features.** New non-parity behavior must default
   OFF so the default code path stays bit-exact. Precedent: glare (lognormal RNG, NOT
   bit-exact) is print-only and default OFF; enlarger/scanner 3D-LUT accelerators are opt-in
   and not bit-exact, default off.
4. **Deterministic seeds everywhere stochastic.** Grain (Poisson-binomial) and any RNG must
   use fixed seeds so output is reproducible and parity-gateable. No wall-clock / per-run
   entropy on the simulate path.
5. **`-fno-finite-math-only` is law — never strip it.** The scanning stage relies on NaN
   propagation through `density_to_light` (`10^-density`, profile null = NaN must collapse to
   0) to match spektrafilm's profile-null handling. Stripping it makes NaN handling undefined
   and breaks parity **silently**. The Release flags are `-O3 -ffast-math -fno-finite-math-only`
   (`engine/spektra-core/src/main/cpp/CMakeLists.txt:12`). (CLAUDE.md 105-107)
6. **Thread-invariance is mandatory.** Per-pixel stages run on the deterministic fork-join in
   `kernels/parallel.cpp`; output must be byte-identical for any worker count. `test_parallel`
   asserts this by running `SPK_NUM_THREADS=1` vs `8`. Never introduce reduction order or
   accumulator state that depends on worker count.

### Toolchain / legal hard pins
7. **NDK r27 (`27.0.12077973`) only**, CMake 3.22.1, build-tools 35.0.0, JDK 21. r27 is the
   first to guarantee 16 KB page-aligned `LOAD` segments (Android 15+); wrong NDK -> `dlopen`
   failure on 16 KB devices. Every `arm64-v8a`/`x86_64` `.so` must have `0x4000` `LOAD`
   alignment and `zipalign -c -P 16 4 <apk>` must pass (CI gates both).
8. **GPLv3 attribution "Film modeling powered by spektrafilm" must stay in the built app.**
   The whole app is a GPLv3 derivative (CLAUDE.md 126, NOTICE.md, README.md).
9. **Commit with `-c commit.gpgsign=false`** (the signing server rejects signing here).
   Never commit APKs or a real `keystore.properties`.
10. **Session continuity.** After a PR merges, recreate the working branch from `origin/main`
    (never keep committing on the pre-merge branch). After a container reset, re-verify:
    oracle checkout `/home/user/spektrafilm` at the golden-pinned SHA (`c1d0e44` for
    param-wiring), `PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs`, and rebuild the
    host-parity binaries before trusting any parity claim.

### Honesty rules (engineering integrity)
11. **Never claim parity or tests pass without running them.** If you did not run the gate,
    say so. No "should pass."
12. **Report failing output verbatim.** Paste the actual `FAIL` line / tolerance numbers.
    Do not summarize a failure as a success or hand-wave it.
13. **Never fabricate parity numbers, file paths, line numbers, or citations.** Use the real
    measured numbers from the repo (README.md 113-126) or run the test to get them.
14. **Preserve UPSTREAM-UNVERIFIED claims as flagged-uncertain.** The film-physics reference
    flags several items (the "Hanatos 2025" method has no located formal paper; the OFX
    whitepaper was unreadable; exact CC/grain constants came from porting docs not live
    profiles). Do not launder a flagged claim into asserted fact — keep the flag.
15. **GPU is preview-only, never the export or parity path.** GPU float varies by vendor and
    is not bit-reproducible; the parity engine is CPU C++ only. `LutGpuPreview.kt` is an
    optional preview accelerator, default OFF. Never route `simulate`/export through GPU.

---

## Map of the engine pipeline (5 stages, in order)

81 spectral bands, 380-780 nm, 5 nm step. Top-level orchestration:
`spektra.cpp` / `spektra.h` (`spk_simulate` / `spk_simulate_preview`).

| # | Stage | Implements | Key model files |
|---|-------|-----------|-----------------|
| 1 | **Filming** (RGB -> negative density) | `runtime/stages/filming.cpp/.h` | `kernels/spectral_upsampling.cpp` (Hanatos2025 LUT), `model/spectral`, `model/density_curves`, `model/emulsion`, `model/couplers` (DIR), `model/diffusion` (halation + scatter), `model/grain` + `kernels/stats` |
| 2 | **Printing** (film CMY -> print density) | `runtime/stages/printing.cpp/.h` | `model/color_filters` (dichroic Y/M/C), `model/spectral`, `model/density_curves` (paper, NO couplers) |
| 3 | **Scanning** (print density -> output RGB) | `runtime/stages/scanning.cpp/.h` | `model/spectral` (CIE CMFs -> XYZ), `model/color_output` (6 spaces + CCTF), `model/glare` (NOT bit-exact, default OFF) |
| - | **Crop/Resize** (geometry) | `runtime/stages/crop_resize.cpp/.h` | cubic 0.5x-2.0x, default identity |
| - | **Auto-exposure** (metering) | `runtime/stages/autoexposure.cpp/.h` | 7 metering methods -> EV comp |

**Routing:** `scan_film=true` (default) skips Printing and scans the negative directly;
`scan_film=false` runs the full print route. Two quality modes share one code path:
**preview** (downscaled, `preview_max_size` default 640 px) and **scan** (full-res, export).

Hot primitives in `kernels/`: `exp10.h` (vector `exp10` -> NEON `fmla`, replaces `pow(10,-x)`
in spectral integrals), `gaussian`/`exponential_filter` (spatial convs), `interp`/`lut3d`,
`tonecurve`, `half`, and `parallel`.

Profile/asset loaders: `profiles/profile.cpp` + `io/npy_lut.cpp`. Profiles + LUTs + ICC under
`engine/spektra-core/src/main/assets/spektra/`. ~28 film/paper profiles, 28 built-in presets.

See `references/film-emulation.md` for the physics and `references/parity-and-build.md` for the
full build + CI gate.

---

## Lightroom-mobile editor laws

The editor (~56 Kotlin files in `app/src/main/java/com/spectrafilm/app/`) is a parametric,
non-destructive Lightroom-style stack. Hard rules:

- **Parametric, never pixel-baked.** Edits are a serializable `SpektraParams` snapshot; the
  original RAW is never mutated. `Recipes.kt` is a non-destructive JSON sidecar keyed to the
  source URI; `EditHistory.kt` is in-memory undo/redo.
- **Presets/recipes are parameter snapshots, NOT LUTs.** `Presets.kt` round-trips every field
  to versioned JSON; "apply preset" pre-populates the param stack, it does not composite a
  fixed image. `BuiltInPresets.kt` = 28 curated looks.
- **Two-resolution proxy is THE rule, not an option.** Edit against the ~640 px preview; render
  full-res only on export.
- **Never render on the main thread**, and never full-res during interaction. Decode + simulate
  run on a background dispatcher; only the finished bitmap touches the UI thread.
- **Coalesce-to-latest + debounce.** Drive renders from `snapshotFlow(ParamsState)` ->
  `debounce`/`conflate`/`collectLatest`; cancel/replace superseded preview jobs. Do NOT render
  on every slider pixel and do NOT trigger renders via recomposition.
- **Compose recomposition discipline.** Hold params in `ParamsState` (a stable holder), collect
  state at the smallest scope, defer fast-changing reads (pan/zoom/crop) to layout/draw via
  lambda modifiers (`Modifier.graphicsLayer { }`, `drawWithCache`).
- **Never pass `Painter`/`ImageBitmap` as a composable parameter** (unstable -> recomposition
  storms). Pass a stable key/handle, resolve the bitmap inside; reuse bitmap buffers; call
  `prepareToDraw()`.
- **Wide gamut is opt-in and memory-heavy.** Reserve `wideColorGamut` + `RGBA_F16` + embedded
  ICC for the full-screen preview; sRGB/`ARGB_8888` elsewhere. Carry the engine's output color
  space into the embedded profile on export so files are self-describing.

See `references/lightroom-mobile.md` for the full patterns and pitfalls.

---

## How to run the gate

**Host-parity tests are the real gate** — host g++ (not NDK). After any engine change:

```bash
cd engine/spektra-core/src/main/cpp
CPP=$(pwd)
ASSET=../assets/spektra
SRC="spektra.cpp kernels/*.cpp io/*.cpp model/*.cpp profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp"
g++ -std=c++17 -O2 -pthread -I. -I../../../../../tools/parity \
  -DSPK_TEST_DIR="\"$CPP/tests\"" \
  tests/test_simulate_e2e.cpp $SRC -o /tmp/test_simulate_e2e
# run with the exact argv the CI `engine-parity` job uses — copy from .github/workflows/ci.yml
```

`-pthread` is required (`kernels/parallel`). A test passes when its output has **no `FAIL`
line**. `SPK_NUM_THREADS` overrides `hardware_concurrency()`; the parity tests pin 1 vs 8 to
prove byte-identical output. The CI `engine-parity` job gates **35 tests** — the full
authoritative list + per-test argv live in `.github/workflows/ci.yml` (copy from there rather
than guessing); the annotated list is in CLAUDE.md. Highlights beyond the core stage gates:
`small_preview_aa`, `print_curves_morph`, `np_interp`,
`gamut_out_aces`/`gamut_out_oklch`/`gamut_out_oklrab`/`gamut_in_xy`,
`spatial_decouple_e2e` (per-effect spatial gating), `print_spatial_e2e` (print-route filming
spatial branch), and `simulate_e2e`'s memo scenarios (both film-density slots + the
print-density memo + per-param key completeness).

Kotlin / build:

```bash
ANDROID_SDK_ROOT=/opt/android-sdk JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
  ./gradlew :app:assembleDebug          # debug APK (all 3 ABIs)
./gradlew :app:testDebugUnitTest        # JVM unit tests (the only Kotlin test layer)
./gradlew :app:lint                     # abortOnError=true; baseline app/lint-baseline.xml
```

---

## On-device export benchmark (measured; the export path ≠ the parity gate)

The parity gate above proves *correctness*; `engine/spektra-core/src/main/cpp/tests/bench_export.cpp`
measures *export speed* on a real arm64 device (local tool, not CI). Build it with the NDK
`aarch64-linux-android24-clang++` over the same engine source glob + the two writer TUs, with
`-lz -landroid` (`-landroid` is REQUIRED — `__ANDROID__` pulls in the AAssetManager path); push the
binary + the `assets/spektra` tree to `/data/local/tmp` and run under `adb shell` (full recipe in the
file header). **On Windows, drive `adb` from PowerShell, not git-bash** — git-bash mangles
`/data/local/tmp` into `C:/Program Files/Git/data/...`. For the app build use Android Studio's jbr as
`JAVA_HOME` (a too-new PATH `java`, e.g. JDK 26, breaks Gradle 8.14). PowerShell `>` corrupts binary
redirects (UTF-16) — capture `adb exec-out screencap` etc. from git-bash or via `adb pull`.

**Measured 2026-07-02 (SM-S948W, 12 MP):** a full export ≈ **74.5 s, of which grain is ~67 s (90 %)**
— NOT the spectral integrals. Grain parallelizes **byte-identically** (9 independent channel×sublayer
RNG streams saturate 8 cores; *within* a call the `mt19937` stream must stay serial per
`parallel.h:20-21`). The serial separable blurs (`kernels/gaussian.cpp`, `model/diffusion.cpp` have no
`parallel_for`), the double 140 MB result memcpy, and the PNG16 zlib deflate are the other bit-exact
levers → ~6× export. Ranked plan + root causes: `docs/EXPORT_PERF_2026-07-02.md`. Grain's determinism
constraint is *thread-invariance*, not oracle-byte-equality (grain is only statistically matched to
the oracle + excluded from the byte-exact goldens), so parallelizing the 9 calls is byte-identical to
today's output.

---

## Reference files

- `references/parity-and-build.md` — toolchain pins, full CI gate list, host-parity compile
  recipe + fast full-suite replay, the two-memo architecture, S4 parallelization rules, golden
  generation discipline, benchmarking doctrine, JNI/lifecycle/threading contract, gotchas,
  open AUDIT items (tiling).
- `references/film-emulation.md` — spectral-vs-LUT, spectral upsampling lineage, the three
  stages mapped to C++ files, a glossary, and a flagged "Sourcing & uncertainty" section.
- `references/lightroom-mobile.md` — editing model, real-time pipeline, Compose patterns,
  color management/export, and the pitfalls list.
