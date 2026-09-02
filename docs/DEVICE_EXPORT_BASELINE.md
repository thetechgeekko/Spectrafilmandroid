# Canonical release export baseline (issue #119)

The current-HEAD release/R8 export baseline on Tier A hardware, measured with the #177
harness under the owner-approved #126 contract. Evidence lives in
[docs/device/ticket119/](device/ticket119/); this file is the reading of it.

**Status:** current. Supersedes every earlier export timing in `docs/AUDIT.md`,
`docs/PERF_ROADMAP.md` and `HANDOFF.md` — including the historical 6.251 s and ~9.8 s
figures, which measured different builds, different parameters and a plugged-in device.

## Identity

| | |
|---|---|
| Source commit | `1c7a63e` (branch `claude/perf-lab`) |
| App | `0.9.0` (versionCode 11), release + R8, **not** debuggable |
| Installed APK | `e86f327fa33eff46cbdfbb5e3afb7805a78918a06f3615234959094236b1f42d` |
| Device | Samsung SM-S948W, Android 16 (API 36), 8 cores, `arm64-v8a`, GLES 3.2 |
| Build fingerprint | `samsung/m3qcsx/m3q:16/BP4A.251205.006/S948WVLS4AZG3_OYV4AZG3:user/release-keys` |
| Source image | `66381a8ed2d1a6ab…`, 4080×3060 (12.5 MP), 37,460,417 bytes |
| Preset | Portra 400 — Wedding Warm (`kodak_portra_400` → `kodak_portra_endura`) |
| Protocol | 5 runs × 12 cell/format pairs = 60 exports, unplugged, `require_thermal_status: 0` |

Every one of the 60 samples recorded `plugged: 0` and `thermal_status: 0`, the thermal
precondition never timed out (total wait 0 ms), battery went 73% → 69% against a 50%
floor, and stage totals reconcile with wall time to a median of 13 ms (max 19 ms).

## Export wall time (p50, warm, ms)

| cell | route | effects | JPEG_Q95 | PNG16 | TIFF16 |
|---|---|---|---:|---:|---:|
| SCAN_CLEAN | scan | — | — | 6044 | **5585** |
| BASE | print | — | 6052 | 7802 | 7108 |
| SCAN_GRAIN | scan | grain | — | 12241 | 9862 |
| PRINT_GRAIN | print | grain | — | 14963 | 11986 |
| HEAVY | print | grain + halation + DIR couplers | 14465 | 18606 | 15732 |

