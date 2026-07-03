# Export performance — measured on-device (2026-07-02)

**Status:** item 1 (grain) **IMPLEMENTED + measured**; items 2–5 still planned. All wins below are
bit-exact (byte-identical to today's output, or pixel-identical for the PNG writer) unless flagged.

> **UPDATE 2026-07-02 — grain parallelized (item 1 done).** The 9 (channel×sublayer) RNG streams
> now run on a coarse task-parallel helper (`kernels/parallel.h` `parallel_tasks`), each into a
> private plane, accumulated serially in canonical order → **byte-identical** to the old serial
> grain (proven three ways: `test_grain_parallel` 1-vs-8 on both paths, full `test_parallel`
> grain+halation scan+print 1-vs-8, and a direct new-vs-old serial-output `cmp` = 0). Measured
> on-device (SM-S948W, 12 MP, 8 threads): **grain 67 s → 32.8 s (~2.0×)**, **export 74.5 s → 40.0 s
> (~1.86×)**. This is the **ceil(9/8) = 2-wave** ceiling — the "~8.5 s / ~6×" projection below was
> optimistic (it assumed near-linear 8× across 9 unequal streams on 8 heterogeneous, thermally
> limited cores). Same-session 1-vs-8: 76.7 s → 32.8 s = 2.34×. Beating ~2× bit-exactly needs
> per-pixel counter-RNG (changes the grain realization → golden/stat regen + product sign-off),
> which is out of scope (see item 1's parity note). Strike-through numbers below are the pre-impl
> projections, kept for context.

**Device:** Galaxy S24-class **SM-S948W** (serial `R5GL13Z3S6L`), Android 16 / API 36, arm64-v8a,
8 cores. Bench tool: `engine/spektra-core/src/main/cpp/tests/bench_export.cpp` (build/run recipe in
its header). This satisfies `PERF_ROADMAP.md`'s "profile on a real arm64 device" caveat.

## The headline

A **12 MP full-res export takes ~74.5 s** on-device, and **grain is 90 % of it (~67 s)**. Everything
else combined is < 10 s. Grain parallelizes **byte-identically** (9 independent RNG streams). ~~grain
alone: 67 s → ~8.5 s, export 74.5 s → ~15 s (~5×); full set → ~11–13 s (~6×)~~ — **MEASURED**: the 9
streams on 8 cores are a **2-wave** job, so grain **67 s → 32.8 s (~2.0×)** and export **74.5 s →
40.0 s (~1.86×)**, byte-identical, zero visual change (item 1 done; see the UPDATE banner). Items 2–5
below (blurs, PNG, copy) would take the remaining ~7 s of non-grain serial cost down further.

Note this **corrects** the old `PERF_ROADMAP.md` framing ("12 MP proxy ≈ 15 s; the scan integrals
are the bottleneck; CPU can't help"): for *export*, grain — not the spectral integrals — dominates,
and it is CPU-parallelizable bit-exactly. (That doc's numbers were host-x86 and pre-grain.)

## Measured baseline (cold, 12.2 MP = 4032×3024, SPK_NUM_THREADS=8)

| phase | time | notes |
|---|---:|---|
| **FULL export (all effects ON)** | **74 509 ms** | scan route; default preset (Portra) |
| — of which **grain** | **~67 083 ms (90 %)** | `FULL − (FULL minus-grain)` |
| — everything else (spectral+blurs+scan) | 7 426 ms | already parallel + NEON |
| PNG16 write (zlib level-6, serial) | 2 151 ms | +64.9 MB out |
| TIFF16 uncompressed | 23 ms | +69.8 MB out — effectively free |
| TIFF16 PackBits | 127 ms | +69.8 MB |
| float32 → uint16 quantize (serial) | 5 ms | trivial |

Isolation matrix at 0.3 MP (Δ vs pointwise floor, AE on): halation **+58 ms**, DIR diffusion
**+49 ms**, scanner unsharp ~0, **camera diffusion +8482 ms** (pathological, default-OFF), grain
**+2161 ms**. (AE-off drives synthetic densities to extremes and makes grain look 15× worse — the
bench forces `auto_exposure=1` for representativeness.)

## Root causes + the fix (ranked by 12 MP impact, all bit-exact)

### 1. GRAIN — 67 s (90 %) → 32.8 s. Parallelize the 9 (channel×sublayer) streams. ✅ DONE, byte-identical
- Grain's RNG is a **single sequential `std::mt19937` per `layer_particle_model` call**, consumed in
  raster order with **data-dependent draw counts** (`grain.cpp:44,53,54`; `stats.cpp:29-32,50-52`).
  So parallelizing *across pixels within a call* is **NOT** bit-exact (verdict (b)) — and `parallel.h:20-21`
  explicitly forbids it.
- **BUT** the default (sublayers-active) path loops `for c(3): for sl(3)` = **9 independent calls**,
  each with its own seed `seed_base[c] + sl*10 + seed_offset` and a fresh RNG
  (`grain.cpp:181-197`, `apply_grain_to_density_layers`). Nine independent streams ≥ 8 cores.
- **Fix (implemented):** the 9 calls run on `parallel_tasks` (a new coarse task-parallel helper in
  `kernels/parallel.h` — dynamic atomic-counter scheduling, NO min-chunk gate, unlike `parallel_for`
  which is per-pixel and would run 9 items serially), each into a **private plane** (`shifted` is now
  task-local; the 9 outputs land in one `planes` buffer indexed `k=c*3+sl`), then **accumulated
  serially in canonical (c, sl=0,1,2) order** (float add-order matters; sublayers of a channel sum
  into the same `acc[i*3+c]` slot). Byte-identical to the serial code for any thread count. The
  non-sublayer path (`grain.cpp`, `apply_grain_to_density`, default n_sub=1) got the same treatment
  (3×n_sub streams). **Measured: grain 67 s → 32.8 s (~2.0×) at 12 MP/8 threads** — NOT the ~8× the
  "9 streams saturate 8 cores" wording implied: 9 unequal streams on 8 heterogeneous, thermally
  limited cores is a 2-wave (`ceil(9/8)`) job, so ~2× is the bit-exact ceiling for this approach.
- **Parity note:** grain is only *statistically* matched to the oracle (C++ `mt19937` ≠ numpy/Numba;
  headers `grain.cpp:14-18`, `stats.h:15-16`) and is **excluded from the byte-exact goldens**
  (`bench_stages.cpp:12-16`); `test_grain*.cpp` are statistical and **not** in CI. Since this fix
  keeps the C++ output byte-identical, those tests are unaffected. A more aggressive per-pixel reseed
  (counter/PCG) would unlock >9× and cheaper draws but **changes the grain realization** (still
  statistically equal + thread-invariant) → needs golden/stat regen + a product call. **Not needed**
  — the 9-way path already saturates 8 cores.

### 2. Halation + DIR-diffusion separable blurs — ~5 s. Parallelize rows/cols. ✅ byte-identical
- `kernels/gaussian.cpp` and `model/diffusion.cpp` have **no `parallel_for`** — every spatial blur is
  serial. Separable filters compute each output row (H pass) / column (V pass) independently, so
  distributing rows/cols across threads never reorders any per-pixel tap-sum → **bit-exact by
  construction** (research: independent output elements, never split a single reduction). These ARE
  in the byte-exact goldens (`diffusion_e2e`, spatial gates) — row/col parallelism keeps them green.
- ~5 s → ~0.7 s on 8 cores. (Also parallelizes the final grain Gaussian + scatter tails.)

### 3. PNG16 writer — 2.1 s. libdeflate (or parallel zlib). ✅ decoded pixels identical
- `png_writer.cpp:120` does one serial `compress2` at zlib level 6. **libdeflate (MIT, GPLv3-OK,
  builds on NDK r27, NEON)** is ~2× at the same ratio → ~1 s; pigz-style per-band parallel deflate
  (independent blocks + multiple IDAT chunks) → ~0.3 s. Output file bytes differ but **decode to
  identical pixels** (lossless). `fpnge` is disqualified (x86-only, hard `#error`, no NEON);
  `fpng` disqualified (8-bit only). TIFF16 uncompressed is already ~free (23 ms) — no change needed
  unless small files matter (then libtiff + ADOBE_DEFLATE + PREDICTOR=2 per-strip parallel).
- Only matters for PNG export; TIFF export is unaffected.

### 4. Camera / enlarger diffusion — 8.5 s *when enabled* (default-OFF). FFT restore + parallel. ✅
- `apply_diffusion_filter_um` (`diffusion.cpp:437-450`) is a **non-separable direct 2-D convolution
  O(n·ks²)** with a **kernel radius that scales with image size** (`8×bloom_max_lambda_px`, default
  Black Pro-Mist 950 µm, capped only at `min(h,w)/2−1`; `diffusion.cpp:336-344`) — an
  **FFT→direct-convolution regression** vs the oracle's `scipy.signal.fftconvolve` (`diffusion.h:22-27`).
  The isotropic-exponential PSF is genuinely non-separable, so the real fix is **restore FFT
  (overlap-add) convolution** (O(n log n), bit-exact to tolerance). Cheap interim win:
  `parallel_for` across output rows + channels (trivially bit-exact) — ~8× constant-factor.
- Enlarger diffusion calls the **same function** (`printing.cpp:288-290`) — one fix covers both.
  Default-OFF, so no default-export benefit; matters for diffusion-filter ("Pro-Mist") presets.

### 5. Double full-res memcpy — ~0.1 s. Single owned buffer. ✅ identical
- `fill_out_image` (`spektra.cpp:1493`) then the JNI copy (`spektra_jni.cpp:641`) each memcpy the
  ~140 MB result. Collapse to one hand-off (write once into the owned direct ByteBuffer / move
  semantics). Small time win but halves peak memory on the export path.

## Recommended sequence

**Grain first** (item 1) — ✅ **DONE**: 34 s off the 67 s problem, byte-identical, guarded by
`test_grain_parallel` + `test_parallel` (both 1≡8) and CI. Export is now **~40 s**. Remaining budget:
the ~7 s of non-grain serial cost (blurs, PNG, copy). Next: items 2 (blurs) and 5 (copy) as a batch
(~5 s → ~0.7 s), then 3 (PNG libdeflate, only for PNG export ~2 s → ~1 s), then 4 (diffusion) for
Pro-Mist presets.

Post-grain default-preset export (TIFF): **~40 s** measured. With items 2+5 done: **~34 s** projected
(grain 32.8 s now dominates; the parallel-grain 2-wave ceiling, not the serial tail, is the floor for
this bit-exact approach). PNG adds ~2 s (→ ~1 s with libdeflate). Every step keeps the CI
`engine-parity` gate green and stays thread-count-invariant. To push grain below ~2× you must switch
to per-pixel counter-based RNG (>8× and cheaper draws, thread-invariant, but a **different grain
realization** → regen the statistical goldens + a product call). That is the only remaining lever on
the 90 % that grain represents.

## Research sources (2026-07-02 web research)

- **Encoders:** libdeflate (MIT, ~2× zlib-6, NEON) https://github.com/ebiggers/libdeflate ·
  zlib-ng https://github.com/zlib-ng/zlib-ng · fpnge x86-only https://github.com/veluca93/fpnge ·
  parallel PNG/IDAT https://github.com/w3c/png/issues/54 · https://zlib.net/pigz/pigz.pdf
- **Bit-exact parallel reduction / fast-math traps:** https://hal.science/hal-00949355v4/document ·
  https://simonbyrne.github.io/notes/fastmath/ · separable convolution
  https://en.wikipedia.org/wiki/Gaussian_blur (IIR/box/running-sum Gaussians are approximations —
  disqualified).
- **NEON float→uint16 (exact):** `vmulq_f32` → `vcvtnq_u32_f32` (round-nearest-even+saturate) →
  `vqmovn_u32`. https://arm-software.github.io/acle/neon_intrinsics/advsimd.html

*Film modeling powered by spektrafilm (GPLv3).*
