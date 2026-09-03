# Research: Highway SIMD vendoring + determinism plan

> **Outcome:** Highway is not a shipping dependency. Retain this qualification evidence and reopen
> only for a narrow kernel whose current trace, digest gate, APK impact, and device A/B justify it.
> Live dependency choices are in [../BIT_IDENTICAL_EXPORT_ROADMAP.md](../BIT_IDENTICAL_EXPORT_ROADMAP.md).

**Date:** 2026-08-26 · **Ticket:** [Research: Highway SIMD vendoring + determinism plan](https://github.com/thetechgeekko/Spektrafilm-android/issues/124) — facts for the "adopt [google/highway](https://github.com/google/highway)" decision.

**Question:** can Highway be vendored the way we vendor LibRaw (CMake 3.22.1 FetchContent +
SHA256, NDK r27, three ABIs), and can it be pinned to one instruction set per ABI so the
engine's determinism contract survives — parity vs oracle `max_abs ≤ 1e-4 / rms ≤ 1e-5`,
byte-identical across thread counts per ABI, NaN propagation intact under
`-O3 -ffast-math -fno-finite-math-only`?

Method: everything below comes from the Highway **1.4.0 source tree itself** (cloned at the
release tag over verified git, `HEAD = 2607d3b5b0113992fe84d3848859eae13b3b52c1`), its
`g3doc/`, and Android NDK documentation. **[measured]** = compiled/ran on this host during
research. **[registry]** = confirmed via independent package-registry pins. Anything weaker
is listed under "Confidence / unverified".

---

## 1. Release pin for FetchContent

Latest stable release: **tag `1.4.0`**, commit `2607d3b5b0113992fe84d3848859eae13b3b52c1`
(committed 2026-04-22, release published 2026-04-23 —
[releases/tag/1.4.0](https://github.com/google/highway/releases/tag/1.4.0)). Highlights per the
release notes: `Fast*` math functions, i8mm for SVE/NEON_BF16, RVV/LSX runtime dispatch —
nothing that changes the analysis below. `project(hwy VERSION 1.4.0)` in
[CMakeLists.txt](https://github.com/google/highway/blob/1.4.0/CMakeLists.txt) confirms the
version.

Two pinnable tarballs exist for the same tree — they are different artifacts with different
hashes:

| Artifact | URL | SHA256 | Verification |
|---|---|---|---|
| **Release asset** (recommended) | `https://github.com/google/highway/releases/download/1.4.0/highway-1.4.0.tar.gz` | `36f672ab48ddb3c8555e9e89e16fe400cd7d16c6eb455a1a3d0c146a63ababdc` | **[measured]** reproduced bit-for-bit on this host: `git archive --format=tar.gz --prefix=highway-1.4.0/ 1.4.0` over the verified tag commit yields exactly this hash (git 2.43); **[registry]** identical to Bazel Central Registry's pin (`sha256-NvZyq0jds8hVXp6J4W/kAM19FsbrRVoaPQwUamOrq9w=`, [source.json](https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/modules/highway/1.4.0/source.json)) |
| Auto-generated archive | `https://github.com/google/highway/archive/refs/tags/1.4.0.tar.gz` | `e72241ac9524bb653ae52ced768b508045d4438726a303f10181a38f764a453c` | **[registry]** [Homebrew formula](https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/h/highway.rb) and [conan-center](https://github.com/conan-io/conan-center-index/blob/master/recipes/highway/all/conandata.yml) agree; not self-computed (this session's proxy blocks direct GitHub downloads) |

Prefer the **release asset**: it is an uploaded, immutable artifact, whereas
`archive/refs/tags/` tarballs are generated on demand and have changed hash before (the
January 2023 `git archive` incident). Size: **3,691,557 bytes** (~3.5 MB) [measured — byte size
of the reproduced archive].

**What gets built.** Not header-only (an experimental `HWY_CMAKE_HEADER_ONLY` option exists but
is default-off). The build produces:

- `libhwy` — **static by default** (`option(BUILD_SHARED_LIBS ... OFF)`; set
  `HWY_FORCE_STATIC_LIBS=ON` as belt-and-braces). Contents are small infrastructure
  (target detection, aligned allocator, timer): **[measured]** host x86_64 Release `libhwy.a`
  is ~110 KB with default dispatch, ~99 KB with `HWY_COMPILE_ONLY_STATIC`. The real code is
  header-inlined into our TUs; the `hwy/` tree is 6.5 MB of source.
- `hwy_contrib` — vqsort/image/thread_pool, **not needed**: `hwy/contrib/math/math-inl.h` is
  header-only and reachable with `HWY_ENABLE_CONTRIB=OFF`, because target `hwy` exports the
  source root as a PUBLIC include dir (`target_include_directories(hwy PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>)`).

Options for us: `HWY_ENABLE_TESTS=OFF` (**mandatory** — ON downloads googletest at *configure*
time via `execute_process`, a network fetch that would break reproducible builds),
`HWY_ENABLE_EXAMPLES=OFF`, `HWY_ENABLE_CONTRIB=OFF`, `HWY_ENABLE_INSTALL=OFF`.
`HWY_CMAKE_ARM7` is "only required with GCC < 6.1.0 or CLANG < 16.0" (CMakeLists comment) —
NDK r27 is clang 18, so not needed.

**CMake 3.22.1 suffices:** `cmake_minimum_required(VERSION 3.10)`. The exact
`FetchContent_Declare(URL ... URL_HASH SHA256=...)` + `FetchContent_Populate` pattern from
`lib/libraw/src/main/cpp/CMakeLists.txt` works unchanged (no `DOWNLOAD_EXTRACT_TIMESTAMP`, no
3.24+ keywords) — except here we *do* want `add_subdirectory` for the `hwy` target rather than
populate-only. C++17 is fine (library requires ≥11; 17 only for vqsort, which we exclude).

## 2. Static dispatch: pinning ONE target per ABI

The mechanism, per
[quick_reference.md](https://github.com/google/highway/blob/1.4.0/g3doc/quick_reference.md)
("Targets" / advanced-macros sections) and
[detect_targets.h](https://github.com/google/highway/blob/1.4.0/hwy/detect_targets.h):

- **`HWY_COMPILE_ONLY_STATIC`** — "selects only `HWY_STATIC_TARGET`, which effectively disables
  dynamic dispatch." `HWY_STATIC_TARGET` is the best target enabled by the compiler's
  *predefined macros* (i.e. by the per-ABI compiler flags NDK already sets). This is the whole
  plan: one `-DHWY_COMPILE_ONLY_STATIC` on the engine target.
- `HWY_BASELINE_TARGETS` — "defaults to the set whose predefined macros are defined"; can
  override the baseline explicitly if we ever want to force a level rather than inherit NDK's.
- `HWY_DISABLED_TARGETS` — blocklist ("definitely avoid generating those target(s)"), useful as
  a second fence, e.g. `-DHWY_DISABLED_TARGETS=(HWY_AVX2|HWY_AVX3)` on x86_64.
- The docs require the macro to be defined "when building Highway **as well as any user code
  that includes Highway headers**" — so it goes in the engine's global compile definitions, not
  just on libhwy.

**Static baseline per our three ABIs.** **[measured]** with clang 18 (same major as NDK r27's
clang 18.0.x) using the NDK target triples, `-dM -E` predefines:

| ABI | Predefines (relevant) | `HWY_STATIC_TARGET` | f64? |
|---|---|---|---|
| arm64-v8a (`aarch64-linux-android24`) | `__ARM_NEON`, `__ARM_FEATURE_FMA`; **no** `__ARM_FEATURE_AES` | `HWY_NEON_WITHOUT_AES` — full AArch64 NEON minus the AES-gated crypto ops (unused by us) | yes, 2×f64 |
| armeabi-v7a (`armv7a-linux-androideabi24`) | `__ARM_NEON` (Neon on by default: "For NDK r21 and newer Neon is enabled by default for all API levels" — [NDK Neon guide](https://developer.android.com/ndk/guides/cpu-arm-neon); our minSdk 24 > 21 also guarantees device support) | `HWY_NEON_WITHOUT_AES` (Armv7 flavor) | **no** — f32 SIMD only (§3) |
| x86_64 (`x86_64-linux-android24`) | `__SSSE3__`, `__SSE4_1__`, `__SSE4_2__`; **no** `__AES__`/`__PCLMUL__` | **`HWY_SSSE3` by default — a trap.** Highway's SSE4 baseline additionally requires `__PCLMUL__ && __AES__` (detect_targets.h `HWY_CHECK_PCLMUL_AES`), which the Android ABI does not mandate ("the base instruction set plus MMX, SSE, SSE2, SSE3, SSSE3, SSE4.1, SSE4.2, and the POPCNT instruction" … "does not include … any variant of AVX" — [NDK ABI guide](https://developer.android.com/ndk/guides/abis)). Define **`HWY_DISABLE_PCLMUL_AES`** (documented in detect_targets.h: "If these are disabled, they should not gate the availability of SSE4/AVX2") to get **`HWY_SSE4`**. | yes, 2×f64 |

**Why per-ABI static dispatch preserves "same APK ABI ⇒ same numbers on every device".** With
`HWY_COMPILE_ONLY_STATIC`, target selection happens at compile time from compiler flags; there
is no CPU detection and only one code path per ABI in the `.so`. Every device running the
arm64-v8a build executes the identical NEON instruction sequence; every x86_64 device the
identical SSE4 sequence. That is exactly the engine's current contract (deterministic and
thread-invariant per ABI; cross-*architecture* variance allowed). Runtime dispatch would break
it: the same x86_64 APK would compute with AVX2 lanes on one device and SSE4 lanes on another —
different accumulation widths/orders, different bytes. Static-only also sidesteps the
documented dynamic-dispatch/-m-flag pragma conflicts (quick_reference "if your compiler is
pre-configured … this can interfere with dynamic dispatch", issues #1460/#1570/#1707), and
compiles our SIMD code **once** instead of per-target (no binary-size multiplier).

## 3. f64 support per target

From [set_macros-inl.h](https://github.com/google/highway/blob/1.4.0/hwy/ops/set_macros-inl.h),
NEON section:

```c
#if HWY_ARCH_ARM_A64
#define HWY_HAVE_FLOAT64 1
#else
#define HWY_HAVE_FLOAT64 0
#endif
```

- **arm64 NEON:** f64 supported, 128-bit vectors → **2 lanes** of double.
- **armv7 NEON:** `HWY_HAVE_FLOAT64 == 0` — f64 vector ops are *compile-time unavailable* on
  that target (guard with `#if HWY_HAVE_FLOAT64`), **not** silently emulated inside the NEON
  target. Portable fallbacks that do have f64 (`HWY_HAVE_FLOAT64 == 1`): `HWY_EMU128`
  (all ops in standard C++) and `HWY_SCALAR` — i.e. scalar-speed correctness, same as today.
- **x86_64:** every x86 target has `HWY_HAVE_FLOAT64 1`; SSE4 (and SSSE3/SSE2) are 128-bit →
  **2 lanes** of double. Quantizer/CCTF-class f32 ops get 4 lanes on all three ABIs.

**Implication for our kernels — stated plainly.** The hot 81-band f64 spectral integral
(`scan()`, `print_expose()`, ~2,900 cycles/px) already runs on a hand-rolled 2-lane f64 vector
(`kernels/exp10.h`: `float64x2` NEON on arm64, SSE2 on x86, scalarised on armv7). Highway's
static targets offer **exactly the same 2 f64 lanes on the same ABIs and nothing on armv7** —
adopting it buys the f64 integral **zero additional width on any ship target**. Wider f64
(4-lane AVX2) exists only off-device. Where Highway *does* add lanes is the **f32 kernels** —
the 16-bit export quantizer, CCTF encode, interp/LUT sampling, spatial filters — which get
4×f32 on *all three* ABIs including armv7 (armv7 NEON is f32-capable), where today those loops
are scalar or compiler-autovectorized at the compiler's discretion. Any restructuring of the
f64 integral itself (e.g. pixel-blocked accumulation) changes association order and re-opens
the golden-drift question regardless of library — see `docs/EXPORT_FASTPATH.md` item 7.

## 4. `hwy/contrib/math`: `Exp` yes, `Exp10` no

[math-inl.h](https://github.com/google/highway/blob/1.4.0/hwy/contrib/math/math-inl.h)
provides `Exp` — "Highway SIMD version of std::exp(x). Valid Lane Types: float32, float64 ·
Max Error: ULP = 1 · Valid Range: float64[-DBL_MAX, +706]" — plus `Exp2` (ULP = 2) and `Expm1`
(ULP = 4). **There is no `Exp10`** anywhere in contrib/math (1.4.0 also added lower-precision
`Fast*` variants in `fast_math-inl.h`; no exp10 there either). Composing
`Exp(x·ln10)`/`Exp2(x·log2 10)` inserts an extra rounding *and* different polynomial constants
— guaranteed golden drift for no benefit.

The right move is the one the ticket anticipates: **keep our own exp10** — same degree-8
minimax polynomial, same truncation-based range reduction — expressed in Highway ops if/where
we port it. The load-bearing constraint from `kernels/exp10.h:27-31` carries over verbatim: the
round-to-nearest must be built from **truncation plus branchless correction**, because the
classic `(y + 1.5·2^52) − 1.5·2^52` trick is algebraically cancelled by `-ffast-math`.
Highway's primitive matches: `ConvertTo` (float→int) is documented as "rounds floating point
towards zero" (quick_reference) — the same semantics as our `__builtin_convertvector`
truncation. Do **not** substitute Highway's `Round`/`NearestInt` without auditing their
per-target lowering first (native `frintn`/`roundpd` on NEON/SSE4 are safe single
instructions; emulated paths on lesser targets need the same fast-math audit).

## 5. Highway under `-ffast-math` / `-fno-finite-math-only`

Highway's own position, [faq.md](https://github.com/google/highway/blob/1.4.0/g3doc/faq.md)
Q2.2, verbatim:

> "The -ffast-math flag can have more subtle and dangerous effects. It allows reordering
> operations (which can also change results), but also removes guarantees about NaN, thus we
> do not recommend using it."

So: **no upstream support claim for our flag set** — the same status quo as our current
vector-extension code, which already lives under these flags with the parity suite as referee.
Empirically: **[measured]** a probe TU compiled against Highway 1.4.0 with exactly
`-O3 -ffast-math -fno-finite-math-only -DHWY_COMPILE_ONLY_STATIC` builds warning-free with
both host g++ 13 and clang 18, and **NaN propagates through `Add`, `Mul`, and `MulAdd`**
(all lanes NaN in, NaN out; host static target SSE2, 2×f64). The mechanism is the one the
engine already relies on: these wrappers lower to LLVM IR float ops, and
`-fno-finite-math-only` withholds the no-NaN assumption — the identical contract
`kernels/exp10.h` and `runtime/stages/scanning.cpp` (NaN profile entries → `isnan()` guard →
0) depend on today. This is an engine-level property to keep gating in CI, not a Highway
guarantee.

Adjacent documented caveats worth knowing:

- FAQ Q2.2 also notes `-ffp-contract`/FMA fusion "typically changes the end results by around
  10^-5", and that `MulAdd` is fused on some targets and not others — that is *cross-target*
  variance, which our policy already permits (arm64 vs x86 goldens differ within tolerance for
  the same reason today).
- `Min`/`Max` NaN behavior is explicitly target-specific: "If either argument is qNaN, x86
  SIMD returns the second argument, Armv7 Neon returns NaN…" (quick_reference). The engine's
  NaN handling uses explicit `isnan` guards rather than vector min/max on NaN, so this is a
  reviewer checklist item, not a blocker: never route NaN semantics through `Min`/`Max`
  (use `MinNumber`/`MaxNumber` if IEEE-2019 semantics are ever needed).
- FetchContent + `add_subdirectory` means libhwy's few `.cc` files inherit our global Release
  flags. They are non-FP infrastructure (CPU detection, allocator, timer), and with
  static-only dispatch mostly inert; scope per-target flags only if we want Highway built
  IEEE-strict on principle.

## 6. License

[LICENSE](https://github.com/google/highway/blob/1.4.0/LICENSE), verbatim: "This project is
primarily dual-licensed under your choice of either the Apache License 2.0 or the BSD 3-Clause
License. The following files are licensed under different terms: hwy/contrib/random/random-inl.h:
CC0 1.0 Universal." (README: "Previously licensed under Apache 2, now dual-licensed as Apache
2 / BSD-3.")

- **NOTICE obligations:** the 1.4.0 tree ships **no NOTICE file**, so Apache-2.0 §4(d)'s
  NOTICE-propagation clause has nothing to propagate. Remaining obligations either way
  (Apache-2.0 or BSD-3): include the license text and retain copyright notices — i.e. one
  attribution section in `NOTICE.md` (the repo's existing pattern) naming Highway, the
  version, and the dual license. We vendor at build time (no committed third-party blob), same
  as LibRaw.
- **Exclusion:** `hwy/contrib/random/random-inl.h` is CC0 — we do not use it (the engine's
  grain/glare RNG is its own), and `HWY_ENABLE_CONTRIB=OFF` keeps contrib out of the build.
- **One-liner for the `docs/LICENSING.md` table:**
  `| Highway (SIMD, vendored at build time) | Apache-2.0 OR BSD-3-Clause (dual; our choice) | Both GPLv3-compatible — Apache-2.0 one-way into GPLv3, BSD-3 permissive; no upstream NOTICE file; contrib/random (CC0) excluded from the build. |`

## Confidence / unverified

Honesty ledger — everything above not tagged otherwise is read directly from the 1.4.0 tree.

- **Release-asset SHA256 (`36f672ab…`):** verified at content level (bit-exact local
  `git archive` reproduction from the verified tag commit) and by Bazel Central Registry
  agreement — but the asset itself could **not** be downloaded through this session's proxy.
  Re-run `curl -L <asset-url> | sha256sum` from an unproxied machine before merging the pin;
  `URL_HASH` will in any case fail closed at first configure if wrong.
- **Codeload-archive SHA256 (`e72241ac…`):** two independent registries agree; **not**
  self-computed. Only relevant if the release-asset URL is rejected.
- **NDK predefines:** measured with stock clang 18 + NDK target triples on this host, not the
  literal NDK r27 binary (r27 ships clang 18.0.x; the driver defaults are upstream). Confirm
  with `$NDK/toolchains/llvm/prebuilt/*/bin/clang -dM -E` per ABI when wiring the flags —
  in particular that `HWY_STATIC_TARGET` resolves to `HWY_NEON_WITHOUT_AES` / `HWY_SSE4`
  (a `static_assert(HWY_TARGET == …)` in the engine would pin this permanently).
- **NaN probe ran on host x86_64 only** (SSE2 static target). arm64 NEON behavior is inferred
  from the identical IR-level mechanism; needs the usual device/CI arm64 run.
- **Per-target lowering of `Round`/`NearestInt`** under `-ffast-math` was not audited (we
  recommend `ConvertTo` truncation instead, which is documented and matches current code).
- **1.4.0 release-notes summary** came from the GitHub release page via a summarizer; the tag,
  commit, and version were verified directly from git and CMakeLists.
- **armv7 `MulAdd` fusion status** (NDK default `-mfpu` FMA availability) unverified —
  informational only, since cross-arch variance is permitted.

## Recommendation to the decision ticket

Vendoring is low-risk and mechanical: pin the 1.4.0 **release asset** with the LibRaw
FetchContent pattern (CMake 3.22.1 is ample; tests/examples/contrib/install off; static lib;
~3.5 MB fetch, ~100 KB `.a`), define `HWY_COMPILE_ONLY_STATIC` + `HWY_DISABLE_PCLMUL_AES`
engine-wide, and determinism per ABI is *stronger* than today's hand-rolled code because the
instruction selection becomes an explicit, asserted contract instead of a compiler-lowering
accident. But be clear-eyed about the payoff: Highway gives the 81-band f64 integral **zero
new lanes on any ship ABI** (2×f64 on arm64/x86_64, nothing on armv7 — exactly what
`exp10_vec` already delivers), has **no `Exp10`** (our polynomial and truncation-based range
reduction must be ported, not replaced), and its own FAQ recommends against `-ffast-math`, so
every parity guarantee stays ours to enforce. The honest case for adoption is therefore the
**f32 tier** (quantizer, CCTF, interp, spatial filters — 4 lanes on all three ABIs, including
armv7 where f64 work is impossible anyway) plus maintainability, with the f64 integral ported
only if the same-bytes parity holds lane-for-lane. If the decision is adopt: land it as
vendoring + one f32 kernel first, `static_assert` the target per ABI, and let the 35-test
parity suite plus `test_parallel` referee every subsequent kernel port — the same
one-revertible-commit discipline as `docs/EXPORT_FASTPATH.md`.