n = 5 per cell/format (4 for BASE/JPEG_Q95, whose first render is the process-cold sample
the protocol discards). **p95 and confidence intervals are not claimable at n = 5** and are
deliberately not quoted here; the SLO proof (#186) carries its own `slo_runs: 11`.

## Where the time goes (p50, ms)

| phase | SCAN_CLEAN | BASE | SCAN_GRAIN | PRINT_GRAIN | HEAVY |
|---|---:|---:|---:|---:|---:|
| decode | 976 | 959 | 979 | 977 | 978 |
| simulate (engine) | 3290 | 5020 | 7652 | 9880 | 13550 |
| grade | 0 | 0 | 0 | 0 | 0 |
| encode (TIFF16) | 1178 | 1154 | 1218 | 1116 | 1189 |

Decode is flat at ~0.98 s. The grade is **zero** on the native writers and 67–70 ms on the
JPEG path (that residue is the float→ARGB bitmap conversion, not the grade itself) — see
[The grade is free at defaults](#the-grade-is-free-at-defaults). Encode depends on the
container, not the pipeline: JPEG 110–162 ms, TIFF16 1116–1189 ms, PNG16 1705 ms clean but
**4316 ms on HEAVY**, because grain noise is close to incompressible.

### Engine stages (native `spk.stage_timings.v1`, p50 ms)

| stage | SCAN_CLEAN | BASE | SCAN_GRAIN | PRINT_GRAIN | HEAVY |
|---|---:|---:|---:|---:|---:|
| preprocess | 328 | 293 | 328 | 327 | 315 |
| filming_expose | 361 | 333 | 365 | 364 | 278 |
| develop | 44 | 40 | 47 | 45 | 44 |
| dir_couplers | — | — | — | — | 1168 |
| **grain** | — | — | **4161** | **4134** | **4081** |
| halation | — | — | — | — | 2169 |
| print_expose | — | 1606 | — | 1710 | 1695 |
| scan | 2133 | 2071 | 2331 | 2656 | 2576 |
| scan_spatial | 511 | 506 | 541 | 512 | 504 |
| glare_field | — | — | — | 357 | 359 |
| **total native** | 3378 | 4853 | 7773 | 10104 | 13162 |

These totals agree with the measured `simulate` phase to within 3%, so the attribution is
sound. Captured in a separate short pass on the same cool device and the same APK, because
the 5 MiB logcat ring cannot hold a 75-minute run.

### What each feature costs (TIFF16, p50 deltas)

| | delta |
|---|---:|
| print route vs. film-scan route (BASE − SCAN_CLEAN) | +1523 ms |
| grain, scan route (SCAN_GRAIN − SCAN_CLEAN) | +4277 ms |
| grain, print route (PRINT_GRAIN − BASE) | +4878 ms |
| halation + DIR couplers (HEAVY − PRINT_GRAIN) | +3746 ms |

**Grain is the single most expensive thing in the engine** — one stage at ~4.1 s, more than
twice the cost of the entire print stage, and stable across routes. It is the first target
for #180, and PNG16 pays for it twice because the noise it adds defeats the encoder.

## Peak memory

| | |
|---|---|
| Peak RSS (`VmHWM`) | **1,853 MB** |
| Peak PSS | 255 MB |

PSS is sampled after the export frees its buffers, so on its own it understates the peak by
roughly 7×. `VmHWM` is a process-lifetime high-water mark, so it reads identically on every
row of the report; stepping through the samples in order attributes it: BASE climbs
1301 → 1354 → 1414 MB across JPEG/PNG16/TIFF16, then HEAVY jumps to 1841 MB and none of the
remaining exports exceeds it. **1.8 GB is the ceiling for this corpus, and HEAVY sets it.**
That is the figure #176 needs for a global memory budget, and it explains the export-OOM
history far better than the 255 MB PSS number does.

## Interactive preview (640 px slider settle)

| route | n | p50 | p95 | engine p50 | bitmap p50 |
|---|---:|---:|---:|---:|---:|
| print | 15 | 106 ms | 122 ms | 101 ms | 3 ms |
| scan | 15 | 77 ms | 85 ms | 74 ms | 3 ms |

Measured as `simulatePreview` on an already-decoded source plus the ARGB bitmap the editor
draws — the pair a slider drag repeats — with an alternating ±0.1 EV nudge so no render can
be served from a memo. Compose layout and present cost sit outside this number.
**Interactive preview is not a problem:** both routes settle inside a ~120 ms budget.

## The SLO is unreachable by engine work alone

The approved target is p50 ≤ 2000 ms / p95 ≤ 3000 ms on the warm, cache-hit path. Two facts
from this baseline bound it:

1. **No sample can claim the SLO at all.** Nothing serves an export from a content-addressed
   cache yet — that is #179 — so every sample here records `served_from_cache: false` and the
   reporter refuses to read an SLO out of a full re-render. The gate's single finding says
   exactly this, and it is the only finding.
2. **Even a free engine misses the p50 target.** Decode (977 ms) plus TIFF16 encode
   (1116 ms) is **2093 ms of non-engine work**, already over the 2000 ms p50 budget with
   `simulate` set to zero. PNG16 is worse.

So the 1–2 s exact export cannot be reached by making the engine faster. It requires the
pre-rendered/cache-hit path (#179) plus attacking decode and encode (#175 streams
quantization and output without full-image staging). Engine work (#180 grain, #160
diffusion, #178 arena, #182 pool) reduces the *cold* cost and the preview cost; it does not
by itself produce a 1–2 s export.

## The grade is free at defaults

`ColorGrade.applyInPlace` returns immediately when saturation, vibrance and gamut
compression are all neutral, and no built-in preset sets any of them, so a default export
performs no post-engine Oklab pass at all — hence the 0 ms above.

It is not free when it runs. An earlier capture of the same corpus with the grade active
(any non-neutral value, however small) measured **~4.8 s** for that pass at 12.5 MP, adding
25–40% to every cell. Two consequences: a user who moves the saturation slider pays a large,
invisible cost, which makes the grade a genuine optimization target of its own; and any
future harness must take these values from `ParamsState` rather than passing constants —
which is precisely the defect that invalidated this baseline's first two attempts (see
[the harness corrections](#history-and-invalidated-attempts)).

## Reproducing

```bash
# 5-run baseline matrix, thermal-governed, unplugged Tier A device
SPK_BENCH_DETACH=1 bash tools/baseline/run_bench.sh <the signed apk you installed> 5
# 640 px preview settle, both routes
bash tools/baseline/run_preview.sh <the signed apk you installed> 15
```

`run_bench.sh` refuses an APK whose SHA-256 is not the installed one, refuses a debuggable
build, and reads its gate threshold from `tools/baseline/corpus.json`.

## History and invalidated attempts

Three full captures were taken. The first two are retained as evidence of what they measure,
but **neither is the baseline**, and the reasons are worth keeping:

| attempt | samples | why it is not the baseline |
|---|---|---|
| 1 | 132 | The harness passed a hard-coded `saturation = 1f`, which is +1% on ColorGrade's [-100,100] scale — visually nothing, but enough to run the full per-pixel Oklab pass a default export skips. Inflated every cell by ~4.8 s. Also drifted +40% across runs (11487 → 16145 ms on BASE/PNG16) while still reporting `thermal_status: 0`, so a coarse thermal reading is not by itself proof of a cool device. |
| 2 | 132 | Grade corrected, but 113 of 132 samples throttled and 31 reached MODERATE, which the gate rejects. Throttling cost up to +56% on identical work (BASE/TIFF16: 7394 ms unthrottled, 9781 at LIGHT, 11520 at MODERATE). |
| **3** | **60** | **The baseline above.** Grade neutral, thermal 0 on every sample, gate clean but for the expected SLO finding. |

The harness changes that made attempt 3 trustworthy: grade and output descriptor derive
from the same `ParamsState` the render used; the benchmark's own C0 digest moved outside the
measured window (it was adding a steady ~122 ms of SHA-256 that no export performs, which is
why the reconciliation gap fell from 122 ms to 13 ms); the corpus declares
`require_thermal_status` and the harness waits for it per sample; and the report now names
the peak RSS and states whether the grade ran at all.

Throughout all three attempts the engine sample digest was byte-identical
(`f7ec4764c6a157c3…` on BASE/JPEG_Q95) across three separate APK builds, and C0/C3 each
showed exactly one distinct digest per cell/format in every capture — so engine determinism
holds, and only the post-engine grade ever moved the output.

*Film modeling powered by spektrafilm (GPLv3).*
