# Audit — 2026-08-29 finding snapshot

> **Historical finding inventory, not live status.** Findings were ticketed after this snapshot and
> some statements below have since been resolved or superseded. Do not infer current issue state,
> gate count, or GPU policy from this file. Start at [EXECUTION_INDEX.md](EXECUTION_INDEX.md),
> [PRODUCTION_READINESS_PLAN.md](PRODUCTION_READINESS_PLAN.md),
> [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md), and the
> [Wayfinder map: production-ready Spektrafilm + 1–2 s exact export](https://github.com/thetechgeekko/Spektrafilm-android/issues/164).
> This audit remains evidence for why tickets exist; native GitHub dependencies are the only live
> implementation frontier.

New release-blocking findings from the full-codebase pass are now ticketed: patched LibRaw plus
hostile-input coverage, K75P/viewing-illuminant correctness, RAW WB adaptation research, static-link
license materials, fail-closed production signing, API 36/AGP toolchain migration, JNI/parser
hardening, one OutputDescriptor, transactional storage, a global memory budget, a digest/device
gate, exact idle pre-render, spatial scratch/thread/grain/writer work, accessibility/E2E, privacy,
build hygiene, final docs and signed-candidate SLO proof. See the canonical plan for linked titles
and dependency order.

A full three-lane sweep (Kotlin app layer · native engine · build/CI/docs) of everything
**not complete**: user-facing bugs, silently-lying API surface, coverage holes, stale docs,
and release follow-ups. Grouped by severity (🔴 notable · 🟡 worth doing · ⚪ minor).
Status snapshot, not a commitment. The 2026-07-02 audit this replaces — and its long
resolved-item history — lives in this file's git history; everything still open from it is
re-verified and carried below.

Owner decisions and implementation items are tracked as native child/dependency issues under the
Wayfinder map so they do not rot here.

## 🔴 Notable — tracked as issues

- **Release due** — the whole #120/#121/#122 perf line (−61% export RSS, O(1) lookups,
  full parallelization) is merged but unreleased; `v0.9.0` predates it. Also
  `release.yml` builds on JDK 17 while every gate runs 21. → [#138]
- **Editor session is not fully durable** — `rememberSaveableStateHolder` now fixes the original
  same-process sub-screen navigation repro, but source/params/masks/history/process recreation,
  stale-render suppression and lifecycle ownership still need the complete contract. → [#139]
- **Ultra HDR export is SDR + a flat 1×1 gain map** (`ImagePipeline.kt`
  `attachNeutralGainmap`, fixed `ratioMax=1.6`) — format name over-promises. → [#140]
- **Mask compositor allocates unbounded ART-heap planes at export resolution** — ≈1 GB
  per active mask on a 50 MP export (`MaskCompositor.kt` / `MaskSpatial.kt`); the one
  seam never moved off-heap/banded. → [#141]
- **`tc_lut_cache` grows without bound under non-default filming params** — ~885 KB per
  distinct slider value (spectral blur, UV/IR floats folded raw into the key), never
  evicted; default path bounded. → [#142]
- **Params that silently lie** — `MALLETT2019` renders Hanatos while busting the film
  memo; gamut `kOff` ≡ `kLegacyClip` (clip is unconditional); film-side
  `glare_*` (4 fields) unconsumed yet `glare_active` busts the memo; `enlarger_lens_blur`
  unwired; JNI `call_*` fallbacks overwrite physical defaults on a missing getter and the
  ~126-getter marshaller has zero tests. Comments now tell the truth; behaviors need the
  decision. → [#143]
- **README opens by declaring the project abandoned** (owner's own statement; contradicts
  the active tree) + omits v0.9.0's gamut features and the grain-reproducibility
  disclosure. Owner's call. → [#144]

## 🟡 Worth doing (tracked here)

App layer:
- ~14 `scope.launch` sites on `lifecycleScope` keep running after the editor leaves
  composition (only magnifier/roi jobs are cancelled) — needs a per-call-site
  cancellation policy (`MainActivity.kt:268` and the export/preset/bake launches).
- `SingleFlight` retains the completed `Deferred`'s `LinearImage` (~36 MB) after
  `invalidate()` (`EngineHelpers.kt:332-348`); `GradeCache.clear()` exists but nothing
  calls it on source switch (a ~12 MB pristine buffer for the previous image survives).
- Export success sets the **full-resolution** bitmap as the live preview
  (`MainActivity.kt` export onSuccess) — 200 MB live bitmap after a 50 MP FULL export.
- `.mcraw` recognized with a 106-line parser + tests but no frame decode wired (import
  shows "coming"); masks JSON has no versioned interop schema (`MaskJson.kt`).
- Manifest hardcodes `Theme.Material.Light` → white cold-start flash in dark mode
  (needs a DayNight theme + `values-night`).
- Eyedropper multi-sample still outstanding (`masks/Mask.kt`).

Engine:
- **Unwired subsystems' fate**: `gpu/vulkan_compute.*` is now WIRED (GPU M1, #146:
  persistent two-kernel host driving the preview scan offload; the `cctf_encode`
  kernel alone remains call-site-free); `ml/segmentation.*` is a stub whose CMake
  option `SPK_ENABLE_LITERT` is a build trap (`#error`); `kernels/half.cpp` ships
  dead in the .so and burns a CI slot (PERF_ROADMAP #3 infra that never landed).
- `io/npy_lut.cpp` resizes to the header-declared shape **before** the truncated-payload
  bounds check (latent 8 TB-allocation path; bundled-only assets today). The two parsers
  (`json_min.h`, `npy_lut.cpp`) have no direct/negative tests; `model/glare.cpp` is
  reached by no e2e (every CI case runs grain-off).
- Dead ported helpers with zero call sites (candidates for deletion or tests):
  `conversions.{light_to_density, apply_matrix3, matvec3, density_to_transmittance}`,
  `spectral.{spectral_wavelength_nm, xyz_from_spectrum}`, `color_output.srgb_cctf_encode`,
  `gaussian.um_to_pixels`, `spectral_upsampling.fetch_coeffs`, `lut3d.apply_lut_3d_pchip`
  (+ `build_lut_3d`'s literally-named `fn_inputs_unused` parameter),
  `print_digest.resolve_neutral_cc(path,…)` (referenced only by the unrun
  `test_printing`).
- `test_scanning` / `test_printing` / `test_spectral_upsampling` remain local-only
  (subsumed by the e2e goldens — fine, but `resolve_neutral_cc(path,…)` hides there).
- JVM marshaller tests (Kotlin → `spk_params`) — #143 item 5 once the semantics decision
  lands.

Build / CI / docs:
- `app/lint-baseline.xml` (2026-06-04): 33 entries of which 26 are dependency-staleness
  and one is a baselined **correctness** check (`Instantiatable` on MainActivity);
  dependencies pinned Oct/Nov 2024 (AGP 8.7.3, Kotlin 2.0.21, Compose BOM 2024.10.01);
  2 deps bypass the version catalog (the two `UseTomlInstead` entries).
- R8 Stage-2 obfuscation + `shrinkResources` still open; no push-triggered emulator
  smoke (manual `android-emulator` only).
- `python-lint` byte-compiles 2 of ~9 golden generators (workflow YAML — owner-gated).
- `docs/screenshots/*.jpg` = 3.1 MB rendered at width=200 (~90% reducible);
  `HANDOFF.md` regrows between manual compressions (136 KB once; watch it);
  `docs/PRESETS.md` has 7 undocumented presets + display-name drift on ~8;
  v0.9.0 is a lightweight tag (checklist prescribes annotated), v0.6.x never tagged,
  old `v0.1/v0.2` local tags lost; two stale `worktree-agent-*` branches.

## ⚪ Minor / by-design (kept, disclosed)

Enlarger lens blur GatedBlock (no oracle call site); MALLETT2019 GatedBlock disclosure;
DIR-gamma GatedBlock (film-stock-overwritten; could be hidden instead of dimmed);
Input-color-space GatedBlock; scan B/W-correction + GPU-preview ad-hoc dimming
(inconsistent with GatedBlock pattern); `RawCoilDecoder`'s naive ACES→sRGB (gallery
thumbs only); glare-on-print stochastic → unparityable by design; `bench_stages` is
explicitly not a gate; grain-field change vs ≤v0.8.0 disclosed in CHANGELOG (⚠).

## Fixed in the 2026-08-27 audit batch (PR #137)

- **#119 unblocked (agent side)**: manifest `<profileable android:shell>`, export
  start/duration breadcrumbs, `tools/baseline/baseline_wizard.sh` + README.
- **Parity gate 36 → 38**: the two statistical grain gates (`test_grain`,
  `test_grain_sublayer`) — the only stage byte-goldens can't cover — verified green
  locally and wired into `ci.yml` + `run_engine_parity.sh` (drift guard intact).
- **JNI exception boundary**: all six allocating entry points are function-try-blocks;
  `std::bad_alloc` → catchable `java.lang.OutOfMemoryError` instead of SIGABRT.
- **`apply_highlight_boost` boost loop parallelized** (the map #122 missed; 1-vs-8
  already gated by `test_highlight_boost_e2e`).
- **Engine comment truth pass**: `kernels/parallel.h` (users + grain scheme),
  `spektra.cpp` tc_lut key/growth, `spektra.h` (M0 fossils, gamut ordinals 3/4 shipped,
  `disable_buffer_memos` direct-f32 role), `gamut_compression.h` kOff reality,
  `params.h` sublayer wiring, filming/autoexposure f32 notes, CMake test-list → pointer
  to the authoritative ci.yml, dangling `CAMERA_PLAN.md` ref.
- **App fixes**: recipe auto-save/draft-render/preset-blend/uri-permission failures now
  logged (were silent); `HowToUseScreen` BackHandler (back no longer exits the app from
  the guide); export temp files deleted on all paths (try/finally); LUT `.cube` write
  moved off the main thread; empty custom export size now means full resolution (+ unit
  test); 11 stale comments corrected (mask pipeline reality, gainmap in-place truth,
  clipboard scope, garbled fragments); dead code deleted (`HistogramCard` + policy
  comment re-anchored, `TooltipIconButton`, old `Recipes.save`, `UpdateInfo.apkUrl`,
  `StockCatalog.isPrintKind`, `BuiltInPresets.byId`, `PRINT_ILLUMINANTS`).
- **Docs**: 12-file correction batch — gate-count cluster (now 38), stray tool-call XML
  removed from RELEASE_CHECKLIST, workflows README rewritten from its M0 stub,
  false LiteRT claims fixed, broken preset id, research docs de-orphaned, version drift
  (v0.8.0 → v0.9.0 refs), CLAUDE.md CI section (r8-smoke, lint, 16 KB gate).

*Film modeling powered by spektrafilm (GPLv3).*
