# Masking spec — Lightroom-RE'd, for the Spectrafilmandroid port

> **STATUS 2026-07-02: masking v1 SHIPPED** (linear/radial + luminance/color range, 13 local ops
> incl Class-S spatial, draw-on-preview overlay, eyedroppers; per-component invert/value in
> Mask.kt). Open: brush, polygon, AI Subject/Sky, XMP interop/export, Dehaze.
>
> The shipped app JSON interchange envelope is separately normative and
> versioned: [MASK_JSON_SCHEMA.md](MASK_JSON_SCHEMA.md) and
> [schema/spektrafilm-mask-v1.schema.json](schema/spektrafilm-mask-v1.schema.json).

Synthesis of a 3-agent reverse-engineering sweep over our own decompile of the current Lightroom
Android build (`docs/lightroom-re/` — `cr_*`/`ICB*` symbols), ExifTool's verbatim `crs` masking
schema, and Adobe public docs, cross-checked against our current `app/.../masks/` code. The goal:
mirror Lightroom's **proven** on-disk masking data model **1:1 for free XMP interop**, while keeping
every pixel op a **parity-safe Tier-2** pass on the engine OUTPUT (`simResultToBitmap` / `SimResult.data`
seam) — `engine/spektra-core/cpp/**` is never touched, so the C++ parity suite is unaffected.

## Ground-truth tiers (used throughout)
- **[GT-XMP]** confirmed in ExifTool's published XMP-crs tag table (field name + type authoritative).
- **[GT-RE]** confirmed by our decompile (`cr_*` symbol names are real, not stripped).
- **[OBS]** observed convention in real LR XMP / community parsers (e.g. the `MaskBlendMode` ints have
  **no ExifTool `PrintConv`** — stored raw — so the int→name mapping is RE'd, not spec'd).
- **[RECON]** the *exact math* (alpha falloff curve, brush spacing, Auto-Mask tolerance, color-distance
  metric) is **unrecoverable from a static decompile and unpublished by Adobe** — every formula below is
  a standard reconstruction chosen to match observed behavior. Keep these as **tunables**, never claim
  bit-exact Lightroom. (This is fine: Tier-2/off-engine, no parity impact.)

## Headline: our core model is already correct ✅
Both the schema and geometry sweeps confirmed our `masks/Mask.kt` runtime is schema-faithful:
- **Fold algebra is identical to Adobe's** — ADD `1−(1−A)(1−m)` (soft-OR), SUBTRACT `A·(1−m)`,
  INTERSECT `A·m`; per-component invert `1−m`; folded in document order. [GT-RE: Adobe even implements
  Intersect as subtract-of-inverse — the same darktable identity.]
- **`BlendMode{ADD,SUBTRACT,INTERSECT}` already ordinals to the `crs:MaskBlendMode` ints `0/1/2`.**
- **Linear** = `t = clamp(dot(P−Z,F−Z)/|F−Z|²,0,1)`, `α=smoothstep(t)` — **verbatim** our `Linear.alphaAt`.
- **Radial** = normalized elliptical radius + feather band over `[1−f,1]` — **verbatim** our `Radial.alphaAt`.

So the work is **structural rename/regroup for XMP interop + new component types + the Tier-A op set** —
not a rewrite.

---

## 1. Data model — the `crs:MaskGroupBasedCorrections` schema (1:1 mirror)

Namespace `crs:` (`http://ns.adobe.com/camera-raw-settings/1.0/`). A UI "mask" = a **`Correction`**.

```
MaskGroupBasedCorrections                         [GT-XMP] Seq of Correction
└── Correction                                     (one per UI mask pin)
    ├── What = "Correction"                        [OBS]
    ├── CorrectionID (GUID)                        [OBS]
    ├── CorrectionActive = true                    [GT-XMP] bool
    ├── CorrectionAmount = 1.0                      [GT-XMP] real  — GROUP opacity 0..1
    ├── CorrectionName / CorrectionSyncID          [GT-XMP]
    ├── <the Local* adjustment set>                [GT-XMP] (§4) — the payload
    └── CorrectionMasks                            [GT-XMP] Seq of CorrectionMask (folded in order)
        └── CorrectionMask                         (one shape/range component)
            ├── What = "Mask/<Type>"               [OBS] Gradient|CircularGradient|Paint|Image|RangeMask
            ├── MaskActive = true                  [GT-XMP] bool
            ├── MaskName / MaskSyncID              [GT-XMP]
            ├── MaskBlendMode = 0                  [GT-XMP] int  0=Add 1=Subtract 2=Intersect [OBS map]
            ├── MaskInverted = false               [GT-XMP] bool — PER-COMPONENT invert
            ├── MaskValue = 1.0                    [GT-XMP] real — PER-COMPONENT strength 0..1
            ├── ReferencePoint = "x y"             [GT-XMP] the draggable pin (UI; not in alpha)
            ├── MaskSubType                        [GT-XMP] (opaque pass-through)
            ├── <geometry fields per What>         (§3)
            └── CorrectionRangeMask                [GT-XMP] optional NESTED range refinement (§5)
```

