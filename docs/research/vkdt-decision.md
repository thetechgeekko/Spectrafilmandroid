# The vkdt question, costed against measured numbers

*Owner asked for this after the export profile came in. It is a decision document,
not a plan — nothing here is committed to or started.*

> **Renumbered 2026-08-29.** The first version of this document was costed against a
> **debug** export (12189 ms), before it was discovered that three of the four native
> modules compiled at `-O0` in debug (`perf-lab.md` §19). Every figure below is now the
> **release** measurement. The conclusion did not change — it got sharper: the engine is
> **88%** of a release export, not 67%.

GPLv3 throughout. Film modeling powered by spektrafilm (GPLv3).

## 1. Why the question is live now, and what changed

For most of this project the performance work looked inside the engine because the
engine is what the parity gate covers. The export profile ended that — and once the
debug `-O0` bug was fixed and the same export was measured on a **release** build, it
ended it far more decisively than the first reading suggested.

On a 12.5 MP export, release build, SM-S948W:

```
  simulate       5504 ms   88.0%    the engine — parity-gated
  decode          546 ms    8.7%    of which dcraw_process 325
  everything else 171 ms    2.7%    encode, setup, exif, residual
  grade            30 ms    0.5%
  ------------------------------
  total          6251 ms
```

The target is 1–2 s. **That is the whole reason this question exists**: no arrangement
of the non-engine work reaches it. On these numbers that is not an argument, it is
subtraction — see §2.

## 2. The arithmetic a rewrite would have to beat

The favourable case for staying put no longer needs constructing, because the
unfavourable-to-the-CPU case is now trivial. **Delete decode, grade and encode
entirely** — not optimise them, delete them, a bound no implementation can beat:

```
  simulate   5504 ms    unchanged — CPU
  everything else 0 ms  the impossible best case
  -------------------
  total      5504 ms
```

**5.5 s, against a 1–2 s target, with the entire rest of the pipeline free.** Incremental
CPU work outside the engine cannot reach the target under *any* assumption; there is no
close call left to make. Either the engine moves to the GPU or the target moves.

This also retires #158 as a lever: OpenMP on `dcraw_process` (325 ms) is worth ~160 ms
at an ideal 2×, i.e. 2.6% of the export. It is landed and it stays, but it is not part
of any route to 1–2 s.

That is the strongest form of the case for a GPU pipeline, and it is a real one.

## 3. The parity problem — state it exactly, because it is the crux

The prime directive is parity with the spektrafilm oracle, defined in `CLAUDE.md` as two
separate things:

1. **within the band** — `max_abs ≤ 1e-4`, `rms ≤ 1e-5` against the oracle;
2. **byte-identical across thread counts** (`SPK_NUM_THREADS` 1 ≡ 8).

A GPU pipeline can satisfy (1). We have measured it: the scan and print-expose offloads
land at **3.1e-06** against the CPU export — 32× inside the band. GPU float is not the
obstacle people assume.

(2) is where it breaks, and it breaks in a way worth being precise about. The
thread-invariance contract exists because the CPU fork-join splits work into chunks and
we require the split not to change the answer. On a GPU the equivalent guarantee — that
the result does not depend on workgroup scheduling — holds for pointwise and
row-independent work, and does **not** hold for anything with a reduction or a
cross-lane dependency unless it is written to. It is a property of each kernel, not of
"the GPU".

So the honest framing is not *"parity dies"*. It is: **the gate would have to be
re-derived per stage**, and one gate — grain's — is already statistical rather than
byte-exact, which is why grain keeps coming up as the least-obstructed target.

## 4. What we actually know about the GPU here, measured

| | result | source |
|---|---|---|
| scan + print-expose offload correctness | 3.1e-06 vs CPU | perf-lab, `test_gpu_host` |
| GPU preview offload speed, 0.195 MP | 1.00× | #146 |
| GPU preview offload speed, 0.78 MP | 0.95× | #146 |
| GPU preview offload speed, 3.12 MP | 1.007× (slower) | #146 |

