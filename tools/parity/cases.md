# Parity case matrix

The initial set of golden-vector parity cases that gate **M3** (pipeline port) and
**M4** (params/profiles) in `docs/PORTING_PLAN.md`. Each case fixes a film/print
profile pair plus deterministic toggles; for each case we capture a set of *taps*
(intermediate buffers) as `.spkvec` goldens. A stage of the C++ port is "done" when
its tap matches the Python golden within tolerance (see `README.md`).

Goldens are generated from the synthetic 64×64 ramp+Macbeth test image
(`gen_goldens.py make_test_image`, seed `20260529`, ProPhoto-RGB linear input).

## Taps (pipeline order)

| Tap                 | DebugParams field            | C API `tap_name`     | Shape     | Gates stage |
|---------------------|------------------------------|----------------------|-----------|-------------|
| `film_log_raw`      | `output_film_log_raw`        | `film_log_raw`       | (H,W,3)   | filming/expose (RGB→raw, Hanatos/Mallett) |
| `film_density_cmy`  | `output_film_density_cmy`    | `film_density_cmy`   | (H,W,3)   | filming/develop (density curves, couplers) |
| `print_density_cmy` | `output_print_density_cmy`   | `print_density_cmy`  | (H,W,3)   | printing (enlarger expose+develop) |
| `final_rgb`         | — (full pipeline, debug off) | (use `spk_simulate`) | (H,W,3)   | scanning (density→XYZ→output) |

`print_density_cmy` does not exist on the `scan_film` route (no print stage).

## Cases

| case_id        | film              | print               | route       | grain | spatial/stochastic | taps |
|----------------|-------------------|---------------------|-------------|-------|--------------------|------|
| `print_portra` | kodak_portra_400  | kodak_portra_endura | print       | off   | off                | all 4 |
| `print_kodak_2383_k75p` | kodak_portra_400 | kodak_2383 | print | off | off | all 4 |
| `print_kodak_2393_k75p` | kodak_portra_400 | kodak_2393 | print | off | off | all 4 |
| `scan_portra`  | kodak_portra_400  | kodak_portra_endura | scan_film   | off   | off                | film_log_raw, film_density_cmy, final_rgb |
| `print_ektar`  | kodak_ektar_100   | kodak_supra_endura  | print       | off   | off                | all 4 |
| `scan_portra_spatial` | kodak_portra_400 | kodak_portra_endura | scan_film | off | spatial ON         | film_density_cmy, final_rgb (+) |
| `scan_portra_crop`    | kodak_portra_400 | kodak_portra_endura | scan_film | off | off (crop+upscale) | film_density_cmy, final_rgb |
| `scan_portra_lensblur` | kodak_portra_400 | kodak_portra_endura | scan_film | off | spatial ON (camera/scanner lens blur) | film_density_cmy, final_rgb |
| `scan_portra_autoexp` | kodak_portra_400 | kodak_portra_endura | scan_film | off | off (auto-exposure ON) | film_log_raw, film_density_cmy, final_rgb |
| `scan_portra_autoexp_matrix` | kodak_portra_400 | kodak_portra_endura | scan_film | off | off (auto-exposure ON, `matrix` metering) | film_log_raw, film_density_cmy, final_rgb |
| `scan_diffusion` | kodak_portra_400 | kodak_portra_endura | scan_film | off | spatial ON + camera optical diffusion ON (strength 0.8) | film_log_raw, film_density_cmy, final_rgb |

Rationale:
- `print_portra` is the canonical reference pair (matches `init_params` defaults)
  and exercises every stage of the full print pipeline.
- `print_kodak_2383_k75p` and `print_kodak_2393_k75p` independently lock the
  two bundled cinema-print profiles to their declared Kinoton 75P viewing
  illuminant. Their `final_rgb` taps cover the K75P spectral integral and the
  K75P-to-output-white CAT; the upstream oracle generated both fixtures, so a
  silent D50 fallback exceeds the normal parity band.
- `scan_portra` flips `io.scan_film` to gate the negative-scan route, which skips
  the print stage entirely (`PORTING_PLAN.md` Stage 3 `scanning.py`).
- `print_ektar` uses a different film/paper pair to catch regressions that are
  coupled to profile data (density curves, dye spectra) rather than the math.
- `scan_portra_spatial` / `scan_portra_crop` exercise the spatial branch and the
  crop+cubic-upscale geometry stage respectively (still deterministic: grain off).
- `scan_portra_lensblur` exercises the camera/scanner lens-blur spatial effect
  (deterministic Gaussian/exponential filter port, grain off). Tested by
  `tests/test_lensblur.cpp`, which asserts film_density_cmy + final_rgb bit-exact.
- `scan_portra_autoexp` is the NON-default auto-exposure case: it flips
  `camera.auto_exposure = True` (method `center_weighted`) to exercise
  `FilmingStage.auto_exposure` in `pipeline._preprocess` — the image is metered
  (small_preview luminance → `-log2(Y_meter/0.184)` EV) and globally scaled by
  `2**ev` before crop/rescale. Tested by `tests/test_autoexposure.cpp`.

