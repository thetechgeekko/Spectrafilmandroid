# NEXT_SESSION_LIGHTROOM.md — Lightroom-parity feature backlog

> Generated 2026-07-02 from four Lightroom reverse-engineering readers
> (`docs/RESEARCH_LIGHTROOM_IMPLEMENTATION.md`, `docs/RESEARCH_LIGHTROOM_STACK.md`,
> `docs/RESEARCH_LIGHTROOM_RENDER.md`, `docs/lightroom-re/{icb-by-feature.md,icb-signatures.txt,cr-symbols-curated.txt,tiparamsholder-natives.txt}`)
> cross-checked against the current app + engine inventory. Every claim about what LR does is
> cited to a specific RE doc/symbol; every "our gap" was verified against the tree.

## The prime directive (read this first)

**Bit-exact parity with the upstream spektrafilm oracle on the DEFAULT / EXPORT path is non-negotiable.**
Any change under `engine/spektra-core/src/main/cpp/**` must keep the 35-gate host C++ engine-parity
suite green. "Bit-exact" = (1) within oracle tolerance (`max_abs ≤ 1e-4` AND `rms ≤ 1e-5`, goldens
pinned to `c1d0e44` / `27bd085`) AND (2) byte-identical across thread counts (`SPK_NUM_THREADS` 1 vs 8).
`-O3 -ffast-math -fno-finite-math-only` are load-bearing (scanning relies on NaN propagation — do NOT
strip `-fno-finite-math-only`).

**Every item below is classified into the parity taxonomy from `spectrafilm-solutions`:**

- **default-safe** — Tier 0 (UI/workflow), Tier 1 (pre-engine transform on the linear input buffer,
  outside the parity surface), Tier 2 (post-engine op on the output RGB buffer at the single
  `simResultToBitmap` seam), or Tier 4 (data/profile JSON). **No `cpp` change, so the 35 goldens are
  untouched by construction.** Identity-at-neutral is the correctness bar.
- **opt-in** — Tier 3, touches `cpp`. Must be DEFAULT-OFF / DEFAULT-NO-OP so every pre-existing golden
  reproduces bit-exactly, AND must ship its OWN new oracle golden + a thread-invariance assertion
  before it can be enabled (exactly like the existing `gamut_out_*` gates).
- **preview-only** — runs only in the downscaled preview path (`simulate_preview`); the full-res
  `scan`/export path is untouched, so parity is unaffected.
- **parity-risk** — touches the default export path. Ships only with a mechanical proof of
  non-perturbation (halo + a new tile/thread-invariance golden). Highest bar; last resort.

## How a fresh agent should use this doc

1. Pick the highest-ranked item you have budget for. Ranking is **user value ÷ effort**, best first.
2. Read the per-item section: the **CITATION** anchors what LR actually does; **Our gap** is verified
   against the current tree; **Gate** tells you the exact parity discipline; **First step** names the
   concrete target file/function to open.
3. If it is `opt-in` (Tier 3), you MUST author a new oracle golden + thread-invariance assertion in the
   SAME session — do not merge an ungated engine change. If it is `default-safe`, the engine is not
   your concern; work in `:app` (Kotlin) or the pre/post-engine seam only.
4. Items we already ship are in **"Already have — do not rebuild"**; do not re-implement them.
5. Update this doc's status column as you land things.

---

## Ranked summary

| # | Title | Value | Effort | Parity class |
|---|-------|-------|--------|--------------|
| 1 | "Neutral (Adobe-like)" built-in preset | High | S | default-safe |
| 2 | Live histogram + shadow/highlight clip indicators | High | S | default-safe |
| 3 | Debounce/pause render during slider & crop gestures | High | S | default-safe |
| 4 | 8-band HSL / Color Mixer (partition-of-unity) | High | M | default-safe |
| 5 | 3-way Color Grading wheels + Split Toning | High | M | default-safe |
| 6 | Creative post-crop vignette | Med-High | S | default-safe |
| 7 | Whites/Blacks + parametric tone regions + per-RGB curves | Med-High | M | default-safe |
| 8 | Auto-Tone (histogram-driven baseline) | Med | M | default-safe |
| 9 | Embedded-JPEG instant first-paint (RAW) | High | M | preview-only |
| 10 | Spectral-upsampling prefix cache (warm-tune speedup) | High | M | default-safe |
| 11 | Clarity / Texture (local-contrast) | High | L | default-safe |
| 12 | Dehaze (dark-channel prior) | Med | L | default-safe |
| 13 | Progressive coarse→fine pyramid preview | High | L | preview-only |
| 14 | Capture-sharpen Detail + edge Masking | Med | M | opt-in |
| 15 | Input noise reduction (pre-engine only) | Med | L | default-safe |
| 16 | Edge-aware Highlights/Shadows (guided filter) | High | XL | default-safe |
| 17 | Lens-profile distortion / vignette / CA correction | Med | L | default-safe |
| 18 | Copy/paste-by-section + named snapshots + crs schema mirror | Med | M | default-safe |
| 19 | AVIF / JPEG-XL export writers | Med | M | default-safe |
| 20 | Tiled full-res export (halo + tile-invariance gate) | Med | XL | parity-risk |

---

## Per-item detail

### 1. "Neutral (Adobe-like)" built-in preset — default-safe, S

