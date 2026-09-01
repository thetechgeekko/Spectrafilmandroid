# Spektrafilm production-readiness implementation plan

Status: canonical release-acceptance and implementation plan, reconciled 2026-08-31. The live
Wayfinder graph owns ticket state, blockers, priority, and claims; this document owns the required
outcomes and architecture. Start at [EXECUTION_INDEX.md](EXECUTION_INDEX.md).

Tracker: [Wayfinder map: production-ready Spektrafilm + 1–2 s exact
export](https://github.com/thetechgeekko/Spektrafilm-android/issues/164)

Nested performance history/workstream: [Wayfinder workstream: 1–2 s exact export + fast
interactive preview](https://github.com/thetechgeekko/Spektrafilm-android/issues/117)

Performance architecture: [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md)

## Outcome

The next public release must be one immutable, production-signed Android artifact that is:

- safe against malformed RAW/profile/recipe/mask inputs;
- color-correct across every CPU, LUT, GPU and file-output path;
- legally distributable with accurate notices and corresponding materials;
- resilient to navigation, process recreation, cancellation and background execution;
- bounded in memory at the approved 12.5 MP and 50 MP workloads;
- accessible, localizable and covered by real Android end-to-end tests; and
- measured against a versioned exact-output and 1–2 second export contract.

This is an execution-bearing Wayfinder map. A child ticket closes only after its implementation,
tests, measurements and documentation are attached. A green build alone is not completion.

## The non-negotiable truth about 1–2 seconds

The last comparable 12.5 MP release run took 6,251 ms, of which 5,504 ms was native simulation.
That run predates exact CPU improvements now present on the branch, so it is historical evidence,
not a current-HEAD baseline. Even so, eliminating decode and encoding entirely would still leave a
multi-second renderer. No compressor, SIMD wrapper or thread library can make the whole app 1–2
seconds by itself.

The plan therefore keeps three different promises separate:

1. **Strict Exact CPU, cache hit:** render the exact full-resolution result during editor idle time;
   export validates the content key, encodes and publishes it. This is the credible route to a
   tap-to-gallery p50 at or below 1 second and p95 at or below 2 seconds.
2. **Strict Exact CPU, cold miss:** preserve the approved arithmetic and output digest while
   removing allocation, copies, scheduling and writer overhead. Two seconds remains an R&D target,
   not a promise, until current-HEAD measurements prove it.
3. **Fast GPU, cold miss:** a persistent full-chain Vulkan DAG is the plausible flagship route to
   cold 1–2 seconds. It can be oracle-equivalent and same-device deterministic, but must not be
   marketed as CPU-byte-identical across devices or drivers.

If the owner requires cold 1–2 seconds and cross-device byte identity simultaneously, the next
research frontier is a versioned fixed-point/integer `ARCHIVAL_V2` engine. It would be a new numeric
contract and cannot also reproduce every current floating-point byte cheaply.

## Release-gate ownership

Do not infer open/closed state from this table. Run `python tools/wayfinder/frontier.py` or open the
production map for the live dependency graph. This table only routes each release-gate family to
its owning ticket; native dependencies decide execution order.

| Gate family | Required outcome | Owning ticket(s) |
|---|---|---|
| Exactness and SLO | One approved matrix for engine samples, decoded samples/metadata, containers, routes, workloads, devices, and cold/warm statistics | [Define “bit-identical” and pin the 1–2 s export SLO matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/126) |
| Output and HDR truth | One descriptor for color/transfer/depth/format and an honest, user-visible Ultra HDR contract | [Define and test one OutputDescriptor for color space, transfer, depth, and format](https://github.com/thetechgeekko/Spektrafilm-android/issues/174); [Choose and implement an honest Ultra HDR contract](https://github.com/thetechgeekko/Spektrafilm-android/issues/140) |
| RAW/color parity | Implement the oracle-locked CAT02 fixture on the production RAW path and preserve native sample precision where supported | [Implement oracle-locked CAT02 RAW white balance and exact cast-order goldens](https://github.com/thetechgeekko/Spektrafilm-android/issues/192); [Preserve declared native RAW sample precision through linear conversion](https://github.com/thetechgeekko/Spektrafilm-android/issues/190) |
| Input safety | Retain completed native NPY/JSON/profile and vendor-RAW gates; finish explicit byte/depth/count budgets, hostile inputs, and atomic failure for app documents | [Patch LibRaw to 0.22.2 and add hostile-RAW regression coverage](https://github.com/thetechgeekko/Spektrafilm-android/issues/165); [Bound and fuzz remaining recipe, preset, sidecar, mask, and import parsers](https://github.com/thetechgeekko/Spektrafilm-android/issues/173) |
| Legal distribution | A human-approved LibRaw route agrees across notices, About UI, SPDX, and source/relink materials | [Resolve LibRaw static-link compliance and publish a complete license/source bundle](https://github.com/thetechgeekko/Spektrafilm-android/issues/166) |
| Android platform | The chosen distribution route passes Android 16/API 36 policy, build-system, NDK, and real 16 KiB-environment gates | [Validate Android 16 behavior and meet API 36 policy where distributed](https://github.com/thetechgeekko/Spektrafilm-android/issues/171); [Migrate AGP 9.3 and Gradle 9.5 to AGP built-in Kotlin](https://github.com/thetechgeekko/Spektrafilm-android/issues/188); [Upgrade the NDK independently and prove 16 KiB compatibility on every ABI](https://github.com/thetechgeekko/Spektrafilm-android/issues/187) |
| Memory and application behavior | Approved 12.5/50 MP budgets, prompt ownership release, durable sessions/exports, and truthful UI controls | [Release full-resolution buffers promptly and enforce one global memory budget](https://github.com/thetechgeekko/Spektrafilm-android/issues/176) and the other live production-map children |
| Release evidence | A versioned export-digest device gate, current release/R8 baseline, approved SLO proof, synchronized docs, then one immutable release | [Create a release export-digest benchmark and instrumented device gate](https://github.com/thetechgeekko/Spektrafilm-android/issues/177); [Record the canonical release/R8 export baseline and digest matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/119); [Prove the approved 1–2 s exact-export SLO on the release candidate](https://github.com/thetechgeekko/Spektrafilm-android/issues/186); [Ship the next security- and correctness-gated release](https://github.com/thetechgeekko/Spektrafilm-android/issues/138) |

Render-local timing, the patched/fuzz-gated LibRaw baseline, fail-closed signing mechanics, stock
viewing illuminants, and transactional storage are completed foundations recorded in the parent
map's Decisions section. Completion of those implementation tickets does not activate production
secrets, approve the legal route, or waive their downstream release gates.

### Exact release-candidate evidence — [Make production signing and exact release-candidate verification fail closed](https://github.com/thetechgeekko/Spektrafilm-android/issues/168)

The implementation retains one auditable chain from source to publication:

- strict Gradle locking across the six participating scopes; deterministic app
  and LibRaw SPDX; release runtime classpath; R8 mapping; full native symbols;
- fixed `PASS\n` gate inventory for LibRaw qualification, both engine flag legs,
  release JVM/lint, unsigned assembly, release instrumentation assembly, and
  R8/16 KiB checks;
- source SHA, stable version, run ID/attempt, candidate artifact database ID and
  digest, unsigned/signed/installed/test APK hashes, production certificate, and
  per-gate results in the release provenance; and
- immutable-release/numeric-ID publication with exact remote-byte verification,
  response-loss recovery, and draft-only cleanup ownership.

Offline release tools pass 32 unit/structure cases, and the release plus
release-targeted instrumentation variants build offline. On 2026-08-30 an API 36
SM-S948W installed the locally test-signed pair; instrumentation returned `PASS`
and code `-1`, the pulled installed APK matched the signed local SHA-256, cold
launch completed in 92 ms, no app-fatal Java/JNI/native log appeared, R8 JNI
members were present, `zipalign -P 16` passed, and all 14 64-bit shared objects
reported `PT_LOAD` alignment `0x4000`.

This is local mechanical evidence, not a production signature or a live GitHub
publication. The real release remains fail-closed until the protected Environment
supplies four signing secrets, `RELEASE_CERT_SHA256`, and
`RELEASE_GITHUB_TOKEN` (Administration read + Contents write), repository
immutable releases are enabled, the stable-tag ruleset is active, and the human
LibRaw distribution-route decision is recorded.

### Exact JNI lifetime evidence — [Harden JNI lifetime, buffer bounds, cancellation, and render-close races](https://github.com/thetechgeekko/Spektrafilm-android/issues/172)

The implementation evidence is complete: native allocations use explicit owners and leases; JNI
boundaries validate logical buffer windows and checked geometry; native-owned direct-buffer views
normalize Java byte-order metadata; engine render/close, cancellation, result publication, and
exact-once release are linearized; foreground-service start/stop commands are generation-safe; and
the shared native-safety runner locks the sanitizer inventory used by CI and release. A physical DEX
gate also proves the separately packaged AndroidTest APK's exact target-APK classes, fields, method
prototypes, facade ancestry, and interface dispatch rather than relying on owner names alone.

On 2026-08-31 the exact locally test-signed release app and AndroidTest APKs were installed in place
on an arm64 API 36 SM-S948W. Full release instrumentation passed twice, durable
`seed -> force-stop -> recover` passed, standalone engine and LibRaw JNI suites passed, both pulled
installed APKs matched their local SHA-256 values exactly, and cold launch produced no app-scoped
fatal log. The app/test hashes were respectively
`EE1A1BA636FA123C93186EB4A3E80E964281080462B5AB8E24B790DE361318E8` and
`6368EF2697FFD4A557E4418C22B7052F831D518AE4340E04473776CABB6E0F4A`. These are local-candidate
engineering results, not protected-Environment production-signing evidence. The complete ownership
contract and proof matrix are in [JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md); the reproducible
device procedure is in [TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md).

## Execution order

The critical measured sequence is the owner-approved exactness/SLO contract, then the
matched-resolution timing harness, then the HITL baseline recording, then only the optimizations
selected by those measurements, followed by signed-candidate SLO proof and terminal documentation
synchronization. Correctness, color, legal, lifecycle, storage, accessibility, memory, and build
work can run in parallel when their native dependencies permit it.

Native GitHub sub-issue order and `blocked-by` edges are the authority for the exact live frontier;
this document explains the acceptance sequence without freezing a duplicate ticket queue.

## Phase 0 — resolve contracts and make evidence trustworthy

### 0.1 Define exactness and the SLO

Owner: [Define “bit-identical” and pin the 1–2 s export SLO matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/126)

Record separate checkboxes for:

- previous-release engine/sample bytes;
- repeat and 1/2/4/8-worker identity in one build;
- cross-build, cross-ABI, cross-device and cross-OS identity;
- decoded image samples plus normalized metadata; and
- complete file/container bytes.

Pin the input hash, resolution, route, preset/effects, format, cold/warm state, foreground/background,
p50/p95, thermal protocol and reference device tiers. Until that resolution is posted, use precise
terms such as `engine sample digest`, `decoded sample digest` and `container SHA-256`; do not use
“bit-identical” alone.

### 0.2 Replace the process-global timer

Owner: [Make stage timing render-local before automated performance claims](https://github.com/thetechgeekko/Spektrafilm-android/issues/163)

Primary files:

- `engine/spektra-core/src/main/cpp/runtime/stage_timer.h`
- render entry points in `engine/spektra-core/src/main/cpp/spektra.cpp`
- preview/export orchestration in `app/src/main/java/com/spectrafilm/app/MainActivity.kt`

Give every render a context ID and local nested spans. Emit completion status, wall time and stable
stage identifiers into JSON and Perfetto. Add an overlap test where preview and export run together;
their totals must not reset or contaminate one another.

### 0.3 Build the digest and benchmark referee

Owner: [Create a release export-digest benchmark and instrumented device gate](https://github.com/thetechgeekko/Spektrafilm-android/issues/177)

Create a dedicated Macrobenchmark/instrumented module rather than timing Compose callbacks by hand.
The target APK must be release-minified and profileable; the harness must reject a stale package or
unexpected certificate/build fingerprint. Persist raw runs, not only medians.

Minimum matrix:

| Axis | Required initial cells |
|---|---|
| Input | pinned 12.5 MP JPEG/DNG/RAW corpus plus malformed negatives |
| Route | scan and print |
| Effects | base, grain, halation, DIR couplers, Pro-Mist, combined approved workload |
| Format | JPEG, Ultra HDR decision, PNG16, TIFF16, TIFF32F/scene-linear |
| State | cold process/cache miss, warm process/cache miss, exact cache hit |
| Lifecycle | foreground, background, cancel, process recreation, open/share |
| Evidence | engine/decoded/container digests, p50/p95/CI, RSS/PSS, thermals, energy, stage totals |

For C4/container cells, inject a fixed clock and deterministic metadata order, compare a documented
normalized representation, or explicitly mark the format unsupported for complete-file identity.
The current TIFF writer embeds a wall-clock date, so naive repeat SHA-256 is not a valid gate.

Run at least 10–15 alternating A/B samples for performance claims. Record the connected SM-S948W
(Android 16/API 36) as the flagship reference and add at least one representative lower-tier device.

### 0.4 Establish the current-HEAD baseline

Owner: [Record the canonical release/R8 export baseline and digest matrix](https://github.com/thetechgeekko/Spektrafilm-android/issues/119)

This comes after the contract, timer, harness and patched RAW decoder. It must remeasure the current
branch, including the unmeasured planar exponential-filter and coupler-copy reductions. Never add
stage values from the 6.251 s base run to the separate ~9.8 s effects ladder.

## Phase 1 — security, legal and supply chain

### 1.1 Patch and constrain LibRaw

Owner: [Patch LibRaw to 0.22.2 and add hostile-RAW regression coverage](https://github.com/thetechgeekko/Spektrafilm-android/issues/165)

Implementation record: [`dependencies/LIBRAW.md`](dependencies/LIBRAW.md). The
security upgrade is intentionally serial in release builds because compressed
Fuji output was non-deterministic in five of five OpenMP runs on LibRaw 0.22.2.
The rawpy 0.27 / LibRaw 0.22.1 versus Android 0.22.2 output rebaseline is routed
to the upstream parity-manifest ticket rather than being silently called exact.

Implementation sequence:

1. Pin LibRaw 0.22.2 source and SHA; store each local patch as a reviewable file.
2. Audit the open upstream OpenMP wavelet-denoise/fuzz issues. Patch or disable the affected path;
   an upstream version number is not itself a security proof.
3. Add known malformed lossless-JPEG/column fixtures and a representative camera corpus.
4. Fuzz the smallest native decode boundary under ASan/UBSan with time, byte and dimension limits.
5. Compare decoded outputs against the old supported corpus; record every intentional change.
6. Make release configuration fail if 0.21.4 or an unverified archive is selected.

Treat the older pin as affected pending fixture confirmation; do not overstate a CVE's exact version
mapping. The security ticket owns the reachability proof.

### 1.2 Resolve static-link obligations

Owner: [Resolve LibRaw static-link compliance and publish a complete license/source bundle](https://github.com/thetechgeekko/Spektrafilm-android/issues/166)

The current CMake builds LibRaw as a static library inside the native RAW module. That route is
recorded in `docs/LICENSING.md` as an unresolved release blocker. A maintainer or counsel must
select the dual-license route. Then update exact
notices, license texts, source/relink materials as applicable, About/licenses UI, SBOM and release
artifact together. Also remove unsupported blanket compatibility claims.

### 1.3 Bound every parser and JNI boundary

Owners:

- [Harden JNI lifetime, buffer bounds, cancellation, and render-close races](https://github.com/thetechgeekko/Spektrafilm-android/issues/172)
- [Bound and fuzz remaining recipe, preset, sidecar, mask, and import parsers](https://github.com/thetechgeekko/Spektrafilm-android/issues/173)

[Harden JNI lifetime, buffer bounds, cancellation, and render-close races](https://github.com/thetechgeekko/Spektrafilm-android/issues/172)
implements checked allocation geometry, direct-buffer range and byte-order validation, contained
C++ exceptions, one-shot native release, an explicit render/close lease state machine,
generation-safe foreground-service teardown, and exact cross-APK R8 ABI checks. Its offline and
connected arm64 API 36 device evidence is complete.

[Bound and fuzz remaining recipe, preset, sidecar, mask, and import parsers](https://github.com/thetechgeekko/Spektrafilm-android/issues/173)
now has a frozen, independently approved native JSON/profile/neutral-filter slice. JSON input is
capped at 1 MiB before allocation, with depth 8, 16,384 nodes, 512 array elements, 64 object
members, 4,096 decoded bytes per string/key and 128 bytes per number token. The parser rejects
duplicate decoded keys, malformed UTF-8 and surrogate pairs, unescaped controls, trailing data,
invalid number grammar and non-finite overflow. Profile V1 validation enforces required fields,
allowlists, dimensions and finite float range while retaining upstream-compatible nullable spectral
values and bounded dynamic 3 x N density models (`N=0..512`). All 28 bundled profiles load.
Neutral-filter lookup is failure-atomic, and render/probe callers explicitly own the zero fallback.

Ordinary hostile-input tests, ASan/UBSan, a 1,000-run bounded libFuzzer smoke, native-safety and
release-policy tests are wired into CI; the frozen slice passes those gates. The broader ticket
remains open for recipe, mask, RAW/DNG, Kotlin/import and updater/download boundaries.

## Phase 2 — color and file-output truth

### 2.1 Honor viewing illuminants

Owner: [Honor stock viewing illuminants, including Kodak K75P, in every scan path](https://github.com/thetechgeekko/Spektrafilm-android/issues/169)

Status: **implemented and verified**. One profile-driven, fail-closed registry resolves D50 and
K75P once at profile load and supplies the CPU direct scanner, scanner LUT/cache key, GPU
linear/fused preview and experimental export, viewing glare, and black/white reference paths.
Unknown/malformed identifiers return `SPK_ERR_PROFILE_INVALID` with field-specific thread-local C
detail that JNI preserves; a missing profile remains `SPK_ERR_PROFILE_NOT_FOUND`, and profile-load
allocation failures remain inside the C ABI as `SPK_ERR_OOM`.

Kodak 2383 and 2393 have independent pinned-oracle fixtures for direct RGB, upstream LUT17 RGB,
and all six output spaces, plus K75P-vs-D50 visual evidence. Direct and accelerated modes are gated
against their matching upstream modes at `max_abs <= 1e-4`, `rms <= 1e-5`; the native results were
about `1.1e-6` and `0.7e-6` max respectively. Upstream LUT17 itself differs from direct K75P by
about `3.97e-3` / `7.20e-3`, so exact export keeps that approximation off. Raising the grid to 80
would approach direct tolerance but inflate one prepared LUT from about 0.64 MiB to 69.4 MiB and
thrash the 8 MiB cache, so it is neither a parity nor performance fix.

Both fixture generators verify the checkout they actually import is exactly
`c1d0e44b962d80a51ea096d33faea346e4f3836c` and Git-clean before and after oracle execution.
Wrong-HEAD, tracked-dirty, and untracked-dirty repositories fail closed; deterministic manifests
record the verified commit and `worktree: clean`. Ten offline provenance/publish-transaction
tests cover that gate, including ambient exclude bypass, repo-specific ownership trust, linked
worktrees, and late-dirty manifest invalidation.

The complete 39-test host matrix passed with O2 and the shipping
`-O3 -ffast-math -fno-finite-math-only` flags. JVM tests, Android lint, and the three-ABI debug APK
build passed.
On the connected SM-S948W, K75P ran first and actually engaged both Vulkan scanner and print
self-checks; linear/fused GPU export stayed within `2.01e-6` of CPU export, repeated previews were
byte-identical, and the installed APK launched without fatal/native-load errors.

### 2.2 Resolve RAW white-balance adaptation

Owner: [Research and lock RAW white-balance chromatic adaptation against the upstream oracle](https://github.com/thetechgeekko/Spektrafilm-android/issues/167)

Research resolved CAT02 against the pinned source/test blobs and colour-science 0.4.7, quantified
the current XYZ-scaling/Bradford/no-adaptation errors, and locked white ownership, cast order,
camera-seed vectors, and rebaseline rules in
[`research/raw-wb-chromatic-adaptation.md`](research/raw-wb-chromatic-adaptation.md). Production
pixels remain unchanged here. Implementation owner:
[Implement oracle-locked CAT02 RAW white balance and exact cast-order goldens](https://github.com/thetechgeekko/Spektrafilm-android/issues/192).

### 2.3 Introduce one OutputDescriptor

Owner: [Define and test one OutputDescriptor for color space, transfer, depth, and format](https://github.com/thetechgeekko/Spektrafilm-android/issues/174)

Represent primaries, white point, transfer, scene/display encoding, bit depth, alpha/range, format,
ICC and HDR metadata in one immutable value. Engine output transform, quantizer, Bitmap/ColorSpace
tag and encoder must derive from it. Illegal combinations fail before rendering.

This unblocks [Choose and implement an honest Ultra HDR contract](https://github.com/thetechgeekko/Spektrafilm-android/issues/140).
Either remove Ultra HDR for the release or generate a real spatial gain map and validate both HDR
and SDR viewers. A constant 1x1 boost is not an honest spatial HDR result.

## Phase 3 — editor, lifecycle and durable storage

Owners:

- [Preserve the complete editor session across navigation and process recreation](https://github.com/thetechgeekko/Spektrafilm-android/issues/139)
- [Make MediaStore exports, URI imports, recipes, and masks transactional and versioned](https://github.com/thetechgeekko/Spektrafilm-android/issues/170)
- [Make ExportForegroundService own and recover the export job](https://github.com/thetechgeekko/Spektrafilm-android/issues/153)
- [Clear export busy state exactly once after every terminal result](https://github.com/thetechgeekko/Spektrafilm-android/issues/161)
- [Harden incoming and outgoing content-URI intents, MIME types, and grants](https://github.com/thetechgeekko/Spektrafilm-android/issues/162)

Implement a render/export coordinator with monotonic request IDs and latest-wins cancellation. The
service owns long-running export and its progress/cancel/recovery state; the Activity observes it.
Use the truthful foreground-service type and test OEM background execution. MediaStore publication
must be pending/atomic and idempotent; failure/cancel removes the row. Saveable editor state includes
source grant, params, masks, undo/redo, selected tool and schema version. Test both inbound
`VIEW`/`SEND` imports and outbound open/share for every supported content type.

## Phase 4 — bounded memory

Owners:

- [Release full-resolution buffers promptly and enforce one global memory budget](https://github.com/thetechgeekko/Spektrafilm-android/issues/176)
- [Tile the mask compositor and enforce a full-resolution memory budget](https://github.com/thetechgeekko/Spektrafilm-android/issues/141)
- [Bound tc_lut_cache with a byte-budgeted LRU](https://github.com/thetechgeekko/Spektrafilm-android/issues/142)
  ([implementation contract and evidence](research/tc-lut-cache.md))
- [Stream exact quantization and PNG/TIFF output without full-image staging](https://github.com/thetechgeekko/Spektrafilm-android/issues/175)

Inventory Java/Kotlin, native, GPU and file-writer allocations on one timeline. Define device-tier
ceilings and ownership. Tile masks with radius-aware overlap; an off-heap full-size copy is still an
unbounded copy. Recycle stale/full-export preview bitmaps, clear completed caches and make every LRU
byte-budgeted. Writer rows/strips stay bounded and participate in the same export budget.

## Phase 5 — exact performance work

The detailed design and budgets live in [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md).
The strict ordering is:

1. [Build an exact idle full-resolution pre-render and content-addressed export cache](https://github.com/thetechgeekko/Spektrafilm-android/issues/179).
2. [Build a render-scoped planar scratch arena for exact f64 spatial filters](https://github.com/thetechgeekko/Spektrafilm-android/issues/178).
3. [Replace per-call threads with a persistent deterministic worker pool](https://github.com/thetechgeekko/Spektrafilm-android/issues/182).
4. [Decide the grain numeric contract and remove pathological sampler cost](https://github.com/thetechgeekko/Spektrafilm-android/issues/180).
5. [Finish RAW decode optimization on patched LibRaw release builds](https://github.com/thetechgeekko/Spektrafilm-android/issues/158).
6. [Finish diffusion FFT/R2C optimization under the exact-output gate](https://github.com/thetechgeekko/Spektrafilm-android/issues/160).
7. [Measure the dominant matched-resolution cold-start and first-touch mechanism](https://github.com/thetechgeekko/Spektrafilm-android/issues/152).

Do not ship hard prime-core pinning: prior whole-export evidence showed it worsening runtime. Do not
replace the deterministic engine executor with oneTBB without a whole-render win and digest proof.

## Phase 6 — optional Fast GPU path

Owners:

- [Complete the Fast GPU resident DAG beyond the qualified pointwise chain](https://github.com/thetechgeekko/Spektrafilm-android/issues/148)
- [Experimental tolerance-bounded GPU export — not the strict exact path](https://github.com/thetechgeekko/Spektrafilm-android/issues/149)

One persistent Vulkan DAG owns 81-band buffers from filming through print/scan, spatial effects and
quantization, with one upload and one download. A scan-only shader cannot solve the 5.5 second
simulation. Require fixed per-pixel accumulation order, explicit NaN/Inf/index behavior, shader and
driver fingerprints, same-device repeat tests, CPU-oracle sweeps, watchdog, remote kill switch and
fail-closed CPU fallback.

[Complete the Fast GPU resident DAG beyond the qualified pointwise chain](https://github.com/thetechgeekko/Spektrafilm-android/issues/148)
now has an independently approved, frozen product route for eligible pointwise filming, printing
and scan work. It uses one upload, three resident dispatches and one readback; folds live tables;
keeps prepared tables under full-byte keys; runs a keyed CPU-oracle capability self-test; reports
render-local engagement; and falls back to Strict Exact CPU without publishing partial output. The
three-ABI Android Release build and full native parity matrix 39/39 pass at both O2 and the shipping
`-O3 -ffast-math -fno-finite-math-only` flags. The exact frozen O2 and shipping arm64 hashes also
pass on the connected Android 16 device with `test_gpu_host: ALL OK`: the product materialized,
direct-gain and tone maxima are `1.66893005e-6` / `1.73598528e-6` / `9.01520252e-7` at O2 and
`1.73598528e-6` / `1.73598528e-6` / `9.79751348e-7` at shipping flags. Both runs engage the
resident route, remain byte-identical on warm repeats, and pass cache, cancellation and exact-CPU
fallback coverage.

This is a tolerance-bounded, same-device-deterministic Fast GPU route, not a CPU-byte-identical
route. Spatial/stochastic stages remain open, 12/50/200 MP coverage remains planner-only, and no
1-2 s export result has been measured.

GPU results are never inserted into the strict exact cache unless the owner explicitly changes the
numeric contract. VkFFT is an optional Pro-Mist experiment after the DAG and memory system exist.

## Phase 7 — Android and toolchain migration

Owners:

- [Migrate AGP 9.3 and Gradle 9.5 to AGP built-in Kotlin](https://github.com/thetechgeekko/Spektrafilm-android/issues/188)
- [Validate Android 16 behavior and meet API 36 policy where distributed](https://github.com/thetechgeekko/Spektrafilm-android/issues/171)
- [Upgrade the NDK independently and prove 16 KiB compatibility on every ABI](https://github.com/thetechgeekko/Spektrafilm-android/issues/187)

Current pins are AGP 8.7.3, Kotlin 2.0.21, Compose BOM 2024.10.01, compile/target SDK 34,
build-tools 35.0.0 and NDK r27. The connected flagship is already Android 16/API 36.

Offline validation on 2026-08-29 found that host PATH Java 26 fails during Kotlin DSL setup
(`JavaVersion.parse` rejects `26`), while Android Studio JBR 21.0.10 completes
`:app:testDebugUnitTest :app:lint` successfully (212 actionable tasks). The first successful run
also reported SDK XML v4 being consumed by tooling that understands only through v3. A follow-up
`--warning-mode all` run identified a null-key attribute lookup scheduled to fail in Gradle 10;
determine whether it is project-, AGP- or lint-owned before the migration. Use JDK 21 for the
pre-migration baseline and attach the complete warning/SDK-tool inventory to the toolchain tickets.
The same JBR/offline configuration also completed `:app:assembleDebug` successfully across the
configured native ABIs; no APK was installed on the connected device.

Execute three isolated waves after the canonical baseline:

1. **Build-system wave:** use Upgrade Assistant; move to the supported AGP 9.3.x/Gradle 9.5/JDK
   combination; migrate to AGP built-in Kotlin; audit R8, shader, source-set and task behavior. Do
   not change target SDK or NDK in this ticket.
2. **Android behavior/policy wave:** decide distribution policy, raise compile/target SDK to 36 when
   required for Google Play, and execute the Android 16 behavior matrix. A GitHub-only release still
   needs the behavior matrix but must not describe Play policy as a universal Android rule.
3. **Native toolchain wave:** pin an NDK r28+ bridge independently; run parity/digests, ABI/JNI/
   OpenMP/Vulkan checks and a real 16 KiB environment. A newer stable NDK is another measured update.

Update Compose/AndroidX/Kotlin feature dependencies in later small compatibility waves, not in the
same diff as AGP, API behavior, NDK, numeric or color changes.

Toolchain upgrades are support/security work, not presumed runtime optimization. Every wave runs
the two parity flag legs, JVM/lint/build, release R8/JNI smoke and ABI/16 KiB validation.

## Phase 8 — UI quality, privacy and build hygiene

Owners:

- [Add Compose accessibility, localization, adaptive-layout, and E2E coverage](https://github.com/thetechgeekko/Spektrafilm-android/issues/181)
- [Harden backup, diagnostics, updater, and privacy disclosures](https://github.com/thetechgeekko/Spektrafilm-android/issues/183)
- [Decide inert engine controls that silently render unchanged pixels](https://github.com/thetechgeekko/Spektrafilm-android/issues/143)
- [Ratchet build hygiene and remove dead pseudo-modules](https://github.com/thetechgeekko/Spektrafilm-android/issues/184)
- [Make README and project status truthful for the next release](https://github.com/thetechgeekko/Spektrafilm-android/issues/144)

Add real `androidTest` coverage for import/edit/mask/navigation/process recreation/export/open/share.
Resource all strings; test TalkBack/Switch Access, keyboard, RTL/pseudo-locale, 200% text and adaptive
window sizes. Explicitly control backup, cached full-res images, diagnostic redaction/retention and
updater host/size/signature checks. Remove stale lint baselines and dead build targets only with
evidence; keep unrelated runtime changes out of the toolchain migration.

## Phase 9 — prove, document and release

1. [Prove the approved 1–2 s exact-export SLO on the release candidate](https://github.com/thetechgeekko/Spektrafilm-android/issues/186) runs the immutable signed artifact and attaches raw evidence.
2. [Synchronize audit, performance, release, licensing, and project-status documentation](https://github.com/thetechgeekko/Spektrafilm-android/issues/185) reconciles every version, gate, feature and guarantee.
3. [Ship the next security- and correctness-gated release](https://github.com/thetechgeekko/Spektrafilm-android/issues/138) publishes only after all native blockers are closed.

## Verification ladder for every implementation ticket

Use the smallest sufficient rung while iterating, then every affected higher rung before closing:

1. focused C++/Kotlin unit or fuzz test;
2. native parity at O2 and shipping `-O3 -ffast-math -fno-finite-math-only` flags;
3. genuine 1/2/4/8-worker digest comparison;
4. `./gradlew :app:testDebugUnitTest`;
5. `./gradlew :app:lint`;
6. `./gradlew :app:assembleDebug` and affected release variant;
7. R8/JNI smoke on the exact APK;
8. ABI and 16 KiB zip/ELF checks;
9. instrumented/API/device matrix;
10. statistically valid release-device performance and memory A/B;
11. signed release-candidate digest/SLO run.

The CI `engine-parity` job currently contains 39 `build_run` cases. The workflow is the authority;
scripts and prose must be updated whenever the table changes.

## External references used by this plan

- [Google Play target API requirements](https://support.google.com/googleplay/android-developer/answer/11926878)
- [Android Gradle Plugin 9.3 release notes](https://developer.android.com/build/releases/agp-9-3-0-release-notes)
- [Android NDK downloads and supported releases](https://developer.android.com/ndk/downloads/)
- [Android 16 KiB page-size guidance](https://developer.android.com/guide/practices/page-sizes)
- [Macrobenchmark overview](https://developer.android.com/topic/performance/benchmarking/macrobenchmark-overview)
- [Foreground-service type guidance](https://developer.android.com/develop/background-work/services/fgs/service-types)
- [LibRaw releases](https://github.com/LibRaw/LibRaw/releases)

## Change-control rules

- One ticket, one primary decision/diff. Do not combine toolchain, numeric and UI migrations.
- Preserve user changes in a dirty tree; no broad cleanup in a focused fix.
- Pin source/version/SHA/license for every native dependency.
- A measured win must name artifact, workload, device, sample count and uncertainty.
- A tolerance pass must never be written as byte identity.
- Do not close a ticket on host-only evidence when its acceptance names Android hardware.
- Update the Wayfinder ticket with raw artifacts and a concise resolution before closing it.
