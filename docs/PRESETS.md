# Spektrafilm Built-in Presets

Spektrafilm ships 28 curated presets, each pairing a film stock with a print medium and a
small set of complementary engine tweaks to reproduce a coherent, recognizable look. The
machine-readable definitions live in
`engine/spektra-core/src/main/assets/spektra/presets.json`; this document is the design
rationale and the cited research behind every choice. (Presets added after this document was
written are described in `presets.json` itself.)

## How presets work

A built-in preset is a sparse overlay: it changes only the fields authored in `presets.json`, and
`BuiltInPresets.apply` leaves every omitted field at its current editor value. On a fresh/reset
editor those values are the `ParamsState` defaults; after prior edits or another built-in preset,
they may not be. These are intentionally light-touch looks rather than full state snapshots. User
presets use a separate full-state schema.

Key knobs used here:

- **`camera.exposureCompensationEv`** — overall exposure. Positive opens up shadows / pushes
  toward the bright, airy end of a negative's latitude.
- **`filmRender.densityCurveGamma`** — film contrast. `>1` snappier, `<1` softer. The bundled
  values span 0.88–1.18; Faded Matte and Bleach Bypass are the deliberate endpoints.
- **`filmRender.grain.blur`** — grain softness. Higher = smoother/finer-looking; lower =
  grittier. `agxParticleScale` (R,G,B) enlarges grain when we want it visible.
- **`filmRender.halation.*`** — the glow bright lights scatter into the emulsion.
  `halationAmount`/`scatterAmount` scale the effect; `boostEv` reconstructs clipped highlights
  so the glow blooms harder; `halationStrength` is an R,G,B triple — its red-weighted default
  is exactly why film halation reads as a warm/red halo.
- **`filmRender.dirCouplers.amount`** — interlayer (DIR) coupler strength: edge contrast and
  color crispness. Nudged up slightly for punchy stocks.
- **`io.scanFilm`** — when `true`, scans the developed film directly and skips the print
  stage. This is the correct path for reversal/slide stocks (they are already positives) and
  for anyone who wants the "raw scan" negative look.
- **`scanner.unsharpMask`** — capture sharpening; raised for the crispest, finest-grain stocks.
- **`enlarger.*`** — untouched unless a preset explicitly authors a filter shift or preflash.
  Several creative/stock-specific looks do so; all other presets preserve the current enlarger
  state.

Presets are grouped: Portrait, Landscape, Slide / Chrome, Cinema, Low Light / Night,
Nostalgic / Consumer, Creative, and Neutral. The IDs below are checked against `presets.json` by
`tools/docs/check_docs_consistency.py`; the asset is authoritative for exact parameter values.

---

## Portrait

### Portra 400 — Wedding Warm  (`portra400_endura_wedding`)
**Kodak Portra 400 → Portra Endura.** The archetypal wedding/portrait negative. Portra 400 is
prized for warm, natural skin and a relatively low-contrast curve that holds both highlight
and shadow detail in tricky light, with surprisingly fine grain for a 400 speed. Pairing it
with low-contrast Portra Endura keeps everything soft and forgiving.
Tuning: `exposureCompensationEv 0.3` (negatives flatter slight overexposure — cleaner shadows,
creamier skin), `densityCurveGamma 0.96` (a touch gentler still), `grain.blur 0.85` (smooth,
restrained texture), `halation 1.0` (subtle, just enough warmth on highlights). DIR couplers
on for natural color separation.

### Portra 160 — Soft Light Portrait  (`portra160_endura_softlight`)
**Kodak Portra 160 → Portra Endura.** Portra 160 is the finest-grain, lowest-contrast member of
the line — built for studio/soft-light portraiture. On low-contrast Endura the result is
luminous and delicate.
Tuning: `exposureCompensationEv 0.3`, `densityCurveGamma 0.94` (softest of the portrait
presets), `grain.blur 0.9` (near-invisible grain), `halation 0.85` (very restrained).

