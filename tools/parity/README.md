# Golden-vector parity harness

This directory contains the **golden-vector parity harness** for Spektrafilm for
Android. Its job is to prove the C++ engine port (`engine/spektra-core`,
`libspektra.so`) matches the Python `spektrafilm` engine **stage by stage**, not
just on the final image.

The strategy is the one in `docs/PORTING_PLAN.md` → "Numerical parity strategy":
run the Python engine on fixed inputs/params, dump each intermediate buffer to a
portable binary (`.spkvec`), and assert the C++ port reproduces each stage within
tolerance. The upstream `DebugParams` taps already expose exactly the
intermediates we want, so we reuse them.

```
Python spektrafilm                         C++ spektra-core
------------------                         ----------------
init_params(film, print)
       │                                   spk_engine_create(...)
       ▼                                          │
simulate(image, params)   ── DebugParams ──►      ▼
  ├─ film_log_raw     ─┐                    spk_simulate_tap(.., "film_log_raw", ..)
  ├─ film_density_cmy ─┤  write .spkvec     spk_simulate_tap(.., "film_density_cmy", ..)
  ├─ print_density_cmy─┤  goldens/<case>/   spk_simulate_tap(.., "print_density_cmy", ..)
  └─ final_rgb        ─┘                    spk_simulate(.., out)
                          │                        │  write .spkvec
                          ▼                        ▼
                  goldens/<case>/*.spkvec    engine output .spkvec
                                   └────────────┬───────────┘
                                                ▼
                                  spkvec_compare golden engine --max-abs T --rms T
                                                ▼
                                        PASS / FAIL (exit 0/1)
```

## Files

| File                 | What it is |
|----------------------|------------|
| `spkvec_format.md`   | Spec for the portable `.spkvec` binary container (magic, version, dtype, shape, raw LE data). |
| `spkvec.py`          | NumPy reader/writer for `.spkvec`. Used by `gen_goldens.py`; CLI `python spkvec.py info FILE`. |
| `spkvec_io.h`        | Header-only C++ reader/writer for `.spkvec`, byte-compatible with `spkvec.py`. |
| `gen_goldens.py`     | Runs `spektrafilm` with each DebugParams tap on a fixed synthetic image; writes `goldens/<case>/<tap>.spkvec` + `manifest.json`. **Requires a spektrafilm env.** |
| `gen_viewing_illuminant_visuals.py` | Re-scans the two committed cinema-print density taps under K75P (plus a deliberately wrong D50 counterfactual), writes six output-space references, two review PNGs, hashes, and numeric deltas. **Requires the pinned oracle + colour 0.4.7.** |
| `raw_wb_cat_research.py` | Dependency-free lock for the RAW-WB CAT02 decision; compares current XYZ scaling, Bradford, no adaptation, cast order, and oracle tolerance skips against pinned float32 bits in `fixtures/raw_wb_cat_vectors.json`. |
| `compare_main.cpp`   | Standalone comparator: reads a golden `.spkvec` and an engine `.spkvec`, reports max-abs / RMS vs tolerance. Seed of the CI parity test. |
| `CMakeLists.txt`     | Host build for `spkvec_compare` (independent of the Android/NDK engine module). |
| `cases.md`           | The parity case matrix (profile pairs, scan_film on/off, taps) gating M3/M4. |
| `goldens/`           | Generated `.spkvec` goldens + per-case `manifest.json`. |

## Part A — generate goldens (Python side)

`gen_goldens.py` builds a small deterministic synthetic test image in-script (a
64×64 RGB ramp with a Macbeth-like patch grid and shadow/highlight tiles — no
external files), builds params via `init_params(film, print)`, and for each tap
runs `simulate` with the matching `DebugParams` flag set:

| Tap                 | How it is captured |
|---------------------|--------------------|
| `film_log_raw`      | `debug.debug_mode="output"`, `debug.output_film_log_raw=True` |
| `film_density_cmy`  | `debug.debug_mode="output"`, `debug.output_film_density_cmy=True` |
| `print_density_cmy` | `debug.debug_mode="output"`, `debug.output_print_density_cmy=True` |
| `final_rgb`         | `debug.debug_mode="off"` — the full pipeline result |

