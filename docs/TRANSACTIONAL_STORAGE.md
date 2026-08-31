# Transactional storage and durable URI access

> Implementation contract for ticket #170. This document describes the code as shipped in the
> current Android app; it is not a design proposal.

Spektrafilm keeps an export private until its encoded bytes have been checked, restores only source
URIs that are still authorized and readable, and replaces app-private JSON without exposing a
partially written file. The guarantees differ by storage API, so do not treat every destination as a
database transaction.

## Guarantee matrix

| Storage path | What the app guarantees | Important boundary |
|---|---|---|
| MediaStore image export, API 29+ | A tagged row stays pending while the app copies and verifies the staged bytes. A successful call applies the requested name and publishes that row together, after close and SHA-256 readback verification. | A provider can return an unknown state. The app retains recovery evidence and reports reconciliation pending instead of risking deletion or an ordinary duplicate retry. |
| Public Pictures export, API 24-28 | After the user grants the max-SDK-28 `WRITE_EXTERNAL_STORAGE` runtime permission, the app copies to a same-directory `.part`, flushes and syncs it, verifies its digest, reserves a unique final name with `createNewFile`, and renames the stage over that reservation. A null gallery-index insert is failure. | This path has no `IS_PENDING` barrier. Denial stops before any write. The process lock and exclusive reservation prevent collisions among app exports, but a foreign shared-storage actor can still manipulate the reservation. A hard kill after reservation can leave a zero-byte final placeholder. |
| SAF `ACTION_CREATE_DOCUMENT` output | The app flushes, closes, reopens, and verifies the selected document; it tries to delete an app-created document after failure. | SAF providers do not expose a common pending/commit primitive. Verification is not an atomic visibility guarantee, and cleanup is provider-dependent. |
| App-private preset or recipe | `AtomicFile` preserves the previous committed file if replacement fails. Reads are bounded and strict UTF-8. | The added path lock is process-local. `AtomicFile` itself does not provide cross-process locking. |
| Selected photo or RAW URI | The app records whether access is persisted or transient and verifies the grant and readability after restart. | Moving/deleting a document or revoking a grant requires the user to choose it again. |

Digest equality in this document means that destination bytes equal the completed encoded staging
file. It does not prove render parity with upstream Spektrafilm or equality with the source image.

## MediaStore export transaction (API 29+)

All gallery formats use the same publication path after encoding. JPEG, Ultra HDR, PNG, 16-bit PNG,
16-bit/32-bit TIFF, and scene-linear TIFF are first completed in a unique app-private cache file named
with the `spectrafilm-export-` prefix and `.part` suffix.

### Commit sequence

1. A native encoder must report a positive byte count equal to the staging file length; a bitmap
   encoder must report success, and its completed stage must be non-empty. The app records the
   resulting file's length and SHA-256 digest.
2. Immediately before publication, the app rechecks the staging file length and digest. A changed or
   missing stage fails before a public item is committed.
3. The app inserts one MediaStore row with `IS_PENDING=1`, MIME type, and the
   `Pictures/Spektrafilm` relative path. Its reserved display name is
   `Spektrafilm-pending-tx-<random-UUID>.<sanitized-extension>`, which makes an unjournaled orphan
   discoverable without matching unrelated app media.
4. It synchronously records the returned content URI in the private
   `pending_media_exports_v1` journal. The journal accepts at most 128 tokens.
5. It copies the stage while calculating a second length and SHA-256 digest. It then flushes and
   closes the destination. When the provider returns a `FileOutputStream`, it also calls `fsync`.
6. It reopens the pending URI and requires the readback length and digest to equal the stage.
7. In one `ContentResolver.update()`, it applies the requested display name and sets `IS_PENDING=0`.
   The normal return path requires exactly one affected row.
8. It queries the row and requires the observed state to be published.
9. It removes the URI from the journal and returns success.