That is a **null result across a 16× range**, and it is the single most important
counterweight to the rewrite case. The GPU path is genuinely active and genuinely
correct, and it bought nothing — because `scan` is a small fraction of a frame while
grain and halation dominate and stayed on the CPU.

**It does not follow that a full GPU engine would also buy nothing.** #146 offloaded
~1 s of an 8.1 s engine. But it does mean the rewrite case currently rests on *zero*
measurements of the GPU doing the work that actually costs.

## 5. The gap, and the experiment that closes it

**Nobody has measured GPU against CPU on a big engine stage at full resolution.**

That is a startling thing to be true before contemplating a rewrite, and it is cheap to
fix, because the machinery already exists: `gpu/vulkan_compute.cpp` is a persistent
Vulkan compute host, already validated, already shipping behind a toggle.

The decisive experiment is **one stage, at export resolution**:

- **grain — 42% of the engine.** The largest single stage, and the one whose parity
  gate (`test_grain`, `test_grain_sublayer`) is **already statistical** — mean and
  noise-std against a committed reference, not bytes. So a GPU implementation can be
  validated by the gate that already exists rather than needing a new contract.
- **halation — 23%** — second, and a separable IIR blur, the classic GPU shape.

Those two shares were measured inside the debug engine (3384 ms and 1905 ms of 8150).
The engine was the one module already compiling at `-O2` in debug, so the *shares* are
meaningful; the absolute times are not, and scaling them by the engine's 1.48x
debug→release ratio assumes that ratio is uniform across stages, which nobody has
checked. Treat "grain is ~40% of the engine" as the finding and re-measure the
milliseconds on release before sizing anything against them.

**The decision rule, on release numbers.** If GPU grain at 12.5 MP comes back at 10×,
the engine goes 5504 → ~3100 ms — still not 1–2 s, but it proves the GPU delivers on
this hardware for this shape of work, and a full-pipeline rewrite becomes the obvious
route. If it comes back at 2×, grain drops to ~1150 ms, the engine to ~4300 ms, and the
rewrite case collapses: a whole-engine 2× on 88% of the export lands at ~3.5 s, months
of work for less than half the target.

**We do not currently know which.** That is the whole decision, and it costs one stage
to find out.

## 6. What vkdt would fix, and what it would not

Recorded honestly, and flagging where this needs primary-source verification rather than
recollection.

**Would fix.** A node-graph GPU pipeline puts demosaic on the GPU — though on release
that is `dcraw_process` at 325 ms, 5% of the export, so this is a rounding error, not a
reason. The real thing it fixes is that it removes the CPU↔GPU round trips a partial
offload pays, which is a cost our incremental path keeps re-paying on every stage it
moves. That argument stands on its own and does not depend on the decoder at all.

**Would not fix.** It does not answer §5's question either — it *assumes* the answer.
Adopting vkdt is a bet that GPU grain, GPU halation and GPU DIR-couplers are fast, made
without measuring any of them.

**Would cost.** The parity gate is built around our CPU stage structure. `engine-parity`
recompiles 78 files 38 times, twice over (`-O2` and shipping flags); a node-graph engine
does not slot into that. Every stage's gate would need re-deriving, and §3 says that is
per-kernel work, not a one-time policy change.

**Verify before relying on any of this:** vkdt's actual licence terms as vendored, what
its graph does and does not let you pin numerically, and whether its demosaic is one we
would accept as a *quality* matter — that last one is the same `user_qual` question in
different clothes, and it is the owner's call, not an engineering one.

## 7. Recommendation

**Do not decide vkdt from principle. Measure GPU grain at export resolution first.**

The rewrite case is real — §2's arithmetic is not escapable, and 84%-engine at ~9.7 s is
the honest ceiling of the incremental path. But the case currently rests on an untested
assumption, and the one measurement that would test it is cheap, uses machinery that
already exists, and targets the stage with the least parity friction in the entire
engine.

If that number is good, this document becomes the plan. If it is bad, it saves months.

