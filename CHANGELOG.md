# Changelog

## Unreleased

### Transactional storage and versioned documents (#170)

- MediaStore export now stages bytes behind an app-owned pending row, verifies
  the closed payload digest, and publishes the final display name exactly once.
  Failed writes are cleaned up; ambiguous provider outcomes are reconciled
  before another export, including after process death. Export work and its
  terminal result are process-owned, so Activity recreation cannot cancel a
  commit or turn a confirmed publication into a duplicate retry.
- Source-picker access now persists durable URI grants, restores them across
  launches, releases replaced grants, and presents a reauthorization state when
  a document moves or permission is revoked. FIFO mutation ordering and durable-
  state reconciliation cover rapid selection, demo-clear, and recreation races.
- Recipe, preset, sidecar, and mask JSON now use bounded/versioned decoding,
  atomic replacement, corruption quarantine, explicit migrations, and stable
   mask schema identifiers. Exact numeric parsing prevents Android `Double`
   rounding from accepting fractional/future versions; classification/quarantine
   and recipe save/read/reset are race-fenced. Unsupported future recipes remain
   untouched. Shared editor/import limits now reject spectral, upscale, film-format,
   grain, and halation values that could otherwise amplify native allocation or
   sampler work, even when the persisted effect is currently disabled. The native
   grain sampler now short-circuits its numerically inevitable probability-underflow
   result without changing RNG order and safely handles invalid/overflowing derived
   counts and Poisson conversion.
- Android 7-9 gallery export now requests and rechecks the max-SDK-28 legacy
  storage permission; denial stops before any public write.
- Added release-targeted storage instrumentation and JVM regressions for
  interrupted writes, orphan pending rows, duplicate names, permission loss,
  process restart, malformed/oversized JSON, and stale autosave fencing.

### Profile-driven scan viewing illuminants (#169)

- Added one exact, fail-closed viewing-illuminant registry. Profile loading now
  resolves `D50` or `K75P` once and shares its spectrum, XYZ normalization, and
  per-output-space adaptation matrices across every scan consumer; unsupported
  or misspelled identifiers are rejected instead of falling back to D50.
- Kodak 2383 and 2393 now use their declared Kinoton 75P illuminant through the
  CPU direct scanner, scanner 3D-LUT/cache, Vulkan linear and fused preview,
  experimental GPU export, viewing glare, and scanner black/white reference
  measurement.
- Added upstream-oracle full-route fixtures for both cinema-print stocks and
  matching LUT17 fixtures, isolated references for all six output spaces, and
  hashed K75P/D50 visual evidence. Direct and LUT modes are now compared with
  their matching pinned upstream modes instead of widening a cross-mode band;
  exact export keeps the intentionally approximate scanner LUT disabled.
- Extended scanner-LUT/cache, GPU-host, output-space, C API/JNI, and black/white-
  correction regressions. The global GPU self-check sees K75P first; malformed
  identifiers return `SPK_ERR_PROFILE_INVALID` with exact detail, and reference
  measurement is viewing-sensitive, finite, active, and repeat-deterministic.
- Golden generation now verifies the actual imported checkout is exactly the
  pinned commit and Git-clean both before and after oracle execution; wrong or
  dirty checkouts fail closed, and manifests record the verified commit/state.
  Profile-load allocation failures are contained as `SPK_ERR_OOM` at the C ABI.

### RAW white-balance CAT research (#167)

- Pinned the upstream RAW processor/test blobs and the exact Python, NumPy, and
  colour-science environment. Upstream's `method='Von Kries'` resolves to CAT02;
  the current native diagonal-XYZ approximation is not equivalent.
- Added a dependency-free, digest-locked vector harness covering as-shot,
  daylight, tungsten, mixed-light proxy, CCT/tint extremes, neutral/HDR patches,
  and Samsung/MotionCam/Fujifilm camera seeds. The harness independently matched
  colour-science 0.4.7 bit-for-bit at the declared float32 boundaries.
- This was a research-only decision: production pixel math is unchanged and a
  separate implementation/golden task is required.

### Fail-closed exact release candidate (#168)

- Stable-tag releases now bind one unsigned R8 candidate to the exact source SHA,
  version, workflow run/attempt, uploaded artifact database ID/digest, both APK
  hashes, all six Gradle locks, deterministic app/LibRaw SPDX, runtime classpath,
  R8 mapping, full native symbols, fixed gate attestation, and provenance.
- The candidate job reruns `-O2` and shipping-flags engine parity, release JVM
  tests/lint, R8/JNI, legal, and 16 KiB checks. The protected job downloads only
  that artifact ID, signs the exact app/instrumentation pair with no debug-key
  fallback, proves key cleanup and certificate pinning, then verifies installed
  byte identity, release instrumentation, cold launch, and app-fatal logs on API 35.
- GitHub publication uses a stdlib, numeric-ID publisher that requires immutable
  releases and exact remote bytes. It accepts only an already immutable,
  byte-identical rerun and may delete only its own draft—never a published or
  foreign Release. Production still requires the human LibRaw route approval and
  protected Environment credentials/settings; local debug-test-signed device
  evidence is not production-signing evidence.

### Parallelize the serial per-pixel maps: LUT apply + spatial filters (#122)

Three families of serial hot loops now run through `kernels/parallel`'s
deterministic fork-join — same arithmetic per pixel/row/column, chunk
boundaries a pure function of (count, threads), so output stays byte-identical
for any worker count (36-gate suite green; `test_parallel` extended with
dedicated LUT-acceleration and diffusion-filter scenarios, all 7 scenarios
memcmp-identical at 1 vs 8 workers):

