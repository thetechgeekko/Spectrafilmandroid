# Perf lab — the levers we had never measured

> **Historical lab notebook:** current planning and release claims live in
> [../BIT_IDENTICAL_EXPORT_ROADMAP.md](../BIT_IDENTICAL_EXPORT_ROADMAP.md). Later sections in this
> notebook supersede parts of the early verdict table (including whole-export core-affinity and
> current spatial-filter conclusions). Do not quote one row without its workload/commit/state, and
> do not present the historical 6.251 s run as a current-HEAD baseline.

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

## Status of each lever — DEVICE-DECIDED

Measured on an SM-S948W (Galaxy S26 Ultra), Adreno 840, on `claude/perf-lab @ 5de7a8b`.
Host numbers are kept below for contrast; **the arm64 column is what decides**.

| # | Lever | Host said | Device said | Verdict |
|---|-------|-----------|-------------|---------|
| 1 | Highway **f64** on halation | inconclusive | **~2% SLOWER**, and failed its own byte test | **REMOVED** |
| 2 | **big.LITTLE affinity** | no-op (no cpufreq) | **1.51×, checksum unchanged** | **SHIPPED (§13.4)** |
| 3 | GEMM-shaped spectral integral | 1.05× | **0.96–0.97×** | dead, twice over |
| 4 | Gaussian-mixture diffusion PSF | 109×, 9.2e-02 off | **85× … 692×**, 6.5e-02 off | preview-only |
| 5 | fp16 / f32 plane storage | no win | **0.97× / 0.94×** | dead |
| 6 | Per-ABI `-mtune`/`-mcpu` | untested | untested | still open |
| 7 | **GPU print-expose** offload | 3.1e-06 vs CPU | **engaged, all four log lines fired** | works |
| 8–9 | Draft gate + tunable rung | — | shipped | — |
| 10 | Irregular-kernel profile | 8.3× spread | **11.8× / 8.1× spread** | cliff confirmed |

**One lever survives as a clear win, and it is the one nobody expected.** Affinity —
pure scheduling, zero arithmetic change, bit-identical output — beat every SIMD and
codegen idea on this branch combined.

## 1. Highway f64 lanes on the halation tier — BUILT, MEASURED, REMOVED

The reasoning was sound and the result was not. The earlier round covered only the
f32 FIR; halation runs through a separate float64 tier and `halation_active`
defaults to 1, so the default-ON spatial cost had never been touched. Four routines
went in — `vertical_accum`, `horizontal_interior`, `iir_step`, `axpy`.

**The device killed it on both axes at once.**

*Speed*, S26 Ultra, SIMD ON vs OFF:

```
640x480    34.19 ms  vs  33.43 ms      (SIMD 2% SLOWER)
1024x768   73.50 ms  vs  71.96 ms      (SIMD 2% SLOWER)
```

arm64 NEON gives f64 only **two lanes**, and the IIR is latency-bound on a serial
three-tap recurrence rather than throughput-bound — so there was nothing for two
lanes to recover, and the load/store overhead showed up as a small loss.

*Correctness*, and this is the more important half: `test_exp_filter_hwy` **failed**
on device with `f64 horizontal_interior byte-identical to scalar` — 36 mismatches.
The host had passed. Reproduced immediately once the host was rebuilt with the
**flags the engine actually ships with**:

```
-O2                                 : ALL OK          <- what was tested
-O3 -ffast-math -fno-finite-math-only : FAILS          <- what ships
```

**Root cause, and the lesson.** `-ffast-math` licenses the compiler to reassociate
and contract the **scalar reference** as freely as it likes. The hand-written lanes
have a fixed order. Both are "correct"; they simply land in different places. The
byte-identity claim was never wrong about the lanes — it was **only ever validated
at `-O2`**, and the engine ships at `-O3 -ffast-math`.

A follow-up probe put the actual divergence at **max_abs 1.1e-16, max_rel 2.1e-16,
on one element per row** — about twelve orders of magnitude inside the 1e-4 band,
and independent of thread count, so the thread-invariance gate was never at risk.
So this was a **claim** failure, not a numerical one.

**Removed anyway**, because slower plus unprovable is not a trade worth keeping. The
routines, the wiring and the test are gone; `exponential_filter.cpp` is scalar again.

**What did NOT get removed, and why:** the f32 tier stays. It sits on the grain path
the device says is hot, and it showed 2.1–2.7× on host. Its test now reports
`max_abs` on every comparison and gates on **absolute** distance (the oracle band is
absolute; a relative measure explodes wherever a normalised tap sum passes near
zero). Under the shipping flags it measures **max_abs 2.38e-07** — 420× inside the
band — and it passes at both `-O2` and `-O3 -ffast-math`.

**A warning worth keeping.** The end-to-end checksums matched across `SPK_SIMD` on/off
even while the unit test failed, and the script's VERDICT line said "IDENTICAL". The
device session flagged the contradiction rather than reporting the green line, and
was right to: agreement was achieved by **not reaching the failing path** at those
sizes. An end-to-end checksum is not a substitute for a unit claim.

## 2. big.LITTLE affinity — the win, and it was not the expected one

`kernels/parallel.cpp` had no affinity code at all. Measured on the S26 Ultra
(6× 3.63 GHz + 2× 4.74 GHz), on the halation path:

```
cpuinfo_max_freq: 3628800 x6, 4742400 x2

baseline (no pinning)        73.50 ms   checksum=d44700b538679a17
SPK_BIG_CORES=1 ratio=1.00   46.41 ms   checksum=d44700b538679a17   1.58x
SPK_BIG_CORES=1 ratio=0.80   46.34 ms   checksum=d44700b538679a17   1.59x
SPK_BIG_CORES=1 ratio=0.50   69.85 ms   checksum=d44700b538679a17   1.05x
```

**1.51×, and the checksum is byte-identical in every row.** (The 1.58× below is
the first single pass; §13.2 re-measured it as a median of four and separated the
pinning from the worker-count cap — pinning is what wins.) Of everything on this
branch this is the only lever that is *both* faster *and* exact.

The shape of the result is the interesting part: ratios 1.00 and 0.80 select the
**same two prime cores** (0.80 × 4742400 = 3793920, above the 3628800 the other six
run at), and ratio 0.50 admits all eight, landing back at baseline. So the measured
finding is precise: **two 4.74 GHz cores beat all eight.** A fork-join completes when
its slowest chunk does, and with equal-sized chunks the six slower cores set the pace
for everyone while adding memory contention and join overhead.

That also **explains a separate bug**. #153 records a 12.5 MP export SIGKILLed by
Samsung device-health after 4m46s while backgrounded — and Android moves backgrounded
work onto the efficiency cores. The same export on this branch, in the foreground,
finished in **13.85 s**. Affinity and the foreground service in #153 are two views of
one problem: *this engine is extremely sensitive to which cores it lands on.*

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

## 7. GPU print-expose offload — the first real rung of #148

