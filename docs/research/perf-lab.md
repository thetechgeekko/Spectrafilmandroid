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
