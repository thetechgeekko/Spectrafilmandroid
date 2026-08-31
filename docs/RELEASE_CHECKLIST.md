# Maintainer Release Checklist

> **Release hold (2026-08-29):** do not tag the current tree. The active blockers and ordered
> implementation plan live in [PRODUCTION_READINESS_PLAN.md](PRODUCTION_READINESS_PLAN.md) and the
> [production-readiness Wayfinder map](https://github.com/thetechgeekko/Spektrafilm-android/issues/164).
> The release pipeline now builds without secrets, transfers a hash-bound unsigned candidate, and
> signs only inside the protected Environment. The remaining LibRaw/security/licensing and
> color/output blockers on the map must still close before tagging.
> If the artifact is a Google Play update, target SDK 36 must replace target SDK 34; a GitHub-only
> route must be an explicit owner decision and still pass the Android 16 behavior matrix.

Releases are **automated**. Pushing a stable `vMAJOR.MINOR.PATCH` tag (no
prerelease/build suffixes or leading-zero components) triggers
`.github/workflows/release.yml`, which resolves one immutable commit, qualifies
the pinned LibRaw dependency, then builds and publishes a production-signed APK
plus verified source/relink, SPDX, dependency-lock, mapping, native-symbol,
runtime-classpath, provenance, and checksum artifacts. Signing and publication
are available only through the protected `release-signing` Environment. **APKs
are not committed to the repository** — there is no `dist/` directory
and you should never copy a built APK into the repo.

Current in-tree version: **v0.9.0 / versionCode 11** (`minSdk 24`, `targetSdk`/`compileSdk 34`);
latest released tag is **v0.9.0** (tagged 2026-08-26).

Ticket #168's local test candidate was validated on 2026-08-30 on an API 36
SM-S948W: both release APKs installed, instrumentation passed, the pulled
`base.apk` was byte-identical, R8/JNI and 16 KiB checks passed, and cold launch
completed without app-fatal logs. That test used the repository's public debug
certificate solely to exercise the mechanics; it is not production-signing
evidence and cannot satisfy the protected Environment gate.

Ticket #170 adds a release-targeted transactional-storage suite. Before a
candidate is accepted, run its normal storage smoke plus the documented
`seed -> force-stop -> recover` phases on a real API 29+ device. The recovery
phase must remove the exact app-owned pending row and durable journal entry;
there must be no tagged pending export left behind. Also retain JVM coverage for
encoder/stream failure, digest mismatch, out-of-space injection, future schema
versions, corrupt/oversized JSON, duplicate names, canceled Activity waiters,
FIFO source/recipe races, and revoked URI grants. Rotate/recreate the Activity
during an export and confirm the retained terminal result is delivered without a
second publication. API 24-28 candidates must separately verify legacy storage-
permission grant and denial; an API 36 device cannot certify that branch. See
[TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md) for the exact commands and
the limits/guarantees being certified.

[Harden JNI lifetime, buffer bounds, cancellation, and render-close races](https://github.com/thetechgeekko/Spektrafilm-android/issues/172)
adds a second mandatory native-safety layer. Before accepting a candidate, require the fixed shared
host inventory (four ASan+UBSan and four TSan suites), the standalone engine JNI boundary runner with
`ENGINE_BOUNDARY_INSTRUMENTATION: PASS` and instrumentation code `-1`, and the release-targeted app
runner's native-result Activity-recreation marker. The release gate must also resolve the separately
packaged AndroidTest APK's exact target classes, field types, method prototypes, facade ancestry, and
interface dispatch in the physical minified DEX; member-name checks alone are insufficient. Retain
the deterministic foreground-service rapid-completion/watchdog regression and native-owned direct-
buffer byte-order tests. The host sanitizer runner covers native helpers, the real C render/cancel
path, allocation-token races, and writers; it does not claim sanitizer instrumentation of the Android
JNI bridge. The exact ownership and evidence contract is in
[JNI_LIFETIME_SAFETY.md](JNI_LIFETIME_SAFETY.md). Local connected-device qualification must use the
fail-closed `tools/android/run_release_device_gate.ps1` helper documented in
[TRANSACTIONAL_STORAGE.md](TRANSACTIONAL_STORAGE.md), not an unchecked sequence of native commands.

Ticket #172's final local candidate passed on 2026-08-31 on an arm64 API 36 SM-S948W. The app and
AndroidTest SHA-256 values were
`EE1A1BA636FA123C93186EB4A3E80E964281080462B5AB8E24B790DE361318E8` and
`6368EF2697FFD4A557E4418C22B7052F831D518AE4340E04473776CABB6E0F4A`; pulled installed bytes matched
both. Full instrumentation passed twice, process-death recovery preserved and recovered the exact
token, standalone engine/LibRaw suites passed, and cold launch had no app-scoped fatal log. As with
#168, the repository debug key was used only for local mechanics; this is not production-signing
evidence.

---

## 1. How a release happens (the automated flow)

`.github/workflows/release.yml` fires on:

- a pushed tag matching `v*` (e.g. `v0.7.0`), or
- a manual `workflow_dispatch` with an existing tag name as input.

The `resolve-release` -> `qualify-libraw` -> `build-release-candidate` ->
`sign-and-publish` chain then:

1. Validates the tag format, resolves its peeled commit SHA, binds a tag-push run
   to the event commit, and checks out that exact SHA.
2. Verifies the official LibRaw archive plus all hashed patches; runs serial and
   OpenMP ASan/UBSan, serial TSan, and bounded public-seam fuzz gates.
3. In a job with no Environment and no signing secrets, verifies the official
   Gradle wrapper JAR and distribution hashes, installs **NDK `27.0.12077973`,
   CMake `3.22.1`, build-tools `35.0.0`**, and checks that the tag exactly matches
   the literal app `versionName`.
4. Runs the engine parity suite at `-O2` and at the shipping
   `-O3 -ffast-math -fno-finite-math-only` flags, then release JVM tests,
   LibRaw JVM tests, and `:app:lintRelease`.
5. Builds an **unsigned**, R8-minified app APK and a release-targeted custom
   instrumentation APK. It verifies package/version/runner identity, R8's
   name-based JNI members, the AndroidTest APK's exact target-DEX ABI (classes,
   fields, method prototypes and hierarchy dispatch), the R8 mapping, full native
   symbols, and 16 KiB ZIP plus every 64-bit ELF `PT_LOAD`; `apksigner` must reject
   the app as unsigned.
6. Verifies the app GPL/NOTICE plus the three pinned LibRaw legal files in that
   candidate, then creates a canonical stored source/relink ZIP and SPDX sidecar
   from the authenticated archive and exact checked-out source. It verifies with
   `--require-resolved`. An `UNRESOLVED`/unsupported route, source or patch drift,
   missing APK notices, a malformed SBOM, or a checksum
   mismatch fails before publication. A green tool result is technical evidence,
   not legal approval.
7. Generates a deterministic app SPDX document from the exact six Gradle
   lockfiles and release runtime classpath. It archives those six locks exactly,
   retains the LibRaw SPDX/source bundle, R8 mapping and full native symbols, and
   emits a fixed gate attestation bound to source SHA, version, run ID/attempt,
   and both unsigned APK hashes.
8. Uploads one run/attempt-specific candidate artifact and exposes its numeric
   artifact database ID and service-computed digest. Only then enters the
   protected `release-signing` Environment, downloads by that exact ID, verifies
   its digest/inventory/manifest/attestation, recomputes both SPDX documents and
   all lock bytes, and rechecks the tag. No Gradle task runs in the protected job.
9. Aligns and production-signs the exact app and instrumentation APKs with pinned
   build-tools `35.0.0`. The key exists for that step only; an `if: always()` step
   proves it and any untracked keystore are gone before certificate verification.
   Both APKs must have exactly one signer matching `RELEASE_CERT_SHA256`.
10. Rechecks the signed app's R8/JNI, legal, ZIP, and every 64-bit ELF invariant.
    An API 35 emulator installs both signed APKs, pulls installed `base.apk` and
    byte-compares it with the local signed APK, runs the release instrumentation,
    cold-launches `MainActivity`, and rejects app-scoped fatal Java/JNI/native logs.
11. Stages the app APK, both SPDX documents, LibRaw source/relink ZIP, six-lock
    archive, R8 mapping, full native symbols, runtime classpath, provenance, and
    checksums. Provenance binds source/tag/version, run ID/attempt, candidate
    artifact ID/digest, signer, unsigned/signed/installed/test APK hashes, and all
    release gates.
12. The publisher first requires repository immutable releases to be enabled. A
    new Release stays draft while every operation is bound to numeric release and
    asset IDs and all downloaded remote bytes are verified. It publishes only
    after a final tag check. A rerun accepts only an already immutable,
    byte-identical Release. Recovery may delete only the exact run-owned draft;
    it never deletes published or foreign releases.

`release.yml` therefore reruns the candidate-critical gates on the exact bytes it
publishes. Standing CI must still be green before tagging, but it is not accepted
as a substitute for an absent, failed, stale-run, or stale-attempt release gate.
The LibRaw qualifier emits the current run ID/attempt/source SHA and the candidate
job rejects a receipt from any earlier attempt. After any failed release job, use
**Re-run all jobs**; a partial "re-run failed jobs" intentionally fails closed.

---

## 2. Pre-tag checklist

- [ ] Confirm a repository ruleset covers `refs/tags/v*` and forbids tag updates
  and deletion. Workflow SHA rechecks detect movement while a run is active;
  the ruleset keeps the published tag immutable afterward.
- [ ] Bump `versionCode` and `versionName` in `app/build.gradle.kts` (currently `11` / `"0.9.0"`).
- [ ] Update `CHANGELOG.md` for the new version.
- [ ] Note: the release build now runs **R8** (`isMinifyEnabled = true`, `app/build.gradle.kts:64`).
  This is **Stage 1 — shrink only, `-dontobfuscate`** (`app/proguard-rules.pro:2`), with explicit
  keep-rules for the four name-based JNI boundaries (`com.spectrafilm.engine.**`, `RawDecoder`,
  `TiffWriter`, `PngWriter`, `native <methods>`) and enum value/`valueOf` persistence. Because the
  JNI symbols resolve classes/methods by literal string from C++, a missing keep-rule surfaces only
  at runtime — sanity-check that a release build still loads native libs and exports/decodes before
  publishing. The release workflow now scans the shrunk DEX and runs the exact signed
  candidate on API 35; `android-emulator` remains an advisory debug-build job. R8 Stage-2 +
  `shrinkResources`: see `docs/AUDIT.md` §D (which owns that open item).
- [ ] Confirm CI is green on `main`. The relevant gating jobs in `.github/workflows/ci.yml` are:
  - `engine-native` — host C++ build of libspektra.
  - `engine-parity` — the stage parity gate (deterministic goldens, thread-invariance).
  - `libraw-hostile` — shipping-serial and OpenMP-required sanitizer suites,
    serial TSan first-use regression, and both bounded fuzz targets.
  - `parity` — the `.spkvec` comparator self-test.
  - `python-lint` — byte-compile plus deterministic compliance and release-policy tests.
  - `android` — JVM unit tests + **`:app:lint`** (`ci.yml:379-388`; a hard gate —
    `abortOnError = true`, baseline at `app/lint-baseline.xml`) + full debug assemble,
    an NDK 27 x86_64 standalone-recipient relink smoke (SONAME, JNI/marker exports,
    and 16 KiB `PT_LOAD` alignment),
    **including the 16 KB-page-alignment
    check** (`zipalign -c -v -P 16 4 <apk>` and `readelf -lW` requiring `LOAD` offset `0x4000`
    on every 64-bit `arm64-v8a`/`x86_64` `.so`; 32-bit ABIs are exempt).
  - (`android-emulator` is manual-dispatch only and not a standing gate.)
- [ ] Confirm the five protected Environment secrets, signer-fingerprint
  variable, required reviewer, immutable-releases setting, and tag ruleset are
  configured exactly as described in §4.
- [ ] Confirm `lib/libraw/compliance/license-route.txt` records the exact
  human-reviewed SPDX route. `UNRESOLVED` is the intentional repository default
  while ticket #166 is open and is a hard release failure. Including both
  upstream texts does not select a route.
- [ ] Recheck LibRaw OSS-Fuzz mirrors [#840](https://github.com/LibRaw/LibRaw/issues/840)
  and [#843](https://github.com/LibRaw/LibRaw/issues/843). If either discloses
  memory corruption reachable through an enabled codec, keep the release on hold
  until the pinned dependency is patched and its hostile regression is green.
- [ ] Commit version/changelog changes with `-c commit.gpgsign=false`.

---

## 3. Cutting the release

Tag and push from a machine with normal GitHub access:

```bash
git tag -a v0.7.0 -m "Spektrafilm v0.7.0"
git push origin v0.7.0
```

(Or trigger `release.yml` manually via **Actions → Release → Run workflow** with the tag name.)

Expect the unsigned-candidate and protected signing jobs to assemble, gate,
sign, device-verify, and publish `Spektrafilm-v0.7.0.apk`, the deterministic
LibRaw source/relink ZIP, app and LibRaw SPDX documents, six-lock archive, R8
mapping, full native symbols, release runtime classpath, provenance, and
`SHA256SUMS` to the Release for that tag.

**Verify the published APK** (download from the Release, then):

```bash
# Signature (production keystore, not debug)
$ANDROID_HOME/build-tools/35.0.0/apksigner verify --print-certs Spektrafilm-v0.7.0.apk

# Checksum matches the sidecar
sha256sum -c Spektrafilm-v0.7.0.apk.sha256

# Verify every compliance artifact listed by the bundle generator
sha256sum -c SHA256SUMS

# Re-run the fail-closed bundle audit against the downloaded APK
python3 tools/compliance/libraw_bundle.py verify \
  --repo-root . \
  --bundle <libraw-source-relink.zip> \
  --require-resolved

# 16 KB-page alignment (same checks ci.yml's android job runs)
$ANDROID_HOME/build-tools/35.0.0/zipalign -c -v -P 16 4 Spektrafilm-v0.7.0.apk
# and, for each extracted 64-bit lib/*.so:
readelf -lW lib/arm64-v8a/libspektra.so | awk '/LOAD/{print $NF; exit}'   # expect 0x4000
```

---

## 4. Required protected Environment configuration

Create a GitHub Environment named `release-signing`, require the designated
maintainer approval, prevent self-review where the plan supports it, and keep
all production signing values out of repository-level secrets. Configure these
Environment secrets:

- `SIGNING_KEYSTORE` — base64 of `spectrafilm-release.jks`.
- `SIGNING_KEY_ALIAS` — keystore key alias.
- `SIGNING_KEYSTORE_PASSWORD` — store password.
- `SIGNING_KEY_PASSWORD` — key password.
- `RELEASE_GITHUB_TOKEN` — a fine-grained PAT or GitHub App token scoped only
  to this repository with **Administration: read** (to prove immutable releases
  are enabled) and **Contents: write** (to create/upload/publish the Release).

Configure this Environment variable:

- `RELEASE_CERT_SHA256` — the 64-hex SHA-256 fingerprint of the only accepted
  APK signing certificate (colons and case are normalized by the workflow).

Also enable repository immutable releases and protect `refs/tags/v*` against
updates/deletion. If any signing input, publisher token, repository setting, or
certificate fingerprint is empty/invalid, the job fails closed. The ordinary
workflow `GITHUB_TOKEN` is deliberately not used for release publication because
it cannot be assumed to have repository Administration read permission.

---

## 5. Post-release

- [ ] Open the immutable published Release; confirm the APK, both SPDX files,
  LibRaw source/relink ZIP, six-lock archive, R8 mapping, full native symbols,
  runtime classpath, provenance, and `SHA256SUMS` assets are attached.
- [ ] Verify the APK is production-signed and 16 KB-aligned (§3).
- [ ] Close/update issues resolved by the release; keep `CHANGELOG.md` current.
- [ ] **Do not** commit the APK anywhere in the repo — the release artifacts live only on the
  GitHub Release. There is intentionally no `dist/` directory.
- [ ] The attribution **"Film modeling powered by spektrafilm"** must remain in the app and
  release notes (GPLv3 requirement).

---

### Sources

- `.github/workflows/release.yml` (exact candidate gates and attestation,
  run/attempt/artifact-ID binding, protected signing and cleanup, API 35
  instrumentation, provenance, immutable tag/release verification, publication).
- `tools/release/` (deterministic app SPDX, fixed gate attestation, immutable
  numeric-ID publisher, and structural regression tests).
- `.github/workflows/ci.yml` (standing gates, including LibRaw and 16 KB
  `zipalign -P 16` plus `readelf -lW` checks).
- `app/build.gradle.kts:12-22` (optional local release keystore; absent means
  unsigned), `:39-40` (versionCode 11 / 0.9.0), and the release build type (R8,
  full native symbols, and optional
  real release signing; never a debug-key fallback).
- `app/proguard-rules.pro` (R8 Stage-1 keep-rules: JNI boundary classes + enum persistence,
  `-dontobfuscate`).