### Pro 400H — Airy Pastel  (`pro400h_crystalarchive_pastel`)
**Fujifilm Pro 400H → Fujifilm Crystal Archive Type II.** Pro 400H's signature is the airy,
pastel wedding look that emerges when it is generously overexposed — colors lighten, greens
stay clean and fresh, and skin renders cooler and softer than Kodak. Crystal Archive keeps the
Fuji palette intact.
Tuning: `exposureCompensationEv 1.0` (the deliberate overexposure that triggers the pastel
shift), `densityCurveGamma 0.9` (low contrast for the lifted, airy tonality), `grain.blur 0.9`,
`halation 0.9`.

---

## Landscape

### Ektar 100 — Punchy Landscape  (`ektar100_ultra_landscape`)
**Kodak Ektar 100 → Ultra Endura.** Ektar is the world's finest-grain color negative, the
closest C-41 gets to slide film: vivid, saturated, high-acutance, with especially punchy blues
and greens. High-contrast Ultra Endura amplifies that into a bold, graphic landscape look.
Tuning: `densityCurveGamma 1.06` (snappy contrast Ektar can carry), `grain.blur 0.9`
(its real grain is tiny — keep it clean), `halation 0.7` (Ektar has strong halation
protection; keep glow minimal), `dirCouplers.amount 1.15` (extra edge/color crispness),
`scanner.unsharpMask [0.9, 0.7]` (sharpen for the "HD" Ektar bite).

### Ektar 100 — Travel Vivid  (`ektar100_supra_travel`)
**Kodak Ektar 100 → Supra Endura.** The same stock on moderate-contrast Supra, for travel and
mixed scenes where you want Ektar's saturation without Ultra's harder contrast clipping skin
and skies.
Tuning: neutral `densityCurveGamma 1.0`, `grain.blur 0.85`, `halation 0.7`,
`dirCouplers.amount 1.1`.

### Superia X-TRA 400 — Cool Greens  (`superia400_crystalarchive_landscape`)
**Fujifilm Superia X-TRA 400 → Crystal Archive Type II.** A foliage-oriented variant that keeps
Fuji's green/cyan signature and adds a faint cool print balance. It uses a small enlarger-filter
shift, lightly stronger density/coupler settings, and restrained grain/halation.

---

## Slide / Chrome

All slide presets use **`io.scanFilm: true`** — reversal films are positives, so they are
scanned directly with no print stage. (A print profile is still listed because the schema
requires the field; it is ignored when `scanFilm` is true.) Grain is barely touched
(`blur 0.95`) because E-6/K-14 stocks are very fine-grained, and halation is held low — chromes
have strong anti-halation backing.

