# Perf lab — the levers we had never measured

One branch, every performance idea that had been written down or argued about but
never actually tried, each reduced to a number. Companion to `docs/PERF_ROADMAP.md`
(which records what *shipped*) and `docs/research/simd-halide-experiment.md` (the
Highway/Halide round that preceded this one).

**Why one branch:** these levers compete with each other for the same milliseconds.
Measuring them one PR at a time invites picking the first one that looks good rather
than the one that wins. Everything here is default-OFF and nothing is wired into a
shipping path, so the branch can be judged on the numbers and then merged, split, or
dropped whole.

## The decision rule

A lever ships only if it is **both** faster **and** inside the oracle band
(`max_abs ≤ 1e-4` and `rms ≤ 1e-5`), *or* it is confined to the preview path under
the already-adopted "proxy approximate, export exact" policy. A lever that is fast
and outside the band on the export path is not a speedup; it is a different
renderer.

## Status of each lever

| # | Lever | Wired in? | Host verdict |
|---|-------|-----------|--------------|
| 1 | Highway **f64** lanes on the halation tier | yes, `SPK_ENABLE_HIGHWAY` + `SPK_SIMD` | bit-identical; speed inconclusive on host |
| 2 | **big.LITTLE affinity** for the fork-join pool | yes, `SPK_BIG_CORES` | no-op on host (no cpufreq data) |
| 3 | Spectral integral as **batched/GEMM-shaped** matrix ops | bench only | **~1.05× — not worth it** |
| 4 | Diffusion PSF via **Gaussian mixture** instead of direct O(ks²) | bench only | **109× but far outside the band** |
| 5 | **fp16 / f32** storage for the spatial planes | bench only | **no win; fp16 is slower** |
| 6 | Per-ABI **`-mcpu`/`-mtune`** tuning | yes, `SPK_TUNE_ARM64` | needs a device |

Host numbers below are x86 `-O2` on a shared container and are **indicative only** —
`docs/PERF_ROADMAP.md` records that this host has already mispredicted on-device
behaviour twice (the scanner LUT, and the scan-vs-print ordering). Run
`tools/perf_lab/build_push_run.sh` for the numbers that decide anything.

---

## 1. Highway f64 lanes on the halation tier