Each captured buffer is written to `goldens/<case>/<tap>.spkvec` and the case
(profiles, toggles, image spec, per-tap stats, tolerances) is recorded in
`goldens/<case>/manifest.json`.

The Kodak 2383/2393 K75P cases additionally run the pinned oracle with
`settings.use_scanner_lut=True` and `lut_resolution=17`, writing
`final_rgb_scanner_lut_17.spkvec`. This gates accelerated mode against the same
upstream mode; it is not a widened comparison with direct spectral integration.

### Requirements (spektrafilm env)

Generating goldens **requires a working `spektrafilm` environment**, exactly as
described in the upstream spektrafilm README: clone the repo and install the
package plus its data assets (profiles/LUTs/filters). Install spektrafilm and run
this script from that environment, e.g.:

```bash
cd /path/to/spektrafilm && pip install -e .
python /path/to/Spectrafilmandroid/tools/parity/gen_goldens.py
```

If `spektrafilm` (or a required asset) is not importable, `gen_goldens.py` exits
with a clear, actionable message — it never half-writes goldens. **Reading and
inspecting existing `.spkvec` goldens, and building/running the C++ comparator, do
NOT require spektrafilm.**

### Oracle provenance — the goldens are pinned to ONE upstream commit

The committed goldens are **reproducible only at one exact upstream commit**.
Upstream `spektrafilm` HEAD has drifted, so generating from HEAD will NOT match.

| | |
|---|---|
| Oracle repo | `https://github.com/andreavolpato/spektrafilm` |
| **Pinned oracle SHA** | **`c1d0e44b962d80a51ea096d33faea346e4f3836c`** — "docs: update high-res banner", 2026-05-23 |
| Drift commit (first BAD) | `a9bccd6bf58764811eeb69883cdc9d86ab64ee18` — "feat: tap inject/collect system for runtime", 2026-05-23 (immediate child of the pin) |

At the pinned SHA, regenerating **every** `gen_goldens.py` case reproduces the
committed `.spkvec` files **bit-exactly** (`max_abs = 0`, `rms = 0` for all taps:
`film_log_raw`, `film_density_cmy`, `print_density_cmy`, `final_rgb`). The drift
commit `a9bccd6` ("tap inject/collect system") changed the filming raw-scaling
semantics — at it and every later commit up to HEAD, `film_log_raw` diverges
(`max_abs ≈ 4.44` vs the committed golden). That is why the oracle SHA must be
pinned: a fresh clone defaults to HEAD and silently produces non-matching goldens.

`tools/parity/setup_env.sh` exports `SPEKTRAFILM_ORACLE_SHA` and gives an early
warning if your `SPEKTRAFILM_SRC` checkout is not on it. Both golden generators
also enforce provenance themselves: they resolve the checkout actually imported,
require the exact SHA and a Git-clean tracked/staged/non-ignored-untracked state
before and after oracle execution, and refuse to write a misleading manifest.
To reproduce the goldens:

```bash
git clone https://github.com/andreavolpato/spektrafilm /tmp/spektrafilm
git -C /tmp/spektrafilm checkout c1d0e44b962d80a51ea096d33faea346e4f3836c
export SPEKTRAFILM_SRC=/tmp/spektrafilm/src
source tools/parity/setup_env.sh
python tools/parity/gen_goldens.py     # regenerates the committed goldens bit-exactly
```

### Commands

```bash
python gen_goldens.py            # generate all cases (cases.md) at 64x64
python gen_goldens.py --list     # list cases (no spektrafilm needed)
python gen_goldens.py --case print_portra   # one case
python gen_goldens.py --size 64  # test-image edge length (px)

# K75P output-space refs + visual evidence (checkout must be pinned and clean).
python gen_viewing_illuminant_visuals.py --spektrafilm-src /tmp/spektrafilm

# Offline provenance-gate tests (clean/wrong-HEAD/tracked-dirty/untracked-dirty).
python -m unittest tools.parity.tests.test_oracle_provenance -v

python spkvec.py info goldens/print_portra/film_density_cmy.spkvec   # inspect a golden

# Reproduce the research-only RAW-WB CAT decision without external packages.
python raw_wb_cat_research.py --check

# Independently execute the exact pinned colour-science environment, if installed.
python raw_wb_cat_research.py --check --verify-colour
```

