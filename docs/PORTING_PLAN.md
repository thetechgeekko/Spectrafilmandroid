# Porting Plan — spektrafilm (Python) → spektra-core (C++/Kotlin)

> **STATUS 2026-07-02: DONE FOR THE ENUMERATED PINNED BASELINE** — that reviewed engine surface was
> ported and parity-gated (38 host gates, see
> CLAUDE.md / ci.yml at that snapshot). Kept as the historical porting map; for current state see
> [EXECUTION_INDEX.md](EXECUTION_INDEX.md). The current CI matrix has 39 cases.

This maps every spektrafilm module to its Android target, with effort and strategy. LOC are
approximate (from the source map). "→ C++" means it becomes part of `libspektra.so`; "→ Kotlin"
means it lives in the JVM facade; "→ asset" means data shipped in `assets/spektra/`.

## Public API parity

spektrafilm's surface (`spektrafilm/__init__.py`):

```python
init_params / digest_params         # build + resolve a RuntimePhotoParams
simulate(image, params)             # full pipeline
simulate_preview(image, params)     # downscaled fast path
load_profile / save_profile         # profile JSON I/O
RuntimePhotoParams                  # dataclass tree
```

Android facade (`SpektraEngine` / `SpektraParams`):

```kotlin
SpektraEngine.listProfiles(): ProfileCatalog
SpektraEngine.simulate(linearRgb, params): Bitmap
SpektraEngine.simulatePreview(linearRgb, params, maxSize=640): Bitmap
SpektraParams(...)  // 1:1 with RuntimePhotoParams (camera/enlarger/scanner/film_render/...)
```

## Stage 1 — Pure math (port first, easy, ~1,000 LOC)

| spektrafilm module | LOC | Target | Notes |
|--------------------|-----|--------|-------|
| `model/density_curves.py` | 101 | → C++ | 1D interpolation of log-exposure→density; multi-layer split. |
| `model/emulsion.py` | 87 | → C++ | tensor contraction `density_cmy ⊗ channel_density` (einsum → gemm/loops). |
| `model/couplers.py` | 161 | → C++ | DIR coupler matrix + spatial diffuse (Gaussian + exp tail). Constants in `params_schema`. |
| `model/glare.py` | 24 | → C++ | lognormal scatter + blur. |
| `model/color_filters.py` | 272 | → C++ | UV/IR bandpass, KG3 heat filter, dichroic Y/M/C shifts — all transmittance LUTs. |
| `model/illuminants.py` | 56 | → C++ + asset | black-body formula trivial; CIE D-series + custom (TH-KG3) tabulated as assets. |
| `utils/autoexposure.py` | 111 | → C++ | center-weighted metering to 18% gray. |
| `utils/conversions.py` | 76 | → C++ | density↔light, embedded ACES/RGB matrices. |
| `config.py` (SPECTRAL_SHAPE, CMFs) | — | → C++ const + asset | 380–780 nm @5 nm (81 samples), CIE 1931 2° CMFs `(81,3)`. |

## Stage 2 — Hot kernels (mechanical from Numba, ~1,500 LOC)

| spektrafilm module | LOC | Target | Notes |
|--------------------|-----|--------|-------|
| `utils/fast_gaussian_filter.py` | 413 | → C++ | separable Gaussian; the workhorse for grain/halation/blur. |
| `utils/fast_interp_lut.py` | 827 | → C++ | 1D/2D/3D cubic LUT interpolation (enlarger/scanner/Hanatos LUTs). |
| `utils/fast_interp.py` | 181 | → C++ | 1D linear interpolation. |
| `utils/fast_stats.py` | 353 | → C++ | Poisson + binomial sampling for grain (deterministic seed for reproducibility). |
| `utils/numba_boost_hightlights.py` | 164 | → C++ | pre-clip highlight reconstruction for halation. |
| `utils/fft_gaussian_filter.py` | 171 | → C++ (optional) | FFT Gaussian; can defer — separable Gaussian covers v1. |

## Stage 3 — Pipeline orchestration (~1,800 LOC)

| spektrafilm module | LOC | Target | Notes |
|--------------------|-----|--------|-------|
| `runtime/pipeline.py` | 220 | → C++ | stage composition + the two pipelines (`scan_film`, full print). |
| `runtime/stages/filming.py` | ~250 | → C++ | RGB→raw (Hanatos2025/Mallett2019), expose+develop negative. |
| `runtime/stages/printing.py` | ~200 | → C++ | enlarger spectral calc, dichroic filters, print expose/develop. |
| `runtime/stages/scanning.py` | 180 | → C++ | density→XYZ via CMFs, chromatic adapt, output color space. |
| `runtime/services/spectral_lut_compute.py` | 163 | → C++ | enlarger/scanner LUT caching; Hanatos TC LUT. |
| `runtime/services/color_reference.py` | ~120 | → C++ | white/black point + midgray normalization. |
| `runtime/services/filter_enlarger_source.py` | ~100 | → C++ | dichroic filter spectra from CC values. |
| `runtime/services/resize.py` | ~80 | → C++/Kotlin | preview downscale + pixel-size tracking (µm/px drives all spatial effects). |
| `utils/spectral_upsampling.py` | 394 | → C++ + asset | Hanatos2025: parse `.lut` binary, deg-4 2D polynomial, cubic interp. |
| `model/grain.py` | 239 | → C++ | Poisson-binomial particle model, sublayers, micro-structure. |
| `model/diffusion.py` | 639 | → C++ | halation (multi-bounce √k Gaussians) + in-emulsion scatter (core+exp tail) + diffusion filters. |