### Velvia 100 — Chrome Landscape  (`velvia100_chrome_landscape`)
**Fujifilm Velvia 100.** The landscape shooter's slide: ultra-saturated, high-contrast,
exceptionally sharp, with yellows/reds/blues/greens that nearly run out of the frame.
Tuning: `densityCurveGamma 1.05` (Velvia's punchy contrast), `halation 0.5`,
`dirCouplers.amount 1.1`, `scanner.unsharpMask [0.9, 0.7]` for bite.

### Provia 100F — Natural Chrome  (`provia100f_chrome_natural`)
**Fujifilm Provia 100F.** The reference E-6 transparency: natural, accurate color, modest
contrast, fine grain — the calm counterpoint to Velvia.
Tuning: neutral `densityCurveGamma 1.0`, `halation 0.5`. Everything else default.

### Ektachrome E100 — Clean Slide  (`ektachrome_e100_chrome_clean`)
**Kodak Ektachrome E100.** The revived 2018 slide with clean neutrals, fine grain, and a wide
tonal range for a reversal film — slightly cooler/cleaner than the Fuji chromes.
Tuning: neutral contrast, `halation 0.5`.

### Kodachrome 64 — Nostalgic Chrome  (`kodachrome64_chrome_nostalgic`)
**Kodak Kodachrome 64.** The legendary K-14 look: deep saturated reds, luminous blues, rich
earthy greens, warm golden midtones, fine grain, and strong-but-not-harsh contrast with inky
blacks.
Tuning: `densityCurveGamma 1.04` (its punchy contrast), `halation 0.6` (a hair more warmth than
the other chromes to support the nostalgic glow). The distinctive red/green response comes from
the stock's own spectral profile.

---

## Cinema

Motion-picture negatives are printed onto a release print film (2383 standard / 2393 premium)
rather than RGB paper — that print stage *is* the cinema look: rich blacks, neutral highlights,
and the saturated film-print palette.

### Vision3 500T — Night Cinema  (`vision3_500t_2383_night`)
**Kodak Vision3 500T → 2383.** The fast tungsten-balanced workhorse of digital-era film, the
night/low-light cinema stock. In stills use (remjet removed), 500T is famous for the warm red
halation halo that blooms around point lights — streetlamps, neon, headlights.
Tuning: halation pushed hard — `halationAmount 1.6`, `scatterAmount 1.3`, `boostEv 1.0`
(reconstruct clipped highlights so the glow blooms), `halationStrength [0.09, 0.02, 0.0]`
(red-weighted for the classic warm halo). `grain.blur 0.75` so 500T's grain reads as real
texture. This is the signature "CineStill-style" tungsten night look.

### Vision3 250D — Daylight Cinema  (`vision3_250d_2383_daycinema`)
**Kodak Vision3 250D → 2383.** The medium-speed daylight cine negative: rich contrast, deep
blacks, fine grain, broad latitude — the everyday modern theatrical look in daylight.
Tuning: neutral contrast (the 2383 print supplies the cinema contrast), `grain.blur 0.8`,
`halation 1.1` (a gentle daytime glow, far less than 500T).

### Vision3 50D — Premier Daylight  (`vision3_50d_2393_premier`)
**Kodak Vision3 50D → 2393 (Premier).** The sharpest, finest-grain Vision3 stock on the premium
2393 print film, which gives deeper blacks, brighter highlights, and more saturation than 2383
— a crisp, high-impact daylight cinema look.
Tuning: `grain.blur 0.85` (50D is very fine), `halation 0.9`, `scanner.unsharpMask [0.9, 0.7]`
for the extra crispness this stock is known for.

### Vision3 200T — Tungsten Interior  (`vision3_200t_2383_interior`)
**Kodak Vision3 200T → 2383.** A low-grain tungsten-balanced interior look with a slightly warm
print balance, gentle highlight glow, and otherwise neutral exposure/contrast.

### Verita 200D — Warm Cinema  (`verita200d_2383_warmcine`)
**Kodak Verita 200D → 2383.** Kodak's new (2026) daylight cine negative with bold saturation,
warm skin, and a deliberately shorter, classically cinematic tonal range.
Tuning: `halation 1.2`, `dirCouplers.amount 1.05` (lift the saturation/edge the stock is built
around). Character notes follow Kodak's announcement materials, so this preset is a
best-estimate starting point.

---

## Low Light / Night

### Portra 800 — Natural Low Light  (`portra800_endura_natural`)
**Kodak Portra 800 → Supra Endura.** One of the last fast color negatives, carrying Portra's
natural warm palette into low light, with more grain and contrast than 400. Shot at box speed.
Tuning: `grain.blur 0.7` (grain more visible than the slower Portras), halation lifted
(`halationAmount 1.4`, `boostEv 0.7`, `halationStrength [0.08, 0.018, 0.0]`) for the warm
red/orange halo Portra throws around highlights at night. Supra Endura gives livelier color
than Portra Endura for after-dark scenes.

### Portra 800 +1 — Neon Night  (`portra800_push1_endura_night`)
**Kodak Portra 800 (Push +1, EI 1600) → Supra Endura.** Pushed one stop for available light:
punchier contrast, bolder grain, and a stronger red halo around neon and lamps.
Tuning: `densityCurveGamma 1.05` (push contrast), `grain.blur 0.6` + `agxParticleScale
[1.0, 1.2, 2.3]` (visibly larger grain), halation pushed further (`halationAmount 1.7`,
`scatterAmount 1.3`, `boostEv 1.0`, `halationStrength [0.1, 0.022, 0.0]`).

### Portra 800 +2 — Available Dark  (`portra800_push2_endura_available`)
**Kodak Portra 800 (Push +2, EI 3200) → Supra Endura.** Two stops pushed for the darkest
available light: heavy grain, high contrast, glowing highlights. The point is mood, not
fidelity.
Tuning: `densityCurveGamma 1.1` (highest contrast in the set), `grain.blur 0.5` +
`agxParticleScale [1.1, 1.3, 2.5]` (heavy grain), halation maxed (`halationAmount 1.9`,
`scatterAmount 1.4`, `boostEv 1.2`, `halationStrength [0.11, 0.025, 0.0]`).

### Vision3 500T — Halation Glow  (`vision3_500t_halation_glow`)
**Kodak Vision3 500T → 2383.** A deliberately stronger neon-night treatment than the base cinema
preset: warm scatter bloom, lifted print, larger grain, and high halation around point highlights.

---

## Nostalgic / Consumer

### Gold 200 — Golden Hour  (`gold200_ektacolor_goldenhour`)
**Kodak Gold 200 → Ektacolor Edge.** The nostalgic sunny-snapshot film: warm yellows and golds,
made for golden-hour everyday shooting. Consumer Ektacolor Edge paper completes the
minilab-print feel.
Tuning: `exposureCompensationEv 0.3` (Gold sings when bright and warm), neutral contrast,
`grain.blur 0.7`, `halation 1.0`.

### UltraMax 400 — Everyday Snapshot  (`ultramax400_ektacolor_snapshot`)
**Kodak UltraMax 400 → Ektacolor Edge.** The punchy, versatile consumer 400: vibrant color,
warm/orange highlights, slightly green shadows, and a pleasingly gritty grain.
Tuning: `densityCurveGamma 1.03` (consumer punch), `grain.blur 0.55` + `agxParticleScale
[1.0, 1.2, 2.3]` (chunky grain on purpose), `halation 1.1`.

### Superia X-TRA 400 — Cool Everyday  (`superia400_crystalarchive_cool`)
**Fujifilm Superia X-TRA 400 → Crystal Archive Type II.** Fuji's grainy consumer 400 with its
fourth color layer for cleaner mixed light; cooler than Kodak with the characteristic Fuji
green in the shadows.
Tuning: `grain.blur 0.55` + `agxParticleScale [1.0, 1.2, 2.3]` (chunky), neutral contrast,
`halation 1.0`. Crystal Archive keeps the cool Fuji palette.

### C200 — Crisp Everyday  (`c200_crystalarchive_budget`)
**Fujifilm C200 → Crystal Archive Type II.** The economical everyday negative: crisp, slightly
cool color with accurate skin — cleaner and finer-grained than the 400 consumer stocks.
Tuning: neutral contrast, `grain.blur 0.65` (finer than the 400s), `halation 0.9`.

---

## Creative

### Dreamy Pro-Mist — Portra 400  (`portra400_promist_dreamy`)
**Kodak Portra 400 → Portra Endura.** Activates the camera diffusion stage, spectral blur, soft
scanner sharpening, and a warm highlight bloom. The current asset does not author
`filterFamily`: a fresh editor therefore uses its Black Pro-Mist default, but a previously selected
Glimmerglass/CineBloom family survives the sparse overlay. The state-independent Black Pro-Mist
promise is tracked by [Make Dreamy Pro-Mist select Black Pro-Mist independently of prior editor state](https://github.com/thetechgeekko/Spektrafilm-android/issues/194).

### Faded Matte — Lifted Blacks  (`portra400_faded_matte`)
**Kodak Portra 400 → Portra Endura.** A flashed, low-contrast print with an explicit master tone
curve that lifts shadows and rolls off highlights. This is a creative grade, not a stock-neutral
reference.

### Bleach Bypass — High Contrast  (`ektar100_bleach_bypass`)
**Kodak Ektar 100 → Ultra Endura.** Uses reduced coupler influence, steeper film/print density,
black correction, and an S-curve for muted color and crushed silver-retention-style contrast.

### Wide-Gamut Glow — Portra 800  (`portra800_wide_gamut_glow`)
**Kodak Portra 800 → Supra Endura.** Selects Adobe RGB output with veiling glare, warm highlight
bloom, visible grain, and a wide-gamut print-oriented finish.

---

## Neutral

### Neutral (Adobe-like)  (`neutral_adobe_like`)
**Kodak Portra 400 → Portra Endura, film character minimised.** Not a film *look* but a clean,
Lightroom-default-style **starting point**: the full negative→print positive path with the
emulsion's personality dialled out, so you can build a look on top of a neutral base instead of
fighting an existing one. Derived from a reverse-engineering study of Lightroom's default render
(Adobe Color DCP + medium-contrast baseline) — see `docs/RESEARCH_LIGHTROOM_RENDER.md`.
Tuning: `io.scanFilm false` (print path) with **grain, halation, glare, and DIR couplers all
OFF**, `densityCurveGamma 1.0`, `autoExposure true`, and `exposureCompensationEv -0.2` to sit
slightly darker and track Adobe Color's neutral medium-contrast baseline. Use it as a base, then
dial in any film stock or look.

---

## Sources

- [Kodak Portra 400 — The Darkroom](https://thedarkroom.com/film/portra-400/)
- [Kodak Portra 400 Review — Moment](https://www.shopmoment.com/eu/articles/kodak-portra-400-review-the-film-stock-everyone-loves)
- [Kodak Portra 800 at Night — Adam Insights](https://www.adaminsights.com/shooting-kodak-portra-800-at-night-a-handheld-street-photography-review/)
- [Kodak Portra 800 — The Darkroom](https://thedarkroom.com/film/portra-800/)
- [Kodak Ektar 100 — The Darkroom](https://thedarkroom.com/film/ektar-100/)
- [Kodak Ektar 100 Review — Daydream Film](https://www.daydreamfilm.app/blog/film-reviews/kodak-ektar-100-review)
- [How to Shoot Fuji Pro 400H — Shoot It With Film](https://shootitwithfilm.com/how-to-shoot-fuji-pro-400h/)
- [Fujicolor Pro 400H Profile — Casual Photophile](https://casualphotophile.com/2018/11/28/fujifilm-fujicolor-pro-400h-film-profile/)
- [Velvia — Wikipedia](https://en.wikipedia.org/wiki/Velvia)
- [Fuji Velvia 100 Review — Blue Moon Camera Codex](https://bluemooncameracodex.com/film-fridays/ffvelvia100)
- [Classic Film Review: Kodachrome 64 — Alex Luyckx](http://www.alexluyckx.com/blog/2019/06/03/classic-film-review-kodachrome-64/)
- [Kodachrome 64: Remembering This Iconic Film — Paul Pope](https://paulpope.co.uk/kodachrome-64-remembering-this-iconic-35mm-colour-film/)
- [VISION3 500T 5219/7219 — Kodak](https://www.kodak.com/en/motion/product/camera-films/500t-5219-7219/)
- [Kodak Vision3 500T 5219 Review — Analog.Cafe](https://www.analog.cafe/r/kodak-vision-3-500t-52197219-film-review-kxxq)
- [VISION3 250D 5207/7207 — Kodak](https://www.kodak.com/en/motion/product/camera-films/250d-5207-7207/)
- [Kodak Vision3 5207 250D Review — Tahusa](https://tahusa.co/analog-film-review/kodak-vision-3-5207-250d/)
- [KODAK VISION Color Print Film 2383/3383 — Kodak](https://www.kodak.com/en/motion/product/post/print-films/vision-color-2383-3383/)
- [How to shoot Kodak Vision Color Print (2383) — 35mmc](https://www.35mmc.com/24/09/2024/how-to-shoot-kodak-vision-color-print-2383-in-colors/)
- [KODAK VISION Premier Color Print Film 2393 datasheet (PDF)](https://www.cinematography.net/Files/VISPREM.PDF)
- [VERITA 200D 5206/7206 — Kodak](https://www.kodak.com/en/motion/product/camera-films/verita-200d-5206-7206/)
- [Kodak Gold 200 vs UltraMax 400 — Max Kent](https://www.maxkent.co.uk/blogg/kodak-gold-200-vs-kodak-ultramax-400-whats-the-difference)
- [Fuji Superia 400 vs Kodak UltraMax 400 — Shoot It With Film](https://shootitwithfilm.com/fuji-superia-400-vs-kodak-ultramax-400/)
- [Fujicolor C200 datasheet — Fujifilm](https://asset.fujifilm.com/master/emea/files/2020-10/98c3d5087c253f51c132a5d46059f131/films_c200_datasheet_01.pdf)
- [Kodak Portra Endura vs Ultra vs Supra — Photrio](https://www.photrio.com/forum/threads/kodak-portra-endura-vs-ultra-vs-supra.122196/)