All cases EXCEPT `scan_portra_autoexp` / `scan_portra_autoexp_matrix` run with
`auto_exposure = False`; every case uses `exposure_compensation_ev = 0` and
stochastic effects off so the goldens are bit-stable across hosts. (The
auto-exposure cases are still fully deterministic — the synthetic image is fixed,
so the metered EV is reproducible.)

- `scan_portra_autoexp_matrix` is the NON-`center_weighted` auto-exposure case:
  it flips `camera.auto_exposure = True` with `auto_exposure_method = "matrix"`
  (a 5×5 raised-cosine zone-weighted pattern) to exercise the matrix branch of
  `utils.autoexposure.measure_autoexposure_ev` end-to-end — the existing
  `scan_portra_autoexp` only covers `center_weighted`. Tested by
  `tests/test_autoexposure.cpp` (the same binary now gates BOTH patterns and
  asserts the two EVs differ).
- `scan_diffusion` is the FULL-PIPELINE camera optical-diffusion case: it keeps
  spatial effects on (the oracle's `digest_params` zeroes
  `camera.diffusion_filter.active` under `deactivate_spatial_effects=True`, so the
  diffusion filter lives on the spatial branch) and ADDITIONALLY sets
  `camera.diffusion_filter = {active:True, family:black_pro_mist, strength:0.8}`
  (a NON-default strength). It runs the whole scan_film route
  (expose → develop → scan) so `apply_diffusion_filter_um` is exercised inside
  `FilmingStage.expose`, not just in stage isolation (the `diffusion_bpm` case
  only tests the filter in isolation). Tested by `tests/test_diffusion_e2e.cpp`
  (asserts film_log_raw + film_density_cmy + final_rgb bit-exact and confirms
  turning the filter off changes the output).

## Stage-isolation cases (not run through `gen_goldens.py`)

Some pipeline stages are not DebugParams taps — they run mid-pipeline on the
float64 irradiance. They get a dedicated stage-isolation golden + host test:

- **`diffusion_bpm`** — the optical **diffusion filter** (Black Pro-Mist family),
  `spektrafilm.model.diffusion.apply_diffusion_filter_um`. Generated by
  `gen_diffusion_golden.py` (NOT `gen_goldens.py`): a deterministic float64
  irradiance image (ramp + bright sources, seed `20260529`) is run through the
  oracle filter for the NON-default setting `active=True,
  family=black_pro_mist, strength=0.5, spatial_scale=1.0, halo_warmth=0.0`. The
  golden is `goldens/diffusion_bpm/{input_rgb.f64, diffusion_out.spkvec}`; the
  host test is `tests/test_diffusion.cpp`. The C++ port uses a direct
  double-precision spatial convolution (the oracle's `scipy.signal.fftconvolve`
  is a plain linear convolution; direct == FFT to ~1e-16), so the result is
  bit-exact (max_abs = 0 at float32 store). The diffusion filter is part of the
  spatial-effects branch (the oracle's `digest_params` zeroes
  `camera/enlarger.diffusion_filter.active` when
  `deactivate_spatial_effects=True`), wired in `filming.expose` (camera) and
  `printing.expose` (enlarger).

