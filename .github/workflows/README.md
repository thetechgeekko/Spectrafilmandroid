# CI workflows

## `ci.yml`

Runs on every push, PR, and manual dispatch. Eight jobs:

| Job | What it gates |
|-----|---------------|
| **engine-native** | The engine C++ + JNI bridge compile and link into `libspektra.so` on a host g++ toolchain (`-Wall -Wextra`; JDK provides `jni.h`); checks the exported `spk_*` symbols exist. |
| **native-safety-writers** | One committed runner performs the exact twelve-suite host inventory: seven ASan+UBSan rows for JNI safety helpers, fork/join exception containment, bounded JSON/NPY parsing, an asset-backed engine C render/cancel race, and PNG/TIFF hostile-input/JNI-helper tests; five TSan rows repeat the synchronized engine race and cover fork/join containment, the allocation registry, and writer cancellation races. This does **not** claim sanitizer runtime coverage of the actual engine JNI bridge. |
| **engine-parity** | The stage-parity gate: **39 `build_run` cases** against bundled assets and committed goldens (e2e goldens pinned to oracle `c1d0e44`), incl. thread-invariance (`SPK_NUM_THREADS` 1 vs 8). The workflow table is authoritative; `tools/parity/run_engine_parity.sh` must fail loudly if its table drifts from that count. |
| **libraw-hostile** | Fetches the official SHA-pinned LibRaw 0.22.2 archive through the Android resolver, verifies/applies all 23 hashed patches, runs independent shipping-serial and OpenMP-required public-seam ASan/UBSan gates (including dormant X3F model-boundary regressions), a serial TSan first-use gate, and 1,000 iterations on each bounded libFuzzer target. It also builds the LibRaw source/relink ZIP twice, verifies deterministic bytes, and audits the route-neutral bundle; CI permits `UNRESOLVED` so packaging remains testable before human selection. Shipping decode stays serial because the repeated compressed-Fuji corpus rejected OpenMP exactness for the current patched release. |
| **parity** | The standalone `.spkvec` comparator (`tools/parity`) builds via CMake and its `spkvec_selftest` ctest passes. |
| **python-lint** | The parity harness, LibRaw compliance tools, release-policy tools, documentation consistency checker, and offline Wayfinder frontier tests byte-compile; deterministic bundle/relink, app-SPDX, exact gate-attestation, immutable-publisher, workflow-structure, repository-fact, common Markdown link/image, preset-inventory, and ticket-routing tests pass. |
| **android** | JDK 21. Runs debug JVM unit tests for `:app`, `:lib:libraw`, `:engine:spektra-core`, `:lib:pngwriter`, and `:lib:tiffwriter`, then `:app:lint` (a hard gate — `abortOnError = true`, baseline at `app/lint-baseline.xml`), assembles the debug APK with the NDK-built `.so` for all 3 ABIs, and verifies the bundled app GPL/NOTICE plus the three pinned LibRaw legal assets. It extracts the generated source/relink bundle and performs a real NDK 27 x86_64 recipient build, checking SONAME, JNI/recipient marker exports, and every `PT_LOAD` alignment. Finally it runs the APK **16 KB-page gate** (`zipalign -c -P 16 4` plus `readelf -lW`). It uploads the app APK and standalone engine boundary Android-test APK as separate exact artifacts. |
| **android-emulator** | Required on every CI run. A single pinned emulator action installs the standalone engine Android-test APK on the same API 35 x86_64 `google_apis` route used by release, runs the actual `EngineBoundaryInstrumentation` JNI bridge suite, and requires both `ENGINE_BOUNDARY_INSTRUMENTATION: PASS` and `INSTRUMENTATION_CODE: -1`. It then installs/launches the app and rejects fatal Java/JNI/native failures. |

`bash tools/release/run_native_safety.sh --list` prints the locked seven
ASan+UBSan plus five TSan inventory. Running it without arguments compiles and
executes every row. CI and release both delegate to this one file, so their
native-safety command bodies cannot drift independently.

## `release.yml`