- **3D-LUT PCHIP apply** (`kernels/lut3d.cpp`) — the per-pixel interpolation
  and its input normalization are chunked over pixels. Preview force-enables
  both spectral LUTs, so this ran serially on every preview frame of both
  routes.
- **Gaussian / exponential filters** (`kernels/gaussian.cpp` float32,
  `kernels/exponential_filter.cpp` float64) — FIR passes and the IIR
  horizontal sweep chunk over rows; the IIR vertical sweep chunks over columns
  with chunk-local recurrence state (each column runs the exact serial op
  sequence); the per-channel de/re-interleave and the exponential surrogate's
  init/copy/axpy passes chunk over elements. Serves halation, lens blur,
  scanner blur + unsharp, DIR-coupler diffusion, grain's field blurs, glare.
- **Optical diffusion filter + halation maps** (`model/diffusion.cpp`) — the
  direct O(w·h·ks²) convolution and its reflect-pad build chunk over rows; the
  scatter/halation mix and accumulate loops chunk over elements. (The
  convolution algorithm itself is unchanged — no separable rewrite; that would
  touch parity numerics.)

New `parallel_for_weighted(begin, end, unit_work, body)` in
`kernels/parallel.h`: same deterministic chunking, but the serial-below-
min-chunk clamp measures pixel-equivalents of work instead of the bare item
count — a few-thousand-row image no longer collapses to one chunk when each
row carries a full image width of work.

**Measured** (host, 4-core container, 3.1 MP, median of 3; before ≡ 1 worker,
which runs the identical serial code path):

| kernel | 1 worker | 4 workers | speedup |
|---|---:|---:|---:|
| LUT PCHIP apply (17³) | 255.5 ms | 103.2 ms | 2.5× |
| Gaussian f64 IIR σ=6 | 155.3 ms | 52.2 ms | 3.0× |
| Gaussian f64 FIR σ=1.2 | 205.5 ms | 74.5 ms | 2.8× |
| Gaussian f32 IIR σ=6 | 123.5 ms | 39.4 ms | 3.1× |
| Gaussian f32 FIR σ=1.2 | 129.3 ms | 37.3 ms | 3.5× |
| exponential filter (4 px) | 656.4 ms | 251.6 ms | 2.6× |
| halation scatter+3-bounce | 1801.7 ms | 871.4 ms | 2.1× |
| diffusion conv (512×384) | 8704.4 ms | 2226.9 ms | 3.9× |

FNV checksums identical across 1/4/8 workers for every kernel.

Route-level (640×480 preview-class renders, every-render memo miss, 4 workers,
old binary vs new binary — output checksums identical across the two builds):
print route with both spectral LUTs 213.6 → 174.6 ms (**−18%**), scan route
with halation 389.1 → 316.8 ms (**−19%**), scan route with halation + the
Black Pro-Mist diffusion filter at strength 0.8 49.2 s → 13.4 s (**3.66×** —
the direct convolution dominates that config; an algorithmic replacement is a
separate, parity-affecting decision).

### Export fast path, part 2 — retire the full-res float64 intermediates (#121)

A 12 MP export used to allocate and zero-touch ~900 MB of full-resolution
float64 intermediates (`src`, `rgb`, filming `raw`, scanning `lin_rgb`) before
doing useful work — peaking at ~1.1 GB, inside lmkd-kill territory on 4 GB
devices. This is allocation/lifetime work only: float64 stays wherever the
parity contract computes in float64, no arithmetic changed, and every output
is byte-identical (36-gate suite green; `test_simulate_e2e` scenario G now
also gates the direct path vs the materialized path, AE-on and spatial-on
included).

- **Direct float32 filming** on one-shot renders with no-op geometry (export,
  magnifier): filming reads the caller's float32 frame through
  `expose_f32_gain` with the auto-exposure gain folded into each pixel load,
  and AE meters the float32 frame directly (`measure_auto_exposure_ev_f32`) —
  the ~288 MB float64 image never exists. Byte-identical by construction
  (float→double widening is exact; the gain multiply is the same double op in
  the same order). `spk_meter_exposure_ev` drops its full-res float64 scratch
  the same way.
