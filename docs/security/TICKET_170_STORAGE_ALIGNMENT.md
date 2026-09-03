# Best Practices and Security Alignment Update — Ticket #170

Date: 2026-08-30

Area: Android storage, URI authorization, export publication, and local JSON documents

Impact: data integrity, least privilege, privacy, and crash/recreation safety
Priority: release-blocking / high

## Files in scope

- `app/src/main/java/com/spectrafilm/app/ExportTransaction.kt`
- `app/src/main/java/com/spectrafilm/app/ExportWorkRuntime.kt`
- `app/src/main/java/com/spectrafilm/app/ImagePipeline.kt`
- `app/src/main/java/com/spectrafilm/app/SourceAccess.kt`
- `app/src/main/java/com/spectrafilm/app/AtomicJsonStore.kt`
- `app/src/main/java/com/spectrafilm/app/Presets.kt`
- `app/src/main/java/com/spectrafilm/app/Recipes.kt`
- `app/src/main/java/com/spectrafilm/app/RecipeWorkRuntime.kt`
- `app/src/main/java/com/spectrafilm/app/RecipeDocumentCodec.kt`
- `app/src/main/java/com/spectrafilm/app/VerifiedDocumentWriter.kt`
- `app/src/main/java/com/spectrafilm/app/masks/MaskJson.kt`
- `app/src/main/AndroidManifest.xml`
- ticket-specific JVM and release-instrumentation tests under `app/src/test` and `app/src/androidTest`

## Alignment summary

### Least privilege and URI boundaries

- Source and SAF document boundaries accept only `content://` URIs with an authority; file and network
  schemes are rejected before persistence or output.
- Source access requests read permission only. A persistable grant is taken only when offered and is
  verified through `persistedUriPermissions`; transient access is labeled and reauthorization is
  explicit when readability is lost.
- Production has one persisted-source slot and one caller of `takePersistableUriPermission`. Replacing
  or clearing that slot releases its old grant; recovery reconciles crash-window surplus grants.
- API 29+ gallery writes need no broad media permission. API 24-28 declares
  `WRITE_EXTERNAL_STORAGE` with `maxSdkVersion=28`, requests it only when export is invoked, rechecks it
  at the write boundary, and performs no public write after denial.
- `ExportForegroundService` is not exported. `POST_NOTIFICATIONS` affects visibility of its ongoing
  notification, not authorization to read or write images.

### Fail-closed publication and cleanup

- Encoded bytes remain app-private until complete. MediaStore rows remain `IS_PENDING=1` through close,
  length/SHA-256 verification, and readback; one checked update applies the requested name and publishes.
- Provider state, not coroutine cancellation, decides commit. `PUBLISHED` is success; `PENDING` is
  deleted; `UNKNOWN` retains journal evidence and blocks an ordinary retry.
- Pending-row discovery is restricted to this package, the exact export directory, and an app-specific
  random pending-name prefix. Recovery does not sweep other apps' or untagged rows.
- SAF cleanup applies only to a newly created `ACTION_CREATE_DOCUMENT` URI. The app never uses the
  helper to delete an arbitrary existing user document.
- Process-owned export work and FIFO source/recipe queues survive Activity cancellation. This removes
  lifecycle gaps that could otherwise republish, resurrect, or overwrite state.

### Untrusted JSON and private paths

- Provider reads are byte-bounded and strict UTF-8. A bounded RFC 8259 parser limits nesting, nodes,
  array/object sizes, strings, and scalar tokens before building an object tree.
- Numeric tokens remain `BigInteger`/`BigDecimal` until schema/version validation, preventing
  device-only `Double` rounding bypasses. Future/foreign documents are rejected without mutation.
- Presets and recipes share editor/native operational bounds for spectral blur, upscale, film format,
  grain particle area/scales/density, sublayer count, and halation bounce count. They are checked even
  when an effect is inactive, preventing a dormant imported value from later amplifying allocation or
  sampler work. Current-state encoders fail fast, and validation decodes imports into detached state,
  so a rejected late field cannot partly apply.
- Native defense preserves valid-domain output/RNG order while eliminating the probability-underflow
  O(n) walk, guarding Poisson integer conversion, and making an invalid derived particle count a
  deterministic no-op before the sampler.
- Preset names are normalized into the app-private preset directory; recipe filenames require a
  64-character lowercase hexadecimal source key. No user string is accepted as a filesystem path.
- Atomic replacement shares a reentrant path lock with classification/quarantine. Recipe operations
  also share a per-key gate and process FIFO, preventing stale bytes from quarantining or overwriting a
  newer valid document.
- Corrupt app-private documents are moved to bounded sibling quarantine names. External SAF imports
  are rejected in place and are never quarantined or deleted.

### Privacy

- GPS copying remains opt-in through the export option; ticket #170 does not broaden metadata access.
- Source display names and URI strings stay in app-private state. Instrumentation uses a scoped test
  provider and verifies that unrelated persisted grants remain unchanged.
- No credentials, signing secrets, or remote telemetry were introduced.

## Representative implementation diff

```diff
+ val pendingAcquire = sourceRuntime.submitReconciled(token) {
+     sourceAccess.acquire(uri.toString(), kind.name, displayName.take(512))
+ }
+ val outcome = pendingAcquire.await() // waiter may cancel; process FIFO work continues

+ val pendingRestore = RecipeWorkRuntime.submit {
+     val before = Recipes.generation(recipeKey)
+     val result = Recipes.readResult(context, recipeKey)
+     Triple(before, result, Recipes.generation(recipeKey))
+ }

+ val updated = backend.publish(token, spec)
+ if (updated != 1 || backend.state(token) != PUBLISHED) failClosed()
```

## Verification

Offline gate:

```powershell
.\gradlew.bat --offline `
  :app:testDebugUnitTest :app:testReleaseUnitTest :app:lintRelease `
  :app:assembleRelease :app:assembleReleaseAndroidTest
```

The targeted suites cover short/out-of-space writes, interrupted close, digest mismatch, cancellation
after publication, duplicate names, process-recovery evidence, immutable stage cutoffs, URI scheme and
grant failures, FIFO recreation races, malformed/oversized/exact-number JSON, migrations, quarantine
TOCTOU, operational resource-amplification bounds, malicious recipe envelopes, and recipe
reset/autosave ordering. Native seam tests verify the underflow result/RNG draw, bounded completion,
overflow conversion, invalid derived counts, and the original grain oracle. Release instrumentation
additionally uses real MediaStore
rows and a real persistable-grant provider. The required device sequence and signing/alignment commands
are maintained in [TRANSACTIONAL_STORAGE.md](../TRANSACTIONAL_STORAGE.md).

Connected-device coverage is API 36. It certifies the scoped-storage branch, signed R8 candidate,
process-death recovery, and provider grant/revocation. API 24-28 permission grant/denial remains a
separate compatibility-matrix requirement because that platform branch cannot execute on API 36.

## Platform references

- [Android Photo Picker](https://developer.android.com/training/data-storage/shared/photo-picker)
- [Storage Access Framework](https://developer.android.com/training/data-storage/shared/documents-files)
- [Shared media and pending writes](https://developer.android.com/training/data-storage/shared/media)
- [`MediaStore.MediaColumns.IS_PENDING`](https://developer.android.com/reference/android/provider/MediaStore.MediaColumns)
- [`ContentResolver` checked update/delete contracts](https://developer.android.com/reference/android/content/ContentResolver)
- [AndroidX `AtomicFile`](https://developer.android.com/reference/androidx/core/util/AtomicFile)