Fires on a stable `vMAJOR.MINOR.PATCH` tag push with no leading-zero components
(or manual dispatch with an existing stable tag). A push-triggered run binds the
resolved tag to the event commit; manual
dispatch resolves the named tag. It qualifies pinned LibRaw in serial/OpenMP
sanitizer, TSan, and fuzz gates. A separate job with no signing secrets verifies
the Gradle wrapper/distribution, matches the tag to source `versionName`, builds
the R8-minified APK, its release-targeted app instrumentation APK, and the
standalone engine boundary instrumentation APK; it proves the app is unsigned
and checks its compiled package/version. A separate native-safety
qualification calls CI's same committed twelve-suite ASan+UBSan/TSan runner for
JNI safety helpers, fork/join exception containment, bounded JSON/NPY parsing, the synchronized
asset-backed engine C cancellation race, and PNG/TIFF writer gates on
the exact tag commit before candidate build/signing. It makes no actual-JNI ASan
claim.
That job reruns the exact engine suite at both `-O2` and the shipping flags, release JVM tests for the app and all four native-backed modules, and release lint,
R8/JNI checks, and 16 KiB ZIP/ELF checks. A fixed gate attestation binds those
results, the exact commit/version, run ID/attempt, and all three APK hashes. The
service artifact digest and checked manifest bind that attestation together with
all six Gradle lockfiles, deterministic app and LibRaw SPDX documents, runtime
classpath, R8 mapping, and full native symbols; final provenance records the
artifact database ID/digest and the signed/installed identities.
The separate LibRaw qualifier emits its run ID/attempt/source receipt; a partial
rerun cannot relabel a previous attempt as current and fails with an instruction
to re-run all jobs.
Only that qualified artifact enters the protected `release-signing` Environment;
no Gradle task runs with the production key. Pinned `zipalign`/`apksigner` tools
align and sign both exact APKs, remove the temporary keystore before the next
step, and pin their single signer fingerprint. An API 35 emulator installs the
signed bytes, proves the installed `base.apk` is byte-identical, runs both the
exact transferred engine JNI boundary APK and the custom release instrumentation,
cold-launches `MainActivity`, and rejects app-scoped fatal Java/JNI/native logs.
The engine runner's APK hash, PASS/code transcript hash, and gate result are
recorded in published provenance, and its APK/transcript are retained as release
assets. That standalone APK is the engine module's debug Android-test variant:
it executes the resolved source's real JNI bridge, but does not claim dynamic
coverage of the R8/shipping `libspektra.so`. The exact release app is separately
R8/dex-gated, installed-byte checked, instrumented, and cold-launched. The
compliance verifier uses `--require-resolved`;
`UNRESOLVED`, an unsupported route, source/patch drift, or a missing notice/bundle
component stops publication.

The stdlib publisher requires repository immutable releases to be enabled. New
releases remain drafts until the exact numeric release/asset IDs, complete remote
inventory, and downloaded bytes have been verified and the tag has been checked
again. Existing releases pass only when already immutable and byte-identical.
Recovery may delete only the run-owned draft; it never deletes a published or
foreign release. Publication uses the protected Environment secret
`RELEASE_GITHUB_TOKEN`, a fine-grained PAT or GitHub App token with repository
Administration (read) and Contents (write); a missing/under-scoped token fails
closed. A successful run publishes the APK, both SPDX documents, LibRaw
source/relink ZIP, six-lock archive, R8 mapping, full native symbols, runtime
classpath, engine boundary APK/transcript, provenance, and checksums.
Release uses JDK 17; NDK r27, CMake 3.22.1, and build-tools 35.0.0 match CI.
Repository administrators must additionally protect `refs/tags/v*` with a
ruleset that forbids updates and deletion, enable immutable releases, and protect
the `release-signing` Environment with maintainer approval.

## `r8-smoke.yml` (added 2026-08-26)

Manual `workflow_dispatch` only. Builds an **unsigned R8-minified** release APK,
then explicitly aligns and signs it with the hash-pinned committed public debug
keystore (no production secrets), runs the same 16 KB-page checks as `ci.yml`, and uploads the APK
as `Spektrafilm-r8-smoke-apk`. Purpose: the CI `android` job builds debug (minify off), so a wrong
R8 keep-rule surfaces only at runtime — download this artifact and smoke-test it on a device
**before** tagging a release. See `docs/RELEASE_CHECKLIST.md`.