## Stage 4 — Params & profiles (~800 LOC)

| spektrafilm module | LOC | Target | Notes |
|--------------------|-----|--------|-------|
| `runtime/params_schema.py` | 218 | → Kotlin `SpektraParams` + C++ struct | 1:1 dataclass mirror (see `engine/spektra-core`). |
| `runtime/params_builder.py` | 211 | → C++/Kotlin | `init_params`/`digest_params`: resolve profiles + neutral filters. |
| `profiles/io.py` | 368 | → C++/Kotlin | JSON profile parse → `Profile{metadata,info,data}`. |
| `data/profiles/*.json` (28) | — | → asset | film + paper stocks. |
| `data/luts/*`, `data/filters/*`, `data/icc/*` | ~17 MB | → asset | see `ASSETS.md`. |

## Stage 5 — RAW & I/O (Android-native, not a C++ port)

| spektrafilm module | LOC | Android replacement |
|--------------------|-----|---------------------|
| `utils/raw_file_processor.py` (rawpy) | 445 | `lib:libraw` (LibRaw NDK) — same options; see `RAW_DNG.md`. WB via the same temperature/tint math. |
| `utils/io.py` (OpenImageIO, exiv2) | 402 | Host: Coil/`BitmapFactory` decode, `ExifInterface` metadata, ImageToolbox export (16-bit TIFF/PNG; EXR optional). |
| `lensfunpy` (unused upstream) | — | skip. |

## Skipped for v1

- `spektrafilm_gui/` (napari/Qt desktop GUI) — replaced by the standalone `:app` Compose UI (the `feature:film-emulation` module referenced by early plans is not built — see CLAUDE.md).
- `utils/plotting.py`, `model/parametric.py`, `model/stocks.py`, `utils/calibration_targets.py`.
- `lensfunpy` lens correction (unused upstream).

> **Scope note (bit-exact, "everything"):** the full engine is in scope. In particular
> **FFT-based diffusion is NOT skipped** — bit-exact parity on large-radius diffusion/halation
> requires it (CPU FFT, e.g. pocketfft/kissfft). Only niche *file I/O* is deferred: EXR and
> 32-bit-float TIFF (spektrafilm's OpenImageIO path) land behind the same writer interface in a
> later milestone; the internal pipeline is 32-bit float regardless. See `MOBILE_STRATEGY.md`.

## Numerical parity strategy

spektrafilm commits regression snapshots as `.npz` in `tests/baselines/`, checked by
`tests/test_regression_baselines.py`. We build a **golden-vector harness**: run the Python
engine on fixed inputs/params, dump intermediate buffers (film raw, film density, print
density, final RGB) to a portable binary, and assert the C++ port reproduces each stage within
tolerance. Port order = the stage table above; each stage is "done" when its golden vector
matches. The upstream `debug` params (`output_film_log_raw`, `output_film_density_cmy`,
`output_print_density_cmy`, `inject_film_density_cmy`) already expose exactly these
intermediates — we reuse them to generate goldens.

**Parity bar = bit-exact.** Per-stage golden vectors gate to a tight tolerance
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`) against the Python engine; deterministic settings
(auto-exposure off, fixed seeds). The one inherently stochastic stage — grain
(Poisson-binomial) — is seeded for reproducibility and, where exact match is impossible across
RNG implementations, compared on distribution statistics rather than element-wise. A GPU
preview path (future) is held only to a looser *visual* tolerance and never gates parity; export
always uses the CPU engine. See `MOBILE_STRATEGY.md` for why CPU is the parity-bearing engine.

## Effort summary

| Bucket | LOC | Est. |
|--------|-----|------|
| Pure math | ~1,000 | 1–2 wk |
| Hot kernels | ~1,500 | 2–3 wk |
| Pipeline + spectral | ~1,800 | 3–4 wk |
| Params/profiles | ~800 | 1 wk |
| RAW + I/O (Android) | — | 1–2 wk |
| Compose UI feature | — | 2 wk |
| **Total to functional app** | **~6,000 ported** | **~8–12 wk** |
