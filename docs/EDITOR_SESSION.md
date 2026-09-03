# Editor session persistence and recreation

> Implementation contract for ticket #139. This describes the current app behavior, persistence
> bounds, recovery policy, and verification surface.

The editor owns one versioned session document outside the destination-specific Compose state.
Session storage and `SourceAccess` are reconciled before `EditorScreen` enters composition; an older
`rememberSaveable` bucket is only a small source/rotation fallback when no valid complete session
exists. A round-trip through Settings, About, Diagnostics, film curves, or print curves therefore
reconstructs the editor from the same source and editing cursor. Activity recreation flushes the
newest in-process checkpoint before the replacement editor reads it. Every later navigation entry
also waits behind the process-owned SourceAccess FIFO and reconciles again before composing; a grant
mutation completed while the editor was disposed cannot leave the host session stale.

A startup `Unavailable` session read keeps writes in `RECOVERING`. When storage later returns a
valid document, the app reconciles and adopts it before exposing `WRITABLE`. The recovered durable
cursor wins if the live editor has not changed; an explicitly tracked live mutation wins only after
its source reconciles to the current `SourceAccess` authority. The editor is recreated from that
decision, and lifecycle teardown calls the current checkpoint callback rather than a captured stale
one. `Unsupported` remains permanently protected for that app run.

## Stored state

`EditorSessionDocument` stores only model data and bounded identifiers:

- source `content://` URI identity, source kind, bounded display name, and whether reauthorization
  is currently required;
- current and last-committed parameter snapshots plus manual rotation;
- both bounded undo and redo branches in oldest-to-newest order;
- open editor category, safe resumable overlay, and selected mask index;
- preset amount base/full anchors, selected/saved preset names, and the settings clipboard;
- export-sheet options and sanitized process-owned export phase/run identifier.

The document never stores a `Bitmap`, native handle/buffer, URI capability token, stack trace, or
provider data. `SourceAccessCoordinator` remains the sole authority for persisted grants and checks
the saved source identity against current grant/readability state.

The file is `noBackupFilesDir/editor-session/current.json`. It is intentionally outside cloud/device
backup and is replaced through `AtomicJsonStore`; the previous committed file remains readable when
a replacement fails.

## Bounds and write behavior

| Item | Bound |
|---|---:|
| Schema | `org.spektrafilm.editor-session`, version 1 |
| Whole UTF-8 document | 32 MiB |
| One params/preset/clipboard snapshot | 4 MiB |
| Whole JSON structure | depth 48, 600,000 nodes, arrays 128, objects 4,096 keys |
| One snapshot structure | depth 40, 200,000 nodes; same array/object/string bounds as the root |
| Combined undo + redo entries | 50 |
| Combined undo + redo retained bytes | 8 MiB |
| Source URI / display name | 16 KiB / 512 characters |
| Selected and saved preset names | 96 characters each |

Every current, committed, undo, redo, preset-anchor, and clipboard snapshot is decoded into a
detached `ParamsState` during validation. Rotation, enum, numeric, mask-index, export-option, and URI
constraints are checked before any document becomes live. History replacement validates count and
byte totals before mutating either stack. Encoding validates the complete materialized root against
the same structural limits used by decoding before bytes reach `AtomicFile`; an accepted write can
therefore never be quarantined on its next read because of a write/read aggregate-bound mismatch.

If an otherwise valid live cursor exceeds the aggregate structural bound, checkpoint retention walks
one fixed monotonic ladder: drop the oldest undo entries first, then the oldest redo entries, then the
settings clipboard, then the paired preset base/full anchors. Removing anchors also resets preset
amount to `1`; current, committed, source, tool, and export state are never discarded. Before choosing
that prefix, every subtree—including one that may be removed—is syntax-, structure-, and
`ParamsState`-validated and then discarded after recording only node/depth/canonical-byte scalar
costs. Undo/redo suffix totals make every candidate an arithmetic calculation: no candidate JSON root
or parsed-tree collection is retained. The selected document is copied once, its complete root is
materialized/validated/serialized exactly once, and those final bytes are reused by the physical
write. A final invariant also requires the algebraic node/depth/UTF-8 cost to equal the serialized
root exactly.

Settled render edits checkpoint after the undo coalescing window. UI-only cursor changes checkpoint
after a short debounce, and navigation/lifecycle teardown captures synchronously on the main thread
before handing the immutable document to IO. The process writer uses one monotonic compare-and-set
latest slot plus a conflated signal, so slider churn retains at most one pending multi-megabyte
document. A recreation read and exactly-once export-terminal handoff share the writer lock. The
reader retries when an offer generation changes after its drain but before/during disk read, and a
monotonic accepted-generation high-water mark remains after the pending slot is drained. A delayed
older producer therefore cannot repopulate an empty slot and regress disk after a newer write. Every
offer completed before the read began is included; an overlapping offer observed during drain/read
is retried, while an offer after the final stability check linearizes after that read.

