# Experiment: Highway SIMD + Halide codegen on the engine's spatial kernels

*Branch `claude/perf-halide-highway` — an owner-commissioned experiment ("implement both,
we'll test at home"), NOT a merge candidate on its own. Everything here is opt-in and the
default build is byte-identical to `main`.*

## Why these two, and where each one actually lands

The engine is **compute-bound ~100× past the roofline knee** (`docs/EXPORT_FASTPATH.md`:
`scan()` spends ~2900 cycles/pixel to consume 24 bytes; streaming is ~3% of a 12 MP render's
wall clock). That single fact decides the shape of both experiments: **fusion and tiling — the
lever Halide is famous for — buy footprint, not speed**, on the spectral stages. What is left
is instruction-level: more lanes per cycle, better schedules on the few genuinely *stencil*
kernels. So each tool was pointed at the kernel where it can actually win, and then measured.

The result is a two-line map that matters more than either speedup:

| Experiment | Kernel it accelerates | Who calls it | Default state |
|---|---|---|---|
| **Highway** f32 lanes | `kernels/gaussian.cpp` separable FIR | `model/grain.cpp` ×4 (dye clouds, clumping, micro-structure, sublayers), `model/glare.cpp` | **grain is ON in interactive previews** |
| **Halide** codegen | the `ks×ks` PSF convolution in `model/diffusion.cpp::apply_diffusion_filter_um` | camera / enlarger diffusion filter (Black Pro-Mist) | **OFF by default** (`camera_diffusion_active = 0`) |

**Highway lands on the default preview path; Halide lands on an opt-in effect.** That is the
headline, and it inverts the intuitive ranking (Halide's speedup is the bigger number).

## Highway — f32 separable FIR

Vendored per `docs/research/highway-vendoring.md`: release **1.4.0**, CMake `FetchContent` with
`URL_HASH SHA256` (the pin was re-verified in this session — the downloaded asset is
**3,691,557 bytes**, SHA256 `36f672ab…ababdc`, matching the doc's measurement byte-for-byte),
`HWY_COMPILE_ONLY_STATIC` + `HWY_DISABLE_PCLMUL_AES`, tests/examples/contrib/install OFF.
`kernels/gaussian_hwy.cpp` `static_assert`s the resolved per-ABI target
(`HWY_NEON_WITHOUT_AES` on ARM, `HWY_SSE4` on x86_64) so a toolchain change that moves the
baseline fails the **build**, not the goldens.

**Bit-identical, not approximate.** Both ported routines vectorise *across output pixels*: each
output keeps the scalar accumulation order over the kernel taps, and neither uses a fused
multiply-add (plain `Mul` then `Add`). `tests/test_gaussian_hwy.cpp` asserts byte equality
across widths that exercise the vector body, the scalar tail and the shorter-than-one-vector
case, and across four kernel radii — **all byte-identical** (host, this session).

Host throughput (x86_64, 4096-wide rows, radius 6 → 13 taps). This container is shared and
noisy, so the honest report is a **range over repeated runs**, not one number:

| pass | speedup, repo baseline flags (target `SSE2`, 4×f32 — the closest host proxy for arm64 NEON) |
|---|---|
| horizontal FIR | **~2.1× – 2.7×** (4 runs: 2.13, 2.30, 2.11, 2.73) |
| vertical accumulate | ~1.3× (1.28, 1.40) |

The horizontal pass is where the work is (`taps` multiplies per output vs one); the vertical
accumulate is memory-bound and gains little, as expected.

**A caveat worth carrying forward: wider is not automatically faster.** Rebuilt with
`-march=native`, Highway resolved to `AVX3` (512-bit) and the two passes moved in *opposite*
directions — horizontal **5.96×**, but vertical **0.60×, i.e. a regression** versus the same
scalar code. Android pins the ARM target at `HWY_NEON_WITHOUT_AES` (128-bit, 4×f32), so the
shipped ABI sits near the `SSE2` row above rather than the AVX3 one — but the result is a
reminder that a lane-width change must be *measured per pass*, not assumed.

A single binary can A/B both paths: **`SPK_SIMD=0` (or `off`/`scalar`) disables the lanes at
runtime**, read once, so the phone can be measured without a rebuild.

## Halide — the diffusion PSF convolution

`tools/halide/gen_diffusion_conv.py` reproduces the engine's convolution verbatim in shape
(`out[y][x] = Σᵢⱼ padded[y+i][x+j] · kern[ks-1-i][ks-1-j]`, float64, flipped-kernel indexing),
leaving the reflect-padding in C++ so the generator owns exactly the O(w·h·ks²) inner loop.
AOT-compiles for `host` **and `arm-64-android`** (verified: the emitted object is a real AArch64
ELF), so the same pipeline can be benched on the phone.

**No algorithmic shortcut exists here.** The PSF is a sum of `exp(-r/λ)` terms — radially
symmetric but **not separable**, so a separable rewrite would change the rendered look, not just
the schedule. Halide's win is therefore pure scheduling (vectorise x, parallel rows, tap loops
as the serial inner dimensions).

Host numbers, and this is where the framing matters:

| case | scalar (`-march=native`, serial) | Halide serial | speedup | max_abs |
|---|---:|---:|---|---:|
| 640×480 ks=9 | 19.2 ms | 10.8 ms | **1.79×** | 0 |
| 640×480 ks=17 | 94.2 ms | 35.9 ms | **2.63×** | 0 |
| 640×480 ks=33 | 393.8 ms | 132.6 ms | **2.97×** | 0 |
| 1024×768 ks=17 | 240.3 ms | 95.2 ms | **2.53×** | 0 |

*The honest comparison is the one above — both serial, both allowed the host's full ISA.* A
naive run (Halide's own `parallel(y)` against a serial reference, reference at the repo's
default baseline) reads **6.4–10.9×**, but the engine already parallelises rows around the
scalar convolution, so most of that gap is threading the engine has too. **~2–3× is the real
scheduling win.**

`max_abs = 0` on this host/flag combination is a measurement, **not a guarantee**: vectorising
a reduction reassociates the sum, so the Halide path is an opt-in experiment held to a
tolerance, never a drop-in for the parity path. (Even the reassociated worst case measured at
the repo default was `4.4e-16` — twelve orders below the 1e-4 oracle bar.)

## Halide's other backends (owner asked: HVX, Vulkan, CPU)

The same generator source was compiled against four targets — all four **built**, but building
is not offloading, so each was checked further.

**Vulkan — real, and strategically the interesting one.** Halide's Vulkan backend genuinely
emits SPIR-V: a GPU-*scheduled* variant (`gpu_tile`) cross-compiled to `arm-64-android-vulkan`
produces an artifact containing **2 SPIR-V magic numbers** (verified by scanning the emitted
`.a` for `0x07230203`) — i.e. actual compute kernels, from the same source that produces the
CPU path. For **#148 (full-chain GPU preview)** that is a genuine architectural option: write
filming/printing/scan **once** and get CPU + GPU, instead of hand-writing GLSL plus maintaining
a parallel CPU implementation. Two caveats keep it an option rather than a plan:

- **f32 only on this phone.** `doc/Vulkan.md` exposes a `vk_float64` feature, but it needs
  device `shaderFloat64` — and our own device probe measured **`shaderFloat64 = false` on the
  S26 Ultra's Adreno 840** (PR #145). Our diffusion convolution is f64, so a Halide→Vulkan port
  of it must drop to f32; that is the same precision question the GPU line already answered by
  measurement for the scan integral (2.15e-06, comfortably inside tolerance), and it is
  **unmeasured** for this kernel.
- **Not perf-tuned yet.** Halide's own Vulkan doc lists "Performance tuning of CodeGen and
  Runtime" under *Known TODO*. Our hand-written Vulkan scan kernel is already device-validated,
  so Halide-Vulkan is a **maintainability** argument first, a speed one only if measured to be.

**HVX (Hexagon DSP) — a dead end for this engine.** Three independent reasons, not one:
Halide's own HVX example apps (`apps/hexagon_benchmarks`, `apps/hexagon_dma`) are uniformly
`uint8_t` — HVX is an integer/fixed-point vector engine (f32 has only partial codegen support,
**f64 none**), while our math is f64 spectral; it requires the **proprietary Qualcomm Hexagon
SDK + Hexagon Tools** (`doc/Hexagon.md`), which is a heavy, licence-bound build dependency; and
DSP access on retail handsets goes through FastRPC sessions that normally require Qualcomm
**signing**. The DSP is built for 8-bit imaging and ML, not 81-band float integrals.

**CPU — measured above** (~2–3× on the convolution, the honest serial-vs-serial figure).

Worth mining if this goes further: `apps/gaussian_blur`, `apps/blur` and `apps/iir_blur` are
direct analogues of our own FIR/IIR Gaussian kernels (schedules to copy rather than invent), and
`apps/HelloAndroid` is the reference Android integration.

## What this predicts, and what to measure at home

`bash tools/simd_bench/build_push_run.sh` (owner's laptop; needs the NDK + an attached device)
cross-compiles both experiments for arm64, pushes them, and prints the same tables **from the
phone** — including the `SPK_SIMD=0` A/B and the serial/parallel Halide framings.

Read the result against the per-stage timings that now land in logcat (#146/#152):

- If **grain** is the big number on device → Highway is the lever, and it is *already*
  bit-identical, so it can land on the default path with the 38-gate suite as the only gate.
- If **camera diffusion** is enabled and big → Halide is worth wiring, opt-in, at ~2–3×.
- If **halation** is the big number → **neither of these touches it.** Halation runs the *f64*
  exponential/Gaussian mixture (`kernels/exponential_filter.cpp`), where Highway gives two f64
  lanes on arm64 — exactly what `kernels/exp10.h` already delivers — and which this experiment
  did not target. That would be the next Halide candidate, not a Highway one.

## State of this branch

- Default build unchanged: `SPK_ENABLE_HIGHWAY=OFF` → `gaussian_hwy.cpp` compiles to three
  inert stubs, `hwy_fir::available()` is false, the scalar branch runs. The **38-gate parity
  suite is the gate** and is green on this tree.
- Halide is not wired into the engine at all — it lives in `tools/halide/` as a generator plus
  an A/B bench. Wiring it would be a separate, opt-in decision once the device numbers justify
  it.

*Film modeling powered by spektrafilm (GPLv3).*
