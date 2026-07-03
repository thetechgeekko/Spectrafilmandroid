# Performance roadmap — toward Lightroom-class speed

Goal: interactive speed comparable to Lightroom mobile. This records **measured** numbers, the
**measured bottleneck**, and a staged plan — with the hard constraint called out up front.

## The constraint (read first)
Spektrafilm's headline value is **bit-exact parity** with the spektrafilm oracle (the whole
CI `engine-parity` gate). The techniques that make Lightroom fast — **GPU**, **fp16**, and
**LUT-accelerating the spectral integrals** — are **not bit-identical** (GPU/fp16 differ in the
last bits; LUTs trade ~5e-5 for speed). So they cannot be the *default* path without redefining
"correct". The viable model is Lightroom's own: **approximate for the interactive proxy, exact
for export.** Which precision policy to adopt is a product decision (see end).

## Measured (host, 4-core x86, `-O3 -ffast-math`, this engine)
Full print pipeline (filming → enlarger → print → scan + auto-exposure + grain + halation +
glare), 1200×900 ≈ 1.07 MP:

| Threads | Time | Note |
|--------:|-----:|------|
| 1 | 4.12 s | |
| 4 | 1.38 s | **3.0×** — fork-join scales well |
| 8 | 1.91 s | oversubscribed (4 physical cores) |

Scanner 3D-LUT (`use_scanner_lut`) on the **print route**: 1.12 s → 1.10 s (res 17), **no real
gain** — and *slower* at res 33 (LUT build cost). **Conclusion: the final scan is not the
bottleneck.** The cost is the per-pixel **81-band spectral integrals in the filming and print
*expose* stages** (run once each, per pixel), which are *not* LUT-accelerated on the default path
(`use_enlarger_lut` is now wired but opt-in / default-off — the exact non-LUT path is the parity
gate).

At ~0.8 MP/s on 4 cores, a 12 MP proxy ≈ 15 s on CPU — orders of magnitude off Lightroom's
GPU pipeline. CPU micro-opt alone won't close that; the gap is **architectural (GPU)**.

## Measured (on-device arm64, the EXPORT path — 2026-07-02, SM-S948W, 8 cores)

This satisfies the "profile on a real arm64 device" caveat below. **Full detail:
`docs/EXPORT_PERF_2026-07-02.md`. Bench: `engine/spektra-core/src/main/cpp/tests/bench_export.cpp`.**