**What LR does + CITATION.** LR's default render applies the *Adobe Color / Adobe Standard* DCP camera
profile (baseline exposure + medium-contrast tone + HSV color rendering) at the camera→working-space
transform. The RE author measured our render vs LR's neutral default on the same test DNG and quantified
the gap: LR is neutral & slightly darker (meanL 0.229 vs our 0.269; warmth R−B 0.064 vs our 0.089), with
no bloom. They then wrote a concrete parameter recipe to close it.
(`RESEARCH_LIGHTROOM_RENDER.md` "Lightroom's default render pipeline" L9-20 + Result table L28-37 +
"Alignment" levers 1-4 L39-49 + "Recommended product move" L51-53.)

**Our gap.** We ship 28 film/print-pairing presets (`BuiltInPresets.kt`, `PRESETS.md`) but no
Lightroom-style neutral starting point. The DCP camera profile CANNOT go into the default filming stage
(it would break `simulate_e2e`/`filming`), but every lever the RE author cites already exists as a
`SpektraParams` field.

**Parity class + gate.** default-safe (Tier 0/4). Pure parameter bundle in Kotlin — zero `cpp`, the film
default stays the app identity. No golden impact by construction.

**Effort.** S — one preset object + tuning against the test DNG.

**First step.** Add a preset entry in `app/.../BuiltInPresets.kt` composed of existing knobs the RE names:
reduce `dirCouplers.amount` (cut the coupler warm bias driving R−B 0.089→0.064); `exposureCompensationEv
≈ −0.2` (or lower the AE target ≈0.04 L for meanL 0.269→~0.229); `glare.active = false` + lower
`halation.*Amount` (kill the bloom LR lacks); a mild contrast curve via the existing `ContrastCurve.kt`.
Validate visually against `raw_test.bin` (4080×3060) and record the deltas in `PRESETS.md`.

**Risks/unknowns.** The target numbers are from one DNG; the recipe is a look, not a colorimetric match
(we have no per-camera DCP — `RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §B L123 "Gap"). Ship as a named
preset, not a default, so it is opt-in for the user and never perturbs the film default.

---

### 2. Live histogram + shadow/highlight clip indicators — default-safe, S

**What LR does + CITATION.** `ICBCalculateHistogram(TIHistogramData, bool, bool)` computes RGB +
luminance histograms with SDR/HDR flags and shadow/highlight clip warnings for the UI.
(`icb-by-feature.md` ## Render_Preview L750; `icb-signatures.txt` L134. Also `cr_stage_ColorHistograms`
in `cr-symbols-curated.txt`.)

**Our gap.** A histogram is already computed and drawn *behind the tone-curve editor* (`ToneCurve.kt`,
`Viewer.kt` reference it), but there is no standalone always-on histogram panel and no clip-warning
overlay. This is the cheapest UX win in the set — the reduction primitive already exists.

**Parity class + gate.** default-safe (Tier 0). Read-only reduction over the final output `ByteBuffer`;
zero effect on exported pixels.

**Effort.** S.

**First step.** Extract the histogram reduction currently inside `ToneCurve.kt` into a reusable
`stats`-style pass over the `SimResult` buffer, expose it as a small Compose panel in `Viewer.kt`, and
add per-channel clip counts (pixels at 0 / at 1.0) as tappable shadow/highlight clip toggles. Optionally
back it natively with `kernels/stats` if the Kotlin reduction is too slow at 12 MP.

**Risks/unknowns.** HDR-range visualization needs the `REC2020`/PQ output-space wiring; ship SDR clip
flags first, defer the HDR flag until the PQ output path (item interplay with #19) lands.

---

### 3. Debounce/pause render during slider & crop gestures — default-safe, S

**What LR does + CITATION.** `ICBPauseRendering` / `ICBRefreshRendering` — LR suspends rendering
mid-interaction and fires one render on release, avoiding wasted mid-drag renders.
(`RESEARCH_LIGHTROOM_STACK.md` item 5 L47-48 "Small".)

**Our gap.** The editor re-runs the off-main-thread `simulate` on each slider tick; there is a draft/settle
path but no explicit gesture-scoped suspend. Cheapest perceived-smoothness win called out.

**Parity class + gate.** default-safe (Tier 0). Pure app-side scheduling in Kotlin; no engine change.

**Effort.** S.

**First step.** In `ImagePipeline.kt` / `Viewer.kt`, gate the preview `simulate` behind an
`isGestureActive` flag set by the slider/`CropOverlay` drag handlers; coalesce to one render on gesture
end (debounce ~120 ms). Keep the existing draft(low-res)→settle(preview-res) swap.

**Risks/unknowns.** Must not swallow the final settle render on release; add a trailing-edge guarantee.

---

### 4. 8-band HSL / Color Mixer (partition-of-unity) — default-safe, M

**What LR does + CITATION.** 8 hue bands (R/O/Y/G/Aqua/B/Purple/Magenta) × Hue/Sat/Lum using **smooth
overlapping bands (partition-of-unity weighting), not hard hue slices**; TAT samples a pixel's hue to
pick a band. Camera Raw derives internal luminosity/saturation/contrast masks so the weighting is soft
(avoids posterized band edges). (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §B "HSL / Color Mixer" L116-117;
`icb-by-feature.md` ## WhiteBalance_Color L217-248, `ICBFillColorMixValues`; `cr-symbols-curated.txt`
`cr_stage_HSLTuner` + `_luminosityMask`/`_saturationMask`/`_contrastMask`.)

**Our gap.** Verified absent — no HSL/color-mixer anywhere in `:app` (grep for hsl/colorMix/hue-band
returns nothing). `ColorGrade.kt` is Saturation/Vibrance only. This is one of the biggest tone/color
gaps vs LR.

**Parity class + gate.** default-safe (Tier 2). An 8-band HSL op on the engine OUTPUT RGB at the
`simResultToBitmap` seam: RGB→HSL, apply per-band partition-of-unity-weighted hue-rotate/sat-scale/
lum-scale, back to RGB. Default all-zero = strict identity → 35 goldens untouched. Working space aligns
(LR "Lightroom RGB" = linear ProPhoto D50 = our engine input, `RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §B
L119-120) so no colorspace adapter is needed.

