# Exact and 1–2 second export roadmap

Status: canonical numeric-contract and performance architecture, reconciled 2026-08-31. The live
Wayfinder graph owns ticket state and dependency order. Start at
[EXECUTION_INDEX.md](EXECUTION_INDEX.md).

Parent plan: [PRODUCTION_READINESS_PLAN.md](PRODUCTION_READINESS_PLAN.md)

Tracker: [Wayfinder map: production-ready Spektrafilm + 1–2 s exact
export](https://github.com/thetechgeekko/Spektrafilm-android/issues/164)

Nested performance history/workstream: [Wayfinder workstream: 1–2 s exact export + fast
interactive preview](https://github.com/thetechgeekko/Spektrafilm-android/issues/117)

## Executive decision

There is no open-source library that can make this whole application export in 1–2 seconds while
also preserving every current floating-point and container byte. The historical 12.5 MP release
run spent 5,504 of 6,251 ms in the simulation engine. Codec replacement cannot remove that work.

Use two honest routes:

- **Strict Exact CPU:** keep the owner-approved arithmetic/output contract. Make perceived export
  1–2 seconds with an exact idle full-resolution pre-render plus a content-addressed cache, then
  incrementally reduce cold compute with allocation/copy/scheduling/writer improvements.
- **Fast GPU:** keep the entire 81-band graph and spatial effects resident in Vulkan. Target cold
  1–2 seconds on approved devices under CPU-oracle tolerances and same-device repeatability. This
  is not cross-device CPU-byte identity.

The product may ship one or both only after their names and guarantees are distinct in UI, docs,
telemetry and tests.

## “Bit-identical” is not one contract

The repository currently defines “bit-exact” as oracle tolerance (`max_abs <= 1e-4`,
`RMS <= 1e-5`) plus byte identity across worker counts in the same build. It explicitly does not
promise cross-architecture bytes, and release flags include floating-point reassociation.

[Define “bit-identical” and pin the 1–2 s export SLO matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/126)
must accept or reject each proposed level:

| Level | Proposed name | Question | Current guarantee |
|---|---|---|---|
| C0 | same-build deterministic | Same engine/sample bytes on repeat and 1/2/4/8 workers? | Yes for the parity-bearing scenarios |
| C1 | historical sample identity | Same engine/sample bytes as the previous release/build? | No general promise; some landed FFT changes differ in last bits |
| C2 | portable sample identity | Same bytes across compiler, ABI, CPU, device and OS? | No; shipping fast-math/FMA behavior makes this a new contract |
| C3 | decoded-output identity | Same decoded pixels and normalized metadata, allowing a different compressed stream? | Not yet gated as a complete export matrix |
| C4 | container identity | Same complete file SHA-256, chunk order, metadata bytes and compressor output? | Not gated; volatile timestamps/metadata and encoder upgrades must be controlled per format |
| E3 | oracle-equivalent GPU | Within approved max/RMS error and deterministic on one device/driver? | Experimental; never C1/C2 by implication |

Every benchmark artifact must record three digests separately:

1. engine/sample payload digest;
2. decoded samples plus normalized metadata digest; and
3. complete container SHA-256 when C4 is supported for that format.

Changing a lossless PNG compressor can preserve the second while changing the third. Changing a
JPEG encoder can change both the codestream and decoded samples. Current TIFF output includes a
wall-clock date, so a C4 test must inject a fixed clock and deterministic metadata order, compare a
documented normalized representation, or mark C4 unsupported for that format. A tolerance pass is
not a digest pass.

## What the existing measurements actually say

### Historical comparable base export

Galaxy SM-S948W, release build, 4,080 x 3,060-class workload:

| Phase | Time | Share/meaning |
|---|---:|---|
| total | 6,251 ms | historical run, not current HEAD |
| native simulate | 5,504 ms | 88%; the real SLO blocker |
| decode | 546 ms | includes RAW/decode path for that run |
| grade | 30 ms | not a headline lever |
| remaining setup/EXIF/encode/publish | 171 ms | format-specific split was not captured |

This branch later removed a large coupler copy and changed the planar exponential-filter path.
Host evidence reports 1,293.2 to 824.8 ms for that filter and an estimated coupler memory/copy win,
with exact local gates. Neither has a canonical current-HEAD release-device rerun, so the 6.251 s
number must not be presented as current performance.

### Arithmetic implied by the target

If the historical non-engine 747 ms remains unchanged:

- a 2,000 ms total leaves 1,253 ms for a 5,504 ms engine: **4.39x engine speedup**;
- a 1,000 ms total leaves 253 ms for the engine: **21.76x engine speedup**; and
- even halving all non-engine work leaves a 1-second target needing about **8.79x** engine speedup.

That is why strict cold 1–2 seconds is a research goal rather than an engineering estimate.

### Do not combine incomparable scenarios

The separate named-preset/effects ladder was roughly 9.8 seconds, with grain, halation and DIR
couplers contributing roughly 7.9 seconds; Pro-Mist added about 9.9 seconds in another cell. Those
measurements use different state/workload details and must not be added to the 6.251 s base run.

The historical GPU export latch accelerated only the scan slice and was bounded to about a 1.2x
whole-export improvement by Amdahl's law. The current eligible pointwise print route now keeps
filming, printing, and scan in one resident three-dispatch Vulkan chain, but spatial/stochastic
effects still fall back to CPU. Cold 1–2 seconds needs the remaining resident graph, not more
isolated shaders.

## Missing measurements before implementation claims

- no current-HEAD release-device rerun after the latest planar filter/coupler changes;
- no canonical current-HEAD release-device baseline using the implemented render-local timing;
- no release Macrobenchmark/simpleperf/Perfetto flamegraph tied to the exact APK;
- no statistically valid 10–15-run p95/confidence interval baseline;
- no fully pinned source/parameter/format manifest for the 6.251 s run;
- no controlled cold/warm/page-cache protocol;
- no per-format quantize/encode/publish split;
- no peak RSS/PSS, energy, thermal, battery and cpuset series;
- no full base/effect/route/format matrix;
- no full-chain spatial/stochastic GPU engagement/copy/fallback timing (the pointwise route is
  instrumented and functionally device-gated); and
- no exported engine/decoded/container digest matrix across supported ABIs.

Until these exist, performance numbers are hypotheses or historical observations, not release
acceptance evidence.

## Provisional budgets

The owner ticket may change these. They are engineering envelopes, not promises.

### Strict cold 2-second research envelope

| Phase | p95 budget |
|---|---:|
| setup, EXIF and residual lifecycle | 100 ms |
| RAW/input decode | 300 ms |
| full simulation | 1,250 ms |
| grade and output packing | 50 ms |
| encode and transactional MediaStore publish | 300 ms |
| total | 2,000 ms |

### Strict cache-hit user-facing envelope

| Phase | p95 budget |
|---|---:|
| validate full content key and cached payload digest | 100 ms |
| format-specific pack/encode | 1,200 ms |
| metadata and atomic publish/open-ready | 300 ms |
| scheduling/IO variance reserve | 400 ms |
| total | 2,000 ms |

The cache path should target p50 at or below 1 second and p95 at or below 2 seconds on the approved
12.5 MP flagship workload. Fifty-megapixel and all-effects targets remain separate until measured.

## Stage 0 — build the referee

Required tickets and completed foundation:

- [Define “bit-identical” and pin the 1–2 s export SLO matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/126)
- [Make stage timing render-local before automated performance claims](https://github.com/thetechgeekko/Spektrafilm-android/issues/163) — completed; `spk.stage_timings.v1` and overlap-isolation tests are the required timing substrate
- [Create a release export-digest benchmark and instrumented device gate](https://github.com/thetechgeekko/Spektrafilm-android/issues/177)
- [Record the canonical release/R8 export baseline and digest matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/119)

Instrumentation requirements:

- render-local nested spans with a unique export ID;
- GPU engagement, shader pipeline, upload/download and CPU fallback counters;
- FFT transform size, tile count, scratch bytes and fallback counters;
- allocation/copy/first-touch spans for spatial filters;
- format-specific quantize/filter/compress/write/publish spans;
- LibRaw unpack/process/copy/JNI spans; and
- raw JSON plus Perfetto trace linked to build/device/source/params fingerprints.

## Stage 1 — exact perceived speed through idle pre-render

Owner: [Build an exact idle full-resolution pre-render and content-addressed export cache](https://github.com/thetechgeekko/Spektrafilm-android/issues/179)

### Architecture

1. After a settled edit, debounce and schedule the approved full-resolution Strict Exact CPU render.
2. Key it by source content hash, every engine/edit/mask parameter, profile/LUT/asset hashes,
   engine/build/numeric-contract version, dimensions and OutputDescriptor.
3. Store a versioned linear/packed payload off-heap or in an mmap-backed file with an atomic commit.
4. Use latest-wins cancellation. An obsolete render may finish cleanup but may never publish a key.
5. Enforce byte, entry and age budgets; exclude or encrypt/clear sensitive cached content according
   to the backup/privacy decision.
6. On export tap, recompute/validate the key and payload digest, encode and transactionally publish.
7. A miss executes the same exact cold path; corruption or version mismatch is only a safe miss.

This is not “fake speed”: the exact work still runs, but it moves from the user's export tap to idle
time. Telemetry and benchmarks must report cache-hit and cache-miss latency separately.

## Stage 2 — exact cold CPU work

### Reuse spatial memory and stay planar

Owner: [Build a render-scoped planar scratch arena for exact f64 spatial filters](https://github.com/thetechgeekko/Spektrafilm-android/issues/178)

Reuse aligned correction/tail/component/filter planes inside one render context; keep data planar
through consecutive spatial operations; permit last-component/in-place reuse only when lifetime and
alias proofs are explicit. Never create a process-global mutable scratch pool that preview/export can
race. This is the safest path to hundreds of milliseconds or perhaps low seconds on spatial-heavy
presets, but it is not independently a 4.4x engine plan.

### Reuse threads without changing arithmetic

Owner: [Replace per-call threads with a persistent deterministic worker pool](https://github.com/thetechgeekko/Spektrafilm-android/issues/182)

Current `parallel_for` constructs and joins `std::thread` workers repeatedly. Preserve fixed chunk
boundaries and disjoint writes; allow workers to claim those fixed chunks dynamically; reuse threads
and per-worker scratch. Coordinate the thread budget with LibRaw OpenMP. Do not hard-pin two prime
cores: whole-export device evidence showed a regression.

Expected value is likely tens of milliseconds unless current traces show otherwise. Adopt only on a
whole-render Android A/B, not a scheduler microbenchmark.

### Make the grain choice explicit

Owner: [Decide the grain numeric contract and remove pathological sampler cost](https://github.com/thetechgeekko/Spektrafilm-android/issues/180)

Grain uses deterministic 8,192-pixel seed blocks and dynamic work claiming, but
`fast_binomial_one` can enter an O(n) CDF-inversion walk. There are two valid routes:

- preserve historical draw count/order and accept only profile-guided/low-level exact changes; or
- version a bounded deterministic sampler with statistical, repeatability and visual gates.

The second can be much faster but changes samples. It cannot be called historical-byte-identical.

### Stream quantization and encoders

Owner: [Stream exact quantization and PNG/TIFF output without full-image staging](https://github.com/thetechgeekko/Spektrafilm-android/issues/175)

The PNG writer currently creates a complete filtered scanline buffer, calls one-shot `compress2`,
creates a complete output vector and then writes it. Stream rows through bounded chunks and emit
multiple IDAT chunks; stream TIFF strips/tiles and use checked offsets/BigTIFF policy. This is a
large memory improvement and a tail-latency improvement, not the simulation solution.

### Keep RAW work in proportion

Owner: [Finish RAW decode optimization on patched LibRaw release builds](https://github.com/thetechgeekko/Spektrafilm-android/issues/158)

An OpenMP qualification build exists, but shipping LibRaw remains serial until a fixed RAW corpus
proves deterministic decoded outputs, memory, and thermal behavior. The historical release path
spent roughly 546 ms in decode and 325 ms in `dcraw_process`; even an ideal 2x demosaic gain saves
about 160 ms. Profile copies, allocation and JNI handoff first; enable parallel decode only through
the qualification gate and a bounded global thread budget. Consider NDK `AImageDecoder` only for
eligible non-RAW inputs and only after an OS-version corpus comparison.

### Tune diffusion after evidence

Owner: [Finish diffusion FFT/R2C optimization under the exact-output gate](https://github.com/thetechgeekko/Spektrafilm-android/issues/160)

Keep the custom CPU FFT unless a replacement wins the real transform-size/crossover/memory case.
Tune overlap-save/tile size and scratch reuse first. The FFT/direct paths can differ around 1e-15
from reassociation; their shipping status follows the resolved exactness level.

## Stage 3 — full-chain Fast GPU

Required tickets:

- [Complete the Fast GPU resident DAG beyond the qualified pointwise chain](https://github.com/thetechgeekko/Spektrafilm-android/issues/148)
- [Experimental tolerance-bounded GPU export — not the strict exact path](https://github.com/thetechgeekko/Spektrafilm-android/issues/149)

Required architecture:

- one persistent Vulkan device/context and pipeline cache;
- device-local 81-band intermediates and spatial scratch;
- filming, develop, print, scan, LUTs, grain/spatial stages and quantization in one DAG;
- one input upload and one final download, with no inter-stage host round trips;
- fixed per-pixel accumulation order and no subgroup reductions for correctness-sensitive sums;
- explicit NaN/Inf, bounds, LUT-index, rounding and saturation semantics;
- device/driver/OS capability cache keyed with shader hashes;
- startup/upgrade self-test, at least 100 repeat digests and CPU-oracle corpus sweep;
- watchdog, allocation limits, kill switch and fail-closed Strict Exact CPU fallback.

The “LUTs” in this graph do not mean a baked whole-look 3D cube. Scientific response tables,
density curves and an explicitly selected user LUT may be resident resources; the normal Fast GPU
render must evaluate the direct shader graph. `.cube`/CLF baking remains an export operation.

### 2026-08-31 resident pointwise product-route checkpoint

[Complete the Fast GPU resident DAG beyond the qualified pointwise chain](https://github.com/thetechgeekko/Spektrafilm-android/issues/148)
now routes eligible production `run_print` renders through one resident filming -> printing -> scan
Vulkan pointwise chain: one frame upload, three compute dispatches, device-local intermediates and
one final readback. It folds the live film, DIR, print and scan tables, applies direct-input gain
exactly once, and reuses prepared tables under full-byte cache keys. GPU output stays private until
the whole chain succeeds; non-cancellation availability, validation, allocation, or dispatch
failures fall back to Strict Exact CPU without exposing partial pixels or partially bypassing CPU
memos. Cancellation terminates the render and never restarts it on CPU.

The keyed capability verdict exercises a 512-point 8 x 8 x 8 lattice three times against the direct
f64 CPU stages at `max_abs <= 1e-4` and `RMS <= 1e-5`. Render-local diagnostics report route
engagement, fallback and frame resource counters while keeping self-test state, duration and work
separate. The frozen implementation received an independent `APPROVED` review, builds as Android
Release for all three configured ABIs, and passes the full native parity matrix 39/39 at both O2 and
the shipping `-O3 -ffast-math -fno-finite-math-only` flags. The exact frozen arm64 artifacts
(`89854375...39C` at O2 and `EDDB82CE...5B1` at shipping flags) also pass the connected Android 16
Adreno gate with `test_gpu_host: ALL OK`. O2 reports combined-pointwise `max_abs=1.00188277e-7`
and product materialized/direct-gain/tone maxima of `1.66893005e-6`, `1.73598528e-6` and
`9.01520252e-7`; shipping reports `9.23373584e-8`, `1.73598528e-6`, `1.73598528e-6` and
`9.79751348e-7` respectively. Both runs engage the resident `3 dispatch / 1 upload / 1 readback`
route, keep warm output byte-identical, and pass cache, cancellation and exact-CPU-fallback gates.

This checkpoint covers eligible pointwise work only. Spatial and stochastic stages, including
halation, diffusion, Pro-Mist and grain, remain open. Fast GPU is tolerance-bounded and intended to
be repeatable on the same approved device/driver; it is not CPU-byte-identical. The 12/50/200 MP
cases remain planner-only, and no 1-2 s export claim follows from these functional gates.

Vulkan's specification does not promise cross-implementation pixel identity. A device that fails the
self-test simply cannot expose Fast GPU export.

## Open-source decision matrix

Versions are evaluated pins as of 2026-08-29. Every adopted dependency still needs a source SHA,
license texts, SBOM entry, ABI/16 KiB check, fuzz/update owner and measured release-device benefit.

| Component | Decision | Where it helps | Exactness/engineering boundary |
|---|---|---|---|
| Vulkan compute with negotiated capabilities (shipping host currently requests 1.1) | build the Fast GPU route | Only plausible whole-simulation 1–2 s direction | Define minimum required features per device; do not mandate Vulkan 1.4 across the minSdk-24 fleet or assume cross-device bytes |
| Vulkan Memory Allocator (MIT) | conditional adopt with the resident DAG | Persistent GPU buffer/suballocation reliability | Memory management, not compute speed; pin only after device A/B |
| libspng 0.7.4 + zlib-ng 2.3.3 | A/B, then adopt for streaming PNG if it wins | PNG16 row streaming, peak memory and encode tail | Can preserve decoded pixels; changes IDAT/container bytes. Namespace zlib-ng |
| LibRaw 0.22.2 plus reviewed patches | mandatory security upgrade | RAW safety/maintenance | Not a whole-export speed lever; corpus and hostile-input gates first |
| NDK `AImageDecoder` (API 30+) | narrow spike | Supported non-RAW decode into native memory | OS codec output may vary; keep API 24–29 and RAW fallbacks |
| Highway 1.4.0 | narrow per-kernel spike | Independent FP32 pointwise/LUT/quantization kernels | Local f64 was slower; FP32 gained 2.1–2.7x but changed last bits. `memcmp` each candidate |
| VkFFT 1.3.4 (MIT) | Fast GPU Pro-Mist/diffusion spike only | Large-radius R2C/overlap-save after measured crossover | Runtime shader/build/Android integration cost; approximate GPU contract only |
| NDK LLVM OpenMP | qualification-only until corpus approval | Potential LibRaw demosaic parallelism | Shipping stays serial; require deterministic decode, memory/thermal evidence, capped threads, and no engine-pool oversubscription |
| existing custom CPU R2C FFT | keep | Current CPU diffusion fallback | It beat the tested NumHalide path; do not replace without real A/B |
| libjpeg-turbo 3.2.0 | optional format-specific spike | JPEG/lossy-DNG only if encoding becomes material | Codestream and possibly decoded samples change; historical Amdahl ceiling is small |
| Little CMS 2.19.1 | feature-dependent, not speed | Arbitrary external ICC support only | Larger untrusted parser surface; will not reproduce current fixed-transform pixels |
| Halide 21.0.0 | reject broad adoption; allow one narrow CPU AOT spike | Only a measured stage above 1.5x with exact digest | Real local chains were 0.78–1.51x; NumHalide FFT was much slower; Vulkan backend unsuitable here |
| oneTBB 2023.1.0 | reject shipping dependency | No proven gap over fixed-chunk executor | Adds scheduler/ABI/oversubscription cost; deterministic static scheduling loses balancing |
| libdeflate 1.26 | reject for PNG | Fast bulk deflate | No streaming API, directly conflicts with bounded row-streaming architecture |
| OpenCV/OpenCL | reject | No unique measured stage | Adds a second GPU/image stack and does not fit current f64 recurrences |
| RenderScript/toolkit | reject | Deprecated intrinsics only | Hardware acceleration deprecated; spectral DAG belongs in C++/Vulkan |
| libjxl 0.12.0 | defer product research | Future archival format/size, not speed SLO | Large dependency/security surface and no whole-export benefit |
| vkdt | architecture reference only | GPU DAG/shader organization ideas | Different spectral model/grain; audit per-file license before deriving code |

### Why the tempting broad replacements are rejected

- **Highway:** the real f64 lever was about 2% slower and failed the shipping-flag byte claim;
  FP32 kernels can still be worthwhile under a tolerance or separately digest-proven contract.
- **Halide:** synthetic pointwise gains did not survive the real blur/FFT shapes; AOT integration and
  strict floating scheduling add maintenance without a demonstrated whole-stage win.
- **oneTBB:** the engine already has deterministic fixed chunks and good four-core scaling. The next
  experiment is persistent internal workers, not a second scheduler beside OpenMP.
- **Codec swaps:** the historical non-simulation remainder was only 747 ms including decode. They are
  important for memory/format behavior, not a 5.5-second simulation shortcut.
- **Scan-only GPU:** prior evidence caps the whole export around 1.2x because the hot grain, halation,
  coupler and Pro-Mist work remains on CPU.

## Exact optimization acceptance template

Every Strict Exact CPU performance pull request/ticket must attach:

1. the exact before/after commit, compiler, flags, assets/profile and source/params hash;
2. focused stage timings and whole-export raw runs on release Android hardware;
3. alternating A/B p50/p95 and confidence interval, with thermals and peak RSS/PSS;
4. engine/decoded/container digests required by the resolved contract;
5. 1/2/4/8-worker repeats and concurrent preview/export evidence;
6. the 39-case parity matrix at O2 and shipping flags;
7. cancellation, NaN/Inf, allocation failure and fallback behavior; and
8. a revert condition when the real-device win is below the ticket's minimum.

For Fast GPU, replace cross-worker evidence with same-device 100-repeat, driver/OS matrix, CPU-oracle
max/RMS results, engagement/copy traces and forced fallback tests.

## Final release proof

[Prove the approved 1–2 s exact-export SLO on the release candidate](https://github.com/thetechgeekko/Spektrafilm-android/issues/186)
is the only ticket allowed to claim the destination is met. It runs the immutable signed/minified
APK, pins every workload dimension, reports cache hit/miss and GPU separately, attaches raw samples
and required digests, and obtains explicit maintainer sign-off.

## Primary external references

- [Vulkan floating-point/pixel invariance](https://docs.vulkan.org/spec/latest/appendices/invariance.html)
- [Vulkan registry](https://registry.khronos.org/vulkan/)
- [SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)
- [libspng progressive encoder](https://libspng.org/docs/encode/)
- [zlib-ng releases](https://github.com/zlib-ng/zlib-ng/releases)
- [Highway releases](https://github.com/google/highway/releases)
- [Android NDK image decoder](https://developer.android.com/ndk/guides/image-decoder)
- [VkFFT releases](https://github.com/DTolm/VkFFT/releases)
- [libjpeg-turbo Android builds](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/BUILDING.md)
- [Halide releases](https://github.com/halide/Halide/releases)
- [oneTBB partitioner semantics](https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/Partitioner_Summary.html)
- [libdeflate limitations](https://github.com/ebiggers/libdeflate)
- [RenderScript migration guidance](https://developer.android.com/guide/topics/renderscript/migrate)
- [LibRaw releases](https://github.com/LibRaw/LibRaw/releases)