Each `EditorScreen` composition also owns a new live-instance token. Readiness is published only after
engine/catalog restoration and complete cursor capture; checkpoints from a disposed older token cannot
replace the bridge's live snapshot. Release instrumentation waits for a fresh readiness generation on
initial entry, every navigation return, Activity recreation, and process recovery, then compares the
complete live cursor with the flushed durable document.

## Restoration policy

Startup waits for the off-main session read before constructing the editor. This avoids publishing a
default/demo frame and then replacing it with restored state.

| Source/grant result | Editor action |
|---|---|
| Same URI and kind, readable | Restore the complete cursor and render it. |
| Same URI and kind, grant expired/missing | Keep edits, show reauthorization, and do not decode until the user chooses the file again. |
| Different durable URI or kind | Invalidate old publications/resources, clear cross-image undo/preset anchors, then restore the replacement source recipe. |
| Invalid durable record | Clear the poisoned grant record, retain the session source identity, and request reauthorization. |
| No durable record for a transient picker source | Retain source identity and edits, request the same source again, and never silently substitute demo pixels. |
| Explicit durable demo tombstone | Select demo and discard any stale URI session, even if the process died before the demo session checkpoint. |

An installed profile catalog can legitimately remove an identifier named by an older session. The
catalog check covers current/committed state, both history branches, both preset-amount anchors, and
the clipboard. If any branch names a removed profile, the editor keeps the source and valid current
parameters, selects an available profile, and clears history, anchors, and clipboard so the obsolete
identifier cannot be reintroduced later.

Crop and mask-geometry overlays contain an uncommitted gesture draft inside their Composable. Tool
selection and selected mask index are durable, but that draft is not: recreation reopens the selected
tool and initializes a new no-op draft from the committed crop or committed mask geometry. Merely
reopening therefore cannot change current params or history; only a later gesture plus Confirm can
commit an edit. Color/luminance/WB sampling overlays resume directly because they have no detached
gesture draft. Corrupt documents are quarantined with a generic one-time notice. Newer-version
documents are preserved and writes stay disabled until a compatible app reads them.

Recipe write authority is separate from source identity. Every editor composition starts a URI
sidecar as `PENDING`; only a completed read/classification of the same recipe generation can produce
`WRITABLE`. Future versions, transient IO, and failed quarantine remain `PROTECTED`, including across
navigation and recreation, so a pre-read checkpoint or autosave cannot overwrite their bytes. A
loaded/missing/successfully quarantined generation can save only with the generation returned by
that classification. Choosing demo first commits a durable SourceAccess tombstone and only then
changes live/session state, closing the clear-before-session-commit crash window.

## Rendering and export ownership

Every source replacement or authorization-required transition—including revocation of the same
URI—invalidates `RenderPublicationGate`, decoded-source caches, ROI/magnifier gates, GPU proxy/LUT,
and source-derived previews before the new identity is committed. Preview, ROI, magnifier, sampling,
and GPU effects all check the reconciled authorization gate before decoding/rendering. A delayed
result carrying an old revision therefore cannot publish after restoration or rapid source
replacement. Export is gated at both the toolbar and the sheet callback; an already-open sheet is
closed on revocation, so an authorization-required source cannot reach decode through stale UI.

`ExportWorkRuntime` remains process-owned. Every run and terminal result carries the exact source
identity plus its authorization generation. A restored RUNNING phase resumes only when both that
identity and the exact run ID remain current; otherwise it becomes RECONCILING and is not retried
automatically. A terminal result is checkpointed and flushed before the runtime's atomic
`claimFinished`, so only one Activity receives it. A source switch or authorization revocation makes
the outcome non-publishable: it is still claimed exactly once and any bitmap is recycled exactly
once. Authorization is checked again after the durability wait, closing the completion-during-switch
race. Success/failure/cancellation terminal UI state is restored without putting a bitmap in the
session. Existing lifecycle disposal still cancels UI render jobs and closes source/native leases;
it does not close the process-owned engine or cancel a durable export transaction.

## Verification

