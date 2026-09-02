# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A native-C++ Android port of the [spektrafilm](https://github.com/andreavolpato/spektrafilm)
spectral film-simulation engine, driven by a Jetpack Compose UI. The engine reconstructs spectra
from RGB and runs a physically-based virtual **negative → enlarger → print → scan** pipeline.
GPLv3 (derivative of GPLv3 spektrafilm).

**The prime directive is bit-exact parity with the upstream spektrafilm oracle.** Every engine
stage was ported parity-first against golden vectors captured from the real Python engine. Any
change to `engine/spektra-core/src/main/cpp/**` must keep the host parity suite green (see below).
"Bit-exact" = within parity tolerance (`max_abs ≤ 1e-4`, `rms ≤ 1e-5`) of the oracle **and**
byte-identical across thread counts — not necessarily byte-identical across CPU architectures
(`-ffast-math` FMA contraction differs by arch).

## Module layout

Gradle modules actually built (`settings.gradle.kts`):
- **`:app`** — `com.spectrafilm.app`, the application. All UI lives here (~27 Kotlin files in
  `app/src/main/java/com/spectrafilm/app/`): `MainActivity`, the Lightroom-style editor
  (`Viewer`, `ParamsState`, `ImagePipeline`, `CropOverlay`, `CategoryIcons`/`SpectraIcons`),
  presets/recipes, settings, profile-curve browser, diagnostics.
- **`:engine:spektra-core`** — `com.spectrafilm.engine`. NDK C++ engine (`libspektra.so`) + the
  Kotlin facade `SpektraEngine` / `SpektraParams`. Bundles film/paper profiles, spectral LUTs,
  and ICC profiles under `src/main/assets/spektra/`.
- **`:lib:libraw`** — `libsfraw.so`, LibRaw via an ACES intermediate → linear ProPhoto RGB for RAW/DNG import.
- **`:lib:tiffwriter`** (`libsftiff.so`) and **`:lib:pngwriter`** (`libsfpng.so`) — 16-bit
  TIFF/PNG export writers.

`feature/film-emulation/` was a never-compiled pseudo-module (never in `settings.gradle.kts`);
it was deleted by #184 — its history survives in git and in `docs/DECISION.md`. The
real app is the standalone `:app` module documented in `docs/ARCHITECTURE.md`. The abandoned
ImageToolbox-host proposal survives only as historical decision input in `docs/DECISION.md` and
`docs/maps/IMAGETOOLBOX_MAP.md`. Start at `docs/EXECUTION_INDEX.md` for the current authority order
and live-work protocol.

## Engine architecture (C++, `engine/spektra-core/src/main/cpp/`)

- **`spektra_jni.cpp`** — JNI bridge; the single native boundary. Buffers cross as direct
  `ByteBuffer` (interleaved float32 RGB, row-major) to avoid per-pixel JNI calls.
- **`spektra.cpp` / `spektra.h`** — top-level `simulate` / `simulate_preview` orchestration.
- **`runtime/stages/`** — the pipeline stages in order: `filming` (RGB → spectral via Hanatos2025
  LUT → camera raw → film density CMY, with DIR couplers), `printing` (film CMY → enlarger
  dichroic Y/M/C filters → print paper density), `scanning` (density → spectral radiance → CIE
  XYZ → output RGB), plus `crop_resize` and `autoexposure` geometry/metering stages.
- **`model/`** — photographic math: `spectral`, `density_curves`, `emulsion`, `couplers`,
  `diffusion` (halation + in-emulsion scatter), `grain` (Poisson-binomial particle model),
  `color_filters`, `color_output`, `glare`.
- **`kernels/`** — hot numeric primitives: `spectral_upsampling`, `gaussian`/`exponential_filter`
  (spatial convs), `interp`/`lut3d`, `stats` (samplers), `exp10.h` (vector `exp10` → NEON `fmla`
  on arm64, replaces `pow(10,−x)` in the spectral integrals), and `parallel` (deterministic
  fork-join per-pixel threading — output is byte-identical for any worker count).
- **`profiles/`** + **`io/npy_lut.cpp`** — profile JSON + `.npy`/`.lut` asset loaders.

Two quality modes mirror upstream: **preview** (downscaled, default 640px, for interactive
tuning) and **scan** (full-res, for export). Decode + simulate run off the main thread.

## Build commands

Required toolchain (Android 15 16 KB page support): **NDK r27 (`27.0.12077973`)**,
**CMake 3.22.1**, **build-tools 35.0.0** (first `zipalign` with `-P 16`).
`sdkmanager "ndk;27.0.12077973" "cmake;3.22.1" "build-tools;35.0.0"`.

