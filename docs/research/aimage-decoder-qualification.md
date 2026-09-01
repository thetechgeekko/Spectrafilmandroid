# API 30+ `AImageDecoder` qualification (#198)

Status: **isolated experiment; not adopted; production routing is absent and OFF**.

This document is the execution contract for deciding whether the NDK
`AImageDecoder` can replace part of the current Java decode paths: two-pass
`BitmapFactory` for ordinary non-RAW and `ImageDecoder` for explicitly
display-referred DNG fallback imports. A fast result is not
enough: color, alpha, orientation, memory, cleanup, and OS reproducibility must
all pass first.

## Decision boundary

- LibRaw remains the scene-linear RAW developer. This experiment does not
  replace, bypass, or reinterpret it.
- DNG is accepted only with both `image/x-adobe-dng` and an explicit
  `allowDisplayReferredDngFallback` caller attestation. Any future production
  callsite must supply that attestation only after LibRaw has failed. This
  isolated runner directly exercises the candidate and therefore records that
  LibRaw-first ordering is required but not runtime-enforced here. DNG is never
  described as scene-linear RAW or archival exact.
- API 24–29 keep the existing Java/Bitmap route unchanged.
- The experiment is a library module and is not an `:app` dependency. No
  feature flag or production call site exists.
- Platform output may change with Android/Skia/DNG codec revisions. A digest is
  evidence for one recorded build fingerprint, not a universal bit-identical
  promise.

Current answer: **do not wire it into production yet**. The candidate can
produce an admitted linear-ProPhoto result only for explicit sRGB
`RGBA_8888`. P3, 16-bit, HLG, PQ, unknown-dataspace, default-F16,
`RGBA_1010102`, and every other non-exact-8888 platform default are preserved
as `RGBA_F16` evidence but deliberately cannot enter the engine until
a separately qualified F16/dataspace-to-working-f32 conversion exists. That is
the correct fail-closed behavior and also means a speed win alone cannot adopt
the route for HDR/wide-gamut imports.

The HLG/PQ fixtures currently prove finite F16 samples, their pinned dataspace,
alpha bounds, and exact first decoded code values against the pinned PNG source
codes. They do **not** carry an independent absolute-luminance/reference-sample
oracle, so HDR
headroom and absence of tone mapping remain explicitly **UNQUALIFIED**. A full
corpus execution therefore reports `HDR_HEADROOM_ORACLE_BLOCKED`; it is not a
completed qualification cell.

## Implemented isolated experiment

`lib/aimage-decoder` contains:

- an API-24-loadable JNI library with weak API-30 references and guarded
  callsites;
- fd and direct-buffer header gates for JPEG, PNG, GIF, WebP, BMP, ICO, WBMP,
  HEIF, and fallback-only DNG;
- Android's canonical `image/x-ico` MIME plus documented aliases;
- independently reopened, seekable fd ownership and exact RAII cleanup;
- decoder ownership adopted only after `AImageDecoder_createFrom*` returns
  success and a non-null out-pointer (the failure out-pointer is never read or
  deleted);
- checked dimensions, stride, pixel count, direct-buffer capacity, and a
  128 MiB encoded-input ceiling;
- client-owned output allocated through the engine's process-wide
  `NativeBufferOwner` admission coordinator;
- explicit `RGBA_8888`/sRGB or evidence-only `RGBA_F16`/source-dataspace plans;
- unpremultiplied alpha when unscaled, and a fail-closed return when alpha plus
  scaling cannot preserve hidden RGB;
- source/header re-probe before decode, with the normalized declared MIME and
  original DNG permission rebound and enforced again at that boundary, plus
  deterministic decoder/fd leak counters;
- pre-decode cancellation and post-platform-decode discard/cleanup. The
  platform `AImageDecoder_decodeImage` call itself has no interrupt API;
- an inverse-sRGB to linear-ProPhoto f32 conversion matching the current app
  constants, compiled with FP contraction disabled;
- dependency-free host header/MIME tests, pure plan tests, and a platform-only
  connected qualification runner.
- a kept native fallback exception plus a bridge-supplied `Class` reference,
  so R8 cannot turn a deliberate typed fallback into a renamed-class JNI
  lookup failure; the release test target can be minified and exercised on a
  device; and
- a sealed, factory-minted public qualification contract with no consumer
  constructor, implementation, or data-class `copy` bypass, for the separately
  compiled release AndroidTest consumer. R8 may optimize that contract, while a
  deliberately disposable canary remains renameable and proves that the
  executed target is actually minified.