A **12 MP full-res export = ~74.5 s**, and — contrary to the host-x86 framing above — the spectral
integrals are **not** the export bottleneck: **grain is ~67 s (90 %)**; everything else (spectral +
blurs + scan) is ~7.4 s and the PNG16 zlib write ~2.1 s (TIFF-uncompressed ~free). Crucially the big
lever here is **CPU-parallelizable AND bit-exact**, unlike the interactive-proxy levers (#1–#3 below):

- **Grain 67 s → 32.8 s (~2.0×), byte-identical — ✅ DONE (measured).** The default (sublayer) path
  runs **9 independent (channel×sublayer) `layer_particle_model` calls**, each with its own seed;
  they now run on `parallel_tasks` (`kernels/parallel.h`, coarse atomic-scheduled — `parallel_for`
  would run 9 coarse items serially past its min-chunk gate), each into a private plane, accumulated
  serially in canonical order. *Within* a call the `mt19937` stream stays serial. Grain is only
  statistically oracle-matched + excluded from the byte-exact goldens, so this is byte-identical to
  the serial output for any worker count (gated by `test_grain_parallel` + `test_parallel`, 1≡8).
  **Reality check:** 9 unequal streams on 8 heterogeneous, thermally-limited cores is a `ceil(9/8)`
  **2-wave** job → ~2×, not the ~8× the "saturate 8 cores" wording implied. >2× bit-exactly needs
  per-pixel counter-RNG (different grain realization → stat-golden regen + a product call).
- **Halation + DIR separable blurs ~5 s → ~0.7 s**, byte-identical (parallelize rows/cols; currently
  `kernels/gaussian.cpp`/`model/diffusion.cpp` have no `parallel_for`).
- **PNG16 zlib 2.1 s → ~0.5 s** via **libdeflate** (MIT) / parallel bands — decoded pixels identical.
- **Camera/enlarger diffusion** is a pathological O(n·ks²) non-separable direct convolution with an
  image-scaled radius (default-OFF; an FFT→direct regression vs the oracle) — restore FFT + parallel.

Net (grain done, rest projected): default-preset export **74.5 s → 40.0 s now (grain), → ~34 s with
items 2+5, all bit-exact.** A genuine CPU win with NO GPU/fp16/LUT precision trade-off — just
un-parallelized serial work. Grain (32.8 s) still dominates the residual; its 2-wave ceiling, not the
~7 s serial tail, is the floor for the bit-exact approach.

## Already done (the bit-exact wins)
- **Deterministic fork-join threading** (`kernels/parallel`) — fills oneTBB's role here and is
  *byte-identical across thread counts* (a property raw TBB wouldn't give for free). 3× on 4 cores.
- **Vector `exp10` SIMD** (NEON `fmla` on arm64) — removed the `pow(10,·)` bottleneck in the
  spectral integrals, bit-exact at float32.
- **Branchless half→double in `io/npy_lut.cpp::rd_f16le`** — replaced the per-element `std::ldexp`
  (a libm call ~3M times) when parsing the 192×192×81 spectra LUT with integer bit-manipulation
  (half→binary32 bits → exact widen to double). Bit-identical output (verified exhaustively over
  all 65536 half patterns + the asset), 3.1× faster conversion → the one-time engine-creation LUT
  load drops ~36 ms → ~17 ms (host). One-time/cached startup cost, not a per-render lever.
- **Proxy / half-size RAW decode** + fd decode — bounded memory on big files (#43/#44).
- **S1 — film-density memo** on BOTH routes (Option-A spatial key; key completeness
  test-enforced) — recompute filming only when a filming-affecting param changed.
- **S2 — print-density memo** — output-only edits rerun `scan()` alone (works with grain ON).
- **S3 — Kotlin retained-result grade cache** — grade-only edits do zero native work.
- **S4 — serial-loop parallelization** (DIR-coupler develop, exposure→density interp, expose
  tails via deterministic `parallel_for`) — cold scan 243 → 211 ms (−13%).

Measured 2026-07-02 (512×512 medians, `SPK_NUM_THREADS=8` on the 4-core container): warm print
edits 153–162 ms vs 402 ms cold; warm scan 144–159 ms vs 243 ms cold. Note the older 1200×900
table above **predates the memos**; print stages alone are ~5 ms at 512², so the S2 win scales
with resolution / enlarger paths / grain-ON.

## Staged plan (biggest lever first), with the parity cost of each

| # | Item | Speedup (est.) | Bit-exact? | Effort |
|---|------|---------------|-----------|--------|
| 1 | **Vulkan compute** port of the per-pixel spectral kernels (expose/print/scan) | **10–50×** (the real Lightroom lever) | No (GPU rounding) | XL — needs device + a compute-shader port. *NB:* an experimental default-OFF OpenGL ES **3D-LUT loupe** (`app/.../LutGpuPreview.kt`) is already in the tree — a different technique (a baked pointwise-look LUT sampled by GLES graphics, grain/halation forced off), **not** this per-kernel compute port; it does not subsume this item. |
| 2 | **Enlarger/expose spectral LUT** (`use_enlarger_lut` is now wired, opt-in/default-off; it LUT-accelerates the print expose integral like the scanner LUT — could extend to filming) | ~3–8× on the print route | No (~5e-5) | M–L, native |
| 3 | **fp16 intermediate buffers** on the proxy path | ~1.5–2× + ½ memory/bandwidth | No (fp16) | M, native (NEON `__fp16`) |
| 4 | **Per-stage caches** — ✅ SHIPPED (moved to "Already done" above: S1 film-density memo on both routes, S2 print-density memo, S3 Kotlin retained-result grade cache) | shipped: warm print edits 153–162 ms vs 402 cold; warm scan 144–159 vs 243 cold (512², 8 threads) | Yes (cached, identical) | done |
| 5 | **Pause/refresh render on gesture** (LR `ICBPauseRendering`) | perceptual | Yes | S |
| 6 | **Progressive pyramid render** (coarse→fine, LR `ICBSetRenderLevel`; a CPU coarse→fine two-pass progressive preview already shipped in v0.5.0 — this is the deeper native model). This doc owns the item; backlog copies point here. | perceptual instant | Yes | L |

**oneTBB:** intentionally *not* adopted — our fork-join already provides the parallelism and is
thread-count-invariant (a parity requirement); adding TBB is a dependency with no parity-safe win.
**LiteRT/ML:** a *feature* track (subject/sky masking), not performance — separate from this doc.

## Recommended sequence
#4 shipped (S1/S2/S3 memos + grade cache). Remaining parity-safe win is #5 (pause/refresh),
then the precision decision below unlocks #2/#3 (proxy-only approximation) and ultimately #1
(GPU), which is the only thing that truly reaches Lightroom-class speed.

## Decision (adopted)
**Proxy approximate, export exact** — Lightroom's model. Interactive *preview* renders may use
the fast approximate paths (expose/scanner LUT, fp16, and ultimately a GPU compute path); **export
and the CI parity gate stay bit-exact** against the oracle. Concretely: approximate paths are gated
behind a preview-only flag; the default/export path is unchanged, so the goldens never see them.

## Measurement caveat (important for whoever builds #1–#3)
The numbers above are **host x86** (`-O3 -ffast-math`), and are *indicative only* — they did **not**
behave like the arm target will:
- the scanner LUT was a **no-op (sometimes slower) at 1 MP** on x86 — the LUT build cost ≈ its
  savings at small sizes; it only pays off at higher resolutions and on the integral it covers, and
- the scan-the-negative route timed *slower* than the print route on this host (inverted vs
  expectation), i.e. the host scheduler/cache behaviour does not predict on-device cost.

**Do not commit the LUT/fp16/GPU work off host timings.** Profile on a real arm64 device
(`SPK_NUM_THREADS`, a representative 12–24 MP proxy) to (a) confirm the expose integrals are the
true hotspot on-device and (b) size the LUT resolution / fp16 / tile parameters. The bit-exact
parity gate (`test_*`) is the guardrail for the *exact* path throughout.

*Film modeling powered by spektrafilm (GPLv3).*