**Effort.** M.

**First step.** New `app/.../ColorMixer.kt` (mirror `ColorGrade.kt`'s in-place `applyInPlace(ByteBuffer,
…)` structure and its CCTF round-trip), wired into the same post-op chain `ColorGrade.applyInPlace`
already sits in. Add 8×3 fields to `ParamsState.kt`. **The load-bearing detail is the partition-of-unity
band weight** — use raised-cosine overlap so adjacent-band weights sum to 1 at every hue (prevents
banding). Add a per-band Compose UI + TAT (reuse `PixelSample.kt`/`PixelSampleOverlay.kt` for the hue
pick).

**Risks/unknowns.** Adobe's exact band centers/widths are unpublished — treat as standard reconstruction,
not a bit-match target. Chroma axis is perceptually exact only for the sRGB family (same caveat
`ColorGrade.kt` documents for wide spaces).

---

### 5. 3-way Color Grading wheels + Split Toning — default-safe, M

**What LR does + CITATION.** 3 zones (Shadow/Mid/Highlight) + Global, each Hue/Sat/Lum, plus **Blending**
(region-overlap width) and **Balance** (split shift). Legacy Split Toning = the Shadow+Highlight wheels
at Blending=100. Camera Raw bakes the 3-wheel model into three 1-D per-channel (R/G/B) transfer curves
via a luminance-weighted blend. NOT lift/gamma/gain.
(`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §C "Color Grading" L140-142; `cr-symbols-curated.txt`
`cr_stage_SplitTone::BuildTable(...11 doubles)` / `BuildCurves→3 dng_1d_functions`,
`cr_color_grading_params`; `icb-by-feature.md` ## Presets_Profiles L545 `ICBCopyValidSplitToningParams`.)

**Our gap.** Verified absent. `ColorGrade.kt` is chroma only; no per-region tinting exists.

