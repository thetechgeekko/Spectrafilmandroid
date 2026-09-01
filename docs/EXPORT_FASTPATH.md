# Export fast path — maximum quality, minimum time

> **Current plan (2026-08-29):** [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md)
> supersedes this file for the 1–2 second SLO, current measurements and library choices. This file
> remains the engineering history of landed/rejected fast-path ideas. “Bit-identical” must now be
> qualified as engine samples, decoded samples/metadata, or complete container bytes; Fast GPU is a
> separate oracle-equivalent route, not the strict exact route.
> The repository authority order and live frontier are in [EXECUTION_INDEX.md](EXECUTION_INDEX.md).

Mandate: make export as fast as possible **without ever trading quality**. Speed comes from
better engineering, not from computing less. Every item below is either provably bit-exact,
explicitly flagged as not, or rejected.

Findings marked **[measured]** were benchmarked on the host during analysis. Findings marked
**[needs device]** are structural expectations that require real arm64 hardware to confirm —
they are not claims until measured.

---

## Phase 0 — the referee (nothing else lands first)

### The thread-invariance gate was vacuous — FIXED in this change

`test_parallel` is the CI gate that proves the engine is byte-identical across thread counts.
It was not testing that.

- `kParallelMinChunk = 8192`; `parallel_for` computes `max_by_work = ceil(count / min_chunk)`
  and clamps the worker count to it.
- The fixture is 64×64 = **4096 pixels** → `max_by_work = 1` → `nthreads` clamps to 1 → the
  **serial path** runs at *every* thread count.
- **[measured]** With the real fixture size: default gives **1 chunk at 1 thread and 1 chunk at
  8 threads**. The 1-vs-8 comparison was serial-vs-serial and could not fail.

Eight of the eleven `parallel_for` call sites were therefore unprotected by the gate that exists
to protect them.

**Fix (in this change):** `parallel_min_chunk()` honours a `SPK_PARALLEL_MIN_CHUNK` override
(mirroring how `SPK_NUM_THREADS` already works), and `test_parallel` sets it so the fixture
splits for real. **[measured]** after the fix: 8 threads → **8 chunks**, results still correct.
Unset in production, so the shipping path and every golden are byte-identical to before.

> **Run the current 39-case `engine-parity` suite before merging.** The workflow is the authority;
> do not trust a prose count if its `build_run` table changes. The toolchain and assets to run it
> were not available where this change was written; the default-inert argument is sound but the
> suite is the authority.

### Why this blocks everything else

Three separate optimization designs proposed rewriting `parallel_for` in **mutually
incompatible** ways, and all three cited `test_parallel` as their safety proof. Until the gate
is real, no chunking change can be trusted — including the ones recommended below.

---

## Ranked optimizations

