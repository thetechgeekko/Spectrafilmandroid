# LibRaw dependency and qualification record

Status: implemented for ticket [Patch LibRaw to 0.22.2 and add hostile-RAW regression coverage](https://github.com/thetechgeekko/Spektrafilm-android/issues/165). This record describes the candidate dependency; the separate legal-distribution gate remains open.

## Pinned upstream

| Field | Value |
|---|---|
| Version | `0.22.2` |
| Official archive | `https://www.libraw.org/data/LibRaw-0.22.2.tar.gz` |
| Archive size | `1,682,962` bytes |
| Archive SHA-256 | `de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa` |
| Audited patched-tree SHA-256 | `bc463c30e414781d2455a47b99c30741830798b5941a049782b560fbb3abc74c` over the sorted 100-file source/header manifest |
| Annotated Git tag object | `24fa7e5463cbf8b8615dbd2b16c933a294d52400` |
| Peeled release commit | `b93f6e45c194f5df9b02a43b1af9a54b4f41f33f` |
| Signature | The annotated tag is not signed; the official URL plus archive SHA-256 is the shipping trust anchor. |

The official archive's critical decoder, postprocessing, version, and copyright
files were byte-compared with tag `0.22.2`. The build uses a version-stamped
FetchContent name, so an existing `0.21.x` `_deps` tree cannot satisfy the pin.
The three old independently editable cache variables were removed from the
build contract. Release and RelWithDebInfo configurations reject
`SFRAW_LIBRAW_SOURCE_DIR`; that override is only for verified debug/offline work.

`lib/libraw/cmake/LibRawVendor.cmake` verifies the archive, every local patch
hash, exact version macros, security contracts, the 100-file patched-tree
aggregate, and the hostile-column guard. Missing, stale, or locally mutated
source is fatal: a runtime decoder stub is no longer a release-capable result.

## Ordered patch series

The complete manifest and patch hashes are in
[`lib/libraw/patches/README.md`](../../lib/libraw/patches/README.md).

| Patch | Reason and upstream disposition |
|---|---|
| `0001-openmp-wavelet-initialize-size.patch` | Restores the initializer missing from the OpenMP branch in LibRaw issue #842. The defect is present in 0.22.2 and current upstream had no merged fix at qualification time. |
| `0002-newsubfiletype-unsigned.patch` | Source portion of upstream PR #853 / commit `d9437df…`; removes issue #844's file-controlled float-to-signed-int UBSan overflow and retains the full TIFF `LONG`. |
| `0003-ljpeg-zero-category.patch` | JPEG lossless Huffman category zero means a zero difference. Handles it before the otherwise undefined `1 << (len - 1)` and rejects categories outside `0..16`; issues #367/#473 were closed upstream as damaged-input/GIGO without a fix. |
| `0004-bound-tiff-metadata-allocations.patch` | Adds a cumulative identify-time member-allocation ceiling, exact payload reads, and duplicate rejection for allocation-bearing strip/XMP/opcode tags. Android and the host gate set the ceiling to 16 MiB. |
| `0005-bound-lossless-jpeg-work.patch` | Bounds embedded SOF3 decoded samples to a conservative raw-geometry allowance and the configured raw-memory budget, preventing small category-zero entropy from becoming an availability/CPU bomb. |
| `0006-bound-dng-tile-streams.patch` | Bounds DNG tile streams and cumulative internal-LJPEG sample work. |
| `0007-bound-sony-ljpeg-work.patch` | Validates Sony tiled-LJPEG geometry and bounds tile/setup work. |
| `0008-bound-ljpeg-segments.patch` | Makes marker parsing exact, allocation-checked, and work-bounded. |
| `0009-bound-ljpeg-setup-work.patch` | Accounts Huffman/quantization setup work for each LJPEG stream. |
| `0010-harden-hasselblad-ljpeg.patch` | Rejects invalid Hasselblad geometry, categories, and stores. |
| `0011-harden-ljpeg-idct.patch` | Checks predictor/IDCT arithmetic before narrowing. |
| `0012-harden-cr2-slice-arithmetic.patch` | Validates CR2 slice geometry and widens index arithmetic. |
| `0013-harden-canon-sraw.patch` | Validates Canon sRAW rows, slices, components, and signed arithmetic. |
| `0014-bound-identify-ljpeg-work.patch` | Applies a cumulative LJPEG setup budget during identify/probe paths. |
| `0015-bound-fp-dng-compressed-work.patch` | Validates compressed float-DNG offsets, sizes, and cumulative work. |
| `0016-bound-canon-sraw-white-balance.patch` | Rejects non-finite/out-of-range Canon sRAW white-balance scaling. |
| `0017-make-ljpeg-idct-init-thread-safe.patch` | Replaces racy first-use IDCT writes with immutable initialization. |
| `0018-bound-identify-maximum-shift.patch` | Defines maximum/black shifts for float and high-bit TIFF metadata. |
| `0019-bound-hasselblad-predictor-arithmetic.patch` | Widens and range-checks Hasselblad predictor accumulation. |
| `0020-bound-olympus-metadata-and-arithmetic.patch` | Validates Olympus 14-bit metadata, exact refills, unary work, shifts, predictors, and pixel narrowing. |
| `0021-harden-panasonic-c8-decoder.patch` | Preserves raw C8 metadata counts, validates tables, destination geometry and each in-file source range, enforces bit budgets, and defines predictor arithmetic plus OpenMP error reduction. |
| `0022-bound-fixed-header-string-reads.patch` | Bounds identify and MakerNote fixed-header comparisons and the dormant X3F model probe. The host gate enables X3FTOOLS to qualify this seam; it does not qualify the full optional parser for Android. |
| `0023-record-local-modification-notices.patch` | Adds a dated Spektrafilm Android modification notice to every upstream file changed by patches 0001–0022. This is a notice-only patch; aggregate `2fb59481…` is retained only as the exact migration input to the 23-patch notice tree. |
| `0024-define-xtrans-negative-index-arithmetic.patch` | Rewrites X-Trans neighbor offsets from undefined `-i << c` to the intended `-(i << c)`. A valid 520 x 520 public-seam X-Trans DNG keeps the exact pre-fix float digest `5238915555911424415` while ASan/UBSan stays clean. |
| `0025-define-icc-s15fixed16-conversion.patch` | Rounds generated ICC XYZ coefficients in the signed domain before defined s15Fixed16 word encoding, preserving negative ACES matrix entries without floating-to-unsigned undefined behavior. |

Panasonic C8 codebooks are intentionally decoded in metadata order. Requiring a
prefix-free table would reject a real S5M2 table whose 8-bit entry shadows two
12-bit entries. The patch instead validates every entry and rejects an unmatched
`huff_index == 17` fail-closed; this is a deliberate divergence from upstream's
implicit zero fallback. Captured GH6, GH7, G9M2, and S5M2 codebooks cover all
input prefixes and therefore do not use that fallback.

LibRaw 0.22.2 already contains the CR2Slice column bound for
CVE-2026-21413 / TALOS-2026-2331 and the truncated Sony YCC fix. The resolver
checks the hostile-column bound rather than applying a duplicate patch.

## Android build contract

- NDK `27.0.12077973`, CMake `3.22.1`, C++17, `c++_shared`.
- ABIs: `arm64-v8a`, `armeabi-v7a`, and `x86_64`.
- Gradle passes `SFRAW_ENABLE_OPENMP=OFF` explicitly, so a stale external-native
  cache cannot retain an earlier experimental opt-in.
- Official release archive is compiled statically into `libsfraw.so`.
- Definitions: `LIBRAW_NODLL`, `LIBRAW_CALLOC_RAWSTORE`,
  `LIBRAW_MAX_PROFILE_SIZE_MB=16`, `LIBRAW_MAX_METADATA_ALLOC_SIZE_MB=16`,
  `NO_JASPER`, `NO_JPEG`, and `USE_ZLIB`; LCMS stays disabled by omitting every
  `USE_LCMS*` define.
- `NO_JPEG` disables the external lossy-JPEG dependency; it does not disable
  LibRaw's internal lossless-JPEG/CR2 decoder and is not a security mitigation.
- RawSpeed and Adobe DNG SDK glue are compiled inert because `USE_RAWSPEED` and
  `USE_DNGSDK` are not defined.
- Shipping `RelWithDebInfo` now uses `-O3 -g -DNDEBUG` with NDK hardening flags,
  not the previous accidental `-O2`.
- A sorted `libraw-sources-0.22.2.txt` source manifest is generated in each
  native build directory.
- `raw_decoder.cpp` has a compile-time exact-version assertion, explicitly pins
  LibRaw wavelet `threshold = 0`, caps encoded input at 64 MiB and LibRaw's
  per-unpack raw-store budget at 128 MiB, and rejects declared/adjusted/stretched
  frames above 12 MiPixels on 64-bit or 8 MiPixels on 32-bit. The dimension gate
  runs after identify and again after unpack; the processed image is checked
  before float allocation. It includes ActiveArea margins and DefaultScale.
- `RawInputLimits` applies the same 64 MiB ceiling to byte arrays, byte buffers,
  streams, and the Coil peek path before they can grow the ART heap.

## OpenMP exactness decision

OpenMP is default-off and forbidden in shipping Release/RelWithDebInfo builds.
It may be enabled in Debug solely for sanitizer and qualification work. This is
an exactness gate, not a claim that serial decoding is the final performance
design.

On an arm64 SM-S948W (Android 16), the compressed X100S RAF supplied with
upstream issue #845 was decoded five times using patched LibRaw 0.22.2 with
OpenMP. All five float-output SHA-256 values differed. Comparing two runs found
45,146 changed float values (`0.0925%`), maximum absolute delta `0.0159025`.
Serial 0.22.2 produced one identical digest in three runs:

`c18a1e36f5f1f5cc9b20599e44ef8a10b708d68cea6458915865f3b7c2531d8e`

Serial versus one OpenMP result differed in 193,445 values (`0.3965%`), with a
maximum absolute delta of `0.0321387`. This directly reproduces the upstream
nondeterminism on 0.22.2; enabling OpenMP would violate the requested exact
contract. Ticket [Finish RAW decode optimization on patched LibRaw release builds](https://github.com/thetechgeekko/Spektrafilm-android/issues/158)
must find a qualified deterministic policy rather than toggling the flag back on.

## Supported-corpus comparison

Private user captures are never committed or uploaded. Two user-authorized DNGs
were decoded locally through the same Android arm64 public seam; only aggregate
results and derived output digests are recorded.

| Local sample | Shape | 0.21.4 repeatability | 0.22.2 serial repeatability | Reviewed old/new change |
|---|---:|---|---|---|
| MotionCam DNG | `2736x3648x3` float32 | 2/2 `4f363d4a…` | 3/3 `1f83e610…` | Same shape; 144,869 values changed (`0.4838%`), mean absolute delta among changes `1.106e-5`, maximum `0.048809`, no non-finite values. |
| Latent DNG | `3060x4080x3` float32 | 2/2 `0991b189…` | 3/3 `f1c23a56…` | Same shape; 687 values changed (`0.001834%`), mean absolute delta among changes `1.079e-5`, maximum `0.0020843`, no non-finite values. |

The changed buffers are not silently labelled “bit-identical.” Current stable
rawpy `0.27.0` uses LibRaw `0.22.1`; direct uint16 postprocess comparisons also
produce different digests for both DNGs and the Fuji sample under 0.22.1 versus
0.22.2. The security-upgraded 0.22.2 serial output is the Android candidate
baseline. [Pin the latest Spektrafilm upstream and generate a parity manifest](https://github.com/thetechgeekko/Spektrafilm-android/issues/189)
must pin the desktop rawpy/LibRaw build to the same reviewed decoder or explicitly
approve and version the deviation before global parity can be claimed.

A current Samsung Expert RAW DNG on the same phone used JPEG-XL compression
(`0xCD42`). The module safely returned the typed JPEG-XL fallback result; it did
not decode sensor-linear pixels. That capability is not counted as supported
corpus and requires its own high-bit exact codec decision.

## Hostile-input and fuzz gates

The standalone host project under `lib/libraw/src/test/host` builds the exact
pinned/patched source and exercises the public sequence
`open_buffer -> unpack -> dcraw_process` with bounded inputs.

- Clang ASan + UBSan regression: issue #844's exact 64-byte TIFF plus
  project-generated truncated/malformed lossless-JPEG DNGs and a complete
  category-zero/hostile-CR2Slice public-seam case for CVE-2026-21413.
- Runtime and compile-time `0.22.2` assertions.
- Production caps: 64 MiB encoded input, 128 MiB LibRaw raw store, 12 MiPixels
  on 64-bit / 8 MiPixels on 32-bit, and 16 MiB each for embedded ICC data and
  cumulative identify-time metadata allocation. The fuzz runner deliberately
  uses a stricter 16 MiB per-input operational cap.
- Official OSS-Fuzz-style libFuzzer entry point, including parameter variation
  and all reachable public processing stages.
- CI independently runs the shipping-serial and OpenMP-required sanitizer
  variants, then a bounded fuzz smoke on every change and before signing.
- The OpenMP wavelet patch is compiled and executed under sanitizer coverage
  even though the shipping decoder is serial.

Local decoder evidence at implementation time used a byte-verified official
archive plus the 22 behavioral hardening patches. Patch 0023 subsequently added
notices only. Patch 0024 then defined X-Trans negative-neighbor index arithmetic
without changing its exact float output: the independently captured pre-fix and
patched digest is `5238915555911424415`. Patch 0025 defines signed ICC
s15Fixed16 conversion while retaining negative ACES profile coefficients. The
current 25-patch `bc463c30…` aggregate passed all 7/7 shipping-serial host CTests
under Clang 18.1.3 ASan + UBSan + float-cast-overflow on 2026-09-01, including
both-endian hostile next/reduced IFDs and the complete RAW precision contract.
Earlier #165 qualification passed 4/4 CTest
targets in both shipping-serial and OpenMP-required builds. The dedicated
first-use concurrency target passed under ThreadSanitizer; WSL required
`setarch x86_64 -R` only to avoid its known shadow-memory/ASLR collision. Clang
18 libFuzzer completed 1,000 public-LibRaw and 1,000 bounded-sniffer iterations
over 82 deterministic seeds without a sanitizer finding. The corpus covers
CVE-2026-21413, issue #844, malformed LJPEG, duplicate/aggregate metadata,
ActiveArea, DefaultScale, profile, input, raw-store, dimension budgets, Sony and
Samsung signed shifts, Hasselblad predictors, Olympus metadata/unary/predictor
paths, Panasonic C8 table/stripe/bit/arithmetic contracts, fixed-size identify
and MakerNote headers, and tail-boundary X3F model probes. Exact Panasonic
controls assert one- and two-stripe planes, S5M2 ordered first-match output, and
stable negative/signed predictor digests. An OpenMP-required Android arm64 build ran
the positive wavelet control and hostile fixtures on an SM-S948W. The final
shipping `RelWithDebInfo` library built for all three ABIs. The local unstripped
ELF SHA-256 values were `754dc24950dde15a1976744c33889c65365e137b096d828c1558a811ac346785`
(arm64-v8a), `c3742e652e26efca6ec2bbefc59fd8de3b8993e58e9299c36de884885c8f9fd8`
(armeabi-v7a), and
`4694f9a54018eea767cd53b1dc6d4b723eedc8ae33822ce2ca61b43e22d80d2a`
(x86_64). The arm64 file retained that exact hash after transfer, then decoded
the generated 256 x 256 positive DNG three times on that device with one
byte-identical output SHA-256:

`afcbd0a0e19bfbec3f47cadd63a74f17174b4b812c1b01ab29f4b0b2b04c0200`

The final arm64 binary also rejected the 64-byte non-terminated identify and
MakerNote controls through the production wrapper (`status=4`, LibRaw `-2`)
without a Java or native crash. A fresh three-ABI Debug rebuild, unit-test run,
APK install, and cold launch completed afterwards; the activity resumed in
402 ms and its process log contained no fatal exception or signal.

Two negative configuration gates also passed: shipping builds reject both a
local source override and `SFRAW_ENABLE_OPENMP=ON`.

Residual compatibility work is explicit rather than silently waived: qualify
real Olympus/OM 14-bit captures against the unary policy, add isolated causal
controls for the Olympus carry-refill and shift-31 branches, and add real RAWs
when Panasonic publishes a codebook not represented by GH6/GH7/G9M2/S5M2.
Android must also keep `USE_X3FTOOLS` disabled until the
[Qualify optional X3F support before Android enablement](https://github.com/thetechgeekko/Spektrafilm-android/issues/191)
closes the property-list minimum/even-length contract (including the current
`data_size / 2 - 2` underflow) and adds MemorySanitizer coverage for initialized
metadata strings. The host-only model-boundary tests are not that enablement
approval.

## Upstream security monitoring

As of 2026-08-30, upstream LibRaw issues
[#840](https://github.com/LibRaw/LibRaw/issues/840) and
[#843](https://github.com/LibRaw/LibRaw/issues/843) are open mirrors of
access-restricted OSS-Fuzz reports. Their public pages expose neither crash type,
reproducer, affected decoder, nor fix, so applicability to this enabled public
seam cannot be proved or dismissed. The local caps, sanitizer regressions, and
bounded fuzz smoke reduce exposure but do not constitute an upstream fix.
Recheck both reports before every release; if details disclose memory corruption
reachable in an enabled codec, hold release until a reviewed patch and regression
land. The dedicated monitoring ticket owns this residual risk.

## Stale-cache recovery and rollback

The patched-tree aggregate intentionally makes an edited or partially patched
`_deps` tree fail closed. Resolve the exact build directory first, then remove
only that generated directory (for Android, the affected ABI subtree under
`lib/libraw/.cxx`; for the standalone gate, the explicitly named
`build/libraw-host-*` directory) and reconfigure. Never point a shipping build at
an old `0.21.x` tree or enable the no-LibRaw stub as a workaround.

A dependency rollback is a reviewed pin change, not a cache edit. Update the
version, official URL/archive hash, ordered patch hashes, patched-tree aggregate,
source assertions, corpus expectations, and this record together; then rerun
all host sanitizers/fuzzers, Kotlin tests, three Android ABIs, and device corpus
qualification. If any of those are unavailable, keep the release gate closed.

## Codec and license boundaries

<!-- libraw-license-route: UNRESOLVED -->

LibRaw Android distribution route: UNRESOLVED.

Native precision-parity support in this build includes uncompressed RAW/DNG and
internal lossless-JPEG/LJ92. Floating-point DEFLATE
(`Compression=8`, `SampleFormat=3`) is parsed only by the audited dependency
corpus: the wrapper rejects it before LibRaw's quantizing memory-bitmap handoff.
It, integer/linear Compression 8, and Adobe 0x80B2 deflate use the dedicated
`DEFLATE_DNG` fallback classification. External lossy JPEG and JPEG-XL DNG are
not enabled; every platform fallback is display-referred and is not equivalent
to a high-bit scene-linear RAW decode.

The v1 wrapper binds DNG metadata to the single non-reduced RAW IFD matching
LibRaw geometry before and after unpack; root `DNGVersion` is the only inherited
tag. Any valid or malformed non-root `DNGVersion` is hostile and rejects for a
recognized TIFF even when IFD0 does not establish semantic DNG identity. Its
bounded pre-open walk also rejects malformed/missing eligibility fields
for every non-reduced RAW-looking IFD, plus unsafe BlackLevel conversion data in
every walked IFD, including reduced/non-selected images. Both buffer and fd
entry points therefore stop before `LibRaw::open_buffer`; a test-only observer
asserts zero dependency-open attempts. `BlackLevelDeltaH/V` presence is also
dependency-wide across bounded next/SubIFD paths, reduced images, and malformed
eligibility. The walker tracks ten distinct IFD offsets, matching LibRaw's
`LIBRAW_IFD_MAXCOUNT`, follows forward or backward next links, and rejects cycles,
aliases, malformed/duplicate/over-capacity SubIFD declarations, out-of-bounds
edges, and any depth/capacity exhaustion as incomplete before dependency open.
An `II`/`MM` header with wrong or truncated TIFF magic is likewise rejected before
LibRaw's endian-header dispatch. The review matrix proves 17 hostile classes in
two byte orders through buffer and fd (68 zero-open assertions). Verified Bayer 2×2 and X-Trans 6×6
additionally require the RGB-identity
`CFAPlaneColor` mapping and rectangular `CFALayout` (including their specified
defaults). Duplicate, malformed, remapped, or staggered selected-IFD tags fail
before dependency parsing and preview tags cannot provide RAW provenance.

Three/four-sample LinearRaw is qualified only with uniform BitsPerSample and
SampleFormat arrays, chunky planar configuration, no ExtraSamples, a 1×1 black
repeat, exact per-sample BlackLevel/WhiteLevel counts, and a complete local
two-illuminant ColorMatrix1/2 pair. Matrix type, dimension, payload,
denominators, and every plane row are checked pre-open; promoted matrices and
LibRaw's output coefficients must keep every plane finite and active after
identify. Spatial row×column×sample black matrices, custom CFA tables, and
`BlackLevelDeltaH/V` fail with `PRECISION_METADATA` until a versioned contract
can carry them. Every SHORT/LONG/RATIONAL BlackLevel element is parsed and bounded
against the selected declared-bit maximum before `LibRaw::open_buffer`; fractional,
zero-denominator, and out-of-range rational values cannot enter LibRaw conversion.

LinearRaw level validation and effective-bit derivation use exactly its active
three or four sample planes, and its public black/white lists have that same
length. CFA keeps four LibRaw level slots so the second-green plane is preserved.

LibRaw offers a choice of LGPL-2.1-only or CDDL-1.0. The distribution route for
this static integration remains `UNRESOLVED`; including both upstream license
texts records provenance and does not elect a route. The deterministic
source/relink bundle, notices, and SBOM are technical evidence, not legal
approval. Do not close the gate from this record; see [Resolve LibRaw static-link compliance and publish a complete license/source bundle](https://github.com/thetechgeekko/Spektrafilm-android/issues/166).

`tools/compliance/libraw_bundle.py` authenticates an explicitly supplied official
archive against the resolver's size and SHA-256 pins, applies all ordered patches,
checks the final 100-file aggregate, and writes a deterministic source/relink ZIP.
The ZIP contains canonical SPDX 2.3 JSON at `sbom.spdx.json`; `--sbom-output`
writes a byte-identical release sidecar. Ordinary `verify` accepts the committed
`UNRESOLVED` marker for CI auditing. Release uses `verify --require-resolved`,
which fails closed unless the human-reviewed, release-eligible route is recorded.
The Android CI job also extracts that ZIP and uses its standalone project with
NDK 27 to rebuild x86_64 `libsfraw.so`, then checks its SONAME, JNI and
recipient-marker exports, and 16 KiB `PT_LOAD` alignment. That automated result
is not a legal conclusion.
