# Spektrafilm Android — Session Handoff

## Current state (2026-07-02, branch `claude/exciting-hamilton-hya62`)

- **"Exact + fast" pass MERGED (PR #109 + #110).** The PM directive (*"spektrafilm-exact result at
  ultra-fast speed"*) is fully landed: F1–F7 Kotlin robustness fixes, **E1** per-effect spatial
  decouple, **E2** print-route spatial + grain enable, **S1** scan-route film-density memo,
  **S2** print-density memo, **S3** Kotlin retained-result grade cache, **S4** deterministic loop
  parallelization. Every default engine path stays byte-identical; the two intentional look
  changes (RAW-input colorspace correction, print-route now carries film character) are onto the
  oracle. See the changelog entry below for commits + the perf table.
- **Oklch perceptual output-gamut compression MERGED (PR #111) — P2 #6 slice 1.** Opt-in /
  default-OFF, byte-identical off. C++ `OutputGamutCompress::kOklch=3` / facade `OKLCH` / UI
  "Oklch (perceptual, keep hue)": perceptual-hue-preserving chroma compression at constant
  Oklch(L, h) — Reinhard knee on `C / C_max`, `C_max` regenerated in-engine by a 64×720 bisection,
  float64 matrices from colour-science. Bit-exact to the oracle (gate `max_abs 1.077e-14`), gated
  by **`test_gamut_out_oklch`** (golden generated at oracle `27bd085`).
- **Oklrab perceptual output-gamut compression LANDED on this branch (P2 #6 slice 2) — NOT yet
  merged.** Opt-in / default-OFF, byte-identical off. C++ `OutputGamutCompress::kOklrab=4` / facade
  `OKLRAB` / UI "Oklrab (perceptual, even lightness)": the oklch chroma reduction with the per-pixel
  `C_max` lookup indexed by Ottosson 2023's rebased lightness `Lr = f(L)` instead of raw `L` (and the
  `C_max(Lr,h)` table built over an Lr grid, OkLab `L` recovered per row by the inverse remap before
  `Oklab→XYZ`); reconstruction still preserves `L`. Bit-exact to the oracle (gate `max_abs 1.055e-14`
  / probes `1.221e-15`), gated by **`test_gamut_out_oklrab`** (golden at oracle `27bd085`). Commits
  `9cb0a0b` golden / `94d2274` engine+ci / `e1b75d8` app on `claude/exciting-hamilton-hya62`.
- **Host parity suite = 35 gates**, all green (argv authoritative in `.github/workflows/ci.yml`);
  `SPK_NUM_THREADS` 1≡8 byte-identical (oklrab compress is serial+stateless); NDK r27 3-ABI build
  path unchanged. App **0.8.0 / versionCode 10**.
- **This branch now carries the unmerged oklrab commits (slice 2) on top of `origin/main` + the
  `1174fd8` docs commit.** Open a NEW draft PR for them; the remote branch auto-deletes on merge and
  recreates with a plain push. Never stack new work on already-merged history.
- **Export-speed: grain parallelized + MEASURED (2026-07-02, on-device SM-S948W).** A 12 MP export was
  **74.5 s, of which grain was ~67 s (90 %)**. Grain's 9 (ch×sublayer) RNG streams now run on a new
  `parallel_tasks` helper (`kernels/parallel.h`), each into a private plane, accumulated serially in
  canonical order — **byte-identical** to serial for any worker count (proven: `test_grain_parallel`
  1-vs-8 both paths, `test_parallel` grain+halation scan+print 1-vs-8, and a new-vs-old serial `cmp`
  = 0). **Measured: grain 67 s → 32.8 s (~2.0×), export 74.5 s → 40.0 s (~1.86×).** This is the
  `ceil(9/8)` **2-wave** ceiling — the earlier "~8.5 s / ~6×" projection was optimistic (9 unequal
  streams on 8 heterogeneous, thermally-limited cores). Items 2–5 (blurs/PNG/copy, ~7 s) still open.
  Full breakdown + the >2× counter-RNG lever in **`docs/EXPORT_PERF_2026-07-02.md`**; repeatable
  on-device bench at **`engine/spektra-core/src/main/cpp/tests/bench_export.cpp`** (recipe in header).
  See the "Export speed" section below.

## Next session (user directive 2026-07-02) — Lightroom-parity feature backlog

**Read `docs/NEXT_SESSION_LIGHTROOM.md` first.** 20 ranked improvements RE'd from the Lightroom docs
(`docs/RESEARCH_LIGHTROOM_*` + `docs/lightroom-re/*`), cross-checked against our tree, each parity-
classified (default-safe / opt-in / preview-only / parity-risk) with a cited LR source, our verified
gap, the exact gate, and a concrete first step (target file/function). Ranked by value ÷ effort.
Top of the list (all **default-safe** = `:app`/seam only, engine goldens untouched):
1. **"Neutral (Adobe-like)" preset** (S) — closes the measured LR-neutral render gap with existing params.
2. **Live histogram + clip indicators** (S) — promote the tone-curve histogram to a standalone panel.
3. **Debounce render during slider/crop gestures** (S) — one settle-render on release.
4. **8-band HSL / Color Mixer** (M) — post-engine, raised-cosine band weights (anti-banding), identity at 0.
5. **3-way Color Grading wheels + Split Toning** (M) — shares the HSL band kernel.
Then vignette, Whites/Blacks+parametric curves, Auto-Tone, embedded-JPEG first-paint, spectral-prefix
cache, Clarity/Texture, Dehaze, … down to XL/parity-risk (guided-filter Highlights/Shadows, tiled
export). Discipline: `default-safe` → work in `:app` or the pre/post-engine seam, never touch `cpp`;
`opt-in` → author a NEW oracle golden + `SPK_NUM_THREADS` 1≡8 assertion in the SAME session (like the
`gamut_out_*` gates); never merge an ungated engine change.

## Also queued — P2 #6 slice 3: `jzazbz` (then slice 4 `cam16ucs`)

Slice 2 `oklrab` is DONE (see the state block above). Clone the same pattern; the templates are now
`tools/parity/gen_gamut_oklrab_golden.py`, `model/gamut_compression.{h,cpp}` (oklch + oklrab
sections), and `tests/test_gamut_out_oklrab.cpp` + its ci.yml argv. **`jzazbz` is harder than
oklrab** — it is NOT a simple L-remap: it needs a JzAzBz forward/inverse (PQ encoding + matrices,
absolute-luminance scaled by `_JZAZBZ_Y_W_CDM2 = 100` cd/m²) and its OWN C_max table geometry
(`L_grid=linspace(0.002, 0.18, 64)`, `chroma_initial_upper=0.3`) plus a per-space Jz-white
normalizer for the lightness knee (`_jzazbz_white_Jz`). See oracle `compress_rgb_jzazbz_chroma` +
the `"jzazbz"` branch of `_get_output_c_max_table` in `utils/gamut_compression.py`.
- **Golden:** new `gen_gamut_oklrab_golden.py`-clone → `gen_gamut_jzazbz_golden.py`; call
  `gc.compress_rgb_jzazbz_chroma`; generate at oracle `27bd085` (already checked out) and pin the SHA.
- **C++:** port JzAzBz forward/inverse into `model/gamut_compression.cpp` (capture the colour-science
  matrices/constants as bit-exact hex, as oklch did for OkLab); enum slot `kJzazbz=5` is reserved.
- **Gate:** `test_gamut_out_jzazbz` + its `ci.yml` `build_run … tests/gamut_jzazbz_cases.bin` line
  (bumps the suite to 36). Add `gamut_out_jzazbz` to the enumerated lists in CLAUDE.md + the skills.
- **Facade/UI:** add `JZAZBZ` to `enum class OutputGamutCompress` (+ the exhaustive `when` in
  MainActivity — Kotlin will error if you forget) and the Output-gamut dropdown.
- Then **`cam16ucs`** (`kCam16ucs=6`, the heaviest — full CIECAM16 forward/inverse). Default upstream.

Per increment: default path byte-identical, opt-in/default-OFF, feature-on within tol
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`), `SPK_NUM_THREADS` 1≡8, NDK r27 3-ABI build, commit+push on green.
**Ship ONE algo per PR** — subagents died on token limits when given more, so keep each unit small.

## Export speed (bit-exact) — grain DONE 2026-07-02; items 2–5 open

Full detail: `docs/EXPORT_PERF_2026-07-02.md`. Bench: `tests/bench_export.cpp`. Order (all keep the
CI parity gate green + `SPK_NUM_THREADS` 1≡8):
1. **Grain — ✅ DONE (67 s → 32.8 s, ~2.0×, byte-identical).** The 9 `layer_particle_model` calls in
   `apply_grain_to_density_layers` + `apply_grain_to_density` now run on `parallel_tasks`
   (`kernels/parallel.h` — coarse atomic-scheduled task-parallel, no min-chunk gate), each into a
   private plane, accumulated serially in canonical (c, sl) order. Gated by `test_grain_parallel`
   (both paths) + `test_parallel`, both 1≡8. **~2× is the `ceil(9/8)` 2-wave ceiling** for the
   bit-exact approach; >2× needs per-pixel counter-RNG (different realization → stat-golden regen +
   product call). Export is now **~40 s**; grain (32.8 s) still dominates.
2. **Halation + DIR blurs (~5 s → ~0.7 s).** `parallel_for` the separable row/col passes in
   `kernels/gaussian.cpp` + `model/diffusion.cpp` — bit-exact (independent output elements). These
   ARE byte-exact-gated (`diffusion_e2e`, spatial gates) — verify they stay green.
3. **Double 140 MB memcpy → single owned buffer** (`spektra.cpp:1493` + `spektra_jni.cpp:641`).
4. **PNG16 zlib → libdeflate** (MIT, GPLv3-OK) or per-band parallel deflate (2.1 s → ~0.5 s; decoded
   pixels identical, file bytes differ). PNG-export only; TIFF-uncompressed is already ~free.
5. **Camera/enlarger diffusion** (8.5 s when ON, default-OFF): `parallel_for` rows+channels now, and
   restore FFT convolution (`apply_diffusion_filter_um`, `diffusion.cpp:437-450` is O(n·ks²) with an
   image-scaled radius — an FFT→direct regression vs the oracle). Shared by camera + enlarger.

Grain is only STATISTICALLY oracle-matched (never element-wise), so its determinism constraint is
thread-invariance, not oracle-byte-equality — the 9-way parallel keeps it byte-identical to today.

### Also open (unchanged)
- **Strategy-B rebaseline cluster** (`PRIORITY_ROADMAP` #20-27; incl. CAT02→CAT16 + xy-clip removal)
  — one coordinated baseline bump; trigger NOT fired (upstream WB-norm `e301791`/`526e200` still
  churning on `reflectance-upsampling-methods`, checked 2026-07-01). Keep the `c1d0e44` pin.
- **Device-gated queue** (user tests on his SM-S948W/Android 16): R8 0.8.0 release smoke; GPU-LUT
  re-arm feel; the E2 print-route look change (film character now in prints — intentional, eyeball
  it); AUDIT §A param-wiring UX decisions.
- **MALLETT2019** — disclosed as a GatedBlock; implement-vs-remove decision still open (`#18`).

---

## Evergreen operating notes (read once per session)

- **Container-reset recovery** (drilled 5+ times): the env re-clones to a stale commit mid-session.
  Recover via `git fetch origin main` (and the branch) → `git remote prune origin` → verify pushed
  work is on origin → `git reset --hard <ref>`. Untracked new files SURVIVE `reset --hard`; tracked
  edits do NOT. Rule: `git add && git commit -c commit.gpgsign=false && git push` the instant a unit
  builds green. `/tmp` and pip envs do not persist.
- **Proxy-desync recovery:** the local git proxy can return a stale snapshot and refuse
  `git fetch origin <branch>` by name — `git fetch origin <full-sha>` or `refs/pull/<N>/head` still
  works → `git reset --hard FETCH_HEAD`. Once a PR is merged the work is safe on real GitHub.
- **PR/branch lifecycle:** the remote branch auto-deletes on merge — recreate with a plain
  `git push` (`--force-with-lease` fails 'stale info'; `git fetch --prune` first). After a merge,
  restart from origin/main and open a NEW PR (never stack on merged history). The user may merge
  mid-session and webhooks don't deliver merges — re-check PR state before pushing. Merging is
  policy-gated (explicit user go-ahead); tag-push releases allowed when asked.
- **Oracle setup:** local clone at `/home/user/spektrafilm`; env = system python3.11 with
  `PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs` (stubs mock heavy IO deps). **e2e /
  param-wiring goldens pinned at `c1d0e44`** (upstream drift began at `a9bccd6` — never regenerate
  from tip); **gamut primitive goldens generated at `27bd085`**. Checkout the pin SHA before
  generating, restore the branch after; new gen scripts must pin the SHA they generate at.
- **Parity gate: 34 host tests**; per-test argv is authoritative in `.github/workflows/ci.yml`
  (copy, never guess) — any doc citing 15/26/31/33 gates is stale. Every engine change: default
  path byte-identical, feature-on within tol, `SPK_NUM_THREADS` 1≡8, NDK r27 3-ABI build green. All
  new engine features ship opt-in / default-OFF.
- **Land engine fixes ONE AT A TIME**, one small item per subagent — parallel agents collide on the
  shared engine files and the PR, and larger tasks blew the subagents' token limits mid-run.
- **`-fno-finite-math-only` is required** (scanning relies on NaN propagation through
  `density_to_light`); **GPU is preview-only, NEVER export** (vendor-varying float, float64 expose
  integrals, implementation-defined NaN handling).
- **Build distributable debug APKs with plain `./gradlew :app:assembleDebug`** — NEVER
  `-Pandroid.injected.build.abi` (stamps `android:testOnly`, blocks tap-install, moves output to
  `intermediates/`). **R8/minified release is NOT exercised by CI** — smoke-test on-device before
  tagging (last validated 2026-06-04 on SM-S948W/Android 16).
- **User directives on record:** do NOT modify `.github/workflows/` ('everything works there'); do
  NOT convert `.lut`→`.bin` (measured net-negative); **GPLv3 attribution "Film modeling powered by
  spektrafilm" must stay**; never put the model identifier in committed artifacts.
- **Toolchain** at `/opt/android-sdk` (NDK 27.0.12077973, CMake 3.22.1, build-tools 35.0.0) may not
  persist across containers — reinstall via `sdkmanager` if gone; `local.properties` is gitignored.
- **Kotlin/UI-only changes never touch the parity suite.** Post-engine grades and masks composite
  once, in-place on `res.data` via `simResultToBitmapGraded` right after simulate — never inside
  `simResultToBitmap` (the export site feeds `res` to both the bitmap and the 16-bit writers, so
  consumer-side mutation double-applies).
- **Engine param honesty:** presets/UI must set only engine-honored fields (halation via
  `halationAmount`/`scatterAmount`/`boostEv` — `halationStrength`/`halationFirstSigmaUm` are baked
  per-profile and ignored); params threaded only inside conditional blocks (e.g. `if(spatial)`) get
  silently dropped on the default path — thread unconditionally and fold into the relevant cache keys.
- **Perf medians are container-specific** — never compare benchmark numbers across boxes (the older
  2-core numbers are not comparable with the current 4-core ones).
- **CI flake:** the android job intermittently fails setup-android with 'Error on ZipFile unknown
  archive' (corrupt SDK download) — not a code failure; re-run the job.
- **Orphaned commit:** §6g ProfileValidator was committed as `660d33a` and pushed but never merged
  (slipped #102, force-dropped from #103) — re-land it if profile import is prioritized.
- **The user is Akshay Sharma**, the app's author (pixls.us megathread), testing on a Galaxy S26
  Ultra (SM-S948W, Android 16, arm64) — device-gated items queue until he tests. His laptop env
  (adb device testing): working copy `C:\Filmcam123\Spectrafilmandroid` (`C:\Spectrafilm` is
  docs-only — a trap); oracle = Python 3.13 venv `C:\Filmcam123\spkenv` + `spkstubs`; arm64 test
  binaries at `C:\Filmcam123\spk_arm64`; `JAVA_HOME` = Android Studio jbr JDK 21.
- `docs/PRIORITY_ROADMAP_2026-06-24.md` defines the P0–P3 item numbering (#1–#27) used throughout
  (P2 #6 = perceptual gamut algos, #18 = MALLETT2019, #20-27 = Strategy-B rebaseline cluster).

---

## Session history (compressed; full prose in this file's git history)

- **2026-07-02 — P2 #6 slice 2: `oklrab` output-gamut compression (new draft PR, unmerged).** Cloned
  the merged `oklch` pattern: `compress_rgb_oklrab_chroma` = the oklch chroma reduction with the
  `C_max` lookup indexed by Ottosson 2023's rebased lightness `Lr = f(L)` (constants k1=0.206,
  k2=0.03, k3=(1+k1)/(1+k2)); the `C_max(Lr,h)` bisection table is built over an Lr grid with each
  row's OkLab `L` recovered by the inverse remap before `Oklab→XYZ`, and reconstruction preserves the
  original (lightness-compressed) `L`. Reuses oklch's OkLab/RGB↔XYZ hex constants, `cmax_lookup`,
  `reinhard_knee`; table built locally per call (thread-invariant, warm==cold). Golden
  `gen_gamut_oklrab_golden.py` @ oracle `27bd085` (24 cases / 1152 px); gate `test_gamut_out_oklrab`
  `max_abs 1.055e-14` / probes `1.221e-15`. Suite 34→35, defaults byte-identical, oklch/aces/
  output_spaces/simulate_e2e/test_parallel unchanged. Facade `OKLRAB`=4 + Output-gamut dropdown
  ("Oklrab (perceptual, even lightness)"); `:app:compileDebugKotlin` green. Commits `9cb0a0b` /
  `94d2274` / `e1b75d8`.
- **2026-07-02 — "exact + fast" pass (PR #109 + #110).** F1–F7 Kotlin fixes; **E1** per-effect
  spatial gates (`test_spatial_decouple_e2e`, golden `scan_portra_lensblur_nohalation`); **E2**
  print-route filming spatial + grain (`test_print_spatial_e2e`, golden `print_portra_spatial`);
  **S1** scan-route film-density memo + per-param key-completeness tests; **S2** print-density memo
  (keyed on film_density_cmy CONTENT ⊕ printing inputs ⊕ the tc_lut-shaping film params); **S3**
  Kotlin retained-result grade cache (grade-only edits = zero native work); **S4** DIR-develop +
  exposure-interp + expose-tail loops → deterministic `parallel_for`. Both gamut e9e70f8 goldens
  ACCEPTED. Perf (4-core, 8 threads, 512²): cold scan **211 ms** (−13% from S4); warm scan / output-
  only edit 144–159; cold print ~400; warm print y-shift / output-only 153–162 (film + print memo →
  `scan()` alone).
- **2026-06-24 — P2 #5/#7 gamut + #8/#9 + P3 quick-wins (PR #109).** Output ACES-RGC v1.3
  (`test_gamut_out_aces`) + input radial-to-locus xy tc_lut bake (`test_gamut_in_xy`), both
  default-OFF, goldens @ `27bd085`; gamut flags → JNI → facade → two Simulation→Output dropdowns;
  `input_gamut_compress` folded into the tc_lut + film-memo keys only when active. Preset/diagnostics
  IO off-main; undo restoring-flag window fix; P3 quick-wins #10-16. SCOPE finding: CAT02→CAT16 +
  xy-clip removal are UNCONDITIONAL default-path changes (Strategy-B), NOT the opt-in locus bake.
- **2026-06-09 — WB wave + v0.8.0 release prep (PR #103).** Gray-point eyedropper, "Balance to film
  stock" (virtual-85, Bradford-adapt of the profile `reference_illuminant` CCT), auto-exposure
  default ON (matches upstream). versionCode 9→10, versionName 0.7.0→0.8.0. Scanner white/black
  correction gated to the strict-no-op case in UI.
- **2026-06-08 — masking + color/tone foundation (PRs #90–#103).** The keystone arc, all device-
  confirmed: §2 P0 color management (display tag + wide-gamut + ICC embed), §3.1 Contrast, §3.2
  Sat/Vibrance (Oklab post-engine grade), §3.3 couplers relabel, §2 P1 ACES gamut slider (post-
  engine v1), the full masking system (radial/linear/luminance/color-range masks → per-mask Tier-A
  Temp/Tint/Exp/Sat/Hue/Contrast/Whites/Blacks → draw-on-preview overlay → Class-S spatial ops,
  13 of LR's ~14 local ops), CLF/`.cube` LUT export (17/33/65), Lightroom-style export sheet
  (JPEG/UltraHDR/PNG16/TIFF16/TIFF32F/scene-linear), onboarding + slide-mode, the
  spectrafilm-solutions skill + `docs/USER_DRIVEN_SOLUTIONS.md`, and the full Lightroom RE
  (`docs/lightroom-re/`). Zero engine C++ changes in this arc. Design rule established: post-engine
  grades composite once in-place on `res.data`, mask ordinals pinned to crs:MaskBlendMode for XMP
  interop, gray-neutrality from Oklab/Rec-709 rows summing to 1.
- **2026-06-05 — editor + preview-speed wave (PRs #82–#88).** Point tone-curve editor (#88, faithful
  Fritsch–Carlson monotone-cubic Kotlin port); Lightroom UX + draft/final render worker + zoom ROI
  (#85/#86); highlight-boost ported (#82, `diffusion.cpp::apply_highlight_boost`, golden
  `scan_portra_boost` @ `c1d0e44`, `test_highlight_boost_e2e`); half→float LUT-load speedup (#83).
  GPU fit-preview promoted then reverted (broke the editor on SM-S948W). brutalist-re skill added.
- **2026-06-05 — param-wiring audit + print EV-comp (PRs #77–#80).** Downscale AA-prefilter parity
  fix (#77, real ~0.18–0.4 bug); print EV-compensation midgray fix (#80, `runtime/print_digest.cpp`,
  goldens `print_portra_evcomp{,_nonorm}`); R8 on-device validation recorded (#79). Opened the
  5-finding audit ledger (all since closed: boost→#82, MALLETT disclosed, spatial→E1, print→E2,
  dead sliders disclosed).
- **2026-06-04 — oracle pin + inert params + positive-film coupler (PRs #67–#76).** Oracle PINNED at
  **`c1d0e44`** (#67; drift = `a9bccd6`, changed filming raw-scaling). Wired all inert marshalled
  params: spectral blur (#68), hanatos window/surface (#69, surface = per-cell degree-4 2D poly),
  camera UV/IR (#72), enlarger preflash (#73, print-route only, NOT in the film-memo key), scanner
  white/black (#74, new `runtime/color_reference`). Positive-film DIR coupler fix (#75: per-stock
  provia/velvia gamma overrides; ~0.32 divergence on scan_film with couplers ON).
- **2026-06-03 — audit + lifecycle + zoom (PR #60).** Removed stale committed `dist/` APKs + closed
  the ICC license gap; process-scoped `EngineHolder` singleton (immutable engine never closed mid-
  life); profile+tc_lut memo keyed on immutable profile id (byte-exact); Lightroom ROI zoom. GPU
  standing verdict recorded: preview-only accelerator, never export.
- **2026-06-02 — v0.7.0 released.** `release.yml` published the signed APK + `.sha256`; apksigner
  verify passed. Workflow: feature branch → PR → merge (policy-gated); tag-push releases on request.
- **v0.7.0 session — engine completion (PR #59, Windows laptop + Galaxy S25 over adb).** AAssetManager
  direct-load (`SpektraEngine.fromAssets`, `#ifdef __ANDROID__`, skips the ~17 MB first-run extract)
  and `use_enlarger_lut` wired (opt-in PCHIP LUT mirroring the scanner LUT, `test_enlarger_lut_e2e`)
  — last reserved engine LUT flag gone. On-device parity runner: NDK clang `--target=aarch64-linux-
  android24`, push test + `libc++_shared.so` + assets + goldens, run under adb (`max_abs 5.96e-08`).
- **RAW export OOM (PR #56, device-confirmed v0.6.3).** Full-res RAW input + engine output moved off
  the ART managed heap via `malloc` + `NewDirectByteBuffer`; `LinearImage`/`SimResult` made
  `AutoCloseable`. Root cause: `ByteBuffer.allocateDirect` is a non-movable `byte[]` on the ~256 MB
  managed heap, not native memory — two ~140 MB full-res buffers cannot coexist there.
- **Since v0.4.0 (merged).** LR-RE feature wave (#35–#42 preset amount / copy-paste / resets /
  tone-curve stage), MotionCam `.mcraw` parser (#37/#38), perf scaffolding all opt-in/off (#46–#52:
  Vulkan compute + SPIR-V scan port, fp16 NEON, oneTBB, LiteRT stub), big-file RAW fixes
  (#43/#44/#56), Neutral (Adobe-like) preset (#55). GPU speedup remains UNPROVEN/hardware-blocked.

## Doc map (what to read for what)

`CLAUDE.md` build/parity/arch · `docs/AUDIT.md` open items + severity · `CHANGELOG.md` release notes ·
`docs/PRIORITY_ROADMAP_2026-06-24.md` the #1–#27 priority numbering ·
`docs/UPSTREAM_SYNC_2026-06-24.md` Strategy-A/B port plan · `docs/IMPROVEMENT_BACKLOG.md` LR-RE'd
feature list · `docs/PERF_ROADMAP.md` perf plan+policy · `docs/EXPORT_PERF_2026-07-02.md` measured
on-device export breakdown (grain=90%) + bit-exact speedup plan · `docs/USER_DRIVEN_SOLUTIONS.md` +
`.claude/skills/spectrafilm-solutions/` the user-need catalog · `docs/RESEARCH_*` / `docs/lightroom-re/`
RE studies · `docs/PRESETS.md` / `docs/FILM_STOCKS.md` content · `docs/maps/` source-project maps.