**The gap this closes.** GPU M1 put the *scan* integral on the GPU. The on-device
round then reported no measurable preview win, because the print and filming
stages still ran every band on the CPU. Full-chain GPU (#148) is XL work; this is
the slice of it that needed no new shader at all.

**Why no new shader.** The two integrals are the same shape:

```
scan  : out[k] = Σ_b 10^-(c · dye[b])          · icmf[b][k]
print : raw[k] = Σ_b 10^-(c · cd[b] + base[b]) · fi[b] · sens[b][k]
```

so the existing, device-validated `scan_spectral_lin.comp` runs the print route
unchanged once the per-band constants are folded differently:

```
dye[b][k]  = film.channel_density[b][k]                        (identical to scan)
icmf[b][k] = 10^-base_density[b] · filtered_illuminant[b] · sens[b][k]
```

with an identity matrix in the kernel's XYZ→RGB slot, since the print integral's
output is already the value wanted. The arithmetic the PR #145 device probe
measured inside the oracle bar is therefore the arithmetic that runs here.

**NaN contract**, mirroring `build_gpu_scan_tables`: a band whose base or channel
density (or filtered illuminant) is NaN makes `light` NaN, which the CPU zeroes
for every pixel, so **both** table rows are zeroed — with `dye` zeroed the shader
computes `D = 0` and `10^0 · 0 == 0`, reproducing `nan_to_num` exactly instead of
propagating a NaN the shader has no guard for. `sens` cannot carry NaN; it is
`nan_to_num`'d where it is built.

**It precedes the enlarger LUT rather than deferring to it**, which is the scan
offload's own precedent and matters twice over. The direct fp32 integral is
**~3e-6** against the f64 chain, tighter than the LUT's ~5e-5 interpolation
error, so preferring the LUT would trade accuracy away for nothing. And
`spk_simulate_preview` force-enables both spectral LUTs, so gating behind
`!use_enlarger_lut` would have meant the offload never engaged on the
interactive path — the one that needs it. That was measured, not assumed: the
first wiring did exactly that, and the frame counter read 2 (exports only).

**Verified** under SwiftShader by `tests/test_gpu_host.cpp`, which now asserts
the offload actually engaged rather than silently falling back:

```
info print/linear: GPU-export-vs-CPU-export max_abs=3.149e-06
info print/fused : GPU-export-vs-CPU-export max_abs=2.434e-06
info: gpu print state=1 frames=2
ok: print-expose self-check passed (spk_gpu_print_state == 1)
ok: print-expose offload engaged (spk_gpu_print_frames > 0)
```

and by a preview-path probe (three renders with a print-affecting param varied so
the density memo cannot serve them): frames 1 → 2 → 3, state 1.

**Governed by the existing toggles.** `PrintingParams::allow_gpu` is fed from the
same `allow_gpu_scan` latch scanning uses, so the Settings GPU preview / GPU
export switches cover it with no new switch. `spk_gpu_print_state()` /
`spk_gpu_print_frames()` and a matching pair of one-time logcat lines make it
externally observable — "gpu scan path ACTIVE" says nothing about the print
kernel, and unobservable silence is the failure mode #146 already caught once.

## 8–9. Draft-render gate and the draft rung

`SliderInteraction` has written `interacting` since the widget shipped, and its
own doc comment describes reading it — "render a fast live DRAFT only while a
slider is actively dragged, then the crisp full pass on release — so a discrete
edit (switch/dropdown) skips the draft". **Nothing ever read it.** The draft pass
fired on every edit, including discrete ones, where it buys nothing: the crisp
settle pass is only 500 ms behind, so the draft just burns a render and flashes a
soft frame before the sharp one lands. Now read.

The draft rung is `AppSettings.draftRenderMaxPx` (Settings → "Draft render size",
128…512, default `DRAFT_RENDER_MAX_PX` = 384) rather than a constant, so the
coarse/fine trade can be swept on a device instead of guessed — lower tracks the
finger more closely, higher makes the live frame a better preview of the settle
result.

## 10. The irregular kernels, profiled as a class — the gap a review found

Both of our SIMD/codegen efforts landed on **regular, dense, order-fixed** work: Highway
on the f32 FIR and the f64 IIR, Halide on the ks×ks PSF convolution. The genuinely
irregular kernels — the Poisson/Binomial particle samplers in `kernels/stats.cpp` that
grain is built from — got neither, and had **never been profiled as a class**. That gap
matters: the device round found grain *and* halation holding the preview time. Halation
is covered by §1 above. Grain was not covered by anything.

What makes them irregular is structural, not incidental. `fast_poisson_one` branches on
`lam >= 30`; `fast_binomial_one` branches on `n < 25` and then on `n·p·(1−p) > 10`. So
the work per sample depends on the **data**, and each branch consumes a different number
of draws from a serial `std::mt19937`.

**Host result (x86, 1 thread, 200k samples per regime):**

```
fast_poisson_one:
  lam=2      (small, inversion)          61.2 ns/sample
  lam=15     (mid, inversion)           179.6 ns/sample
  lam=29     (just under lam>=30)       300.4 ns/sample
  lam=31     (just over  lam>=30)        36.7 ns/sample
  lam=500    (normal approx)             36.0 ns/sample
  lam varies per sample                  77.7 ns/sample   <-- realistic
  regime spread 8.3x;  mixed vs best 2.16x

fast_binomial_one:
  n=24  p=0.5  (just under n<25)        218.3 ns/sample
  n=200 p=0.5  (npq=50, normal)          37.9 ns/sample
  n,p vary per sample                    61.5 ns/sample   <-- realistic
  regime spread 5.8x;  mixed vs best 1.62x
```

**There is a cliff at the threshold, and it points the wrong way.** λ=29 costs
**300.4 ns** and λ=31 costs **36.7 ns** — an 8× step *down* as λ grows, because the
inversion branch accumulates the CDF term by term (work grows with λ) while the normal
approximation above the threshold is O(1). The expensive branch is the **low-λ** one,
and low λ is the ordinary grain regime.

**What this rules out, and what it opens.** The 8.3× spread is the answer to "why not
SIMD this too": lane-parallel execution cannot absorb data-dependent cost when each lane
would want a different number of draws from one serial RNG stream — which is also why
the fixed-block seeding contract exists (it is what makes 12 MP 1-vs-8-worker outputs
`memcmp`-identical). So Highway is not the tool here, and neither is ISPC.

What it does open is a different question the bench makes askable: the cliff is in *our
own inversion loop*, not in the RNG, and it is a pure restatement of the same CDF. That
is an algorithmic target rather than a vectorisation one — and it sits on the stage the
device says is hot.

**Two honest caveats.** Host x86, so treat the ratios rather than the digits. And the
"varies per sample" case sweeps a synthetic λ range (1…600); the real per-pixel λ
distribution from `apply_grain_to_density` is what the device run should be read
against, which is why the tool prints both.

## 11. What the device run added that no host bench could

### The export finding — ISOLATED IN §13, AND IT WAS NOT THE BRANCH

> **Superseded. Read §13 before quoting anything below.** The 2×2 isolation run
> settled it: backgrounding costs **4.0×**, this branch is worth **1.12×**, and the
> ~20× headline this section was built on does not survive. The paragraph is kept
> as written because the doubt it recorded turned out to be the correct read.

On `main @ e7cd9d0` a 12.5 MP export ran **4m46s and was SIGKILLed** by
`com.sec.android.sdhms` before finishing (#153). On this branch, the same-size
export **completed in 13.85 s**.

**The cause is not isolated**, and it should not be claimed as this branch's doing
until it is. The likeliest explanation is not a code change at all: the killed run
was **backgrounded**, and Android parks backgrounded work on efficiency cores — which
§2 just measured as worth 1.58× on its own, before thermal throttling over four
minutes compounds it. Isolating this deliberately (foreground vs background, branch
vs main) is worth more than any remaining micro-optimisation on this branch.

### Where export time actually goes — 3060×4080, 13.85 s total

```
grain          3043.8 ms
halation       2139.5 ms
dir_couplers   1321.6 ms
scan            700.1 ms
scan_spatial    335.7 ms
print_expose    319.2 ms
filming_expose  171.4 ms
preprocess      134.3 ms
develop          30.8 ms
```

**grain + halation + couplers ≈ 6.5 s of the ~8.2 s in stages.** The GPU work so far
targets `scan` + `print_expose` ≈ 1.0 s. That is the honest map of the remaining
headroom, and it matches the preview finding from the earlier device round.

### All four GPU log lines fired

```
gpu preview self-check PASSED on this device/driver
gpu scan path ACTIVE (eligible preview frames render on the GPU)
gpu print-expose self-check PASSED on this device/driver
gpu print-expose path ACTIVE (print-route frames render their spectral integral on the GPU)
```

The print-expose offload — the tractable slice of #148 — **engages on real hardware**,
not just under SwiftShader.

### Cold vs warm is NOT ~15×, and the dossier said it was

Measured on this branch:

| Case | Cold | Warm | Ratio |
|---|---|---|---|
| demo 256×256 | 283 ms | 78 ms | **3.6×** |
| RAW 510×383 | 699 ms | 595 ms | **1.18×** |

The mechanism: `tc_lut_build` (26.1 ms) is paid **once, on the demo render at app
start**, so by the time a RAW is imported it already logs `tc_lut_build=0.0`. The
demo's 3.6× comes from the memos skipping filming/develop/couplers/print entirely,
not from a LUT rebuild.

**Caveat against over-correcting.** The earlier ~8.8 s cold figure behind #152 was a
much larger image than the 510×383 used here, so these are not the same measurement
and the new numbers do not simply refute the old one. What they *do* establish is
that the mechanism attributed to per-source setup is largely a **one-off at app
start**. #152 needs re-measuring at a comparable resolution before its framing is
trusted.

### Levers 3–5 and 10, arm64 numbers

```
[A] spectral integral, 1e6 px, 1 thread
    per-pixel loop (today)          438.69 ms
    tiled P=32/128/512      452.03 / 452.45 / 453.96 ms   0.97x   EXACT
    tiled+planar (GEMM-like) 455.08 / 457.30 / 459.15 ms  0.96x   1.4e-14
    -> host said 1.05x. arm64 says 0.96x. A dud AND slightly negative. Dead.

[B] radial-exponential PSF, 512x512
    lambda=4.0  ks=65    direct  395.92 ms -> mixture 4.63 ms   85.5x   7.4e-02 off
    lambda=12.0 ks=193   direct 3410.65 ms -> mixture 4.93 ms  692.2x   6.5e-02 off
    -> the win GROWS with kernel size (the mixture is O(1) in ks). 692x is enormous
       and the deviation is ~650x outside the band. Preview-only, never export.

[C] spatial-plane precision, 1024x1024, sigma=8.0 (half_simd=yes on arm64)
    f64 plane + f64 filter   5.53 ms   reference
    f32 plane + f32 filter   5.68 ms   0.97x   max_abs 6.3e-08
    fp16 storage + f32       5.91 ms   0.94x   max_abs 3.8e-05
    -> host predicted no win and fp16 worst. Confirmed exactly, even with native
       fp16 conversion available. PERF_ROADMAP item 3 is retired on real hardware.

[D] grain samplers, 1e6 samples, 1 thread
    poisson  lam=2 24.6 | lam=15 75.9 | lam=29 129.5 | lam=31 11.0 | lam=500 11.0
             varying 14.6 ns    spread 11.8x   mixed/best 1.33x
    binomial n=8 31.0 | n=24 91.7 | n=26 72.9 | n=200 11.3 | n=200 p=.01 29.9
             varying 35.7 ns    spread 8.1x    mixed/best 3.17x
    at 12 MP x 3ch, one draw each: poisson 0.52 s, binomial 1.28 s (1 thread)
    -> the cliff REPRODUCES and is sharper than host: 129.5 ns at lam=29 vs
       11.0 ns at lam=31. Still the wrong way round, still our own inversion loop.
```

### Five script bugs the device run found, all fixed

The runner had never been executed anywhere but this container, and it showed:
`uname` on Git Bash yields `mingw64_nt-…` where the NDK wants `windows-x86_64`;
no `-static-libstdc++`, so the pushed binary died on a missing `libc++_shared.so`;
`-I$CPP` unquoted, which word-splits on a checkout path containing spaces; SDK
auto-detect assuming the Linux layout; and `set -e`, which aborted the whole run on
the first failing test — **losing the affinity sweep and every lever to one non-zero
exit.** That last one is the worst of the five: a correctness failure is exactly when
the rest of the data matters most.

## 12. "Don't accept defeat" — where the next win actually is

The owner's direction after the device run was that a flat result is not a stopping
point, and specifically asked whether OpenCV (including OpenCV on GPU) is the route.
Taking that seriously means pointing it at the stage the device says is hot, not the
stage that is convenient.

### OpenCV on halation — the arithmetic says no, and it is not close

Halation's 2139 ms looks like a textbook case for a tuned CV library. It is not, for
a structural reason:

- `gaussian_iir_plane` uses the **Young & van Vliet 3rd-order IIR** — 3 taps forward
  and 3 back, **O(1) per pixel regardless of sigma**.
- `cv::GaussianBlur` is a **separable FIR** — O(ks) per pixel, ks ≈ 6σ+1.

Halation runs decay 14 px through the 3-Gaussian mixture, so its widest component is
σ ≈ 38.8, i.e. **ks ≈ 233**. Swapping our 6 taps for OpenCV's ~233 is roughly **39×
more arithmetic per pixel**, and no amount of SIMD in OpenCV's kernel recovers a 39×
algorithmic gap. OpenCV would very likely be *slower* here, not faster.

Two measurements from this branch already point the same way and were taken on the
exact code in question: Highway f64 lanes on this path came out **2% slower** (§1),
and an f32 plane through an f32 filter came out **0.97×** (§5, lever C). The filter is
**latency-bound on a serial recurrence**, not throughput-bound — which is precisely
the regime where a faster library kernel buys nothing.

### OpenCV on GPU — a second GPU stack for a problem we already solved

OpenCL is not in the Android NDK and is vendor-optional; Qualcomm ships a driver but
apps reach it by `dlopen`, which is not a supported contract. We already have a
working, device-validated Vulkan compute host with a self-check and automatic CPU
fallback. Adding OpenCL means a second GPU stack with the same non-bit-reproducible
float behaviour, for stages we can already dispatch through the host we have.

Plus size: core+imgproc across three ABIs is comparable to or larger than the entire
current 15.9 MB APK.

**None of that is "OpenCV is bad."** It is a fine library, Apache-2.0, and its
`imgproc` grab-bag is genuinely useful if masking ever needs broad CV ops. It is the
wrong tool for *this* bottleneck.

### The lever that IS there — and it is bit-exact

Grain is **3043 ms**, the largest stage in the export. Lever D found the cliff; a
follow-up probe found where the time inside it actually goes:

```
fast_poisson_one(lam=29)       160.92 ns/sample
  draws consumed per sample     30.00        (Knuth: expected lam+1)
  30 draws x 4.11 ns         =  123.3 ns     -> 77% of the sample
StatsRng::uniform()              4.11 ns/draw
raw mt19937 word                 1.69 ns/word
StatsRng::normal()              13.11 ns/draw
```

**77% of the Poisson sampler is the random number generator**, not the sampler logic.
Knuth's method multiplies uniforms until the product falls below `exp(-λ)`, so it
consumes λ+1 draws — 30 of them at λ=29, and low λ is the ordinary grain regime.

That reframes the problem completely:

- **Changing the sampler** (rejection methods, a lower normal-approximation threshold)
  would cut the draw count, but changes which numbers come out. That is a visible
  change to the grain a user sees, and it is not bit-exact.
- **Changing how MT19937 produces its words is free.** The Mersenne Twister's output
  is defined by a fixed linear recurrence over a 624-word state. *How* those words are
  computed is an implementation detail — a block-at-a-time SIMD twist and tempering
  emits the **identical sequence**. Same draws, same samples, same grain, same bits.

So the one route that attacks 77% of the largest stage **without touching a single
output bit** is a vectorised MT19937 block generator. `uniform()` at 4.11 ns against a
1.69 ns raw word says the distribution wrapper is already thin; the win would come
from the generation itself.

### The probe was built. The hypothesis is WRONG — recorded, not buried.

`tools/perf_lab/mt_probe.cpp`. It proves byte-identity first and only then reports a
number, because the f64 lever on this branch was removed precisely for having done
that in the other order.

**Both proofs passed.** The scalar transcription matches `std::mt19937` over 10 M
words with 0 diffs; the SIMD twist matches it over 10 M words with 0 diffs; and
Poisson samples drawn through the SIMD engine are **identical over 200 000
mixed-λ draws**. The bit-exactness claim is real: the twist's 227-word dependency
distance does allow an intra-state vectorisation that changes nothing.

**And it buys nothing where it matters.**

```
std::mt19937 word            1.76 ns/word
SIMD twist, scalar temper    1.27 ns/word   1.38x
SIMD twist + SIMD temper     1.14 ns/word   1.55x   <- the generator IS faster

uniform(), std engine        4.37 ns   = 2.48 engine words + wrapper
uniform(), SIMD engine       4.17 ns   1.05x        <- and it stops there
SIMD engine word alone       1.25 ns   -> wrapper costs 1.68 ns/draw

fast_poisson_one(lam=29)  std 145.82 ns  vs  SIMD 157.00 ns   (0.93x)
```

A 1.55× on raw words became **1.05× on `uniform()`**, and the sampler measured
between 0.93× and 1.08× across runs — i.e. **noise**. No win.

**Why the model was wrong.** §12 assumed a `uniform()` draw is essentially two engine
words, so a faster word would carry most of the way. It is not:
`std::uniform_real_distribution<double>` costs **1.68 ns per draw on top of its two
words** — roughly **40% of every draw**, and that share does not shrink when the words
do. Amdahl, applied one layer lower than the analysis had looked.

**One mistake worth naming**, because it nearly produced a wrong answer in the other
direction: the probe's first shape tempered per word inside `next()`, so the block
bench read 1.97× while the sampler read **0.81× — slower**. The vectorised temper only
counts if the sampler actually goes through it. Handing words out of a pre-tempered
buffer fixed the shape; the honest verdict did not change.

### What this leaves

The residual finding is sharper than the hypothesis it replaced: **the distribution
wrapper, not the generator, is ~40% of every uniform draw** — about 50 ns of a 146 ns
Poisson sample at 30 draws. `generate_canonical<double, 53>` has a defined formula, so
inlining it to produce the identical doubles is a real bit-exact target and a bigger
one than the engine words ever were. It is also stdlib-implementation-shaped, which
makes it a different kind of risk; it is not attempted here.

**The ordered recommendation below is unchanged by this result** — affinity is still
the measured 1.51×, and the export finding is still worth more than any of it.

### What to do now, in order

1. ~~Ship the affinity win~~ — **done (§13)**: reachable from the app as a setting
   (`spk_set_big_cores`), because the env-var gate was unreachable from a running
   JVM. Default OFF pending a whole-render A/B.
2. ~~Isolate the export finding~~ — **done (§13), and the answer was
   foreground-vs-background**. #153's foreground service landed here as a result;
   it is worth ~4×, more than every kernel lever on this branch combined.
3. ~~Probe the vectorised MT19937~~ — **done, and it is a no**: bit-identical
   (0 diffs over 10 M words and 200 k samples) but 1.05× on `uniform()` and noise on
   the sampler, because the distribution wrapper is 40% of a draw.
4. Leave OpenCV out of the render path.
5. **Open**: why the fork-join is non-monotonic in thread count (§13), and what the
   remaining unexplained ~4.3× in the original SIGKILLed run actually was.

## 13. Second device run — the two things only a phone could answer

Device: SM-S948W. Both questions were designed so the answer would change what we
build next, and both did.

### 13.1 Export isolation — the 20× was not the branch

Four runs, same image, same settings, screen held on:

| # | branch | app state | result | export time |
|---|---|---|---|---|
| 1 | `claude/perf-lab` | foreground | completed | **13 894 ms** |
| 2 | `claude/perf-lab` | HOME immediately | completed | **55 631 ms** |
| 3 | `main` | foreground | completed | **15 574 ms** |
| 4 | `main` | HOME immediately | completed | **66 278 ms** |

- **Backgrounding: 4.00× (branch), 4.26× (main).**
- **This branch: 1.12× foreground, 1.19× background.**

So the ~20× headline of §11 decomposes into a 4× scheduling effect and a 1.12×
code effect. Every kernel lever on this branch put together is worth less than
one-third of what leaving the app costs. That is the finding.

The slowdown is broad, not one stage — which is what a cpuset move looks like and
what a single slow kernel does not (run 1 → run 2):

```
preprocess     134.9 ->   737.5   5.5x        print_expose   324.7 ->  1232.0   3.8x
grain         3086.7 -> 15811.6   5.1x        scan           742.9 ->  2381.8   3.2x
filming_expose 173.6 ->   872.5   5.0x        scan_spatial   380.4 ->  1149.9   3.0x
develop         30.4 ->   121.4   4.0x        halation      2125.2 ->  4454.6   2.1x
                                              dir_couplers  1296.7 ->  2710.8   2.1x
```

**What is still NOT explained.** The run did not reproduce the kill: run 4 is
exactly the configuration that died at 4m46s last time and it finished in 66 s.
Backgrounding accounts for 4× of the original ~20×; the remaining ~4.3× is
unaccounted for. The differences from the original are real — the screen was held
on and the app only backgrounded (not screen-off, not minutes of dwell), and the
format was ULTRA_HDR rather than JPEG. **The direction is settled; the magnitude
is not.** Reproducing the kill needs screen-off and several minutes of dwell,
which is a different experiment.

Minor, recorded rather than smoothed: branch runs wrote 10 348 734 bytes, `main`
runs 10 348 689 — 45 bytes apart on identical inputs, unverified, probably
metadata. `main` emits no `stage timings` line (that logging is new here), so runs
3–4 have totals only.

### 13.2 Affinity — faster cores, not fewer threads

`SPK_BIG_CORES` does two things at once (pins to the prime cores **and** caps the
pool to their count), so the win could have been either. Separating them, medians
of four passes at 1024×768 (the first pass came out non-monotonic, so single
passes were not trusted):

| config | median ms | vs baseline |
|---|---|---|
| baseline (no env) | 71.86 | 1.00× |
| `SPK_NUM_THREADS=1` | 59.61 | 1.21× |
| `SPK_NUM_THREADS=2` | 68.70 | 1.05× |
| `SPK_NUM_THREADS=4` | 54.67 | 1.31× |
| `SPK_NUM_THREADS=6` | 70.39 | 1.02× |
| `SPK_NUM_THREADS=8` | 68.21 | 1.05× |
| `SPK_BIG_CORES=1` ratio 1.00 | **47.58** | **1.51×** |

Checksums identical in every run (`ec74c9dfb7bc2f31` at 640×480,
`d44700b538679a17` at 1024×768). Nothing diverged.

**Capping the pool is not what wins.** `SPK_NUM_THREADS=2` — the same worker count
pinning ends up with — lands at baseline. Pinning is doing the work, so core
placement is the mechanism and thread-count tuning is not a substitute.

This also revises §2's 1.58× down to **1.51×**: that figure was a single pass, this
is a median of four.

**Open question — thread count is not monotonic.** T=4 (54.67) beats T=1, T=2, T=6
and T=8, and T=2 is worse than T=1. A plausible mechanism: the join waits on the
slowest chunk, so the result depends on where each chunk lands, and at small worker
counts a single chunk on an efficiency core paces everything — at T=4 the chunks
are small enough that even a slow one finishes quickly. That is a hypothesis, not a
measurement. Practical consequence: **T=4 alone recovers ~60% of the pinning win
with no affinity code**, which is a cheap fallback where pinning is unavailable.

### 13.3 A provenance correction

The run reported that `perf_lab --halation-only` does not exist and that the
73.50/46.41 figures came from `test_exp_filter_hwy`. Checked against the branch:

- `--halation-only` **does** exist (`perf_lab.cpp:666`) — added in `87c60af`,
  three commits after the `5de7a8b` the device session had checked out.
- The four runner-portability bugs **were** fixed, also in `87c60af`. At `5de7a8b`
  the script genuinely has `set -euo pipefail` and none of the rest.
- `test_exp_filter_hwy.cpp` was **deleted** in `87c60af` together with the f64
  Highway tier it tested (`grep -c hwy exponential_filter.cpp` = 0).

So §13.2 was measured with a **stale binary benchmarking a removed feature**. The
relative conclusion survives — every config ran that same binary, and both benches
drive the same `exponential_filter_per_channel_d` through the same fork-join — but
the absolute milliseconds do not describe shipping code, and the provenance as
reported was wrong. A stale checkout is the cause of all three discrepancies.

### 13.4 What was built in response

> **SUPERSEDED BY §15.** Both changes below were built on predictions that the next
> device run falsified. The foreground service does **not** recover the 4× (no
> service-tier cpuset on that device contains the prime cores), and the pinning
> setting is a **1.41× regression** on a whole render. Left as written, because the
> reasoning is the record of what was believed and why.

| Finding | Change |
|---|---|
| Backgrounding costs 4× | `ExportForegroundService` — holds the process in the foreground scheduling group for the duration of an export (#153). Runs no work of its own, so it touches nothing on the render path. |
| Pinning is worth 1.51×, and the env gate is unreachable from a running JVM | `spk_set_big_cores()` / `spk_big_core_count()` + a **Use performance cores** setting. Applies mid-session: the policy is generation-counted so an already-pinned worker re-evaluates, and turning it off restores the mask captured before the first pin. |
| Both need a whole-render A/B | Setting defaults **OFF**. 1.51× is one spatial filter on one device. |

Gate: all 38 parity tests green, including a new `test_parallel` scenario 8 that
toggles pinning off→on→off around renders and asserts byte-identical output plus a
correct `spk_big_core_count()` when off. Stated honestly, that scenario gates the
**plumbing**, not the pinning — on a homogeneous host `detect_big_cores` returns 0
and the pin is a no-op, so placement can only be observed on a big.LITTLE device.

## 14. The gate itself was measured — CI tested a build nobody installs

§11 removed the Highway f64 tier because a byte-equality claim established at `-O2`
did not survive the shipping flags. That finding named a second, larger problem in
passing and then left it open: **the same is true of the entire parity suite.**

| | flags | who compiles it |
|---|---|---|
| CI `engine-parity` | `-O2` | `.github/workflows/ci.yml` |
| CI `engine-native` | `-O2` | same |
| debug APK | `-O2 -g` | `CMakeLists.txt` (pinned deliberately, so timings mean something) |
| **release APK** | **`-O3 -ffast-math -fno-finite-math-only`** | `CMAKE_CXX_FLAGS_RELEASE` |

`CMakeLists.txt` calls this "the documented divergence". It was named in three
places and measured in none. Every parity number this project has ever quoted
describes a binary that only developers run.

### The measurement

The full 38-test table, recompiled at the shipping flags:

```
SPK_PARITY_EXTRA_FLAGS="-O3 -ffast-math -fno-finite-math-only" \
  bash tools/parity/run_engine_parity.sh
```

**`engine-parity: ALL OK` — 38/38, zero `FAIL` lines.** The release configuration
sits inside the oracle band.

### Two controls, because a green null result is worthless without them

A passing run is also what you get when the experiment silently did not happen, so
both failure modes were ruled out before the result was believed:

1. **Do the flags reach the compiler?** The same channel, fed `-fspektra-not-a-real-flag`,
   produces `g++: error: unrecognized command-line option` and `engine-parity: BUILD
   FAILURES`. It reaches `g++`.
2. **Does `-O3` beat the script's own earlier `-O2`?** `test_half` built both ways:
   different SHA-256, and **706 232 vs 588 520 bytes** — 20% more code, which is what
   `-O3` inlining and unrolling look like. Materially different codegen, not a no-op.

### What this does and does not establish

- It establishes the **band** (`max_abs ≤ 1e-4`, `rms ≤ 1e-5`) at the shipping flags.
- It does **not** establish byte-equality between the `-O2` and `-O3` builds. `-ffast-math`
  reassociates; §11 measured that divergence at 1.1e-16. The byte-identity gates
  (`test_parallel`) compare a build against itself, so they are unaffected — but
  cross-optimisation-level byte-identity was never claimed and is still not claimed.
- It is **host x86-64 g++**. The shipping engine is **NDK clang on arm64**. This closes
  the *flags* half of the gap; the *toolchain and architecture* half is untested, and
  `CLAUDE.md` already says bit-exactness is not expected across architectures.

### What was built in response

`engine-parity` is now a two-leg matrix — the same 38 tests at `-O2` and at the
shipping flags — with `fail-fast: false` so one leg cannot mask the other. The first
leg keeps its exact old check name, since that is what any branch protection refers
to. The local runner already supported the second leg through
`SPK_PARITY_EXTRA_FLAGS`; its header now documents the recipe, because a runner that
advertises itself as mirroring CI should say when a plain run does not.

The cost is one extra parallel runner for ~13 minutes per push. The thing it protects
is the prime directive.

## 15. Third device run — both §13 changes were wrong, and the data says why

§13 built two things off two predictions. A device run on `e1063dc` tested both.
**Both predictions failed**, and in each case the same run explains the failure.

### 15.1 The foreground service does not recover the 4×

7 runs, 12.5 MP, SM-S948W. Backgrounded median **55063 ms** vs foreground **14102 ms**
= **3.90×**. The pre-fix number was 4.00×. Nothing meaningful changed.

The service is not failing to start. It starts every run, and the notification posts
every run: `isForeground=true foregroundId=1001 types=0x1`, `Background started FGS:
Allowed`, and no warning in any run.

**My reading guide for this test was wrong and the run corrected it.** I wrote that
4× *with* the notification showing "would mean the cpuset was never the mechanism."
That inference does not follow, and the counter-evidence is direct — same APK, same
image, same action, only the cpuset differing:

```
  cpuset /foreground-boost -> 14927 ms   (1.06x, fully fixed)
  cpuset /moderate         -> 53-56 s    (3.9x)
```

The cpuset predicts the result perfectly. **The cpuset IS the mechanism.** What fails
is the assumption that a foreground service determines which cpuset you get.

Why it cannot work on this hardware:

```
  cpu0-5  max 3.63 GHz          cpu6-7  max 4.74 GHz   (prime pair)

  /top-app          cpus=0-7
  /foreground-boost cpus=0-7        <- transient interaction boost, not FGS-earned
  /foreground       cpus=0-5        <- NO prime cores; best an FGS normally rates
  /moderate         cpus=0-1,4-5    <- where backgrounded exports land
  /background       cpus=0-1,4-5    <- IDENTICAL mask to /moderate
```

`/moderate` is byte-identical to `/background` in CPU terms: half the cores, zero
prime cores. That is the entire 4×. **No service-tier cpuset on this device contains
cpu6/7**, so changing `foregroundServiceType` cannot rescue it either.

**The service is still worth keeping, for the other half of #153:**

```
  with FGS running:  oom_score_adj =  50
  after it stops:    oom_score_adj = 700
```

That is the anti-SIGKILL protection, and it works. Keep the service for
kill-resistance; stop attributing speed to it. Its KDoc now says so.

### 15.2 The 1.51× pinning win is a 1.41× regression on a whole render

`big cores set=true detected=2` — detection is correct (0.80 × 4.74 GHz = 3.79 GHz,
and cpu0-5 at 3.63 GHz fall below it), so the test is valid.

```
  big_cores=false : 14254  14476  14745   median 14476 ms
  big_cores=true  : 19621  20458  21759   median 20458 ms     ON is 1.41x SLOWER
```

Per stage, medians of 3, ON/OFF:

| stage | OFF | ON | ratio |
|---|---|---|---|
| **grain** | 3620.1 | **8612.4** | **2.38×** |
| filming_expose | 177.4 | 390.6 | 2.20× |
| print_expose | 300.7 | 555.2 | 1.85× |
| develop | 35.5 | 54.2 | 1.53× |
| scan | 737.6 | 947.4 | 1.28× |
| scan_spatial | 385.5 | 481.1 | 1.25× |
| dir_couplers | 1348.9 | 1465.9 | 1.09× |
| halation | 2054.8 | 2158.9 | 1.05× |
| preprocess | 150.0 | 149.2 | 0.99× |

**Grain is the regression**: +4992 ms of a +5982 ms total delta. Clean split —
compute-bound pointwise stages lose badly, spatial/bandwidth-bound stages barely
notice.

Mechanism, read from source: `parallel.cpp` caps the pool to the big-core count when
pinning is on, and `grain.cpp` inherits it via `parallel_num_threads()`. So ON is
**2 workers on 2 prime cores** against OFF's **8 workers across all 8** — 9.48 GHz of
aggregate clock against 31.26 GHz. A 1.31× per-core clock edge cannot cover a 3.3×
compute deficit. The `e1063dc` commit message predicted this exact failure mode as a
risk; it is what happened.

The bench was unrepresentative: one spatial filter on a small fixture is
thread-spawn-overhead dominated, where 2 workers legitimately beat 8. A 12.5 MP
render is not that.

**The Settings row is removed.** Its copy read "measured 1.5x faster on the test
device" — on a whole render, on the device it was measured on, it is 1.41× slower.
The pref and the native API stay for experiments (which is how this A/B was actually
run); there is simply no user-facing switch promising a win that does not exist.

The bit-exactness half held up: checksums identical in every configuration.

### 15.3 Three bugs the run surfaced, all verified against source and fixed

| Bug | Verified | Fix |
|---|---|---|
| Opening Settings destroys the loaded image — `screen = Screen.SETTINGS` swaps the top-level composable, so `EditorScreen` leaves composition and its `rememberSaveable` source identity goes with it. Coming back, you are silently on the demo image. | `MainActivity` nav `when (screen)`; `sourceUri`/`sourceKind`/`sourceName`/`rotation` are all `rememberSaveable` | Nav wrapped in `rememberSaveableStateHolder()` + `SaveableStateProvider(screen)`, so each destination keeps its own bucket. |
| "Preview max size" inert for real photos — honoured for the demo, ignored for RAW. | `ParamsState.loadFrom` restored `previewMaxSize` from the incoming params, and recipes are keyed by source uri, so opening a RAW replayed its saved value | `loadFrom` no longer reads it back — matching `gpuEngine`/`gpuExport`, whose docs two lines below already state that policy. Unblocks the 1–2 MP sweep for #146. |
| `POST_NOTIFICATIONS` declared but never requested, so the export notification is invisible on a real install. | manifest declares it; zero `requestPermission` calls in app source | Requested in context at the first export, non-blocking. |

The third one has a sting worth keeping: my own reading guide said "no notification
appeared → the service never started." On a shipping build that would have been a
false negative.

### 15.4 Where this leaves the performance work — and a 39% blind spot

The 4× is real, it is the cpuset, and no foreground service can reach the prime cores
on this device — **that lever is spent**. It is also only ever paid while the user
leaves the app; the foreground path was never slow. So the target is the **~14 s
foreground export**.

Summing the stage timings from that run against the total exposes something nothing
has looked at:

```
  preprocess    151.7    filming_expose  177.4    halation      2054.8
  develop        36.0    dir_couplers   1377.1    grain         3620.1
  print_expose  288.3    scan            728.4    scan_spatial   385.5
  print_digest    0.4
                                     engine stages =  8819.7 ms   (61%)
                                     export total  = 14476.0 ms
                                     UNACCOUNTED   =  5656.3 ms   (39%)
```

**39% of an export is outside the engine entirely** — RAW decode, the full-resolution
bitmap grade (`simResultToBitmapGraded`: saturation, vibrance, gamut compression,
local adjustments — a per-pixel pass that appears in no stage timing), and the
Ultra HDR encode plus file I/O.

This governs the GPU question directly. Against a **1–2 s** export target:

- Even if a GPU pipeline made **every engine stage free**, the floor is still
  **5.66 s** — 3–6× above target. GPU is necessary and **not sufficient**.
- Within the engine, three stages are 80% of the time: **grain 3620 + halation 2055 +
  dir_couplers 1377 = 7052 ms**. `scan` and `print_expose` are already offloaded and
  measured **3.1e-06** against the CPU — 32× inside the 1e-4 band, so GPU float does
  not break the band, only bit-exactness.
- Grain is the largest single stage **and** the one whose parity gate is already
  **statistical** (`test_grain`/`test_grain_sublayer` check mean and noise std, not
  bytes). It is therefore the highest-value GPU target with the least gate friction.
- The LUT route (`spk_bake_cube_lut`) cannot substitute: a 3D LUT is *pointwise*, so
  it carries colour but not grain, halation or glare — precisely the 5.7 s that
  dominates.

So the decision between an incremental GPU path and adopting a full external GPU
pipeline cannot be made from current data: nobody knows whether that 5656 ms is
mostly decode or mostly encode, and the two have completely different answers.

**Instrumented, pending the next device run.** The export now logs

```
  export phases ms: decode=… simulate=… grade=… encode=…
```

alongside the existing `stage timings` line. Until that number exists, any plan to
reach 1–2 s is a guess.

### 15.5 A method note, because it has now cost time twice

The previous run was measured against a stale checkout and produced three false
findings. This run produced two contaminated results before the tester noticed that
opening Settings had swapped the subject to the demo image — the first four A/B
attempts "succeeded" in 2.2 s and looked like a spectacular win.

**Both classes of error look exactly like clean data.** Anything that changes app
state between A and B needs a positive confirmation in the log that the subject is
still what you think it is — here, grepping `decode kind=RAW 383x510` on every run.

### 15.6 A prediction about `grade`, written before the measurement arrives

> **OUTCOME (§16): survived its falsifier, but it was not the big fish.** `grade`
> measured **707 ms** — above the "a few tens of ms" that would have falsified it, so
> the three single-threaded JVM passes are real and cost real time. But it is 4.9% of
> an export against decode's 35.3%. Calling it "the cheapest remaining win on the
> export path" was wrong: it is cheap, it is a win, and it is seventh of the size of
> the actual problem. The prediction was right about the mechanism and wrong about
> the stakes.

Recorded now, while the device is unreachable, so the pending run tests it rather
than confirms it after the fact — the discipline §11 adopted after the f64 tier was
removed for benchmarking before proving.

`simResultToBitmapGraded` -> `gradeBufferToBitmap` is three passes over the
full-resolution buffer, and **all three are single-threaded JVM per-pixel loops** —
while the entire native engine beside them is multi-threaded:

| pass | early-out |
|---|---|
| `ColorGrade.applyInPlace` — `OutputCctf.decode` x3, optional gamut compression, Oklab chroma | yes, if saturation/vibrance/gamut are all inactive |
| `MaskCompositor.applyInPlace` — **one full pass per active local adjustment** | yes, if no adjustment has an op and >1e-4 coverage |
| `simResultToBitmap` — clamp, round, pack to ARGB8888, `setPixels` per strip | **NO. It always runs.** |

**The prediction.** `grade` will be materially non-zero even with every slider at its
default, because that last pass is an unconditional ~12.5 M-iteration JVM loop doing
three `FloatBuffer.get()` calls, a clamp, a round and a pack per pixel. With chroma
or gamut active it is two such passes; with N local adjustments, 2+N.

**Why this matters more than its size.** None of it is parity-gated. It is entirely
post-engine — the project's own playbook tier — so parallelising it across the same
fork-join the engine already uses, or moving it native, touches no golden and risks
no band. If the prediction holds, it is the cheapest remaining win on the export path
and it needs no GPU at all.

**What would falsify it:** `grade` coming back at a few tens of ms. That would mean
the JIT is handling these loops far better than the shape suggests, and the 5656 ms
lives in decode or encode instead — in which case this section is wrong and the
answer is elsewhere.

Either way the next run decides it, which is the point of writing it down first.

## 16. The phase split — decode is the biggest thing outside the engine

> **The first numbers in this section were contaminated and are corrected in §16.6.**
> The run below was measured on a **rotated** source without that being noticed as a
> variable, which inflated `decode` from 3616 ms to 5093 ms. The corrected split, and
> the 1155 ms rotation surcharge that explains the gap, are in §16.6. The table is
> kept because the reconciliations and the encode result stand.
>
> **Second, larger correction: every absolute number in §16 and §17 is a DEBUG-BUILD
> number.** Three of the four native modules were compiling at `-O0` in debug. See
> §19 for the release figures, which are 2-8x smaller and change which levers matter.
> The *ratios within* a build (rotation 1155 -> 40, grade 650 -> 159) still stand;
> the *shares of the export* do not.


The device came back and the run completed. Same RAW, 3060x4080, Ultra HDR / Q100 /
full resolution / sRGB, foreground throughout, `decode kind=RAW 3060x4080` confirmed
on every run per §15.5. Run 1 carried the permission prompt and is excluded from the
medians; runs 2-5 are clean.

| run | decode | simulate | grade | encode | ok in | sum | total−sum |
|---|---|---|---|---|---|---|---|
| 1* | 5191 | 9436 | 663 | 217 | 15532 | 15507 | 25 |
| 2 | 5005 | 8342 | 715 | 220 | 14298 | 14282 | 16 |
| 3 | 5122 | 8511 | 706 | 217 | 14572 | 14556 | 16 |
| 4 | 5064 | 8250 | 708 | 215 | 14250 | 14237 | 13 |
| 5 | 5505 | 8271 | 706 | 217 | 14711 | 14699 | 12 |

**Medians of runs 2-5:**

```
  decode     5093 ms   35.3%
  simulate   8306 ms   57.5%
  grade       707 ms    4.9%
  encode      217 ms    1.5%
  residual     15 ms    0.1%
```

### 16.1 What this settles

**Decode is the answer, and encode is a rounding error.** 5093 ms against 217 ms —
a factor of 23. Decode alone is larger than any single engine stage: 1.5× the whole
grain stage, 2.5× halation.

Against the 1-2 s target, if a GPU pipeline made **every engine stage free**:

```
  decode 5093 + grade 707 + encode 217 + residual 15  =  ~6032 ms
```

So GPU-on-the-engine cannot reach the target on its own — §15.4 said that as
arithmetic, and this is the measurement. What changes is *where* the remaining work
is: **what LibRaw is doing for 5.09 s on a 25 MB file** is now the highest-value
unexamined question in the project. Threading, half-size paths, demosaic choice —
none of it has ever been profiled. It also sharpens the vkdt question rather than
settling it: vkdt-style pipelines put demosaic on the GPU, so decode is precisely the
part that would move if we went external.

### 16.2 Both reconciliations closed, and two pre-flight warnings were wrong

The instrumentation fixes in `876bed3` worked. **Reconciliation 1 closes to 12-25 ms
(0.1%)** — the head/mid/tail gaps were real but together worth ~15 ms, not the
visible hole predicted.

**Reconciliation 2 also closes, and the prediction that it would NOT was wrong.** The
warning was that `simulate` must exceed the stage sum because `spektra_jni.cpp`
mallocs ~140 MB and wraps it *after* printing the timings. Measured, the engine
boundary is exactly where we thought:

| run | stage sum | simulate | delta |
|---|---|---|---|
| 1 | 9437.4 | 9436 | −1.4 |
| 2 | 8391.5 | 8342 | −49.5 |
| 3 | 8552.6 | 8511 | −41.6 |
| 4 | 8295.2 | 8250 | −45.2 |
| 5 | 8345.3 | 8271 | −74.3 |

**A small real finding hides in the sign.** The delta is consistently *negative*: the
per-stage timers sum to 40-75 ms MORE than the wall clock of the entire native call,
which cannot literally be true. Some stage timers overlap or double-count by
**0.5-0.9%**. Not a boundary problem, but the `stage timings` line is very slightly
optimistic and should not be treated as exact.

**And a retraction of ours.** §15's fix moved the `POST_NOTIFICATIONS` request off the
export path on the theory that a permission dialog would demote the process out of
`/top-app` and cost 3.90×. Measured, it does not:

```
  cpuset WHILE PERMISSION DIALOG UP: /top-app  adj=0
  mCurrentFocus=...GrantPermissionsActivity
```

`GrantPermissionsActivity` overlays the task without moving the process. Run 1's
~7% overshoot is taps and dialog, not a cpuset move. The change is kept — asking
while the user is still choosing options is better anyway — but **its stated
justification was falsified**, and that is worth more than the change.

### 16.3 Two bugs of our own, one of them a stranding trap

**The `big_cores` migration trap.** Removing the Settings row left nothing writing the
pref while `EngineHolder` still read and honoured it. Any user who had ever flipped
that switch was left permanently **1.41× slow**, with no UI to discover it and no way
to turn it off. Found because the device itself arrived in that state. Fixed: the key
is renamed so a stale value can never be honoured again, and the old one is dropped on
first run.

**"Preview max size" was not fixed by the first attempt — the wrong function was
patched.** The diagnosis was right (a per-uri recipe replays a stale value) but
`ParamsState.loadFrom` is not the live path. There were **four**:

```
  Recipes.load -> Presets.decode -> the "display" block   <- the read that actually bit
  Presets.encode "display"                                <- the write
  BuiltInPresets                                          <- a third copy
  ParamsState.loadFrom                                    <- the only one first removed
```

All four are gone now. Dropping the **read** is what disarms the recipes already on
disk — all 33 on the device still carry `previewMaxSize: 640`. The decisive control
was a source with no saved recipe: `decode kind=PHOTO 383x510 maxEdge=1019`, honoured.

### 16.5 Two unexamined knobs on the decode, found by reading the build

> Written when decode read 5093 ms; the corrected figure is **3616 ms** (§16.6). The
> knobs are unaffected — only the size of the prize changes.

Not measured — read from source, and stated as leads rather than findings.

> **Superseded 2026-08-30.** This subsection is retained as investigation
> history, not current build truth. The decoder is now pinned to patched LibRaw
> 0.22.2. Release OpenMP is deliberately off: five debug OpenMP decodes of the
> upstream compressed-Fuji #845 sample produced five different hashes on the
> SM-S948W, while three serial runs were identical. Provenance, deltas, and the
> sanitizer gate are in `docs/dependencies/LIBRAW.md`. The original "pure win"
> hypothesis below was disproved.

**LibRaw is built without OpenMP.** `lib/libraw/src/main/cpp/CMakeLists.txt` mentions
`USE_ZLIB`, `USE_JPEG`, `USE_DNGSDK`, `USE_RAWSPEED` — and OpenMP nowhere. LibRaw
parallelises demosaic and several other loops only when built with it, so on an
8-core phone the decode is very likely running **single-threaded**. This is the safer
lever of the two: LibRaw's OpenMP paths are per-pixel deterministic, so it should be
output-identical, which makes it a pure win if it works. It costs linking libomp on
the NDK.

Stated precisely, because a first pass at this got it half wrong: LibRaw is **not
vendored in-tree**. It arrives at configure time via `FetchContent`, pinned to the
0.21.4 release tarball plus a SHA256, so grepping `lib/libraw/` says nothing about
what pragmas the upstream sources carry. What IS verified is the enablement side —
no `find_package(OpenMP)`, no `-fopenmp`, no OpenMP define anywhere in the build.
**Whatever OpenMP support LibRaw 0.21.4 has, this build does not turn it on.**
Confirming the pragmas exist in the fetched tree is step zero for anyone picking
this up.

**`user_qual` is never set**, so LibRaw's default interpolation applies — AHD, one of
the slower ones. Unlike threading this is **not free**: changing the demosaic changes
the decoded image, therefore the engine's input, therefore the output. It is a
quality/performance trade for the owner to make with pictures in front of him, not a
silent optimisation. Worth measuring what the alternatives cost and look like; not
worth changing quietly.

Neither is parity-gated in the oracle sense — the goldens feed the engine fixed RGB,
so the decoder sits upstream of the gate entirely. That is exactly why it has escaped
attention for this long.

### 16.4 #146's preview offload is a null result, across a 16× range

Six renders per config, fresh process and recipe-free source each time, GPU
self-check PASSED and both GPU paths ACTIVE in every ON config. Note the decode is
**power-of-2 quantized** by both LibRaw and the platform decoder, so preview size is
not continuous — reachable points are 0.195, 0.78, 3.12 and 12.5 MP. 1-2 MP was
bracketed, not hit.

| preview | GPU OFF median | GPU ON median | ratio |
|---|---|---|---|
| 0.78 MP | 691.5 ms | 658.0 ms | 0.95× (4.8% faster) |
| 3.12 MP | 2438.5 ms | 2456.5 ms | 1.007× (0.7% slower) |

Scaling the preview up does **not** reveal a GPU win. At 0.78 MP the 4.8% sits inside
the OFF set's own spread (629-714); at 3.12 MP it is gone. With the 195k px result
from the #146 validation (562 vs 562 ms) that is three sizes across a 16× range with
no usable speedup at any of them.

The GPU scan path is genuinely active and genuinely correct. It is simply too small a
fraction of a frame to matter — grain and halation are filming-stage, on the CPU, and
dominate. **#146's preview-offload question is closed on this device.**

### 16.6 CORRECTION: decode is 3616 ms, and rotation costs 1155 ms

A second run on the same reference RAW, three clean foreground exports, residual
6-11 ms:

| | setup | decode | exif | simulate | grade | encode | residual | total |
|---|---|---|---|---|---|---|---|---|
| median | 4 | **3616** | 3 | 8291 | 678 | 205 | 10 | **12834** |

```
  setup         4 ms    0.03%
  decode     3616 ms   28.2%
  exif          3 ms    0.02%
  simulate   8291 ms   64.6%
  grade       678 ms    5.3%
  encode      205 ms    1.6%
  residual     10 ms    0.08%
```

Spread 12664-13006, 2.7%, and the two unexplained outliers of the earlier run did not
recur.

**Why the earlier 5093 was wrong: the source was rotated.** The decode boundary is
byte-identical between the two builds, so it is not the instrumentation.

```
  rotation NONE   (decode kind=RAW 3060x4080)   3591  3650  3616   median 3616
  rotation 90     (decode kind=RAW 4080x3060)   4725  4817         median 4771
  rotation 180    (decode kind=RAW 3060x4080)   4737  4803         median 4770

  penalty  +1155 ms, 1.32x
```

**180° is the discriminating case**: its dimensions are *not* transposed, yet it costs
exactly what 90° costs. So this is not a transpose cost and not a dimension-swap cost
— it is a flat, angle-independent surcharge on any non-zero rotation. `simulate` is
unchanged across all three (8291 / 8071 / 7937), so nothing else moved.

**The mechanism, read from source after the measurement predicted its shape.**
`MainActivity.loadSource` ends in `based.rotated(rotation)`, and `Rotation.kt`'s
`LinearImage.rotated()` early-outs on `NONE` — then, for every other angle, runs a
**single-threaded Kotlin per-pixel loop over the full-resolution image**, three
`FloatBuffer.get()` and three scattered indexed `put(d, …)` per pixel. At 12.5 MP that
is ~75 M bounds-checked buffer operations on one thread.

Every branch is the same shape, which is exactly why 180° costs what 90° costs — the
measurement predicted the code, and the code confirms it. CW180 in particular is a
pure reversal that never needed a scatter at all.

### 16.9 #159 step 1, measured: it is BOTH, in almost equal parts

The rotation cost was instrumented directly rather than inferred — `rotate ms=1126
angle=180 3060x4080` — which independently confirms the 1155 ms surcharge and the code
reading. Then four variants of the same rotation:

| | reads | writes | time |
|---|---|---|---|
| **A** shipping | per-element | per-element, scattered | 1126 ms |
| **D** | **bulk** | per-element, scattered | 1119, 1172 ms |
| **C** | bulk | per-element, **cache-local** | 597, 625 ms |
| **B** | bulk | **bulk sequential** | **39, 43 ms** |

```
  A -> D   reads made bulk, writes unchanged     no change   => READS COST NOTHING
  A -> C   writes made cache-local               -515 ms     => memory scatter,  46%
  C -> B   writes made bulk                      -570 ms     => per-element op,  51%
```

**The step-1 hypothesis was a false binary.** It asked "traversal or scatter", expecting
one. It is both, in almost equal halves, and it is neither of them on the read side.
A/C/B are all the same angle, so the 515 ms is a clean within-angle comparison; D is a
different angle, but it sits on A's number despite bulk reads, so the "reads are free"
leg does not carry the conclusion alone.

**This retired half of what had just been built.** The first cut of this fix bulk-read
source rows for every angle and kept a per-element scattered write for 90/270 — which
is *exactly variant D*, worth nothing. Only the CW180 path (variant B) was right.

So 90/270 now work a **64×64 tile** at a time. Within a tile each source column's slice
lands on a **contiguous** destination column run, so it bulk-writes; the transpose
itself happens in plain `FloatArray`s, so the inner loop has no bounds-checked buffer
ops at all. That attacks both halves rather than the 46% tiling alone would reach —
the measurement's own conclusion was that tiling by itself lands near 600 ms, not 40.

Measured side effect of the CW180 path: decode fell to 3803/3921 ms against 4737
rotated and 3616 unrotated, i.e. the surcharge is essentially gone on that angle.

### 16.8 What was built for the rotation half

`LinearImage.rotated()` no longer does the obvious per-pixel loop.

- **Source rows are read in bulk** into a plain `FloatArray` — sequential and unchecked
  — instead of three indexed `FloatBuffer.get()` per pixel.
- **180° writes its destination row in bulk too**, with no scatter at all. It is a pure
  reversal and never needed one. Only 90°/270° still scatter, because their destination
  is a column.
- **90°/270° work a 64×64 tile at a time** (see §16.9 for why row-at-a-time could only
  ever scatter), transposing in plain `FloatArray`s and bulk-writing each contiguous
  destination run.
- **Work is split by source row for 180° and by source column for 90°/270°.** In both
  cases the destination *row* is then a function of the split variable alone, so every
  worker owns whole destination rows — no shared element and no false sharing.
- Below 64 000 px it stays single-threaded, so a preview render pays no spawn cost.

Gated by `RotationTest` on the JVM, in CI: the existing exact pixel-mapping tests still
pass, plus **1-worker vs 8-worker byte-identity** (the same contract `kernels/parallel`
holds natively, asserted the same way the parity suite asserts `SPK_NUM_THREADS` 1 ≡ 8),
and equality against a naive reference at 1/3/8/64 workers on a 61×43 image whose
dimensions divide by no worker count.

**The 180° path is measured** (§16.9: 1126 → ~41 ms, and decode drops to ~3.8 s against
4737 rotated). **The tiled 90°/270° path is not yet measured on device** — it is built
from the variant ladder rather than guessed, but that is a prediction until a run says
otherwise.

### What was built for the grade half

The safety problem was that `simResultToBitmap` — the one pass with **no** early-out, so
the whole 678 ms baseline at default sliders — writes through `Bitmap.setPixels`, which a
JVM test cannot gate.

The answer was to split it: the per-pixel hot loop is now a pure
`packToArgb(FloatArray, IntArray, count)`, which a JVM test *can* gate, leaving only the
`setPixels` call in the Android-touching part. The strip is then **bulk-read** in one
`FloatBuffer.get(array)` instead of three bounds-checked `get(i)` per pixel — §16.9 put
per-element buffer ops at ~51% of an equivalent loop's cost, and this loop has no scatter
at all, so that is the whole of the win available.

The arithmetic is character-for-character what the inlined loop did, so output is
unchanged. `PackToArgbTest` asserts the contract rather than restating the formula:
clamping in both directions, round-half-up, channel order, opaque alpha, that a short
final band writes no further than its own pixel count, and that **NaN clamps to 0** —
which matters because the engine's NaN semantics are load-bearing elsewhere.

One memory note: the scratch is now a float strip *plus* the int strip, so the band is
sized by the float budget (~4 MB = 1M floats) rather than the int one. Total managed
scratch stays in the same class as the single 4 MB `IntArray` it replaced, and stays
independent of image megapixels — the OOM that motivated striping is not reintroduced.

`ColorGrade` and `MaskCompositor` are untouched: both early-out at default settings, so
neither is in the measured 678 ms. They are the same shape and can follow if a
measurement ever puts them on the path.

### 16.7 The real shape of the non-engine time

Two costs, same class: single-threaded JVM per-pixel loops, downstream of the decoder,
**upstream of nothing the parity gate covers**, and untouched by any GPU port of the
engine.

| | cost | when |
|---|---|---|
| `LinearImage.rotated` | **1155 ms** | any non-zero rotation — i.e. most phone photos |
| `gradeBufferToBitmap` | **678 ms** | always; §15.6's prediction, confirmed |

That is **~1.8 s of a 12.8 s export** sitting outside the engine and outside the gate.
Neither needs a GPU, a new dependency, or an architecture decision.

**§15.6's prediction is confirmed, and its framing corrected.** `grade` measured 678 ms
across seven runs — not the "few tens of ms" that would have falsified it. But the
falsifier was written as a false dichotomy: it said a small `grade` "would mean the
missing time is in decode or encode instead." Both are true at once. `grade` is
materially non-zero *and* decode is the bulk; 678 ms never could have been the bulk.
The prediction was right; the either/or was not.

**Encode is settled at 205 ms** — 1.6%, a rounding error, not worth touching.

**And the reconciliation-2 sign is now reproducible.** Stage sums exceed `simulate`'s
wall clock by 24-48 ms here and 40-75 ms before: two builds, eight runs, consistently
negative. Nothing near the ~140 MB allocation scale, so the boundary is where we think
it is — but a few stage timers overlap or double-count by ~0.3-0.9%, and the
`stage timings` line should not be quoted as exact.

## 17. Both answers: the transpose holds, and decode is 77% demosaic

> **DEBUG-BUILD NUMBERS.** Same correction as §16: `lib:libraw` compiled at `-O0` in
> debug, so "`decode` 3647 ms, of which `process` 2760 = 77%" is a measurement of the
> compiler as much as of the code. On release `decode` is 546 ms and `process` is
> 325 ms. The *finding* — that decode is demosaic, not I/O — survives; its *size* does
> not. See §19.

### 17.1 #159 — the tiled transpose works, and the surcharge is gone

| angle | `rotate ms` | vs the original 1126 ms |
|---|---|---|
| 180 | **25** | 45× |
| 270 | **37** | 30× |
| 90 | **44** | 26× |

90/270 sit next to 180. **They did not stall near 600**, so there is no residual
per-element half hiding in the transpose — tiling *plus* plain-`FloatArray` inner loops
took both halves, which is what §16.9's decomposition said was needed. The "tiling alone
lands near 600" caveat was correct about tiling alone and simply does not apply.

180 came in at **25 ms, below the hand-rolled 41 ms variant B** — the 8-worker split is
worth roughly another 1.6× on top of the bulk-op win.

**The surcharge is closed.** Decode is now flat across every rotation:

```
  NONE 3565      180 3580      270 3589      90 3665      (ms)
```

against 4737 rotated / 3616 unrotated before. A rotated export now costs within ~100 ms
of an unrotated one. Correctness was spot-checked on device as well as in CI: the 90°
case rendered full-screen with no tile seams and no artefacts at the 64-px boundaries.

### 17.2 #158 — it is NOT I/O. `dcraw_process` is 77% of decode.

Four full-resolution exports, 25 MB DNG (24 999 540 bytes):

| phase | ms | share |
|---|---|---|
| `fileread` | ~120 | 3.4% |
| `unpack` | ~43 | 1.2% |
| **`process`** (`dcraw_process`) | **~2750** | **77.0%** |
| `memimg` | ~96 | 2.7% |
| `copy` (uint16→float subsample) | ~218 | 6.1% |
| `adapt` | ~5 | 0.1% |
| `colour` | ~61 | 1.7% |
| unaccounted | ~287 | 8.0% |

**The caveat resolved in the favourable direction.** §16.5 said that if the time were in
I/O, neither LibRaw knob would touch it. Reading the whole 25 MB file costs **120 ms**,
and the DNG's own decompression (`unpack`) is **43 ms** — both nearly free.

77% is `dcraw_process`, which is **exactly where the pragmas are**: `ahd_demosaic.cpp`
and `postprocessing_aux.cpp` both live inside it, and with `user_qual` unset AHD is what
runs. Both levers in #158 point at the same 2750 ms.

Neither is consequence-free, and the difference matters:

- **OpenMP** does not change a pixel, but adds a runtime dependency and needs a
  thread-count policy that does not fight the engine's own fork-join.
- **`user_qual`** changes the decoded image, so it is a quality decision, not a perf one.

Two smaller items the split surfaced, both the same non-parity-gated class as rotation
and grade: `copy` at 218 ms is a single-threaded scalar uint16→float loop over 12.5 M
pixels, and **287 ms (8%) is unaccounted inside decode** — `open_buffer`, the
`result.rgb` allocation and the JNI marshalling. Flagged rather than assumed inert;
decode is not 100% understood yet.

The instrumentation that produced this is now landed rather than a local patch, so the
split can be re-measured at any time.

### 17.3 Where the export stands, and what is NOT yet measured

12405 ms unrotated:

| | ms | share | |
|---|---|---|---|
| `simulate` | ~8000 | 65% | parity-gated — the engine |
| `decode` | ~3565 | 29% | of which 77% is `dcraw_process` |
| `grade` | ~655 | 5% | single-threaded JVM, not parity-gated |
| `encode` | ~205 | 2% | settled, not worth touching |
| rotation | **~0** | — | was 1155 — **closed** |

**The 655 ms grade figure is from the OLD code.** That run was on `0ee3319`, which
predates the bulk-read and `packToArgb` split (`506991b` / `69e816b`). The grade half of
#159 is built and CI-gated but **has not been measured on device**, and nothing here
should be read as evidence that it worked.

So the live targets are `dcraw_process` at 2750 ms and `grade` at ≤655 ms. Neither needs
a GPU and neither touches a golden.

### 17.4 The grade half, measured: 650 → 159 ms

Three exports on `e82e10c`:

```
  grade  BEFORE (0ee3319)   632  643  655  663    median ~650 ms
  grade  AFTER  (e82e10c)   156  159  164         median  159 ms
                                                  4.1x, -490 ms
```

Correctness was checked on device as well as by `PackToArgbTest` — the render was
visually identical to the pre-fix one, with no channel swap and no banding. A packing
bug would have been loud.

**And the ladder is refined rather than refuted.** §16.9 concluded "reads cost nothing"
from A→D. That was over-general, and this run bounds it. The correct statement:

> When the **write** side is pathological, the read side is masked.

In the rotation transpose the per-element scattered writes cost ~1145 ms, so removing
the reads changed nothing measurable — variant D. In `simResultToBitmap` the writes are
a sequential `IntArray` fill with **no scatter at all**, so nothing masks the reads, and
they turn out to have been ~490 ms of the ~650. Same physics, opposite visibility.

That is why the fix here was read-side only and needed no threading: there was nothing
else in the way.

### 17.6 #158, first lever: OpenMP enabled

> **The sizing below is wrong by ~8.5x** — it is stated against the debug `process` of
> 2760 ms. On release `dcraw_process` is 325 ms, so an ideal 2x is ~160 ms of a 6251 ms
> export, not a lever. The change itself is correct and stays landed. See §19.3.
> For what a *different decoder* would buy — and why rawspeed would not help, since it
> does not demosaic — see `vkdt-decision.md` §10.

Owner's call was OpenMP first, and it is the right order — it is the one of the two
that **changes no pixel**, so it can be verified rather than judged.

`lib/libraw/src/main/cpp/CMakeLists.txt` now compiles LibRaw with `-fopenmp`
(`-static-openmp` on Android, so libomp is linked into the `.so` rather than shipped
beside it), which defines `_OPENMP`, which is what `libraw_types.h` gates
`LIBRAW_USE_OPENMP` on — and that is what all 43 pragmas are behind. They sit in
exactly the files that matter: `ahd_demosaic.cpp` (the default, since `raw_decoder.cpp`
never sets `user_qual`) and `postprocessing_aux.cpp`, both inside `dcraw_process`.

Detection is a real compile-and-link probe with the same flags, so a toolchain without
OpenMP degrades to the previous single-threaded build with a warning rather than failing
the build. `SFRAW_ENABLE_OPENMP=OFF` turns it off.

**Not measured, and the verification is specific.** Two things have to be true, and
neither is assumed:

1. `process` drops from ~2760 ms. If it does not, `_OPENMP` did not reach the LibRaw
   translation units and the pragmas are still inert — check the configure log for
   `sfraw: LibRaw OpenMP ENABLED`.
2. **The decoded output is byte-identical.** LibRaw's OpenMP loops are per-pixel and
   per-row independent, so it should be — but "should be" is what the parity gate exists
   to distrust, and the decoder sits *outside* that gate. Export the same RAW before and
   after and compare the files byte-for-byte.

No thread cap was set. Capping without measuring is exactly what made the "Use
performance cores" setting a 1.41× regression (§16.3), and decode does not overlap the
engine's own pool in time, so there is nothing to contend with.

### 17.5 #159 closed — the tally, and what is left

| | before | after |
|---|---|---|
| rotation | 1155 ms | **~40 ms** |
| grade | 650 ms | **159 ms** |

**~1.6 s off a 12.5 MP export, none of it parity-gated and none of it needing a GPU.**
End to end the export went from ~14.4 s when this work started to **~12.2 s**.

Where it stands now (medians, rotated 90 so decode carries the ~40 ms):

| | ms | share | |
|---|---|---|---|
| `simulate` | 8150 | 66.9% | the engine — parity-gated, the hard one |
| `decode` | 3647 | 29.9% | of which `process` 2760 = 77% of decode |
| `encode` | 216 | 1.8% | settled |
| `grade` | 159 | 1.3% | was 650 |
| `setup`/`exif`/`residual` | 16 | 0.1% | |

The decode phases held steady across all three runs, confirming `e82e10c` disturbed
nothing: `fileread` 119/140/137, `unpack` 42/46/46, `process` 2745/2760/2786, `memimg`
96/94/99, `copy` 223/222/225, `adapt` 4/5/5, `colour` 61/63/63.

**What is left, and it is lopsided:**

- **`simulate` 8150 ms** — the engine. Parity-gated, and the only genuinely hard one.
- **`process` 2760 ms** — `dcraw_process`. The #158 decision: OpenMP (no pixel changes,
  but a runtime dependency and a thread-count policy) or `user_qual` (faster, but it
  moves pixels, so it is a quality call).
- **`copy` 223 ms** — the last cheap one, and worth naming: it is the *same shape* as
  the grade loop that just gave 4.1× — a scalar per-pixel pass over 12.5 M pixels with
  no scatter — except it is already native, so it is not even fighting the JVM. Small,
  but after grade it should not be assumed optimal.
- **`unaccounted` 287 ms** inside decode — `open_buffer`, the `result.rgb` allocation
  and the JNI marshalling.

## 18. The blind spot: every optional effect is unmeasured

Owner's catch, and it is the most consequential one in this whole dossier. Every number
in §16 and §17 was taken at **default settings**. `stage_timings_format` **skips zero
slots**, so a gated-off filter does not appear as `0.0` — it does not appear at all. The
export profile is therefore silent about anything a user might switch on.

Reading the enum against the export line:

| stage slot | gate | default | ever measured? |
|---|---|---|---|
| `camera_diffusion` — **Black Pro-Mist** | `diffusion_filter.active` | **false** | **never** |
| `lens_blur` | `lens_blur_um > 0` | **0** | **never** |
| `glare_field` | `glare_active && glare_percent > 0` | off | **never — and it had no slot at all** |
| `highlight_boost` | `boost_ev > 0` | 0 | printed 0.0 |
| `tc_lut_build` | cold start | warm | printed 0.0 |

**Glare had no timer whatsoever.** Even switched on it cost nothing visible, because
`compute_random_glare_amount` — a stochastic full-resolution field plus a blur — was
never bracketed. That is fixed here: `STG_GLARE` now measures the field build. The
per-pixel add is folded into the scan loops and is not separable from them.

### Why this is the dangerous kind of gap

All three unmeasured effects live in `filming.expose`, on the **float64 irradiance at
full resolution** — the same place, and the same shape, as `halation`. Halation costs
**1905 ms**. There is no reason in the code to expect Pro-Mist or lens blur to be
cheaper, and one reason to expect worse: §12 measured a Gaussian-mixture approximation
of the diffusion PSF at **109×** faster than the exact path, which only pays off if the
exact path is very expensive.

So a user who turns on Pro-Mist may be paying seconds that no profile has ever seen, on
top of a 12.2 s export.

**And it invalidates the GPU targeting, not just the totals.** Picking GPU stages from a
defaults-only profile ranks grain first and never sees diffusion at all. If Pro-Mist is
4 s when enabled, the ranking is wrong.

### A third case, different from the other two

The **print route has its own diffusion filter** (`printing.cpp`, inside
`print_expose`). It is not missing a timer and it is not gated off by the export
profile — it is simply *folded into* `print_expose` with no separate slot. So enabling
Pro-Mist on the print route makes `print_expose` swell with no way to see why.

Three distinct failure modes, then, and they need different fixes:

| | example | symptom |
|---|---|---|
| no slot at all | `glare_field` (until now) | costs nothing visible even when ON |
| slot exists, effect off | `camera_diffusion`, `lens_blur` | absent from the line entirely |
| slot exists, work folded in | print-route diffusion | parent stage swells, cause invisible |

### A second thing the enum reading fixed

`scan_spatial` — and now `glare_field` — are **nested inside** the `scan` bracket: all
three `ScopedStage`s live in the same `scan()` function. So the printed slots **must not
be summed**.

That resolves §16.2's loose end. The apparent "stage timers exceed the native call's
wall clock by 40-75 ms" was an artifact of adding a nested sub-measure twice:

```
  naive sum          8391.5 ms
  minus scan_spatial  412.9 ms   (nested inside scan)
  true stage total   7978.6 ms   vs simulate 8342 ms
  -> 363 ms genuinely outside every stage: JNI entry, param marshalling, result alloc
```

Which is the ~140 MB-allocation-scale gap that was predicted in the first place, and is
a far more sensible picture than timers outrunning the clock. `stage_timer.h` now says
both things at the point of use, so neither reading recurs.

### The measurement this demands, before any GPU work

One export per effect, at full resolution, each enabled alone — **on a release build**,
per §19; a debug ladder would measure `-O0` differences between effects:

1. baseline (all off) — the control, ~6.3 s on release
2. **Black Pro-Mist** on
3. **lens blur** on
4. **glare** on (print route)
5. highlight boost on
6. and the same for anything else the UI can switch on that does not appear above

Only then is there a profile that describes what users actually run — and only then can
GPU targets be chosen from evidence rather than from the subset that happened to be on.

## 19. CORRECTION: every number above was a DEBUG build, and three modules were at -O0

This is the largest correction in this dossier, and it invalidates the *sizing* of most
of §15-§18 while leaving almost all of the *findings* intact. It came from the device
session re-running the same export on a **release** APK.

### 19.1 The two builds, same RAW, same settings

| phase | debug | release | ratio |
|---|---|---|---|
| **total** | 12189 | **6251** | 1.95x |
| `simulate` | 8150 | **5504** | 1.48x |
| `decode` | 3647 | **546** | 6.68x |
| — of which `process` | 2760 | **325** | 8.49x |
| `grade` | 159 | **30** | 5.3x |
| everything else | 232 | **171** | — |

("Everything else" is `encode` + `setup` + `exif` + `residual`, taken as the remainder
`6251 - 5504 - 546 - 30`; the release run reported the three named phases and the total,
not a separate encode figure, so it is not split further here.)

The engine moved 1.48x. Decode moved 6.68x. That spread is not R8 (R8 does not touch
native code) and it is not `-O3` vs `-O2` (that is worth tens of percent, not 6.7x).

### 19.2 The cause, found by reading the four CMakeLists

`engine/spektra-core/src/main/cpp/CMakeLists.txt` has carried this since the engine's
own debug-timing scare:

```cmake
if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif()
```

**The other three native modules did not.** `lib/libraw`, `lib/tiffwriter` and
`lib/pngwriter` all set `CMAKE_CXX_FLAGS_RELEASE` and said nothing about debug — so
CMake's default `CMAKE_CXX_FLAGS_DEBUG` of `-g` applied, which carries **no `-O` flag at
all, i.e. `-O0`**. The engine was the only module that was optimised in a debug APK.
1.48x is the engine going `-O2` -> `-O3 -ffast-math`; 6.68x is decode going `-O0` ->
`-O3`. The uneven ratio column is the diagnosis.

Fixed by copying the engine's guard into all three. Proven by control rather than by
inspection — configuring `lib/tiffwriter` for `Debug` twice, with and without the guard:

```
with:     CXX_FLAGS = -O2 -g -std=gnu++17 -fPIC
without:  CXX_FLAGS = -g -std=gnu++17 -fPIC
```

(Note `CMakeCache.txt` still shows `CMAKE_CXX_FLAGS_DEBUG:STRING=-g` either way — the
guard is a directory-scope `set()` that shadows the cache entry. Read `flags.make`, not
the cache, when checking this.)

### 19.3 What this does to #158, and the honest sizing

The OpenMP work landed in `a184bf5` is still correct: 43 `#pragma omp` in LibRaw were
inert, they are now live, the probe degrades safely. But **its justification in that
commit message and in §17.6 was sized against 2760 ms, a number ~8.5x too large.**

On release, `dcraw_process` is **325 ms**. An ideal 2x from OpenMP on the demosaic is
therefore worth about **160 ms of a 6251 ms export — 2.6%.** That is worth keeping (it
is already landed and costs nothing at runtime) but it is not a lever, and #158 should
not be described as one.

The same deflation applies to the rest of the non-engine work. The rotation transpose
and the grade rewrite were real and are kept — but "~1.6 s off the export" was ~1.6 s
off a *debug* export. On release the whole of `decode + grade + encode` is 776 ms.

### 19.4 What survives, and it is the part that matters

Every *directional* finding holds, because both builds ranked the phases the same way:

- decode is demosaic, not I/O (`process` is 60% of release decode, was 77% of debug)
- the rotation surcharge was traversal + scatter, and the tiled transpose removes it
- `grade` was a JVM per-pixel loop and the bulk rewrite fixed it
- the byte-identity and parity guarantees are untouched — none of this changed a pixel

And the one number that got *more* decisive:

| release | ms | share |
|---|---|---|
| **`simulate`** | **5504** | **88.0%** |
| `decode` | 546 | 8.7% |
| everything else | 171 | 2.7% |
| `grade` | 30 | 0.5% |

**On the build users actually run, the engine is 88% of an export.** Every CPU-side
micro-optimisation left outside the engine is fighting over 12% of 6.3 s. The 1-2 s
target cannot be reached from there under any assumption — even deleting decode, grade
and encode entirely leaves 5.5 s. This is the strongest evidence yet for the GPU
direction, and it was produced by a build-flag bug that had been hiding it.

### 19.5 The method rule this adds

§15.5 already said to record the variables. This adds one that is cheaper and blunter:

> **Never compare a measurement to a target across build types, and never size a lever
> from a debug number.** State the build type on every timing in this document. If a
> phase ratio between two builds is wildly uneven across modules, suspect per-module
> compiler flags before suspecting the code.

Both prior contaminations in this dossier (the rotated source in §16, the debug build
here) were unrecorded *variables*, not wrong *measurements*. The instrument was fine
both times.


## 20. The blind spot, measured: Black Pro-Mist is O(n²) and unusable

§18 said every optional effect was unmeasured and called it the most consequential gap in
the dossier. The owner's instinct — *"jitni bhi effects hai hamne check nahi kie aur vo hi
sabse khatarnak hai"* — was right, and one of them is far worse than anything else here.

`tools/stage_split/` renders a synthetic scene at full resolution and prints
`spk_stage_timings` per case, including the optional effects that no export profile has
ever covered (`stage_timings_format` skips zero slots, so a gated-off filter is invisible
rather than shown as `0.0`). Host, 4-core Xeon @2.8 GHz, `-O2`, 4 workers — the absolute
milliseconds do not transfer to the phone, the **shape** does.

### 20.1 The ladder, print route, 768x768, tame scene

| case | wall ms | what changed |
|---|---|---|
| baseline (grain + halation on) | 1129 | |
| + glare | 995 | `glare_field=30` (nested in `scan`) |
| + lens blur | 1147 | `lens_blur=15` |
| + highlight boost | — | `highlight_boost=2.8` |
| **+ Black Pro-Mist** | **65265** | **`camera_diffusion=64118`** |

Three of the four unmeasured effects are trivial. The fourth is **98.2% of the render**.

### 20.2 It is quadratic in pixel count, and the law is clean

| side | pixels | `camera_diffusion` ms | ratio |
|---|---|---|---|
| 192 | 0.037 MP | 253 | |
| 384 | 0.147 MP | 4108 | **16.2x** for 4x pixels |
| 640 | 0.41 MP | 30653 | |
| 768 | 0.59 MP | 66376 | **16.2x** for 4x pixels |

Sixteen-fold for four-fold pixels, twice, is n². The mechanism is in the source, not
inferred: `apply_diffusion_filter_um` (`model/diffusion.cpp:333`) sets

```
radius = ceil(max(8 * bloom_max_lambda_px, 5)),  bloom_max_lambda_px ∝ 1 / pixel_size_um
```

so the kernel radius grows with image width (physically correct — the PSF is a fixed size
*on the film*), builds a full 2D `ks x ks` PSF with `ks = 2*radius+1`, and then, in its own
words, "convolve each channel **directly** in double precision". Cost is
`n_pixels x ks² x 3`, and `ks ∝ √n`. That is n², exactly as measured.

The radius cap (`min(radius, min(h,w)/2 - 1)`) does not rescue it: the cap is `∝ width` and
the radius is `∝ width`, so whichever is smaller stays smaller at every resolution — the
cap either always binds or never does, and it cannot change the exponent.

### 20.3 What this means for the product, which is the point

**These are the app's own defaults.** `DiffusionState` (`ParamsState.kt:609`) ships
`family = "black_pro_mist"`, `strength = 0.5f`, `spatialScale = 1f`, every size `1f` —
exactly the configuration benchmarked. A user flipping the toggle gets this.

- **30.7 s for ONE 640x640 preview render** — and 640 is `preview_max_size`'s default. The
  interactive slider path, not an export. Even if the phone is 5x this host, that is a
  6 s hang per preview.
- **A 12 MP export extrapolates to ~7.6 hours** on this host (n² from 768: 20.35² = 414x).
  Divide by any plausible device factor and it is still hours. This is an extrapolation,
  flagged as one — but the law held across a 16x pixel range with 4% error, and §20.2
  shows the cap cannot bend it.

**Nobody has ever run this.** Every profile in §16-§19 was taken at defaults, and
`camera_diffusion` defaults off, and zero slots are skipped — so the stage has been
invisible in every measurement this project has taken.

### 20.3b FIXED: FFT convolution, then a real-to-complex transform

The repair landed in two steps, both computing the **same sum** as the direct loop rather
than approximating it (`551c57f`, then the r2c pass).

**Step 1 — FFT.** `kernels/fft_convolve` replaces the O(w·h·ks²) loop with an
overlap-save transform. The derivation is in the file header; the part that matters is
that substituting `p = ks-1-i` turns the flipped-kernel correlation into a linear
convolution read at offset `ks-1`, which is why the kernel sits at the **origin** of the
transform and not at its centre — centring would translate the image by `radius` and look
plausible while being wrong.

**Step 2 — real-to-complex.** Both the tile and the kernel are real, so their spectra are
Hermitian and only `n/2 + 1` of the `n` columns are independent. Keeping just those halves
the scratch *and* the work. That matters more than the 2× suggests: scratch is what caps
the transform size, and transform size is what makes a large kernel cheap.

| side | direct | FFT (c2c) | FFT (r2c) |
|---|---|---|---|
| 640 | 30653 | 429 | **175** |
| 768 | 66376 | 2619 | **828** |
| 1024 | — | 3005 | **1129** |
| 1536 | — | 8185 | **4270** |

Best-of-3 on the same run, which is the honest same-methodology comparison (this host is
shared and single-rep times swing; the *ratio* is stable across both methodologies):

> **640px preview: 17663 ms → 194.7 ms. 90.7×.**

`max_abs` against the direct loop is ~1e-13 on values of order 300 — about 1e-15 relative,
eleven orders inside the 1e-4 bar — and both paths stay byte-identical across worker
counts. Gated by `tests/test_fft_convolve.cpp` (single transform, genuine overlap-save
tiling with partial edge tiles, a kernel wider than the image, 1-vs-8 equality).

**Step 3 — choose the transform size by cost, not by size.** The cap was doing two jobs
and getting one of them wrong. `fft_convolve_transform_size` always returned the LARGEST
admissible transform, so the cap was also the choice, and both a low and a high cap were
wrong for some image. Measured on the operator alone (8 workers, `-O2`, best of two):

| case | N=1024 | N=2048 | N=4096 | N=8192 |
|---|---|---|---|---|
| 640 preview, `ks=273` | **28 ms** | (same N) | | |
| 1536, `ks=651` | **373 ms** (25 tiles) | 386 (4) | 767 (1) | 739 (1) |
| 4080×3060, `ks=1725` | — | 9909 (130) | **1890 (4)** | 3094 (1) |

A bigger transform buys a bigger usable block `B = N - ks + 1` and so fewer tiles, but the
per-element cost roughly *doubles* per doubling of `N` — far faster than `log2 N` grows —
because the column pass is memory-bound. So the selector now minimises
`tiles × N^2.8`, an empirical model that ranks all six measurements above correctly where
`N² log N` ranks two of them backwards. It is a heuristic over a handful of candidates, so
being wrong costs speed and never correctness: every candidate computes the same operator,
and the choice stays a pure function of `(w, h, ks, cap)`, which is what keeps the output
byte-identical across worker counts.

With the choice separated from the ceiling, the ceiling could rise to 4096 (r2c had already
halved the scratch to 402.8 MB):

> **12 MP Black Pro-Mist: 9909 ms → 1890 ms. 5.2×.** And 1536 px, which the old code ran at
> N=4096 whenever the cap allowed it, drops 767 ms → 373 ms, **2.1×**.

8192 is *not* the next step: one tile, but measured slower than 4096 and 1.5 GB of scratch.

Because 402.8 MB is real money on a phone mid-export, `model/diffusion.cpp` clamps the
ceiling to half of the process memory budget's remaining headroom, so a large transform can
never turn a completing export into a controlled OOM — it just picks a smaller `N` and runs
slower. The scratch itself is not reserved through the budget, so two concurrent large
diffusion renders would each see the same headroom; exports serialize through one runtime
and a preview never reaches these sizes, so that is a documented limitation, not an observed
one. The choices are pinned in `tests/test_fft_convolve.cpp` so a future model change has to
re-measure rather than re-guess. `SPK_DIFFUSION_FFT=0/1` and `SPK_DIFFUSION_FFT_MAX`
(`debug.spektra.fft`, `debug.spektra.fftmax`) still expose both knobs for on-device A/B.

**What is left.** Packing two of the three channels into one complex transform (three
channel passes → two) is the next cheap win. None of the numbers above is a device
measurement: they are host times whose *ratios* transfer, and the on-device A/B is still
owed.

### 20.4 It is a faithful port, which makes it a parity decision

`model/diffusion.cpp`'s header says it mirrors `spektrafilm/model/diffusion.py`'s
`apply_diffusion_filter_um` and its PSF helpers. So the cost is almost certainly inherited
from the oracle's algorithm rather than introduced here. *(Stated from our own source
comments — the oracle is not checked out in this container, so this specific claim is
unverified against upstream.)*

That matters because it makes the fix a **parity** question, not a bug fix. `diffusion`
and `diffusion_e2e` are two of the 38 gates. Any of the obvious repairs changes pixels:

- **FFT convolution** — mathematically the same operator, O(n log n), but not bit-exact.
- **A sum of separable/IIR exponential filters** — the PSF is a weighted sum of
  `exp(-r/λ)/(2πλ²)` terms, and `kernels/exponential_filter.h` (which already makes
  halation O(n) per pass, independent of sigma) is the machinery for it. Radially
  symmetric `exp(-r/λ)` is not separable, so this is an approximation.
- **Cap the radius in absolute pixels** — cheapest, changes the look at high resolution,
  and breaks the "fixed physical size on film" property that makes the effect correct.

All three are the owner's call, not an engineering one — the same class as the 44-band
question in `vkdt-decision.md` §9.

### 20.5 Two things this corrects about our own GPU reasoning

1. **"grain is 42% of the engine" is scene-specific, not a constant.** The same bench on a
   wide-range scene (8 stops, `lum=8.0` speculars) versus a tame one (2 stops, no
   speculars), same resolution:

   | scene | grain @384 | grain @768 | scales with pixels? |
   |---|---|---|---|
   | wide | 8053 ms | 9253 ms | **no** — 1.15x for 4x pixels |
   | tame | 82 ms | 282 ms | yes — 3.4x |

   ~100x spread at fixed resolution. The mechanism is documented at `grain.cpp:82`:
   `fast_binomial_one` "falls into an O(n) CDF-inversion walk where density approaches its
   maximum, so a bright sky can cost hundreds of times more per pixel than a shadow" — the
   dynamic block scheduling exists precisely to balance it. What was never quantified is
   the magnitude, and it is enormous. **Export time is not bounded by resolution**; a
   high-contrast frame can cost orders of magnitude more than a flat one.

2. **That weakens the GPU-grain case in `vkdt-decision.md` §5.** Massively divergent
   per-lane work is the worst shape for a GPU: every lane in a subgroup waits for the
   slowest. The §5 experiment is still the right one to run, but "grain is the biggest
   stage so move it first" was reasoning from a share that turns out to be a property of
   one photograph.

### 20.6 The method note

This bench was nearly reported wrong. The first run used the wide scene and said "grain is
91% of the engine" — a number that would have gone straight into a decision doc and
redirected the GPU work. It was caught by a scaling check that took two minutes: **4x the
pixels gave 1.15x the time**, which no per-pixel stage can do. The rule this adds:

> Before believing any stage share, check that the stage scales with the thing you think
> drives it. A stage that does not scale with pixel count is not measuring what you think.

## 21. The Halide fusion spike, and a quantisation cliff worth more than the result

Owner raised the Lightroom precedent: Adobe uses Halide, and **pipeline fusion** is one of
the reasons — instead of writing a full-resolution temporary after every adjustment, Halide
computes several stages together on small tiles. Our engine does write those temporaries.
`tools/halide_fusion/` asks what that would buy us. Synthetic pipeline shaped like filming's
O(n) run, not our real stages; best of 5, shared host.

### 21.1 Fusion is spectacular on the wrong shape

| chain | 1024² | 2048² |
|---|---|---|
| elementwise only | **18.4×** | **35.7×** |
| **with a separable blur — our actual shape** | **0.78× (slower)** | **1.51×** |

**That gap is the answer.** Fusion pays when nothing needs its neighbours, because no
full-res buffer is ever written. Put a stencil in the middle and it has to recompute the
producer for every consumer tile, and the win collapses — at 1024 it went *negative*.

Our chain is the second row. Halation, DIR diffusion and scanner unsharp are all stencils,
and the largest stage (grain) is neither elementwise nor a stencil but a divergent
per-pixel sampler. Lightroom's sliders — exposure, contrast, curves — are mostly the first
row, which is why the precedent is real for Adobe and much weaker for us.

### 21.2 The determinism contract holds

Fused, parallel, vectorised, blur in the middle: **byte-identical across `HL_NUM_THREADS`
1 vs 8**, at both sizes. Same answer the NumHalide FFT gave. Halide's schedules fix their
reassociation at compile time, so worker count does not enter it. The objection this
project has repeatedly raised against Halide is, on this evidence, not the real obstacle.

### 21.3 The finding that matters more than either — a LUT-index cliff

The materialised and fused schedules disagreed by **3.648e-03** at 2048² while agreeing
exactly at 1024². Both cannot be right, and a plain-C++ reference settled it: the
**materialised** schedule matched to 5.96e-08 and the **fused** one was the outlier.

Counting the differing samples explains it and clears Halide:

| | |
|---|---|
| differing samples | **682 of 12,582,912 — 0.0054%** |
| mean delta on those | **3.648e-03** |
| one LUT step at that index | **3.980e-03** |

A vanishing fraction of pixels, each off by almost exactly **one table entry**. It is a
**quantisation cliff**: the schedules differ by ~1 ULP in the index expression (FMA
contraction differs between the vectorised inline form and the materialised one), and
`cast<int>` on a value sitting a hair from an integer boundary lands on the neighbouring
entry. A 1-ULP input difference becomes a **60,000×-larger output difference**. Not a
Halide bug, and not a scheduling bug — an unguarded nearest-index lookup.

**We are already immune, by design, and that is now a property to protect.** Every LUT in
the engine interpolates rather than rounding — `kernels/interp.cpp` returns
`fp[low] + t * (fp[low+1] - fp[low])`, and `kernels/lut3d.cpp` takes `floor` and
interpolates from there. With interpolation a 1-ULP index shift moves the output by 1 ULP.
With `cast<int>` it moves by a whole step.

**This is a live hazard for the GLSL route, not a curiosity.** A GPU port differs from the
CPU path by ~1 ULP *everywhere* by construction — fp32 against f64 (`vkdt-decision.md`
§11.4). Any nearest-index LUT fetch introduced in a shader (a `texelFetch` on a rounded
index rather than an interpolating `texture()` sample) would therefore scatter
single-step errors across the frame: rare, bright, and impossible to attribute later. The
rule to carry into the shaders: **interpolate every LUT; never round an index.**

### 21.4 Verdict

Fusion is **not** the lever for our pipeline — the shape is wrong, and the honest number
for our chain is between 0.78× and 1.51×, not the 18–36× a naive spike reports. The
determinism objection is retired. The spike's real yield is §21.3, which is a constraint on
how the shaders get written.

## 22. The effects ladder, measured on device (2026-08-29)

Six full-resolution exports, 4080x3060 = 12.48 MP, print route, JPEG q100, GPU export on,
on the fixed post-R8 release build. Each middle row is baseline **plus one** effect; the
last is all four together. Param state held constant across every row and pinned as the
run's header: the shipped built-in preset **"Portra 160 - Soft Light Portrait"**, not
`PARAM_DEFAULTS` — a named built-in is as reproducible as the defaults, which was the
actual requirement.

| row | total ms | delta vs base | the stage itself |
|---|---:|---:|---|
| L1 baseline | 10013 | - | - |
| L2 + Pro-Mist | 19673 | **+9660** | `camera_diffusion` 9877.3 |
| L3 + lens blur | 9774 | -239 | `lens_blur` 177.3 (4.60 um) |
| L4 + glare | 9929 | -84 | `glare_field` 190.9 (0.03%) |
| L5 + highlight boost | 9932 | -81 | `highlight_boost` 37.3 (2.0 EV) |
| L6 ALL ON | 20109 | +10096 | all four |

Every row was range-checked (no `min == max` in any arm) — the discipline that caught the
degenerate-frame GPU measurement in §2. `fft_fallbacks=0` on both diffusion rows, so those
`camera_diffusion` numbers are genuine N=2048 transforms and not a silent direct-loop
fallback. That was the counter's first real use; note it has only ever printed 0, so it is
confirmed **wired**, not yet confirmed **sensitive**.

### 22.1 The totals are noise-bound; read the stage timings instead

Sum of the four total-deltas is 9256 against an ALL-ON delta of 10096 — a gap of +840 that
looks like superadditivity. **It is not.** Rows L3, L4 and L5 each did strictly MORE work
than baseline and every one came in BELOW it (-239, -84, -81). That is physically
impossible, so the floor is measurement noise, and the inflated sample is the baseline:
L1 was the first export of the session and ran cold, while every later comparable row
landed in 9774-9932. **Noise floor is roughly +/-250 ms on a 10 s export, about 2.5%.**

So any effect under about half a second cannot be read from the total at all. At stage
level the four are additive to **1.3%**:

    isolated:  9877.3 + 177.3 + 190.9 + 37.3 = 10282.8
    in ALL ON: 9719.5 + 190.5 + 197.8 + 41.1 = 10148.9

Glare is the one effect whose own slot understates it: `glare_field` is nested inside
`scan`, and `scan` moved 428.7 (baseline) -> 648.3 (+glare) -> 676.6 (all on). Glare
really costs about 220 ms, of which the field build is 191.

### 22.2 There are exactly two effects worth optimising

Pro-Mist is 9877 of the 10283 ms of total ladder-effect cost — **96%**. The other three
together are 405 ms, which is inside twice the noise floor.

But the 10 s baseline is not free, and none of it is a ladder effect: grain 4340-4540,
halation 1915-2124, dir_couplers 1243-1305, so about **7.9 s of the 10 s baseline** is
those three. The honest summary of an all-effects export is roughly **half Pro-Mist and
half grain+halation+couplers**, with the three small effects as rounding error.

If effect cost is the target there are two candidates, `camera_diffusion` and `grain`, and
nothing else on this list is worth engineering time.

### 22.2b The rows were validated against #163, and they pass

#163 is the finding that the native stage timer is a process-global reset per top-level
render, with no serialization behind it — so a preview/ROI/draft render overlapping an
export can zero the export's accumulator mid-flight. That would corrupt a ladder row
silently, and the corrupted row would still look plausible beside its neighbours.

Protocol used (before the warning was even circulated): `logcat -c` -> tap Export -> wait
-> read, with no slider, pan, zoom, magnifier or overlay touched between the tap and the
timings line, and all param changes >= 3 s before the export sheet opened. `ExportMask`
also swallows pointer input for the duration, so nothing could reach the UI mid-export.

Then the arithmetic check, sum of top-level stages against that row's `simulate`
(`scan_spatial` and `glare_field` excluded — they nest inside `scan`):

| row | stages / simulate | | unaccounted |
|---|---:|---:|---:|
| L1 baseline | 8909.7 / 9235 | 96.48% | 325.3 ms |
| L2 +Pro-Mist | 18634.7 / 18983 | 98.17% | 348.3 ms |
| L3 +lens blur | 8838.0 / 9128 | 96.82% | 290.0 ms |
| L4 +glare | 8846.2 / 9149 | 96.69% | 302.8 ms |
| L5 +boost | 8924.5 / 9230 | 96.69% | 305.5 ms |
| L6 ALL ON | 18979.0 / 19313 | 98.27% | 334.0 ms |

**The unaccounted term holds a 290-348 ms band across all six rows, including both 19 s
ones.** That is the signature of fixed untimed overhead, not corruption: a mid-flight
reset drops a whole stage and blows one row's gap wide open, and the band would not stay
flat while `simulate` more than doubles.

It also matches an independently-derived figure. `stage_timer.h:97` records "~363 ms of
JNI entry, param marshalling and result allocation that genuinely is outside every stage",
reached from a different reconciliation entirely. Two measurements, taken months and
methods apart, landing on the same ~300 ms of boundary cost is the strongest evidence
available that these rows are clean.

Nothing to re-run. The check is worth keeping as the standard ladder gate: **stages should
account for ~96-98% of `simulate`, and the remainder should be flat across rows.** A row
that breaks either half of that is suspect regardless of how reasonable its total looks.

### 22.3 CORRECTION: the 39 s Pro-Mist figure was a SETTING, not a resolution property

Section 20 and PR #156 quoted `camera_diffusion` at **39127 ms**, and derived from it that
Pro-Mist was **85% of a 12 MP export**. This ladder measures the same filter, on the same
image, at the same 12.48 MP, at **9877 ms** and **48%**.

The difference is the Pro-Mist strength in this preset versus whatever was set on the day
of the #160 run. So:

- **39127 ms was never a property of 12 MP.** It was a property of one filter setting.
- The "85% of the export" framing belongs to that setting, not to Black Pro-Mist generally.
- The **90.7x FFT speedup is unaffected** — that was a before/after on one fixed
  configuration, which is exactly the comparison a speedup claim needs.

Recorded rather than quietly restated, because it is the fourth time on this branch that a
number turned out to describe a condition nobody had written down: a debug build (S19), a
degenerate frame (S2), a file size (S1), and now a slider position.

## 23. The DIR-coupler stage is 44% allocation, 0.4% arithmetic

Starting point: `grain + halation + dir_couplers` is ~79% of a 12.5 MP export (§2 of
[#156](https://github.com/thetechgeekko/Spektrafilm-android/pull/156)), and the plan was to
take them to the GPU in ascending order of risk — couplers first, being pure per-pixel work
with no RNG.

That plan died on contact with a measurement, and the measurement is the useful part.

### 23.1 The GPU kernel was built, engaged nothing, and was removed

`gpu/dir_couplers.comp` plus its Vulkan kernel, a one-time self-check, partial-progress
reporting and the `spk_gpu_couplers_*` counters were written and worked: `test_gpu_host`
went green under lavapipe with the offload wired to the existing `allow_gpu_scan` latch.

**It never ran.** The engagement assertion — added on the same principle as the print-expose
one, that a gate which cannot distinguish "passed" from "never ran" is not a gate — reported
`state=0 pixels=0`. The cause: `run_scan_film` calls `digest_filming_params(..., spatial_effects
= true)`, which sets `dir_couplers.diffusion_size_um = 20.0`, so production takes the
**spatial** variant. The pointwise fused loop the shader replaced is only reached when a user
zeroes `dir_diffusion_size_um`.

Every `max_abs` `test_gpu_host` printed would have passed unchanged on a silent CPU fallback.
Without the engagement check this would have shipped as a "validated GPU offload" that no
render ever entered.

### 23.2 Where the stage actually spends its time

> **CORRECTED in §24.5.** The table below is a COLD-start measurement — the bench never
> faulted its pages in before timing, so every allocation row also carries the first-touch
> cost. It is a real condition (it is what the first render after a launch pays) but it is
> not steady state, and the shares below over-attribute allocation by roughly 4.6x against a
> warmed run. The conclusion it drove — that the per-pixel loops are not worth offloading —
> survives unchanged and is re-grounded in §24.5. Left in place rather than edited, because
> the cold/warm split is the point.


Phase breakdown of `apply_density_correction_dir_couplers_spatial`, host, 12.5 MP
(4096×3052), release flags:

| phase | ms | share |
|---|---|---|
| alloc `correction` (300 MB) | 1246.2 | 14.8% |
| **loop 1** — silver → correction | **12.9** | **0.2%** |
| copy `gauss(correction)` (300 MB) | 1772.1 | 21.0% |
| gaussian filter | 1538.4 | 18.2% |
| alloc `tail` (300 MB) | 685.7 | 8.1% |
| exponential filter | 3171.9 | 37.5% |
| **blend** | **21.1** | **0.2%** |

- **Allocation and one copy: 43.8%.**
- **The two filters: 55.8%.**
- **The per-pixel loops — the entire GPU target: 0.4%.**

A perfect GPU offload of both loops would have removed 34 ms of an 8448 ms stage. Even on the
pointwise path, where those loops *are* the whole stage, it is ~1.3% of an export. It does not
meet this branch's bar, so it was removed, exactly as the Highway f64 halation tier was.

The 1246 ms "allocation" is page faults, not the `memset`: loop 1 writes the same 300 MB in
12.9 ms (23 GB/s) once the pages are resident. Zero-fill is a rounding error; **first touch is
the cost.**

### 23.3 What shipped instead: one reordering, no copy

The two filters are independent — the exponential tail reads `correction` and writes `tail`,
the Gaussian blurs `correction` in place. The old code blurred a full-resolution **copy**
purely to keep `correction` alive for the call that followed it. Run the tail **first** and
the copy is unnecessary:

```
old:  gauss = copy(correction); blur(gauss); exp(correction -> tail); blend(gauss, tail)
new:  exp(correction -> tail);  blur(correction in place);            blend(correction, tail)
```

**Byte-identical, proved directly rather than inferred:** an A/B running both orders in one
process `memcmp`'d the f64 results over ten filter configurations (FIR-only, IIR-only, the
`SMALL_SIGMA_MAX = 3` dispatch boundary, tail-weight 0 and 1) across two image shapes
including a non-power-of-two, plus the 12.5 MP frame. All identical. `test_spatial` gates it
in CI — it digests with `spatial_effects = true`, so its golden runs at
`diffusion_size_um = 20.0`, the production value.

**Timing, and a controlled one.** A single old-then-new pass reported 59.3%, which is wrong:
the first arm page-faults the heap in and the second inherits warm pages. Alternating the
order exposes it:

| rep | first arm | old | new | saved |
|---|---|---|---|---|
| 0 | old | 6754.8 ms | 3351.1 ms | 50.4% |
| **1** | **new** | **5894.3 ms** | **4605.1 ms** | **21.9%** |
| 2 | old | 4550.8 ms | 3305.6 ms | 27.4% |

**Take ~22%** — rep 1 is the conservative arm (new cold, old warm), and it matches the
independent phase estimate of 21% from §23.2. Absolute times drift ~30% across reps as the
allocator warms, which is why the reps alternate rather than average. That is the fifth time
on this branch a number described an unwritten condition; this one was caught before it was
published rather than after.

The memory half needs no such hedging: **three full-resolution f64 planes become two,
900 MB → 600 MB at 12.5 MP.** On a phone that matters more than the milliseconds.

### 23.4 What this redirects

1. **The remaining 23% of the stage is still allocation** (`correction` 1246 ms + `tail`
   686 ms of page faults). It cannot be reordered away — it needs the buffers to survive
   across renders, i.e. an engine-level f64 scratch pool. `exponential_filter_per_channel_d`
   allocates its own 300 MB `comp` internally on every call, so the pool would serve it too.
2. **Halation does NOT have the same win.** Checked, not assumed: `apply_halation_um`'s blend
   is `raw = (1-s)*raw + s*scattered`, which consumes the *original* `raw`, so its `core`
   copy is genuinely required. Its redundant `tail` zero-fill is real but small.
3. **The GPU case for this stage is weaker than the 19% headline suggested.** 19% of an
   export is the *stage*; the part a per-pixel shader can touch is 0.4% of it. Any serious
   GPU work here has to target the separable f64 filters — which are the *same two functions*
   halation uses, making `gaussian_blur_per_channel_d` + `exponential_filter_per_channel_d`
   a single target worth roughly 47% of an export rather than two separate ones.

## 24. The separable f64 filter was 42% data movement

§23.4 named the real target: `gaussian_blur_per_channel_d` and
`exponential_filter_per_channel_d`, the two separable f64 filters that **halation and the
DIR couplers both call** (`model/diffusion.cpp:92` and `model/couplers.cpp:432`). Measuring
before writing anything — the explicit lesson of §23 — found the cost was not the blur.

### 24.1 A correction first

An initial decomposition reported the exponential filter at **2799 ms** with a **65%**
zero/copy/axpy share. **Both numbers are wrong.** They came from a bench whose process held
1.1 GB live across several unrelated arms; a direct A/B measured the same old code at
1270 ms. A decomposition whose total disagrees 2.2× with a direct measurement of the same
function is not publishable, so it was re-run with internal timers whose parts sum to the
function's own total, in a minimal process. The reconciled figures are below and supersede
the first set.

### 24.2 Where the time was (host, 12.5 MP, release flags, decay 22.76)

| part of `exponential_filter_per_channel_d` | ms | share |
|---|---|---|
| zero `out` | 20.1 | 1.6% |
| 3× copy `img` → `comp` | 58.2 | 4.5% |
| **3× `gaussian_blur_per_channel_d`** | **958.5** | **74.1%** |
| 3× axpy `out += a*comp` | 52.2 | 4.0% |
| residual — allocating the 300 MB `comp` | 204.2 | 15.8% |
| **total (measured directly)** | **1293.2** | |

That total agrees with the independent A/B's 1270.4 ms, which is the check the first attempt
failed.

The cost sits *inside* the blur calls — and inside those, most of it is not blurring:

| σ | 3× `gaussian_blur_plane_d` | `gaussian_blur_per_channel_d` | de/re-interleave |
|---|---|---|---|
| 34.68 | 121.5 ms | 279.1 ms | **157.5 ms (56.5%)** |

`gaussian_blur_per_channel_d` deinterleaves one channel into a plane, blurs it, and
reinterleaves — on **every call**. The old exponential filter called it once per mixture
component, so three components × three channels was **nine strided gathers and nine strided
scatters**, at stride 3 doubles (24 B), where a 64 B cache line yields under three useful
values. Roughly **42% of the whole filter was moving data**, not blurring it, and that share
is flat across σ because the IIR blur itself is O(1) per pixel.

### 24.3 The change: stay planar for the whole mixture

Deinterleave once per channel, run all three components in planar space, reinterleave once —
three gathers and three scatters instead of nine and nine, and every remaining pass
contiguous.

```
old:  for k in 0..2 { copy interleaved img; per_channel_blur (deinterleave/reinterleave x3); axpy }
new:  for c in 0..2 { deinterleave once; for k in 0..2 { plane copy; blur_plane; axpy }; reinterleave once }
```

**1293.2 ms → 824.8 ms, 36.2% faster**, corroborated by a separate A/B at 34.3% median
(34.3% / 44.8% / 26.9% across alternated reps — note the *new-first* rep, where the new arm
runs cold, is the best of the three, so this win is not the page-fault artifact that inflated
§23.3's first reading). Scratch drops from four planes to three: the old path held a
3-plane `comp` **plus** the 1-plane deinterleave buffer inside every
`gaussian_blur_per_channel_d` call; the new one holds `src` + `comp` + `acc`.

**Byte-identical**, and proved directly: 16 configurations — 4 shapes (including 1-channel,
4-channel, and a non-power-of-two) × 4 decay regimes (FIR-only at 0.4, the
`SMALL_SIGMA_MAX = 3` boundary, production 22.76, wide 90), each with **unequal per-channel
decays** so that a bug collapsing the per-channel σ vector would fail rather than pass.
`memcmp` on the f64 results, all identical. Each component is still
`gaussian_blur_plane_d` over exactly `img[:, c]` at `ratio_k * decay[c]`, and the
accumulation still runs k ascending with the same operands in the same order; only the order
memory is visited changed.

Parity 39/39 ALL OK on both legs. `test_parallel` scenarios 3–4 cover it for
thread-invariance (scan and print routes with `halation_active = 1`, 1 vs 8 workers,
byte-identical required), which matters because the parallel structure changed.

### 24.4 What is left in this filter

At 824.8 ms the remaining shape is ~417 ms of actual plane blurs plus the gathers, scatters,
plane copies and axpy around them. The blurs are now the largest single part, which is the
right place to be. Two further levers, neither yet measured: the last mixture component could
blur `src` in place instead of copying it (saving three of nine plane copies), and the three
plane buffers are still allocated per call, so the scratch-pool idea from §23.4 applies here
too.

### 24.5 Correcting §23.2: allocation is a COLD cost, and cross-bench absolutes do not compare

Re-measuring the coupler block after both changes landed, in a minimal process that warms
every buffer first and whose parts reconcile against a directly measured total (residual
13.1 ms on 3119.9 ms — 0.4%, the check §23.2's bench never had):

| phase, current block | ms | share |
|---|---|---|
| alloc `correction` (300 MB) | 192.2 | 6.2% |
| loop 1 — silver → correction | 12.8 | 0.4% |
| alloc `tail` (300 MB) | 226.5 | 7.3% |
| **`exponential_filter_per_channel_d`** | **2202.2** | **70.9%** |
| `gaussian_blur_per_channel_d` | 442.7 | 14.2% |
| blend | 15.4 | 0.5% |
| loop 2 | 14.9 | 0.5% |
| **allocation 13.5% · filters 85.1% · per-pixel loops 1.4%** | | |

Against §23.2's cold table (43.8% / 55.8% / 0.4%), two things changed and only one of them is
my code:

- The `gauss` copy is gone — that is §23.3, as intended.
- **Allocation fell from 1932 ms to 419 ms, and nothing in the diff touches those two
  allocations.** That drop is the bench, not the code: §23.2 timed fresh anonymous pages on
  first touch, this run warms them first. Both are real; they describe different conditions.
  **Allocation cost here is a cold-start cost, not a steady-state one** — which reframes the
  scratch-pool idea from §23.4 as a fix for the first render (#152's ~8.8 s cold versus
  ~562 ms steady) rather than for repeated exports.

**The method rule this establishes, and it invalidates several numbers on this branch:**
absolute timings from different bench processes in this container **do not compare**. The
same planar exponential filter measured 824.8 ms in §24.2's dedicated bench and 2202.2 ms
here; the same coupler replication measured 3119.9 ms while the real function measured
1652.8 ms *in the same process*, because the replication ran first and handed the real call
its warmed pages. Only two kinds of number survive: a **within-process A/B with alternated
arms**, and a **decomposition whose parts sum to a directly measured total**. Every claim in
§23.3 and §24.3 is of one of those kinds, which is why they stand.

What this does NOT change: the per-pixel loops are 1.4% here and 0.4% there — trivial under
every measurement — so §23.1's verdict on the GPU kernel holds. And the filter is now **71%
of the coupler block**, so §24 landed its 36% on the largest part.

## 25. Grain was one degenerate loop: 40.8 billion iterations that computed nothing

§22 measured grain at **4340-4540 ms of a ~10 s device export** — the largest single item
in the baseline, and the one thing on the perf list that had never been looked at. Following
§23's lesson, it was decomposed before anything was written.

### 25.1 The whole stage is one phase

Phase timers inside `apply_grain_to_density_layers` (the production path: `sublayers_active`
is the schema default and portra carries `density_curves_layers`), host, 12.58 MP,
4 threads, shipping flags, parts summing to a directly measured total:

| phase | ms | share |
|---|---|---|
| **RNG sampling (9x `layer_particle_model`)** | **29491.1** | **94.6%** |
| final blur | 747.3 | 2.4% |
| dye-cloud blur (9x) | 285.1 | 0.9% |
| shifted fill (9x) | 166.8 | 0.5% |
| accumulate (9x) | 134.3 | 0.4% |
| `-= density_min`, acc->out | 45.6 | 0.2% |
| `add_micro_structure` (3 phases) | **0.0** | **0.0%** |
| sum of phases | 30870.2 | 99.0% |
| **measured total** | **31179.1** | |

Two things fall out immediately. Every structural idea worth having — parallelising the
serial loops, hoisting the loop-invariant `log`/`sqrt` out of the lognormal sampler — targets
the 5.4% that is not the sampler. And `add_micro_structure` costs **exactly zero**, because
it early-returns: its `sigma = micro_structure[1] * 0.001 / pixel_size_um` needs
`pixel_size_um < 0.6 um` to clear the 0.05 threshold, i.e. a 35 mm frame sampled wider than
58,000 px. It is inert at every real resolution including a 100 MP export — correctly so,
since sub-pixel clumping averages out — but it means the hoist spotted there before measuring
would have optimised code that never runs.

### 25.2 Census: where the sampler's time actually goes

Counting branch regimes (single-threaded, same 12.58 MP geometry):

| binomial branch | calls | share |
|---|---|---|
| normal approximation | 74,388,548 | 65.69% |
| **CDF inversion** | **38,840,540** | **34.30%** |
| Bernoulli (n < 25) | 17,120 | 0.02% |

Poisson is 99.99% normal approximation and never matters. The CDF-inversion branch does,
and not because of its call count:

- **21,543,383** of those calls (55.47%) have `pow(1-p, n)` **underflowed to exactly 0**;
- they account for **40,767,030,234 loop iterations — 99.62% of every iteration the branch
  runs**, averaging **1892 per call**;
- the remaining 0.38% averages **8.9** iterations per call.

### 25.3 Why: a walk whose answer underflow has already decided

`fast_binomial_one` reaches CDF inversion when `var = n*p*(1-p) <= 10` with `n >= 25`, which
pins `p` to one end of [0,1]. At the `p -> 1` end — a pixel at maximum density, where
`saturation = 1 - p*uniformity` collapses toward 0.03 and so the Poisson rate
`n_ppp/saturation` explodes — `pow(1-p, n)` underflows to exactly 0. Then:

```cpp
double prob = std::pow(1.0 - p, n);   // == 0.0
while (cdf < u && k <= n) {
    cdf += prob;                       // stays 0.0, forever
    if (k < n) prob = prob * ... ;     // 0 * finite == 0
    k += 1;
}
return k - 1;                          // therefore always n, after n+1 iterations
```

The accumulator can never advance, so the loop is *guaranteed* to run to `k = n+1` and return
`n`. Up to 2629 iterations per pixel at export resolution (41,643 at coarser pixel sizes),
computing a value underflow settled before the first one. The upstream Numba loop has the
same structure and the same underflow, so this was a faithful port of a degenerate walk.

### 25.4 The change

```cpp
if (prob == 0.0 && u > 0.0) return n;
```

**Byte-identical, not an approximation.** The loop draws no randomness — `u` is drawn above
it — so both the variate and the surviving RNG stream are unchanged. The `u > 0.0` guard
preserves the one case that behaves differently: `uniform()` can return exactly 0.0, and then
`cdf < u` is false at `k = 0` and the original returns -1.

Measured with the two arms in ONE process behind a runtime flag, **alternated** so neither
ordering can favour either (§24.5's method rule), at the engine's own
`pixel_size_um = film_format_mm*1000/max(w,h)`:

| geometry | pixel | before | after | speedup |
|---|---|---|---|---|
| 4096x3072 (12.58 MP, export) | 8.545 um | 30225-32893 ms | 3601-3895 ms | **8.4-8.8x** |
| 2048x1536 (3.15 MP) | 17.090 um | 19204-19287 ms | 749-762 ms | 25.3-25.7x |
| 640x480 (0.31 MP) | 54.688 um | 4750-4813 ms | 75-81 ms | 58.6-64.0x |

Every rep byte-identical, in both orderings. The multiplier *rises* as pixels get coarser,
because `n` scales with pixel area and `n` is the length of the wasted walk — so the table is
a scaling law, not a preview claim: the app's fit preview and live draft already skip grain
entirely (`ParamsState.skipGrainHalation`). What does run grain is the 100% zoom ROI, the
magnifier, and export — all at native pixel size, i.e. the 8.4x row.

After the change the stage decomposes as sampler 2430.4 ms (67.3%), dye-cloud blur 285.4,
final blur 259.7, everything else 342, total 3611.1 ms. The sampler alone went
**29491 -> 2430 ms (12.1x)**; what is left of it is genuine work — ~113 M Poisson normal
draws plus ~74 M binomial normal draws.

### 25.5 The gate, and why the existing ones were not enough

The two grain tests are **statistical** (mean preservation, noise std +/-15%): they cannot
see an element-wise change in a sampler, so they would have passed just as happily had this
been subtly wrong. That is the §23 trap in a different costume — an assertion that cannot
fail is not a gate.

`tests/test_binomial_shortcircuit.cpp` gates it element-wise against a **verbatim
transcription** of the loop it replaces (the pattern `test_fft_convolve` uses), checking both
the variate and the surviving RNG stream over ~21,500 cases: the degenerate region, the
ordinary small-p region that must still walk, the `n = 24/25/26` and `var = 10` boundaries,
constructed cases straddling the underflow edge, and a randomised sweep. It refuses to pass
if the sweep failed to enter any of the three regions, so it cannot silently stop testing
what it claims to test.

It was mutation-checked. `return n-1`, an extra RNG draw, and a short-circuit loosened to
`prob < 1e-300` are all caught. Two mutations survive, both understood: `prob < 1e-320` is
caught at -O2 but not at the shipping flags, because `-ffast-math` flushes denormals to zero
and the two conditions become the same program there — which is precisely why the parity job
runs both legs; and the `u > 0.0` half of the guard, which would need `uniform()` to return
exactly 0.0 (~2^-53) and is therefore documented as uncovered rather than papered over.

One harness detail worth knowing: `run_engine_parity.sh` marks a test failed when its stdout
matches `/fail/i`, so a summary counter printed as `failures=0` fails the gate. It is
`mismatches=` now.

Parity **40/40 ALL OK on both legs** (39 + this one).

### 25.6 What is left

Grain is now 3.6 s of a 12.58 MP host render, 67% of it the surviving normal-approximation
draws — ~190 M `std::normal_distribution` variates, which is rejection sampling with a
`sqrt` and a `log` apiece. Replacing that generator would be the next lever and it is **not**
byte-identical: it changes the RNG stream and therefore the grain field. That is survivable
by the same argument the block-seeding change already relies on (goldens are grain-off; the
grain gates assert statistics plus reproducibility) but it is a deliberate change to output,
not a free one, so it is an owner call rather than something to take unilaterally.


## 26. #158 final: remove the duplicate RAW result, keep the decoder exact

The patched LibRaw 0.22.2 release path was re-profiled on the connected SM-S948W
(API 36, arm64-v8a) with `-O3`, FP contraction disabled for the wrapper, zlib on,
and shipping OpenMP off. The authoritative thread-policy result did not change:
compressed Fuji output is scheduling-dependent under OpenMP, so release stays serial.
`raw_decoder.cpp` still does not set `user_qual`; changing demosaic quality was not
smuggled into a performance ticket.

The measurable redundant work was ownership. The old successful path held a
`std::vector<float>`, allocated another equally large `malloc` buffer in JNI, copied
the complete frame in cancellable 1 MiB chunks, then registered the copy. The new path
writes every sample once into uninitialized malloc-backed storage and transfers that
exact base into the existing token/base/capacity registry. This removes one complete
output allocation and copy: **149,817,600 bytes** for the 3060x4080 Samsung frame and
**119,771,136 bytes** for the 2736x3648 MotionCam frame. Every JNI failure after transfer
releases through the registry; failures before transfer remain RAII-owned. The JNI path
uses the same injectable production publication seam as the ASan/UBSan host test. That
test covers release-to-adopt, pre/post-adopt cancellation, platform failure/exception,
post-publication cancellation, normal close, stale-token rejection, and zero outstanding
entries after every rollback rather than testing a detached copy helper.

The added release telemetry names all boundaries: fd I/O, `open_buffer`, `unpack`,
`dcraw_process`, `dcraw_make_mem_image`, output allocation, uint16-to-float copy,
CAT/tint, ACES-to-ProPhoto conversion, and JNI handoff/publication. The device probe
now selects buffer or fd input, reports decode and output-write wall time separately,
and emits one payload per repetition for SHA-256 comparison.

### 25.1 Matched native A/B and why there is no inflated speed claim

Three-run medians for the full 3060x4080 Samsung source were:

| arm64 release path | buffer decode | fd decode | output allocation + uint16 copy |
|---|---:|---:|---:|
| frozen vector/JNI-copy baseline | 456.274 ms | 449.826 ms | 43.209 ms (buffer), 30.139 ms (fd) |
| malloc-backed/final core | 438.496 ms | 450.877 ms | 32.031 ms (buffer), 32.975 ms (fd) |

The buffer run improved 17.778 ms, while the fd repeat was 1.051 ms slower. That is the
same anonymous-page first-touch effect documented in §24.5: removing value-initialisation
moves page faults from allocation into the first sample writes. It is **not** a stable
LibRaw throughput win. The stable result is the deleted *second* JNI allocation/copy and
the corresponding full-frame peak-memory reduction.

### 25.2 Matched minified release/R8 APK evidence

The final release target and separate release AndroidTest APK were built with R8 and
`lintVitalRelease` enabled. For the baseline arm, only `lib/arm64-v8a/libsfraw.so` was
replaced by the frozen vector/JNI-copy build; the R8 dex, resources, other ABIs, test APK,
debug signer, input path and phone were the same. Both fixtures were copied into the
target app's external-files sandbox and SHA-256 verified before the run. The focused
probe reopened an fd for every repetition, timed `RawDecoder.decodeToLinear`, and hashed
the complete public float-buffer lease before closing it.

Qualification is fail-closed: `ticket158_expected_sha256` must name the independently
pinned 64-hex float-payload digest, and every repetition is compared to it. Omitting that
argument fails with `TICKET158_RAW_RELEASE_R8: FAIL`. A developer may instead pass
`ticket158_exploratory=true` to discover a repeatable digest, but the output is deliberately
labelled `TICKET158_RAW_RELEASE_R8_EXPLORATORY: RESULT (UNQUALIFIED)` and is not
qualification evidence. The two modes are mutually exclusive. Content-URI probes use only
`READ_MEDIA_IMAGES` on API 33+, only `READ_EXTERNAL_STORAGE` on API 29-32, and require an
already-granted manifest/URI read permission below API 29; no unavailable shell identity is
called on the legacy branch.

| source / release arm | repetitions | decode p50 | JNI publication p50 | float payload SHA-256 |
|---|---:|---:|---:|---|
| Samsung 3060x4080 baseline | 5 | 870.807 ms | 34.628 ms | `a78c7e3957c39f55b782ee4af69be937ec8d633fd3e0bb689bdc6f841c51c463` |
| Samsung 3060x4080 final | 5 | 838.443 ms | 0.040 ms | same, 5/5 |
| MotionCam 2736x3648 baseline | 3 | 671.795 ms | 25.730 ms | `ecfadb24fefc31d9044830f2a8f65ae9b61eae08fc42befd10fb77aa1ef79fda` |
| MotionCam 2736x3648 final | 3 | 640.619 ms | 0.032 ms | same, 3/3 |

The 32.364 ms and 31.175 ms decode-p50 reductions are approximately the removed JNI
publication cost. The internal decode phase did not become consistently faster: its page
faults moved between allocation and the first sample write, just as the standalone buffer/fd
A/B showed. Therefore the supported claim is one fewer full-frame allocation, lower peak
memory, and a removed 25-35 ms JNI copy on these inputs—not a faster demosaic and not a
1-2 second whole-export result. The final APK also passed the complete release-candidate
instrumentation smoke, including injected write failures and native-result recreation/lifetime.

### 25.3 Shipping thread and oversubscription verdict

All three release ABI CMake caches record `SFRAW_ENABLE_OPENMP=OFF`; arm64 compile commands
contain `-O3 -DNDEBUG` and no `-fopenmp`. ELF inspection of the final `libsfraw.so` found no
`libomp` dependency and no `omp_`, `GOMP_`, or `__kmpc` dynamic reference. During three
consecutive ten-decode final-APK runs, 29 active `/proc` samples observed at most 15 process
threads. The sampled names were the main/instrumentation thread plus ART signal, profiler,
JIT, heap/finalizer, binder and profile-saver threads—no LibRaw/OpenMP worker appeared.

That is the measured reason no new LibRaw/engine thread-budget coordinator was added: the
shipping decoder contributes one calling thread, and import completes before engine rendering.
An OpenMP experiment would require a shared bounded budget and a fresh exact corpus gate, but
the current patched release is not eligible because the compressed-Fuji output changed across
runs.

### 25.4 `AImageDecoder` is a separately qualified non-RAW route

NDK `AImageDecoder` (API 30+) can decode supported platform formats into caller-provided
memory, but it publishes platform RGBA/dataspace output—not LibRaw's scene-linear ACES to
ProPhoto RAW-development contract. The app already has a Java `ImageDecoder`/`BitmapFactory`
path for non-RAW images and display-referred compressed-DNG fallback. Replacing that path is
therefore not part of this LibRaw ownership change and must never silently become an
archival-exact entry point.

The separately routed work should be titled **“Benchmark and qualify NDK AImageDecoder for
API 30+ non-RAW/fallback imports.”** Its gate is: capability/MIME selection; matched Java-vs-
native latency and peak-memory A/B; JPEG, PNG, GIF, WebP, BMP/ICO/WBMP, HEIF and fallback-only
DNG comparisons on API 30, 34 and 36; orientation, ICC/dataspace and decoded-sample evidence;
caller-memory OOM/cancellation/hostile-input tests; unchanged API 24-29 and LibRaw RAW routes;
and no archival-exact admission unless the OS-version corpus proves the required samples.

The serialized probe containers did not move. The following values are SHA-256
digests of the complete `.sfraw-f32` files—a 24-byte container header followed by
the float payload—not the headerless raw float-buffer digests reported in §25.2:

- Samsung source `58093ddf…`: baseline, malloc intermediate, final buffer, baseline fd,
  and final fd — 3/3 each, all `.sfraw-f32` file SHA-256
  `f1c23a56519300b85ec006e08e1c28dc722d4f53015377d2bc0d166a2e928309`.
- MotionCam source `bb74d328…`: baseline and final — 3/3 each, all
  `.sfraw-f32` file SHA-256
  `1f83e610505fbc232f79e4c58cb7eccb4e572c01e340676a22dcb7443d3da14b`.

The six-test shipping-serial host project passed under ASan/UBSan, including hostile
inputs, exact 56-vector CAT02 bits, in-flight cancellation, dimension/metadata/OOM
limits, concurrent lossless-JPEG first use, and the production publication seam's
release/adopt/cancel/failure/exception/success/overflow exact-cleanup checks. The Android
release module built arm64-v8a, armeabi-v7a, and x86_64. These are
component facts: a roughly 0.36-0.45 s RAW decode cannot establish a 1-2 s whole export,
whose dominant engine/effect work is measured elsewhere in this dossier.

## Running it

```bash
bash tools/perf_lab/build_push_run.sh   # laptop + attached device
```

Three sections: the f32 Highway A/B (with a cross-process checksum equality check;
the f64 tier this once ran was removed in `87c60af` — see §1),
an affinity sweep over `SPK_BIG_CORE_RATIO`, and the three parity-affecting levers.
Host-side, the same binaries build with the compile lines in each file's header.

## What this branch deliberately does NOT contain

Cold start (#152) — app-architecture work a bench cannot answer.

(The export foreground service, **#153**, was on this list until the isolation run
in §13 measured it at ~4×. It is now implemented here: `ExportForegroundService`.)

**The REST of #148.** The print integral is one of three; `filming` (spectral
upsampling → camera raw → density curves → DIR couplers) is a genuinely new
shader with a much larger surface, and the remaining CPU↔GPU round-trips between
stages are the other half of the milestone. What landed here is the slice that
reused a validated kernel; the rest is still XL and still wants the on-device
stage timings before anyone sizes it.

**The deep progressive pyramid** (`PERF_ROADMAP` item 6). The app-level ladder is
now gated and tunable (§8–9), but the native model — the engine itself producing
a coarse result and refining it, reusing work across levels — needs the memo
structure reworked and is not a bench question either.

*Film modeling powered by spektrafilm (GPLv3).*