Determinism: the synthetic image is seeded (`SEED=20260529`); cases disable
auto-exposure and (by default) stochastic + spatial effects, so regenerated
goldens for unchanged inputs are byte-stable and `git diff`-able. Goldens are tiny
(64×64×3 float32 ≈ 48 KiB each) and meant to be committed.

## Part B — run the C++ engine and compare

1. Build the host comparator (independent of the Android engine):

   ```bash
   cmake -S . -B build && cmake --build build
   ./build/spkvec_compare --selftest      # checks spkvec_io.h == spkvec.py byte format
   ```

2. Drive the C++ engine to produce each tap. The engine exposes
   `spk_simulate_tap(engine, in, params, tap_name, out)` with `tap_name` ∈
   `"film_log_raw" | "film_density_cmy" | "print_density_cmy"`, and `spk_simulate`
   for the final RGB. A small host driver (to be added alongside the engine build)
   feeds the **same** synthetic image and params, then writes each `out` buffer to
   a `.spkvec` using `spkvec_io.h` (`spkvec::write(path, shape, data, count)`).

3. Compare each tap against its golden within the case tolerance:

   ```bash
   ./build/spkvec_compare \
       goldens/print_portra/film_density_cmy.spkvec \
       engine_out/print_portra/film_density_cmy.spkvec \
       --max-abs 1e-4 --rms 1e-5
   ```

   Exit code `0` = within tolerance (PASS), `1` = out of tolerance or shape
   mismatch (FAIL), `2` = usage/IO error. The tool prints `max_abs`/`rms`, the
   worst-element index and both values, so a failing stage points straight at the
   pixels that diverged. The default tolerances match those written into each
   `manifest.json`.

This is the seed of the on-host/CI parity gate: CI builds `spkvec_compare`, runs
the engine driver, and runs the comparator over the case matrix in `cases.md`. The
port order follows the stage table in `PORTING_PLAN.md`; each stage is "done" when
its golden vector matches.

## Tolerances

The default deterministic tolerance is `max_abs ≤ 1e-4`, `rms ≤ 1e-5` (well above
float32 epsilon — the goldens themselves are stored as float32). Each
`manifest.json` carries the per-case tolerance the comparator should gate on, so
tolerances can be tuned per case/tap without code changes.

## Grain & stochastic taps

`model/grain.py` (and other stochastic effects) use random sampling. Even with a
fixed seed, the Python and C++ RNG streams differ, so **grain-on cases cannot be
compared element-wise.** Two mitigations:

1. **Default goldens are grain-off / spatial-off** (`grain_active=False`,
   `deactivate_stochastic_effects=True`, `deactivate_spatial_effects=True`), making
   the captured taps fully deterministic and element-comparable. These are what
   gate M3/M4 today.

2. **Grain-on cases** (future, see `cases.md`) are validated by *statistics*, not
   by exact values: compare per-channel mean and variance, and local
   autocorrelation / power-spectrum summaries, within a looser band. The same
   `.spkvec` container holds the buffer; only the comparison metric changes
   (a statistics mode will be added to the comparator when grain lands).

## Format

See `spkvec_format.md`. In one line: magic `SPKVEC`, `uint16` version, `uint8`
dtype (1=float32), `uint8` ndim, `uint32` shape per dim, then raw little-endian
`float32` data in C/row-major order. One tensor per file; semantics live in the
sibling `manifest.json`.

## License

GPLv3, matching spektrafilm and the rest of Spektrafilm for Android. Film modeling
powered by `spektrafilm` (GPLv3) by Andrea Volpato.