The Java comparator intentionally selects today's source-specific behavior.
Ordinary non-RAW uses the production two-pass `BitmapFactory` bounds/pixel
decode, power-of-two `inSampleSize`, and exact fallback scale. Explicit
display-referred DNG fallback uses the production software `ImageDecoder` path
with its power-of-two target sample and exact fallback scale. The ordinary
route requests production `ARGB_8888`; qualification rejects the timing cell
if the decoder ignores that preference, so no harness-only format copy enters
the timed route. Both run one inverse-sRGB/ProPhoto conversion. The pinned
orientation-6 non-RAW comparator also applies the app's downstream 90-degree
rotation so final endpoints, rather than unlike orientation ownership, are
checked. Oriented inputs remain performance-unqualified because the current
route rotates the linear image downstream while the candidate receives
already-oriented pixels; only an atomic ownership change can make that timing
work identical. Correctness hashing uses separate untimed passes. A performance
comparison is emitted only when both routes have already proven the same
dimensions and exact decoded/linear digests and each hands back the same live,
owned packed RGB-f32 linear-ProPhoto endpoint. Alpha, F16, wide-gamut, HDR, and
otherwise unequal endpoints are reported as performance-unqualified instead
of timing unlike work.
For export measurement, the final Java f32 hand-off uses the same
coordinator-owned, exactly closeable owner that current full-resolution export
already uses. Its platform decode, software Bitmap, sample hint, exact scale,
8-bit copy, and conversion remain the current route. Preview currently returns
a GC-owned direct output while the candidate is exactly closeable, so preview
performance is deliberately marked ownership-unqualified instead of
normalizing away that difference or accumulating unreclaimable repeat output.

## Orientation ownership differs by Java route

Android CTS explicitly verifies that Java `ImageDecoder` and NDK
`AImageDecoder` return EXIF-oriented pixels and oriented dimensions.
`BitmapFactory`, used by the app's ordinary non-RAW route, does not own that
step; the app applies EXIF downstream. The harness mirrors that downstream
rotation for its pinned orientation-6 comparator. The final comparator and
NDK endpoints must therefore both report `12x16` from an encoded `16x12`
raster.