**The gap this closes.** The earlier Highway round (#155) covered only the f32 FIR,
which is the *grain and glare* blur. Halation runs through
`kernels/exponential_filter.cpp` — a separate float64 tier — and `halation_active`
defaults to 1, so that tier executes on essentially every render. The original
header argued the f64 side was not worth porting because arm64 gives it only two
lanes. That reasoning weighed lane count and ignored which stage the device said was
hot: the #146 validation round found the filming stage (grain **plus halation**)
holding the preview time the GPU scan offload could not touch. Two lanes over a
stage that runs beats eight over one that does not.

**What was ported.** Four routines in `spk::hwy_f64` (`kernels/gaussian_hwy.{h,cpp}`):
`vertical_accum`, `horizontal_interior`, `iir_step`, `axpy`. The IIR step is the one
that matters — the Young & van Vliet recurrence runs *down* rows, so columns are
independent and lane-parallel, and it is 4 multiplies + 3 adds per element, i.e.
arithmetic-bound rather than load-bound.

**Bit-identity.** Every routine keeps the scalar accumulation order and uses separate
`Mul`/`Add` — never a fused multiply-add, which would round once where the scalar
rounds twice. The four-term IIR sum reproduces C++'s left-to-right association
exactly. `tests/test_exp_filter_hwy.cpp` asserts byte-equality against transcribed
scalar references over lengths 1…4099 (straddling every lane boundary Highway can
pick) and, for the IIR, over four chained steps so a divergent lane would compound
rather than cancel.

**Host result.** All four routines byte-identical. End-to-end
`exponential_filter_per_channel_d` checksums identical with SIMD on and off. Timing
at 1024×768, four paired runs: SIMD 46.5 / 51.6 / 57.8 / 60.9 ms, scalar 52.0 / 52.7
/ 57.7 / 60.6 ms — **overlapping ranges, no verdict**. The host target is SSE2 (2
f64 lanes, the same width arm64 NEON gives), so the device is not expected to be
dramatically different in *ratio*, but it is far less noisy and it is the machine
that matters.

## 2. big.LITTLE affinity

**The gap this closes.** `kernels/parallel.cpp` had no affinity code at all. A
fork-join is only as fast as its slowest chunk: one worker scheduled onto a 2.0 GHz
efficiency core stalls the join for every other worker, so the whole map completes at
little-core speed no matter how many big cores sat idle. On a 3-cluster ARM part
that is a plausible ~2× left on the floor, and it had never been checked.

**Implementation.** `parallel_pin_to_big_cores()` reads each core's
`cpufreq/cpuinfo_max_freq` and pins to those at or above `SPK_BIG_CORE_RATIO`
(default 0.80) of the fastest. Spawned workers inherit the mask on Linux, so one
`sched_setaffinity` per thread covers the pool; a `thread_local` latch makes repeat
calls a predicted branch. When pinning is on, `parallel_num_threads()` caps the pool
to the big-core count — oversubscribing a restricted mask just queues chunks behind
each other, which is the problem, not the fix.

**Output is unaffected by construction.** Affinity changes *where* a chunk runs,
never what it computes; chunk boundaries remain a pure function of
`(count, nthreads)`, and the thread-count invariance contract already covers the
worker-count cap — that is exactly what `test_parallel` asserts.

**Gated OFF** (`SPK_BIG_CORES`), and it self-disables when the mask would cover every
core or when no cpufreq data exists (x86 hosts, most emulators), so an untouched
build is scheduler-for-scheduler what it was.

## 3. Spectral integral as batched matrix ops — **negative result**

**The claim being tested.** `scanning.cpp` computes, per pixel:
`spectral[l] = c0·cd[l][0] + c1·cd[l][1] + c2·cd[l][2] + base[l]` over 81 bands, then
`w = exp10(-spectral)·illum`, then `X/Y/Z = Σ_l w[l]·CMF[l][k]`. Structurally that is
a `(N×3)(3×81)` product, an elementwise transcendental, and an `(N×81)(81×3)`
reduction — two GEMMs with a nonlinearity between them. The independent Rust port of
this same engine reports its single largest win from expressing exactly this as BLAS
`dgemm`, which made it the most credible untried idea we had.

**Host result — it does not transfer.**

```
per-pixel loop (engine today) :   321.84 ms
tiled P=128                   :   307.85 ms  (1.05x)  EXACT
tiled+planar P=128 (GEMM-like):   301.21 ms  (1.07x)  max_abs=2.5e-14
```

Both the cache-blocked form and the planar-CMF form (unit-stride reduction, multiple
accumulators — the shape a BLAS kernel actually consumes) land at **1.04–1.07×**.
The per-pixel loop was never memory-bound: 81 bands × 3 doubles is ~2 KB of tables
that stay resident in L1 across every pixel, so blocking has nothing to recover, and
the transcendental dominates what is left.

**Why the Rust port saw a win anyway, most likely:** its reference is NumPy, where
the same restructuring also replaces per-element Python-level dispatch, and its
`dgemm` came with Accelerate's hand-tuned kernels. Neither applies to a C++ loop that
already runs the band axis contiguously with a vectorised `exp10`.

**Read this as retiring the idea, not the technique.** If a future profile shows the
*reduction* rather than the transcendental on top — the engine's `kernels/exp10.h`
makes its transcendental cheaper than this bench's `std::pow`, so the reduction
weighs relatively more there — the planar layout is the first thing to revisit, and
it costs 2.5e-14, comfortably inside the band.

## 4. Diffusion PSF via Gaussian mixture — **fast, but not at parity**

**The claim being tested.** `model/diffusion.cpp` convolves a `ks×ks` radial PSF
directly. `PERF_ROADMAP` measured that path at 13.4 s for one 640×480 render *after*
parallelisation and parked it as "replacing the algorithm is a separate,
parity-affecting decision". But the PSF is a weighted sum of 2D isotropic
exponentials, and `kernels/exponential_filter.cpp` already approximates that exact
function with a 3-Gaussian mixture — the engine sanctions the approximation for
halation while paying O(ks²) for the same shape here.

**Host result.**

```
direct O(ks^2) (engine today) :   344.25 ms
3-Gaussian mixture, separable :     3.15 ms  (109.3x)  max_abs=9.24e-02 rms=4.97e-03
```

**109× — and roughly a thousand times outside the band.** The error is real, not a
normalisation artefact: the isotropic exponential has a cusp at r=0 that a sum of
three Gaussians cannot reproduce, and that cusp is the visible core of the bloom.

**Verdict: preview-only, if at all.** This is precisely the case the adopted "proxy
approximate, export exact" policy exists for. A 109× cut to the single most expensive
optional effect would make Black Pro-Mist interactively usable for the first time,
with export still taking the exact path. It must never become the default or reach
`spk_simulate`.

## 5. fp16 / f32 plane storage — **negative result**

**The claim being tested.** `PERF_ROADMAP` item 3, written down and never attempted,
though `kernels/half.h` has shipped exact IEEE-754 converters the whole time. The
blur planes are float64: 8 bytes per sample through what should be a bandwidth-bound
filter.

**Host result.**

```
f64 plane + f64 filter (today):     4.07 ms   (reference)
f32 plane + f32 filter        :     3.94 ms  (1.03x)  max_abs=5.7e-08  within band
fp16 storage + f32 filter     :     6.32 ms  (0.64x)  max_abs=2.5e-05  within band
```

Halving the width buys **3%**, and fp16 is **slower** than the f64 baseline once the
conversion is counted. The IIR blur is not bandwidth-bound at these sizes — it is
three multiply-adds deep in a serial recurrence, so it is latency-bound, and
narrowing the data does not shorten the dependency chain.

Both precisions land inside the oracle band, so accuracy was never the obstacle;
there is simply no win to collect. Caveat worth one device run: `half_is_simd()` is
false on this host and true on arm64, so the fp16 conversion is cheaper there — but
it would have to be free *and* find a bandwidth wall that does not exist here.

## 6. Per-ABI `-mcpu` / `-mtune`

`SPK_TUNE_ARM64` + `SPK_TUNE_ARM64_CPU` (+ `SPK_TUNE_ONLY`, default on). The shipped
flags target the ABI baseline — `arm64-v8a` means ARMv8.0, so the compiler may not
use anything a 2018 phone lacked even on a 2026 flagship.

`-mtune` (the default when this is enabled) schedules for a chosen core while keeping
the baseline ISA: safe to ship, still needs a parity re-gate. `-mcpu` additionally
*raises* the baseline, which SIGILLs on older arm64 devices and changes instruction
selection — measurable here, not shippable without a device-tier policy. The CMake
block warns loudly on that path.

## Running it

```bash
bash tools/perf_lab/build_push_run.sh   # laptop + attached device
```

Three sections: the f64 Highway A/B (with a cross-process checksum equality check),
an affinity sweep over `SPK_BIG_CORE_RATIO`, and the three parity-affecting levers.
Host-side, the same binaries build with the compile lines in each file's header.

## What this branch deliberately does NOT contain

Cold start (#152), the export foreground service (#153), full-chain GPU preview
(#148), pause-render-on-gesture and the progressive pyramid (`PERF_ROADMAP` items 5
and 6). Those are app-architecture and UI work, not kernel levers — a bench cannot
answer them, and folding them in here would make one branch impossible to judge.

*Film modeling powered by spektrafilm (GPLv3).*
