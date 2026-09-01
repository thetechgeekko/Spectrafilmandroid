# Can the GPU produce *exact* results? — [Research: can the GPU produce exact (byte-identical) results vs the CPU C++ engine?](https://github.com/thetechgeekko/Spektrafilm-android/issues/135)

> **Historical research outcome, not current product policy.** The original preview-only
> recommendation in the closing section was superseded after the device probe and resident Vulkan
> pointwise product-route gates passed. Current policy is two named routes: parity-bearing **Strict
> Exact CPU**, and capability-gated **Fast GPU** within oracle tolerance plus same-device
> repeatability. Spatial/stochastic coverage and fleet qualification remain open. See
> [../BIT_IDENTICAL_EXPORT_ROADMAP.md](../BIT_IDENTICAL_EXPORT_ROADMAP.md).

**Question (owner, 2026-08-27):** can we run the engine's processing on the GPU and get the
*exact same result* as the CPU path — not an approximation?

**Answer, in one paragraph:** No — not in the sense of "the same bytes as today's CPU build",
because that target does not exist even on CPU: our own bytes already differ across CPU
architectures (`-ffast-math` FMA contraction, [CLAUDE.md](../../CLAUDE.md)), the Vulkan spec
explicitly disclaims cross-implementation exactness, mobile GPUs have effectively no float64,
and no transcendental function on any GPU API is required to be correctly rounded. **But** the
question has two useful positive answers: (1) *exactness by construction* is achievable if both
sides compute in integer/soft-float or correctly-rounded arithmetic — at a cost that makes it a
research project, not a ticket; and (2) the parity bar we actually ship (**within oracle
tolerance + deterministic**) is plausibly achievable on GPU and is exactly what should be
measured on device. The recommendation for
[Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127)
is at the end.

*Method: 13-agent verified research run (six primary-source investigations, each claim
re-opened and adversarially checked against its cited source: 122 confirmed, 4 refuted — the
corrected versions are used below), plus a dedicated case study of hanatos/vkdt, plus a
completeness-critique pass. Sources are linked inline; quotes are verbatim.*

---

## 1. "Exact" is not one question — it is three

The repo's parity bar ([CLAUDE.md](../../CLAUDE.md)) is: within `max_abs ≤ 1e-4` / `rms ≤ 1e-5`
of the Python oracle **and** byte-identical across thread counts — **"not necessarily
byte-identical across CPU architectures (`-ffast-math` FMA contraction differs by arch)."**
That caveat matters enormously here: there is **no canonical CPU byte stream** for a GPU to
match. An arm64 phone and the x86 CI host already produce different bytes from the same code.
So "exact result par GPU pe" decomposes into:

- **E1 — match the current CPU build's bytes.** Ill-posed (match *which* build?) and
  unattainable on GPU (§2–§4). This is the strict reading, and the answer is no.
- **E2 — make CPU *and* GPU both produce one defined byte stream.** Attainable by
  construction (integer emulation or correctly-rounded math on both sides, §6) — this would
  actually make the CPU cross-architecture byte-stable too, which today it is not. Cost: large
  (§6); it also means abandoning `-ffast-math` on CPU, whose own cost is unmeasured.
- **E3 — GPU within the oracle tolerance, deterministically.** The opt-in spectral LUT
  precedent is approximate by design, although scanner LUT error is profile/domain dependent
  (LUT17: locked D50 <=5e-5; K75P 2383/2393 about 0.0040/0.0073 vs direct). Plausible in
  fp32; the deciding number (81-band fp32 accumulation error vs `1e-4`) is unmeasured and
  needs device hardware (§9).

## 2. What the Vulkan/SPIR-V specs actually guarantee (very little)