Either way it should be taken before the commitment, not after — which is the same
discipline that retired the Highway f64 tier, the "Use performance cores" setting, and
the first cut of the 90/270 transpose.

## 8. Reading the actual source: vkdt already implements spektrafilm — on a different grid

Checked against a clone of `hanatos/vkdt` at `b95b3a0`, not from recollection.

**`src/pipe/modules/filmsim/readme.md`, first line:** *"this is an implementation of
Andrea Volpato's spektrafilm."* Same upstream our engine is a port of. The module has
our whole pipeline as Vulkan compute shaders — `expose.comp`, `develop.comp`,
`scatter.comp`, `halin/halout.comp`, `dirlut.comp` (DIR couplers), `negprint.comp`,
`print.glsl`, `scan.glsl` — and its parameter list maps almost one-to-one onto ours,
down to `preflash`, `cp amt`, `lang r/g/b`, `cp rad`, `hal bnc`, `hal dec`, `scat amt`,
`g fast`, `g slow`, `exhaust`, `hl boost`. Those are the same features our
`preflash_e2e`, `provia_couplers_e2e` and `highlight_boost_e2e` gates cover.

The whole module is **1979 lines**, because most of the math is precomputed into
`filmsim.lut` by `mklut-profiles.py` — which runs against spektrafilm's own
`profile/*.json`.

**Licence is fine.** vkdt's code is 2-clause BSD (13 files carry GPL, none in filmsim),
which is compatible with our GPLv3. The filmsim *profiles* are CC BY-SA 4.0 by Andrea
Volpato — the same data-licensing situation we are already in.

### But it is not our model, and the readme understates by how much

`filmsim.glsl` declares its wavelength grid outright:

```
  lambda_arr[11] = 380, 390, 400 ... 800, 810      // 44 bands @ 10 nm
```

Ours, from `model/spectral.h`: *"Mirrors spektrafilm/config.py exactly:
SpectralShape(380, 780, 5), i.e. 380..780 nm @ 5 nm -> 81 samples. This governs every
spectral..."*

**vkdt runs the spectral integrals at half our resolution** — 44 bands at 10 nm against
81 at 5 nm — and over a different range. That is a different quadrature in *every*
spectral step: camera-raw formation, dye density to transmittance, enlarger exposure,
scan to XYZ. It will not sit inside `max_abs ≤ 1e-4`; it is not a precision question but
a different discretisation.

The readme discloses "some changes for efficiency (for instance the grain model was
swapped out)". The 5 nm → 10 nm halving is a larger change than the grain swap and is
not named there. Not a criticism of vkdt — it is a real-time editor and the trade is
obviously right for it — but it is decisive for us.

Two smaller structural differences, recorded for completeness:

- **Base-2 density math.** vkdt uses `exp2(-ds)`; our engine has a dedicated
  `kernels/exp10.h` because spektrafilm's densities are base-10. Convertible in
  principle, a different numerical path in practice.
- **Subgroup reductions.** `filmsim.glsl` reduces with `subgroupAdd`. Summation order
  depends on `gl_SubgroupSize`, which differs between GPU vendors — so results would
  vary by *device*, not just from the CPU. Our contract currently requires invariance
  across thread counts; this would add a device axis we have never had to defend.

### What this changes about the decision

It removes the vaguest version of the question and replaces it with a sharp one.

**"Adopt vkdt's filmsim" now means "ship a 44-band model instead of an 81-band one."**
That is a *product* decision about output, not an engineering one about speed, and it is
the owner's to make — the same class as `user_qual` in #158, and much bigger.

The third route is now the well-founded one rather than the speculative one:

> **Write our own 81-band GPU shaders, using vkdt's module as the architecture guide.**

vkdt has already proven the pipeline maps onto Vulkan compute, shown how to lay out the
LUT, and shown which stages decompose into which kernels — at 1979 readable lines under
a compatible licence. That is an enormous head start on design while leaving our grid,
our grain model, and our parity gate intact.

