# RAW white-balance chromatic-adaptation decision

Status: **research resolved; CAT02 implementation and device goldens verified**

Date: 2026-08-30
Wayfinder ticket: [Research and lock RAW white-balance chromatic adaptation against the upstream oracle](https://github.com/thetechgeekko/Spektrafilm-android/issues/167)
Implementation ticket: [Implement oracle-locked CAT02 RAW white balance and exact cast-order goldens](https://github.com/thetechgeekko/Spektrafilm-android/issues/192)

## Decision

Use a full **CAT02 Von Kries transform** for temperature-derived RAW white
balance. Do not use the current diagonal XYZ scaling, Bradford, or no
adaptation. The result is not a subjective preference: it is the transform that
the pinned upstream call resolves to under the pinned `colour-science` API.

Keep the following ownership and ordering explicit:

1. **LibRaw owns sensor-domain work and the base WB.** `as_shot` enables camera
   WB. Daylight, tungsten, and custom modes start from LibRaw's daylight base.
   LibRaw documents ACES as output colour-space code 6 and documents the camera
   WB/fallback controls in its [output-parameter API](https://www.libraw.org/docs/API-datastruct-eng.html).
2. **The RAW adapter owns exactly one temperature-derived CAT.** For tungsten or
   custom mode, convert the CCT to an XYZ source white, normalize it to `Y=1`,
   and CAT02-adapt it to the 6504 K reference in linear ACES2065-1. `as_shot`
   and daylight receive no extra CAT. A source white within NumPy's default
   `allclose` band of the reference skips the CAT, as upstream does.
3. **Tint is a separate float32 operation after the CAT.** Upstream casts CAT
   output to float32 and then multiplies by a float32 `[1,tint,1]`. Combining
   tint with the CAT's final cast is not bit-identical: the fixture measured up
   to `2.38418579e-7` in final float32 ProPhoto values.
4. **The ACES-to-ProPhoto conversion remains a separate colour-space CAT.** Its
   D60-like-to-D50 conversion is not a second scene-white correction. The
   engine boundary is explicitly the float32 result of that conversion; the
   desktop function itself returns a float64 array for this call.

No native decoder arithmetic changed in the original research ticket. The
separate implementation ticket now applies this contract in production and
keeps the research fixture as its immutable external baseline.

## Implementation evidence (2026-08-31)

`raw_decoder.cpp` now constructs colour-science 0.4.7's CAT02 matrix in the
oracle's exact float64 operation order, disables floating-point contraction for
that production translation unit, rounds CAT output to float32, and only then
performs the float32 tint multiply. `as_shot` and daylight return without any
pixel arithmetic. Native requests fail closed before LibRaw/math unless
temperature and tint are finite and inside the product ranges `[1000,12000] K`
and `[0.2,1.8]`; the deterministic 1000 K compatibility endpoint remains accepted.

The host CMake test generates a build-only C++ table directly from this JSON
after `--check` verifies the canonical digest. It passes all 56 combinations
(8 scenarios x 7 patches) bit-for-bit at both the ACES-after-WB and float32
ProPhoto boundaries. It also proves that the fixture still distinguishes the
wrong one-cast tint, missing `allclose`, and missing `isclose` implementations.
An optimized Windows `/O2 /fp:strict` run and an Android arm64 NDK r27 Clang
18.0.1 `-O3 -ffp-contract=off` run produced the same canonical evidence file:

`1ada31ace191d8d7bfffb7bb3cf1e96ae17bc78cd660cb380d743b3241d87d1a`

The final arm64 math-probe executable SHA-256 was
`c4d716e5973fcf826e2737996d9d11f5d3e1d8e85dad4b4e79b82bbcf4742120`.

The Android run was on a Samsung SM-S948W, API 36, Android 16, arm64-v8a,
fingerprint
`samsung/m3qcsx/m3q:16/BP4A.251205.006/S948WVLS4AZG3_OYV4AZG3:user/release-keys`.
The freshly built debug variant of the production decoder `libsfraw.so` was then
exercised twice per source with custom 4100 K / 0.85 tint and a deterministic
512-pixel longest-edge copy bound. Private RAW bytes stayed on-device; only
source and derived-output SHA-256 values are recorded.

| Device cohort | Source bytes / SHA-256 | Derived shape | Exact repeat output |
|---|---|---:|---|
| Samsung SM-S948W native DNG (the exact committed camera-seed source) | `24,999,540` / `58093ddfb0bba8fa6be32ae038b1323584e30902b20fb7c338eab4d6e16e444a` | `383x510x3` float32 | 2/2 `b3ab6024dd8545f4cd4666e33a888ba3f779749fd3ac9f2dcbd113f56e582375` |
| MotionCam DNG (the exact committed camera-seed source) | `20,333,486` / `bb74d3285ccb32cb775aaac029471cf6ed23ce4ec66e9e370ada1b2b2010b314` | `342x456x3` float32 | 2/2 `ba3dae0cdfb31adcc17c67bf3766acb433b4a5b3c963afe926c76d37468663d4` |
| User-authorized local VWFNDR Mobile DNG (third deterministic device cohort; never redistributed) | `25,014,193` / `f5a8e51c2ffc30a5f18a8388c26bf9ced6e6674631db6e64ec81224e64f4c0ea` | `383x510x3` float32 | 2/2 `f70c30c10bb0ccc47c88bb9247deb6ba6dbbe678886de2a506099f5c05a2f630` |

The exact debug device library used for this capture was
`e63e7c13ccd606206c3d06ae0ed7fc932a38d0b5e4d1e1d469a6ae8c273996e2`.
The linked device-probe executable SHA-256 was
`8f58144226daaa7af2a90a3d1c4e007bd13e60f1cda496e013898bf246aec6e5`.
This is numeric/device evidence, not a release-signing attestation.

## Oracle and environment pin

| Item | Pin/evidence |
|---|---|
| Upstream repository | `https://github.com/andreavolpato/spektrafilm` |
| Oracle SHA | `c1d0e44b962d80a51ea096d33faea346e4f3836c` |
| RAW-WB source blob | `97e34feb80fe47a6a3b36fcb93bef626d397135a` |
| RAW-WB test blob | `9103a1d05110fa973a9e696d9a7a02ee865ff81a` |
| Latest upstream checked | `3bb2c2d2801ff68b92019cf1dbcbb133d60832bc` on 2026-08-30 |
| Python environment | Python 3.11.2, NumPy 1.26.4, colour-science 0.4.7 |
| Float32 decision digest | `fff1b77b0dfd776fcf6eefd24451166affb2f465695c5b89d0edc13453dc3d09` |

The [pinned RAW processor](https://github.com/andreavolpato/spektrafilm/blob/c1d0e44b962d80a51ea096d33faea346e4f3836c/src/spektrafilm/utils/raw_file_processor.py)
calls `colour.chromatic_adaptation(..., method='Von Kries')` without a
`transform`. The source and test blobs are unchanged at the latest checked
upstream SHA. In [colour-science 0.4.7's Von Kries implementation](https://github.com/colour-science/colour/blob/v0.4.7/colour/adaptation/vonkries.py),
both the matrix and adaptation functions default `transform` to `CAT02` and
construct `inv(M) * diag(M*target / M*source) * M`. Therefore the Android
comment that equated the upstream call with direct XYZ scaling is incorrect.

The ACES intermediate remains linear AP0 with a D60-like white; the [Academy's
ACES documentation](https://docs.acescentral.com/white-point/) explains that
white-point choice and why an encoding white does not by itself dictate every
reproduction-neutral choice.

## Reproducible fixtures

The dependency-free checker is
`tools/parity/raw_wb_cat_research.py`; its data is
`tools/parity/fixtures/raw_wb_cat_vectors.json`. The fixture covers:

- as-shot and daylight no-op paths;
- 2850 K tungsten;
- a 4100 K / 0.85 tint fluorescent-or-mixed-light proxy;
- the near-6504 K `allclose` skip with a non-neutral tint;
- the 4100 K / 1.000001 near-unity `isclose` tint skip;
- the exposed 1000 K / 0.2 and 12000 K / 1.8 UI extremes;
- three analytic neutral levels, a wide-gamut HDR colour, and fixed ACES patch
  seeds from Samsung native DNG, MotionCam DNG, and Fujifilm X100S RAF paths.

The camera seeds are downstream of raw decode. That is deliberate: once LibRaw
has emitted ACES, the CAT matrix is camera-independent. Camera diversity still
exercises realistic channel mixtures without coupling this small decision
fixture to private or multi-megabyte RAW files.

Run the standard-library lock anywhere:

```text
python tools/parity/raw_wb_cat_research.py --check
```

With the exact research environment installed, independently execute the real
`colour` implementation for every vector:

```text
python tools/parity/raw_wb_cat_research.py --check --verify-colour
```

The second command matched every pinned ACES and float32 ProPhoto bit pattern.
Use `--markdown` for the complete per-scenario table and `--json` for all vector
outputs. The digest covers oracle provenance, inputs, and every candidate's
float32 ACES/ProPhoto bit patterns. It intentionally excludes derived float64
RMS values, whose final libm bit can differ across Python versions without any
pixel-bit change; the standard check was also verified under Python 3.14.

## Measured errors against the CAT02 oracle

Values below are max absolute error at the final float32 ProPhoto engine
boundary across all seven patch vectors. The normal engine tolerance is
`1e-4` max / `1e-5` RMS; these differences are model errors, not harmless
roundoff.

| Scenario | Current XYZ scaling | Bradford | No adaptation | CAT02 with wrong one-cast tint |
|---|---:|---:|---:|---:|
| As-shot | 0 | 0 | 0 | 0 |
| Daylight | 0 | 0 | 0 | 0 |
| Tungsten 2850 K | 0.658978820 | 0.282274723 | 4.010872600 | 0 |
| Fluorescent/mixed proxy | 0.144449472 | 0.035758019 | 1.434883590 | 0.000000238 |
| Near-reference + tint | 0.000002623 | 0 | 0 | 0 |
| Near-unity tint skip | 0.144268394 | 0.035759687 | 1.434819460 | 0 |
| Extreme warm UI minimum | 4.134622620 | 6.268114090 | 18.493125200 | 0 |
| Extreme cool UI maximum | 0.142969131 | 0.012952805 | 0.647493482 | 0.000000060 |
| **Overall RMS** | **0.454605185** | **0.536668775** | **1.611756920** | **0.000000019** |

CAT02 reproduces the real colour-science 0.4.7 oracle bit-for-bit at the
declared float32 boundaries. Bradford is often closer than current XYZ scaling,
but it is still the wrong upstream transform and exceeds tolerance materially.
Two additional diagnostic candidates isolate the oracle's tolerance contracts:
omitting only the near-reference CAT skip reaches `2.62260437e-6`, and omitting
only the near-unity tint skip reaches `2.38418579e-6` at the ProPhoto boundary.

Selected CAT02 example vectors:

| Scenario / patch | ACES after WB | Float32 ProPhoto engine input |
|---|---|---|
| Daylight / neutral 18% | `0.180000007, 0.180000007, 0.180000007` | `0.180032253, 0.179992661, 0.180049390` |
| Tungsten / neutral 18% | `0.203521430, 0.233074456, 0.539498031` | `0.173435479, 0.204379395, 0.541027963` |
| Mixed proxy / neutral 18% | `0.182957128, 0.167528525, 0.308588684` | `0.176068008, 0.154421329, 0.309249699` |
| Tungsten / Samsung DNG seed | `0.011051619, 0.011364083, 0.009403114` | `0.011126388, 0.011545382, 0.009397883` |
| Mixed proxy / MotionCam seed | `0.795815647, 0.532927394, 1.225329760` | `0.806749642, 0.469277591, 1.228109600` |
| Cool extreme / Fujifilm seed | `0.307043731, 0.526261508, 0.312011689` | `0.270780325, 0.545430541, 0.311625093` |

## Failure cases and product limits

- CCT plus a scalar green tint cannot describe a fluorescent SPD, mixed-light
  field, or an arbitrary Duv. The 4100 K case is explicitly a proxy, not a claim
  of spectral relighting.
- Upstream and Android expose 1000 K, while Kang 2002 documents a lower domain
  of 1667 K. To preserve upstream parity, the fixture locks the deterministic
  1000 K extrapolation, but the UI must not present it as physically validated.
  Changing the range or model is a new colour decision and requires a rebaseline.
- Native validation rejects non-finite or out-of-product-range temperature/tint
  before LibRaw and colour math. Host regressions cover NaN, both infinities,
  non-positive temperature, both range boundaries, and out-of-range values.
- Production now preserves the oracle's near-reference `allclose` CAT skip and
  near-unity `isclose` tint skip. Diagnostic candidates remain in the fixture so
  removing either skip fails by up to `2.62260437e-6` / `2.38418579e-6`.
- `as_shot` relies on LibRaw metadata and fallback policy. Missing camera WB is a
  decode-policy issue, not a reason to add this CAT a second time.

## Golden and rebaseline policy

1. The committed JSON's source/test blobs, environment, seven independently
   captured colour-reference bit vectors, and canonical report digest are the
   immutable research baseline.
2. Production consumes the same constants and reproduces all fixture
   ACES/ProPhoto float32 bits, including the separate CAT and tint casts. The
   C++ host gate and connected-device digests above are the implementation
   evidence; a changed output requires the rebaseline process below.
3. Existing engine stage goldens must not be rewritten: they begin after a
   caller supplies linear ProPhoto. Add RAW-import goldens alongside them.
4. A rebaseline is allowed only when the upstream SHA or a pinned numeric
   dependency intentionally changes, the diff identifies model/constants/cast
   changes, and an independent colour review approves it. Never update the
   digest merely to make an implementation pass.

## Sources

- [Pinned Spektrafilm RAW processor](https://github.com/andreavolpato/spektrafilm/blob/c1d0e44b962d80a51ea096d33faea346e4f3836c/src/spektrafilm/utils/raw_file_processor.py)
- [Pinned Spektrafilm RAW tests](https://github.com/andreavolpato/spektrafilm/blob/c1d0e44b962d80a51ea096d33faea346e4f3836c/tests/test_raw_file_processor.py)
- [colour-science 0.4.7 Von Kries source](https://github.com/colour-science/colour/blob/v0.4.7/colour/adaptation/vonkries.py)
- [colour-science 0.4.7 CAT matrices](https://github.com/colour-science/colour/blob/v0.4.7/colour/adaptation/datasets/cat.py)
- [LibRaw output parameters](https://www.libraw.org/docs/API-datastruct-eng.html)
- [ACES white-point derivation](https://docs.acescentral.com/white-point/)
