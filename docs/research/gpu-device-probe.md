# On-device GPU numeric probe — [Research: can the GPU produce exact (byte-identical) results vs the CPU C++ engine?](https://github.com/thetechgeekko/Spektrafilm-android/issues/135), feeding [Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127)

> **Follow-up (2026-08-31):** the probe's positive result now supports an eligible resident
> pointwise filming → printing → scan product route on the frozen Android 16/Adreno artifacts.
> It does not qualify spatial/stochastic stages, other devices/drivers, or a 1–2 second SLO. See the
> current checkpoint in [../BIT_IDENTICAL_EXPORT_ROADMAP.md](../BIT_IDENTICAL_EXPORT_ROADMAP.md).

**Question** ([gpu-bit-exact.md](gpu-bit-exact.md) §10.3, the decider for option B): on real
device hardware, does the fp32 GPU scan integral sit inside the engine's oracle tolerance
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`), and is it same-device deterministic?

**Answer: YES — worst case `max_abs = 2.15e-06` (46× inside the bar), `rms ≈ 7.1e-08`
(141× inside), byte-identical outputs across every repeated dispatch.** And not just
against the shader's own math: with the engine's CAT02 round-trip matrix composed in
(`sweep_mc` below), the GPU is within **≈2.3e-06 of the engine's full linear chain** —
the fp32 error is three orders of magnitude below the tolerance either way.

*Method: `tools/gpu_probe/` — a standalone arm64 NDK executable (no app change, engine
sources byte-untouched) that runs the **unmodified** `spk::gpu::scan_spectral` host +
vendored SPIR-V (`gpu/scan_spectral.comp`) and compares against an f64 CPU reference
compiled without fast-math that mirrors the shader 1:1 (same folded fp32 tables, same op
order — verified down to the SPIR-V disassembly), so the diff isolates **precision**, not
algorithm. Tables extracted through the engine's own loaders/constants
(`profiles/profile.cpp`, `model/color_output`, `model/spectral`) and folded per the
`gpu/vulkan_compute.h` contract; NaN bands zeroed (the engine's `w = NaN → 0` semantics —
20 of 81 Portra bands). An independent adversarial review (3 lenses: table-fold,
reference-fidelity, methodology) confirmed the fold is equivalent to
`runtime/stages/scanning.cpp`'s direct path to ≤1.02e-7 in f64, surfaced the one material
gap — the shader omits the engine's `kRGB_to_RGB_CCTF` (Mc) matrix, whose off-diagonals
matter ~1e-4 near black — and that gap is closed below by the `_mc` cases (Mc·M composed
into the push-constant matrix, an exact linear composition needing no shader change).
Captured 2026-08-27 on the device below, repo commit `bb6c9db`, profile
`kodak_portra_400`, scan route.*

## Device

Samsung SM-S948W (Galaxy S26 Ultra), Android 16, SoC SM8850 — **Adreno (TM) 840**,
Vulkan **1.4.295**, driver **512.842.19** (raw `0x8034a013`), driverID 8
(`VK_DRIVER_ID_QUALCOMM_PROPRIETARY`), build 87ff20b216 / compiler E031.50.19.18
(2026-03-26). Subgroup size 64. Timestamps supported (period 52.08 ns).

## Tier 1 — fp32 GPU vs f64 CPU reference

| case | npix | max_abs | rms | det ×5 | notes |
|---|---:|---:|---:|---|---|
| golden (`scan_portra` density plane) | 4,096 | **4.74e-07** | **5.98e-08** | IDENTICAL | worst px cmy=(0.125, 1.057, 0.661), comp G |
| golden_mc (matrix = Mc·M, engine chain) | 4,096 | **4.70e-07** | **5.95e-08** | IDENTICAL | composing the CAT02 round-trip costs nothing in precision |
| sweep (64³ lattice, −0.1..nanmax(density_curves) per ch) | 262,144 | **2.15e-06** | **7.07e-08** | IDENTICAL | worst px at the negative-density edge, cmy=(−0.1, 0.957, 0.005), comp G |
| sweep_mc (same lattice, matrix = Mc·M) | 262,144 | **2.11e-06** | **7.07e-08** | IDENTICAL | same worst pixel |
| NaN density | 3 | — | — | — | GPU emits **(0,0,0)** for NaN inputs (Adreno's `clamp(NaN,0,1)` → 0); engine semantics for NaN density = black — behaviourally aligned **on this driver**, but GLSL leaves `clamp(NaN)` undefined, so any future GPU-export path needs an explicit NaN guard, not driver luck |

Tolerance bar: `max_abs ≤ 1e-4`, `rms ≤ 1e-5`. The worst case is **46×** inside on
`max_abs`, **141×** on `rms`. Errors concentrate in dark output values where the sRGB
CCTF slope is steep — exactly where fp32 `pow(10,-D)` + the synthesized
`exp2(y·log2(x))` ULP bounds land. The sweep's lower bound covers the engine's
negative-density scan domain (`-grain_density_min`).

**GPU-vs-engine chain**: the review measured the f64 mirror (with Mc composed) against
the engine's actual `scanning.cpp` semantics at ≤1.02e-7 (the residual is the disclosed
`log10/pow10` 1e-10 floor, ~1.6e-9, plus fp32 table quantization — the shader's declared
contract). Chained with `sweep_mc`, the GPU result is **≤ ≈2.3e-06 from the CPU engine's
default output path** (BW/glare corrections off, as in the goldens).

**Determinism**: 5 identical dispatches byte-compare equal on every case, rerun buffers
poisoned beforehand (Vulkan Invariance Rule 7 verified, not assumed); the five 12 MP
perf runs hash identically too.

## Tier 0 — float-controls facts from the GPU exactness research

- `shaderFloat64` = **false** — confirms the fleet expectation on Adreno 8xx stock
  Samsung driver 512.842.19; the 512.863.x fp64=true reports in
  [Research: can the GPU produce exact (byte-identical) results vs the CPU C++ engine?](https://github.com/thetechgeekko/Spektrafilm-android/issues/135) do
  not apply to this branch.
- `shaderFloat16` = true, `storageBuffer16BitAccess` = true
  (`uniformAndStorageBuffer16BitAccess` = false).
- fp32: `shaderRoundingModeRTEFloat32` = **true**, `signedZeroInfNanPreserveFloat32` =
  **true**, `denormFlushToZeroFloat32` = true (denormPreserve false) — the driver
  *advertises* the float_controls facilities the E2/option-C route would need.
- fp16: RTE true, NaN/Inf preserve true, denormPreserve true.
- fp64: all controls false (consistent with no fp64 at all).
- Independence: denorm and rounding-mode both `..._INDEPENDENCE_ALL`.

## Tier 2 — perf sanity (preview-decision context only)

| size | warm-call median | throughput |
|---|---:|---:|
| 0.3 MP (640×480) | 48.3 ms (24.8 ms in an earlier, cooler run) | 6–12 MPix/s |
| 12 MP (4000×3000) | 158.3 ms (101.5 ms earlier run) | 76–118 MPix/s |

Warm-call wall time of the **current host as-is**, which re-creates buffers + pipeline
and round-trips host-visible memory every call — honest *offload* cost, not kernel time
(the 0.3 MP call is almost entirely fixed overhead: ~48 ms vs ~158 ms for 39× the
pixels). Run-to-run spread across sessions is thermal/clock state; within a session the
5-run spread was ≤20%. A persistent-pipeline host would cut the small sizes hard. No
export implications — GPU stays preview-only.

## Tier 3 — precision brackets (same Tier 1 run, recompiled shader)

| variant | sweep max_abs | verdict |
|---|---:|---|
| `precise` (NoContraction on the band accumulators; 12 decorations verified in the SPIR-V) | 2.15e-06 | **outputs byte-identical to the default compile** — the driver's default codegen for this kernel already matches the NoContraction result |
| `mediump` (RelaxedPrecision everywhere — 99 decorations; driver evaluates fp16) | 1.24e-02 | **~124× OUTSIDE tolerance** — fp16 arithmetic (vkdt's floor) fails the oracle regime, as predicted; fine for a proxy preview, unusable for oracle-verified work |

## What this means

1. **The E3 bar is met on this device, against the engine's own chain**: fp32 GPU scan
   is oracle-tolerance-accurate with ~50× margin and same-device deterministic. Per
   [gpu-bit-exact.md](gpu-bit-exact.md) §10.3 / option B, **GPU export
   ("oracle-verified on your device") is now a legitimate owner decision** — the
   standing law (GPU preview-only) is untouched until the owner makes it; this probe
   wires nothing into the app.
2. For [Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127), fp32 Vulkan compute is numerically over-qualified —
   even fp16 (1e-2-class error) is visually plausible for a 640 px proxy, and the fp32
   kernel moves ~100 MPix/s through a deliberately naive per-call host.
3. If the scan kernel ever feeds anything engine-facing, push **Mc·M** as the matrix
   (exact composition, no shader change) — the raw `kXYZ_to_RGB` alone differs from the
   engine's default output path by up to ~1.5e-4 near black (the Mc off-diagonals ×
   the 12.92 CCTF slope), which *would* breach the tolerance.
4. Caveats bounding the claim: one device, one driver (512.842.19), BW/glare
   corrections and the spatial branch not in scope, and GPU NaN handling is
   driver behaviour, not spec — guard it explicitly before any export-path use.
   ~~One kernel (the scan integral)~~ — closed by the M2 measurement below
   ([GPU M2: E3 device measurement — filming + printing integrals (probe extension; laptop session order)](https://github.com/thetechgeekko/Spektrafilm-android/issues/147)):
   the filming and printing integrals are now measured on the same device and
   sit inside the bar too, so **all three per-pixel pipeline integrals are
   oracle-tolerance-accurate in fp32 on this device**.

---

# M2 — [GPU M2: E3 device measurement — filming + printing integrals (probe extension; laptop session order)](https://github.com/thetechgeekko/Spektrafilm-android/issues/147)

*Same device and driver as above (SM-S948W / Adreno 840 / 512.842.19, re-verified in
this run's `caps` capture). Captured 2026-08-27, repo commit `8d50c9e` + the
`tools/gpu_probe/` M2 extension. Method: probe-local fp32 shaders `filming.comp` /
`printing.comp` (engine sources byte-untouched) vs f64 CPU mirrors compiled without
fast-math that replicate each shader 1:1 on the same fp32 tables. Tables are folded on
the host through the engine's OWN builders (`build_filming_tc_lut`,
`normalize_density_curves`, `compute_dir_couplers_matrix` + `np_interp_array`,
`digest_printing_params` + `resolve_neutral_cc` + `compute_midgray_exposure_factor`),
and — new over M1 — every run executes the REAL engine stage on-device and checks the
fold in-binary: `CHAIN` (f64 mirror vs engine) and `SETUP` (engine vs committed golden,
which must PASS the parity bar) replace M1's offline adversarial fold review. Profile
`kodak_portra_400`; print paper `kodak_portra_endura`, natively-digested neutral CC
(0, 51.43, 55.26) and midgray factor 0.8531.*

## Filming — ProPhoto→tc/b → Mitchell-cubic tc_lut → log10 → density curves → DIR couplers

The full pointwise filming chain (`expose` fused path + `develop` pointwise, the
goldens' regime) in one fp32 kernel: 3×3 matrix + chromaticity clamp + tri2quad, the
16-tap Mitchell-Netravali cubic over the 192×192×3 tc_lut, `log10(max(raw,0)+1e-10)`,
the fp32 density-curve interp (n=256), and the pointwise DIR-coupler correction
(silver@M + re-interpolation on the pre-DIR curves) — the two "likely suspect"
interpolation sub-ops included.

| case | npix | max_abs vs f64 | rms | det ×5 | notes |
|---|---:|---:|---:|---|---|
| golden (`scan_portra` input) | 4,096 | **1.22e-06** | **1.03e-07** | IDENTICAL | vs committed golden: 1.28e-06 / 1.18e-07 — **78× / 85× inside** the bar |
| sweep (64³ RGB lattice, −0.05..2.0 per ch) | 262,144 | **6.46e-06** | **1.33e-07** | IDENTICAL | worst at the high-red LUT corner; vs engine 6.26e-06 — **15× inside** |
| NaN input | 3 | — | — | — | GPU emits **(0,0,0)**, byte-matching the f64 mirror (the `b = NaN→0` guard + the probe shader's bounded cubic index make NaN input well-defined; the ENGINE's own cubic has no defined NaN-input semantics) |

`CHAIN` (f64 mirror vs engine, golden input): max_abs **2.46e-07** — the fold (fp32
tables + fp32 input cast) costs under 3e-7 against the engine's f64 chain. `SETUP`
(engine vs golden): 2.38e-07, PASS.

## Printing — film CMY → 81-band dichroic-filtered integral → midgray → paper curves

`print_expose`'s direct spectral path (81-band `10^-(cmy·dye)` against a folded
`10^-base × filtered-illuminant × print-sensitivity` table, midgray factor, the
verbatim `10^lr → log10` round trip) + `print_develop`'s paper density-curve interp
(n=256), one fp32 kernel. 20 of 81 bands NaN-nulled (same fold rule as the M1 scan).

| case | npix | max_abs vs f64 | rms | det ×5 | notes |
|---|---:|---:|---:|---|---|
| golden (`print_portra` film density) | 4,096 | **5.92e-07** | **9.79e-08** | IDENTICAL | vs committed golden: 5.96e-07 / 9.64e-08 — **168× / 104× inside** the bar |
| sweep (64³ CMY lattice, −0.1..nanmax per ch) | 262,144 | **7.91e-07** | **9.11e-08** | IDENTICAL | vs engine 8.34e-07 — **120× inside** |
| NaN density | 3 | — | — | — | GPU output **byte-matches the engine's defined semantics** (light = NaN→0 ⇒ near-black paper base) to the last printed digit — again via Adreno's `max(NaN,0)→0`, driver behaviour, not spec |

`CHAIN`: max_abs **1.62e-07**. `SETUP`: 2.38e-07, PASS.

## M2 precision brackets (same runs, recompiled shaders)

| variant | filming sweep max_abs | printing sweep max_abs | verdict |
|---|---:|---:|---|
| `precise` (NoContraction on the main accumulators) | 6.46e-06 | 7.91e-07 | **byte-identical outputs to the default compile** on both kernels — same as the M1 scan finding |
| `mediump` (RelaxedPrecision; driver evaluates fp16) | 6.09e-02 | 5.01e-03 | **~600× / ~50× OUTSIDE tolerance** — fp16 fails the oracle regime on both kernels (still deterministic ×5); proxy-preview-only material |

## What M2 adds to the picture

1. **Every per-pixel integral of the pipeline (filming, printing, scan) is now
   measured on-device and sits inside the oracle bar in fp32 with ≥15× margin**,
   deterministic across repeated dispatches. The M2 measurement contingency (“if a kernel lands
   outside, cut M3/M4 scope to the ones that pass”) is moot — nothing landed outside.
2. The feared sub-ops — the density-curve LUT lookups and the DIR-coupler
   interpolation — cost nothing measurable beyond the pure exp10 integrals: filming's
   worst case (6.5e-06) is dominated by the cubic-LUT/chromaticity path at a sweep
   corner, not the interps, and printing (which shares the scan's op class plus the
   curve interp) is actually *tighter* than the scan sweep.
3. The in-binary `CHAIN`/`SETUP` checks pin the folds to ≤2.5e-07 of the real engine
   and prove the probe's digested parameters reproduce the committed goldens — the
   M1-style fold-equivalence argument is now a measured number, not a review.
4. Spatial branches (halation, diffusion, grain), preflash, morph and the enlarger
   LUT remain out of scope, as does every non-Adreno-840 device.

*Probe: `tools/gpu_probe/` (`build_push_run.sh` reproduces everything; raw captures in
`tools/gpu_probe/captures/`, untracked). Research for
[Decide: GPU preview route](https://github.com/thetechgeekko/Spektrafilm-android/issues/127),
[Research: can the GPU produce exact (byte-identical) results vs the CPU C++ engine?](https://github.com/thetechgeekko/Spektrafilm-android/issues/135), and
[GPU M2: E3 device measurement — filming + printing integrals (probe extension; laptop session order)](https://github.com/thetechgeekko/Spektrafilm-android/issues/147), part of
[Wayfinder workstream: 1–2 s exact export + fast interactive preview](https://github.com/thetechgeekko/Spektrafilm-android/issues/117). Film modeling
powered by spektrafilm (GPLv3).*