What it does not tell us is still §5's question: what GPU actually buys on our biggest
stage. Nothing here substitutes for measuring that.

## 9. MEASURED: what 44 bands would actually cost the picture

§8 turned "adopt vkdt" into "ship a 44-band model instead of an 81-band one" and called
it a product decision. A product decision still needs a number, and this one is
answerable on a laptop: **band-limit our own profiles and render the same fixture.**

`tools/spectral_bands/` does it with **no engine change, no device, no GPU**. The
film/paper spectral arrays are partitioned into blocks of N samples, each block replaced
by its band value (averaged on the *linear* physical quantity — sensitivity, not log
sensitivity; transmittance, not density) and replicated back across the block. The tree
still has 81 samples, so the unmodified engine runs it, but it carries only the
information a 41-band / 10 nm model would. The NaN mask is preserved exactly, so band
width is the only variable.

**The control is exact.** Rendering the shipped assets against a tree that went through
the identical copy + JSON round-trip returns 0.00 codes on every case, and the untouched
`test_simulate_e2e` still reports `max_abs = 5.96e-08` against the goldens. Every number
below is band width and nothing else.

Output is sRGB-encoded, so these are **8-bit display codes** — the unit the question is
actually asked in.

### 81 bands @ 5 nm vs 41 bands @ 10 nm — the vkdt-comparable case

| case | median | p90 | p99 | max | rms | ≥1 code | ≥5 codes |
|---|---|---|---|---|---|---|---|
| scan, portra 400 | 0.23 | 0.58 | 0.86 | **1.50** | 0.33 | 0.26% | 0.00% |
| scan, provia 100f | 0.12 | 0.71 | 2.25 | **9.49** | 0.60 | 6.39% | 0.22% |
| print, portra 400 → endura | 0.21 | 1.30 | 3.86 | **15.08** | 0.99 | 14.26% | 0.55% |
| print, ektar 100 → supra | 0.16 | 1.23 | 3.54 | **16.78** | 0.91 | 12.43% | 0.31% |

**The two routes answer differently, and that is the finding.**

- **The scan (slide) route survives 10 nm.** Portra at 1.5 codes worst-case, a quarter
  of a code typically, 0.26% of samples off by even one code. That is invisible.
- **The print (enlarger) route does not.** 15–17 codes at the worst pixel, one sample in
  eight off by a code or more, one in two hundred off by five or more. Not catastrophic,
  but not something you could ship as "the same picture".

The print route is ~10× worse on the same profiles, and the reason is structural: the
enlarger multiplies the negative's spectral transmittance against dichroic filters and
then against the paper's sensitivity, so a band-averaging error is applied three times
in series instead of once. Positive film through the scan route pays it once.

### The trend, which rules out going further

| bands | scan portra max | print portra max | print portra ≥5 codes |
|---|---|---|---|
| 41 @ 10 nm | 1.50 | 15.08 | 0.55% |
| 27 @ 15 nm | 3.62 | 27.22 | 4.67% |
| 21 @ 20 nm | 7.83 | 46.02 | 17.78% |

Error grows faster than linearly in band width on the print route — 46 codes at 20 nm,
with 18% of samples off by 5 or more. Whatever else is true, **10 nm is the floor**, not
a waypoint to something coarser.

### What this settles

1. **44 bands is not free, and it is not fatal.** Anyone claiming either without a
   number was guessing. The honest statement is: free on the slide route, visibly
   different on the print route.
2. **The halving is worth ~2× on the spectral loops only** — the stages that iterate
   over bands. It does nothing for grain, which is ~40% of the engine and does not touch
   the spectral axis. So the *speed* case for 44 bands is much weaker than the
   band-count ratio suggests, while the *quality* cost is real and concentrated in the
   route most of our profiles target.
3. **This strengthens route three** (§8): write our own **81-band** GPU shaders with
   vkdt as the architecture guide. We would be giving up measurable print-route accuracy
   for a speedup that does not apply to the largest stage. There is no reason to pay it.