The pending row is owner-only until publication under Android's
[`IS_PENDING` contract](https://developer.android.com/reference/android/provider/MediaStore.MediaColumns).
The checked row counts follow the documented return values of
[`ContentResolver.update()` and `delete()`](https://developer.android.com/reference/android/content/ContentResolver.html).

Duplicate display names are valid. Each transaction reserves a distinct row; MediaStore/provider
naming policy decides how those rows appear to the user. `Publish once` applies to one transaction,
not to two separate export requests with the same name.

### Failure handling

| Observed state after a failure | Action |
|---|---|
| `PENDING` | Delete the row. Remove the journal token only after deletion succeeds or the row is confirmed missing. |
| `MISSING` | Remove the journal token. |
| `PUBLISHED` | Treat the commit as successful and retire the journal token on a best-effort basis, even if the provider threw after applying the update. A late cancellation must not delete committed media. |
| `UNKNOWN` | Keep the journal token and throw `ExportReconciliationPendingException`. The app does not delete a destination that might already be published or present it as an ordinary retryable failure. |

Copy errors, out-of-space errors, interrupted close, digest mismatch, a zero/multiple-row publish
update, and a state that remains pending all fail the export unless the provider independently
confirms that the row is already published. No such path intentionally publishes a partial row.

### Activity recreation and export ownership

`ExportWorkRuntime` owns export work in a process-level `SupervisorJob`; the Activity supplies an
immutable decode/render/export request and only observes state. `ExportForegroundService` supplies
foreground scheduling/kill resistance but executes no image code. Rotation or Activity destruction
therefore cancels only the UI waiter, not a staged encode or MediaStore commit. One terminal outcome is
retained until a recreated UI acknowledges it (or for at most five minutes when no UI returns). Any
retained preview is bounded to a 2048-pixel long edge and recycled on expiry/replacement.

Service commands carry the export generation. Both START and STOP are queued through
`startForegroundService`; every `onStartCommand` call promotes first, then applies the command, and a
matching STOP uses `stopSelfResult(startId)`. This closes the fast-completion race where work could
finish before the service received its START command, while preventing a stale completion from
stopping a newer generation. The release-device suite blocks the target main looper, completes and
claims three rapid exports, releases the queued commands, and remains alive beyond Android's
foreground-service watchdog interval.

The transaction state remains authoritative: cancellation or a post-commit preview/reporting failure
after the provider confirms `PUBLISHED` is reported as success and cannot delete the row or encourage a
duplicate retry. A failure before commit remains failure and follows the cleanup/reconciliation table.

### Process-start recovery

`MainActivity` registers the first Activity's stage cutoff synchronously, then launches recovery on an
IO dispatcher. At most one provider/journal recovery is active. A successful run is retained for the
rest of the process; a failed provider/journal query may be retried by a later Activity. The abandoned
stage sweep is different: it consumes the immutable first-Activity cutoff exactly once and is never
rerun in that process, even when provider recovery fails. A recreation therefore cannot advance the
cutoff and delete a live process-owned export stage. Recovery and every API 29+ MediaStore commit also
share one process-wide lock, so they
cannot mutate the journal or provider state concurrently; journal instances use a second shared lock
for their read-modify-write updates.

Recovery takes the union of durable journal tokens and provider-discovered pending rows. Discovery is
deliberately narrow: `IS_PENDING=1`, `OWNER_PACKAGE_NAME` equal to this app, `RELATIVE_PATH` equal to
`Pictures/Spektrafilm` with or without the provider's trailing slash, and a display name beginning
`Spektrafilm-pending-tx-`. This allows recovery to find a row inserted immediately before a hard
process kill even if its journal entry was never written, without sweeping unrelated pending media.

| Observed row state | Recovery result |
|---|---|
| `PENDING` | Delete it. If it was journaled, retire the token only after deletion is confirmed; otherwise provider discovery is the recovery evidence. |
| `PUBLISHED` | Keep the media and retire the stale token. Recovery never republishes it. |
| `MISSING` | Retire the token. |
| `UNKNOWN` | Retain the token for a later retry. |

There is still a time interval between MediaStore insertion and journal insertion, but an app-owned
tagged pending row in the export directory is discoverable on the next process start and is deleted by
the same recovery state machine. Recovery does not happen while the process is dead, and its success
still depends on MediaStore durably retaining and later exposing that row. If provider discovery/query
is temporarily unavailable, recovery fails closed and no new row is reserved; cleanup waits for a
later Activity/process retry or the provider's own pending-item expiration policy. That timing is not
an app-level recovery SLO.

Every new API 29+ publication also runs reconciliation under the same process lock before reserving a
row. If any earlier outcome remains unknown, the new export stops with reconciliation-pending instead
of creating another destination. A provider-confirmed published row is success even when the update
call threw after applying it; this prevents the UI from encouraging a duplicate retry.

### Abandoned staging-file recovery

Normal completion and failure delete each app-private stage in a `finally` block. A hard process kill
cannot execute that block, so first-Activity recovery also scans `cacheDir` for export-prefixed `.part`
files whose modification time predates this process's startup cutoff. It deletes only those
prior-process candidates, leaves unrelated files and current-process stages untouched, and reports any
deletion that must be retried on a later process start. On API 24-28 the same cutoff policy also removes
hidden `.spectrafilm-*.part` siblings from `Pictures/Spektrafilm`.

Legacy recovery cannot safely infer ownership of a final user-facing filename. A hard kill in the
short interval after the exclusive zero-byte reservation and before rename can therefore leave that
zero-byte placeholder for manual cleanup.

See Android's [shared-media storage guide](https://developer.android.com/training/data-storage/shared/media)
for the platform pending-write pattern.

## SAF document writes

Preset JSON and baked LUT documents use `writeVerifiedNewDocument`; preset output is limited to 4 MiB
and LUT text to 64 MiB. This helper is only for a new app-created URI returned by
`ACTION_CREATE_DOCUMENT`; passing an existing document URI would violate its cleanup contract. The
writer:

1. accepts only a `content://` URI and rejects empty or oversized output;
2. opens the URI using `rwt`, writes all bytes, flushes, optionally `fsync`s a `FileOutputStream`, and
   closes it;
3. reopens the URI, bounds the readback, and compares both length and SHA-256; and
4. on any failure, asks the provider to delete the app-created URI, requires one deleted row for
   confirmed cleanup, and propagates the original failure with cleanup failure attached if needed.

All current callers obtain the destination through `ACTION_CREATE_DOCUMENT`. Android does not
overwrite an existing document for that action; a provider can create a unique name such as
`name (1)` instead.
See the [Storage Access Framework guide](https://developer.android.com/training/data-storage/shared/documents-files).

This is verified publication of a newly created document, not universal atomic replacement. Another
observer may see a provider's document while it is being written, a provider may reject `rwt`, `fsync`
is unavailable for a generic provider stream, and a provider may refuse cleanup. The checked delete
result makes that failure visible but cannot force a provider to remove the document. The readback
check proves the bytes visible through that provider at the end of the call.

## Source URI grants and reauthorization

The source-access controller treats a URI as untrusted input. Acquisition requires:

- a non-blank `content://` URI with an authority;
- source kind `PHOTO` or `RAW`;
- a non-blank display name of at most 512 characters; and
- a successful read probe before any grant or private state is changed.

The app then requests a persistable read grant and verifies that it appears in
`ContentResolver.persistedUriPermissions`. If both steps succeed, it stores the reference as
`PERSISTED`. If the provider offers only temporary access, it stores the reference as `TRANSIENT` and
labels the current session accordingly. A grant that existed before acquisition is reused, not taken
again. If saving a newly acquired persisted reference fails, the app releases only that new grant; it
does not revoke a pre-existing grant. After a different replacement reference is durable, the old
persisted grant is released on a best-effort basis.

The private `source_access_v1` record has schema version 1 and stores the URI, `PHOTO`/`RAW` kind,
display name, and `PERSISTED`/`TRANSIENT` access mode. Writes use synchronous `SharedPreferences.commit`
so a reported success is durable before acquisition returns. Unknown record versions or incomplete
records are not restored.

A process-wide FIFO actor serializes acquire, demo-clear, and restore operations. The generation check
skips superseded intents, while FIFO prevents a recreated Activity's restore from overtaking a clear
that the previous Activity already queued. These operations continue after an Activity waiter is
canceled. A current `Ready` result is authoritative over stale `SavedState` (for example, source B may
commit after the old Activity saved source A). If the latest acquire or clear fails, the same queued
operation reads the state that is actually durable and the UI reconciles to it instead of displaying a
source that will change on the next recreation.

In a new process, a `PERSISTED` source is ready only when the grant still exists and the URI still
opens. A `TRANSIENT` reference may remain ready across same-process Activity recreation while the URI
still opens, but normally requires authorization after a cold restart/revocation. Revoked, moved,
deleted, or unreadable content enters `NeedsAuthorization`. The editor blocks decoding, explains that
access expired or the file moved, and offers an indefinite **Choose again** action routed to the
appropriate photo or RAW picker. Choosing the demo source clears the durable source reference and
best-effort releases its persisted grant. Restore also releases surplus grants created in either crash
window around replacement; this app has only one persistable-source slot and no other production
caller takes persistable grants.

The [Photo Picker documentation](https://developer.android.com/training/data-storage/shared/photo-picker)
explains that its default grant is temporary and that apps can request persistent access. The
[SAF documentation](https://developer.android.com/training/data-storage/shared/documents-files) notes
that a persisted grant no longer identifies readable content after a document is moved or deleted.

## Versioned JSON persistence

### Atomic replacement

App-private presets live at `filesDir/presets/<safe-name>.json`; recipes live at
`filesDir/recipes/<64-lowercase-hex-source-key>.json`. Writes use AndroidX `AtomicFile`, which writes and
syncs a replacement before committing it. An interrupted or oversized replacement therefore leaves
the previously committed document readable. Spektrafilm also serializes access to each normalized path
inside one process. See the [`AtomicFile` reference](https://developer.android.com/reference/androidx/core/util/AtomicFile).

The source key is SHA-256 of the source URI string. A recipe records that key again and must match the
filename-derived key before it is applied. The source image itself is never modified.

Delete uses the same per-path lock as AtomicFile read/write and verifies that the base, `.new`, and
legacy `.bak` generations are gone. Read, parse/classify, quarantine, write, and delete share one
reentrant per-path lock, so a stale corruption decision cannot quarantine a valid replacement that
committed in between. Recipes additionally serialize the complete read with save/delete on a per-key
operation gate. A recreated Activity therefore waits for an already executing old-Activity save and
reads its committed document. Reset increments the per-key generation before deletion; both a
pre-reset restore result and any save already inside the gate are fenced. A synchronous edit epoch is
also invalidated at reset intent, before delete is queued, so a still-waiting debounce cannot enqueue
stale edits behind that delete and resurrect them.

### Schema and migration matrix

| Document | Current identifier/version | Accepted older input | Future/foreign input |
|---|---|---|---|
| Preset | `org.spektrafilm.preset`, version `2` | Version 1 without a schema (or with the current schema) is migrated to v2. A legacy bare `masks` array is wrapped as a mask-set v1 document; schema/version are added. | Strict import and persistent-read boundaries reject versions above 2 and any explicit foreign schema. A v2 document must carry the exact schema identifier. |
| Recipe envelope | `org.spektrafilm.recipe`, `recipeVersion: 2` | A v1 envelope is accepted and its nested preset is migrated. The earliest bare-preset form is accepted and bound to the filename-derived source key with empty/default metadata. | Versions above 2 are rejected. A v2 envelope must carry the exact schema identifier. |
| Mask set | `org.spektrafilm.mask-set`, version `1` | A bare adjustments array (legacy v0) is accepted and has the same meaning. | Any other object identifier or version is rejected; future versions are never partially applied. |
| Source reference | Private record version `1` | No older record exists. | Unknown/incomplete records are not restored. |
| Pending export journal | Version encoded by private store name `pending_media_exports_v1` | None. | A later journal requires an explicit new store/migration path. |

`Rejected` describes the schema/codec boundary. An unsupported external SAF import is left untouched
and the import fails. A future/foreign app-private preset and a future-version app-private recipe are
also preserved byte-for-byte and surfaced as unsupported/unavailable, so a newer app can still open
them. Malformed documents within a supported version follow the corruption policy below.

Recipe v2 metadata is also validated: the source key is exactly 64 lowercase hexadecimal characters,
the source name is at most 512 characters, `updatedAt` is non-negative, and manual rotation is one of
`0`, `90`, `180`, or `270` degrees. Recipe save truncates the display hint to the supported length.

The mask-set wire contract and its machine-readable schema are documented in
[MASK_JSON_SCHEMA.md](MASK_JSON_SCHEMA.md).

### Input limits

| Limit | Value |
|---|---:|
| Preset or recipe UTF-8 document | 4 MiB |
| JSON nesting depth | 32 |
| JSON nodes | 200,000 |
| Items in any JSON array | 4,096 |
| Keys in any JSON object | 2,048 |
| Characters in any JSON key or string | 65,536 |
| Characters in a scalar JSON token | 128 |
| Mask adjustments | 64 |
| Components in one mask | 32 |
| Pending export journal tokens | 128 |

Persisted look parameters that can multiply native work are validated even while their effect is
disabled, because a saved value can become active later. The editor uses the same constants as preset
and recipe decoding; current-state `toJsonString`/`encode` calls fail before producing an unsafe
snapshot:

| Operational parameter | Accepted persisted domain |
|---|---:|
| Spectral Gaussian blur | `0..20` samples |
| Upscale factor | exactly `0` (disabled sentinel), or `0.5..4` |
| Camera film format | `8..120` mm |
| Grain particle area | `0.2..2` µm² |
| Each grain particle-scale component | `0.1..5` |
| Each grain sublayer-scale component | `0.25..5` |
| Each grain minimum-density component | `0..0.5` |
| Grain sublayers | `1..5` |
| Halation bounces | `1..5` |

The native sampler is defense in depth, not a replacement for these document bounds. If the initial
binomial probability underflows to exactly zero, it returns the same inevitable `n` result in O(1)
after consuming the original uniform draw, preserving the subsequent deterministic RNG stream.
Non-finite or unrepresentable Poisson rates cannot reach `llround`, and a non-positive/non-finite
derived particle count makes the particle stage an exact no-op. Valid-domain arithmetic and RNG order
are unchanged.

Provider imports are streamed only to the byte limit and decoded as strict UTF-8 before JSON parsing;
the app does not use an unbounded `readBytes()` allocation. A non-allocating RFC 8259 lexical preflight
enforces syntax, depth, collection, string, node, and scalar-token limits before object construction.
The bounded exact parser retains numeric tokens as `BigInteger`/`BigDecimal`, preventing Android's
usual `Double` rounding from turning a fractional or huge schema version into an accepted integer.
The constructed object tree is validated again afterward, including finite-number checks.

### Corruption quarantine

When an existing app-private document is proven corrupt, the app attempts to remove it from the active
namespace and rename it beside the original as:

```text
<name>.json.corrupt-<epoch-millis>[-<collision-suffix>]
```

For presets, proven corruption includes oversize or malformed UTF-8 content and parse/validation
failure within a supported schema. A future version, foreign schema, or other potentially transient
read I/O failure is preserved in place. For recipes, oversize content or supported-version
parse/validation failure enters quarantine; a future recipe version and read I/O failure are preserved
and reported unavailable. A recipe whose corrupt bytes cannot be moved is reported separately as a
quarantine failure, so autosave does not silently overwrite it.

When quarantine succeeds, the bytes are preserved for diagnostics instead of being silently
overwritten. Quarantine itself is best-effort because a filesystem error can also prevent the rename.
Recipe restore distinguishes missing, unsupported/unavailable, quarantined, and quarantine-failed
state. External SAF imports are rejected in place; Spektrafilm does not rename or delete a user's input
document.

## Verification coverage

The JVM suites exercise the failure/state machine without provider nondeterminism:

- `ExportTransactionTest`: encoder count, copy/close/readback failures, cleanup retry, checked publish,
  provider-throws/cancellation-after-publish success, duplicate names, discovered unjournaled rows,
  one-shot prior-process stage cleanup, and recovery of pending/published/missing rows with retained
  retry evidence;
- `AtomicJsonStoreTest`: interrupted and oversized replacement, bounded input, malformed UTF-8,
  pre-parse lexical/structural/token limits, non-finite runtime numbers, locked full-generation delete,
  and quarantine;
- `SourceAccessTest`: persisted/transient acquisition, revoked or unreadable restore, URI validation,
  new-vs-pre-existing grant rollback, old-grant release, FIFO acquire/clear/recreated-restore ordering,
  canceled Activity waiters, and latest-operation failure reconciliation;
- `ExportForegroundServiceGenerationGateTest`: matching-generation stop, stale-stop rejection, and
  newer-generation survival;
- `RecipeOperationGateTest`: an already executing save wins before a recreated read; and
- `RecipeDocumentCodecTest` and `MasksRoundTripTest`: version migrations, metadata validation,
  partial-future-envelope rejection, round-trip fidelity, operational resource-amplification limits,
  and future-version rejection; and
- `PresetsRoundTripTest`: exact-number/type validation, rejected operational values at both sides of
  every supported domain, accepted endpoints, zero-upscale sentinel semantics, and detached validation
  that cannot partially mutate live editor state.

The native `test_grain` seam regressions cover the binomial underflow result, its one-draw RNG contract
and bounded runtime, Poisson non-finite/overflow conversion, invalid derived particle counts, and the
existing grain mean/noise/locality oracle.

The default release-candidate instrumentation invokes `StorageReliabilityChecks` on Android. It uses
fresh backend/journal instances to model adapter reopen, device-runtime fault injection for out-of-space
and interrupted-close failures, a test-only provider to take a real persistable grant and then
release/revoke it, and real MediaStore rows to verify duplicate-name publication and byte readback.
ENOSPC and interrupted close remain deterministic injected faults, not physical disk exhaustion.
The grant provider is implemented in Java so that provider itself adds no Kotlin runtime dependency
inside the standalone Android-test package. The overall test package does invoke target-side
Kotlin/coroutine ABI, which is retained by exact keep rules and verified against the physical minified
DEX before device installation.

A separate host-orchestrated `seed`/force-stop/`recover` sequence is the real process-death gate. Phase
one leaves one tagged pending MediaStore row, production journal token, display-name evidence, and PID
durable. Phase two requires a different PID, verifies the exact row/token survived, runs production
recovery, and confirms both are gone. Both phases emit explicit `PASS`/`FAIL` markers.

Ticket #172's 2026-08-31 API 36 SM-S948W replay ran the full minified release instrumentation twice,
including Activity recreation and the deterministic foreground-service race. Its process-death phases
used the same durable token `content://media/external/images/media/216358`; recovery passed after a
real `force-stop`. The pulled app and test APKs exactly matched the locally signed candidates documented
in [JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md). No uninstall or app-data clear was used.

### Offline JVM command

Run from the repository root with Android Studio's bundled JDK (or another project-compatible JDK).
All dependencies must already be present in the Gradle cache:

```powershell
.\gradlew.bat --offline :app:testDebugUnitTest `
  --tests "com.spectrafilm.app.ExportTransactionTest" `
  --tests "com.spectrafilm.app.AtomicJsonStoreTest" `
  --tests "com.spectrafilm.app.ExportForegroundServiceGenerationGateTest" `
  --tests "com.spectrafilm.app.SourceAccessTest" `
  --tests "com.spectrafilm.app.RecipeOperationGateTest" `
  --tests "com.spectrafilm.app.RecipeDocumentCodecTest" `
  --tests "com.spectrafilm.app.masks.MasksRoundTripTest"
```

### API 29+ device gate

The complete storage gate requires an API 29+ device with writable external MediaStore. The custom
instrumentation targets the minified `release` app, so a repository without `keystore.properties`
produces an unsigned target APK. For local testing only, sign a copy with the repository's public debug
key so it matches the Android-test APK. Never distribute this locally signed candidate.

```powershell
# Build offline, align and locally sign both APKs, then run the complete gate.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\android\run_release_device_gate.ps1 `
  -Build `
  -Serial '<adb-serial>'

# Or verify a specific already-signed candidate pair without rebuilding it.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\android\run_release_device_gate.ps1 `
  -AppApk '<path-to-signed-app.apk>' `
  -TestApk '<path-to-signed-androidTest.apk>' `
  -Serial '<adb-serial>'
```

`run_release_device_gate.ps1` is PowerShell 5.1-compatible and fail-closed. One centralized native
invoker checks every Gradle/build-tool/ADB exit code. Before either replace-only install, it pulls any
currently installed app and test base APK and requires their single signer to match the candidate pair;
an absent package is allowed, but a mismatched installed signer aborts without changing device data.
It then requires the registered runner, runs the full release instrumentation twice, rejects every
`FAIL`, and asserts all ticket #170/#172 `PASS` markers plus `INSTRUMENTATION_CODE: -1`. Its real
`seed -> force-stop -> recover` sequence requires the same token in both phases. Finally it pulls both
installed packages and requires candidate-equal SHA-256 bytes and signer digests before emitting
`RELEASE_DEVICE_GATE: PASS`.

`-ExecutionPolicy Bypass` above applies only to that child PowerShell process; it does not change the
machine or user execution policy. It keeps the documented command usable when direct `.ps1` launch is
disabled. Omit it only on a machine whose existing policy already permits the checked-in script.

If install reports `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, the installed target uses another signing
key. Back up any app-private work before explicitly uninstalling it; do not make uninstall an automatic
test step. The instrumentation deletes its two successfully published duplicate-name images in a
`finally` block and deletes its process-recovery row. If the runner itself is killed, launch the target
once to recover tagged pending rows and prior-process cache stages, then remove any published
`Spektrafilm_ticket170_duplicate.png` test images from the gallery. You can remove only the test package
afterward with:

```powershell
& $adb uninstall com.spectrafilm.app.test
```