**Parity class + gate.** default-safe (Tier 2). Region-weighted HSL tint on output RGB with
luminance-based zone weights; Blending drives the partition-of-unity zone width (shares machinery with
#4), Balance shifts the shadow↔highlight split point. Neutral wheels = identity → parity-safe.

**Effort.** M (can share the band-weight kernel from #4 — sequence #4 then #5).

**First step.** Extend the post-op chain next to `ColorGrade.kt` with a `ColorGradeWheels.kt`: compute
per-pixel output luminance, derive Shadow/Mid/Highlight weights (Balance-shifted, Blending-widened
partition-of-unity), add each zone's hue/sat tint + lum offset. Add the 3+1 wheel params + Blending +
Balance to `ParamsState.kt`; Compose color-wheel UI (LR exposes `ICBGetColorWheelBuffer`).

**Risks/unknowns.** Deciding Tier-2 output-RGB tint vs the more physical "bias enlarger dichroic Y/M/C
per tonal region inside printing" (`icb-by-feature.md` note). The Tier-2 route is parity-free and
correct-enough for a creative grade; take it. The printing route would be Tier-3/opt-in and is not worth
the golden cost here.

---

### 6. Creative post-crop vignette — default-safe, S

**What LR does + CITATION.** Creative vignette AFTER crop: Amount, Midpoint, Roundness, Feather,
Highlight-priority, plus a **Style** flag (highlight-priority / color-priority / paint-overlay), computed
in post-crop radius space — distinct from lens vignetting.
(`icb-by-feature.md` ## Presets_Profiles L541 `ICBCopyValidPostCropVignettParams`, ## Geometry_Lens L387;
`cr-symbols-curated.txt` `cr_post_crop_vignette_function`, `cr_params::PostCropVignetteStyle`.)

**Our gap.** Absent. Cheap, popular control that fits our spatial infra directly.

**Parity class + gate.** default-safe (Tier 2). Radial darken/lighten on output RGB keyed on
crop-relative coordinates (we already have crop geometry in `CropOverlay.kt` / `crop_resize`). Amount 0 =
no-op.

**Effort.** S.

**First step.** Add `PostCropVignette.applyInPlace(...)` to the post-op chain, reading the crop rect from
`ParamsState`. Radial falloff = smoothstep over [Midpoint·(1−Feather), Midpoint]; Roundness warps the
radius metric. Implement the highlight-priority Style first (multiply-under-highlights).

**Risks/unknowns.** Roundness/Style math is unpublished; reconstruct. Keep it post-crop so rotation/crop
interplay is correct.

---

### 7. Whites/Blacks + parametric tone regions + per-RGB curves — default-safe, M

**What LR does + CITATION.** `ToneCurvePV2012` = ≤16-pt monotone-cubic point curve **+ per-RGB channels
+ parametric Shadows/Darks/Lights/Highlights region bumps with split points 25/50/75**. Whites/Blacks =
global endpoint/clip points (Whites does NOT engage the exposure shoulder); Contrast = midtone-pivot S.
Named Linear/Medium/Strong = point-curve presets. LR evaluates with the DNG clamped monotone-cubic spline
solver. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §C "Whites/Blacks"/"ToneCurvePV2012" L136-139;
`RESEARCH_LIGHTROOM_RENDER.md` alignment item 4 L47-48; `icb-by-feature.md` ## Export_Formats L836-839
`ICBDNGSplineEvaluate`.)

**Our gap.** We ship a point curve (`ToneCurve.kt`, master + per-RGB via `ParamsState.toneCurve*`,
Fritsch–Carlson monotone-cubic) and a `ContrastCurve.kt`. Missing: dedicated Whites/Blacks endpoint knobs
and the 4 parametric region sliders with 25/50/75 split points. This is Tier-0 — no new engine stage.

**Parity class + gate.** default-safe (Tier 0). Map Whites/Blacks/parametric-region bumps INTO the
already-wired PCHIP curve as control points. Identity curve = no-op; the region math MUST collapse to
identity when all four sliders are 0 so `simulate_e2e`/`tonecurve` stay green.

**Effort.** M.

**First step.** In `ToneCurve.kt` / a new `ParametricCurve.kt`, add Highlights/Lights/Darks/Shadows
sliders that emit smooth spline anchors at the 25/50/75 split points and fold them into the existing
master control-point list before the engine bakes the LUT. Add Whites/Blacks as endpoint control points.
Add Linear/Medium/Strong preset buttons.

**Risks/unknowns.** Optional: swap our Fritsch–Carlson eval for the DNG clamped monotone-cubic solver
(`icb-by-feature.md` L826) for exact LR/ACR preset compatibility — but that changes the wired
`kernels/tonecurve.cpp` eval, so it becomes opt-in (Tier 3) and needs its own golden. Defer; the region
UI is the value.

---

### 8. Auto-Tone (histogram-driven baseline) — default-safe, M

**What LR does + CITATION.** `ActivateAutoTone` computes Exposure/Contrast/Highlights/Shadows/Whites/
Blacks (+Vibrance) from image statistics/histograms in one shot; cacheable, comparable, equality-detect.
(`icb-by-feature.md` ## Tone_Grading L251-252 `ICBApply/CalculateAutoToneParams`;
`cr-symbols-curated.txt` `cr_adjust_params::ActivateAutoTone(cr_auto_tone_options)`.)

**Our gap.** We have `autoexposure` (metering, sets exposure) but no one-tap tone baseline that also
proposes contrast/highlight/shadow targets.

**Parity class + gate.** default-safe (Tier 0). Suggestion-only host-side heuristic that WRITES
user-facing params (exposure comp + the item-7 curve controls); it never alters the deterministic
`simulate` path.

**Effort.** M.

**First step.** New `app/.../AutoTone.kt`: reuse the histogram reduction from #2 to compute percentiles,
then propose `exposureCompensationEv` + Whites/Blacks endpoints + parametric contrast into `ParamsState`.
Wire a one-tap button + "reset to before auto" (LR's compare/equality behavior).

**Risks/unknowns.** Our render is a film sim, not scene-linear digital — the percentile targets need
tuning against film stocks (seed from the same histogram the AE already builds).

---

### 9. Embedded-JPEG instant first-paint (RAW) — preview-only, M

**What LR does + CITATION.** Shows the camera's embedded preview JPEG immediately (<100 ms) while the
real decode runs: `ICBGetAndReleasePreviewJpegBytes`; raw analog = LibRaw `unpack_thumb`.
(`RESEARCH_LIGHTROOM_STACK.md` item 6 L49-50 "Small-Med"; `RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §D
L171-172.)

**Our gap.** RAW/DNG open shows nothing until the full LibRaw decode + first `simulate` finishes. Big
perceived-latency win on RAW open.

**Parity class + gate.** preview-only (placeholder). The thumbnail is displayed only; it NEVER enters the
pipeline, so parity is untouched by construction.

**Effort.** M.

**First step.** Surface `LibRaw::unpack_thumb()` through the `:lib:libraw` JNI (alongside the existing
linear-ACES import), hand the JPEG bytes to `:app`, paint it in `Viewer.kt` as the first frame, then
replace with the real `SimResult` when `simulate` returns. Fall back to a coarse 128 px sim if no
embedded thumb.

**Risks/unknowns.** Thumbnail orientation/aspect must match the eventual render's crop; treat as a
transient placeholder, don't let users export it.

---

### 10. Spectral-upsampling prefix cache (warm-tune speedup) — default-safe, M

**What LR does + CITATION.** ACR caches the deterministic prefix (decode/linearize/demosaic, file-keyed)
and recomputes develop settings fresh; the `cr_*_cache` family memoizes each stage keyed on ONLY the
params that stage reads. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §D "ACR cache seam" L167-168.)

**Our gap.** We have per-instance film-density + print-density stage memos and a Kotlin `GradeCache.kt`,
but tuning print/scan params still re-pays the 81-band `spectral_upsampling` cost (the filming hotspot).
Caching the RGB→spectral→camera-raw prefix keyed on input+camera/Hanatos params would make print/scan
tuning near-instant.

**Parity class + gate.** default-safe. Pure memoization — same inputs → same bytes, bit-exact by
construction, zero parity risk. This is NOT a new engine capability, just a cache seam.

**Effort.** M.

**First step.** In `filming.cpp`, add a memo of the post-`spectral_upsampling` camera-raw buffer keyed on
{input buffer hash, camera UV/IR, `hanatos2025` surface/adaptation, spectral-blur}. Reuse the existing
per-instance memo pattern already used for film-density. Confirm the key excludes every print/scan param
so those stay cache-hits while tuning them.

**Risks/unknowns.** Memory: an 81-band prefix buffer is large at full res — cache only at preview res, or
store the camera-raw (3-band) result, not the 81-band intermediate. Grain (our ~90% export hotspot per
`docs/EXPORT_PERF_2026-07-02.md`) is the next cache/skip target when its params are unchanged.

---

### 11. Clarity / Texture (local-contrast) — default-safe, L

**What LR does + CITATION.** Clarity = edge-aware midtone local contrast (large-sigma unsharp mask +
midtone tone-mask, built on a Gaussian/Laplacian pyramid, gated so deep shadows/highlights are
protected). Texture = mid-FREQUENCY band boost (DoG/bilateral, noise-sparing).
(`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §C "Clarity/Texture/Dehaze" L144-146; `cr-symbols-curated.txt`
`cr_stage_localized_detail_clarity_blur`/`_clarity_mask`, `cr_tone_map_info::ClarityToneMask`,
`cr_stage_texture_direct_gf_ycc`; `icb-by-feature.md` ## Tone_Grading L257/L264.)

**Our gap.** No local-contrast controls. Our masking v1 already does "Class-S spatial" local ops
post-engine (`masks/MaskSpatial.kt`), proving post-engine spatial work is viable.

**Parity class + gate.** default-safe (Tier 2). Global Clarity/Texture as a post-engine spatial op on the
output RGB buffer (same seam as `MaskSpatial.kt`). Amount 0 = no-op. Because it lives on output, the 35
`cpp` goldens are untouched — but it must still be thread-invariant (the Tier-2 compositor already runs
deterministically).

**Effort.** L (two radii + tone-mask; needs a solid separable-Gaussian on the output buffer).

**First step.** New post-op `LocalContrast.kt` reusing the Gaussian primitive `MaskSpatial.kt` already
uses (or bind `kernels/gaussian` through a Tier-2-safe path). Clarity = large-sigma USM gated by the
midtone tone-mask (smooth luma rolloff protecting shadows/highlights); Texture = DoG mid-band add-back.
Add amount sliders to `ParamsState.kt`.

**Risks/unknowns.** If ever moved onto the engine/export path for speed it becomes opt-in (Tier 3) with a
golden; keep it Tier-2 first. Large-sigma blur cost at 12 MP — pair with #13's pyramid or compute on a
decimated grid + joint-bilateral upsample (`cr_stage_bilateral_upsample`, RE'd) later.

---

### 12. Dehaze (dark-channel prior) — default-safe, L

**What LR does + CITATION.** Dark-Channel-Prior atmospheric model `y = t·x + (1−t)·a` → recover
`x = (y−(1−t)a)/t`; **full equations published (patent US20160196637A1)** — the only tone tool with a
published closed form. Estimates atmospheric light + transmission map; operates on the un-warped image.
(`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §C L144-146; `cr-symbols-curated.txt` `cr_stage_dehaze`,
`cr_build_dehaze_mask_task`.)

**Our gap.** Absent. Most faithfully portable of the spatial tools because the math is published.

**Parity class + gate.** default-safe. Tier-1 pre-engine on the linear input buffer (dehaze belongs
before the virtual film) OR Tier-2 post; either way default amount 0 = identity → parity-free.

**Effort.** L (dark-channel = local-min filter + guided-filter transmission refine).

**First step.** New pre-engine transform on the decoded linear input (the Tier-1 seam that feeds fixed
buffers into `filming`): implement the DCP model exactly — atmospheric light `a` from the brightest
dark-channel pixels, transmission `t` from the dark channel, guided-filter refine (the same guided-filter
primitive #16 needs), recover `x`. Negative amount adds haze.

**Risks/unknowns.** Guided filter must be deterministic. Lower aesthetic priority for a film sim than the
tone/color items, but cheap given published equations — good "when the guided-filter primitive exists"
follow-on to #16.

---

### 13. Progressive coarse→fine pyramid preview — preview-only, L

**What LR does + CITATION.** Replays the edit list through a Gaussian pyramid (US5790708: octaves,
256² tiles, level chosen by viewport pixel demand), renders a chosen level first then refines in place;
symbols `cr_base_pyramid`, `ICBSetRenderLevel`, "Choosing RPTM Pyramid Level".
(`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §D "Foundations" L156-166; `RESEARCH_LIGHTROOM_STACK.md` item 2
L38-40.)

**Our gap.** `simulate_preview` renders one fixed 640 px level; no coarse-first-then-refine. Confirmed
not built (no pyramid/renderLevel symbol in our tree). Highest-ROI perf item per the impl doc; validated
by Adobe's own "approximate preview / bit-exact export" patents — which IS our parity policy.

**Parity class + gate.** preview-only. The scan/export path is untouched, so `simulate_e2e` is
unaffected. Build an input Gaussian pyramid + viewport level-select, scaling spatial-kernel radii with
the level.

**Effort.** L.

**First step.** In `spektra.cpp` `simulate_preview`: render a coarse downscale immediately (reuse
`crop_resize`/downscale + `kernels/gaussian`), swap in the full preview-res render when ready. Keep it
strictly on the preview branch.

**Risks/unknowns.** Interaction with the draft/settle path (#3) — unify them. Large but preview-only, so
no parity exposure.

---

### 14. Capture-sharpen Detail + edge Masking — opt-in, M

**What LR does + CITATION.** Luminance-channel USM with Amount/Radius/**Detail** (halo-suppression →
deconvolution-like at 100) and **Masking** (edge-gradient gate limiting sharpening to edges), plus a
highlight/shadow tone-mask. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §E "Sharpen" L195-198;
`cr-symbols-curated.txt` `cr_stage_sharpen_3` constants `kDetailSharpenAmount`/`kEdgeMaskWidthScale`/
`kToneMaskStartA`; `icb-by-feature.md` ## NR_Sharpen L471.)

**Our gap.** We ship a fixed scanner unsharp pair (`scanning.cpp` ~L347-360, `do_unsharp`, default off =
goldens bit-exact) with no Detail/Masking terms and it works per-channel, not on luminance.

**Parity class + gate.** opt-in (Tier 3) — extend the EXISTING gated `do_unsharp` seam. Default stays
off/identity so scanner goldens hold; the new Detail + edge-mask + luminance-channel behavior needs its
own oracle golden + thread-invariance assertion before enabling.

**Effort.** M.

**First step.** In `scanning.cpp`, generalize the unsharp block: operate on luminance, add a
gradient-magnitude edge mask (`kernels/gaussian` supplies the blur) and a Detail halo-suppression term.
Add params + a new `capture_sharpen_e2e` gate mirroring the existing `tonecurve`/`gamut_out_*` gate
structure. Deconvolution mode (Detail=100) is a later opt-in.

**Risks/unknowns.** Must be tile/thread-invariant to satisfy `test_parallel`. Keep the current default
byte-identical.

---

### 15. Input noise reduction (pre-engine only) — default-safe, L

**What LR does + CITATION.** Luminance NR = multi-scale/wavelet band attenuation; Color NR = chroma
bilateral/wavelet; ISO-adaptive, in a variance-stabilized (Anscombe-like) YCC domain with a per-camera
noise model. **Key negative constraint: OUTPUT NR is WRONG for a film sim — it erases modeled grain.**
(`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §E "NR" L198-199; `cr-symbols-curated.txt`
`cr_flatten_raw_noise_curve`, `cr_stage_denoise`.)

**Our gap.** No NR. Only INPUT NR (clean the digital capture before spectral upsampling) is appropriate,
so the virtual negative is built from a cleaner input.

**Parity class + gate.** default-safe (Tier 1). Pre-engine op on the linear input buffer; default off =
identity → parity-free. **Do NOT offer output NR** — it would remove `model/grain.cpp`'s synthesized
grain.

**Effort.** L (edge-aware/wavelet NR).

**First step.** New Tier-1 pre-engine stage on the decoded linear input (`:lib:libraw` → input buffer),
before `filming`. Separate luma/chroma radii; the variance-stabilizing-transform idea is also directly
relevant to `model/grain.cpp`'s Poisson-binomial cost (note it).

**Risks/unknowns.** AI/CNN denoise is explicitly out of scope (desktop NPU-only per the doc). Keep it a
manual, pre-engine, default-off knob.

---

### 16. Edge-aware Highlights/Shadows (guided filter) — default-safe, XL

**What LR does + CITATION.** Highlights/Shadows are **LOCAL edge-aware operators** (local-Laplacian /
guided-filter regional gain driven by a luminance mask), NOT a 1-D curve — "the key implementation
consequence". (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §C "★ Highlights/Shadows are LOCAL" L133-135, "port
line 152 (hardest)"; `icb-by-feature.md` ## Tone_Grading L250-315; `cr-symbols-curated.txt`
`cr_stage_guided_filter_ycc`, `cr_stage_fill_light_32`, `cr_stage_local_whites_blacks`.)

**Our gap.** We expose only global `exposureCompensationEv`. Highest-value tone insight but hardest to
port — requires a guided filter we don't have (`kernels/{gaussian,exponential_filter}` give the
primitives to build one).

**Parity class + gate.** default-safe (Tier 2, post-engine on output RGB, like `MaskSpatial.kt`). Amount 0
= identity. Produces a base/detail split then applies regional gain by a luma mask.

**Effort.** XL — spatial + must be thread/tile-invariant to satisfy the determinism bar even at Tier 2.

**First step.** Build a deterministic guided filter (box-mean via `kernels/gaussian` or a summed-area
table) as a reusable primitive, then a `LocalTone.kt` post-op: base/detail split, luma-mask-driven
regional gain for Highlights (compress top) and Shadows (lift bottom). This same guided-filter primitive
unlocks #12 (Dehaze) and #11 (Clarity tone-mask).

**Risks/unknowns.** The hardest item; sequence it after the guided-filter primitive is proven on the
cheaper Dehaze/Clarity paths. If pushed onto the export path for perf, it becomes parity-risk — keep it
Tier-2.

---

### 17. Lens-profile distortion / vignette / CA correction — default-safe, L

**What LR does + CITATION.** LCP: Brown-Conrady radial distortion `1+k1r²+k2r⁴+k3r⁶` (+tangential),
vignette gain `1+a1r²+a2r⁴+a3r⁶` (Adobe deliberately under-corrects ~50-70%), lateral CA = separate R-G
& B-G radial polys, EXIF-matched per focal/aperture. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §E "Lens"
L184-194; `icb-by-feature.md` ## Geometry_Lens L370-431 `ICBApplySelectedLensProfile`.)

**Our gap.** `LensProfiles` data exists but there is NO UI and no warp stage.

**Parity class + gate.** default-safe (Tier 1). Pre-engine geometry warp on the linear input (before
`filming`), or folded into `crop_resize`; identity warp when disabled → parity-free (the parity path
assumes already-corrected linear input).

**Effort.** L (Brown-Conrady undistort + per-channel CA resample; no OpenCV linked today).

**First step.** Add a pre-engine warp reading the existing `LensProfiles` data, matched by EXIF
make/model/focal. Implement Brown-Conrady undistort + radial vignette gain + per-channel CA scale
directly (avoid pulling in OpenCV). Wire the correction-amount sliders + a UI.

**Risks/unknowns.** Match Adobe's 50-70% vignette under-correction constant. Resample determinism if it
ever needs a golden (it won't at Tier 1 when identity-off).

---

### 18. Copy/paste-by-section + named snapshots + crs schema mirror — default-safe, M

**What LR does + CITATION.** The `crs:` XMP namespace is Adobe's canonical edit schema; copy-paste is
section-based subsets (tone-only/color-only/masks-only), masks re-derived on paste; process-version
pins reproducibility. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §F "Versions/copy-paste" L218-220;
`RESEARCH_LIGHTROOM_RENDER.md` "Process version" L14.)

**Our gap.** Only linear `EditHistory` (undo/redo), no named snapshots, no section-scoped copy/paste, and
`PRESET_VERSION` is written but never read on decode (robustness gap in the inventory).

**Parity class + gate.** default-safe (Tier 0/4). Kotlin `ParamsState`/recipe-JSON only; zero engine.

**Effort.** M.

**First step.** In `Recipes.kt`/`Presets.kt`, add named snapshots + section-scoped copy/paste (tone /
color / masks subsets), and shape the recipe JSON field names to mirror `crs:` sections for future LR XMP
interop. Wire version-gated migration reading `PRESET_VERSION` (fix the write-but-never-read gap) and
stamp a "render version" into saved recipes so future opt-in stages don't silently alter old edits.

**Risks/unknowns.** Full XMP round-trip with LR is a stretch goal; start by mirroring the schema shape.

---

### 19. AVIF / JPEG-XL export writers — default-safe, M

**What LR does + CITATION.** AVIF (AV1-in-HEIF, 8/10/12-bit, CICP, `tmap` gain map) and JPEG-XL
(ISO 18181, up to 32-bit float, `jhgm` gain map). (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §F "Export
formats" L221-224; `RESEARCH_LIGHTROOM_STACK.md` item 8 L52-53.)

**Our gap.** We ship 16-bit TIFF/PNG + Ultra-HDR + JPEG (`ExportSheet.kt`, `:lib:tiffwriter`,
`:lib:pngwriter`). AVIF is the biggest format gap; JPEG-XL next.

**Parity class + gate.** default-safe. New encoder module(s) consuming the SAME final RGB buffer the
engine already emits; encode-only, after the pipeline → no engine/parity implication.

**Effort.** M per format.

**First step.** Add a native AVIF encoder module (e.g. `avif-coder`/libavif) parallel to
`:lib:tiffwriter`/`:lib:pngwriter`, wired into `ExportOptions.kt`/`ExportSheet.kt`. Map our Ultra-HDR
gain-map path to AVIF `tmap` for HDR.

**Risks/unknowns.** APK size + 16 KB-page alignment for the new `.so` (CI gates `zipalign -P 16`).
Lower priority than tone/color work.

---

### 20. Tiled full-res export (halo + tile-invariance gate) — parity-risk, XL

**What LR does + CITATION.** Export is tiled (const/dirty tile buffers), halo = operator radius,
raised-cosine overlap blend; global scalars (autoexposure) computed in a whole-image pre-pass so tiling
stays exact. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §D "Tiling" L169-170; `RESEARCH_LIGHTROOM_STACK.md`
item 3 L41-43; `cr-symbols-curated.txt` `cr_cpu_const_tile_buffer`/`cr_cpu_dirty_tile_buffer`.)

**Our gap.** Only app-side OOM-retry + opt-in half-decode; no native tiling/streaming for >12 MP. Enables
memory-bounded large-image export.

**Parity class + gate.** **parity-risk** — touches the default export path. The spatial stages
(`model/diffusion` halation/scatter, `grain`, lens blur, downscale AA) are neighborhood ops, so naive
tiling changes seams. Ships ONLY with: halo per tile = each spatial op's radius, raised-cosine stitch,
and **`test_parallel` extended to assert TILE-invariance (byte-identical across tile sizes)** exactly as
we already prove thread-invariance. Our `autoexposure` is already a whole-image pre-pass (matches
Adobe's global-scalar rule).

**Effort.** XL, native.

**First step.** A tiled driver over the existing stages in `spektra.cpp` with per-stage halo; add the
tile-invariance assertion to the parity harness BEFORE enabling. Do not merge until byte-identical vs the
whole-frame render is proven.

**Risks/unknowns.** Highest-parity-risk item in the set. Only pursue when >12 MP export demand justifies
it; `kernels/parallel` determinism must be preserved by the tile scheduler.

---

## Already have — do not rebuild

Verified present in the tree; the readers list these but we ship them:

- **Mask geometry (linear/radial), luminance & color range masks, add/subtract/intersect fold, per-mask
  local adjustment set** — `app/.../masks/*` + `MASKING_SPEC.md`. (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md`
  §A is largely satisfied; only **brush + AI Select Subject/Sky + depth-range** remain — see below.)
- **Creative Temp/Tint white balance + gray-point eyedropper** — `CreativeWhiteBalance.kt`,
  `LocalWhiteBalance.kt`, `FilmStockBalance.kt`. (icb ## WhiteBalance_Color, cr WB stages.)
- **Saturation + Vibrance** — `ColorGrade.kt` (Oklab, gray-neutral). (icb Vibrance/Saturation.)
- **Point tone curve, master + per-RGB, monotone-cubic; Contrast** — `ToneCurve.kt`, `ContrastCurve.kt`,
  `kernels/tonecurve.cpp`. (§C ToneCurvePV2012 point-curve part.)
- **Preset/Profile Amount (continuous lerp in param space)** — `PresetAmount.kt`. (§F Profiles vs presets.)
- **Output gamut compression (ACES-RGC/Oklch/Oklrab) + input xy** — `GamutCompress.kt` + engine opt-in
  gates. Doubles as the LR soft-proof/gamut-warning primitive if a preview overlay is wanted.
- **Per-stage caches** — film/print-density memos + `GradeCache.kt`. (§D ACR cache seam — partially;
  #10 extends it to the spectral prefix.)
- **fp16 groundwork** — `kernels/half.{h,cpp}` (CI-gated, preview-only). (Stack item 1.)
- **JPEG / Ultra-HDR / 16-bit TIFF+PNG / 32f + scene-linear TIFF export** — `ExportSheet.kt`.
- **Whole-image AE pre-pass** — `autoexposure` (matches Adobe's global-scalar-in-pre-pass rule).

## Explicitly deferred / rejected

- **Output noise reduction** — rejected: erases the grain `model/grain.cpp` synthesizes (the whole point
  of a film sim). Only INPUT NR (#15) is appropriate. (§E "NR".)
- **AI Denoise / generative remove** — deferred: desktop NPU/TensorCore only, not on-device mobile per
  the RE. (§E L198-199.)
- **GPU compute path for the spectral integral** — rejected as an export accelerator: different FMA/float
  ordering diverges from the CPU oracle, breaking bit-exactness. Only viable preview-only with relaxed
  tolerance; `LutGpuPreview.kt` already explores this. (Stack item 7.)
- **Depth-aware Lens Blur / Bokeh** — deferred: depends on Adobe's cloud CPF depth map
  (`cr_cpf_service_request`), not portable; only the variable-radius splat is portable if a depth source
  ever exists. (icb ## ML_Bokeh_Adaptive; cr `cr_stage_lens_blur`.)
- **DNG dual-illuminant per-camera color / DCP into default filming** — rejected for the default path: we
  have no per-camera DCP, and a DCP camera profile cannot enter the parity-locked filming stage. It can
  only ship as the item-1 "Neutral" preset or a future opt-in input-rendering mode.
  (`RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §B L123 "Gap"; `RESEARCH_LIGHTROOM_RENDER.md` camera-profile.)
- **AI Select Subject / Sky masks** — deferred (heavy): net-new on-device LiteRT/TFLite runtime +
  segmentation models (~3 MB); sequence AFTER the deterministic mask geometry (already shipped). Feeds
  the existing mask compositor, never the parity engine. (§F "ML stack"; Stack item 9.)
- **Upright / perspective / homography geometry** — deferred: lowest fit for a film sim; standalone
  geometry-only opt-in (Tier 1) if pursued, orthogonal to the spectral pipeline. (§E "Geometry/Upright".)
- **Alternate cheap grain mode (LR seeded-blurred-noise overlay)** — allowed only as an OPT-IN alternate
  grain mode, NEVER a change to the default (any default grain change breaks `simulate_e2e`). Low
  priority given our physical `model/grain.cpp` is the parity default. (cr `cr_grain_maker`/
  `cr_stage_overlay_grain`.)