```bash
# Debug APK (builds libspektra/libsfraw/libsftiff/libsfpng .so for all 3 ABIs)
ANDROID_SDK_ROOT=/opt/android-sdk JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
  ./gradlew :app:assembleDebug

# JVM unit tests (the only automated test layer for the Kotlin code)
./gradlew :app:testDebugUnitTest

# Lint (abortOnError = true; baseline at app/lint-baseline.xml)
./gradlew :app:lint
```

16 KB page check (CI gates this): `build-tools/35.0.0/zipalign -c -P 16 4 <apk>` must pass, and
every `arm64-v8a`/`x86_64` `.so` must have `0x4000` `LOAD` alignment (`readelf -lW`).

## Engine host-parity tests (the real gate)

Stage tests live in `engine/spektra-core/src/main/cpp/tests/` and run on the **host** g++
toolchain (not NDK) — they are not part of the Android library. After any engine change, run them.
Compile a single test against the full source set (note `-pthread` is required for
`kernels/parallel`):

```bash
cd engine/spektra-core/src/main/cpp
CPP=$(pwd)
ASSET=../assets/spektra
SRC="spektra.cpp gpu/*.cpp kernels/*.cpp io/*.cpp model/*.cpp profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp"
g++ -std=c++17 -O2 -pthread -I. -I../../../../../tools/parity \
  -DSPK_TEST_DIR="\"$CPP/tests\"" \
  tests/test_simulate_e2e.cpp $SRC -o /tmp/test_simulate_e2e
# then run with the args the CI `engine-parity` job uses (see .github/workflows/ci.yml)
```