- **Fused per-pixel passes**: `expose` and `scan` fuse their compute and
  encode loops whenever no spatial/pointwise op runs between them (every gate
  mirrors that op's own activation condition), so the ~288 MB `raw` and
  `lin_rgb` planes exist only when an active effect actually needs them — and
  then as uninitialized buffers instead of zero-filled vectors.
- **Free-at-last-use**: the float64 image is released right after `expose`,
  the film density right after `print_expose`; the geometry passthrough moves
  instead of copying; stage float32 buffers allocate only when their path
  runs.

**Measured** (12 MP host render, `VmHWM`): print route **1.10 GB → 0.43 GB
(−61%)**, scan route **0.97 GB → 0.43 GB (−55%)** — render transients over
the process baseline drop ~840 MB → ~140 MB; grain-on print 1.55 → 1.28 GB
(grain's own buffers are a separate, future item). No speed cost — the killed
memsets and fused loops make it slightly faster (cold scan 196.9 → 179.8 ms,
cold print 336.2 → 329.5 ms at 512², 4 threads).

### Export fast path, part 1 — EXPORT_FASTPATH items 1+2 (#120)

Bit-exact speed work; every default render path stays byte-identical (36-gate
parity suite green; kernel outputs verified byte-identical over 12.6 M
randomized lookups).

- **O(1) uniform-axis density lookups** (`kernels/uniform_axis.h`): the
  per-pixel binary searches in the exposure→density interpolation
  (`interp1d_planar3`) and the DIR-coupler interpolation
  (`fast_interp_channel`) are replaced by a load-time uniformity check + an
  O(1) index estimate + a fix-up walk that restores the exact searchsorted
  bracket, so the interpolation runs on identical operands. Every bundled
  profile's log_exposure axis qualifies; anything else (non-uniform,
  descending, repeated knots) keeps the identical binary search. Host,
  4.19 M px, 1 thread: exposure→density 725.5 → 65.6 ms (**11.1×**), DIR
  couplers 637.3 → 70.3 ms (**9.1×**); cold scan route −9.4%, cold print
  route −6.0% (512², 4 threads).
- **`fast_interp_channel` NaN guard**: a NaN query previously read `xa[-1]`
  (undefined behavior); it now propagates NaN like `np_interp_array`.
- **One-shot renders stop paying the memo machinery**
  (`spk_params.disable_buffer_memos`): the film-density/print-density memo
  keys FNV-hash the entire input buffer — pure overhead for an export whose
  key can never be re-used. The app's JNI now sets the flag for every
  non-preview render (export, magnifier crop): no key hashing, no full-size
  result copies held in the engine, and warm preview slots survive untouched.
  Measured −275 ms per one-shot render at 3.1 MP (≈ −1.1 s at 12 MP, linear).
  Preview misses that do memoize now compute the key once instead of twice.
  Gated by the new `test_simulate_e2e` scenario G (pixels byte-identical, all
  six memo counters frozen, warm slots kept).

## v0.9.0 (versionCode 11) — 2026-08-26 — parity fixes, gamut compression, spatial decoupling, fork engine adoption + the speed pass 🔍⚡

Everything landed after the v0.8.0 release (2026-06-09): a codebase-wide review
(`docs/CODE_REVIEW_2026-06-24.md`) and its top-priority fixes, the
upstream-sync opt-in ports (PR #105), the gamut-compression activation + Kotlin hardening
backlog (PR #109), the "exact + fast" pass (2026-07-02): per-effect spatial decoupling,
the print-route spatial/grain enable, engine memoization, and the Oklch/Oklrab perceptual
output-gamut options (PRs #111/#112) — plus the VirtuaTOA fork engine adoption (PR #130,
below). The host parity suite grew 26 → **36 gates**, all green; every
default engine path stays byte-identical to the oracle except the corrections/enables
explicitly listed below.

### Adopted from the VirtuaTOA camera fork (PR #130, 2026-08-26; verified + hardened, issues #118/#125)
- **Parallel load-balanced grain** — fixed 8192-px blocks with per-block SplitMix64 seeds and
  dynamic scheduling: grain-only 12 MP measured 114.8 s → 35.2 s (3.26×) on 4 cores, byte-identical
  for any worker count (proven by `memcmp` at 12 MP and a new multi-block `test_parallel` scenario).
  ⚠ **The grain field differs from ≤ v0.8.0 for identical (image, seed)** — old exports are not
  grain-bit-reproducible; output stays statistically inside the oracle's bands (`test_grain`).
- **Spectral 3D-LUT memo cache** (scanner + enlarger) — preview's per-frame LUT rebuilds are
  memoized behind exact, length-prefixed byte keys; new CI gate `test_lut_cache_e2e` (suite 35 → 36).
- **Debug native builds now compile `-O2`** (previously an unoptimized `-g`-only default).
- **AE-off `.cube` bake fix + `spk_meter_exposure_ev`** — the bake no longer meters the synthetic
  identity lattice; the experimental GPU preview bakes an sRGB-shaped lattice and applies the
  engine-metered exposure gain (exports stay linear/unshaped); shaper path covered by a new
  `test_bake_lut` property case.

### App waves (PRs #90–#103, merged before this cycle)
- **Masking v1** — radial/linear masks plus luminance and color-range masks, 13 local ops
  (including the Class-S spatial ops), draw-on-preview overlay, and eyedroppers.
- **Lightroom-style export sheet** — JPEG/UltraHDR with quality + size options, PNG16, TIFF16,
  TIFF32F, and scene-linear untagged TIFF32F.
- **LUT export** — `.cube` + CLF v3, in 17/33/65 sizes.
- **WB wave** — gray-point eyedropper, film-stock balance/virtual-85, auto-exposure default ON.
- **Preset amount slider.**
- **Onboarding** — help sheets + Basic/Advanced disclosure, slide-mode suggestion.

### Added
- **Spatial-effect decoupling (parity fix).** Every spatial effect (DIR-coupler diffusion,
  camera diffusion filter, camera lens blur, halation/scatter, scanner blur/unsharp) now gates
  on its OWN params (zero = inert), matching the oracle's per-effect self-gating;
  `halation_active` gates only halation/scatter (its UI meaning). Previously one master toggle
  silently killed them all. New gate `test_spatial_decouple_e2e`
  (golden `scan_portra_lensblur_nohalation`, oracle c1d0e44).
- **Print-route film spatial + grain (parity fix; INTENTIONAL look change).** The print route
  now runs the same per-effect-gated filming as the scan route — halation/scatter, DIR spatial
  diffusion, camera diffusion, lens blur, and AgX grain all carry into the print, matching the
  oracle's single FilmingStage (pre-fix divergence ~1.8e-2). New gate `test_print_spatial_e2e`
  (golden `print_portra_spatial`); grain half verified statistically vs the oracle
  (mean_abs 1.468e-2 both). Default prints in the app now show film character (halation
  defaults on); the fit preview is unaffected.
- **Gamut compression, end-to-end (opt-in / default-OFF).** Output: ACES RGC v1.3; input:
  radial-to-locus xy compression baked into the filming tc_lut. Both wired
  engine → JNI → facade → two dropdowns under Simulation → Output, round-tripping in recipes
  (old recipes → OFF, unchanged look). Gates `test_gamut_out_aces` / `test_gamut_in_xy`.
- **Oklch perceptual output-gamut compression (opt-in, default-OFF).** A third output-gamut
  option (`OutputGamutCompress.OKLCH`, C++ `kOklch=3`): perceptual-hue-preserving chroma
  compression at constant Oklch(L, h) — a Reinhard knee on `C / C_max` with `C_max` regenerated
  in-engine by a 64×720 bisection, float64 matrices captured from colour-science. Selectable as
  "Oklch (perceptual, keep hue)" in the Output-gamut-compression dropdown; recipes round-trip by
  ordinal. Default-OFF is strict byte-identical; the active path is bit-exact to the oracle
  (`27bd085`). New gate `test_gamut_out_oklch`. First slice of the perceptual output-gamut work
  (P2 #6); the second slice, **Oklrab** (Oklch indexed by Ottosson's rebased lightness Lr), landed
  in PR #112 with gate `test_gamut_out_oklrab`; `jzazbz`/`cam16ucs` remain reserved slots (unported).
- **Highlight boost trio** (`boost_ev`/`boost_range`/`protect_ev`) ported into `expose`
  (pre-clip highlight reconstruction; gate `test_highlight_boost_e2e`).
- **Print density-curve morph (s023), opt-in / default-OFF.** First feature from the upstream-sync
  plan. Ports `utils/morph_curves.py::apply_print_curves_morph`: when enabled, the print *develop*
  step rebuilds the paper's density table from its parametric `density_curves_model` and morphs it
  by a coupled gamma (global × band(fast/slow by grain speed) × RGB) plus a developer-exhaustion
  Gumbel blend (D(0)-preserving via a brentq offset), instead of interpolating the stored table.
  New `model/morph_curves.{h,cpp}`; `profiles/profile.cpp` now parses `density_curves_model`; wired
  through `spk_params` → JNI → the `PrintCurvesMorphParams` facade. **Default-off is a strict no-op**
  (every print golden byte-identical); the active path is bit-exact vs the oracle (new gate
  `test_print_curves_morph`, `max_abs 0.0`). Editor UI control still to come.

### Fixed
- **RAW input colorspace (default path; changes native RAW/DNG renders).** LibRaw decodes RAW to
  ACES2065-1, but the engine ingests linear ProPhoto RGB and the JNI hardcoded the input tag to
  ProPhoto — so every native RAW/DNG decode ran ACES pixels through the ProPhoto primaries (wrong
  chromaticity on the core editor flow). `raw_decoder.cpp` now converts ACES2065-1 → linear ProPhoto
  RGB with a baked CAT02 matrix as the final decode step — mirroring `raw_file_processor.py`'s
  `colour.RGB_to_RGB(..., output_colorspace="ProPhoto RGB")` — and tags the result "ProPhoto RGB".
  The conversion is verified vs colour-science 0.4.7 (`test_aces_prophoto`, `max_abs 5.96e-08`; the
  libraw module isn't in the host parity suite, so the matrix is the gate). `nativeSimulate` now
  validates the buffer's colorspace tag and throws on anything but ProPhoto instead of silently
  re-interpreting it (the hole that hid this bug). The inert UI "Input color space" selector is
  gated honestly (the engine renders all input as linear ProPhoto). The platform/photo decoder
  already emitted ProPhoto, so only native RAW renders change — a correction onto the oracle.
- **Auto-exposure metering parity (default path).** `small_preview` — the ≤256 px downscale the
  auto-exposure stage meters on — now applies skimage's anti-aliasing gaussian prefilter
  (`sigma = max(0,(in/out−1)/2)` per axis, `mode='mirror'`, `truncate=4.0`) before the order=0
  nearest resample, matching `skimage.transform.rescale`. The prefilter was omitted, so the
  metered EV (a single global gain on every pixel) diverged from the oracle on every >256 px
  import. The shared kernel (`build_gaussian_kernel`) is reused from the crop/resize stage and
  evaluated fused per sampled output pixel, so a full-resolution export does not allocate a second
  `w*h*3` buffer. New gate `test_small_preview_aa` (stage-local skimage golden, 384×200→256×133;
  matches to 5.95e-08).
- **Oversize-crop slice semantics.** `crop_image` now follows NumPy negative-start-from-end slicing
  when `crop_size > 1.0` (a crop larger than the image) instead of clamping the start to 0 and
  reading the whole axis. In-bounds crops are byte-identical (the crop golden is unchanged).
- **`spk_simulate` null guard.** The public C entry point validates `eng`/`in`/`in->data` before
  dereferencing, matching `spk_simulate_preview` / `spk_simulate_tap`.
- **`grain.cpp` dead store removed** (`dmax_frac` was written but never read; `frac` is used
  inline). No behavioural change.
- **np.interp port for the non-monotonic DIR-coupler axis** (`np_interp_array` reproduces
  numpy's order-dependent bracket choice; gate `test_np_interp`, bit-exact over 82 cases).
- **float→half now true round-to-nearest-even** (normal + subnormal, full sticky;
  bit-identical to numpy float16 over 33k inputs).
- **Kotlin/UI robustness batch**: preset/recipe/diagnostics IO off the main thread with
  serialize-on-main (no torn Compose-state snapshots); undo step no longer lost when an edit
  lands in the restore window; recipeKey `remember`; ROI/magnifier dispose-cancel; crop
  constrain anchors the opposite corner; preset-import `optDouble` safety; rotation-buffer
  overflow guard (+`RotationTest`); GPU LUT preview re-arms after GL context loss;
  RawCoilDecoder frees its off-heap buffer; `SpektraEngine` guards simulate/bake against a
  closed engine.
- **Honest disclosures**: DIR-gamma matrix sliders are visibly gated (per-stock engine
  overwrite — no effect); the Glare section notes it applies on the print route only;
  MALLETT2019 stays gated as unimplemented.

### Performance
- **Film-density memo on BOTH routes** (was print-only): single slot per route, keyed by a
  full content+param digest that now folds every deterministic spatial shape param — so
  spatial-ON renders memoize too; bypass only for debug taps + grain. Key-completeness is
  test-enforced per param.
- **Print-density memo**: `print_expose`+`print_develop` memoize on the film-density buffer
  CONTENT + all printing inputs (plus the tc_lut-shaping film params the midgray factor reads
  directly) — an output-only edit (scanner/output space/tone curve/glare) reruns `scan()`
  alone. Works even with grain on (seeded grain is deterministic, so the content hash matches).
- **Serial loop parallelization (S4)**: the DIR-coupler develop loops (pointwise + spatial
  variant), the exposure→density interpolation, and `expose`'s bw-correction/log10 tails now
  run through the deterministic `parallel_for` — byte-identical output for any thread count
  (grain stays serial; the recursive blur filters stay serial).
- **Retained-result grade cache (Kotlin)**: Saturation/Vibrance/Gamut/mask edits re-grade the
  retained pristine engine output — zero native work on grade-only edits, in both the draft
  and settle passes.
- Bench (512×512, deterministic params, `SPK_NUM_THREADS=8` on a 4-core host, median): warm
  print edits that keep the film density (y-shift steady state, output-only) run **153–162 ms
  vs 402 ms cold**; warm scan repeats / output-only edits **144–159 ms vs 243 ms cold** (the
  per-route film memos). S4 takes cold scan **243→211 ms (−13%)**. A tap-based decomposition
  shows the print stages themselves cost only ~5 ms at this size (print_expose was already
  parallel), so the print-density memo's absolute win grows with export resolution and on the
  enlarger-diffusion/enlarger-LUT paths, and it is what keeps grain-ON print edits (film memo
  bypassed) from re-running the print stages.

### CI
- **engine-parity now honors the test exit code.** `build_run` fails on a non-zero test exit
  (`PIPESTATUS[0]`) or empty output, not only on a "FAIL" string — so a missing-asset/setup
  failure or a crash before any print can no longer silently report green. The grep stays
  case-insensitive.
- **Gate count 26 → 34**: + `small_preview_aa`, `print_curves_morph`, `np_interp`,
  `gamut_out_aces`, `gamut_out_oklch`, `gamut_in_xy`, `spatial_decouple_e2e`, `print_spatial_e2e`.
  `test_parallel` runs each thread-count on a fresh engine (the memo would otherwise make
  the 1-vs-8 filming comparison vacuous) and adds the print+grain+halation case.

## v0.7.0 — engine completion: APK-direct assets + enlarger LUT 🎞️

Closes the two long-standing engine remainders, both verified on-device (Galaxy S25 Ultra,
Android 16, arm64) and parity-gated. No change to the bit-exact default/export path.

### Engine
- **Assets read directly from the APK (AAssetManager).** Profiles, the spectral upsampling LUT,
  and neutral print filters are now read straight from the packaged assets via `AAssetManager`,
  so the app no longer extracts the ~17 MB `spektra/` tree to internal storage on first launch
  (faster first run, no duplicate on-device copy). The on-disk path is preserved as a fallback;
  all Android-only code is `#ifdef __ANDROID__`-guarded so the host parity build is unchanged.
- **Enlarger 3D-LUT acceleration wired (`use_enlarger_lut`).** The print-expose spectral integral
  can now be PCHIP-interpolated through a per-channel 3D LUT (the print-side analogue of the
  scanner LUT), mirroring the spektrafilm oracle. Opt-in and **default-off**, so the default and
  export renders stay bit-exact; the new `test_enlarger_lut_e2e` gates it in CI. This was the last
  reserved engine LUT flag.

### Docs / quality
- Preset count corrected to **21** (the "Neutral (Adobe-like)" preset is now documented), AUDIT
  refreshed to current truth, and an on-device v0.7.0 re-validation recorded (full-res export with
  no OOM, presets re-render, rotate→export dimension swap).

## v0.6.x — RAW export out-of-memory fix 🧠

Device-confirmed fix for the OutOfMemoryError on loading/exporting large RAW/DNG files
(reproduced on a real Galaxy S25, Android 16). No change to the bit-exact render/export result.

### Fixed
- **Full-resolution RAW + engine buffers moved off the ART managed heap.** A full-res decoded
  linear float buffer is ~140 MB; on Android `ByteBuffer.allocateDirect` is a non-movable `byte[]`
  on the ~256 MB managed heap, so the full-res RAW input plus the engine's equally-large output
  could not coexist there and OOMed on export. Following Lightroom, those large buffers are now
  allocated natively (`malloc` + `NewDirectByteBuffer`) and reclaimed explicitly via
  `AutoCloseable` (`LinearImage`/`SimResult`), keeping the full-res pixels off the managed heap.
- Supporting work: file-descriptor RAW decode, an opt-in half-size decode + OOM-retry ladder for
  borderline-memory devices, and direct-buffer file fallbacks.

## v0.5.0 — Lightroom-feel editor wave ✨

A usability/feel pass informed by a deep reverse-engineering study of Lightroom mobile, plus
the Android 15 compatibility + full-resolution export fixes.

### Editor
- **Progressive preview** — slider edits now paint a fast coarse pass first, then refine to full
  resolution, so tuning feels immediate instead of waiting one full render per change. The coarse
  source is decoded separately so it never evicts the cached full-res proxy that look-edits reuse.
- **Slider haptics** — a light tactile tick when a slider drag settles.
- **Double-tap to reset a slider** — double-tap any value pill to snap that control back to its
  neutral default (with a haptic), Lightroom-style. Wired across all 50 single-value sliders.
- **Editor coach marks** — a one-time tip overlay (tap-a-category / before-after / 100% inspect /
  pinch-zoom) the first time the editor opens.
- **Sticky adjustment category** — the open adjustment section now survives a trip to Settings/
  About and back, so you return to where you were editing.
- **GPU preview (beta, opt-in)** — Settings → Experimental adds a GPU LUT preview path (renders the
  live preview by GPU-sampling a 3D LUT of the current look). Default OFF; export and the bit-exact
  film core are unaffected; grain/halation and zoom/compare are not on this path yet.

### Research / docs
- `docs/RESEARCH_LENS_BOKEH.md`, `docs/RESEARCH_FILM_CHARACTER.md`, `docs/IMPROVEMENT_BACKLOG.md`,
  `docs/ENGINE_WIRING_PLAN.md` — a reverse-engineering + film-imaging research wave guiding the
  next feature cycle (masking/local adjust, tone curve, color grading, depth-aware bokeh, lens/
  scatter character). Design studies only; no behavior change.

### Fixes
- **16 KB page-size compatibility (Android 15)** — the app now loads on devices with a 16 KB
  memory page size. Two changes were required: (1) the native libraries we build
  (`libspektra`, `libsfraw`, `libsftiff`, `libsfpng`) are now linked with 16 KB-aligned
  `PT_LOAD` segments — the build moved to **NDK r27** (16 KB by default) and each `CMakeLists`
  also pins `-Wl,-z,max-page-size=16384`; and (2) the APK now uses **build-tools 35**, whose
  `zipalign -P 16` page-aligns the (uncompressed) bundled `.so` to 16 KB offsets in the zip —
  without this the already-16 KB-aligned prebuilt libs (`libc++_shared`,
  `libdatastore_shared_counter`, `libandroidx.graphics.path`) still failed to map. A CI guard
  (`Verify 16 KB page compatibility`) asserts both conditions on every build. 32-bit
  `armeabi-v7a` is exempt (16 KB pages are a 64-bit-only feature).
- **Full-resolution export** — exports were silently capped at the 2048 px interactive-preview
  edge, so e.g. a 12 MP photo exported at ~3 MP. The final export render now uses the full
  source resolution (`EXPORT_MAX_EDGE_PX`); the 2048 px cap stays only for the live preview /
  magnifier. (On-device verification: issue #5 report.)

### Performance (M6)
- **Multithreaded full-res render** — the engine's per-pixel hot loops (`expose`
  spectral upsampling, `scan` density→RGB, `print_expose`) now run across all CPU
  cores via a deterministic fork-join helper (`kernels/parallel`). The image range
  is split into contiguous, disjoint pixel chunks whose boundaries depend only on
  (pixel-count, worker-count), so the output is **byte-identical regardless of
  thread count** — the bit-exact parity gate is preserved. Measured ~3.2× faster on
  a 12 MP scan on a 4-core host; larger gains on 6–8 core phones. Stochastic grain
  and the spatial blurs stay serial (grain walks a seeded RNG in pixel order). Worker
  count follows the core count, overridable via the `SPK_NUM_THREADS` env var; small
  previews fall back to serial below an 8192-pixel-per-worker floor.
- New `tests/test_parallel` gate asserts 1-thread vs 8-thread output is byte-identical
  for the scan route, the print route, and the grain+halation branch; the full
  `engine-parity` suite also runs multithreaded in CI.
- **Vector `exp10` SIMD** (`kernels/exp10.h`) replaces the `pow(10,−spectral)` bottleneck in
  the scan/print spectral integrals; lowers to NEON `fmla v.2d` on arm64, byte-identical at the
  float32 output (goldens unchanged).

### Testing & CI
- **First JVM unit tests** (`:app:testDebugUnitTest`) — `EditHistoryTest` covers the undo/redo
  store (push/undo/redo, redo-branch invalidation, cap eviction, clear, rotation), and
  `PresetsRoundTripTest` covers the non-destructive recipe layer (serialize → parse → decode
  preserves the editing state; missing keys keep defaults; re-serialization is idempotent). Both
  run on the plain JVM (no device) and are gated in the `android` CI job.
- **More parity gates** — `test_output_spaces` (all six output color spaces, not just sRGB) and
  `test_lensblur` (camera/scanner lens-blur spatial parity) are now in the `engine-parity` CI job.

## v0.4.0 — usability, performance & undo/redo ✨

Builds directly on the v0.3.0 engine/export foundation with an editor-usability overhaul, a
performance pass that keeps slider edits instant, in-session undo/redo, and the **Spektrafilm**
rebrand (display name only — package `com.spectrafilm.app` and the engine are unchanged).

### Rebrand
- **App display name is now "Spektrafilm"** across the UI, docs, Gradle, CI, and source headers.
  The application ID (`com.spectrafilm.app`), repository, and bit-exact engine are unchanged, so
  the rebrand carries no signing or compatibility impact.

### Editor usability
- **Open-photo button fixed** — the picker reliably opens from the editor.
- **Interactive crop overlay** — drag-to-adjust crop handles drawn over the live preview
  (replaces the previous numeric-only crop).
- **Histogram over preview** — the histogram now overlays the preview canvas for at-a-glance
  exposure/tonal reading while editing.
- **Reordered category bar** + **tooltips** on the category icons for discoverability.
- **Camera & scanner lens-blur controls un-gated** — both are now adjustable from the UI.
- **In-app "How to use" guide** (`HowToUseScreen`) surfaced from both About and the Welcome
  screen.

### Performance
- **Decoded-source proxy cache** (`DecodedSourceCache`) — the decoded RAW/photo proxy is cached
  so look-parameter edits (sliders, presets) re-render without re-decoding the source, keeping
  interaction responsive. Cache invalidates correctly on source change, white-balance, and
  rotation.
- **Opt-in half-size RAW decode** (`lib:libraw`) — a `halfSize` proxy-decode option that caps
  peak memory on large RAW/DNG. Default remains full-resolution; export is unaffected.

### Editing
- **In-session undo/redo with edit history** (`EditHistory.kt`) — top-bar Undo/Redo step through
  your edits, reusing the existing `Presets` JSON snapshots (rotation-aware). Edits are debounced
  so one drag is one undo step; the history clears on source change.

## v0.3.0 — Lightroom-style redesign, new engine stages, export upgrade 🎛️

Lightroom-style UI redesign, new engine stages, and a major export/import upgrade.

### Engine & pipeline
- **Auto-exposure stage (bit-exact, #6)** — all 7 metering patterns (center-weighted, spot,
  matrix, and 4 more) ported and parity-gated (`scan_portra_autoexp` golden). JNI now forwards
  `auto_exposure_method`, making every pattern selectable from the app.
- **Diffusion-filter stage (bit-exact, #6)** — spatial diffusion filters (halation/scatter
  coupling, DIR diffusion) ported and gated (`diffusion_bpm` golden).
- **Print path proven on all film/paper pairs** — native `print_digest` resolves neutral dichroic
  CC values + midgray exposure from `neutral_print_filters.json` (no longer baked for specific
  pairs). Proven end-to-end on a second pair via new `print_ektar` golden; both `print_portra`
  and `print_ektar` parity tests pass. Any profile combination is now valid.
- **Qualified DEFLATE DNG subset (`lib:libraw`)** — `USE_ZLIB` + NDK libz enable
  Compression 8 with `SampleFormat=3` floating-point data. Integer/linear Compression 8 and
  tag `0x80B2` return typed `DEFLATE_DNG` fallback; model-level Expert RAW support is not claimed.
  Adds structured `DecodeStatus` / `RawDecodeException` propagation and the DNG sniffer.

### New native modules
- **`lib:tiffwriter`** (`libsftiff`) — hand-rolled 16-bit baseline TIFF writer with ICC + EXIF;
  wired live into the export pipeline.
- **`lib:pngwriter`** (`libsfpng`) — 16-bit PNG writer with zlib/deflate + iCCP; built and
  host-tested. Not yet wired into the export UI (in progress).

### App features
- **16-bit TIFF export** — live option in the export sheet, backed by `lib:tiffwriter`.
- **Lightroom-style Auto-exposure control** — "Auto" button is opt-in (default OFF); expands
  to a metering-method popup with adaptive above/below anchoring and tap-outside dismiss.
- **Profile-curve browser** — dedicated screen to browse film/paper density curves.
- **Non-destructive recipe/sidecar layer** — edits are stored as a `SpektraParams` sidecar
  keyed to the source; original RAW is untouched; re-renders on open/export.
- **Engine/render status pill** — persistent readout showing decoding / rendering / exporting /
  error / last-render-ms on the preview canvas.
- **Source EXIF copy on export** — camera/lens/exposure/date EXIF from the source image is
  copied into exported JPEGs. **GPS/location is opt-in** (Settings → "Preserve location",
  default OFF/stripped) so shared images don't leak location by default.
- **Google Ultra HDR export** — exports a gain-map JPEG Ultra HDR when the device supports it.

### Major UI redesign (Lightroom-style)
- **Edge-to-edge full screen** — no Scaffold / ModalBottomSheet; root `Column` layout.
- **Pinned preview** (`weight(1f)`, near-black, fit) with a **90° rotate button** applied via
  the single `loadSource()` decode path (preview + export + magnifier all rotate together).
- **Horizontal scrollable bottom category bar** — custom hand-drawn `SpectraIcons` (12
  categories + gear / "?" / rotate), spring overscroll, sliding indicator,
  `navigationBarsPadding` for gesture-safe operation.
- **Inline `AnimatedVisibility` adjustment panel** between the preview and the category bar
  (replaces modal bottom sheets).
- **Back → previous screen**; **double-back-to-exit** with a one-time DataStore-persisted
  hint toast.
- Settings → gear icon, About → "?" icon.

### Quality & CI
- Crop, auto-exposure, diffusion (incl. full-pipeline + matrix-metering), lens-blur,
  `print_ektar`, and LUT-accel parity tests gated in the `engine-parity` CI job.
- `android-emulator` KVM smoke job added but gated to manual `workflow_dispatch` (hosted
  runners have no `/dev/kvm`; needs runner-level fix before it can run automatically).
- `tools/device_smoke_test.sh` — one-command on-device verification for the maintainer.

### Security hardening (from a pre-release review)
- Reject >2 GiB before `ByteBuffer.allocateDirect((jint))` in the RAW-decode and engine-output
  JNI paths (prevents `jint` truncation → heap overflow). Added direct-buffer capacity checks to
  the TIFF/PNG writer JNI and a 32-bit-ABI overflow guard to the PNG writer.
- GPS-on-export is now opt-in (see above).
- **Release note:** the `dist/` APK is **debug-signed** (fallback) — the maintainer must rebuild
  with a real release keystore before publishing. LibRaw decode paths should be fuzzed pre-release.

*Film modeling powered by [spektrafilm](https://github.com/andreavolpato/spektrafilm).*

---

## v0.2.0 — in development 🎛️

Turning the engine into a real, playable tool.

- **Full parameter surface wired through** — every `SpektraParams` field (camera, enlarger,
  scanner, grain, halation, DIR couplers, glare, IO, settings) now reaches the engine; defaults
  stay bit-exact (parity preserved), and edits measurably change the render.
- **RAW/DNG import** via LibRaw (`libsfraw.so`, all ABIs) → linear ProPhoto after an ACES intermediate, plus an sRGB photo
  picker (→ProPhoto) and the synthetic demo image.
- **Full GUI organized exactly like the spektrafilm desktop GUI** — Input · Import Raw ·
  Simulation · Grain · Preflash · Halation · Couplers · Glare · Experimental · Display —
  ImageToolbox-styled collapsible cards + sliders, with a debounced live preview.
- **Presets:** save / apply / delete and **import / export** as JSON, plus **20 built-in
  researched presets** (portrait, landscape, slide/chrome, cinema, low-light, nostalgic).
- **Film/print-stock catalog** — friendly names grouped by category (negative / slide /
  motion-picture / print film / paper) with ISO · balance · era · character.
- **Custom adaptive app icon** (35 mm film frame + spectral strip; Material You monochrome).
- **Export mask** — a full-screen overlay during the full-resolution render → gallery save.
- **Crop / resize geometry stage ported** (bit-exact) — the previously-inert `IOParams` crop
  (`crop`, `crop_center`, `crop_size`) and cubic `upscale_factor` now run up front in both the
  scan and print routes, matching the spektrafilm `_preprocess` step. Defaults are a strict
  no-op (parity preserved); a new `scan_portra_crop` golden gates the non-default path.
  (Downscale `upscale_factor < 1` AA is a documented follow-up.)
- **RAW white-balance UI** — Temperature/Tint sliders + reset-to-as-shot, shown only for
  RAW/DNG sources and wired to the existing LibRaw decoder so changing WB re-decodes the
  preview. Default (as-shot) decode unchanged.

## v0.1.0 — first release 🎞️

The complete **spektrafilm** spectral film-simulation engine, ported to native C++ and running
on Android. Dedicated to the [pixls.us](https://pixls.us) community.

### Engine (native C++ / NDK, bit-exact vs the original)
- Spectral upsampling (Hanatos2025), filming (expose → develop), **DIR couplers**
  (pointwise + spatial diffusion), printing (enlarger + dichroic Y/M/C filters, **all 28
  film/paper profiles** via a native neutral-filter + midgray digest), scanning
  (spectral → XYZ → RGB + unsharp + CCTF).
- **Halation**, in-emulsion scatter, and **film grain** (Poisson-binomial particle model with
  sublayers + micro-structure, statistically matched).
- **6 output color spaces:** sRGB, Adobe RGB, ProPhoto, Rec.2020, ACES2065-1, linear sRGB.
- `spk_simulate` (both routes) exposed through a C API + **JNI bridge**; `libspektra.so` for
  arm64-v8a / armeabi-v7a / x86_64.

### App
- Jetpack Compose UI: film/print profile pickers, scan-vs-print toggle, exposure, live render.
- 28 profiles + spectral LUTs bundled (~17 MB); assets extracted on first run.

### Quality
- Parity-first port: gated stage-by-stage against the live Python engine (golden vectors).
- CI builds `libspektra.so`, runs the engine parity tests, and assembles the APK on every push.

### Known next steps
- On-device RAW/DNG import (LibRaw module scaffolded).
- Non-destructive recipe/preset editing; richer editing UI.

**APK:** see the [GitHub Releases](../../releases) page (min Android 7.0). *(Historical `dist/`
APKs were removed from the repo — they were stale, 16 KB-page-misaligned and debug-signed.)*
