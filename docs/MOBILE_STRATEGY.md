# Mobile editing strategy — what we learned from Lightroom mobile

Status: mobile product strategy, reconciled 2026-08-31. The current exactness and GPU policy lives
in [BIT_IDENTICAL_EXPORT_ROADMAP.md](BIT_IDENTICAL_EXPORT_ROADMAP.md); live feature coverage is
owned by the Wayfinder graph linked from [EXECUTION_INDEX.md](EXECUTION_INDEX.md).

Decision input for the question *"are we porting the true system, and must the app do everything
the spektrafilm GUI does?"* — answer: **yes**, and here is how we make that smart on a phone,
informed by how Adobe Lightroom mobile (and other modern mobile RAW editors) actually work.

## The two anchoring facts

1. **The spektrafilm GUI is a thin shell over the engine.** Verified in source: `spektrafilm_gui`
   imports `init_params` / `digest_params` / `RuntimePhotoParams` / `load_profile` and only
   contains widgets + a `params_mapper` that turns slider state into a `RuntimePhotoParams` and
   calls `simulate`. **No processing math lives in the GUI.** Therefore "do everything the GUI
   does" == "port the engine faithfully + expose the reviewed `RuntimePhotoParams` surface." Our
   Kotlin/C++ parameter bridge covers the pinned port baseline, but latest-upstream parity and any
   inert or unavailable controls require an explicit parity manifest and tickets; this strategy does
   not claim permanent one-to-one coverage.

2. **The parity bar is explicit.** We gate the C++ port against the pinned Python engine
   stage-by-stage (`tools/parity`) to `max_abs <= 1e-4` and `rms <= 1e-5`, plus byte identity across
   worker counts for the same build. That is a faithful numeric contract, not a promise of
   cross-build, cross-ABI, CPU/GPU, or whole-file byte identity.

## What Lightroom mobile does, and what we copy

| Lightroom behavior | Source | Our decision |
|--------------------|--------|--------------|
| **Smart Previews**: edits run on a lossy DNG proxy (long edge 2560 px, ~2% size); **export re-renders from the full-res original**. | [Adobe: Smart Previews](https://helpx.adobe.com/lightroom-classic/help/lightroom-smart-previews.html) | Adopt the proxy/preview model — which is *exactly* spektrafilm's `preview` vs `scan` split (`settings.preview_max_size`). Interactive sliders run on a downscaled **linear** proxy; the full pipeline runs at full resolution only on export. Raise the default proxy size toward a "smart-preview"-like long edge (≈1280–2560) for quality, configurable. |
| **Non-destructive**: every edit is a recipe/sidecar (XMP); the original file is never modified; edits re-applied on view/export. | [Adobe non-destructive editing](https://lifeafterphotoshop.com/non-destructive-editing-and-how-it-works/) | Add a **non-destructive recipe layer**: the edit is a serialized `SpektraParams` stored as a sidecar keyed to the source RAW; the original is untouched; re-render on demand. Enables presets (the 28 stocks + saved params), history, and "extract preset" like Lightroom. |
| **GPU-accelerated editing** for slider responsiveness; same pipeline, faster. | [ACR GPU FAQ](https://helpx.adobe.com/camera-raw/kb/acr-gpu-faq.html) | Keep **Strict Exact CPU** as the parity-bearing fallback and develop **Fast GPU** as a separately named, capability-gated Vulkan route. The current product route covers eligible pointwise filming → printing → scan with one upload/readback; spatial and stochastic effects remain open. It is oracle-tolerance/same-device work, never universal CPU-byte identity. The older default-off GLES LUT loupe remains a different preview approximation. |
| Imports RAW/DNG/JPEG/TIFF; exports JPEG/DNG/TIFF — **no EXR**. | Lightroom format support | Be photo-app pragmatic: ingest qualified RAW/DNG through patched LibRaw plus supported platform images; export JPEG, PNG8/16, TIFF16/32F, scene-linear TIFF32F, and the separately gated Ultra HDR mode. The Android/native boundary is linear float32; sensor bit depth, internal arithmetic, HDR encoding, and file depth remain distinct contracts. |

Reference open-source corroboration: [RapidRAW](https://github.com/CyberTimon/RapidRAW) — a modern
non-destructive, GPU-accelerated RAW editor (WGPU/WGSL) — confirms the "offload pipeline to GPU
for a fluid UI, keep edits non-destructive" pattern.

## The decisive architectural conclusion: Strict CPU and Fast GPU are different products

Bit-exact parity is required, and **GPU floating-point results are not bit-reproducible across
vendors** (per-architecture precision differs; see the GPU precision discussion in the research).
Therefore:

- **Strict Exact CPU is the parity-bearing route and universal fallback.** `tools/parity` gates the
  adopted CPU arithmetic against the oracle and worker-count invariance contract.
- **Fast GPU is an optional product route**, not the strict route. Its current resident Vulkan
  pointwise chain is functionally verified on the frozen Android 16/Adreno artifacts, but spatial
  effects, device qualification breadth, startup capability persistence, and release SLO evidence
  remain open. A failing or unsupported capability gate falls back to Strict Exact CPU.
- **The GLES LUT loupe is still only a preview approximation.** It samples a baked 3D LUT and cannot
  stand in for the direct spectral graph or its grain/halation stages. Do not conflate that older UI
  experiment with the resident Vulkan compute route.

## Scope verdict for "everything"

**In scope for the pinned port baseline (under the declared parity contract):** spectral upsampling (Hanatos2025 + LUT binary),
all three stages (filming / printing-with-enlarger-dichroics / scanning), DIR couplers, grain
(Poisson-binomial), halation + in-emulsion scatter + diffusion filters, **FFT-based diffusion**
(needed for exact large-radius parity — *no longer trimmed*), spectral-LUT acceleration
(`use_enlarger_lut` / `use_scanner_lut`, with the exact non-LUT path as the parity gate), all
`RuntimePhotoParams` controls, debug taps, and `scan_film` (view-negative). Rendered output offers
sRGB, Adobe RGB, ProPhoto RGB, Rec. 2020, ACES2065-1, and linear sRGB. PNG16 and rendered TIFF embed
the selected ICC. Bitmap JPEG/PNG8 tags supported spaces only on API 26+; API 24–25 falls back to a
plain sRGB bitmap and the 8-bit ACES path is untagged. Scene-linear TIFF32F bypasses simulation and
is deliberately untagged in the decoded input primaries.

**Smart additions (Lightroom-informed):** non-destructive recipe/sidecar + presets/history;
proxy-preview vs full-res-export; device color management for on-screen.

**Deferred (not capability we lose, just niche file formats / later optimization):** EXR;
completion and fleet qualification of the remaining Fast GPU spatial/stochastic graph; and
`lensfunpy` lens correction (unused upstream). TIFF32F and scene-linear TIFF32F already ship via
`:lib:tiffwriter`.

**Dropped (not part of the engine):** the napari/Qt GUI (reimplemented in Compose),
`plotting.py`, and unused upstream modules (`parametric`, `stocks`, `calibration_targets`).
