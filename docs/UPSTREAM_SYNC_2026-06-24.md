# Spektrafilm Upstream Sync Plan — 2026-06-24

> **Historical upstream snapshot.** Strategy A was executed, but this file does not prove parity
> with latest upstream and its 38-gate references predate the current 39-case matrix. Current
> upstream coverage is owned by the live parity-manifest ticket linked from
> [EXECUTION_INDEX.md](EXECUTION_INDEX.md). Oklrab subsequently landed; the older “still open” list
> below is preserved only as the dated plan's input.
>
> **STATUS 2026-07-02: Strategy A executed.** Shipped opt-in/default-OFF: output aces_rgc GC
> (`gamut_out_aces`), the Oklch perceptual output GC (`gamut_out_oklch`, PR #111 — P2 #6 slice 1),
> input xy locus bake (`gamut_in_xy`), print-curve morph (`print_curves_morph`), plus the
> np_interp fix (`np_interp` gate). Still open: the remaining perceptual output-GC algos
> (oklrab/jzazbz/cam16ucs = P2 #6 slices 2-4, reserved enum slots `kOklrab=4`/`kJzazbz=5`/`kCam16ucs=6`);
> Strategy-B rebaseline tracked as roadmap #20-27 (trigger unfired as of 2026-07-01). Parity gate
> is now 38 tests.

> Provenance: produced by two analysis workflows over the upstream repo at
> `/home/user/spektrafilm`. Port baseline = oracle `c1d0e44` (the SHA the Android
> engine's golden vectors are byte-pinned to). This document has two parts:
>
> - **Part A — Merged upstream main** (`c1d0e44 → 3bb2c2d`, 37 commits): the
>   actionable sync. No new film stocks; the new material is a profile refit +
>   default-ON engine/color features + desktop tooling.
> - **Part B — Experimental unmerged branches** (`reflectance-upsampling-methods`,
>   the superset of `dev`/`non-linear-couplers`): the author's next-generation R&D
>   (B&W N-channel, new spectral upsampling, non-linear couplers, grain rework).
>   Track-only — nothing is a self-contained opt-in port today.
>
> Bottom line: pursue **Strategy A** (add Part-A features strictly opt-in / default-OFF,
> each gated by its own newer-oracle golden, so the `c1d0e44` default render stays
> byte-identical and no user's film look changes). Recommended first port:
> **print density-curve morph** (best value-to-risk). Defer the profile refit and any
> default-path change to a single deliberate rebaseline release. Track all of Part B.

---

## Part A — Merged upstream main (`c1d0e44` → `3bb2c2d`)

## 1. Executive summary

The Android port already bundles **all 28 upstream film/paper profiles**, so there are **no new stocks to copy** — the headline "new stuff" in the 37-commit range is not new emulations. It is three things: (a) a **profile data refit** (commit `ad5c8d2`: every stock's `density_curves` / `density_curves_layers` / `density_curves_model` re-fit, plus a `neutral_print_filters.json` refit — deltas up to 0.43, ~570× the parity tolerance); (b) **new engine/color features** that are *default-ON upstream* — input + output gamut compression (`utils/gamut_compression.py`, CAT02→CAT16 + spectral-locus soft-clip + CAM16-UCS/OkLab/aces_rgc output compression replacing the final hard clip) and a coupled-gamma **print-curve morph** (`utils/morph_curves.py`); and (c) **desktop-only tooling** (`spektrafilm_lut_creator`, the `param_manifest.py` GUI refactor, OCIO config emit, the `topology.py` tap dispatcher / `lut_mode`).

The engine is byte-pinned to `c1d0e44` and the default render path must stay byte-identical unless the team deliberately moves the baseline. That is the **central decision**: either (A) keep the default pinned to `c1d0e44` and add the new features strictly **opt-in / default-OFF**, each gated by its own newer-oracle golden, so no existing user's film look changes; or (B) **move the whole baseline to `3bb2c2d`** — import the refit profiles, adopt all the default-path engine changes, regenerate *every* golden and re-validate every stage. Strategy (B) changes the rendered look of every photo for every existing user and invalidates the entire `c1d0e44` golden set.

**Recommendation: pursue Strategy (A).** Ship the high-value, parity-safe features as default-OFF opt-ins (output `aces_rgc` gamut compression first, then the print-curve morph, then the input-side CAT16/locus bake). Defer the profile refit and any default-path change (Strategy B) to a single deliberate, separately-scoped rebaseline release — it is not something to do piecemeal, because the refit and the new default-on features are entangled on the same render path.

---

## 2. Two strategies

### Strategy A — Opt-in features (default byte-identical)

Keep the `c1d0e44` baseline. Add each new feature behind a new **default-OFF** flag on `SpektraParams` / `spk_params`, exactly as `use_scanner_lut` / `use_enlarger_lut` / `spectral_gaussian_blur` / `apply_hanatos_surface` were added. Each feature is proven by its **own** golden generated from a `3bb2c2d` oracle (the first goldens in the repo not pinned to `c1d0e44`), while a paired assertion confirms the feature-OFF path still reproduces the existing `c1d0e44` golden byte-for-byte.

- **Pros:** zero change to any existing user's render; the 34-test (as of 2026-07-02; see ci.yml) `engine-parity` gate stays green untouched; features land incrementally and independently; no coordinated mass golden regeneration; reversible.
- **Cons:** the port carries an OFF branch that deliberately *differs* from upstream's OFF behavior (e.g. upstream deleted the final `clip(0,1)`; the port keeps it on the OFF path); golden provenance is now multi-SHA (must be documented so a future regen doesn't "fix" the pin back to `c1d0e44`); the default look stays "old" vs. upstream's refit.
- **Effort:** per-feature S–XL (see inventory). The cheap wins (output `aces_rgc`, grain defaults) are S–M; the perceptual color paths (CAM16-UCS) are XL.

### Strategy B — Full rebaseline to 3bb2c2d

Move the pin to `3bb2c2d`: import the `ad5c8d2` refit profiles + `neutral_print_filters.json`, adopt the default-path engine changes (CAT16 + input gamut compress + output gamut compress + print `develop_print_morph` replacing the tabulated interp + grain default refit + scanning clip removal), regenerate **all** `.spkvec` + stage goldens from a `3bb2c2d` oracle, and re-pin `tools/parity/setup_env.sh` off `c1d0e44`.

- **Pros:** the port matches upstream exactly going forward; only one golden set; no permanent OFF-branch divergence to maintain; new features become "free" defaults.
- **Cons:** **changes the rendered look of every photo for every existing user**; invalidates the entire `c1d0e44` golden corpus in one shot; the print path semantics change (analytic `density_curves_model` eval replaces tabulated-table interp — measured 0.0033 max_abs / 33× tolerance even at morph-OFF) and the Android-only "Print gamma factor" slider is deleted upstream; requires porting CAM16-UCS + C_max bisection infra to even reproduce the new default; the documented `SPEKTRAFILM_ORACLE_DRIFT=a9bccd6` raw-scaling change sits between the SHAs, so this is an all-or-nothing semantic move.
- **Effort:** XL+ and high-risk; must re-validate every stage and smoke-test a release build on device.

---

## 3. Change inventory

| Area | Android relevance | Already in port | Default-path parity impact | Port approach | Effort | Value |
|---|---|---|---|---|---|---|
| Input + output gamut compression (CAT16 + locus bake + CAM16-UCS/OkLab/aces_rgc) | runtime-engine | no | changes-default-output | opt-in (default-off) | XL | medium |
| Print density-curve morph (`morph_curves.py` / `develop_print_morph`) | runtime-engine | no | changes-default-output | opt-in (default-off) | L | medium |
| Filming stage: input gamut compress + CAT02→CAT16 + xy-clip removal | runtime-engine | no | changes-default-output | opt-in (default-off) | L | medium |
| Scanning stage: output gamut compress / soft-clip (replaces final clip) | runtime-engine | no | changes-default-output | opt-in (default-off) | XL | low |
| color_reference + spectral_lut_compute (CAT16 / input GC; gamma_factor=1.0 fix) | runtime-engine | no | changes-default-output | opt-in — blocked (needs profile-isolation tooling) | L | medium |
| spectral_upsampling + calibration_targets (CAT16 + locus bake) | runtime-engine | no | changes-default-output | opt-in — blocked (oracle drift + refit) | XL | medium |
| Grain model refit (param rename + default values) | param-wiring | yes (math) | none (grain off in gate) | opt-in / optional | S | low |
| DIR couplers (None-guard for LUT bake) | runtime-engine | yes | none-default-off | skip | S | low |
| Diffusion (`sigma_um<=0` guard) | runtime-engine | yes | none-default-off | skip | S | low |
| params_schema + params_builder + pipeline + profiles/io (taps, lut_mode) | param-wiring | partial | none-default-off | skip (port has own tap/bake) | S | low |
| Profile JSON refit (28/29 stocks) + neutral_print_filters | data-asset | no | changes-default-output | asset-copy-needs-rebaseline | M | medium |
| Runtime tap inject/collect + `topology.py` + `lut_mode` | desktop-tooling-skip | yes (equiv.) | none-default-off | skip | S | low |
| Targeted fixes: vlog midgray / GC black level / preview-max removal | desktop-tooling-skip | no | none-default-off | skip | S | low |
| Desktop tooling: param_manifest / lut_creator / OCIO | desktop-tooling-skip | no | none-default-off | skip | S | low |

---

## 4. Portable-now (parity-safe) items

Ordered by value/effort (best ratio first). Each keeps the default path byte-identical to `c1d0e44`; each ships behind a default-OFF flag and gets its own `3bb2c2d`-oracle golden plus a feature-OFF == `c1d0e44`-golden assertion.

### Grain default-value refit (S effort, low value — cheapest win)

**What it is.** The grain *math* is already a 1:1 port (`model/grain.cpp`: `layer_particle_model` Poisson-Binomial, `apply_grain_to_density`, `apply_grain_to_density_layers`, `add_micro_structure`); the upstream `grain.py` diff is a pure rename (`agx_particle_*` → `particle_*`, no math change). The only substantive item is the refit **default values**: `particle_scale (0.8,1.0,2.0)→(1.6,1.6,3.2)`, `particle_scale_layers (2.5,1.0,0.5)→(2.0,1.0,0.5)`, `density_min (0.07,0.08,0.12)→(0.03,0.03,0.03)`, `uniformity (0.97,0.97,0.99)→(0.97,0.99,0.97)`.

**Why it is safe.** Grain is excluded from the byte-exact gate — `ci.yml` line 72 ("Grain tests are statistical and run locally"); `test_simulate_e2e.cpp` sets `grain_active=0` on every case. The `c1d0e44` `.spkvec` goldens are all grain-OFF, so no default change can move them. `std::mt19937` can never byte-match numpy/Numba, hence the statistical-only gate.

**Files to change.**
- `engine/spektra-core/src/main/kotlin/com/spectrafilm/engine/SpektraParams.kt` (lines 61–65: the 4 tuples — these are the EFFECTIVE runtime defaults that flow through JNI).
- `engine/spektra-core/src/main/cpp/model/grain.h` (lines 41–58: in-class initializers — keep host-test fallbacks in sync).
- `engine/spektra-core/src/main/cpp/runtime/params.h` (line 199: `grain_density_min` default).
- `engine/spektra-core/src/main/cpp/runtime/params.cpp` (digest comment only, lines 128–134).
- `engine/spektra-core/src/main/cpp/tests/gen_grain_ref.py` + `gen_grain_sublayer_ref.py` (patch oracle attribute reads `agx_particle_*` → `particle_*` before regen, else AttributeError).
- `engine/spektra-core/src/main/cpp/tests/grain_ref_stats.json` + `grain_sublayer_ref_stats.json` (regenerated, committed).
- No change to `spektra_jni.cpp` (getters keep internal `getAgxParticle*` names; JNI already maps to `grain_particle_*`), `ci.yml`, or `setup_env.sh`.

**New params + defaults.** No new params — values only. Keeping the `agx_*` identifiers is harmless; the rename need not be applied.

**Golden recipe.** No `.spkvec` feature golden (RNG mismatch). Gate = the two statistical JSON refs (per-channel mean-preservation `|dmean|<1e-3` + noise-std `±15%`). To regen: checkout `3bb2c2d`; patch the two gen scripts' attribute reads to `particle_*`; from repo root run `PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs python3 engine/spektra-core/src/main/cpp/tests/gen_grain_ref.py` (and the `_sublayer_` variant) — each does `sf.init_params("kodak_portra_400","kodak_portra_endura") → digest_params → reads d.film_render.grain`, auto-picking the refit defaults, operating on the committed no-grain `goldens/scan_portra/film_density_cmy.spkvec`. Re-run `test_grain` + `test_grain_sublayer` locally. **Bump defaults and regenerate the two JSONs together** or the host stat tests drift. Defensible to SKIP entirely (purely aesthetic).

### Output gamut compression — `aces_rgc` only (M–L effort, low-but-clear value)

**What it is.** Upstream `scanning._density_to_rgb` calls `compress_rgb(rgb, output_gamut_compress, output_color_space=...)` on **linear** output RGB before CCTF, and the trailing `np.clip(rgb,0,1)` is deleted. The `aces_rgc` algorithm is a pure per-channel Reinhard knee on achromatic distance `d=(max−c)/max` — **no color-science library, no perceptual space, no C_max table** (~30–40 lines, gauntlet-validated bit-identical to OCIO `ACES_GamutComp13`). This is the cheap subset; defer the four perceptual algos (`oklch`/`oklrab`/`jzazbz`/`cam16ucs`).

**Why it is safe.** Add an enum param whose default sentinel = `LEGACY_CLIP` (the existing `scanning.cpp:361-363` hard clip, unchanged). Do **NOT** adopt upstream's `cam16ucs` default. When `LEGACY_CLIP` → run the existing clip byte-for-byte; when `aces_rgc` → compress on `lin_rgb` between the XYZ→RGB matrix `parallel_for` (ends ~line 311) and `lens_blur` (line 319), and skip the now-redundant clip. The OFF path keeps the clip (it does NOT mirror `3bb2c2d`-off, which has no clip — it must match `c1d0e44`). `aces_rgc` is purely per-pixel ⇒ `test_parallel` thread-invariance is preserved.

**Files to change.**
- `runtime/stages/scanning.h` (add `enum OutputGamutCompress { LEGACY_CLIP, OFF, ACES_RGC }` + field, mirroring `use_lut`/`bw_xyz_correction`).
- `runtime/stages/scanning.cpp` (per-pixel `aces_rgc` pass on `lin_rgb` ~line 311→319; gate the `[0,1]` clip at 362-363; add `reinhard_knee` helper in the anon namespace).
- `spektra.h` (add enum + knee params to `spk_params` ~line 217-222).
- `spektra.cpp` (set in BOTH `ScanningParams` sites — scan_film ~779-790 and print ~1128-1162; default `LEGACY_CLIP` in `spk_default_params` ~1355).
- `spektra_jni.cpp` (read the enum in the `---io---` block ~405-417 via the `enum_ordinal_*` pattern).
- `SpektraParams.kt` (`enum class OutputGamutCompress { LEGACY_CLIP, OFF, ACES_RGC }` near `ColorSpace` line 13; `val outputGamutCompress = LEGACY_CLIP` on `IoParams` ~139-150).
- `tests/gen_output_spaces_ref.py` (add aces_rgc dump → `tests/scan_portra_ref_aces_rgc.spkvec`) + new `tests/test_gamut_compress_e2e.cpp` (modeled on `test_output_spaces.cpp`).
- `.github/workflows/ci.yml` (add `build_run` for the new test in `engine-parity` + a `SPK_NUM_THREADS` 1-vs-8 check).

**New params + defaults.** `output_gamut_compress.algorithm` (default `LEGACY_CLIP`; selectable `OFF`, `ACES_RGC`); `knee=(threshold,limit,power)=(0.0,1.0,6.0)`. (`aces_rgc` has no lightness axis.)

**Golden recipe.** Stage-local, modeled on `gen_output_spaces_ref.py`. Reuse the committed `goldens/scan_portra/film_density_cmy.spkvec` (a `c1d0e44` tap → input stays bit-exact). Reproduce `_density_to_rgb` (density_cmy → `compute_density_spectral` → `density_to_light(D50)` → XYZ integral → `colour.XYZ_to_RGB(sRGB, apply_cctf_encoding=False, illuminant=D50_xy)`), apply `compress_rgb_aces_rgc(rgb, 0.0,1.0,6.0)` on linear RGB (import from a `3bb2c2d` checkout, or reimplement the ~10-line knee SHA-independently), apply CCTF, do **not** clip. Write `tests/scan_portra_ref_aces_rgc.spkvec` (tol `max_abs≤1e-4`, `rms≤1e-5`). Test asserts (1) feature-ON == new golden, (2) feature-OFF == existing `c1d0e44` scan_portra golden. **Reinhard knee math:** `s=limit−threshold; for d>threshold: x=(d−threshold)/s, y=x/(1+x^power)^(1/power), out=threshold+s*y; else identity`. **aces_rgc:** `ach=max(R,G,B); if ach≤1e-12 unchanged; else per channel d=(ach−c)/ach, d'=knee(d), c'=ach*(1−d')` (ach unmodified).

### Print density-curve morph (L effort, medium value)

**What it is.** Upstream `printing.develop` now calls `develop_print_morph(log_raw, log_exposure, density_curves_MODEL, density_curves_morph, profile_type)` — it evaluates the parametric NormCDF `density_curves_model` (`CMY = Σ A_i·Φ(signed((logE−μ_i)/σ_i))`), optionally applies the s023 coupled-gamma morph (7 creative controls), then PCHIP-interpolates at hardcoded `gamma_factor=1.0`. Defaults `active=False`.

**Why it is safe.** Do NOT adopt upstream's wiring (it made the morph mandatory — the analytic model-eval replaces the stored-table interp even at `active=False`, measured 0.0033 max_abs / 33× tol, and deletes the print `density_curve_gamma` knob). Keep the existing `print_develop` (stored-table `interpolate_exposure_to_density` + per-channel `density_curve_gamma`) as the byte-exact default. Add a third opt-in branch behind `use_print_curves_morph` (default false): when ON, build the morphed `(N,3)` table from the profile's `density_curves_model` (already bundled in the JSON assets — needs parsing only), then feed it into the existing `interpolate_exposure_to_density` at `gamma_factor=1.0`. The Android assets carry the `c1d0e44`-fit model (byte-identical to `c1d0e44`, ≠ `3bb2c2d` refit), so no profile refit and no baseline move.

**Files to change.**
- `profiles/profile.h` + `profiles/profile.cpp` (parse `data.density_curves_model.{model_type,centers,amplitudes,sigmas}`, shape `(3,n_layers)` — currently ignored).
- `model/morph_curves.{h,cpp}` (NEW: speed-sort layers by ascending center → fast/mid/slow; `σ'=max(σ/g,0.05)`, `μ'=μ/g`, A fixed; `g=gamma_factor·gamma_{rgb}·gamma_{fast|slow}` — **mid AND slow both use `gamma_factor_slow`**, fast uses `gamma_factor_fast`; normal CDF via `0.5*erfc(−z/√2)`; Gumbel-matched CDF `_GUMBEL_LOCATION=−ln(ln2)`, `_GUMBEL_WIDTH=0.5·ln2·√(2π)`; `developer_exhaustion` D(0) re-anchor via deterministic bisection matching `scipy.brentq` xtol=1e-10, 12-iteration bracket-doubling, behind a `>0` guard).
- `runtime/stages/printing.cpp` + `printing.h` (add the `active=True` branch; default path untouched; pass profile type for `signed_z`).
- `runtime/params.h` / `params.cpp` (add `PrintCurvesMorphParams` into `PrintingParams`, `active=false` no-op).
- `spektra.h` (8 morph fields near `print_density_curve_gamma:215`), `spektra.cpp` (defaults ~1351; pass into print build ~913-917 / call site ~1121), `spektra_jni.cpp` (read off `PrintRenderingParams` ~391-402).
- `SpektraParams.kt` (`PrintCurvesMorphParams` data class; keep `densityCurveGamma`), `ParamsState.kt` (UI group).
- `tests/test_print_curves_morph_e2e.cpp` (NEW) + `tools/parity/goldens/print_portra_morph/` + `gen_goldens.py` + `ci.yml`.

**New params + defaults.** `density_curves_morph.active=False`; `gamma_factor`, `gamma_factor_fast/slow/red/green/blue = 1.0` (>0); `developer_exhaustion=0.0` (range [0,1]). `SIGMA_FLOOR=0.05`.

**Golden recipe.** Non-identity feature golden from `3bb2c2d` math fed the **bundled `c1d0e44` model values** (NOT the `ad5c8d2`-refit profiles). Recipe: cherry-pick `a7f0f9a`+`8a6d8dd` onto `c1d0e44` (or monkeypatch `develop_print_morph` into the `c1d0e44` env) so `morph_curves` runs on `c1d0e44` profiles; call `apply_print_curves_morph(log_exposure, model, PrintCurvesMorphParams(active=True, gamma_factor=1.1, gamma_factor_red=0.95, gamma_factor_fast=1.05, developer_exhaustion=0.4), profile_type)`, pass the morphed `(N,3)` table through `interpolate_exposure_to_density` at fixed `log_raw`, dump `print_density_cmy` to `goldens/print_portra_morph/*.spkvec`. CI test asserts (A) morph-OFF == committed `c1d0e44` `print_portra` golden, (B) morph-ON == new `print_portra_morph` golden. Pin this golden under a documented **second oracle SHA** in `setup_env.sh`.

### Input gamut compression (filming side): CAT02→CAT16 + xy-clip removal + locus bake (L effort, medium value)

**What it is.** Upstream `_rgb_to_tc_b` flips the chromatic-adaptation transform CAT02→CAT16 (unconditional), drops `xy=np.clip(xy,0,1)`, and `compute_hanatos2025_tc_lut` gains a `gamut_compress` arg that bakes a radial Reinhard-knee compression toward the CIE-1931 spectral locus into the per-film 128×128×3 `tc_lut` at build time (`remap_tc_lut_for_compression`). Upstream ships `InputGamutCompressSpec(active=True, algorithm='xy', knee=(0,1,6))` on the default Hanatos2025 path.

**Why it is safe.** Gate **all three** (CAT16 matrix, xy-clip removal, locus bake) behind a single default-false flag. Default keeps `prophoto_rgb_to_tc_b` CAT02 + `clip01` (`filming.cpp:47-58`) and `build_filming_tc_lut` with no trailing remap → `c1d0e44` byte-identical. Critical enabler: the `ad5c8d2` refit changed only `density_curves*`; `log_sensitivity`/`wavelengths`/window/surface params (the only `build_filming_tc_lut`/`expose` inputs) are byte-identical `c1d0e44`↔`3bb2c2d`, so a **`film_log_raw` tap** isolates the gamut feature cleanly against the bundled `c1d0e44` profile. A `film_density_cmy`/`final_rgb` tap would be contaminated by the refit and must NOT be used.

**Files to change.**
- `runtime/stages/filming.cpp` (gate CAT16 matrix + drop `clip01` in `prophoto_rgb_to_tc_b` L47-58; add `compress_xy_radial` + `reinhard_knee` + `ray_polygon_distance` + scipy `map_coordinates(order=1, mode='nearest')`-equivalent bilinear; baked CIE-1931 380–700@5nm locus polygon as a static const = indices 0-64 of `kCieCmf1931`; append `remap_tc_lut_for_compression` at end of `build_filming_tc_lut` ~L387). `quad2tri`/`tri2quad` already exist (`spectral_upsampling.cpp:81-94`).
- `filming.h`, `kernels/spectral_upsampling.cpp/.h` (CAT16 matrix constant; declare helpers).
- `runtime/params.h` (`FilmingParams.input_gamut_compress {active=false, algorithm, knee[3]={0,1,6}}`).
- `spektra.h` (`input_gamut_compress_active` + `knee[3]` ~L225); `spektra.cpp` (fold into `engine_tc_lut` cache key L328-352 + print-route fnv1a64 digest L415-422; thread into expose sites L733/L863); `spektra_jni.cpp` (~L419-436); `SpektraParams.kt` (`SettingsParams` ~L152).
- `tests/test_gamut_compress_input_e2e.cpp` (NEW, `film_log_raw`-only + active=false==default assert); `tools/parity/goldens/gamut_compress_input/`; `gen_goldens.py`; `ci.yml`.

**New params + defaults.** `input_gamut_compress.active=false` (upstream true); `algorithm='xy'` (port `xy` only — `oklch` needs an OkLab C_max(L,h) table, reject); `knee=(0.0,1.0,6.0)`.

**Golden recipe.** First repo golden pinned to a non-`c1d0e44` oracle (feature absent at `c1d0e44`, first appears at `30a32a8`). Checkout `3bb2c2d`; `source setup_env.sh` (SHA-mismatch warning expected). Add a `gamut_compress_input` case (`kodak_portra_400`, `scan_portra_input_rgb.f64`, spatial+stochastic OFF, `rgb_to_raw_method='hanatos2025'`, `input_gamut_compress=InputGamutCompressSpec(active=True,'xy',(0,1,6))` — CAT16 + clip removal come for free at `3bb2c2d`). Capture **`film_log_raw` ONLY** (before `develop()`). Write `goldens/gamut_compress_input/film_log_raw.spkvec` + `manifest.json` (`oracle_sha=3bb2c2d`, tap, knee, tol). Test: (a) active=true == golden, (b) active=false == existing `c1d0e44` `film_log_raw.spkvec`. **`compress_xy_radial`:** `delta=xy−white_xy; dist=|delta|; dir=delta/max(dist,1e-12); boundary=ray_polygon_distance(white_xy,dir,locus); d_norm=dist/max(boundary,1e-12); d'=knee(d_norm); new_xy=white_xy+dir*(d'*boundary)`; at-white (`dist<1e-9`) passes through. `white_xy` = D55 (derivable from `kD55Illuminant`×CMF as `filming.cpp:368-377` already does).

### Output gamut compression — perceptual algos (`cam16ucs` default / `oklch` / `oklrab` / `jzazbz`) (XL effort, medium value)

**What it is.** The four perceptual chroma-reduction algos: RGB→XYZ→perceptual polar (L,C,h), look up a bisected per-output-space `C_max(L,h)` table, Reinhard-knee `C/C_max`, plus a one-sided lightness roll-off (`_compress_lightness`, `(0.7,1.0,2.2)`, black anchored at 0). `cam16ucs` is upstream's default and needs full CIECAM16 fwd+inv + a runtime-built `C_max(Jp,hp)` bisection table (L_A=64, Y_b=20, Average surround).

**Why it is XL / why deferred.** Zero color-appearance infra exists in the C++ engine (grep for `cam16|oklab|oklch|jzazbz` over `cpp/` = zero hits; `color_output.h` only has baked XYZ→RGB matrices + per-space CCTF). Each perceptual algo needs a 64×720 `C_max(L,h)` table (18 bisection iters × `XYZ_to_RGB` per cell, per output space) matching the `colour` library to 1e-4 — hundreds of lines. Same gating shape as `aces_rgc` (default `LEGACY_CLIP`, OFF path keeps clip), same golden recipe but each algo gets its own `3bb2c2d` golden. **Ship `aces_rgc` first; only add `cam16ucs` if a colorist explicitly wants the smooth roll-off.** Files: a new `model/gamut_compression.{h,cpp}` (CAM16 fwd/inv, OkLab/Oklch with the `Lr` remap, optional JzAzBz, C_max bisection + bilinear lookup, `_compress_lightness`) on top of the `aces_rgc` scanning hook.

---

## 5. Needs-rebaseline items (Strategy B only)

These cannot land parity-safely against the `c1d0e44` baseline; they require the full rebaseline.

### Profile JSON refit (28/29 stocks) + `neutral_print_filters.json` (commit `ad5c8d2`)

Re-fit `density_curves` (max_abs 0.43), `density_curves_layers` (0.28), `density_curves_model` (0.55); `neutral_print_filters.json` shifts up to ~82.98 — all dwarfing the parity tolerance. **No new JSON keys, no parser change** (confirmed `nk-ok==[]`; the parser already reads every present field and ignores the unread parametric `density_curves_model`). The Android assets are currently byte-identical to `c1d0e44` (max_abs 0 vs `c1d0e44`, large deltas vs `3bb2c2d`). **Blast radius:** profiles load unconditionally with no per-profile "old vs new curve" flag (gating would mean duplicating all 28 assets), and `neutral_print_filters.json` is on the default print path (`spektra.cpp:1372` defaults `neutral_print_filters_from_database=1`; `resolve_neutral_cc` reads it for every print render). Copying it moves the default output of every film and print render. Must be done only as part of (B), with one coordinated regeneration of every density/printing golden (`simulate_e2e`, `filming`, `provia_couplers_e2e`, `print_evcomp_e2e`, `scanner_bwcorr_e2e`, `highlight_boost_e2e`, `hanatos_surface_e2e`, plus the `.spkvec` goldens) and a re-pin of `setup_env.sh` off `c1d0e44`.

### Default-changing engine math bundled in the same range

Adopting upstream defaults rather than gating them is a (B) decision: input gamut compress `active=True` + CAT02→CAT16 + `xy`/`lrgb` clip removal on the filming path; output `cam16ucs` replacing the final clip in scanning; `develop_print_morph`'s analytic NormCDF eval replacing the tabulated print interp (33× tol even morph-OFF) with the `density_curve_gamma` knob deleted; grain default refit. The `color_reference` + `spectral_upsampling`/`calibration_targets` audit areas are **blocked** as standalone opt-ins precisely because their `3bb2c2d` golden cannot be isolated from the `ad5c8d2` profile refit and the documented `SPEKTRAFILM_ORACLE_DRIFT=a9bccd6` raw-scaling change without new profile-injection tooling in `gen_goldens.py` — i.e. they only become tractable under (B), or require building that isolation tooling first.

### Minor parity-safe fix worth carrying

`color_reference` changed `gamma_factor=density_curve_gamma → 1.0` in the negative-print B/W-reference measurement. Android equivalent: `spektra.cpp:1014` pass `1.0f` instead of `pg` to `measure_print_references`. It is gated by `bw_on && prnt.is_negative()` (default-off scanner B/W correction) and `print_density_curve_gamma` defaults to 1.0, so it is a no-op against current goldens — safe to apply as a fix-port, ideally validated against a dedicated B/W-correction golden (generable post-drift).

---

## 6. Skipped (desktop-only / non-portable)

- **`spektrafilm_lut_creator` (~15k LOC, builders cube/3dl/HaldCLUT/lumix, `ocio_emit`, QA suite):** LUT/OCIO authoring tooling, explicitly out of scope; zero engine math.
- **GUI refactor `param_manifest.py` (commit `6deeec3`):** path-keyed napari presentation metadata; imports desktop-only modules (`spektrafilm_gui.options`, Pillow `ImageCms`) and napari-only fields. Not machine-loadable on Android; keep only as a values reference for slider ranges/tiers if the Compose UI ever goes data-driven.
- **OCIO configs:** desktop color-management emit; no Android consumer.
- **`topology.py` tap inject/collect + `lut_mode` (commits `a9bccd6`/`6deeec3`):** Python orchestration refactor; the default end-to-end topology fires the identical stage chain in identical order (no math change). The port already has equivalents — `spk_simulate_tap` (`spektra.cpp:1522`) for named taps and `spk_bake_cube_lut` (`spektra.cpp:1578`) for the deterministic LUT-bake regime that mirrors `lut_mode`.
- **DIR couplers None-guard:** pure Python crash-guard for `pixel_size_um is None` on the LUT-bake path; impossible in C++ (`double pixel_size_um`), and `couplers.cpp:211` already short-circuits — skip.
- **Diffusion `sigma_um<=0` guard:** same Python None-division guard; `filming.cpp:449-456` already gates on `lens_blur_um>0 && pixel_size_um>0` — skip.
- **`params_schema`/`params_builder`/`pipeline`/`profiles/io` (taps, `lut_mode`, license-string relicense GPLv3→CC BY-SA on profile metadata, `list_profiles()`, `clone_runtime_params`):** Python-internal restructuring; no engine relevance (note the metadata relicense for the attribution/AUDIT docs).
- **Targeted fixes (vlog midgray `3bb2c2d`, GC black-level `6ec371b`, preview-max removal `83278a5`):** the vlog runtime hunk lives only inside the `lut_mode`/`deactivate_spatial_effects` branches (absent in the port; defaults already pinned to those values); the GC black-level fix targets `gamut_compression.py` which doesn't exist at `c1d0e44` (apply the FIXED one-sided `_compress_lightness` if/when output GC is ported, never the two-sided version); preview-max is a desktop GUI slider clamp (Android has its own independent `previewMaxSize=640`).

---

## 7. Recommended next step

**Port output gamut compression `aces_rgc` as a default-OFF opt-in** — it is the smallest fully self-contained item with clear, visible user value (no hard-clipped highlights/chroma), needs no color-appearance-model infrastructure, is trivially thread-invariant, and proves out the new "newer-oracle golden + feature-OFF==`c1d0e44`" gating pattern for everything that follows.

Checklist:

1. Add `enum class OutputGamutCompress { LEGACY_CLIP, OFF, ACES_RGC }` to `SpektraParams.kt` (near `ColorSpace:13`) and `val outputGamutCompress = LEGACY_CLIP` on `IoParams`.
2. Mirror the enum + knee `(0.0,1.0,6.0)` into `spk_params` (`spektra.h` ~217-222) and marshal it in `spektra_jni.cpp` (`---io---` block, `enum_ordinal_*` helper).
3. Add the field to `ScanningParams` (`scanning.h`); set it in both `ScanningParams` construction sites in `spektra.cpp` and default `LEGACY_CLIP` in `spk_default_params`.
4. In `scanning.cpp`: add a `reinhard_knee` helper; insert a per-pixel `aces_rgc` pass on `lin_rgb` between the XYZ→RGB `parallel_for` (~line 311) and `lens_blur` (line 319); gate the `[0,1]` clip (lines 362-363) so the active path skips it and `LEGACY_CLIP` keeps it verbatim.
5. Generate `tests/scan_portra_ref_aces_rgc.spkvec` from the committed `c1d0e44` `film_density_cmy` tap with `compress_rgb_aces_rgc(rgb,0.0,1.0,6.0)` (SHA-independent reimplementation of the ~10-line knee — avoids any oracle pin).
6. Add `tests/test_gamut_compress_e2e.cpp` (modeled on `test_output_spaces.cpp`) asserting both (ON==new golden, OFF==existing `c1d0e44` scan_portra golden).
7. Wire it into `ci.yml` `engine-parity` with a `SPK_NUM_THREADS` 1-vs-8 invariance check; confirm the full engine-parity gate (38 tests as of 2026-08-27) stays green.
8. Run the full host parity suite; add a Compose UI control; smoke-test a release (R8-minified) build on device before shipping.

---

## Part B — Experimental upstream branches (track / future)

### 1. Reality check

The `origin/reflectance-upsampling-methods` branch (the superset of `dev` + `non-linear-couplers`, ~28K lines diffed from merge-base `83278a5`) is the upstream author's next-generation R&D, and it is **unmerged and still actively iterating** — the named studies (`a22`/`b30`/`b80`/`a90`, arctic2026 `alpha`→`beta04`) churned across a roughly two-week June-2026 window and several are shipped as opaque baked `.npy` LUTs with no generator code in the repo. Four large reworks land together and are mutually entangled: the **3-channel→N-channel/B&W generalization**, **new RGB→spectra upsampling methods**, **non-linear (Langmuir) DIR couplers**, and a near-total **grain rewrite** — plus a film-convert prototype and a parametric density-curves model. The Android engine is deliberately **byte-pinned to oracle `c1d0e44`**, which predates all of this; worse, the branch moved its *own* default-path baselines as recently as `526e200` (the new white-balance normalization shifts every neutral by ~2-3%), so the upstream default is itself in flux. Bulk-porting now would chase a moving target across the engine's hardest boundaries (the `w*h*3` JNI buffer contract, the 3-hardcoded density/grain arrays in ~26 source files). The responsible posture is to **TRACK**, keep the `c1d0e44` pin, and revisit individual pieces only once upstream merges to `main`, stabilizes a single set of defaults, and the team deliberately decides to move the baseline.

### 2. Feature inventory

| Area | What it is | Maturity | Needs N-ch rework | Parity impact | Portability | Effort | Value |
|---|---|---|---|---|---|---|---|
| New spectral/reflectance upsampling methods (Otsu2018, Jakob2019, Mallett2019, arctic2026 α→β04, gauss-lasers) | Selectable RGB→spectra reconstruction alongside Hanatos2025; reflectance-kind methods relight under scene illuminant then green-anchored midgray-normalize, flowing through one `(192,192,81)` float16 LUT + `.toml` registry | beta-iterating | No | none — could be opt-in | needs-upstream-stabilize-first | L | medium |
| B&W / arbitrary-N-channel pipeline (`n_ch` rework) + `kodak_trix` reversal B&W | Replaces hardcoded 3-channel CMY with profile-driven `ProfileInfo.n_channels`; `match_channels`, `n_ch`-parameterized couplers/grain, parametric `density_curves_model` as source-of-truth | beta-iterating | **Yes (is the rework)** | requires-baseline-move | huge-architectural | XXL | medium |
| Non-linear DIR couplers (Langmuir donor + receiver, a22 gamma re-opt, cine presets) | Amplitude-matched Langmuir saturation replacing linear inhibitor; donor for negatives, receiver for positives; deletes `high_exposure_couplers_shift`; per-stock gammas to `couplers.toml` | beta-iterating | No | requires-baseline-move | needs-baseline-move | L | medium |
| Grain overhaul (RMS-granularity params, mass-conserving multiplicative USM, datasheet presets) | RMS-6328 catalogue numbers replace particle geometry; positive-by-construction multiplicative unsharp; rewritten N-channel streaming; single-layer path deleted | beta-iterating | **Yes** | requires-baseline-move | needs-baseline-move | XL | **high** |
| Convert stage + density-curves model + develop/base-tuning + chemistry gamma | Film-scan invert workflow (Gauss-Newton/LM solver), parametric `DensityCurvesModel`, print/film base tuning, GUI gamma-range widen | prototype-alpha | Yes (partly) | none — could be opt-in | needs-upstream-stabilize-first | XL | medium |
| Camera color (taking) filters + `color_filters.py`/`illuminants.py` | 5 measured Hoya glass filters folded into film sensitivity with no renorm; **replaces** the erf UV/IR band-pass | beta-iterating | No | requires-baseline-move | needs-upstream-stabilize-first | L | low |
| Params/pipeline wiring + gamut-compress default + DWG/VLog colorspaces + WB normalization | WB-norm flip (midgray→exactly (1,1,1)), `WorkflowParams.route` 6-value string, `aces_rgc` vs `oklch` default, autoexposure refactor + new raw-metering, new param blocks | beta-iterating | Yes (partly) | requires-baseline-move | needs-upstream-stabilize-first | L | medium |

### 3. Dependency graph

**Gated behind the big 3→N-channel / B&W rework** (cannot be taken without moving the whole engine off the 3-hardcoded `(n,3)`/`(n,3,3)` arrays and the `w*h*3` JNI contract at `spektra_jni.cpp:537-544`):

- **`kodak_trix` / `kodak_doublex` / `kodak_2302`** — `channel_model:'bw'`, `n_ch==1`; require the parametric `density_curves_model` + `select_development_time` machinery that is **entirely absent** from the C++ engine (grep for `centers`/`sigmas`/`septic`/`norm_cdfs` is empty).
- **Grain overhaul** — shares `match_channels`, the per-channel streaming loop, and the new `interp_density_cmy_layers_channel` with the B&W commits, and **deletes** the single-layer path the C++ currently wires at `filming.cpp:518` (`grain.sublayers_active` branch). You cannot take the grain rewrite without the channel generalization.
- **Convert stage / density-curves model** — the `model_type` → `norm_cdfs`/`sept_norm_cdfs` parametric source-of-truth flip and `n_ch`/`n_layers` generalization ride the same rework. (The convert *workflow* itself is route-gated and opt-in, but its CMY output is 1-ch for B&W / 3-ch for colour.)
- **`WorkflowParams.route` / convert-film / `FilmBase`/`PrintBase`/`Convert` param blocks** — depend on new alpha stages (`ConvertingStage`, `model.convert`) the C++ engine has no equivalent for (no route/workflow concept, no `scipy.optimize.least_squares`).

**Independent of the N-channel rework** (3-channel-only port is mathematically clean — but several still force a baseline move for other reasons):

- **Spectral/reflectance upsampling methods** — fully self-contained for a 3-channel port; the runtime hot path (`kernels/spectral_upsampling.{h,cpp}` `rgb_to_tc_b` + `cubic_interp_lut_at_2d`) and the `(192,192,81)` float16 LUT format already exist. The green-`[1]`-anchor is a 3-channel assumption that *separates* it from the N-channel work. **Genuinely opt-in** (see §4).
- **Langmuir DIR couplers** — the saturation math is per-channel / N-channel-agnostic; only the surrounding a22 diff threads `n_ch`. **But** the K defaults are finite `(1.0,1.0,1.0)` so Langmuir is **ON by default**, a22 retuned every gamma matrix and deleted `high_exposure_couplers_shift` → **default-path change, baseline move required**, not opt-in.
- **Camera taking-filters** — the 81-band-transmittance-into-sensitivity fold is simple and N-channel-independent, **but** in this branch it *deletes* `camera.filter_uv/filter_ir` (which the Android baseline owns and pins via `camera_uvir_e2e`), so adopting `filming.py` wholesale breaks that gate. Portable only as an **additive** opt-in (see §4).
- **WB normalization (`e301791`)** — maps cleanly onto existing `filming.cpp:316-326` `norm[c]`, but lands on the default render path (~2-3% shift on every neutral) → **baseline move**, not opt-in.

### 4. The few self-contained opt-in candidates

Be conservative: most of Part B is **not** in this bucket. Only the following can be ported as default-OFF additions that keep the `c1d0e44` default byte-identical (no baseline move, no new goldens for the default path):

**(A) Reflectance upsampling methods — the cleanest candidate.** Why it's opt-in: the Hanatos2025 *irradiance* path is unchanged (no relight, no midgray-norm), and the reflectance methods are reached only via a method enum; defaulting it to `hanatos2025` leaves the existing path bit-identical. The runtime sampler already exists. Rough recipe:
- Add a sibling C++ builder next to `build_filming_tc_lut()` (`runtime/stages/filming.cpp:247`, which is hard-wired to irradiance — `tc_lut = spectra_lut · sensitivity` with no relight/no self-norm): `relit = spectra_lut * E_ref[None,None,:]; raw_lut = relit · sensitivity;` then per-channel **green-anchored midgray normalization** (`raw_midgray_green = sum(midgray*E_ref*sens)[1]`; `chroma = neutral_raw/neutral_raw[1]` sampled at the scene-illuminant tc cell; `raw_lut /= chroma*raw_midgray_green`).
- Add a **D65 scene-illuminant** variant of the projection matrix (the current `rgb_to_tc_b` bakes a fixed BT.2020→XYZ D55 matrix `kRgbToXyzBt2020D55`; reflectance passes `reference_illuminant=scene_illuminant=D65`).
- Thread a `rgb_to_raw_method` enum/param JNI→Kotlin→C++ defaulting to `hanatos2025`.
- Ship the chosen `<id>_reflectance_xy_tc.npy` as a new asset (~5.7 MB each); `parse_npy` and the float16 footprint are already compatible — **no loader change**.
- New parity goldens needed **only for the opt-in mode**, captured once the oracle SHA settles; the default needs none. Caveat: the most interesting method (arctic2026) ships with no generator code and churned α→β04 — port a *published, stable* method first (mallett2019 is exact in-gamut and scale-separable) and defer arctic2026 until the author settles on one.

**(B) Camera taking-filter — additive only, NOT by adopting upstream's `filter_uv` removal.** Why it could be opt-in: folding an 81-band transmittance into `sens` with no renorm is trivial and N-channel-independent. Recipe: add a new `camera_color_filter` string param defaulting to `"none"`; when set, multiply a **baked** 81-band transmittance table (mirror the existing `kCustomDichroicFilters[81][3]` pattern in `model/color_filters.cpp`) into `sens` beside the existing `eval_camera_band_pass` block in `build_filming_tc_lut()` — **keep** `filter_uv`/`filter_ir` and the `camera_uvir_e2e` gate. Low value for a phone app (enthusiast glass-filter affordance); defer unless a B&W contrast-filter (Y2/red) use case becomes a headline.

**(C) Multiplicative USM kernel — technically self-contained, but buys nothing yet.** `diffusion.apply_multiplicative_unsharp_mask` is pure numerics (Gaussian blur already exists in `kernels/gaussian`), but it needs a *new* deterministic per-channel parallel-reduction primitive (`kernels/parallel` is per-pixel fork-join today), and at the default `mult_usm_amount=0` it is a no-op. **Defer** — it delivers value only when the grain baseline move lands.

Everything else — Langmuir couplers, the grain overhaul, the N-channel/B&W rework, convert/density-model, WB-norm — is **default-changing and therefore requires a coordinated baseline move**, not an opt-in.

### 5. Watch list / recommendation

**Monitor upstream for:**
- **Merge of `reflectance-upsampling-methods` (or its descendants) into `dev`/`main`** and the blessing of a new oracle SHA — the precondition for any baseline move.
- **WB-norm stabilization (`e301791` / `526e200`).** This is the trigger event: it changes every neutral by ~2-3% on the default path (`filming.cpp:316-326` `norm[c]` would swap the bare reference illuminant for the reconstructed `neutral_sd` sampled at `_tri2quad(_illuminant_to_xy(reference_illuminant))`). When upstream settles its own regression baselines around this, it is the natural moment to re-pin the full golden set (36 gates as of 2026-08-27) to the new SHA in one bump.
- **arctic2026 settling on a single method** with generator code (currently only opaque `.npy`; the β04 `.toml` had its `[meta]` block stripped — still moving).
- **Langmuir K defaults + the `couplers.toml` gamma tables stabilizing**, and the deletion of `high_exposure_couplers_shift` becoming final.
- **A real B&W end-to-end oracle vector.** Upstream's own `tests/test_channel_generic.py` docstring says "No BW profile exists yet" and validates only synthetic single-channel arrays — until a B&W e2e golden exists, the Android parity gate **cannot even be defined**.

**Trigger conditions to begin porting:** (1) upstream merges to `main` and stabilizes defaults; (2) a new oracle SHA is blessed; AND (3) the team makes a deliberate product decision to move the baseline (likely a `versionName` bump). Independent prerequisite, do regardless: fix `np_interp` in `couplers.cpp:27` (switch from binary-search to a numpy-matching left-to-right linear scan) — the existing `log_exposure_0 = le - donor@M` non-monotonicity divergence flagged in `docs/CODE_REVIEW_2026-06-24.md` is a latent parity break that the saturating Langmuir donor would make worse, and the fix is a standalone win.

**Single most valuable thing to consider first:** when (and only when) the branch stabilizes and a baseline move is on the table, the **grain overhaul** is the highest-value piece (value: high — RMS-granularity datasheet sizing + the ringing-fix multiplicative USM are the visible product win) — but it is XL and entangled with the N-channel rework, so it can only land *with* that move. The lowest-risk *first concrete port* is the **reflectance upsampling opt-in (A)**: it is the one item that delivers new user-facing capability **without** moving the default baseline or touching the N-channel architecture. Sequence the eventual baseline move as: fix `np_interp` (independent) → WB-norm + Langmuir + a22 gamma re-pin together as one golden regeneration → grain + N-channel as the major refactor.
