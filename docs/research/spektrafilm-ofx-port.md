# spektrafilm OFX to Android port record

Status: first Vulkan orchestration slice in progress under tickets
[#148](https://github.com/thetechgeekko/Spektrafilm-android/issues/148) and
[#149](https://github.com/thetechgeekko/Spektrafilm-android/issues/149).

## Source pin and license

- Repository: https://github.com/chaert-s/spektrafilm-ofx
- Pinned commit: `86476afc5b077de77e2278e3658d1ba9309892a1`
- Commit date and author: 2026-07-06, Aedan Oskar Otto Diez
- Source license: GPL-3.0-only ([pinned license](https://github.com/chaert-s/spektrafilm-ofx/blob/86476afc5b077de77e2278e3658d1ba9309892a1/LICENSE.txt))
- Pin verified against public `main`: 2026-08-31

The Android application is already GPL-3.0-only. GPL therefore permits source adaptation when
copyright/license notices are preserved, modified files are marked, and corresponding source is
provided. The author's separate chat permission should be archived as project correspondence,
but the public GPL grant is the source-code permission used here.

## Adaptation map

| Upstream source | Android destination | Mode | Android change |
|---|---|---|---|
| `src/SpektraVulkanRenderer.cpp` | `engine/spektra-core/src/main/cpp/gpu/vulkan_compute.cpp` | orchestration/resource-lifetime adaptation | Retain the small Android Vulkan interface while adapting persistent device-resident ping-pong buffers, static-table caching, chained dispatch/barrier discipline, bounded allocation and fail-closed CPU fallback. |
| `src/SpektraVulkanRenderer.h` | `engine/spektra-core/src/main/cpp/gpu/vulkan_compute.h` | interface concepts only | Expose only the Android pointwise-chain seam and diagnostics needed by runtime gating; no OpenFX types cross the seam. |
| `tools/SpektraVulkanCopyHarness.cpp` | Android native/device GPU tests | test-structure reference | Reuse the independent upload/dispatch/readback validation pattern, not desktop host integration. |

The exact introducing commits are recoverable from this file and the destination files with
`git log --follow`; every later adapted source path must be added to this table in the same change.

## Numeric authority and first slice

The first slice promotes the Android repository's already measured shaders:

- `tools/gpu_probe/filming.comp`;
- `tools/gpu_probe/printing.comp`; and
- `engine/spektra-core/src/main/cpp/gpu/scan_spectral.comp`.

Those shaders were built from this engine's current profile/parameter folds and passed the
connected-device M2 oracle-tolerance and repeat-determinism probes. No OFX shader math is copied
in the first slice. OFX contributes the resident-DAG orchestration pattern: one input upload, a
shared device-resident pointwise chain with explicit compute barriers, cached static f32 tables,
and one final readback. The existing Strict Exact CPU route remains unchanged and authoritative;
Fast GPU is opt-in, device-self-tested, tolerance-bounded, and fails closed to CPU.

## LUT audit: 65 is an export choice, not the engine's universal grid

There are four different tables in play and their sizes must not be conflated:

| Purpose | Android behavior | Pinned OFX behavior | Port decision |
|---|---|---|---|
| User `.cube`/CLF export | UI offers `17^3`, `33^3`, and `65^3`; the default selection is `33^3`. The exact pointwise film/print pipeline evaluates every lattice point. | UI offers `33^3` and `65^3`; the default selection is `65^3`. It renders a `N^2 x N` identity lattice through the normal renderer after disabling non-pointwise effects. | Keep Android's three explicit choices. Do not silently force every export to 65; label 65 as highest-quality/largest/slowest. |
| Interactive GLES LUT preview | Always bakes a shaped `33^3` lattice and samples it trilinearly. | Not the OFX renderer's primary interactive path. | Temporary preview fallback only; the resident Vulkan DAG should replace it stage by stage. |
| Scanner/enlarger spectral acceleration | Opt-in, default `17^3`, PCHIP-interpolated, cached, and intentionally approximate; Strict Exact bypasses it. | The full Vulkan renderer evaluates its native shader pipeline rather than using this Android PCHIP accelerator. | Never use this approximate LUT on Strict Exact. Keep it only behind the Fast/tolerance gate until the resident shaders supersede it. |
| Color transfer functions | Analytic/native paths plus the spectral-upsample table `192 x 192 x 81` stored as finite f16 and expanded for computation. | Precomputed 1D decode/encode tables contain 4096 samples per color space. | These are not 3D creative LUTs. Evaluate a future 4096-entry transfer-table port separately against the local oracle before adoption. |

Both projects correctly omit auto exposure and spatial/stochastic operations from exported 3D
LUTs: a pointwise RGB cube cannot encode image-wide metering, grain, halation, diffusion, glare,
geometry, or scanner sharpening. The Android bake keeps spectral upsampling, density curves,
pointwise DIR couplers, printing, scanning and output conversion, emits blue-fastest `.cube`
ordering, and uses the regular engine route. The OFX exporter follows the same high-level method,
but its GPL implementation is only a design reference; no exported OFX LUT data is copied.

Later spatial stages may adapt individual GPL shader techniques only after their Android stage
semantics, halo/tiling law, NaN behavior, deterministic seed law, memory budget and CPU-oracle
error are independently gated. A wholesale renderer transplant is not permitted by this plan.

## Explicit exclusions

Do not copy or derive from:

- official OFX binary archives or any resources absent from the pinned public source tree;
- `.cube` or other LUTs exported by spektrafilm OFX, including re-gridded derivatives;
- SMPTE ST 2065-2 licensed CSV data absent from the public repository;
- OFX icons, branding, names used to imply endorsement, packaged manuals or screenshots;
- Metal/MPS code, OpenFX host/UI glue, desktop packaging scripts, or duplicate profiles/LUT assets.

The official-binary notice and exported-LUT license are separate from the GPL source grant:

- [official binary notice](https://github.com/chaert-s/spektrafilm-ofx/blob/86476afc5b077de77e2278e3658d1ba9309892a1/Legal/SPEKTRAFILM_OFX_LICENSE.txt)
- [exported LUT license](https://github.com/chaert-s/spektrafilm-ofx/blob/86476afc5b077de77e2278e3658d1ba9309892a1/Legal/SPEKTRAFILM_OFX_LUT_LICENSE.txt)

## Required gates per slice

1. RED test at the public native render seam, followed by the minimum implementation to pass.
2. CPU path and committed oracle corpus unchanged; 1/2/4/8-worker CPU digests remain stable.
3. Same-device GPU output within `max_abs <= 1e-4` and `rms <= 1e-5`, with at least 100 repeat digests before export enablement.
4. Capability verdict keyed by device, driver, OS and shader hashes; any mismatch re-runs the self-test.
5. One upload/one readback evidence and counters proving no hidden inter-stage host round-trip.
6. Bounded allocations, cancellation/watchdog, kill switch, and forced CPU-fallback tests.
7. Release arm64 benchmark with cold/warm p50/p95, thermals and memory; no speed claim from host-only timings.
8. Independent code, license/provenance and device-evidence review before the ticket closes.
