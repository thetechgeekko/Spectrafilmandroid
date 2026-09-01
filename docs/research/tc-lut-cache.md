# Filming `tc_lut` cache: bounded residency and diagnostics

**Ticket:** [#142](https://github.com/thetechgeekko/Spektrafilm-android/issues/142)

**Implemented/verified:** 2026-09-01

## Result

`spk_engine` no longer stores every distinct filming-parameter key for its
entire lifetime. `runtime/tc_lut_cache.{h,cpp}` now owns two deliberately
separate classes of immutable LUT:

| Class | Key used by the engine | Admission ceiling | Eviction |
|---|---|---:|---|
| Bundled/default | bare film profile ID | 28 entries and 28 MiB | pinned; never evicted |
| Parameterized | exact existing composite key (blur/window/surface/UV/IR/input gamut bytes) | 64 entries and 8 MiB | true least-recently-used |

The entry limit is a second bound on container metadata; byte limits remain the
primary residency control. The cache uses a fixed 92-slot owner array, so a
stream of distinct slider values cannot grow map/list metadata either.

The original key bytes and `build_filming_tc_lut()` implementation are
unchanged. A hit returns the same immutable `NdArray`; a miss rebuilds the same
value. Each render holds an aliasing `shared_ptr<const NdArray>` lease for the
whole consumer path, so a concurrent eviction cannot invalidate data being
read.

## Accounting and admission contract

The charge for an admitted node is the sum of allocator-requested bytes for:

1. the small allocation-lifetime owner that carries the reservation until final
   deallocation;
2. the combined `shared_ptr` control block and cache node, measured by the
   allocator passed to `allocate_shared`;
3. any out-of-line key allocation, measured by the key allocator; and
4. checked `shape.capacity() * sizeof(int)` and
   `data.capacity() * sizeof(double)` allocations.

Every multiplication and addition is checked before admission. A failed check
returns the completed LUT uncached and increments `accounting_overflows`.
Counters use saturating increments. Hits, misses and eviction bookkeeping do
not change the charged byte value of any resident node.

An admitted node reserves its exact charge through the process-wide
`MemoryBudget` from #176 as `MemoryDomain::Cache` / `MemoryStage::Lut`. The
reservation is owned by the allocator lifetime attached to the shared node,
rather than by the cache slot or as a last-declared node member. On final lease
release the key and `NdArray` buffers are destroyed, the node/control-block
allocation is deallocated, and the lifetime owner is deleted before its
stack-moved reservation is reset. Therefore:

- `cache_held_bytes` falls as soon as a slot is evicted or cleared;
- an evicted node leased by an active render remains charged in the global
  cache-domain counter; and
- that global reservation is released only after the last lease dies and all
  charged payload/control storage has actually been deallocated.

This integration is intentionally **post-build cache-residency admission**.
`build_filming_tc_lut()` must finish before its vector capacities and exact
charge are known. Builder scratch and a completed LUT returned after a bypass or
global-budget denial are transient allocations which are not pre-reserved by
this cache. This does not claim admission-before-allocation or complete the
other native allocation domains tracked by #176.

## Bypass and race policy

- A dynamic node larger than 8 MiB is returned uncached without flushing the
  current working set; repeated requests rebuild it.
- A 29th pinned node, a pinned-byte overflow, or a zero entry limit returns the
  result uncached. Pinned defaults are never displaced by dynamic traffic.
- Dynamic admission evicts least-recently-used dynamic nodes until both its
  byte and entry ceilings fit. Pinned nodes are not candidates.
- A global budget denial returns the completed value uncached and records both
  cache-local and global denial counters.
- Builders run outside the mutex. If same-key builders race, the first admitted
  node wins and every loser receives a lease to that resident node; the losing
  temporary is destroyed.
- The first admission determines pinned/dynamic classification for a key.
  `classification_conflicts` exposes a future caller bug. Current engine key
  construction makes the two classes disjoint: bare keys are pinned and all
  parameterized keys have a composite suffix.

## Shipping observability

`spk_engine_tc_lut_cache_stats_json()` exports the stable
`spk.tc_lut_cache.v1` schema through JNI and `SpektraEngine`. Settings ->
Diagnostics displays the current snapshot and includes it in the bounded,
redacted diagnostics report. Opening Diagnostics does not initialize the heavy
engine solely to obtain a snapshot.

The readout includes hits, misses, same-key race hits, evictions, every bypass
or failure class, cache-held/pinned/dynamic bytes, entry counts and configured
ceilings. It labels the memory boundary as `post-build-cache-residency` and the
UI explains why the process cache-domain total can temporarily exceed
`cache_held_bytes` while an evicted node is still leased.

## Verification evidence

Measured on the repository host on 2026-09-01:

- Windows MSVC Release host CTest: **6/6 passed** in 0.49 s; the focused cache
  test itself took 0.01 s.
- WSL GCC ASan+UBSan (`SPK_NATIVE_INPUT_TEST_SANITIZERS=ON`): focused cache
  test **passed**, 0.03 s (configure/build 7.2 s).
- Shared CI/release native-safety runner under WSL Clang 18: all **14/14**
  locked suites passed, including the cache regression as one of eight
  ASan+UBSan rows and one of six TSan rows.
- Full-engine WSL GCC `-O2` spectral-blur gate: oracle taps passed; cache
  miss-then-hits passed; a 1 MiB test ceiling forced eviction; the final RGB
  float buffer before and after eviction was **byte-identical**; shipping JSON
  and insufficient-buffer rejection passed. Full-engine compile was about 60 s
  and execution, including WSL startup, was 3.2 s.
- Android Debug native build: **arm64-v8a, armeabi-v7a and x86_64 passed** in
  19 s.
- Focused `DiagnosticsTest`: **passed** as part of
  `:app:testDebugUnitTest` in 36 s.
- Physical SM-S948W Android instrumentation: the APK-asset-backed engine was
  created, `spk.tc_lut_cache.v1` was read through JNI before/after two real
  renders, miss-to-hit and every byte/entry ceiling were validated, and a
  post-close read was rejected by `EngineHandleLease`. The complete engine
  boundary runner reported `ENGINE_BOUNDARY_INSTRUMENTATION: PASS` and
  `INSTRUMENTATION_CODE: -1`.

The direct regression covers 28 pinned defaults, more than 300 dynamic slider
keys, exact charge ceilings, LRU recency, eviction/rebuild byte equality,
oversize behavior, same-key 16-thread races, builder `bad_alloc`, global-budget
denial, classification conflicts, clear with an outstanding lease, and final
reservation cleanup. A barrier-based release/admission regression pauses after
key/value/node/control-block deallocation but before budget release: a concurrent
same-sized admission is deterministically denied while paused and succeeds only
after the release barrier opens.

An earlier standalone GCC 13.3 TSan binary hit WSL's `unexpected memory mapping`
runtime limitation. That is not used as evidence; the committed shared runner's
Clang 18 TSan cache row subsequently compiled and passed, alongside the MSVC and
ASan+UBSan runs.
