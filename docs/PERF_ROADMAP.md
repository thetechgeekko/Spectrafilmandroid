# Performance roadmap — toward Lightroom-class speed

> **Current plan (2026-08-29):** use
> [EXECUTION_INDEX.md](EXECUTION_INDEX.md) and
> [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md) for the 1–2 second export
> architecture, exactness levels, current ticket graph and OSS decisions. This file preserves the
> shipped/historical performance narrative. The 6.251 s device run predates current-HEAD exact CPU
> changes and must not be presented as a fresh baseline until the release/R8 matrix is rerun.

Goal: interactive speed comparable to Lightroom mobile. This records **measured** numbers, the
**measured bottleneck**, and a staged plan — with the hard constraint called out up front.

## The constraint (historical wording; read the current exactness matrix first)
Spektrafilm's headline value is **bit-exact parity** with the spektrafilm oracle (the whole
CI `engine-parity` gate). The techniques that make Lightroom fast — **GPU**, **fp16**, and
**LUT-accelerating the spectral integrals** — are **not bit-identical** (GPU/fp16 differ in the
last bits; LUT interpolation error is profile/domain dependent—at LUT17 the locked D50 scanner
case is <=5e-5 while K75P 2383/2393 are about 0.0040/0.0073). So they cannot be the *default* path without redefining
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
- **S5 — export fast path part 1** (#120, `docs/EXPORT_FASTPATH.md` items 1+2): O(1)
  uniform-axis density lookups (`kernels/uniform_axis.h` — exposure→density **11.1×**, DIR
  couplers **9.1×** on-kernel at 1 thread; cold scan −9.4% / cold print −6.0% at 512², bytes
  identical by bracket construction with a binary-search fallback for non-qualifying axes) +
  one-shot renders skip the full-buffer memo hashing and stores
  (`spk_params.disable_buffer_memos`, set by the JNI for non-preview renders: −275 ms at
  3.1 MP, ≈ −1.1 s at 12 MP) + memo keys computed once per miss instead of twice.
- **S6 — export fast path part 2** (#121, item 4): the full-res float64 intermediates are
  gone from the common paths — direct float32 filming on one-shot no-op-geometry renders
  (`expose_f32_gain` + float32 AE metering), fused expose/scan per-pixel passes (`raw` /
  `lin_rgb` exist only when an active spatial/pointwise op needs the plane, and then
  uninitialized), free-at-last-use buffer lifetimes, move-passthrough geometry. 12 MP host
  VmHWM: print **1.10 → 0.43 GB (−61%)**, scan **0.97 → 0.43 GB (−55%)**; render transients
  ~840 → ~140 MB — a 12 MP export now fits comfortably on a 4 GB device. Byte-identical
  (no arithmetic change; scenario-G direct-vs-materialized gates), and slightly faster
  (cold scan 196.9 → 179.8 ms at 512²/4T from the killed memsets + fused loops).
- **S7 — parallelize the serial per-pixel maps** (#122): the 3D-LUT PCHIP apply (+ its
  normalization), the Gaussian/exponential filters (FIR + IIR-horizontal over rows,
  IIR-vertical over columns with chunk-local state, de/re-interleave + axpy passes), and
  the optical-diffusion direct convolution / halation mix loops all run through the
  deterministic fork-join (`parallel_for` + new work-weighted `parallel_for_weighted`).
  Byte-identical for any worker count by construction; `test_parallel` grew dedicated
  LUT-accel and diffusion-filter 1-vs-8 scenarios. Host 4-core, 3.1 MP, 1→4 workers:
  LUT apply 255.5→103.2 ms (2.5×), Gaussian f64 IIR 155.3→52.2 ms (3.0×), f64 FIR
  205.5→74.5 ms (2.8×), f32 IIR 123.5→39.4 ms (3.1×), f32 FIR 129.3→37.3 ms (3.5×),
  exponential filter 656.4→251.6 ms (2.6×), halation 1801.7→871.4 ms (2.1×), diffusion
  convolution 8704.4→2226.9 ms (3.9×). This serves every caller: halation, camera/scanner
  lens blur, unsharp, DIR-coupler diffusion, grain field blurs, glare, both spectral LUT
  routes (which preview force-enables every frame). Route-level at 640×480 / 4 workers
  (old vs new binary, output checksums identical): print+both-LUTs 213.6→174.6 ms (−18%),
  scan+halation 389.1→316.8 ms (−19%), scan+halation+diffusion-filter-0.8 49.2 s→13.4 s
  (3.66× — the direct O(w·h·ks²) convolution dominates there; replacing the algorithm is
  a separate, parity-affecting decision).

Measured 2026-07-02 (512×512 medians, `SPK_NUM_THREADS=8` on the 4-core container): warm print
edits 153–162 ms vs 402 ms cold; warm scan 144–159 ms vs 243 ms cold. Note the older 1200×900
table above **predates the memos**; print stages alone are ~5 ms at 512², so the S2 win scales
with resolution / enlarger paths / grain-ON.

## Staged plan (biggest lever first), with the parity cost of each

| # | Item | Speedup (est.) | Bit-exact? | Effort |
|---|------|---------------|-----------|--------|
| 1 | **Vulkan compute** port of the per-pixel spectral kernels (expose/print/scan) | **10–50×** (the real Lightroom lever) | No (GPU rounding) | XL — needs device + a compute-shader port. *NB:* an experimental default-OFF OpenGL ES **3D-LUT loupe** (`app/.../LutGpuPreview.kt`) is already in the tree — a different technique (a baked pointwise-look LUT sampled by GLES graphics, grain/halation forced off), **not** this per-kernel compute port; it does not subsume this item. Feasibility + exactness question settled in `docs/research/gpu-bit-exact.md` (#135): fp32-within-oracle-tolerance is the achievable bar. **In progress**: GPU M1 (#146) shipped the persistent scan host; M2 (#147) measured pointwise kernels; #148 Phase A chains filming -> printing -> scan through device-local ping-pong with one frame-input upload/readback (a cold or static-key-miss run separately copies 11 static tables), a combined f64 oracle, a 100-repeat determinism gate, and an executed 4,194,241-pixel software-Vulkan and current-Adreno runtime gate. Live product routing and spatial/stochastic stages remain. This is a direct shader graph, not a baked whole-look LUT. |
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

## The LUT build cost is now memoized (and measured)

The caveat above — "the LUT build cost ≈ its savings at small sizes" — was the LUT being
**rebuilt on every call**. `spk_simulate_preview` forces both spectral LUTs on, so every
interactive frame paid it twice. Measured on the fork author's host (arm64 M-series, 8 workers)
at the preview's `lut_resolution = 17`, per LUT: **build ≈ 1.3–1.7 ms** (17³ = 4913 samples ×
an 81-band spectral integral) and **PCHIP prepare ≈ 0.23 ms** — so ≈ 3.5 ms per print-route
frame, which is what showed up as a fixed per-call cost that a draft render could not amortise.

`kernels/lut3d_cache.{h,cpp}` memoizes both, keyed by length-prefixed raw bytes of every input
the sample function and the grid consume (compared exactly, never hashed) and bounded by an LRU
byte budget, since several keyed inputs are live slider params. A 384 px draft on that same fork
host: scan_film **13.6 → 12.1 ms**, print **26.0 → 22.9 ms** (both LUTs fetched), ~10–12%, with
the fitted fixed intercept dropping **1.9 → 0.29 ms**. Gated by `test_lut_cache_e2e` (warm
engine must equal a fresh engine byte-for-byte).

What was **left** on this path — `apply_lut_3d_pchip` interpolating the image on **one
thread** (≈ 4.4 ms per LUT for 147k px on the fork host, versus 0.4 ms for the parallelized
direct per-pixel loop it replaces) — ✅ **SHIPPED in S7 (#122)**: the apply (and its input
normalization) now runs through `kernels/parallel`'s deterministic chunking, thread-invariant
by construction (gated by a dedicated `test_parallel` LUT scenario). See S7 above for the
measured numbers.

### This repo's #118 verification numbers (grain parallelization)

Independent verification of the adopted overlay in THIS repo's environment (4-core x86 CI-class
container, issue #118): grain-only render, 1 → 8 workers (`SPK_NUM_THREADS`):
**12 MP 114.8 s → 35.2 s (3.26×)** and **3.1 MP 45.1 s → 11.7 s (3.85×)**, with the 12 MP
1-vs-8-worker outputs **memcmp byte-identical** (the fixed-block seeding contract). Host x86
caveats above apply to absolute numbers; the ratios and the byte-identity are the point.

*Film modeling powered by spektrafilm (GPLv3).*