Any future production integration must carry an `orientationAlreadyApplied`
contract into import orchestration. Re-reading EXIF and applying it again would
double-rotate the image. The current app-level EXIF handling must not be edited
until the candidate has won and that ownership change has its own regression
test. See the official [AImageDecoder CTS orientation test](https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/graphics/src/android/graphics/cts/AImageDecoderTest.java#1002).

## Pinned corpus

Project-authored fixtures are embedded in the test source. External binaries
are not vendored; the preparation script downloads immutable AOSP CTS revisions
and refuses any digest mismatch or overwrite. Preserve the upstream Android CTS
license/notice when redistributing those files. Connected runs are offline.

| Fixture | Purpose | Bytes | SHA-256 |
|---|---:|---:|---|
| `project-gradient.jpg` | JPEG/sRGB | 468 | `b7be17a857ffaf82258716d33628fa8aa1eaca718dbec6afa32d36699ed8b2b5` |
| `project-gradient-orientation-6.jpg` | EXIF orientation | 504 | `b1d8e87f92617bb159bed0b32673859d4709ec8db333133f7d6c3cb5a26b7f2a` |
| `project-gradient.png` | PNG/sRGB | 265 | `be63cc25e26f6b1a328a999ad8ba7a84f65b373693378873a1d37015f217c29d` |
| `project-alpha.png` | unpremultiplied alpha/hidden RGB policy | 654 | `c2873e7844f4b0119105dc6cec43a4e1ba6601a12a0731be49fd253ffc5bd7e9` |
| `project-rgba16-oracle.png` | exact RGBA16 samples, alpha=0 hidden RGB, off-8-bit lattice | 132 | `50a5c017735f446159a4b956c1042765d032f922d9bac3862cce0f249cb7af04` |
| `project-first-frame.gif` | GIF smoke | 972 | `3415b750fc68832d55cf9891a8bb9dea031784a2748a23ef4acc58a05dfa500c` |
| `project-gradient.webp` | WebP | 226 | `1068a89a88d4926f1b63803f35d5d4425f05153c048ddeec23f899a2c277efab` |
| `project-gradient.bmp` | BMP | 630 | `5b96666cbdc817fdab5c4a774d4300462001696d934c6a8e8d825a7f1c7a32e1` |
| `project-gradient.ico` | ICO/`image/x-ico` | 287 | `7c213793e4f3222ac6a91a43702ec97d1b6aae113d8582df3ac7cf090da0ff91` |
| `project-checker.wbmp` | WBMP | 28 | `127226e46f2659a21dcb959b61c01aa5c27eb11294e03694f77b4f2e21ab4deb` |
| `translucent-green-p3.png` | P3 + alpha | 993 | `9f1bd663564634bff9d7f3c25a9495ac71c8565a6a9407a64acfae2bc33e1c57` |
| `blue-16bit-srgb.png` | 16-bit/F16 default | 938 | `aa39b12b96bba7084648902af956f6563f361d8631400396344807ce919cb6db` |
| `red-hlg-profile.png` | BT.2020 HLG | 776 | `9cf5df965aefb69ac6dc9845055c8a84309879dc1f451074cb632159cbb4a193` |
| `red-pq-profile.png` | BT.2020 PQ | 23,386 | `a2b2a147067b0e019ed7768abc424dc7755694fe920021517d3be8257338cb6b` |
| `animated.gif` | actual animated GIF, first frame | 34,978 | `eec5e745032b9775d67f040d9ab95ae3dc296100ce0c5d6bf95667bf2d27d2a6` |
| `heifwriter_input.heic` | HEIF | 15,769 | `62dfb44160403ca8355a874cecc91cdbce57e98dd597fa36a2af55ef54c017ac` |
| `sample_1mp.dng` | display-referred DNG fallback | 87,116 | `271aa1db6369f271e160acaf3029c8e86b8a86d2e9a44d1cc731f50575767ac0` |
| `bug_156261521.dng` | hostile DNG/no crash/leak | 266,600 | `8b0237910cc4ff180ad96fb1af42ef4a5b1edd92f9fa2cb274638a97b20db544` |

The official fixtures are used by Android's own
[ImageDecoder tests](https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/graphics/src/android/graphics/cts/ImageDecoderTest.java)
and [AImageDecoder tests](https://android.googlesource.com/platform/cts/+/1daba777fa1cc472226da4104041849ccbc65b80/tests/tests/graphics/src/android/graphics/cts/AImageDecoderTest.java).

Prepare once while online, then run offline:

```powershell
$corpus = Join-Path $env:TEMP 'sfaimage-corpus'
& .\lib\aimage-decoder\qualification\fetch-corpus.ps1 -OutputDirectory $corpus
adb push $corpus /data/local/tmp/sfaimage-corpus
```

The runner reads only the exact fixed shell path through a pipe, verifies every
SHA-256 again, and materializes private temporary files with exact cleanup.

## Correctness evidence and performance evidence are separate

`AIMAGE_CORRECTNESS_V2` identifies every untimed fixture with source byte count,
verified digest, provenance, declared MIME, sniffed kind, encoded orientation,
and explicit DNG-fallback state. It then records:

- header/output dimensions and platform MIME;
- NDK default/final pixel format, Java decoded/final format,
  alpha/premultiplication behavior, dataspace, and both route color-space
  descriptions;
- decoded-pixel digest and, when admitted, linear-ProPhoto digest;
- whether a qualified `AIMAGE_PERF_V2` record or an explicit
  `AIMAGE_PERF_UNQUALIFIED_V1` record follows; and
- an explicit statement that correctness hashing is outside timing.

`AIMAGE_SAMPLE_ORACLE_V2` separately records:

- finite/alpha/sample-lattice/transparent-hidden-RGB evidence;
- exact source-sample comparisons for the project RGBA16 oracle and the pinned
  P3-alpha sample, plus exact pinned first-code comparisons for HLG/PQ; and
- an explicit false HDR headroom/tone-map qualification flag for HLG/PQ.

`AIMAGE_ROUTE_INVENTORY_V1` accompanies both routes for every corpus fixture
and for the generated phone preview/export cases. It records the observable
owned data allocations and total allocated bytes, decode-output writes,
boundary copies, transform writes, and the ordered boundary operations.
Correctness hash/sample scratch is excluded. Platform/codec-internal
allocations are deliberately reported as `unobservable` with
`platform_internal_allocations_measured=false`; these records are a bounded
copy/allocation inventory, not an allocator-perfect or peak-memory claim.
`AIMAGE_PERF_V2` remains the source of sampled process peaks.

`AIMAGE_PERF_V2` contains only production-equivalent timing and memory for a
previously proven identical endpoint:

- min/p50/p95/max latency after warmup, ending when the live owned f32 image is
  handed back;
- separately sampled baseline/peak PSS, native heap, and JVM heap while that
  endpoint remains live; and
- explicit flags proving corpus fetch/materialization, independent source
  read/hash, output hashing, sample oracles, and cleanup are outside the timed
  distribution. The decoder's intrinsic encoded-input I/O remains timed for
  both file-backed routes because it is part of production import/export.

Peak-memory sampling runs outside the latency distribution at 1 ms intervals,
so its observer overhead does not contaminate latency. It is process evidence,
not an allocator-perfect trace; a later Perfetto/memtrack run must refine any
adoption claim.

A deterministic uncompressed `4080x3060` BMP is generated in the app-private
cache before any benchmark baseline. It supplies two no-download phone-sized
cases. Preview passes the app's declared `maxEdge=2048` policy unchanged to
both routes: bounded NDK scaling must produce exactly `2048x1536`, while the
current ordinary non-RAW route's power-of-two `inSampleSize=2` semantics must
produce exactly `2040x1530`. That observed geometry mismatch, plus different
output ownership, makes preview comparison/performance explicitly unqualified
and unable to support adoption. Full-resolution export must produce exactly
`4080x3060` on both routes, reach an identical live owner, and is then measured.
Generation, source read/hash, and correctness work are excluded from timing and
peak memory. The synthetic pixel pattern proves geometry and harness scaling;
it does not replace a representative high-entropy camera corpus.

The runner additionally covers:

- fd/direct-buffer output equality for bounded fixtures;
- malformed/truncated data, MIME/header mismatch, unknown header, and
  non-seekable fd;
- deterministic cancellation immediately before and immediately after the
  uninterruptible platform decode, with zero outstanding decoder/fd counters;
- coordinator-denied output allocation and restored budget;
- 50 repeated imports with stable digest and zero native resources;
- hostile DNG accepts only bounded success or
  `AImageDecoderFallbackException`; every other `Throwable`/`Error` fails the
  cell. Native decoder/fd counters, process fd count, and coordinator current
  bytes must return to their baselines;
- a two-invocation seed/force-stop/recover marker that persists PID, kernel
  process-start ticks, a process-lifetime UUID nonce, API, and full
  `Build.FINGERPRINT`. Recovery requires a changed process nonce, plus either a
  changed PID or changed kernel start ticks, with the same fingerprint and
  digests. This proves a new kernel process without assuming that a PID can
  never be reused.

## Required device/OS matrix

One API-36 physical-device full-corpus execution has passed. It is still not a
completed qualification cell: the independent HDR headroom oracle and
API-30/API-34 emulator cells are absent, process recreation is a separate
invocation, and production adoption remains off.

| API | Target | Status | Can close acceptance? |
|---:|---|---|---|
| 30 | Google APIs x86_64 r16 AVD | not installed/not run | no |
| 34 | Google APIs x86_64 r14 AVD | not installed/not run | no |
| 36 | physical Samsung device | full pinned corpus execution pass; minified fallback and process recreation pass; HDR oracle blocked | no |
| 37 | Google Play 16K x86_64 image | image installed; AVD descriptor is stale/missing | no; not a substitute for 30/34/36 |

The successful API-36 phone execution cannot waive API 30 and 34, and a Google
APIs emulator cannot prove all OEM codec behavior. Record the full
`Build.FINGERPRINT`, system-image revision, ABI, manufacturer, and model in every
cell.

### Captured API-36 evidence (2026-09-01)

Device: Samsung `SM-S948W`, Android 16 / API 36, `arm64-v8a`, fingerprint
`samsung/m3qcsx/m3q:16/BP4A.251205.006/S948WVLS4AZG3_OYV4AZG3:user/release-keys`.

- The strict-lock full-corpus debug run used 3 warmups and 15 measured repeats,
  verified all eight external CTS downloads again on-device, and ended with
  `AIMAGE_QUALIFICATION: CORPUS_EXECUTION_PASS;
  HDR_HEADROOM_ORACLE_BLOCKED; RECREATION_SEPARATE; MATRIX_INCOMPLETE` plus
  `INSTRUMENTATION_CODE: -1`. It kept `cell_complete=false`,
  `matrix_complete=false`, and `adoption_enabled=false`.
- The source RGBA16, transparent-alpha, premultiplied P3, HLG, and PQ sample
  checks passed their narrowly stated code-value oracles. The DNG candidate was
  forced to F16 evidence-only even though the platform defaulted it to sRGB
  8888; both a rebound false DNG flag and rebound wrong MIME were rejected at
  decode, and the hostile DNG returned the exact typed fallback with zero native
  resources/coordinator bytes and an unchanged process fd count. No HDR
  luminance/headroom or scene-linear DNG claim was made.
- The generated `4080x3060` BMP reached identical decoded-pixel and owned
  linear-ProPhoto-f32 digests. Candidate p50/p95 was
  `660,738,438 / 699,887,812 ns`; the current Java route was
  `3,698,578,020 / 3,704,107,603 ns`. This is promising synthetic harness
  evidence, not an adoption result. Sampled peak PSS was `247,826` versus
  `235,218 KiB`, so this run shows a large latency win but no peak-PSS win.
  Preview remained correctly unqualified because the real 2048-edge policies
  produced different geometry (`2048x1536` versus `2040x1530`) and ownership.
- For full-resolution export the observable boundary inventory recorded two
  candidate-owned data allocations totalling `199,756,800` bytes, zero
  boundary copies, and one transform write. The Java comparator recorded three
  known data allocations totalling `203,951,040` bytes, twelve `getPixels`
  band copies, and one transform write. Platform-internal allocations remain
  explicitly unobservable; these totals are not peak memory.
- The strict-lock minified release smoke emitted
  `target_minified=true`, renamed the canary to
  `com.spectrafilm.aimage.c`, caught the exact native
  `AImageDecoderFallbackException`, emitted
  `AIMAGE_R8_SMOKE: PASS; NOT_A_QUALIFICATION_CELL`, and returned
  `INSTRUMENTATION_CODE: -1`.
- Manual release seed/force-stop/recover changed PID `23348 -> 23425`, kernel
  start ticks `113723552 -> 113725372`, and the process UUID nonce, while both
  pixel and linear digests remained identical. Recovery emitted
  `AIMAGE_RECREATION_RECOVER_V3 ... status=pass` and
  `INSTRUMENTATION_CODE: -1`.

Decision remains unchanged: the module stays isolated and production routing
stays OFF until every adoption gate below is satisfied.

### Installed/download inventory (read-only Android CLI result)

| Package | Archive | Archive bytes | SHA-1 | Estimated installed + writable AVD |
|---|---|---:|---|---:|
| `system-images;android-30;google_apis;x86_64` r16 | `x86_64-30_r16.zip` | 1,438,186,618 | `6ae21030eaadc041078444d3798e4b399f3e787d` | about 2.5–3.5 GiB |
| `system-images;android-34;google_apis;x86_64` r14 | `x86_64-34_r14.zip` | 1,563,721,130 | `e0f6c9a0691aa27bd597d0deb1bcfdc943ac8ca7` | about 2.5–3.5 GiB |
| `system-images;android-36;google_apis;x86_64` r7 | `x86_64-36_r7.zip` | 1,895,447,397 | `c6bf44bdcd885bb902b4ba752d111a073ad7a817` | about 3–4 GiB |

The installed API-37 Google Play 16K image occupies approximately
3,112,901,794 bytes before substantial writable snapshots. Do not install or
repair any image without explicit scoped approval. Prefer the two required
API-30/API-34 Google APIs images only if the API-36 preliminary candidate still
passes correctness and shows a meaningful win.

`AImageDecoder` is implemented in the platform graphics stack, not Play
Services. Google APIs versus AOSP does not change the public contract, but the
exact system-image/Skia revision is part of the evidence. Google APIs images are
the reproducible emulator choice; the physical Samsung API-36 cell supplies one
OEM comparison.

## Serialized verification window

Do not run these concurrently with another Gradle/ADB owner.

Host/static gate:

```powershell
.\gradlew.bat :lib:aimage-decoder:testDebugUnitTest `
  :lib:aimage-decoder:assembleDebugAndroidTest

# Host CTest + API-24 weak symbols/exports for all configured ABIs, plus create ownership.
.\gradlew.bat :lib:aimage-decoder:verifyAImageDecoderHostAndAbi
```

Preliminary API-36 connected run (embedded corpus only):

```powershell
.\gradlew.bat :lib:aimage-decoder:connectedDebugAndroidTest `
  -Pandroid.testInstrumentationRunnerArguments.aimage_expected_api=36 `
  -Pandroid.testInstrumentationRunnerArguments.aimage_require_full_corpus=false
```

Full API-36 cell after pushing the verified external corpus:

```powershell
.\gradlew.bat :lib:aimage-decoder:connectedDebugAndroidTest `
  -Pandroid.testInstrumentationRunnerArguments.aimage_expected_api=36 `
  -Pandroid.testInstrumentationRunnerArguments.aimage_corpus_dir=/data/local/tmp/sfaimage-corpus `
  -Pandroid.testInstrumentationRunnerArguments.aimage_require_full_corpus=true `
  -Pandroid.testInstrumentationRunnerArguments.aimage_repeats=15 `
  -Pandroid.testInstrumentationRunnerArguments.aimage_warmups=3
```

R8/minified typed-fallback smoke uses the same source and instrumentation but
selects the minified release target. It must emit `target_minified=true` and
catch the exact native fallback type. A module-local canary is retained with
`allowobfuscation`; the runner also requires its runtime binary name to differ
from the source name. The separately packaged AndroidTest reads that name through
a narrow kept target bridge rather than linking the renameable class by its source
name, so a BuildConfig flag or keep-rule text alone cannot fake
the minification proof:

```powershell
.\gradlew.bat :lib:aimage-decoder:connectedReleaseAndroidTest `
  -PaimageTestBuildType=release `
  -Pandroid.testInstrumentationRunnerArguments.aimage_expected_api=36 `
  -Pandroid.testInstrumentationRunnerArguments.aimage_recreation_phase=r8-smoke `
  -Pandroid.testInstrumentationRunnerArguments.aimage_require_minified=true
```

Building a release AAR alone is not accepted as the R8/JNI proof. The minified
target must actually execute malformed input and catch
`AImageDecoderFallbackException` on a device. The dedicated smoke mode skips
the large corpus/performance work and explicitly reports that it is not a
qualification cell.

After the first build, use `adb shell pm list instrumentation` to capture the
generated library-test component before running the manual
`aimage_recreation_phase=seed` and `recover` invocations with a force-stop in
between. Recovery fails unless the process-lifetime nonce changes and either
the PID or kernel process-start ticks change while the full build fingerprint
stays identical. The disjunction handles legal PID reuse without accepting the
same kernel process. Do not guess that generated component in release
documentation.

## Adoption/rejection gate

Adopt only if all of the following are true:

1. Every required format passes on API 30, 34, and 36 with source and output
   digests, orientation, alpha, format, dataspace, ICC/color-space, latency, and
   peak-memory evidence.
2. sRGB output matches the approved working contract. The source-sample RGBA16
   and alpha oracles pass, and an independent HDR reference proves headroom and
   absence of tone mapping; metadata/channel dominance alone cannot satisfy
   this gate.
3. Malformed, truncated, OOM, cancellation, repeated import, and process
   recreation leave exact-zero native resources.
4. The NDK route shows a meaningful latency or peak-memory win at an identical
   owned f32 endpoint on representative high-entropy full-resolution phone
   images. The generated 4080x3060 BMP is a harness/geometry gate, not the sole
   adoption corpus.
5. Existing LibRaw corpus digests and the exact release/R8 gate remain unchanged.
6. Orientation ownership is changed atomically with regression coverage.

If any correctness condition fails, or performance is neutral/worse, close the
ticket with the TSV evidence and reject production adoption. Do not leave a
dead production route or weaken the existing fallback.

## Primary references

- [Android NDK Image Decoder reference](https://developer.android.com/ndk/reference/group/image-decoder)
- [Android NDK Bitmap alpha/format constants](https://developer.android.com/ndk/reference/group/bitmap)
- [Android NDK dataspace constants](https://developer.android.com/ndk/reference/group/a-data-space)
- [Java ImageDecoder color-space and premultiplication behavior](https://developer.android.com/reference/android/graphics/ImageDecoder)
- [Java BitmapFactory decode options](https://developer.android.com/reference/android/graphics/BitmapFactory.Options)
- [ColorSpace to DataSpace mapping (API 33+)](https://developer.android.com/reference/android/graphics/ColorSpace#getDataSpace())