All from the [Vulkan spec's SPIR-V environment appendix](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#spirvenv-precision-core-table)
and the [GLSL.std.450 spec](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html):

- Only fp32/fp16 **add, sub, mul** (+ scalar-multiply variants and type conversions) are
  required to be *"Correctly rounded"* — and Vulkan's definition of that term **does not pin
  the rounding direction**: absent the optional `RoundingModeRTE` execution mode, *"either the
  floating-point value closest to and no less than x or the value closest to and no greater
  than x will be returned. Which value is chosen is implementation-defined."* A bare fp32 add
  may legally differ by 1 ULP between two conformant drivers. (`RoundingModeRTE` for fp32 is
  mandated only by the optional Roadmap 2024 profile — core Vulkan requires none of the float
  controls.)
- **Division is 2.5 ULP**, `sqrt` is inherited from `1.0/inversesqrt()` (~2+ ULP). The
  GLSL.std.450 `fma()` is **not** required to be fused (*"Inherited from OpFMul followed by
  OpFAdd"*); the only guaranteed-fused FMA is the brand-new optional
  [VK_KHR_shader_fma](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_KHR_shader_fma.adoc)
  (rev. 1, 2025-06-10).
- **There are zero correctly-rounded transcendentals in the entire API.** `exp/exp2` are
  `3 + 2×|x|` ULP, `log` 3 ULP, `pow` is only *"Inherited from exp2(y × log2(x))"*, `atan` is
  4096 ULP. A ULP bound is a permitted *range*, not a value — every conformant driver may
  return different bits. There is **no `exp10` instruction at all** (it must be synthesized as
  `exp2(x·log2(10))`, inheriting the scaled exp2 bound); our hot path is built on exp10.
- **Compilers have reassociation freedom by default.** Implementations *"may rearrange
  floating-point operations using ... associativity and distributivity"*; under the
  float_controls2 / Vulkan 1.4 regime, undecorated ops are *assumed* to carry
  `AllowContract`/`AllowReassoc`/`AllowTransform` — fast-math is the spec default, opt-out is
  per-operation (`NoContraction` / `FPFastMathMode = None`).
- **fp64, where it exists, is unspecified:** the entire fp64 precision requirement is one
  sentence (*"at least that of single precision"*), and GLSL.std.450 `Exp/Log/Pow/Sin/Cos`
  are **defined only for 16/32-bit operands** — there are no fp64 transcendental instructions
  to call even on hardware that has fp64.
- **NaN propagation is not guaranteed** through extended instructions without
  float_controls2 — and even its proposal admits ULP bounds make Inf/NaN generation
  unreliable. Our engine's NaN-propagation through `density_to_light` is load-bearing
  (`-fno-finite-math-only`), so this is not a corner case for us.
- The [Invariance appendix](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#invariance)
  says it outright: *"The Vulkan specification is not pixel exact. It therefore does not
  guarantee an exact match between images produced by different Vulkan implementations."*
  What **is** guaranteed: same device + identical pipeline ⇒ same result run-to-run (Rules
  4/7; Rule 7 for compute-with-stores holds if no image atomics / no multi-writes / no
  read-after-image-store). Same-device determinism is real; cross-device identity is
  disclaimed by the spec itself.

## 3. Mobile float64: effectively nonexistent

Our engine computes the spectral chain in float64 by design (mirroring NumPy). On mobile GPUs
(data from the community-run [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org) database, per
*device entries*, plus vendor docs):

- **shaderFloat64 on Android: 3.63% of device entries** (125 of 3,439) vs **89.07% on
  Windows**. Enumerating all 125: **zero Mali/Immortalis, zero Samsung Xclipse, zero
  PowerVR** — the list is NVIDIA Tegra K1/X1 (Shield-class), desktop GPUs running Android
  environments/emulators (`llvmpipe`, "MuMu GL/VK", a Ryzen "Emulated Device" reporting as
  "Adreno 751"), patched Mesa-Turnip forks, and a very recent Qualcomm driver branch (below).
  Concrete flagships: Pixel 9 Pro Mali-G715 → `shaderFloat64: false`; Galaxy S24+ Xclipse 940
  (AMD RDNA-derived!) → `false` on Samsung's stock driver.
- **The one emerging exception:** Qualcomm's 2026 proprietary branch 512.863.x reports
  `shaderFloat64 = true` on Adreno 8xx — **undocumented by Qualcomm** (their OpenCL guide
  never mentions `double`; the only documented 64-bit emulation is *integer* register-pairing),
  inconsistent across reports of the same phone on other driver branches, and with **every
  fp64 float-control false** (no RTE rounding, no NaN/Inf preserve, no denorm control). Even
  if native, §2 applies: no fp64 transcendental instructions exist to run our exp10/log10 on.
- Upstream Mesa Turnip (open-source Adreno) hard-codes `shaderFloat64 = false`. Arm's current
  OpenCL guide omits `double` from its supported types entirely (only 5 ancient Midgard-era
  Mali reports in the whole OpenCL DB ever had `cl_khr_fp64`). Apple Metal's shading language
  **has no double type at all**. The only vendor-documented mobile fp64 throughput number is
  Tegra X1 at **1/32 the fp32 rate**.

**Net:** there is no fleet-wide f64 substrate on the hardware we ship to. A native-f64 GPU
port is not on the table.

## 4. The ceiling: what the best GPU compute stack promises (desktop CUDA)

[NVIDIA's floating-point whitepaper](https://docs.nvidia.com/cuda/floating-point/index.html)
and the CUDA math appendix are the best-case reference — and even that ceiling is instructive:

- The five IEEE basic ops (+, ×, ÷, sqrt, FMA) **are** correctly rounded in fp32/fp64 by
  default — so *"The same inputs will give the same results for individual IEEE 754 operations
  ... on the CPU and GPU."* But `-fmad` defaults to **true** (silent FMA contraction), and
  transcendentals are not correctly rounded (`expf` 2 ULP, `powf` 4 ULP, `pow` 2 ULP), their
  bounds are *"derived from extensive, though not exhaustive, testing. Therefore, they are not
  guaranteed"*, and NVIDIA says of GPU-vs-CPU libm outright: *"Because these implementations
  are independent and neither is guaranteed to be correctly rounded, the results will often
  differ slightly."*
- Determinism is a **per-library, conditional promise**, not a platform guarantee: cuBLAS
  bit-reproducibility holds only for one toolkit version on GPUs with *"the same architecture
  and the same number of SMs"*, voided by multi-stream or atomics; cuDNN: *"Across different
  architectures, no cuDNN routines guarantee bitwise reproducibility."*

If the strongest GPU stack in the industry stops at "per-op exactness, same-chip determinism,
no libm parity, no cross-model identity" — mobile Vulkan, which guarantees strictly less on
precision (though *more* on same-device invariance, §2), cannot do better.

## 5. What a GPU port would actually have to reproduce (our math inventory)

From the engine source (verified file:line in the research run):

- Two per-pixel **81-band spectral integrals** (`runtime/stages/scanning.cpp:257-319`,
  `printing.cpp:246-307`): density → `10^-D` × illuminant (NaN→0 per band) → CMF/sensitivity
  accumulation — **all in double, strict band order**, via the custom 2-lane vector `exp10`
  (`kernels/exp10.h`, ≤4 ULP vs `pow(10,x)`, its drift masked by the final f64→f32 store).
- Pervasive double `log10` (filming/printing/scanning round-trips), `pow(10,·)` in LUT/profile
  builds, CCTF/gamut `pow`, Gaussian/exponential kernel `exp` — a wide transcendental surface,
  every call site currently shaped by glibc + `-ffast-math`.
- Float64 spatial chain (halation, diffusion, DIR-coupler diffusion on the f64 irradiance
  plane; the f64 Gaussian/exponential IIR filters are order-sensitive recurrences), stochastic
  grain with seeded RNG in fixed block order, and NaN-propagating profile nulls.
- Already-accepted precision precedents: the exp10 substitution (≤4 ULP, byte-identical after
  the f32 store) and EXPORT_FASTPATH item 7 (≤1.6e-15 f64 drift → **0/1.2M f32 diffs**) show
  the engine's real output tolerance is "f64 drift small enough to vanish at the f32 cast" —
  note this masking **only works because internals are f64**; an fp32-internal GPU port gets
  no such masking.

## 6. Exactness by construction: the four real techniques, with costs

These make E2 (one defined byte stream on both sides) possible. The premise underneath the
first and fourth routes is that **integer arithmetic is exact and portable** (SPIR-V integer
ops are fully-defined two's-complement bit operations; the fast-math freedom of §2 is
floating-point-only). That premise is standard but was not itself adversarially verified in
this pass — flagged for honesty.

1. **Integer-emulated IEEE-754 (soft-float).** [Berkeley SoftFloat](http://www.jhauser.us/arithmetic/SoftFloat.html)
   is the reference; **it has been done in shaders**: Mesa's
   [`float64.glsl`](https://android.googlesource.com/platform/external/mesa3d/+/refs/heads/main/src/compiler/glsl/float64.glsl)
   is a GLSL port of SoftFloat 3e (doubles as `uvec2`, `__fadd64/__fmul64/__ffma64/__fsqrt64`),
   shipped in Mesa 19.0 to give Intel Gen11/12 (no native fp64) OpenGL 4.0. Costs: fp64-heavy
   test shaders compile to **~67,500 instructions** *after* a 30-patch optimization campaign;
   Intel's own emulation FAQ *"does not claim full specification conformance"* and warns of
   unmeasured slowdown; the comparable metal-float64 project estimates **1/32–1/64 of fp32
   throughput**. And soft-float gives us add/mul/fma/sqrt — the transcendentals (exp10, log10,
   pow) would additionally need bit-exact ports of *specific* implementations. Verdict: true
   byte-identity is possible this way, at a cost that almost certainly erases the GPU's speed
   advantage for our compute-bound integrals (Intel's FAQ warns emulated-GPU fp64 is often
   slower than multicore CPU).
2. **Double-double / compensated float-float (Thall df64).** ~48-bit significand from paired
   f32 at ~20 fp32 ops per add — but it is **a different number system, not IEEE binary64**
   (not bit-compatible ever), and its error-free transformations are *broken by fast-math-style
   shader compilers* (Thall observed the Cg compiler destroying twoProd). Rejected for E2.
3. **Reproducible summation (Demmel–Nguyen / [ReproBLAS](https://ieeexplore.ieee.org/document/6875899)).**
   Solves order-dependence (~7–9n flops, ~8× local slowdown) — but our fork-join already
   achieves order-invariance deterministically, and it does nothing about transcendentals:
   *"scientific computations are not bit-to-bit reproducible as soon as they involve
   mathematical functions"* ([Zimmermann](https://members.loria.fr/PZimmermann/papers/coremath.pdf)).
   Not our bottleneck.
4. **Correctly-rounded elementary functions.** The key property: a correctly rounded function
   has **exactly one valid answer**, so any two correct implementations agree bit-for-bit —
   this is how the basic IEEE ops are already portable *on CPUs* (note the scope: that
   portability statement assumes IEEE round-to-nearest environments; Vulkan's weaker
   "correctly rounded" (§2) does not qualify). [CORE-MATH](https://core-math.gitlabpages.inria.fr/)
   ships MIT-licensed correctly-rounded binary32 *and* binary64 sets **including exp10 and
   pow**; [LLVM libc](https://libc.llvm.org/headers/math/index.html) is correctly-rounded for
   nearly all binary32 and much of binary64 (`exp`, `exp10`, `log`, `log10`, `fma`, ... —
   `pow`/`atan2` still 1 ULP), and is often *faster than glibc* (`tanhf` 13 vs 55 clk). This
   is the honest **E2-lite** route: swap the engine's libm surface (CPU side) for
   correctly-rounded implementations, port the same algorithms as integer-precise GLSL on the
   GPU side, pin add/mul ordering (`NoContraction`, no fast-math on either side) — every
   platform then computes the one defined answer. Caveats: LLVM libc's own GPU builds *disable*
   the correctly-rounded pass by default; our goldens would shift (within oracle tolerance —
   they'd need re-blessing); CPU cost of dropping `-ffast-math` is unmeasured.
5. **The fixed-point precedent (what "we need exactness" industries actually did).**
   H.264/AVC abandoned float DCTs for transforms *"computed exactly in integer arithmetic"*
   precisely because — in the designers' words — *"we cannot guarantee an exact result unless
   we standardize on rounding procedures for intermediate results"*
   ([Malvar et al. 2003](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/MalvarCSVTJuly03.pdf)),
   at a fidelity cost under 0.02 dB PSNR. A fixed-point respec of our 81-band integrals is the
   most robust E2 route — and a research-grade redesign of the engine's numerics.

## 7. Nobody ships what we're being asked about

- **Adobe** (Camera Raw/Lightroom GPU FAQs): no GPU==CPU identity promise anywhere; GPU is
  framed as acceleration; the official diagnostic for wrong colors is *turn the GPU off*.
  Since ACR 14.4 the GPU renders exported files too — and on a runtime error Adobe
  automatically disables/downgrades GPU acceleration for the session (with an error message).
  So yes, Lightroom exports on GPU — *because Adobe's own render is the definition of correct;
  they have no external oracle to match.*
- **darktable** (the closest analog — dual CPU/OpenCL pixelpipes):
  *"Except for some rounding errors, the results are designed to be identical"* — an explicit
  tolerance carve-out, not bit-exactness, plus transparent CPU fallback.
- **PyTorch:** *"results may not be reproducible between CPU and GPU executions, even when
  using identical seeds."* **TensorFlow:** determinism scoped to same hardware + same version.
  **NVIDIA's reproducibility project:** cross-stack reproducibility *"outside the scope"*.
- **IEEE 754-2019 itself** (Clause 11, reproducibility): conditional and optional — and its
  user-side conditions ban exactly what we build with (*"Do not use value-changing
  optimizations"*: reassociation, FMA synthesis — i.e. `-ffast-math`). Per
  [WG21 P3375](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3375r3.html), no C++
  implementation delivers Clause 11 today: *"C++ does not support reproducible programming."*
- **Lockstep games** (the one industry that truly needs cross-machine identity): pin one
  binary/arch, replace libm (Factorio wrote its own trig), or abandon floats for fixed point.

## 8. Case study: hanatos/vkdt — the existence proof (owner pointer)

[vkdt](https://github.com/hanatos/vkdt) is a GPU-only Vulkan/GLSL node-graph raw processor by
Johannes Hanika — the same Hanika whose spectral-upsampling method our `HANATOS2025` LUT
implements. Its **`filmsim` module is, verbatim, "an implementation of Andrea Volpato's
spektrafilm"** — our exact upstream (which reciprocates: *"A blazing fast Vulkan implementation
is available in vkdt by hanatos"*). Findings:

- **Coverage:** essentially the whole negative→enlarger→print→scan chain as ~9 compute
  kernels: spectral exposure via a coefficient LUT + sigmoid-polynomial evaluation
  (Jakob–Hanika family), 3-sublayer density, DIR couplers incl. spatial diffusion (runtime
  `dirlut`), in-emulsion scatter (4 Gaussians) + up to 6 halation bounces, enlarger dichroic
  filters + paper preflash, RA4 print, scan illuminant + veiling glare, and hash-driven
  (PCG3D) particle grain. Shipped in vkdt 1.0.0 (Dec 2025); actively developed.
- **Precision:** inter-pass images default **rgba16f**, shader arithmetic **fp32**, no fp64
  anywhere; spectra are **41 bands at 10 nm** (vs our 81 at 5 nm — the Python oracle's grid).
- **Promises:** none of the kind we need — no bit-exactness, no CPU==GPU claim, no cross-GPU
  determinism claim, no accuracy-vs-Python numbers. The readme says the port was done *"in a
  best effort kind of sense, with some changes for efficiency"*. The 41-band grid alone puts
  it outside our `1e-4` oracle regime by construction.
- **License:** vkdt core is BSD-2, but **the filmsim module is GPLv3** (author's own
  statement) and the profile data CC BY-SA 4.0 — both compatible with our GPLv3 app with
  attribution (which we already carry). **Legally and technically, filmsim's shaders can seed
  our GPU preview tier** — ported into our own Android Vulkan host, since vkdt's Android port
  ([PR #197](https://github.com/hanatos/vkdt/pull/197)) is an unfinished WIP ("compiles but
  lacks functional capability").

**What vkdt proves:** this physics runs interactively on GPU at fp32/f16, in a shipping
product, with reusable GPLv3 code. **What it does not touch:** everything in §1–§7 — it never
claims exactness, and its author never tried to.

## 9. Ranked options

| # | Option | Delivers | Cost / risk | Verdict |
|---|---|---|---|---|
| A | **Status quo policy + fp32 Vulkan preview (vkdt-seeded)** — keep "proxy approximate, export exact" (PERF_ROADMAP, adopted); build the preview tier from [Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127) as fp32 compute, using vkdt filmsim as reference/seed; export stays on the parity-gated CPU engine | The Lightroom experience (fast interactive), zero risk to the parity promise | The port effort we already planned; preview≠export difference bounded by a measured tolerance, not bytes | **Recommended now** |
| B | **GPU export *within oracle tolerance*** (E3) — after A exists, measure fp32-GPU vs oracle on device (`max_abs`, `rms`, plus same-device determinism per Invariance Rule 4/7); if it fits inside `1e-4`/`1e-5` with margin, offer GPU export as an owner decision, with CPU as the always-available exact path and per-device digest tests instead of universal goldens | GPU-speed export on capable devices, still oracle-verified | The deciding number (81-band fp32 accumulation + synthesized exp10 error) is **unmeasured**; CI can't cover every handset — validation becomes on-device self-test; NaN semantics need explicit shader handling | **Measure after A; owner decision** |
| C | **Deterministic-math rewrite (E2-lite)** — correctly-rounded libm (CORE-MATH/LLVM-libc, incl. exp10/log10) + no fast-math + pinned op order on CPU *and* integer-precise ports of the same algorithms in GLSL | One defined byte stream: CPU==GPU==every-arch bit-identical, forever; also fixes today's cross-arch CPU drift | Re-bless all goldens (stays within oracle tolerance); unmeasured CPU cost of dropping `-ffast-math`; large, engine-wide numerics surgery; GPU-side correctly-rounded transcendentals in fp32-integer GLSL is novel engineering | Long-term option; not a ticket |
| D | **Soft-f64 GPU (Mesa float64.glsl route)** — emulate binary64 in integer shaders + bit-exact transcendental ports | True byte-identity with a (non-fast-math) f64 CPU build | ~67k-instruction shaders, ~1/32–1/64 throughput, vendor emulators disclaim conformance, likely **slower than the NEON CPU path** for our compute-bound integrals; mobile driver limits untested | Rejected as impractical |
| E | **Fixed-point respec (H.264 model)** — redefine the integrals in exact integer arithmetic | The most robust exactness money can buy | A numerics research project; redefines the oracle relationship itself | Out of scope; noted for completeness |

## 10. Recommendation for [Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127), and what must be measured first

1. **Keep the adopted law for now**: GPU preview-only; export exact on CPU. Nothing found in
   this research weakens it — everything found (spec, fleet data, industry practice) confirms
   the reasoning behind it.
2. **Build the preview tier from [Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127) as fp32 Vulkan compute with vkdt/filmsim as the seed** —
   the existence proof, the performance ceiling, and GPL-compatible source all say this is the
   fastest safe path to Lightroom-class interactivity.
3. **Then measure E3 on device** (the hardware session is owned by
   [Record the canonical release/R8 export baseline and digest matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/119)): port one integral (scan) to
   fp32 compute, compare against the CPU/oracle on real Adreno/Mali across a param sweep —
   `max_abs`/`rms`, same-device run-to-run stability, NaN handling. If it sits comfortably
   inside the oracle tolerance, **GPU export becomes a legitimate owner decision** (option B)
   — the promise would change from "byte-identical across thread counts" to "oracle-verified
   on your device", which is stronger than anything Adobe or darktable promises.
4. Option C (deterministic math everywhere) is the only true "exact on GPU *and* CPU" — keep
   it on the map's horizon as the thing that would also fix cross-arch CPU drift, but it is a
   quarter-scale project, not a fastpath ticket.

**Open questions carried forward** (from the critique pass): the fp32 81-band error number
(the decider for B); mobile driver honoring of `NoContraction`/float-controls in practice;
whether Adreno 512.863.x fp64 is native or emulation (undocumented); grain/IIR determinism
strategy on GPU (the CPU fork-join's role); Android-fleet coverage of
`shaderRoundingModeRTEFloat32`/float_controls2.

---

*Research for [Research: can the GPU produce exact (byte-identical) results vs the CPU C++ engine?](https://github.com/thetechgeekko/Spektrafilm-android/issues/135), part of
[Wayfinder workstream: 1–2 s exact export + fast interactive preview](https://github.com/thetechgeekko/Spektrafilm-android/issues/117). Film modeling powered by spektrafilm (GPLv3).*
