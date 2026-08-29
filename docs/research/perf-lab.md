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
| 2 | **big.LITTLE affinity** | no-op (no cpufreq) | **1.58×, checksum unchanged** | **SHIP IT** |
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

**1.58×, and the checksum is byte-identical in every row.** Of everything on this
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

### The export finding — possibly the biggest user-visible win so far

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

**Unmeasured, and stated as a hypothesis, not a result.** It needs the same treatment
everything else here got: a standalone probe that proves byte-identical output against
`std::mt19937` first, then a number. That probe is the next thing worth building, and
it is a much better use of effort than a library swap the arithmetic already rules out.

### What to do now, in order

1. **Ship the affinity win.** 1.58×, bit-identical, measured. It is sitting in this
   branch behind `SPK_BIG_CORES` and wants a decision about defaulting it on.
2. **Isolate the export finding** (§11): 4m46s + SIGKILL on main vs 13.85 s here. If
   it is foreground-vs-background, #153's foreground service is worth more than any
   kernel work on this list.
3. **Probe the vectorised MT19937** — byte-identity first, speed second.
4. Leave OpenCV out of the render path.

## Running it

```bash
bash tools/perf_lab/build_push_run.sh   # laptop + attached device
```

Three sections: the f64 Highway A/B (with a cross-process checksum equality check),
an affinity sweep over `SPK_BIG_CORE_RATIO`, and the three parity-affecting levers.
Host-side, the same binaries build with the compile lines in each file's header.

## What this branch deliberately does NOT contain

Cold start (#152) and the export foreground service (#153) — app-architecture
work a bench cannot answer.

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
