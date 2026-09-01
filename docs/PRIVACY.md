# Spektrafilm Privacy Policy and Data Safety Mapping

**Effective date: 2026-08-31**

This is the canonical privacy policy for the current Spektrafilm Android app. It also maps the
implemented behavior to the facts the publisher must enter in the current Google Play Data Safety
form. Privacy or support questions can be filed with the project maintainers through
[GitHub Issues](https://github.com/thetechgeekko/Spektrafilm-android/issues).

Spektrafilm has no analytics SDK, advertising SDK, account system, automatic crash uploader, or
developer-operated image service. Image processing and export run on the device. Data leaves the
device only through a user action: checking for updates or opening the GitHub release page, choosing
a Copy/Share target for diagnostics, publishing an image export, or participating in a device setup
transfer that the device manufacturer provides.

## Data flow and retention

| Data | Normal location and lifetime | When it can leave the device | Backup/transfer policy |
|---|---|---|---|
| Original photo/RAW | Remains at the user-selected `content://` provider; decoded pixels and full-resolution renders are memory/cache working data | Only when the user exports to MediaStore/SAF or another chosen system destination | Never allowlisted; only the two named records are eligible where OEM transfer rules are honored, and cache/no-backup directories are excluded by Android |
| Source URI capability | Android persistable read grant plus private `source_access_v1` URI/display-name record | No app transfer path | Outside the exact include-only allowlist; OS URI grants cannot be restored on another device |
| Settings and back-exit hint | App-private preferences/DataStore | Some manufacturer-specific device-to-device implementations may transfer the two allowlisted records | Standard Auto Backup and standard cross-platform participation are disabled; defense-in-depth XML allowlists only these two records where an OEM honors the rules |
| User presets and recipes | Versioned JSON in app-private `presets` and `recipes` directories | Only when the user explicitly exports a preset through the system picker | Outside every exact include-only allowlist, including AtomicFile `.new`/`.bak` and corrupt-quarantine siblings |
| Render/export working data | Native/managed memory and cache staging files; prior-process stages are recovered or deleted | A completed export is published only after the user's Export action | Never included; Android excludes cache/code-cache/no-backup, and all other working paths are absent from the exact include-only rules |
| Pending export journal | Private `pending_media_exports_v1` MediaStore URI tokens, retained only until commit/recovery | No | Outside the exact include-only allowlist as device-local transaction state |
| Diagnostics | One private crash record, at most 64 KiB and seven days; an on-demand process log snapshot, at most 500 lines/128 KiB; a final report, at most 192 KiB | Only after Copy or Share; the chosen Android target then receives the text | The `diag` directory is outside the exact include-only allowlist |
| Update metadata | One bounded response to a user-tapped Check for updates | GitHub receives the ordinary HTTPS request, including IP transport data and the `Spektrafilm-Android` User-Agent | Nothing is persisted or backed up |

## Backup and restore boundary

`android:allowBackup="false"` disables cloud/standard Auto Backup and standard cross-platform
transfer participation. Android documents that some manufacturers may still permit device-to-device
transfer for apps targeting Android 12 or higher even when this flag is false. The two XML rule sets
therefore remain as defense in depth where that OEM implementation honors them:

- `app/src/main/res/xml/backup_rules.xml` covers Android 11 and lower. Its only includes are the
  ordinary settings file and non-sensitive back-exit hint, and each requires client-side encryption.
- `app/src/main/res/xml/data_extraction_rules.xml` covers Android 12 and higher. Its dormant cloud and
  device-transfer sections use that same two-record allowlist; cloud additionally requires encryption
  capability.

Presets and recipes are intentionally absent from the exact include-only rules. Android backup paths
do not support wildcard selection of only active `*.json` files, while these directories can also
contain AtomicFile intermediate/backup generations and quarantined invalid documents. Leaving the
directories ineligible prevents stale, corrupt, or future-format bytes from being transferred.

Android's include semantics make every unlisted path ineligible; redundant `<exclude>` elements for
paths outside those includes are invalid and intentionally absent. Consequently originals/external
files, URI state, pending exports, diagnostics, extracted engine assets, future secrets, updater
state, full-resolution caches, transient exports, presets, and recipes are not eligible. Cache,
code-cache, and no-backup directories are excluded by Android regardless of XML rules.
`android:restoreAnyVersion="false"` rejects restoration from a newer app into an older
version; versioned document codecs independently reject unsupported future documents.

Android 16 QPR2/API 36.1 introduced a separate cross-platform-transfer schema that requires a real
counterpart bundle ID, team ID, and content version. This project targets/compiles API 34 and has no
such iOS identity, so none is invented. Standard participation stays disabled. Enabling backup later
requires a compatible toolchain, genuine platform identity, explicit rules/tests for every transfer
mode, and a matching update to this policy.

See Android's [Auto Backup documentation](https://developer.android.com/identity/data/autobackup).

## Diagnostics privacy contract

The crash handler stores a versioned record locally and chains to Android's normal crash handler. It
does not transmit anything. A process-start retention sweep removes records with an unknown format,
an impossible future timestamp, or an age over seven days even if the Diagnostics screen is never
opened. An oversized restored/corrupt record is redacted and compacted to the 64 KiB limit; malformed
UTF-8 is rejected.

Before persistence, display, copy, or share, diagnostics redact:

- `content://`, `file://`, and other URI-shaped values, including values containing spaces;
- absolute Unix/Windows paths and common image filenames, including names containing spaces;
- URI/path/file/source-name fields; and
- GPS, EXIF, camera, and lens metadata fields.

The report intentionally retains app version, device manufacturer/model, Android/API version, stack
locations such as `MainActivity.kt:123`, image dimensions, stage names, timings, and error classes.
Those fields help reproduce a defect. The Diagnostics screen explains this before capture/export.
Copy and Share are explicit user actions; there is no background upload.

## Update, network, and integrity boundary

The app's only direct network operation is the user-tapped advisory release check described in
[UPDATER_SECURITY.md](UPDATER_SECURITY.md). Cleartext is denied in the manifest and Network Security
Configuration. Code connects only to the exact `api.github.com` repository endpoint and can open only
the matching canonical `https://github.com/.../releases/tag/...` browser page.

The app never downloads an APK, never requests package-install permission, and never claims to
cryptographically verify APK bytes or a release-metadata signature. GitHub HTTPS protects advisory
metadata in transit; Android package-signature continuity is the install-time upgrade gate. The
browser/distribution channel remains responsible for first-install provenance.

## Permissions and foreground service

| Permission | Purpose and user-visible behavior |
|---|---|
| `INTERNET` | One user-tapped advisory GitHub release check; there is no automatic polling |
| `FOREGROUND_SERVICE` and `FOREGROUND_SERVICE_DATA_SYNC` | Keep a user-started local image export alive while it processes/writes data; the non-exported service shows an ongoing export notification and performs no network sync |
| `POST_NOTIFICATIONS` (API 33+) | Show the ongoing-export notification; denial does not grant another access path |
| `WRITE_EXTERNAL_STORAGE` (`maxSdkVersion=28`) | Publish to public Pictures on Android 7-9; Android 10+ uses MediaStore without it |

Spektrafilm does **not** request location, camera, microphone, contacts, broad media read,
`MANAGE_EXTERNAL_STORAGE`, `READ_LOGS`, `REQUEST_INSTALL_PACKAGES`, or background-location access.
Source selection uses Android's picker/Storage Access Framework and only the URI-scoped grant returned
by that action. Android documents import/export and local file processing under the
[`dataSync` foreground-service type](https://developer.android.com/develop/background-work/services/fgs/service-types).

## User controls and deletion

- Clear the retained crash record from the Diagnostics screen at any time.
- Clear the app's storage or uninstall Spektrafilm to remove its private settings, recipes, presets,
  URI records, diagnostics, and cached work. Android also revokes the app's URI grants on uninstall.
- Delete published exports from the destination/gallery that received them.
- Manage manufacturer device-transfer choices in Android's device-setup and backup controls.
- A GitHub request or a report sent to a chosen third-party app is then subject to that service's
  privacy and deletion controls.

## Google Play Data Safety mapping

For this build, the implementation supports these publisher declarations:

- no user data is automatically collected by or sent to a Spektrafilm-operated server;
- no analytics, advertising, account, or automatic diagnostics collection exists;
- images are processed locally and leave only through destinations the user chooses;
- standard cloud backup and standard cross-platform transfer participation are disabled;
- a vendor-specific D2D path may transfer only the two non-sensitive records described above where
  the vendor honors Android's data-extraction rules; and
- a diagnostics report reaches a third party only when the user explicitly selects Copy/Share and a
  destination.

Before distribution, the publisher must compare the exact built artifact and every bundled SDK with
the current [Google Play Data Safety guidance](https://support.google.com/googleplay/android-developer/answer/10787469)
and transpose this mapping into the current Play Console form. That submission is a release gate. If
telemetry, support upload, accounts, cloud editing, a new SDK/permission, backup participation, or a
different update/download flow is added, the code, in-app disclosure, this policy, tests, and store
form must change together.