| # | Change | Expected win | Bit-exact? |
|---|---|---|---|
| 1 | ✅ **LANDED (#120)** — **O(1) uniform-axis density lookup** replacing the per-pixel binary search (`kernels/uniform_axis.h`: load-time uniformity check + estimate + fix-up walk to the exact searchsorted bracket; binary-search fallback for non-qualifying axes) | **[measured]** 7.14× on that kernel; ~−9% scan / −12% print route | **Yes** — 0 / 4.5 M float32 results differ (same bracket by construction) |
| 2 | ✅ **LANDED (#120)** — **Stop hashing on export.** The memo key FNV-1a's the whole float64 buffer byte-at-a-time, twice on a miss, for a one-shot export that can never hit the cache. Landed as `spk_params.disable_buffer_memos` (set by the app JNI for non-preview renders) + single key computation per miss; gated by `test_simulate_e2e` scenario G | **[measured]** 0.74 GB/s over 576 MB ≈ hundreds of ms | **Yes** — pure deletion, no arithmetic |
| 3 | **Heterogeneous-core chunk dispatch.** Keep chunk *boundaries* a pure function of (count, K) — determinism lives there — but hand chunks out via an atomic counter so big cores absorb more work | **[needs device]** 1.3–2× whole render on big.LITTLE | **Yes** — same boundaries, disjoint writes |
| 4 | ✅ **LANDED (#121)** — **Delete the full-res float64 intermediates and the zero-fill.** A 12 MP render allocated and zero-touched ~900 MB before doing useful work. Landed as: direct float32 filming on one-shot no-op-geometry renders (`expose_f32_gain` + float32 AE metering — `src`/`rgb` never exist), fused expose/scan per-pixel passes when no spatial/pointwise op intervenes (`raw`/`lin_rgb` never exist; uninitialized buffers when they must), move-passthrough geometry, and free-at-last-use for the float64 image and the stage float32 buffers. **[measured]** 12 MP host VmHWM: print 1.10 GB → **0.43 GB (−61%)**, scan 0.97 GB → **0.43 GB (−55%)**; render transients over process baseline ~840 MB → **~140 MB** | peak ~1.2 GB → **~0.35 GB** | **Yes** — no arithmetic change (gated by `test_simulate_e2e` scenario G direct-vs-materialized byte identity) |
| 5 | **NEON 16-bit quantizer** replacing the per-sample Kotlin float→uint16 loop (~36 M samples with bounds-checked NIO ops at 12 MP) | large | **Yes** — reproduces the scalar rounding exactly |
| 6 | **Stop paying zlib level 6** on grain-dominated 16-bit data, and delete the three full-image staging copies (~426 MB peak at 12 MP) | large | **Yes** — lossless either way |
| 7 | **Pixel-blocked spectral integral** — vectorize across 8 pixels at a fixed band instead of across 2 bands for one pixel | **[measured]** 2.03× on the kernel; ~−16% scan / −24% print | **No** — see below |

### The one non-bit-exact item (7), stated plainly

f64 results drift by **≤1.6e-15 relative (~7 ULP)**; **0 / 1.2 M** differ after the float32 cast.
It is *not* FMA contraction (checked with `-ffp-contract=off`) — it is `-ffast-math`
reassociation choosing a different association in vector vs scalar form, so it cannot be made
byte-identical without giving up `-fassociative-math`. It stays deterministic and
thread-invariant (accumulation remains in strict band order per pixel), and 1.6e-15 sits eleven
orders of magnitude inside the 1e-4 / 1e-5 oracle tolerance — the same class of trade already
made and documented when `exp10_vec` shipped.

**Decision rule:** if the standard is "within oracle tolerance and thread-invariant", take it —
as its own revertible commit, with the full parity suite green. If the standard is
"byte-identical to the previous build, full stop", **reject it** and take items 1–6, which are
still a large win with no arithmetic change at all.

---

## Corrections to the analysis (found by adversarial review)

Recorded because acting on the uncorrected versions would introduce bugs:

- **The O(1) lookup as first proposed dropped the clamp early-returns** present in both current
  implementations (`x <= xp[0]` / `x >= xp[n-1]`). Must be preserved. *(Honoured in the landed
  version: the clamps run before the bracket lookup, unchanged.)*
- **The DIR-coupler axis is not uniform.** It is built per channel as `le[k] / gamma_factor[c]`,
  so the uniform-axis assumption is unsound there. Apply item 1 only where uniformity is checked
  at load time, with a binary-search fallback. *(Honoured: `detect_uniform_axis` requires
  strictly-ascending + within step/4 of the uniform fit, and the fix-up walk makes the bracket
  exact regardless — the tolerance only guarantees the walk is O(1).)*
- **Glare was missing from the pass-fusion gate.** It runs between the two loops, so fusing them
  is only valid when glare is inactive too.
- **Folding grain into the memo key without folding all grain parameters** is a sticky
  wrong-image bug — the bad entry persists.
- **`fast_interp_channel` has no NaN guard** (unlike `np_interp_array`, which checks explicitly).
  A NaN input takes neither clamp branch and indexes with an undefined comparison result.
  *(Fixed with item 1: NaN now returns NaN up front — the previous behavior was an
  out-of-bounds `xa[-1]` read.)*
- **The "≈20× encoder" headline compares PNG16 before to uncompressed TIFF after** — that is a
  default-format change, not a like-for-like speedup. Report the two separately.

### Premise correction: bandwidth is not the bottleneck here

The usual mobile intuition does not apply to this engine. `scan()` spends ~2900 cycles/pixel to
consume 24 bytes — arithmetic intensity is roughly **100× past the roofline knee**, and total
streaming for a 12 MP render is ~2.5 GB (~3% of wall clock). **Tiling and fusion here buy
footprint and OOM-survival, not speed.** Rank them for what they actually deliver: item 4 is
what makes a 12 MP export survive on a 4 GB device at all (measured peak-RSS slope ≈ 105 MB/MP
+ 29 MB → ~1.3 GB at 12 MP, comfortably inside lmkd-kill territory).

---

## Rejected on principle (do not let these creep in)

- **LUT acceleration on export** (`use_scanner_lut` / `use_enlarger_lut`): a pointwise
  approximation of a chain that must stay exact. Correctly opt-in and default-off — keep it off
  for export.
- **GPU for the export render**: the only path to 10–50×, but GPU float rounding varies by
  driver and device, so "byte-identical across thread counts" degrades to "varies by handset",
  and it cannot be validated in CI. **Preview only.**
- **fp16 intermediates on export**: breaks the working-precision invariant outright.
- **Lowering the `exp10` polynomial degree**: genuinely computing less to go faster — the one
  thing the mandate forbids.
- **Reassociating the 81-band sum**: deterministic and *more* accurate, but a second independent
  source of golden drift for a second-order win. Don't spend the parity budget twice.

---

## The quality gate

Every change must pass before it lands:

1. The full current 39-case `engine-parity` suite green at both flag legs.
2. `test_parallel` green **with real multi-chunk execution** (Phase 0).
3. An **export-digest** check: SHA-256 of the exported container payload over a fixed matrix of
   scene × format × params, so "quality unchanged" is a property of the shipped file rather than
   a claim about intermediate buffers.
4. Peak-RSS recorded, with a ceiling per device tier.

Prefer a **deterministic work-counter** regression gate over a wall-clock budget: the same
binary on the same 1 MP workload measured a **1.8× spread** (879–1596 ms) on a shared runner, so
a wall-clock CI budget will flap.

## What is not proven here

Bake and render timings on real hardware, the big.LITTLE dispatch win, grain's true cost at
export resolution (it is serial by construction and nobody has measured it at 12 MP), and every
per-tier budget. These need a device; the plan is written so they get measured, not assumed.