A test passes when its output contains no `FAIL` line. `tools/parity/run_engine_parity.sh`
builds and runs the whole suite locally with the same argv as CI (it fails loudly if its table
drifts from the workflow's `build_run` count). `engine-parity` is a **two-leg matrix** — the same
42 tests at `-O2` and at the shipping `-O3 -ffast-math -fno-finite-math-only`, because the release
APK's numerics were otherwise never gated. A plain local run reproduces the `-O2` leg only; for the
other, prefix `SPK_PARITY_EXTRA_FLAGS="-O3 -ffast-math -fno-finite-math-only"`. CI `engine-parity`
gates (42 tests):
`simulate_e2e` (goldens + BOTH film-density memos + the print-density memo + per-param key
completeness), `filming`, `spatial`, `crop_resize`, `downscale` (minification AA prefilter),
`autoexposure`, `small_preview_aa` (AE metering downscale AA), `diffusion` (+`_e2e`),
`fft_convolve` (the FFT diffusion path against a verbatim transcription of the direct
loop, incl. a kernel wider than the image, + 1-vs-8-worker byte-identity),
`lut_accel`, `lut_cache_e2e` (spectral 3D-LUT memo: warm engine byte-identical to a fresh one,
every key-folded param perturbed one at a time, 1-vs-8 workers through a warm cache),
`scanner_lut_e2e`, `enlarger_lut_e2e`, `output_spaces`, `lensblur`, `tonecurve`,
`half`, `bake_lut`, `params_passthrough`, `print_curves_morph` (opt-in s023 morph),
`np_interp` (non-monotonic DIR axis), `gamut_out_aces` + `gamut_out_oklch` + `gamut_out_oklrab`
+ `gamut_out_jzazbz` + `gamut_out_cam16ucs`
(opt-in output gamut compression — ACES-RGC, Oklch perceptual, Oklrab = Oklch indexed by
Ottosson's rebased lightness Lr, JzCzhz, and CAM16-UCS) + `gamut_in_xy` (opt-in input gamut
compression), the spektral-param wiring gates
`spectral_blur_e2e`, `hanatos_surface_e2e`, `camera_uvir_e2e`, `preflash_e2e`, `print_evcomp_e2e`,
`scanner_bwcorr_e2e`, `provia_couplers_e2e` (the last gates the positive-film DIR-coupler path),
`highlight_boost_e2e` (the pre-clip highlight-boost in filming.expose),
`spatial_decouple_e2e` (per-effect spatial gating: lens blur ON / halation OFF),
`print_spatial_e2e` (print-route filming spatial branch),
**`test_parallel`** (thread-invariance, fresh engine per thread count), and the
statistical grain gates `test_grain` + `test_grain_sublayer` (mean preservation +
noise std vs committed oracle references — the stochastic stage byte goldens
cannot cover), and `test_binomial_shortcircuit` (element-wise: `fast_binomial_one`'s
degenerate-CDF short-circuit against a verbatim transcription of the loop it replaces,
variate AND surviving RNG stream — the two grain gates above are statistical and
cannot see a sampler change). The param-wiring
goldens are pinned to oracle SHA `c1d0e44` (see `tools/parity/setup_env.sh`). The exact
per-test argv is in `.github/workflows/ci.yml` — copy from there rather than guessing.

- **`SPK_NUM_THREADS`** overrides `hardware_concurrency()` (parity tests pin 1 vs 8 to prove
  byte-identical output).
- Engine `CMAKE_CXX_FLAGS_RELEASE` is `-O3 -ffast-math -fno-finite-math-only`.
  **`-fno-finite-math-only` is required** — the scanning stage relies on NaN propagation through
  `density_to_light` to match spektrafilm's profile null handling. Do not strip it.
  All 42 gates pass at these flags as well as at `-O2`; note this holds for the **band**, not for
  byte-equality between the two builds — `-ffast-math` reassociates, which is exactly what
  invalidated a Highway f64 byte-identity claim proven only at `-O2` (`docs/research/perf-lab.md` §14).
- `tools/parity/` is the standalone `.spkvec` golden-vector comparator (CMake + ctest self-test,
  CI `parity` job). Goldens live in `tools/parity/goldens/` and `tests/*.spkvec`.

## CI jobs (`.github/workflows/ci.yml`)

`engine-native` (host C++ build of libspektra), `engine-parity` (stage parity gate, two legs: `-O2` and the shipping release flags), `parity`
(.spkvec comparator self-test), `python-lint`, `android` (`:app:testDebugUnitTest` + `:app:lint` +
full assemble for all ABIs + the 16 KB `zipalign -P 16`/`readelf` alignment gate),
`android-emulator` (manual dispatch only). `release.yml` builds and hash-binds an unsigned candidate
without secrets, reruns both parity flag legs plus release JVM/lint/R8/16 KiB gates, then a protected
Environment downloads by exact artifact database ID, signs that exact app/instrumentation pair,
proves installed-byte identity on API 35, and verifies immutable remote release/assets by numeric IDs
before publication. Candidate identity is bound to source/version/run ID/run attempt/artifact digest,
all six Gradle locks, both SPDX documents, mapping, symbols, runtime classpath, and provenance.
`r8-smoke.yml` (manual dispatch) builds the R8-minified unsigned candidate,
explicitly signs it with the committed public debug key, and runs the 16 KB pre-tag smoke checks.

## Conventions / gotchas

- Current version: `versionCode 11` / `versionName 0.9.0`, `minSdk 24`, `targetSdk`/`compileSdk 34`.
  ABIs: `arm64-v8a`, `armeabi-v7a`, `x86_64`.
- **Commit signing is environment-dependent — test it, don't assume.** This line used to
  read "commit with `-c commit.gpgsign=false` (the signing server rejects signing here)"
  unconditionally. In a **Claude Code cloud container that already has signing configured**
  (`gpg.format ssh`, `gpg.ssh.program /tmp/code-sign`, `commit.gpgsign true`) that is
  **false**: a probe commit signed fine and carried a real SSH signature, and passing the
  flag is what produced two Unverified commits the stop hook then flagged. So: try a signed
  commit first; only fall back to `-c commit.gpgsign=false` where signing actually fails
  (which is where the original note came from — keep it for that case). Note
  `git commit --amend` / `git rebase --exec` may need explicit permission, so it is much
  cheaper to sign on the first commit than to fix it afterwards. (2026-08-29.)
- **A new engine `.cpp` must be added to `engine/.../cpp/CMakeLists.txt` by hand, and the
  parity suite will NOT catch it if you forget.** The host parity build compiles with a
  glob (`kernels/*.cpp model/*.cpp ...`, see the build line above); the Android build
  enumerates every source explicitly. A file missing from CMakeLists therefore passes all
  40 gates locally and fails only at the Android `ninja ... spektra` step, as an undefined
  reference. Check a new source the way CI links: `g++ -std=c++17 -O2 -pthread -fPIC
  -shared -I. <CMakeLists sources, minus spektra_jni.cpp> -Wl,--no-undefined -o /tmp/x.so`.
  (Cost a red CI run on `551c57f`.)
  **Now checkable without a device or gradle:** `tools/arm64_check/check_android_link.sh`
  links the .so for arm64 with real NDK clang at the shipping flags using the CMakeLists
  **enumerated** list plus `-Wl,--no-undefined`, and fails if a `.cpp` is on disk but
  absent from CMakeLists. It also compiles `spektra_jni.cpp`, which the host suite never
  builds at all. Get the NDK with
  `curl -sSLo ndk.zip https://dl.google.com/android/repository/android-ndk-r27-linux.zip && unzip -q ndk.zip -d /opt/`.
- **A native-only edit may not rebuild through `:app:assembleDebug` alone.** Observed on
  `:lib:libraw`: a changed `.cpp` produced `BUILD SUCCESSFUL in 6s` with a **stale**
  `libsfraw.so`. `./gradlew :lib:libraw:assembleDebug --rerun-tasks` rebuilt it. Verify
  the object actually changed before trusting an on-device measurement of it.
- **Never size a performance lever from a debug APK, and state the build type on every
  timing.** Debug and release differ by ~2x overall but *unevenly per module*: an export
  measured 12189 ms debug / 6251 ms release, while `decode` inside it went 3647 → 546.
  The cause was that `CMAKE_CXX_FLAGS_DEBUG` defaults to `-g` with **no `-O`** (i.e.
  `-O0`), and only the engine's CMakeLists guarded against it. All four native modules
  now carry `if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O") set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")`
  — keep it when editing them. An uneven debug→release ratio *across modules* means
  per-module compiler flags, not slow code. (`docs/research/perf-lab.md` §19.)
- **Do not use `strings` to check a literal made it into a `.so`.** On the Windows /
  Git-Bash toolchain it returns nothing for these libraries and so reports 0 matches for
  strings that ARE present — it did this for the long-standing `decoded %dx%d` literal,
  which is what exposed the tool rather than the build. Use `grep -ac '<literal>' lib.so`.
- Release signing: local maintainers may explicitly provide `keystore.properties`
  (`storeFile`/`storePassword`/`keyAlias`/`keyPassword`). Without it,
  `assembleRelease` deliberately emits an unsigned APK and never falls back to the debug key.
  Production signing occurs only in `release.yml`'s protected signing job, after qualification.
  That Environment also needs `RELEASE_GITHUB_TOKEN` with repository Administration (read) and
  Contents (write), immutable releases enabled, a protected immutable `refs/tags/v*` ruleset, and
  maintainer approval. The temporary key must be absent before verification/publication; never
  replace these fail-closed requirements with the ordinary workflow token or a debug-key fallback.
- Release `isMinifyEnabled = true` (R8 shrink via `proguard-rules.pro`: `-dontobfuscate` + JNI/enum
  keep-rules). The standing `android` job builds debug (minify off); the exact release workflow and
  manual R8 smoke both exercise the minified path. **A wrong keep-rule does NOT only fail as a runtime crash — that claim was
  wrong and it cost us a shipped defect.** R8 removed `kotlin.Triple.getFirst/getSecond/getThird`
  and the `kotlin.Pair` pair, because `com.spectrafilm.engine.**` was kept but `kotlin.**` was
  not, and no *bytecode* ever calls those getters — the only caller is `spektra_jni.cpp`, by
  literal string, which R8 cannot see. `-dontobfuscate` does not save you: it prevents
  RENAMING, not REMOVAL. The result was 19 engine params marshalling as **0.0** on every
  release render, silently — no crash, no log, just wrong numbers, in every APK ever shipped.
  So the real failure mode is a **silently wrong image**, which is worse than a crash because
  nothing announces it. `tools/r8_check/check_release_dex.sh` now reads the shrunk dex and
  fails if any JNI-resolved member is gone (wired into `release.yml` and `r8-smoke.yml`; validated against both
  a good and a deliberately shrunk dex). Still smoke-test a release build
  on a device before tagging. Later exact candidate/device evidence and the mandatory commands live
  in `docs/RELEASE_CHECKLIST.md`; dated audit sections are not current release evidence.
- **Attribution "Film modeling powered by spektrafilm" must stay** (GPLv3 requirement).
- Unit tests put real `org.json` on the test classpath (the `android.jar` stub throws "not mocked")
  so `Presets` JSON round-trips on the plain JVM.
- `docs/EXECUTION_INDEX.md` defines documentation authority and the dependency-aware execution loop.
  GitHub Wayfinder maps own live status; `HANDOFF.md` and `docs/AUDIT.md` are historical evidence,
  not current queues. Run `python tools/docs/check_docs_consistency.py` before a documentation handoff.

## Agent skills

### Reaching another Claude session

`SendMessage` fails from a cloud session with `auth: this cloud session cannot message
other sessions yet`. **That is a property of `SendMessage`, not of cross-session
delivery.** `create_trigger` (claude-code-remote MCP) with `persistent_session_id` set to
the target session ID and a near-future `run_once_at` delivers the prompt into that
session's conversation, and works. `list_triggers` shows which sessions are addressable —
each routine carries the `persistent_session_id` it fires into.

Recorded because a session concluded from three `SendMessage` failures that a peer was
unreachable, told the owner so, and relayed three replies through `HANDOFF.md` instead —
while a routine in its own trigger list already said the trigger path worked. One tool
refusing is not proof the capability is missing. (2026-08-29.)

### Issue tracker

Issues live in this repo's GitHub Issues (`thetechgeekko/Spektrafilm-android`); remote Claude
sessions use the GitHub MCP tools, local sessions use `gh`. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary (`needs-triage`, `needs-info`, `ready-for-agent`,
`ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root (created lazily by
`/domain-modeling`; absence is normal). See `docs/agents/domain.md`.
