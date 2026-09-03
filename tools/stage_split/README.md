# stage_split — the per-stage split, including the effects nobody has measured

`spk_stage_timings` is the engine's per-stage timer. Two properties of its output have
each already misled a reading (see `runtime/stage_timer.h`):

1. **Zero slots are skipped.** A gated-off filter does not print as `0.0` — it does not
   print at all. Every export profile this project has taken was at default settings, and
   `camera_diffusion` / `lens_blur` / `glare` all default off, so all three were invisible.
2. **`scan_spatial` and `glare_field` are SUB-MEASURES nested inside `scan`.** Do not add
   the printed slots up.

This tool renders a synthetic scene at full resolution through a matrix of configurations
and prints the timer line for each, so the optional effects are actually visible.

## Automation contract

The human-readable `spk_stage_timings` line remains available for this tool, but automated
benchmarks should consume `spk_stage_timings_json` (`spk.stage_timings.v1`). Read it on the
same caller thread immediately after a render entry point returns. Each completed snapshot
contains:

- a process-local `render_id`; `trace_id` is the same value;
- `kind` (`exact_render`, `preview`, `export`, `magnifier`, `roi`, `tap`, or
  `lut_bake`), `native_outcome`, and native status code;
- `wall_ms` and `top_level_ms`, where the latter excludes nested `scan_spatial` and
  `glare_field` to prevent double-counting;
- every stage key, including zero-valued gated-off effects; and
- the diffusion FFT fallback count captured with that render.

On Android, Perfetto records the matching top-level ATrace section as
`spk.render.<kind>#<trace_id>` and stable stage-name child sections. Overlapping callers get
different IDs and immutable thread-local snapshots; an in-flight render never publishes a
partial set. App disposition is a separate keyed `spk.render_outcome.v1` event (`consumed`,
`cancelled`, `superseded`, or `failed`) because Compose may discard successful native work on
another thread. Fold the latest outcome per `render_id`, and exclude cancelled/superseded work
from release baselines. Buffer cleanup is not an outcome: Android emits `consumed` only when a
frame is published or an export/LUT is successfully encoded, and emits the discard/failure event
at that decision point. A Kotlin LUT request wraps its sizing and fill passes in one `lut_bake`
context and returns the same id to the caller.

Machine-readable logging is opt-in so formatting/logcat cannot perturb normal preview latency.
Enable it with `adb shell setprop debug.spektra.timingjson 1` (or while ATrace is active); the
logcat prefixes are `stage timings json:` and `render outcome json:`. Clear the property after
measurement with `adb shell setprop debug.spektra.timingjson ''`.

## Running it

```bash
cd engine/spektra-core/src/main/cpp
g++ -std=c++17 -O2 -pthread -I. ../../../../../tools/stage_split/stage_split.cpp \
  spektra.cpp gpu/*.cpp kernels/*.cpp io/*.cpp model/*.cpp profiles/*.cpp \
  runtime/*.cpp runtime/stages/*.cpp -o /tmp/stage_split
SPK_NUM_THREADS=4 /tmp/stage_split ../assets/spektra 768 2
```

`<asset_dir> [side_px] [reps]`. **Start small** — 768 with Black Pro-Mist on takes over a
minute, and the cost is quadratic in side length (that is the finding, see below).

`SPK_SCENE=tame` renders ~2 stops around midgray with no speculars; the default `wide`
spans ~8 stops with `lum=8.0` specular squares.

## Read this before trusting a number it prints

**Scene content changes stage costs by orders of magnitude.** Grain is ~100x more
expensive on the wide scene than the tame one at the same resolution, because
`fast_binomial_one` degenerates to an O(n) CDF walk near maximum density (`grain.cpp:82`).
So a stage SHARE from this tool describes one synthetic image, not the engine.

The check that catches this, and it takes two minutes: **run two sizes and confirm the
stage scales with pixel count.** The first run of this tool reported "grain is 91% of the
engine" — 4x the pixels gave 1.15x the time, which no per-pixel stage can do. That is how
the wide/tame split came to exist.

Host milliseconds do not transfer to the phone. The shapes and the scaling laws do.

## What it found

`docs/research/perf-lab.md` §20. Briefly: three of the four never-measured effects are
trivial (glare 30 ms, lens blur 15 ms, highlight boost 3 ms at 768px), and the fourth,
**Black Pro-Mist, is 98.2% of the render and quadratic in pixel count** — 30.7 s for one
640px preview at the app's own default settings.
