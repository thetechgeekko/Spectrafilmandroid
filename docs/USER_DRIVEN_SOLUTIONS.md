# User-Driven Solutions — implementing what spektrafilm users actually want

> **Research-backed idea inventory, not current execution priority.** Use the evidence below when a
> Wayfinder ticket accepts a feature; use [EXECUTION_INDEX.md](EXECUTION_INDEX.md) for live work.

**Purpose.** A research-backed plan to solve *every* problem real spektrafilm users report, on the
Spectrafilmandroid app. The guiding rule (see the `spectrafilm-solutions` skill): **the app bends to
the user, not the reverse** — keep the film physics honest, make the experience do what people want.

**Source of truth.** ~1,150 posts across the spektrafilm forum category
(<https://discuss.pixls.us/c/software/spektrafilm/43>), mined via a swarm on 2026-06-06: the 897-post
megathread "Spectral film simulations from scratch", "spektrafilm tech discussions", "spektrafilm
LUTs", the data-sheets digitization campaign, troubleshooting, and "Editing — Saturation and
Contrast". Verbatim evidence is preserved per problem.

**How to read.** Every solution is classed by the parity-safe **tier** it lands in (skill §playbook):
Tier 0 UI/workflow · Tier 1 pre-engine (linear input) · Tier 2 post-engine (output RGB) · Tier 3
engine-gated (golden + default-no-op; last resort) · Tier 4 data/profile. Each entry: problem → root
cause → recommended solution → plug-in point → algorithm → effort (S/M/L) → parity impact → risks →
references.

**Status (2026-06-06): synthesis complete.** All six domains below are populated from a parallel
deep-research swarm (each agent grounded in the codebase + external best practice, constrained to
parity-safe solutions). Headline result: **of every user problem, only one solution (true B&W silver,
and a possible future engine cyan-crosstalk cure) needs an engine change + oracle golden — everything
else is parity-safe Tier 0/1/2/4.** File:line references were captured against the pre-#88 checkout, so
treat exact line numbers as approximate and re-verify at implementation time (function/file names are
stable).

**STATUS 2026-07-02: Waves 0-2 largely shipped** — WB wave (CreativeWhiteBalance/FilmStockBalance,
gray-point eyedropper, AE default ON), masking v1 (linear/radial + luminance/color range, 13 local
ops incl Class-S spatial, overlay + eyedroppers — see MASKING_SPEC.md), engine opt-in gamut
compression (output aces_rgc + input xy), stage memos (film-density both routes + print-density) +
retained-result grade cache, DNG-detect fix (SourceDetect.kt). Open: perceptual gamut algos (next),
brush/AI masks, AVIF, B&W/K-25.

---

## The user-problem catalog (ranked)

Maintained canonically in the `spectrafilm-solutions` skill. Summary:

1. **White balance** is users' hardest task; no creative/spot WB; RAW-only today. → §1
2. **No local/selective editing** (the keystone gap; reds-vs-skin; regional multi-stock). → §4
3. **"Too punchy"** contrast/saturation vs scanned film; opaque couplers. → §3
4. **Gamut artifacts** (cyan edge, skin, foliage); want ACES RGC. → §2
5. **Profiles** (B&W, slide/reversal, Ektachrome/K25, fantasy paper, tungsten). → §6
6. **Performance** (GPU, fast preview, big images). → §5
7. **Export/interop** (LUT in/out color-space + CLF, "linear DNG", AVIF/HEIC/JPEG-XL). → §6
8. **Opaque controls / onboarding** (couplers/grain/halation; print-gamma placement). → §6
9. **Robustness** (DNG-detect + lens-switch crashes; recipe deserialization on upgrade). → §6

**Already shipped (don't rebuild):** recipe/sidecar persistence, before/after, reset-to-defaults,
output-color-space selection, grain-scales-with-format, fast draft preview, tap-install, `.cube` LUT
export. (These are things desktop users still ask spektrafilm for — we're ahead on UX.)

---

## §1 — White balance & creative color  ✅ synthesized

**Keystone finding (verified):** the engine's input working space is **fixed linear ProPhoto** — the
JNI `inCs` argument is *discarded* (`spektra_jni.cpp`), and `filming` upsamples with a hardcoded
ProPhoto→XYZ matrix. So the input-colorspace dropdown is cosmetic, and — critically — **any linear-RGB
transform applied to the input buffer before `simulate()` is outside the parity surface** (the goldens
feed fixed buffers straight into C++; they never exercise a pre-multiply). That makes creative WB a
provably parity-safe **Tier 1** op.

**Solutions (all parity-free unless noted):** *(items 1–5 ✅ SHIPPED as the WB wave — shipped as
`CreativeWhiteBalance.kt` / `FilmStockBalance.kt` + the gray-point eyedropper, not the proposed
`WhiteBalance.kt`.)*
1. **Creative all-sources Temp/Tint** (Tier 1, **M**). New `WhiteBalance.kt` + `ImagePipeline.applyCreativeWb`
   on the linear input buffer **before** `simulate`, into a *fresh* buffer (never mutate the cached
   source). Math: Temp/Tint → source whitepoint (reuse the engine's already-shipped
   `whitepointXyzFromTemperature` — CIE-D ≥4000K / Kang2002 <4000K), tint = perpendicular xy offset,
   then a **Bradford CAT** source→**D50** (the engine's reference white) baked to a 3×3, applied per
   pixel. Works for **RAW *and* JPEG/HEIC**. Default = identity at the D50 anchor ("off" means off).
2. **Drop `temperature`/`tint` from `DecodedSourceCache.Key`** (Tier 0, **S**). Kills the re-decode
   thrash — today every WB nudge re-runs the whole LibRaw decode. With creative WB moved pre-engine,
   WB edits reuse the cached proxy and only re-run the fast engine pass.
3. **CAT choice:** Bradford default (ICC daylight standard, ideal on the daylight locus); CAT16 as an
   optional "extreme illuminant" toggle. *Not* Von-Kries (the decode path only uses it to match the
   RAW-import oracle; a new creative op isn't parity-bound, so use the better CAT). Tier 1, **S**.
4. **Gray-point / spot WB picker** (Tier 1, **M**). Tap → sample the **linear input** buffer (not the
   rendered output — WB is a property of the light, before the film), set source whitepoint = the
   patch's chromaticity, back-solve Temp/Tint (McCamy) for the slider readout. Reuses the existing
   tap→normalized-coord plumbing.
5. **Per-stock WB presets** in the recipe JSON (Tier 4, **S**) + **per-stock neutral calibration**
   (Tier 2 + data, **M**): offline-render a mid-gray through each (film,print) pair, store the output
   neutral gain in `neutral_calibration.json`, apply a post-engine diagonal so the Temp slider behaves
   *consistently across stocks* (fixes "filter strength varies by stock"); expose a 0–100% "keep stock
   character" amount.
6. **Brightness jump on film-toggle** (Tier 0/2): render the "off" state through an AE-matched display
   transform using the engine's metered EV (a Kotlin meter, or a read-only no-op AE-gain tap as a last
   resort) so on/off differ by *look*, not *level*.

**Caveats:** a scene-linear pre-WB is the *photographically correct* model ("balance the light hitting
the film") but is **not** numerically Lightroom-identical (Lr balances in a camera-profile space) —
document the Kelvin readout as approximate. Per-stock neutralization is first-order (mid-gray exact,
tails drift).
**Sequence:** 2 + 1 + 3 (kills re-decode + ships creative WB for all sources) → 4 (picker) + 5b
(presets) → 5a (per-stock consistency) → 6 (toggle polish). All share one `WhiteBalance.kt` + one
pre-engine call site.
**Refs:** darktable color-calibration (CAT16/Bradford, picker); RawPedia white-balance; color.js
chromatic-adaptation (Bradford matrices, D50 XYZ); odelama "RAW by hand" (gray-point reciprocal).

## §2 — Gamut & memory colors  ✅ synthesized

**Root cause splits in two.** (a) **An encoding bug, not a gamut bug:** the app is *not color-managed* —
output bitmaps never call `Bitmap.setColorSpace`, exports embed no ICC, and the only gamut handling is
a **per-channel hard clip** at the end of scanning. So wide-gamut output is shown *as sRGB* (wrong), and
the hard clip is exactly the "hard cyan edge." Wide-gamut "rolls off smoother" only because fewer pixels
hit the clip. (b) **An engine characteristic:** the spectral negative drives saturated highlights toward
cyan as dye channels saturate (upstream-confirmed) — a boundary compressor *masks* this but doesn't
*cure* it.

**Solutions (priority order):**
- **P0 — color-manage display + export** ✅ **SHIPPED** (Tier 0, parity none). `ColorManagement.kt`
  maps each engine output space → the matching Android color space (display) + bundled ICC (export).
  `simResultToBitmap` now tags the preview bitmap (`createBitmap` w/ color space, API 26+, sRGB
  fallback on 24–25 / ACES); `MainActivity` requests `COLOR_MODE_WIDE_COLOR_GAMUT`; 16-bit TIFF/PNG
  embed the matching ICC (`saucecontrol`/`elle-stone`, already bundled), and 8-bit JPEG/PNG/UltraHDR
  inherit it via `Bitmap.compress` on the tagged bitmap. The transfers were verified against
  `color_output.cpp::output_cctf_encode` per space. `ColorManagementTest` (5) covers the pure
  mappings; tests 72/72, lint clean. **The biggest cheap win** — every later gamut judgment is now on
  a correct display path. (Deferred: optional `RGBA_F16` preview surface for a faithful ACES preview.)
- **P1 — output-side ACES RGC pass** ✅ **SHIPPED (v1, post-clip)** (`GamutCompress.kt`, Tier 2, parity
  none). The ACES 1.3 RGC shaper (`THR=(0.815,0.803,0.880)`, `LIM=(1.147,1.264,1.312)`, `PWR=1.2`,
  `ach=max(R,G,B)`) as a "Gamut compression" amount slider (0=off → byte-identical) in Output, folded
  into `ColorGrade`'s shared CCTF pass. **Honest caveat:** it runs on the engine's already-clipped
  output buffer, so it *softens* the harsh cyan/edge fringe (pulls the most-saturated colors toward
  neutral) rather than fully curing it — the engine-side opt-in pass has since shipped (output aces_rgc
  pre-CCTF + input xy locus bake, gates `gamut_out_aces`/`gamut_in_xy`); the perceptual algos
  (cam16ucs/oklch/oklrab/jzazbz) are the next task, and the P3 channel-crosstalk cure remains an
  upstream-coordinated research item. `GamutCompressTest` (8). Remaining polish: expose THR/LIM adjustable + a
  target-gamut selector; move the pass pre-clip (needs the engine, P3). **This is the "users want ACES
  RGC built in" feature** (v1).
- **P2 — hue-preserving soft clip** (Oklab cusp projection, Ottosson α≈0.05) replacing the hard clip
  when compression is on (**M**, parity none when off). v1 can ship RGC + plain clamp (RGC already pulls
  values in); add Oklab projection later for the last mile.
- **P5 — heal negative / out-of-locus input** (Tier 1, **M**). ProPhoto/ACES-AP0 input carries
  negatives/out-of-locus values that poison the spectral upsampler → compounds the cyan. Mirror the
  desktop "RGC before the plugin" by healing at **import in Kotlin** (cleanest, zero engine risk).
- **Memory colors (skin/foliage/greens)** (Tier 2, **M**, opt-in). These are *inside* the gamut — not a
  boundary problem — so RGC won't fix them. A subtle, opt-in **hue-local nudge in Oklab/OKLCh**
  (Gaussian hue-weight around skin/foliage anchors, preserve L) is the right tool, but **respect that
  much of the warmth is the intended film look** — prefer per-stock preset tuning over a global
  corrector.
- **Reds/blues oversaturation:** RGC (P1) helps; otherwise reduce `dir_amount` per stock via **presets**
  (Tier 4) — *not* an edit to `couplers.cpp`.
- **P3 — engine channel-crosstalk** to *cure* the cyan at the source is the **only parity-gated item**
  (Tier 3): default-no-op + a **new oracle golden**, coordinated with upstream (who prototyped it).
  Defer; do not hack `couplers.cpp`/`filming.cpp`.

**Honest bottom line:** ship **P0 + P1** first (resolves the bulk of the cyan-edge + delivers ACES RGC),
then P2 + P5; treat skin/foliage as opt-in memory-color presets and the true cyan cure (P3) as an
upstream-coordinated research branch. RGC's published constants are AP1-tuned, so ship them adjustable.
**Refs:** ACES RGC spec + reference DCTL; Ottosson sRGB gamut clipping (Oklab cusp); RawTherapee Gamut
Compression; darktable color-calibration / filmic v6 gamut mapping.

## §3 — Tone: contrast / saturation / "film-feel"  ✅ synthesized

**Root causes (verified in code).** (a) The engine's final per-pixel op is already a parity-safe hook:
`scanning.cpp:366` applies the **tone-curve master LUT** to display-encoded, clipped [0,1] RGB —
default-identity, gated by `tone_curve_active`, CI-green (`tonecurve` gate). On `main` this LUT is
*already driven by the point tone-curve editor* (PR #88), so a Contrast control just composes with it.
(b) `print_gamma` scales the **log-E slope** of the paper curve, broadcast to all 3 channels
(`printing.cpp:351`) → **hue-neutral contrast**; `film_gamma` scales the negative's three *differing*
CMY curves (`density_curves.cpp:30`) → **shifts color** (the user complaint). (c) Saturation is an
emergent property of the couplers' 3×3 inter-layer inhibition matrix (`couplers.cpp:69-85`) where
`amount`/`inhibition_samelayer`/`inhibition_interlayer` all multiply overlapping parts of the *same*
matrix → users can't disentangle them; it's the wrong UI for "I want less saturation."

**Solutions (all Tier 0/2 — zero engine change, parity untouched):**

1. **Contrast slider → tone-curve master S-curve** ✅ **SHIPPED** (`ContrastCurve.kt`, Tier 0). Maps
   −100…+100 to a power S-curve with matched pivot slope `g = 2^(contrast/100)`, pivoted at display
   18% ≈ 0.46 (mid-gray fixed); 7 points → the existing master tone curve (Fritsch–Carlson PCHIP).
   Hue-neutral (master = all channels). Negative = the **mute** direction users want. Composes *under*
   a user-drawn curve (`out = userCurve(contrast(in))`, sampled at the grid) so the two stack; `0`
   emits no points → strict no-op. Lives in the Tone Curve panel (auto-arms; "Reset all" clears it),
   round-trips in the recipe `"grade"` block. `ContrastCurveTest` (9); tests 81/81, lint clean. (Did
   **not** wire to `film_gamma` — that's the color trap.) Original plan below for context:
   Map −100…+100 to a pivoted
   sigmoid (central slope `1+s`, pivot near display 18% ≈ 0.46); generate 5–7 points → existing
   `toneCurveMaster` (Fritsch–Carlson PCHIP bakes a smooth monotone LUT). Hue-neutral (master = all
   channels). Negative = the **mute** direction users want. Compose *under* a user-drawn curve by
   concatenating LUTs in Kotlin before packing. slider=0 emits no points → strict no-op. Do **not**
   wire to `film_gamma` (color trap).
2. **Saturation + Vibrance → Oklab post-op on `SimResult.data`** ✅ **SHIPPED** (`ColorGrade.kt`, Tier
   2). Applied in place to the engine output buffer once, right after `simulate` (`simResultToBitmapGraded`),
   so preview + every export format stay WYSIWYG (the in-place mutation propagates to the 16-bit TIFF/PNG
   read of the same `SimResult`). decode output CCTF → Oklab (Ottosson) → scale chroma → re-encode;
   `cctfEncoded` mirrors `savingCctfEncoding`. Saturation uniform `(1+sat)`, Vibrance low-chroma-weighted
   `(1+vib·exp(-C/0.12))`. **Gray stays neutral for ALL output spaces** (Ottosson LMS rows sum to 1 →
   `(v,v,v)→C=0`), so no risky per-space color matrices — only each space's 1-D transfer. Sliders in the
   Simulation→Output sub-tab; round-trips in the recipe `"grade"` block; default 0,0 → byte-identical.
   `ColorGradeTest` (7); tests 88/88, lint clean. Honest: perceptually exact for the sRGB family
   (default), a faithful creative control for wide spaces. Original plan below:
   Per pixel: decode
   CCTF → linear → Oklab (Ottosson matrices) → scale chroma `C`, preserve `L,h`. Saturation: uniform
   `C·(1+sat)`. Vibrance: low-chroma-weighted `C·(1+vib·w(C))`, `w=exp(−C/C0)`, `C0≈0.12`. Single op
   on the float buffer that feeds **both** preview (`ImagePipeline.kt:92`) and TIFF export (`:671`).
   Default 0 → byte-identical. (Honest: a generic perceptual control, *not* film-accurate
   non-uniform desaturation — composes with, doesn't replace, the couplers' physics.)
3. **Couplers relabel/regroup** ✅ **SHIPPED** (Tier 0, MainActivity `CouplersSection`). Panel →
   "Film color character (couplers)" with a top-of-panel note explaining DIR couplers + a redirect:
   *"Looking for a plain saturation control? Use Saturation / Vibrance in Simulation → Output."*
   Plain labels: Amount→Effect strength, Inhibition samelayer/interlayer→Within-layer/Cross-color
   strength, Gamma R→GB…→Color mix R→G/B (tooltip notes it's a color-mixing matrix term), Diffusion→
   Color bleed radius/tail. Param bindings/ranges/defaults unchanged → zero behavior/parity change.
   **This completes §3 (tone/color).** (Optional remaining: §3.4 "Film-Feel" master + §3.5 "Soft scan"
   preset, both deferred.)
4. **"Film-Feel / Look Amount" master** (Tier 2, **S–M**). Blend the render toward an app-defined
   flat target = `Contrast(−)+Desat(out)` computed from the single output buffer (no 2nd render).
   `final = lerp(target, out, a)`, default a=100% = no-op. (Optional HQ variant: blend toward an
   actual flat-param render — 2× preview cost — defer.)
5. **"Soft scan" built-in preset** (Tier 4, S) tying 1–4 + `print_gamma≈0.85` together — reproduces
   the forum workaround ("pre-flatten the RAW elsewhere") in one tap.

**Plug-in points:** `ParamsState.kt` (+`contrast`→`toneCurveMaster`, `saturation`,`vibrance`,
`lookAmount`); `ImagePipeline.kt:92,671` (Oklab op); `MainActivity.kt` (move Contrast out of
Experimental, add Sat/Vibrance/Look, relabel Couplers); `BuiltInPresets.kt`/`Presets.kt` (preset +
field round-trip). Engine refs (read-only): `scanning.cpp:366`, `kernels/tonecurve.cpp`,
`couplers.cpp:69-85`, `density_curves.cpp:30`, `printing.cpp:351`.
**Caveats:** the curve LUT is display-referred/clipped (can't recover blown highlights — fine for v1,
that's where LR's point curve lives too); pick ONE headline contrast (tone curve) and keep
`print_gamma` as "advanced/physical" so they don't double-apply; Oklab boosts may clip to gamut
(acceptable, as LR does).
**Refs:** Oklab <https://bottosson.github.io/posts/oklab/>; darktable color-balance-rgb &
filmic (chroma vs vibrance, contrast=central slope); A. Pierre "saturation control for the 21st
century".

## §4 — Local editing / masking (+ AI selection)  ✅ synthesized

> **SUPERSEDED by docs/MASKING_SPEC.md (the authoritative spec)** — masking v1 SHIPPED
> (linear/radial + luminance/color range, 13 local ops incl Class-S spatial, draw-on-preview
> overlay, eyedroppers). Kept for the original problem framing; brush + AI Subject/Sky + XMP
> export remain open.

**Load-bearing fact:** every render path (preview-settle, live-draft, zoom-ROI, magnifier, **export**)
funnels the engine's float output through **one function — `simResultToBitmap` in `EngineHelpers.kt`**
(interleaved float32 RGB, pre-8-bit-clamp). That single seam is where Tier-A masking inserts → it's
automatically correct for preview *and* export, byte-identical, **engine untouched (Tier 2, parity
none)**. A global edit is just the degenerate case of a mask with alpha≡1.

**Data model** (new `app/.../masks/`, pure Kotlin, JVM-testable): `Mask = components[] folded by
Add/Subtract/Intersect + invert + opacity`. Components: **Linear, Radial, Brush** (shape generators)
and **LuminanceRange, ColorRange, AiSegment** (modulators that intersect the shape). Fold uses
darktable's proven identities (union `1−∏(1−aᵢ)`, intersect `∏aᵢ`, polarity `1−a`). Geometry stored
**normalized 0..1** → resolution-independent across 640px draft, zoom-ROI, and 12 MP export.
`LocalAdjustment = Mask + TierADelta` (exposure/temp/tint/sat/contrast) `| TierBOverride`. `MaskStack`
layered.

**Compositing** (`simResultToBitmapMasked`): per mask, rasterize alpha (cached in a `MaskRasterBundle`
keyed by mask-hash+size), decode the output CCTF→linear, apply the Tier-A op, re-encode, blend
`(1−a)·in + a·out`. Run it **before the 16-bit TIFF/PNG quantize** too. Empty stack → delegates to the
existing path (zero regression, zero cost when off). Cost ≈ one buffer pass per active mask — sub-10ms
at preview res vs the ~1s engine render.

**Mask math** (all implementable): linear = projected smoothstep ramp; radial = feathered ellipse;
brush = stamped soft discs (stored as **vector strokes**, re-rasterized per size); luminance range =
4-marker trapezoid on luma; color range = hue-weighted distance from eyedropper samples (union). The
**color/luminance range masks are the cleanest fix for the #1 pain** ("restrict the red/saturation
tweak to the reds, leave skin alone").

**AI Subject/Sky** = **MediaPipe `tasks-vision` ImageSegmenter** + two bundled `.tflite` (SelfieMulticlass
256² for subject; DeepLab-ADE20k, sky=class 2, for sky), `outputConfidenceMasks` → soft alpha. Run once
on a downscaled bitmap, **GPU delegate → CPU fallback** (Android GPU delegate is flaky), guided-filter
edge-refine to snap to image edges, cache by source key, allow brush refinement. **Net-new ML infra**
(the app bundles no ML today; ~3 MB of models).

**Persistence:** one optional `"masks"` block in the **existing** Presets/Recipes JSON → undo/redo +
per-image recipes + import/export **for free** (vectors only; AI stores the recipe-to-regenerate, not
the segmentation). Old recipes → `optJSONObject("masks")==null` → today's exact global behavior.

**Phases (all parity none):** **P1** container + linear/radial + overlay-viz + Tier-A (the keystone, L)
→ **P2** brush (L) → **P3** color + luminance range (M — pull ahead if brush UI is heavy; it's the
highest value/effort fix) → **P4** combine/groups/invert (M) → **P5** AI Subject/Sky + Tier-B
"Frankenstein" multi-stock (L; the only new dep + the only multi-render cost). Reuse `CropOverlay`'s
gesture/normalized-coord patterns wholesale.
**Refs:** Adobe LR masking; darktable blending/parametric masks; MediaPipe Image Segmenter; ADE20k
labels; Snapseed on-device segmentation.

## §5 — Performance / GPU / preview  ✅ synthesized

> **Superseded policy snapshot.** The preview-only/CPU-export rule, 38-case count, implementation
> ordering, and “stays CPU by design” statement below describe the 2026-08-27 proposal. Current
> policy has 39 cases at both flag legs and separates Strict Exact CPU from device-qualified Fast
> GPU preview/export. Use [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md); retain
> this section only as user-research history.

**Hotspot:** the per-pixel 81-band expose integrals in `filming`+`printing` (and `scan`). **Policy:
approximate proxy / exact export** — GPU/approximation is preview-only; export always runs the exact
CPU path, so the engine-parity gate (38 tests as of 2026-08-27) is never touched. Levers ranked by speed-per-effort:

| Lever | What | Tier/where | Speedup | Effort | Parity |
|---|---|---|---|---|---|
| **E** Pause render on gesture | Suppress preview render during pan/pinch; resume on settle (the `collectLatest` flow already cancels) | Kotlin `Viewer`/`ImagePipeline` | instant feel | **S** | exact |
| **B** 3D-LUT preview loupe | Bake the pointwise pipeline to a 33³ `.cube`, GLES samples it per frame (<1ms); bake on settle, show last LUT meanwhile (scaffolding exists) | Kotlin `LutGpuPreview`/`Viewer` | ~0 ms/drag | **S–M** | preview-only |
| **D** Stage caches ✅ SHIPPED | S1 film-density memo (both routes) + S2 print-density memo + S3 Kotlin grade cache — measured warm print edits 153–162 ms vs 402 ms cold at 512² | C++ `spektra.cpp` per-instance + Kotlin | 2–8× common edits | **M** | **exact** (caches exact values) |
| **C** fp16/float32 preview buffers | Preview-only flag; float32 accumulators + fp16 inter-stage buffers (kernels exist, CI-gated) | C++ `filming/printing/scanning`, default off | ~1.5–2× | **M** | none (off by default) |
| **infra** Persistent Vulkan ctx | `vulkan_compute.cpp` currently rebuilds device+pipeline **per call** (kills any GPU win); add a persistent `VkContext` + on-disk `VkPipelineCache` | C++ `gpu/` | prerequisite for A | S–M | n/a |
| **A** Fused GPU compute kernel | Port the fused filming→print→scan per-pixel integral to a Vulkan compute shader (the scan shader exists; filming+print shaders missing) | C++ `gpu/`, `AppSettings` flag | **15–50×** preview | **L** | preview-only |
| **F** Progressive pyramid | Render 160px coarse first (~50ms), then 640px; "rubber-band" feel | Kotlin `ImagePipeline` | perceived-instant | **L** | exact |
| **G** Native tiling export | Strip-tile the per-pixel path for >12MP RAW (spatial path needs a halo; keep full-frame) | C++ `spektra.cpp` export | enables huge RAW | **L** | exact (per-pixel) |

**Recommended order:** **E, B, D, C** first (all safe, no device needed; D+C are exact/parity-free
CPU wins) → **infra fix + A** (the architectural win; needs a physical Adreno to validate fp32 `exp10`
precision + dispatch latency) → **F, G**. Honest note: 12 MP **export** stays CPU (~10–20 s) by design —
present it as a backgrounded progress op, never a live preview. vkdt's ~27 ms full-res GPU is the proof
the fused-compute path (A) is the real ceiling.
**Refs:** vkdt GLSL film-sim modules; Qualcomm Adreno Vulkan compute / `shaderFloat16`; existing
`docs/PERF_ROADMAP.md`.

## §6 — Workflow: export / profiles / onboarding / robustness  ✅ synthesized

Almost all of this is Tier 0/2/4 (no engine change). Two **immediate bug-fixes** surfaced:

- ✅ FIXED (SourceDetect.kt content-based detection) **🐞 DNG detection bug** — `MainActivity.kt:571` reads `RawDecoder.isRawFileName(name) || true`: the
  `|| true` force-treats **every picked file as RAW** (a workaround for MIUI document URIs with no
  extension). Fix: detect via `ContentResolver.getType()` (DNG = `image/x-adobe-dng`) **with** the
  filename fallback, drop `|| true`, and filter the `OpenDocument` MIME list. Tier 0, **S**, parity none.
  (Also explains the Xiaomi "lens-switch crash" — it's the system picker/camera crashing and returning a
  null URI; wrap the launch + handle null gracefully.)
- **🐞 Recipe schema migration** — `PRESET_VERSION` is written but never read on decode. Add a
  version-gated migration dispatch + the convention that new fields supply their *old-behavior* default
  explicitly (not the fresh-state default) + a v1-corpus round-trip test. Tier 0, **S**.

| Topic | Solution | Tier | Effort | Notes |
|---|---|---|---|---|
| **(a) LUT export** | Export dialog with **input/output color-space** pickers (engine already threads `io.input/outputColorSpace` through `bakeCubeLut` — UI-only) + resolution 17/33/65 + interop help text | 0 | S | + optional **CLF** (XML-wrap the cube; Resolve/OCIO 2.3+) = M |
| **(b) "Linear DNG"** | **Honest: impossible** — engine output is *display-referred*, a linear DNG is *scene-referred* → double-tone-map. Ship instead: **scene-linear TIFF/EXR of the input** (B1, S), **baked LUT + original RAW sidecar** (B2, S messaging), 32-bit-float output TIFF (B3, S) | 0/2 | S | label clearly |
| **(c) Modern formats** | **AVIF** via `avif-coder` (Apache-2.0, our 3 ABIs) as a new `:lib:avifwriter` (M); **JPEG-XL** via `jxl-coder` (M, +4MB, experimental); **HEIC-10** defer (L, MediaCodec) | 4/native | M | must pass `zipalign -P 16` / `0x4000` LOAD |
| **(d) B&W film** | **Near-term:** color-pipeline profile w/ grey-balanced panchromatic sensitivities + desaturating preset (M, parity-safe, data-only). **True silver:** `channel_model:"bw"` engine path (L, **Tier 3, needs oracle golden**) | 4 / 3 | M / L | start near-term |
| **(e) Slide/reversal** | The `scanFilm` toggle **already works** (4 slide presets ship). Just **auto-suggest** it for `type:"positive"` profiles + relabel **"Slide mode (skip print)"** | 0 | S | UX only |
| **(f) Profiles** | Kodachrome 64 / Ektachrome E100 / Vision3 200T-500T **already shipped**. Add **fantasy-paper** profiles (S, data) + tungsten **presets** (S); **K-25** is data-sourcing (L) | 4 | S–L | check upstream before K-25 |
| **(g) Datasheet pipeline** | Contribution guide (`docs/COMMUNITY_PROFILES.md`, S) + **profile-pack versioning/download** decoupled from APK (M) + **import-profile-from-URI** with JSON validation (S) | 0/4 | S–M | open GPLv3 data |
| **(h) Onboarding** | Plain-language **labels + tooltips** (`ParamHelp.kt` map), **Basic/Advanced** toggle per Grain/Halation/Couplers section, per-section reset, **"?" help sheets**, **"apply film defaults"** snackbar on profile switch | 0 | S (+M help art) | engine gets identical params; relabel only, never change behavior |

**Sequence:** the two 🐞 fixes immediately; then (e)/(h)/(a)/(b-B1)/(f-presets)/(g-import) as a fast
"v0.8 polish" wave (all S, parity none); then (c-AVIF)/(d-near-term)/(g-versioning) as v0.9; true-B&W
and K-25 later. **Caveat:** any new `.so` (avif/jxl) is the single biggest integration risk — verify 16
KB alignment before merge. Keep the "Film modeling powered by spektrafilm" attribution on new dialogs.
**Refs:** ACES CLF spec; DxO linear-DNG explainer; avif-coder / jxl-coder; Android HEIF; 35mmc B&W
spectral-sensitivity.

---

## Cross-cutting roadmap (finalized)

The standalone-mobile thesis holds: the highest-leverage cluster is **color + local editing**, because
desktop users offload those to their host app (darktable/Resolve) and we have none. A near-zero-cost
foundation also exists in the **two bug-fixes** and **color management**, which everything else benefits
from.

**Wave 0 — fixes & foundation (S, parity none, do first).** The DNG-detection `|| true` bug + recipe
schema migration (§6); **color-manage display+export** (§2 P0 — without it every color judgment is on a
broken display path); drop temp/tint from the decode cache key (§1).

**Wave 1 — color foundation (parity-safe Tier 1/2, solves the top voiced pains).** Creative all-sources
**Temp/Tint** + gray-point picker (§1); **Contrast** (tone-curve S-curve) + **Saturation/Vibrance**
(Oklab) + couplers relabel (§3); optional **ACES-RGC** gamut toggle (§2 P1). These controls are designed
to double as the **per-mask Tier-A payload**, so they pay off twice.

**Wave 2 — masking keystone (§4).** P1 container + linear/radial + Tier-A (reusing Wave-1 controls) →
P3 **color/luminance range masks** (the literal fix for "tame the reds, not the skin") → brush →
combine → AI Subject/Sky + Tier-B "Frankenstein". The architectural keystone; everything local hangs off
P1.

**Wave 3 — approachability & breadth.** Onboarding/progressive disclosure + slide-mode UX + LUT-export
spaces + linear-source export + fantasy-paper/tungsten presets + profile import (§6, all S); B&W
near-term profile; AVIF export.

**Wave 4 — performance & scale (§5).** Pause-on-gesture + LUT loupe + stage caches + fp16 (no device
needed) → persistent-Vulkan + fused GPU compute (needs an Adreno) → pyramid + tiling.

**Engine-gated (defer, needs oracle goldens):** true-B&W silver path (§6 d) and the cyan-crosstalk
cure (§2 P3) — coordinate with upstream; everything else above is parity-safe.

## Changelog
- 2026-06-08 — §6b **High-bit-depth TIFF exports SHIPPED (PR #102)** — a true **32-bit IEEE-float TIFF**
  writer (`:lib:tiffwriter`, host-tested) feeds two new export formats: **`TIFF32F`** (B3 — the engine's
  float output written verbatim, no quantise/clamp) and **`SCENE_LINEAR_TIFF`** (B1 — the decoded
  scene-linear input before the engine, untagged 32f float; the honest "linear DNG to finish elsewhere"
  answer). Not the parity engine. Deferred: EXR (needs a new encoder); the LUT input-CS picker stays
  engine-gated.
- 2026-06-08 — §6a/§6b **Export sheet (Lightroom-style) SHIPPED (PR #102)** — tapping Export now opens a
  format-aware `ExportSheet` (RE'd from lrmobile: format → format-specific options → dimensions → colour
  → naming → metadata) instead of using the global Settings defaults. Format (+JPEG/UltraHDR quality),
  **Size** (Full/4096/2048/1024/custom long edge — a post-render downscale, never enlarging; 16-bit pins
  to full-res), Color space + CCTF, optional file name, include-GPS. Pure core (`ExportOptions.kt`) is
  JVM-tested (`ExportOptionsTest`). Tier 0/2, parity untouched. **Deferred:** LUT input-CS (engine-gated),
  AVIF (§6c, new .so), output sharpening/watermark (out of scope); **next:** 32-bit-float / scene-linear
  TIFF (§6b, `:lib:tiffwriter`).
- 2026-06-08 — §6e **Slide-mode UX SHIPPED (PR #102)** — picking a colour-reversal (slide) film offers a
  "Slide mode" snackbar that views it as a positive (flips `scanFilm`); relabel `Scan film` →
  `Slide mode (skip print)`. Reversal detection is a pure predicate (`StockEntry.isReversal()`),
  grounded against `catalog.json` (`StockCatalogTest`). Tier 0, parity untouched.
- 2026-06-08 — §6a **finding:** input/output LUT colour-space pickers are only *half* UI-only — OUTPUT
  flows through `params.io.outputColorSpace` and the size + `.cube/.clf` picker shipped in #99, but the
  LUT **INPUT domain is hardcoded to linear ProPhoto in native** (`spektra.cpp` `kProPhotoRGB`), so an
  input-CS picker is **engine-gated (Tier 3)**. Clean UI-only remainder: surface output CS in the export
  dialog + interop help text.
- 2026-06-08 — §6h **Onboarding SHIPPED (PR #101, three slices)** — Tier 0, relabel/UI-state only, the
  engine receives identical params so parity is untouched (`:app:testDebugUnitTest` 160/160):
  1. **Plain-language help sheets** (`ParamHelp.kt` + `HelpSheet`/`SectionCard` in `Widgets.kt`): a "?"
     badge on each opaque section (grain/halation/couplers/film+print gamma/preflash/glare) opens a
     bottom sheet explaining the control in photographer's terms. `ParamHelp` is pure data,
     JVM-unit-tested (`ParamHelpTest`).
  2. **Basic/Advanced disclosure** (`AdvancedToggle`): Grain/Halation/Couplers show a short Basic set
     by default; "Show advanced options" reveals the full physical control set. Hidden controls keep
     their state.
  3. **"Use its defaults" snackbar** on profile switch: resets the per-stock character
     (grain/halation/couplers/gamma) to neutral via `ParamsState.resetStockCharacter()` while keeping
     creative/global edits; JVM-tested (`ParamsStateResetTest`). **Optional leftovers:** extend
     help/Basic-Advanced to Simulation/Input/Display; persist the Basic/Advanced preference.
- 2026-06-08 — §2 P1 **ACES gamut compression SHIPPED (v1, post-clip softener)** (`GamutCompress.kt`):
  an amount slider that pulls the most-saturated colors toward neutral, softening the cyan/edge fringe;
  default 0 = byte-identical. The pre-clip cure (P3) stays deferred (engine-gated).
- 2026-06-08 — §3.3 **Couplers relabel SHIPPED** — plain labels + redirect to Saturation/Vibrance.
  **§3 (tone/color) complete:** Contrast + Saturation/Vibrance + couplers relabel all shipped.
- 2026-06-08 — §3.2 **Saturation/Vibrance SHIPPED** (`ColorGrade.kt`): post-engine Oklab chroma grade
  on the output buffer, gray-neutral for all output spaces. §3 now: only the couplers relabel (§3.3) +
  "Film-Feel" master (§3.4) remain.
- 2026-06-08 — §3.1 **Contrast SHIPPED** (`ContrastCurve.kt`): a discoverable, hue-neutral Contrast
  slider driving the parity-gated master tone curve, composing under hand-drawn curves. Next in §3:
  Saturation/Vibrance (Oklab post-op) + couplers relabel.
- 2026-06-08 — §2 **P0 color management SHIPPED** (`ColorManagement.kt`): per-output-space display
  tagging + wide-color window mode + ICC embed on TIFF/PNG/JPEG. Wave-0 foundation done; the broken
  display path is fixed, so the remaining color work (Sat/Vibrance/Contrast, ACES-RGC) is now judged
  correctly.
- 2026-06-06 — Scaffold + `spectrafilm-solutions` skill; 6-front deep-research swarm; all six domains
  synthesized; roadmap finalized.
