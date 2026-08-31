# JNI lifetime, buffer, and cancellation contract

This document is the implementation contract for
[Harden JNI lifetime, buffer bounds, cancellation, and render-close races](https://github.com/thetechgeekko/Spektrafilm-android/issues/172).
It covers the engine, LibRaw decoder, PNG/TIFF writers, and the app code that transfers their
off-heap buffers. A successful build is not sufficient evidence: every invariant below has a
focused host/JVM test and an actual Android JNI or release-candidate check.

## Ownership model

Native allocations have one opaque owner. A direct `ByteBuffer` is only a view; it is never release
authority.

- `LinearImage`, `SimResult`, and LibRaw `LinearResult` close idempotently and reject new leases
  after close begins. An existing `DataLease` keeps the allocation alive until that lease closes.
- Cross-owner zero-copy transfer moves an active lease into the destination owner. If destination
  construction fails, the lease is closed on the failure path.
- Manual engine staging allocations are represented by `NativeBufferOwner`. Callers may acquire an
  explicit lease or transfer it to `LinearImage`; no public method accepts an arbitrary borrowed
  buffer and frees it.
- JNI allocation registries require the exact opaque token, base address, and capacity. Foreign
  direct buffers, slices, stale tokens, repeated close, and a duplicate view without the token
  cannot release native memory.
- Kotlin construction after a successful JNI ownership transfer is exception-safe. If metadata or
  owner construction throws, the native allocation is released exactly once and the original
  failure remains primary.
- A `NewDirectByteBuffer` view created by JNI is normalized to `ByteOrder.nativeOrder()` only when
  it enters a native-owned `NativeBufferOwner` or `SimResult`. Caller-owned `LinearImage` input
  remains strict and rejects a non-native-order view. Byte order is view metadata, not ownership.

Scoped helper callbacks return `Unit`; operations that must outlive a call use an explicit
`DataLease`. This makes lifetime transfer visible in code review and avoids APIs whose generic
return type can accidentally return a borrowed native buffer.

## Render and close linearization

`SpektraEngine` owns its native handle through a read/write lease:

1. every engine-handle JNI operation acquires a read lease and snapshots the non-zero handle;
2. `close()` queues for the write lease, atomically takes the handle once, and destroys it once;
3. a close racing an active render cannot destroy the handle until the render and its JNI
   cancellation callback have returned;
4. result/image owners independently defer allocation release until their final data lease exits.

The deterministic Android boundary test blocks inside a real native cancellation poll, proves the
close writer is queued behind the render lease, cancels the render, and requires a
`CancellationException`, no result publication, and completion of close only after the render
lease exits.

## Checked JNI boundaries

All caller-controlled geometry is widened before multiplication and checked before casts,
allocation, or dereference. The engine and LibRaw paths reject non-positive dimensions, overflow,
oversized byte counts, invalid stride/packing, and impossible output geometry with stable Kotlin
exceptions.

For direct buffers, the JNI/Kotlin boundary validates all of the following before reading:

- native C++ requires a direct buffer with an available base address/capacity, checks
  `0 <= position <= limit <= capacity`, requires the logical remaining range to contain the requested
  bytes, and rejects misaligned float offsets;
- Kotlin requires caller-owned float views to use native byte order and normalizes only newly
  transferred native-owned result/allocation views to native order; and
- encoded RAW input remains within the configured byte ceiling.

Every JNI entry point catches `std::bad_alloc`, `std::exception`, and unknown C++ exceptions. No C++
exception may unwind into ART. Native status values map to stable Kotlin exceptions, including
cooperative cancellation.

## Cancellation and publication

Cancellation is polled in the long-running LibRaw phases, engine stages/rows, PNG/TIFF writer loops,
and large JNI result copies. Result copies are chunked and poll both before chunks and after the
final chunk; cancellation frees a partial destination and cannot return a successful result.

Cancellation alone does not decide publication. App render/export generations use monotonic IDs
and linearized commit checks so a cancelled or superseded operation cannot publish stale output.
GPU preview publishes one immutable `(proxy, LUT, exposure gain)` submission through a single atomic
slot, preventing mixed generations and lost updates during a GL-context race.

Activity recreation is tested with a real native render result retained by the process-owned export
runtime. The replacement Activity must observe the same process-owned engine, safely read and close
the retained result, and claim the terminal publication exactly once. Process death remains a
separate durable `seed -> force-stop -> recover` storage test.

Foreground-service commands are generation-qualified. Every start or stop command enters through
`startForegroundService`; `onStartCommand` promotes the service before it evaluates the command and
uses `stopSelfResult(startId)` only for the matching generation. A stale completion therefore cannot
stop a newer export, and a very fast export cannot leave an unpromoted service for Android's
foreground-start watchdog. The release instrumentation deterministically blocks the target main
looper, completes three exports, releases the queued service commands, and stays alive beyond the
watchdog interval.

## Required evidence

| Layer | Required proof |
|---|---|
| Engine JVM | image/result lease close races, constructor rollback, exact-once release, and engine handle close queueing |
| LibRaw JVM | explicit lease transfer, close overlap, logical-window capture, construction rollback, and exact-once release |
| Host native | checked geometry/ranges, allocation-token race, real asset-backed render cancellation under ASan+UBSan and TSan, and writer cancellation races |
| LibRaw host | hostile/resource-limit decode, deterministic in-phase cancellation, and sanitizer qualification |
| Android engine JNI | actual parameter marshalling, direct logical ranges, active render/cancel/close overlap, result close race, and stable exception mapping |
| Android LibRaw JNI | native decode/read held across concurrent close, logical-range rejection, and cancellation mapping |
| Release app | R8-minified assembly; exact separate-APK AndroidTest ABI classes, fields, method prototypes, facade ancestry, and interface dispatch; signed in-place device install; native-result Activity recreation; foreground-service rapid-completion replay; storage process-death recovery; exact installed-APK bytes; and fatal-log rejection |

The shared host runner is `tools/release/run_native_safety.sh`. Its sanitizer evidence covers native
helpers, the real engine C render/cancel path, allocation registries, and writers. It does **not**
claim ASan/TSan instrumentation of the Android JNI bridge. Actual JNI behavior is a separate required
API 35 x86_64 engine instrumentation gate and connected-device arm64 check. The release app is
separately R8/Dex-gated, installed, instrumented, and launched; the standalone engine test APK does
not make a dynamic R8 coverage claim.

The exact local release-device procedure, including certificate comparison before an in-place
install and the process-death phases, is in [TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md).

## Ticket #172 verification record (2026-08-31)

The final locally test-signed candidate was verified in place, without uninstalling or clearing app
data, on an arm64 Samsung SM-S948W running API 36. This is engineering evidence for the exact tested
bytes, not protected-Environment production-signing evidence.

- app APK SHA-256: `EE1A1BA636FA123C93186EB4A3E80E964281080462B5AB8E24B790DE361318E8`;
- AndroidTest APK SHA-256: `6368EF2697FFD4A557E4418C22B7052F831D518AE4340E04473776CABB6E0F4A`;
- both pulled installed APKs exactly matched those local artifacts and both passed 16 KiB ZIP
  alignment and signer checks;
- the full minified release instrumentation passed twice consecutively with
  `TICKET170_INJECTED_FAILURES`, `TICKET172_ACTIVITY_RECREATION`, and
  `RELEASE_CANDIDATE_INSTRUMENTATION` markers and instrumentation code `-1`;
- `seed -> force-stop -> recover` passed with the same durable MediaStore token
  `content://media/external/images/media/216358`;
- rebuilt standalone engine and LibRaw instrumentation passed with
  `ENGINE_BOUNDARY_INSTRUMENTATION: PASS` and `OK (6 tests)` respectively;
- a cold launch completed (`TotalTime=104 ms`, `WaitTime=107 ms`) with no app-scoped fatal log; and
- the exhaustive AndroidTest-to-target DEX sweep resolved all 104 external descriptors, 13 exact
  fields, and 99 exact method prototypes, with no unresolved classes, fields, or methods.

The checked-in fail-closed helper was then replayed against the same signed pair. It independently
emitted `RELEASE_DEVICE_GATE: PASS`, the same two APK hashes and signer digest, and a fresh matched
process-death token `content://media/external/images/media/216385` after both full instrumentation
replays and both installed-package pulls.

The offline proof also passed the full Gradle release/test assembly, 48 release-policy tests, four
ASan+UBSan native suites, four TSan native suites, focused owner/byte-order/service race tests, and an
independent code review with no actionable finding.
