# Roadmap

> **Production/readiness execution moved (2026-08-29):** see
> [EXECUTION_INDEX.md](EXECUTION_INDEX.md),
> [PRODUCTION_READINESS_PLAN.md](PRODUCTION_READINESS_PLAN.md),
> [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md), and the
> [Wayfinder map: production-ready Spektrafilm + 1–2 s exact export](https://github.com/thetechgeekko/Spektrafilm-android/issues/164). The milestone
> history below is retained, but it is not the release-blocker or 1–2 second export plan.

> **ℹ️ Historical milestone record through v0.9.0 (versionCode 11).** The milestone *structure* below is still a useful
> overview, and the per-item status markers have been corrected to match the merged state:
> - ✅ **v0.8.0 wave:** masking v1, Lightroom-style export sheet (incl. TIFF32F/scene-linear),
>   `.cube`/CLF LUT export, WB wave, opt-in gamut compression (output ACES-RGC + Oklch
>   perceptual, input xy-locus), per-effect spatial gating + print-route spatial/grain,
>   engine memos, parity suite now 39 cases.
> - ✅ **`use_enlarger_lut` is wired** (2026-06-01) — opt-in/default-off, default path
>   byte-identical, gated by `test_enlarger_lut_e2e`. No reserved engine LUT flag remains.
> - ✅ **AAssetManager APK-direct asset load is done** (2026-06-01) — the engine reads profiles,
>   the spectral LUT, and neutral filters straight from the APK; the first-run extraction to
>   `filesDir` is skipped. This was the last M3 remainder; it is closed.
> - ⚠️ **Release signing is now fail-closed, but release qualification remains open** — a local
>   `assembleRelease` without an explicit keystore emits an unsigned APK. `release.yml` hash-binds
>   that non-secret candidate, then aligns/signs only inside the protected Environment and verifies
>   the remote draft before publication. The next release still needs the remaining map gates.
>
> Treat M3 and M4 as complete (see the v0.7.0 progress note below).

Milestones are vertical slices. Each ends with something demonstrable and a parity gate.

> **Scope & fidelity (confirmed):** we port the **true spektrafilm engine in full**, to a
> **bit-exact** parity bar (the GUI is just a shell over this engine — see `MOBILE_STRATEGY.md`).
> The app exposes the whole `RuntimePhotoParams` control surface, plus Lightroom-informed
> additions: **non-destructive recipe/sidecar editing** and a **proxy-preview vs full-res-export**
> model. The parity-bearing engine is **CPU C++/NDK** (GPU can't be bit-reproducible across
> vendors); a GPU preview accelerator is an optional M6 item. Only EXR file I/O is deferred —
> 32-bit-float TIFF (including scene-linear untagged TIFF32F) shipped in the export sheet.

> **Progress note (2026-05-29):** beyond M0, a parallel scaffolding wave has landed real,
> compiling code ahead of the host bootstrap: the C++ engine's pure-math + kernel layer
> (`engine/spektra-core/src/main/cpp/{model,kernels}`: Gaussian/interp kernels, density curves,
> emulsion, conversions, glare, and the **corrected 81-sample 380–780@5 spectral grid + CIE
> CMFs** — a grid bug from the initial map was caught and fixed here), the `lib:libraw` decode
> module (M2), the `feature:film-emulation` Compose UI (M5), and the `tools/parity` golden-vector
> harness (the M3/M4 gate). These compile/are structurally complete in isolation; they wire
> together and build as an app at **M1** (host bootstrap).
>
> **Progress note (2026-05-30 — v0.3.0 wave):** a major feature wave landed on the dev branch
> (PR #8). Engine stages: **auto-exposure** (7 metering patterns, bit-exact, parity-gated) and
> **diffusion filters** (bit-exact, parity-gated) are now DONE. Print path is **generalized to
> all film/paper pairs** via native `print_digest` + `neutral_print_filters.json` (was Portra/
> Ektar baked-only; `print_ektar` golden proves the second pair). App features: 16-bit TIFF
> export (live), Lightroom-style Auto-exposure UI, profile-curve browser, non-destructive
> recipe/sidecar layer, engine/render status pill, full source EXIF copy on export, Google Ultra
> HDR export, typed display-referred fallback for every Compression-8 DNG
> (including float sources whose LibRaw bitmap route would quantize), and a
> **major Lightroom-style UI redesign** (edge-to-edge,
> pinned preview + 90° rotate, horizontal scrollable category bar, inline panel, back navigation).
> Issue #6 is essentially resolved: lens blur (camera+scanner) and scanner LUT acceleration are
> now wired too (the latter opt-in/default-off, default path byte-identical). Glare-on-print is
> wired but default-off (stochastic, so not bit-exact). That wave's “no false UI surface” claim has
> since been reopened by the current inert/control-semantics audit ticket.
>
> **Progress note (v0.7.0, `versionCode 9`):** the two remaining "reserved" items from the v0.3.0
> wave have since landed. **`use_enlarger_lut` is wired** (2026-06-01) — the enlarger-side 3D LUT
> now accelerates the print-expose spectral integral (mirroring the scanner LUT); it is
> opt-in/default-off so the default path stays byte-identical (`test_simulate_e2e print_portra`
> unchanged at 5.11e-07), and it is parity-gated by `test_enlarger_lut_e2e`. **The AAssetManager
> APK-direct asset path is done** (2026-06-01) — the engine loads profiles/LUT/filters from the
> APK with no first-run extraction (on-device arm64 parity still `ALL PASS`). On the release side,
> `release.yml` ships a signed APK on a `v*` tag; local release builds use an explicitly supplied
> `keystore.properties` or remain unsigned, never debug-signed. **R8 is now
> enabled** (2026-06-04) — `isMinifyEnabled = true`, Stage 1 *shrink only* (`-dontobfuscate`) with
> keep-rules for the four name-based JNI boundaries and enum persistence (`app/proguard-rules.pro`);
> Stage 2 obfuscation is deferred. Glare, enlarger blur and other exposed/control semantics now
> require the explicit implement/remove decisions in the production-readiness map rather than a
> blanket “by design” closure. The `apply_hanatos2025_*` window/surface toggles are WIRED
> (`test_hanatos_surface_e2e`) — tracked in `docs/AUDIT.md` + `docs/ENGINE_WIRING_PLAN.md`.
>
> **Milestone progress (v0.5.0 → v0.6.x → v0.7.0):** v0.5.0 landed the Lightroom-feel editor wave;
> the v0.6.x line fixed the full-resolution / off-heap RAW export OOM (PR #56) and bumped through
> `versionCode 5→8`. v0.7.0 (`versionCode 9`) closed the last M3/M4 remainders — `use_enlarger_lut`
> wired+gated, the AAssetManager APK-direct asset path, the hanatos2025 adaptation window/surface
> (PR #69) and `spectral_gaussian_blur` (PR #68) now wired, the parity oracle pinned to `c1d0e44`
> (PR #67) — and enabled signed releases via `release.yml` plus R8 Stage-1 shrink. **M3 and M4 are
> complete.**

## M0 — Foundation  ✅ (this commit)
Architecture decided, both repos mapped, port plan written, RAW/licensing strategy fixed,
engine API contract drafted. No build yet.
- **Done when:** docs + engine contract reviewed and merged.

## M1 — Runnable app  ✅ (v0.1.0)
Shipped as a **standalone Compose app** wrapping the engine (pivot from vendoring ImageToolbox —
see `DECISION.md`). Android SDK + NDK build verified; `:app:assembleDebug` produces a 23 MB APK
with `libspektra.so` for all ABIs + bundled assets; CI assembles it on every push.
- **Done:** app builds + installs; profile pickers, scan/print toggle, exposure, live render.
- Follow-up: richer editing UI + optional ImageToolbox host integration (engine module is ready).

## M2 — RAW/DNG decode (`lib:libraw`)
LibRaw compiled for all ABIs; JNI decode of RAW/DNG → linear 16-bit RGB with rawpy-parity
options; WB modes (as-shot/daylight/tungsten/custom).
- Decode a DNG and a CR2/NEF to a linear buffer; verify against rawpy output on the same file.
- Optional: register `RawDecoder` in Coil for full-res gallery open.
- **Done when:** a DNG opens in-app at full resolution; linear values match rawpy within tol.

## M3 — Engine core (`engine:spektra-core`) — scanning path first  ✅ complete
Port the cheapest end-to-end path to prove the boundary: `scan_film` (skip print).
Stage order: params/profiles → density curves/emulsion → spectral upsampling → filming →
scanning. Ship 28 profiles + LUT assets.
- Golden-vector harness green for: film raw, film density, scan RGB.
- **Done when:** `simulate(scan_film=true)` on a test image matches spektrafilm within tol.
- **Landed (2026-05-29):** RAW white-balance UI (Temperature/Tint sliders + WB-mode dropdown
  + reset-to-as-shot) — see progress note below. `scan_film` path itself was already
  bit-exact in v0.1.0. Non-sRGB output spaces are now wired (Settings default + per-image
  dropdown → all six spaces, gated by `test_output_spaces`). The in-APK AAssetManager path —
  the last M3 remainder — landed 2026-06-01, so **M3 is complete**.

> **Progress:** the Python engine runs headless here as a live oracle and real goldens are
> committed (`tools/parity/goldens/`). The **entire `scan_film` path is ported and bit-exact
> vs the oracle**, stage by stage:
> - profile JSON loader, density curves, emulsion, conversions, glare
> - **Hanatos2025 spectral upsampling** (RGB→spectrum, max_abs ≈ 1.1e-7)
> - **filming stage** (RGB→raw→develop incl. **DIR couplers**): `film_log_raw` max_abs ≈ 1.2e-7,
>   `film_density_cmy` ≈ 2.4e-7
> - **scanning stage** (density→RGB): `final_rgb` max_abs ≈ 6e-8
>
> Full `libspektra.so` links (engine + JNI + all scan-path sources).
>
> **`spk_simulate(scan_film=true)` now runs end-to-end** through one C-API call — orchestrating
> profile load → filming → develop(+couplers) → scanning — reproducing `final_rgb` at
> **max_abs ≈ 7.45e-8**. The JNI bridge (`nativeCreate/Destroy/ListProfiles/Simulate`,
> SpektraParams↔spk_params marshaling, direct-ByteBuffer I/O) is implemented and `libspektra.so`
> exports all four `Java_com_spectrafilm_engine_*` symbols. **The scan_film engine is callable
> from Kotlin.**
>
> [Update: non-sRGB output spaces and the grain/halation/glare toggles were since wired; M4
> print route + spatial/stochastic effects landed and are parity-gated. The in-APK
> `AAssetManager` path — the last remainder noted here — landed 2026-06-01; the engine no
> longer needs an extracted asset dir.]
>
> **M3 backlog item landed (2026-05-29):** **RAW white-balance UI** — Temperature/Tint sliders
> + a WB-mode dropdown (as-shot / daylight / tungsten / custom) + reset-to-as-shot, shown only
> for RAW/DNG sources. Wired to the existing LibRaw decoder: changing the mode or sliders
> re-decodes the preview automatically. Default (as-shot) behaviour is unchanged; parity
> preserved. (Issue #6 note: the `rawTemperature`/`rawTint` fields were already in `RawDecoder`
> — this completes the UI surface for that path.)

## M4 — Full negative→print→scan + look effects  ✅ complete
Add printing stage, DIR couplers, grain, halation/scatter, glare, diffusion filters.
- Golden vectors green for print density + final RGB across ≥3 film/paper pairs.
- **Done when:** full pipeline matches spektrafilm baselines; grain visible on upscaled crops.
- **Landed (issue #6 — resolved):** crop/resize, auto-exposure, diffusion, lens blur
  (camera+scanner), scanner LUT acceleration (opt-in), and the enlarger LUT (`use_enlarger_lut`,
  opt-in/default-off, gated by `test_enlarger_lut_e2e`) all ported & parity-gated. Only minor,
  by-design remainder: glare-on-print is default-off and stochastic (can't be bit-exact gated).

> **Progress:** the **printing stage** (enlarger spectral calc + dichroic Y/M/C filters +
> print expose/develop) and the full **negative→print→scan route** are ported and **bit-exact**
> vs the `print_portra` goldens: `print_density_cmy` max_abs ≈ 2.4e-7, `final_rgb` ≈ 4.2e-7;
> end-to-end `spk_simulate(scan_film=false)` ≈ 5.6e-7. DIR couplers already done (M3).
>
> **Print path generalized (2026-05-30):** the per-(film,paper) neutral dichroic CC values and
> midgray exposure factor are now resolved at runtime by a native `print_digest` (digest of
> `neutral_print_filters.json` + filming-midgray balance). The previous baked-for-portra/ektar
> limitation is gone — any film/paper profile combination is now valid. A new `print_ektar`
> e2e golden proves the second pair; both `print_portra` and `print_ektar` parity tests pass
> (host suite 12/12 PASS).
>
> **Diffusion-filter stage (2026-05-30, issue #6):** spatial diffusion filters (halation/
> scatter coupling, DIR coupler diffusion) ported and parity-gated (`diffusion_bpm` golden).
>
> **Auto-exposure stage (2026-05-30, issue #6):** all 7 metering patterns ported and parity-
> gated (`scan_portra_autoexp` golden). JNI forwards `auto_exposure_method`.
>
> **M4/M-geometry item landed (2026-05-29) — partial fix for issue #6:** **crop/resize geometry
> stage ported** (bit-exact). The previously-inert `IOParams` crop fields (`crop`,
> `crop_center`, `crop_size`) and the cubic `upscale_factor` are now a live pre-process stage
> running before filming in both the scan and print routes, matching spektrafilm's
> `_preprocess` step (`runtime/services/resize.py` + `utils/crop_resize.py`). Defaults remain
> a strict no-op; parity on all existing goldens is byte-identical. A new `scan_portra_crop`
> golden gates the non-default path (max_abs ≈ 2e-7).
>
> **#6 is resolved** — lens blur (camera+scanner) and scanner LUT acceleration are now wired and
> parity-gated (LUT opt-in/default-off, default path byte-identical). The crop/diffusion UI "not
> yet active" badges were removed. The **enlarger LUT (`use_enlarger_lut`) was since wired too**
> (2026-06-01, opt-in/default-off, gated by `test_enlarger_lut_e2e`). Minor remainder:
> glare-on-print is wired but default-off (stochastic, not bit-exact). Downscale
> (`upscale_factor < 1`) anti-aliasing prefilter is DONE — parity-gated by `test_downscale`.

## M5 — UI/UX + non-destructive editing  ✅ complete
Shipped in the standalone `:app` module (not `feature:film-emulation`), and has since been
exceeded (tone curve, masking v1, WB wave, export sheet).
Full `RuntimePhotoParams` control surface (camera/enlarger/scanner/grain/halation/couplers/
glare/diffusion/IO/settings), film+print profile pickers, **proxy preview vs full-res scan**,
before/after compare, export with EXIF+ICC.
- **Non-destructive recipe layer:** edits are a serialized `SpektraParams` sidecar keyed to the
  source; original RAW untouched; re-render on view/export; presets (28 stocks + saved params)
  and history (Lightroom-informed — see `MOBILE_STRATEGY.md`).
- Reuse ImageToolbox components (zoomable canvas, sliders, sheets, compare, picker).
- **Done when:** end-to-end on-device: pick RAW → tune (proxy) → export full-res, no code; edits
  persist as a recipe and reopen non-destructively.

## M6 — Performance, GPU accelerator & polish
Tile/threading, native SIMD (NEON), preset save/load, profile catalog UI, About/credits with
attribution, APK-size review. Optional **GPU preview accelerator**: see `docs/PERF_ROADMAP.md` #1
(that doc owns the GPU plan). 3D LUT export DONE (`.cube` + CLF v3, 17/33/65 sizes). Only EXR
file I/O is deferred — 32-bit-float TIFF shipped in the export sheet.
- **Done when:** proxy preview is interactive on a mid-range device; full scan acceptable; GPU
  path (if shipped) matches CPU within visual tolerance.

> **Landed (M6 threading):** the per-pixel hot loops (`expose` spectral upsampling, `scan`
> density→RGB, `print_expose`) are now multithreaded via a deterministic fork-join helper
> (`kernels/parallel`). Disjoint contiguous pixel chunks keep the result **byte-identical for any
> worker count** (parity gate preserved; new `test_parallel` asserts 1- vs 8-thread equality, and
> the `engine-parity` suite runs multithreaded). ~3.2× on a 12 MP scan / 4 cores. Stochastic grain
> + spatial blurs stay serial.
>
> **Native SIMD (NEON) — landed (2026-05-31, see `docs/DECISION.md`).** The `pow(10,−spectral)`
> that dominates (~79%) the `scan()` and `print_expose()` spectral integrals is now evaluated with
> a portable vector `exp10` (`kernels/exp10.h`) that lowers to **NEON `fmla v.2d` on arm64**
> (verified by disassembly) and to SSE2/AVX on x86. It matches `std::pow` to ≤4 ULP and is
> **byte-identical at the float32 output** (0/750k mismatches; goldens' `max_abs` unchanged at
> 5.97e-08; `test_parallel` still 0 across 1↔8 threads). ~1.85× on a 12 MP scan on x86 (more modest
> on 2-wide NEON f64; armv7 scalarises) → **~6× vs the original scalar/single-thread baseline** with
> threading. `expose()` (gather-bound) and the per-pixel `10^log_xyz` round-trips stay scalar.
>
> Still open in M6: memory tiling for very large RAW. The downscale AA prefilter and
> profile-catalog UI shipped; an experimental default-OFF GPU LUT preview exists
> (`LutGpuPreview.kt`) pending on-device validation (see `docs/PERF_ROADMAP.md` #1). **APK-size
> review landed** (2026-06-04): R8 Stage-1 shrink is now on (`isMinifyEnabled = true`, no
> obfuscation, JNI/enum keep-rules — `app/proguard-rules.pro`); Stage-2 obfuscation deferred.

## M7 — Final polish & presentation (requested)
The "do this when literally everything else is done" list:
- ✅ **Welcome / onboarding screen** — shipped in v0.2.0. A multi-page intro covering the
  project story, how the app works, and a guided tour. The last page links to Settings and the
  "Report an issue" GitHub shortcut.
- ✅ **Settings page** — shipped in v0.2.0. Default output color space, preview resolution,
  default film/print, export format/quality + save location, theme (light/dark/dynamic), reset,
  and a "Report an issue / Flag on GitHub" entry opening
  https://github.com/thetechgeekko/Spectrafilmandroid/issues/new.
- ✅ **In-app About section** — shipped in v0.2.0. Credits (spektrafilm/ImageToolbox/
  colour-science/LibRaw), the pixls.us dedication, author links (Akshay — Instagram/YouTube),
  version, license, and source link.
- **GitHub repo "About"** — repository description, topics, and homepage must be set manually
  at https://github.com/thetechgeekko/Spectrafilmandroid/ by the maintainer (the env cannot
  push via the API). Ready-to-paste text is in `docs/RELEASE_CHECKLIST.md`.
- **Remaining (M7/polish):** the gated engine stages are live, but the current audit has reopened
  glare/enlarger-lens and other exposed-control semantics as explicit implement/remove decisions.
  Release signing infrastructure exists: `release.yml` publishes a production-signed APK on a
  `v*` tag. Local release builds without an explicit release keystore are unsigned; the protected
  workflow signs only a qualified, hash-bound candidate. **R8 is enabled** (Stage-1 shrink, no obfuscation,
  JNI/enum keep-rules); R8 Stage-2 + `shrinkResources`: see `docs/AUDIT.md` §D (which owns that
  open item).

## Cross-cutting
- CI: build all ABIs; run golden-vector parity tests; lint.
  → Implemented in `.github/workflows/ci.yml` (see `.github/workflows/README.md`): the standing
  jobs are `engine-native`, `engine-parity` (39 cases), `parity`, `python-lint`, `android`
  (`testDebugUnitTest` + assemble + 16 KB check), and `android-emulator` (manual dispatch only).
- Each engine PR cites which golden vectors it turned green.