The experiment is committed and re-runnable: `bash tools/spectral_bands/run_band_probe.sh`.

## 10. The RAW decoder: what vkdt uses, and why swapping ours would not help

Owner's question — our decode is slow, what does vkdt use, and what about darktable's
`rawspeed`?

**What vkdt uses.** `src/pipe/modules/i-raw` links either **rawspeed** (darktable's,
pinned at commit `ae217c0`, cloned at build time — `i-raw/flat.mk`) or **rawloader**
(the Rust `rawler` crate), selected by `VKDT_USE_RAWINPUT`. Never LibRaw.

**But that is not the interesting part.** `i-raw/main.cc` calls `decodeRaw()` and then
passes the **CFA mosaic** downstream — `mod->img_param.filters =
ColorFilterArray::shiftDcrawFilter(...)`. Demosaic is a **separate GPU module**:
`src/pipe/modules/demosaic/` is a set of compute shaders (`rcd_conv.comp`,
`rcd_fill.comp`, `halfsize.comp`, `down.comp`, `splat.comp`, `gauss.comp`, `fix.comp`)
implementing RCD. rawspeed's job in vkdt is **unpacking and decompression only.**

**Why that matters for us.** Our release decode is 546 ms, and 325 of it (60%) is
`dcraw_process` — which *is* the demosaic (AHD; `raw_decoder.cpp` never sets
`user_qual`, so LibRaw's default applies). rawspeed does not demosaic at all. Swapping
LibRaw for rawspeed would replace our `unpack` phase, which was 46 ms even on the
`-O0` debug build. **It would buy approximately nothing**, at the cost of a C++20
dependency with pugixml/libjpeg/zlib, a runtime `cameras.xml`, and a licence review.
The Rust option is worse for us — a cargo toolchain inside an NDK build.

The lever vkdt actually pulls here is **GPU demosaic**, not a different CPU decoder.

**And the honest size of the prize.** On release, decode is **546 ms of a 6251 ms
export — 8.7%.** Perfect GPU demosaic saves at most 325 ms, ~5%. This looked like a
much bigger problem when decode read 3647 ms, and that number was an artefact of the
debug `-O0` bug (`perf-lab.md` §19). The decoder is no longer where the time is.

**The one cheap CPU lever that does exist** is `user_qual`: LibRaw defaults to AHD, and
PPG or VNG are materially faster. It moves pixels, so it is a quality call and it sits
outside the parity gate — the same shape of decision as §9, and about a 5% prize. It is
recorded, not recommended.

## 11. DECIDED: our own 81-band GLSL shaders, vkdt as the architecture guide

Owner's call, 2026-08-29: **route three.** Keep our 81-band / 5 nm grid, keep our grain
model, keep the parity gate; write our own compute shaders, using vkdt's module graph as
the design reference rather than adopting its code. §9 supports it — 44 bands costs
visible print-route accuracy for a speedup that does not apply to the largest stage.

One thing this decision inherits for free: `gpu/scan_spectral.comp` **is already an
81-band shader** (`#define NB 81`). The route does not start from zero.

### 11.1 What GLSL does to each effect

Answering the owner's question directly — what happens to the effects if we go GLSL.

| stage | shape | GLSL verdict |
|---|---|---|
| `scan` spectral integral | per-pixel, 81-band loop, no cross-pixel reads | **already written**, `scan_spectral.comp`. Needs on-device numeric validation, not new code. |
| `filming_expose` | per-pixel + LUT fetches | same shape as `scan`. Straightforward. |
| `develop` | per-pixel curve interpolation | trivial — a texture fetch. |
| `dir_couplers` | per-pixel + a diffusion blur | fine; the blur is the easy part. |
| `halation` | separable Gaussian + IIR exponential | Gaussian is two passes, textbook. The IIR is a serial recurrence **along** a row, which is bad per-row but there are thousands of rows — parallelise across rows, standard technique. |
| `lens_blur` | Gaussian | trivial. |
| `glare_field` | field build, 30 ms today | trivial. |
| **`grain`** | **per-pixel but wildly divergent** | **the problem child — see 11.2.** |
| **`camera_diffusion`** | **direct 2D convolution, O(n²)** | **GLSL does not save it — see 11.3.** |

### 11.2 Grain: the divergence problem, and why we happen to be allowed to fix it

§20.5 of `perf-lab.md` measured grain's cost varying ~100x with scene content, because
`fast_binomial_one` degenerates into an O(n) CDF-inversion walk near maximum density.

On a CPU that is a load-balancing problem, and `grain.cpp` already solves it with dynamic
block scheduling off an atomic counter. **On a GPU it is much worse**: a subgroup executes
in lockstep, so if one lane needs ten thousand CDF iterations and the other sixty-three
need five, all sixty-four pay for the worst. Divergent per-lane work is the single worst
shape for a compute shader, and grain is the most divergent thing in the engine.

The saving grace is specific to this stage: **grain's parity gate is already statistical.**
`test_grain` and `test_grain_sublayer` check mean preservation and noise standard deviation
against committed references — not bytes. So a branch-free reformulation (widen the normal
approximation's domain, or bound the CDF walk) is checkable by the gate that already
exists. Grain is the one stage where we have that freedom, and it is exactly the stage
that needs it.

### 11.3 Black Pro-Mist: the algorithm matters ~70x more than the language

The cost model is derived from the source (`bloom_max_lambda_um = 380 x 2.5 = 950 µm`,
`radius = ceil(8 x bloom_max_lambda_um x scale / pixel_size_um)`, `ks = 2r+1`, cost =
`n_pixels x ks² x 3`) and **validated against all four measured points**, which come back
at a constant 2.85–3.01e9 MAC/s — under 5% spread:

| side | radius | MACs | measured | implied rate |
|---|---|---|---|---|
| 192 | 41 | 7.62e8 | 253 ms | 3.01e9 |
| 384 | 82 | 1.20e10 | 4108 ms | 2.93e9 |
| 640 | 136 | 9.16e10 | 30653 ms | 2.99e9 |
| 768 | 163 | 1.89e11 | 66376 ms | 2.85e9 |

At a 12 MP export (3060x4080) the radius is **862** and `ks` is **1725**:

| | 12 MP Pro-Mist |
|---|---|
| operations | 1.115e14 MACs |
| host CPU, measured rate | **10.9 hours** |
| mobile GPU in GLSL, brute force (~5e11 MAC/s) | **223 seconds** |
| the same operator via FFT | **~18 ms** |

**GLSL buys ~180x. The algorithm buys ~12,600x.** Porting the direct convolution to a
compute shader takes an eleven-hour export to a four-minute one, which is still two orders
of magnitude off a 1–2 s target. Pro-Mist has to be re-derived as an FFT convolution or a
sum of IIR passes (#160) **whether or not it goes to the GPU**. Doing both makes it free;
doing only the shader does not make it usable.

This generalises, and it is the main thing to take from the exercise: **GLSL is a constant
factor. It does not change complexity class.** Every stage above is O(n) or O(n·k) with
small k, which is why they all port cleanly — and `camera_diffusion` is the one that is
not, which is why it is the one GLSL cannot rescue.

### 11.4 Three things GLSL costs us, and what to do about each

**1. Precision — fp32, and there is no fp64 to fall back on.** Mobile GPUs either lack the
`Float64` capability or run it at 1/16–1/32 rate. Our CPU path accumulates the spectral
integral in `double`, and `scan_spectral.comp`'s own header already concedes the point:
*"PREVIEW-ONLY / non-bit-exact by design (GPU float != the f64 oracle path)."*

The budget works out, though, and this is worth stating precisely rather than fearing:
accumulating 81 fp32 terms carries roughly `√81 × 2⁻²⁴ ≈ 5e-7` relative error against a
tolerance of `1e-4` — about 100x of headroom. **The risk is not the accumulation, it is the
transcendentals**: GLSL specifies `pow`/`exp2` only to a vendor-defined ULP bound, and the
integral calls `pow(10.0, -D)` once per band. That must be **measured on real hardware per
vendor**, not assumed. fp16 is not an option for this kernel — an 11-bit mantissa is
~5e-4 relative, which exceeds the tolerance on its own.

**2. Determinism — keep it by construction, not by hope.** Our contract is byte-identical
output across worker counts. `scan_spectral.comp` satisfies it: every pixel is independent,
there is no cross-lane reduction, and the 81-term accumulation runs in a fixed order inside
one invocation. That is a **design rule to keep**, and it is precisely where vkdt's
`filmsim.glsl` would have hurt us — it reduces with `subgroupAdd`, whose summation order
depends on `gl_SubgroupSize` and therefore varies by GPU vendor (§8). So: **no
`subgroupAdd`, no atomics in the numeric path, no shared-memory reductions with
unspecified order.** Fixed reduction trees if a reduction is unavoidable.

What we cannot keep is byte-equality *with the CPU path*. The GPU contract has to become
"within parity tolerance of the CPU reference", with byte-equality retained only
GPU-to-GPU on the same device.

**3. CI cannot run it — RESOLVED, it can.** The concern was that all the gates compile with
host g++ and GitHub runners have no GPU, so a wrong shader would surface only as a wrong
picture on a device. Mesa **lavapipe** was the candidate and was recorded as untested.

**Tried, and it works.** `mesa-vulkan-drivers` is a stock Ubuntu package the runner installs
in seconds; it provides a software Vulkan 1.4 device (`llvmpipe`,
`PHYSICAL_DEVICE_TYPE_CPU`). Our existing `tests/test_gpu_host.cpp` — previously marked
LOCAL-ONLY for exactly this reason — runs the real Vulkan branch under it and passes:

| | GPU export vs CPU export |
|---|---|
| `scan/linear`, `scan/fused` | within 1e-4 |
| `print/linear` | `max_abs` **3.580e-06** |
| `print/fused` | `max_abs` **2.434e-06** |

plus warm-host determinism (three GPU previews byte-identical) and the #149 law that
`gpu_preview` cannot reach an export. The GPU preview is also *closer* to the export than
the CPU LUT preview is (print/linear 1.332e-04 vs 4.921e-04).

Wired into the `engine-native` job. **One trap, and the gate is built around it:**
`test_gpu_host` reports `ALL OK` in *both* cases — with a GPU it validates the shader, and
without one it validates only the fallback law. Those are not the same gate, so the CI step
additionally requires `self-check passed (state == 1)`; a broken or missing ICD therefore
fails loudly instead of silently downgrading to a no-op that stays green. Verified in both
directions before landing.

**What this gates and what it does not.** It gates the shader's **math** and its
**determinism**. It says nothing about **performance** — lavapipe is a CPU rasterizer — and
nothing about an **arm64 GPU's transcendental precision**, since GLSL leaves `pow`/`exp2` to
a vendor-defined ULP bound. Point 1 above still has to be measured on a device.

### 11.5 The order this implies

1. ~~**Validate `scan_spectral.comp` numerically on real hardware.**~~ **Partly done, in
   software.** Under lavapipe it validates at 2.4–3.6e-06 against the CPU reference, and
   that is now a CI gate (§11.4). What remains is the *arm64* half: vendor `pow`/`exp2`
   precision on a real Adreno, which software Vulkan cannot stand in for.
2. **Measure GPU vs CPU on `scan` at export resolution** (§5) — still the unknown that
   sizes everything.
3. **Fix `camera_diffusion`'s algorithm** (#160) — on the CPU first, where the parity suite
   can see it. It is a parity/product decision and it does not need the GPU.
4. **Then port the O(n) stages** in contiguous runs, not one at a time: each isolated stage
   pays an upload + download, which is the round-trip cost §6 named as the real thing a node
   graph fixes.
5. **Grain last**, with the branch-free reformulation, against its statistical gate.