**Two opacity knobs, not one:** group `CorrectionAmount` × per-component `MaskValue`. **Invert is
per-component** (`MaskInverted`); there is **no** `CorrectionInverted` field — a "group invert" is
realized by inverting each component. Range masks **nest inside a component** as an intersect refinement
(not top-level).

### Recommended Kotlin mirror (round-trips to/from LR XMP)
```kotlin
enum class BlendMode { ADD, SUBTRACT, INTERSECT }            // ordinal == crs:MaskBlendMode (PIN THIS)
enum class MaskWhat { GRADIENT, CIRCULAR_GRADIENT, PAINT, IMAGE, RANGE }
enum class RangeType { LUMINANCE, COLOR, DEPTH }

data class Correction(
    val correctionId: String, val active: Boolean = true,
    val amount: Float = 1f,                 // crs:CorrectionAmount (group opacity)
    val name: String = "", val syncId: String = "",
    val adjust: LocalAdjust = LocalAdjust(),
    val masks: List<CorrectionMask> = emptyList(),
)
data class CorrectionMask(
    val what: MaskWhat,
    val active: Boolean = true, val name: String = "",
    val blendMode: BlendMode = BlendMode.ADD,
    val inverted: Boolean = false,          // crs:MaskInverted (PER-COMPONENT — our biggest gap)
    val value: Float = 1f,                  // crs:MaskValue
    val syncId: String = "", val referencePoint: String = "",
    val geometry: MaskGeometry,
    val rangeMask: CorrectionRangeMask? = null,  // nested intersect refinement
)
```
(Full `LocalAdjust` / `MaskGeometry` / `CorrectionRangeMask` data classes per §3–§5.)

---

## 2. Component shapes — geometry + reconstructed alpha [RECON curves]

All geometry **normalized 0..1**; `alpha(nx,ny)∈[0,1]`; pure; resolution-independent (one mask drives
draft/zoom/export). Complete shape set (from the `cr_mask_*` visitor enumeration):

| `What` | C++ class [GT-RE] | crs geometry [GT-XMP] | our status |
|---|---|---|---|
| `Mask/Gradient` (Linear) | `cr_mask_gradient` | `ZeroX,ZeroY,FullX,FullY` | ✅ matches |
| `Mask/CircularGradient` (Radial) | `cr_mask_circular_gradient`/`cr_mask_ellipse` | `Top,Left,Bottom,Right,Angle,Feather,Roundness,Midpoint,Flipped` | ✅ matches; **add `angleDeg`** |
| `Mask/Paint` (Brush) | `cr_mask_paint` | `Dabs,Radius,Flow,Density,CenterWeight,Feather` | ➕ add |
| `Mask/Image` (AI/pixel) | `cr_mask_image` | semantic label + sidecar α | ➕ add (Raster) |
| `Mask/RangeMask` | `cr_mask_range_mask` | nested `CorrectionRangeMask` | ➕ add (§5) |
| Polygon | `cr_mask_polygon` | **no LR-mobile bridge** | ➕ our own design |
| Clip | `cr_mask_clip` | internal = INTERSECT w/ rect | ✅ via BlendMode.INTERSECT |

- **Linear** — pin = midpoint(Zero,Full); feather = the whole Zero→Full distance (no separate slider).
- **Radial** — bbox⇄(center,radii): `cx=(L+R)/2, cy=(T+B)/2, rx=(R−L)/2, ry=(B−T)/2`; `Roundness`=bbox
  aspect; `Angle` rotates the sample (we must add it); invert swaps center↔perimeter value.
- **Brush** [RECON] — stroke = vector of normalized dabs (circles), densified between pointer samples
  (`AddDabsBetween`, spacing ≈ `0.15·r`); per-dab soft disc `k(ρ)= 1 for ρ≤(1−feather), else
  smoothstep((1−ρ)/feather)`; **stamp-accumulate-clamp `α ← min(Density, α + Flow·k·(1−α))`**; optional
  **Auto-Mask** gates `k` by output-color similarity to the seed (`smoothstep(1−‖c−c0‖/τ)`). Stored as a
  path → re-rasterized per resolution.
- **Polygon** [our design] — even-odd inside test + signed-distance feather band; pin = centroid.

---

## 3. The `Local*` adjustment set — classified P (do now) vs S (defer)

All `real`. `*2012` are current; non-suffixed = legacy PV2010 (read-compat only). [GT-XMP names + ranges.]