Focused JVM coverage lives in `EditorSessionTest` and `EditHistoryTest`. It covers schema round-trip,
single-authority reconciliation, rotation, every current editor sub-screen boundary,
clipboard/preset anchors, a nonzero mask selection with its overlay closed, per-history-snapshot
validation, an independent typed path-to-expected-value oracle for all 129 persisted Params fields
(including the complete three-mask document and swap/duplicate negative mutations), removed-profile
references in every branch, fractional exact-integer quarantine, corrupt/future state, revoked and replaced
sources, stale-result rejection, export reconciliation, transactional undo/redo restoration,
byte/count eviction, maximum-mask aggregate encode/decode symmetry, one-root retention work, the
drain-before-read checkpoint barrier, and overlapping producer ordering.
The ordering barrier includes the adversarial sequence where the newer offer is physically flushed,
the pending slot becomes empty, and only then the paused older producer resumes.
Retention tests require one—and only one—final-root materialization, exact measured-versus-serialized
node/depth/UTF-8 cost, and fail-closed handling for array, object, decoded-string, scalar-token, and
depth boundaries even when the invalid oldest subtree would otherwise be dropped.

Run the focused tests with:

```powershell
.\gradlew.bat :app:testDebugUnitTest `
  --tests com.spectrafilm.app.AtomicJsonStoreTest `
  --tests com.spectrafilm.app.EditorSessionTest `
  --tests com.spectrafilm.app.EditHistoryTest `
  --tests com.spectrafilm.app.RenderPublicationGateTest
```

The release AndroidTest APK has explicit ticket #139 phases. Before launch, `activity` captures an
immutable complete cursor oracle with the crop tool open; every editor return and Activity recreation
compares live state to that oracle before consulting the mutable session checkpoint. The comparison is
full data-class equality: source identity/authorization, current and committed snapshots, exact
undo/redo snapshots and rotations, category/overlay/mask selection, preset anchors/amount/names/
clipboard, and every export option/sheet/GPS/phase/run-id contract. It then
drives the real Settings/About/Diagnostics/film-curves/print-curves destinations, recreates the real
Activity while a real native export result is process-owned, and requires exactly one terminal claim.
The armed release probe also runs future/IO/pending recipe classification, recovery conflict,
demo-tombstone kill-window, and source-switch/revocation export identity races through production
boundaries; mismatched outcomes are concurrently claimed once and their real bitmaps must be recycled.
It then makes provider source A genuinely live, clears only the optional grade cache to force another
real settle, and pauses that completed `SimResult` at `EditorScreen`'s actual
completion-before-publication boundary while its decoded-source lease is still owned. Source B is
adopted and published through the production path before A is released. A's real publication ticket
must then show that its owning settle coroutine was cancelled, lose the production gate, and finish
cleanup before the cancelled dispatcher boundary can discard the result. Total plus every
memory-domain/stage current admission counter must equal the original one-source baseline (source A
before the hold, equivalently source B after A's cleanup).
There is no separate synthetic publication gate. Before `seed` launches, it writes a separate
schema/version/build/UUID-bound complete oracle under
`noBackupFilesDir/ticket139-editor-oracle/`; the production session writer never uses that directory.
The fixture has the mask-geometry tool open. Recovery validates and loads this pre-seed oracle before
the restored UI can checkpoint, then applies the same complete comparison. `seed` starts a real
process-owned RUNNING export,
verifies its rich checkpoint, and leaves both alive. `recover` runs after a host `am force-stop`; it
must have a different PID, retain rotation/history/clipboard/nonzero mask selection, change the
orphaned RUNNING export to RECONCILING, and gate the revoked RAW identity without rendering or silently
substituting demo pixels.

After assembling, signing, and installing the release app and AndroidTest APK, run:

```powershell
adb shell am instrument -w -e ticket139_phase activity `
  com.spectrafilm.app.test/com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation

adb shell am instrument -w -e ticket139_phase seed `
  com.spectrafilm.app.test/com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation
adb shell am force-stop com.spectrafilm.app
adb shell am instrument -w -e ticket139_phase recover `
  com.spectrafilm.app.test/com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation
```

`Ticket139EditorTestBridge` is the one narrow target-APK ABI used by the separately packaged release
test. Its fixed-fixture calls and primitive counters expose no source URI, recipe, bitmap, native
handle, or session payload. Exact keep rules retain only this bridge, and the release dex check asserts
every method descriptor the AndroidTest APK resolves; app-internal session/store/source classes remain
free for R8 to inline or remove.

An unconditional OS/process kill can interrupt any file write. Recovery guarantees the last
atomically committed checkpoint; a mutation that has not reached the latest-slot writer before an
abrupt kill cannot be reconstructed. Normal Activity recreation is stronger because the replacement
read flushes the same process-owned pending slot before restoration.