- **`lut_accel`** — the OPT-IN 3D LUT acceleration for the scanner/enlarger
  density→log_xyz transforms (`settings.use_scanner_lut` / `use_enlarger_lut` /
  `lut_resolution`). The oracle builds a per-channel uniform 3D LUT over
  `[density_min, density_max]` (`utils.lut.compute_with_lut` / `_create_lut_3d`)
  and interpolates it with `utils.fast_interp_lut.apply_lut_3d`'s DEFAULT method
  `pchip` (monotone cubic Hermite with precomputed per-axis slopes + per-cell
  clamping). Generated by `gen_lut_golden.py` (NOT `gen_goldens.py`): a fixed,
  smooth, monotone-per-axis analytic vector function (shaped like a
  density→log-light map, no spectral assets needed) is LUT'd and applied. The
  golden is `goldens/lut_accel/{input_norm.f64, lut.f64, direct.spkvec,
  lut_out.spkvec}`; the host test is `tests/test_lut_accel.cpp`. The native PCHIP
  port (`kernels/lut3d.{h,cpp}`) reproduces the oracle's PCHIP output BIT-EXACT
  at float32 store (interp max_abs = 0), and the native LUT build matches the
  oracle to machine epsilon (~2e-16). LUT-vs-direct error is ~5e-5 (matching the
  oracle's own), which is the documented acceleration tolerance — LUT
  interpolation is intentionally NOT bit-exact vs. the direct path. See
  `kernels/lut3d.h` for the parity contract.

  **WIRING (this pass):** the scanner LUT is now wired into the live pipeline as a
  strictly OPT-IN path. `settings.use_scanner_lut` (default `0`) gates it in BOTH
  scan call sites in `spektra.cpp` (the `scan_film` route in `run_scan` and the
  print-scan route in `run_print`); `scan()` (`runtime/stages/scanning.{h,cpp}`)
  builds a per-channel PCHIP 3D LUT over the density domain
  (`scan_film`: `[-grain.density_min, nanmax(density_curves)]`; print scan:
  `[nanmin, nanmax](density_curves)`) at `settings.lut_resolution` (clamped to
  `[2, 192]`; engine default `17`) and interpolates `density_cmy -> log_xyz`
  instead of the per-pixel spectral integral. When `use_scanner_lut == 0` the LUT
  is NEVER constructed and the output is BYTE-IDENTICAL to the pre-wiring direct
  path (verified by `cmp` of `spk_simulate` final_rgb, HEAD vs wired, for both the
  scan and print routes). The end-to-end LUT-on path is gated by
  `tests/test_scanner_lut_e2e.cpp` (see `scanner_lut_e2e` below). The ENLARGER LUT
  (`use_enlarger_lut`) is left RESERVED/unwired this pass (see note).

- **`scanner_lut_e2e`** — the FULL-PIPELINE gate for the WIRED opt-in scanner LUT
  (`tests/test_scanner_lut_e2e.cpp`, asset-backed). It runs Kodak 2383/K75P,
  Kodak 2393/K75P, and the legacy `scan_portra`/D50 route through
  `spk_simulate`: (A) `use_scanner_lut=0` (default direct path) is asserted
  against each committed `final_rgb` oracle (max_abs <= 1e-4, rms <= 1e-5),
  and (B) `use_scanner_lut=1` at the upstream-default resolution 17 is asserted
  against each case's pinned `final_rgb_scanner_lut_17.spkvec` oracle at the same
  tight tolerance. This is a like-for-like mode gate: upstream's coarse K75P LUT
  itself differs from direct integration by about 3.97e-3 (2383) / 7.20e-3
  (2393), so that cross-mode delta is printed only as a diagnostic, never hidden
  inside a widened parity band. The D50 case retains its resolution-17 and
  resolution-64 direct-convergence checks (5e-5 / 5e-6). Viewing-only memo
  isolation remains covered by `tests/test_lut_cache_e2e.cpp`.

  The complementary route tests are `tests/test_gpu_host.cpp`, which runs both
  K75P profiles first through linear/fused preview and experimental GPU export
  using CPU export as a route-local reference, and
  `tests/test_scanner_bwcorr_e2e.cpp`, which anchors corrections-OFF to the
  committed K75P goldens and proves reference measurement is viewing-sensitive,
  active, finite, and repeat-deterministic when enabled. Exact export keeps the
  approximate scanner LUT disabled; increasing K75P to resolution 80 was rejected
  here because it changes upstream semantics and expands one prepared LUT from
  about 0.64 MiB to 69.4 MiB, far beyond the current 8 MiB cache budget.

## Tolerances

Deterministic cases (all of the above) gate tightly: `max_abs ≤ 1e-4`,
`rms ≤ 1e-5` (well above float32 epsilon; see per-case `manifest.json`).

## CI gating (`engine-parity` job)

All of the cases and stage-isolation tests above are ALREADY gated in CI. The
authoritative list of gated tests and their exact argv is the `engine-parity`
job's `build_run` block in `.github/workflows/ci.yml` — treat that file as the
source of truth and copy argv from there rather than from this doc. As of this
writing it gates: `test_simulate_e2e`, `test_filming`, `test_spatial`,
`test_crop_resize`, `test_autoexposure`, `test_diffusion`, `test_diffusion_e2e`,
`test_lut_accel`, `test_scanner_lut_e2e`, `test_enlarger_lut_e2e`,
`test_output_spaces`, `test_lensblur`, `test_parallel`, `test_tonecurve`, and
`test_half`. They share the same `SRC` full-source set, `DEF`, and `$G` (goldens
root) the other lines use.

`test_autoexposure` already receives `"$G"` as its 4th arg in ci.yml, so the same
binary gates BOTH the `center_weighted` (`scan_portra_autoexp`) and the `matrix`
(`scan_portra_autoexp_matrix`) goldens (it derives the matrix golden dir from
argv[4]=`$G`) — no change needed.

## Future cases (not yet gating)

- **Grain-on** (`grain_active = True`): grain is stochastic. Even with a fixed
  seed the Python and C++ RNGs differ, so these are compared by *statistics*
  (per-channel mean/variance, local autocorrelation) within a looser band, not
  element-wise. Documented in `README.md` → "Grain & stochastic taps". Add once
  `model/grain.py` is ported (Stage 3).
- **Spatial-effects-on** (halation, glare, DIR diffusion): deterministic
  but sensitive to the Gaussian/exponential filter port. Add per-effect as
  `utils/fast_gaussian_filter.py` / `model/diffusion.py` land. (Camera/scanner
  **lens blur** has landed — see `scan_portra_lensblur` above; optical
  **diffusion** is covered by `scan_diffusion` / `diffusion_bpm`.)
- **Positive stock** (e.g. `fujifilm_velvia_100`): exercises the `is_positive`
  coupler-preset branch in `params_builder._apply_film_specifics`.
- **Mallett2019 RGB→raw** (`settings.rgb_to_raw_method = "mallett2019"`): the
  alternate upsampling path (`SPK_RGB2RAW_MALLETT2019`).