**Class P — pointwise on output, parity-safe (do in the Kotlin compositor):**
`LocalExposure2012` (−4..+4 stops, **done**), `LocalTemperature`/`LocalTint` (−100..100), `LocalSaturation`,
`LocalHue`, `LocalContrast2012`, `LocalWhites2012`/`LocalBlacks2012`, `LocalToningHue`/`LocalToningSaturation`.
Each = a per-pixel function of that pixel × α; no neighborhood.

**Apply order within one correction** (mirrors PV2012 scene-referred order; in **linear light**:
decode CCTF → ops → re-encode → `(1−α)·in + α·out`):
> **WB(temp/tint) → Exposure → Contrast → Whites/Blacks → Saturation/Hue → Toning.**
Reuse the global math: **Temp/Tint = `CreativeWhiteBalance` Bradford CAT diagonal**, **Saturation/Hue =
`ColorGrade` Oklab chroma scale + hue rotate**, **Contrast = `ContrastCurve` midtone-pivot S**.

**Class S — spatial / edge-aware: Clarity/Texture/Sharpness/Highlights/Shadows ✅ SHIPPED (PR #103,
`MaskSpatial`); Dehaze still deferred; LuminanceNoise/Moire/Defringe out of scope (read-compat only):**
`LocalClarity2012` (large-σ midtone USM), `LocalTexture` (mid-freq DoG/bilateral residual),
`LocalDehaze` (Dark-Channel-Prior, the one published algo), `LocalSharpness` (luma USM),
`LocalHighlights2012`/`LocalShadows2012` (local-Laplacian/guided-filter regional gain — **not** a 1-D
curve, even globally). `LocalLuminanceNoise`/`LocalMoire`/`LocalDefringe` = **out of scope for a
film-sim** (they'd strip the grain/character the engine just modeled) — read-compat only.

---

## 4. Range masks — nested intersect refinement [GT-XMP `CorrectionRangeMask`]

Fields: `Version, Type(0=Lum,1=Color,2=Depth), Invert, LumMin/LumMax/LumFeather, DepthMin/Max/Feather,
ColorAmount, SampleType, AreaModels[≤5]{ColorRangeMaskAreaSampleInfo, AreaComponents}`. [GT-RE:
`cr_mask_range_mask` is a first-class `cr_mask_entry` variant holding a `cr_range_mask`; materialized as
`cr_range_mask_map`, cached via `cr_adjust_params::RangeMaskMapInfo`.]

- **Luminance** — trapezoid on Rec-709 luma of the output pixel, full in `[LumMin,LumMax]`, feather each
  side; `Invert` flips. Cheap, do first.
- **Color** — ≤5 eyedropper Lab/Oklab samples (mean+spread = `AreaModels`); `α = soft-min over samples of
  f(distance)`, tolerance from `ColorAmount`. Cache per sample-set.
- **Depth** — trapezoid on a normalized depth map (HEIC device depth or AI monocular). **Defer** (no depth
  source in our pipeline yet).

Because a range reads the *pixel value*, it can't live in pure `alphaAt(nx,ny)` — split rasterization:
geometry pass (existing) → range pass multiplies that component's α by `range.eval(pixel)` **before** the
fold (the `α·m` intersect). Thread the output buffer (`data,cs,cctf`) into `MaskRaster`; `range==null`
keeps the current fast path.

---

## 5. AI / Sensei on-device masks [GT-RE inventory]

`cr_ml_masking_manager` + `ObjectDetectorType` → **Subject, Sky, Background, Objects, People(+parts)**
(`DoUpdate_SubjectSky/SelectBackground/SelectObjects/SelectPeople`). Pipeline: bundled **TFLite/LiteRT**
encoder-decoder segmenter → coarse logits → **guided-filter/matting edge-refine using the engine OUTPUT
luma as guide** (`RefineMask`) → soft α stored as a `cr_mask_image` component (folds like any other).
**Geometry not baked** — store the *intent* ("subject"/"sky"), **recompute α on apply / image change**,
cache by image fingerprint.

**Our plan:** an AI mask = a `MaskComponent.Raster(alpha,w,h)`. Bundle small Subject+Sky `.tflite` under
`app/src/main/assets/`; run on the existing decode/simulate background executor (GPU delegate optional);
guided-filter refine on output luma (the one new spatial primitive — reused by Class-S Clarity/Dehaze).
**Excluded:** PDR/generative-remove, AI-denoise, skin-blemish (cloud / desktop-NPU).

---

## 6. Caching — composite fingerprint tree [GT-RE `cr_composite_cache_tree`]

LR composites bottom-up with a fingerprint at every level so a slider drag on one mask never recomputes
the others or the engine render: `maskNode`(component α) → `maskCompositeNode`(folded mask) →
`correctionParametersNode`(Local* within α) → `correctionCompositeNode` → root, keyed by
`ComputeMaskFingerprint`/`ComputeCorrectionFingerprint`/`ComputeCompositeFingerprint`; per-component α
cached by `dng_fingerprint` (`cr_holder_cache`), with stable per-component `MaskSyncID`.

**Our plan (mirror at our scale):** per-`CorrectionMask` `syncId`; component-α cache keyed
`hash(syncId, params, imageFingerprint, rasterRes)` — cheap for geometry, the **real payload is AI rasters
+ Color-range passes** (invalidate on image change, never on a slider drag); correction + composite
fingerprints so unchanged corrections are reused. Fingerprints exclude resolution; α cached per
(fingerprint, resolution) so draft + export coexist (our normalized geometry already gives this).

---

## 7. Deltas vs our current `masks/` code

Current: `Mask{components, invert, opacity}`, `Component{mode, shape}`, `MaskComponent.{Linear,Radial}`,
`TierADelta{exposureEv,temp,tint,saturation,contrast}`, `LocalAdjustment{mask,delta}`; `MaskCompositor`
does exposure-only. **The per-pixel math + fold are already correct and parity-free.** Structural deltas:

1. ✅ **DONE** — **Per-component `invert`** (`crs:MaskInverted`) — `Mask.Component.invert`, applied
   `1−c` before the fold (Mask.kt).
2. ✅ **DONE** — **Split opacity** → per-component `value` (`crs:MaskValue`) shipped in
   `Mask.Component.value` (Mask.kt).
3. **OPEN** — **Regroup `Mask`→`Correction`** `{amount, active, adjust, masks[]}`; rename
   `TierADelta`→`LocalAdjust` with `crs` names (`exposure2012/temperature/contrast2012/…`); widen to
   the full set (wire 5, carry the rest as data for round-trip).
4. **OPEN** — **Component identity** — `What`/`MaskSubType`/`ReferencePoint`/`MaskSyncID`/`MaskName`
   so foreign LR masks survive a round-trip; tolerate unknown `What`.
5. ✅ **DONE** — **Radial `angleDeg`** (rotation) shipped in `Radial` (Mask.kt); bbox⇄center/radii
   converter at the XMP boundary still open.
6. **OPEN** — **New components:** `Brush` (§2), `Polygon` (§2, our own), `Raster` (AI, §5).
7. ✅ **DONE** (Luminance + Color) — **Nested `rangeMask`** on the component (§4):
   `LuminanceRange`/`ColorRange` shipped (Mask.kt); Depth deferred.
8. ✅ **DONE** — **Pin a unit test** asserting `ADD/SUBTRACT/INTERSECT.ordinal == 0/1/2`
   (MaskTest.kt).

## 8. Prioritized implementation plan (each parity-safe, engine untouched)
1. **Class-P Tier-A ops** in `MaskCompositor` (temp/tint, saturation, contrast, whites/blacks, hue) in the
   order above, reusing `CreativeWhiteBalance`/`ColorGrade`/`ContrastCurve`. *(immediate value; pure
   extension of the working exposure v1.)*
2. **Schema refinements** (§7.1–§7.4) + the `"masks"` recipe-JSON block (1:1 `crs` mirror) + the gesture UI
   (linear/radial first, reusing `CropOverlay`'s normalized-coord patterns) → masks become user-creatable.
3. **α-cache** (fingerprint/SyncID) — before Color-range/AI make per-pixel/inference cost real.
4. **Range masks** — Luminance (trivial) → Color.
5. **Brush** + **Polygon**.
6. **AI Subject/Sky** (LiteRT + guided filter).
7. **Class-S** — ✅ **Clarity / Texture / Sharpness / Highlights / Shadows SHIPPED** (PR #103: `MaskSpatial`
   box-blur USM + regional gain on the output luma; radii scale with the long edge). Ticket #141 replaces
   the full-frame alpha/luma/blur scratch with an exact row-streamed, radius-bounded compositor. Its
   memory/publication contract, current-source device-requalification gate, historical measurements,
   and still-open performance gap are in [MASK_COMPOSITOR_MEMORY.md](MASK_COMPOSITOR_MEMORY.md). The
   final preview/export call sites must still pass the live cancellation token before closure.
   **Dehaze**
   (Dark-Channel-Prior) remains — a separate algorithm.

## Sources
In-repo decompile `docs/lightroom-re/{cr-symbols-curated.txt, icb-by-feature.md, icb-signatures.txt,
icb-methods-full.txt, tiparamsholder-natives.txt}`; `docs/RESEARCH_LIGHTROOM_IMPLEMENTATION.md` §A/§C/§D/§F;
ExifTool XMP-crs tag table; Adobe ACR/Lightroom masking + radial/range-mask help. Matting patents
US10068361/US9786078; Dehaze US20160196637A1. Reconstructed curves [RECON] are tunables, not Adobe-exact.
