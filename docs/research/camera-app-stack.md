# Camera-app stack — a verified dossier

**Status: collected information, not a plan.** The owner asked for the groundwork on a
possible future direction — a full camera app doing video with simulation and stills with
bokeh + film simulation — to be researched and kept on file. Nothing here is committed to,
scheduled, or started. Every claim below was checked against a primary source on
2026-08-28; the ones that still need a device are marked.

Companion to `docs/PERF_ROADMAP.md` (what shipped) and `docs/MOBILE_STRATEGY.md`.
**Note:** `docs/research/perf-lab.md` is cited throughout for measured numbers, but it
lives on branch `claude/perf-lab` (PR #156) and is not on `main` yet — read it with
`git show claude/perf-lab:docs/research/perf-lab.md` until that merges.

---

## 1. The central finding: the "framework" is a buffer type, not a library

The question was "which framework ties audio + video + image + depth + matting together
with zero copy". The premise is worth correcting, because it changes the shape of
everything else.

**On Android the zero-copy contract is `AHardwareBuffer`** — an opaque handle to memory
every relevant subsystem can already read and write without a copy:

| Consumer | How it takes an AHardwareBuffer |
|---|---|
| Vulkan | `VK_ANDROID_external_memory_android_hardware_buffer` |
| OpenGL ES | `eglGetNativeClientBufferANDROID` → `eglCreateImageKHR` |
| Camera2 / CameraX | produces AHardwareBuffer-backed images |
| MediaCodec | Surface input — GPU output encodes with no readback |
| LiteRT | GPU delegate buffer interop |
| Another process | Parcelable via Binder; the memory is shared, not copied |

So "the framework" is a **type plus a discipline**: allocate once, pass the handle, never
`memcpy`. A library that owns the pipeline (MediaPipe, GStreamer, a custom EDSL) does not
give zero copy — AHardwareBuffer does — and adopting one costs the freedom to keep our own
engine's structure.

**We already speak half of this contract.** `gpu/vulkan_compute.cpp` is a persistent
Vulkan compute host with pipelines, descriptors and command buffers created once and
persistently mapped buffers. Moving it from host-visible staging to imported
AHardwareBuffers is an evolution of code that exists, not a rewrite.

## 2. Verified component table

Everything checked for licence, owner and live maintenance. Our app is GPLv3, so
Apache-2.0 / MIT / BSD are all inbound-compatible; AGPL is **not**.

| Component | Owner | Licence | Alive? | Role |
|---|---|---|---|---|
| **AHardwareBuffer** | Google (NDK) | platform | yes | the zero-copy currency |
| **Vulkan compute** | Khronos | platform | yes | compute hub — *already in tree* |
| **CameraX / Camera2** | Google | Apache-2.0 | yes | capture |
| **MediaCodec** | Google (platform) | platform | yes | H.264/HEVC/AV1 encode, Surface input |
| **Oboe** | Google | Apache-2.0 | yes | audio I/O (AAudio / OpenSL ES) |
| **LiteRT** | Google | Apache-2.0 | yes (6–8 wk cadence) | ML runtime; GPU + NPU (Qualcomm, Tensor, MediaTek) |
| **ML Kit** segmentation | Google | proprietary SDK | yes | selfie + subject segmentation |
| **MediaPipe** | Google | Apache-2.0 | yes (v0.10.36+) | ML tasks; *see §4 on the framework half* |
| **Highway** | Google | Apache-2.0 | yes | CPU SIMD — *already vendored* |
| **Halide** | Halide org (Google contributes) | MIT | yes | AOT codegen — *benchmarked, PR #156* |
| **Filament** | Google | Apache-2.0 | yes | PBR renderer, if display gets ambitious |
| **Aire** | awxkee | MIT | yes | C++ filters on Highway; polygonal bokeh kernel |
| **jxl-coder / avif-coder** | awxkee | Apache-2.0 / BSD-3 | yes | JPEG XL / AVIF export |

TensorFlow Lite is in maintenance mode — **LiteRT is the successor**, and it is what new
work should target.

## 3. The hard number nobody should plan around without seeing

Video *with the spectral engine per frame* is not a scheduling problem, it is a
two-to-three-orders-of-magnitude problem.

From the on-device validation round (#146/#147, S26 Ultra): a preview at `preview_max_size
= 640` took **562 ms**. That is roughly 0.27 MP.

| Target | Pixels | Linear extrapolation | Budget at 30 fps | Gap |
|---|---|---|---|---|
| 1080p | 2.07 MP | ≈ 4.3 s/frame | 33 ms | ~130× |
| 4K UHD | 8.3 MP | ≈ 17 s/frame | 33 ms | ~500× |

Linear extrapolation from one device datum, so treat the exponent, not the digits. Even a
generous full-chain GPU win (say 50×) does not close it. **The spectral engine will not run
per video frame, on any hardware we can buy.**

### The answer is already in the tree

`spk_bake_cube_lut` bakes the current look to a 33³ `.cube`; `LutGpuPreview.kt` samples one
on the GPU; `test_bake_lut` gates it inside the 38-test parity suite. That is the correct
video architecture and it already exists and is verified:

- **Stills** → the full spectral engine (parity-gated, exact).
- **Video** → bake the look to a 3D LUT **once per look change**, apply per frame on the
  GPU. Per-frame cost becomes one texture fetch, independent of how expensive the look was
  to derive.

The honest limitation: a 3D LUT is a *pointwise* transform. It cannot carry grain,
halation, glare, or the diffusion filter, all of which are spatial. Video would get the
colour of the film simulation and not its texture, unless those are re-implemented as
separate real-time GPU effects. That is a product decision, and it should be made with
open eyes rather than discovered late.

## 4. Where MediaPipe fits, and where it does not

MediaPipe is alive and Apache-2.0, and it has two halves that deserve different answers.

**MediaPipe Tasks** (segmentation, detection, audio classification) — a fine source of
models and pre/post-processing. Worth using.

**MediaPipe Framework** (the C++ calculator graph) — this is the part that looks like "the
framework that ties everything together", and it is the part to be careful with. It wants
to *own* the pipeline: its calculators, its scheduler, its packet timestamps. Our engine is
a parity-gated C++ pipeline with its own deterministic fork-join (`kernels/parallel`, which
is byte-identical across thread counts *by construction* — a parity requirement) and its
own memo layer. Re-expressing that as calculators would mean giving up the thing the whole
project is built on, to buy scheduling and synchronisation that Vulkan semaphores plus the
existing fork-join already provide.

Same reasoning that retired oneTBB in `PERF_ROADMAP`: a dependency with no parity-safe win.

## 5. Audio is a separate machine, and that is correct

Audio is sample-clocked — 48 kHz, buffers of a few milliseconds, a hard real-time callback
that must never allocate or block. Video is frame-clocked at 33 ms. They share no data and
no timing discipline.

**Oboe** (Google, Apache-2.0, C++) is the answer and it stays on its own thread with its own
buffers. Any proposal to unify audio and image processing under one EDSL is a category
error — it is what makes Faust and CSL irrelevant here despite both being excellent at what
they do.

## 6. ML models — sizes and licences

| Task | Model | Size | Speed (S23-class) | Licence |
|---|---|---|---|---|
| Segmentation | **ML Kit Selfie / Subject** | **0 MB in APK** (Play Services) | — | Google SDK terms |
| Segmentation | MediaPipe Image Segmenter | few MB | real-time | Apache-2.0 |
| Depth | **MiDaS-V2 quantized** | **16.6 MB** | **1.1 ms** | check per-export |
| Depth | MiDaS-V2 float | 63.2 MB | 3.2 ms | " |
| Depth | Depth Anything | — | 166 ms | " |
| Matting | **MODNet** | **~7 MB** | real-time | **Apache-2.0** (code *and* models) |
| Matting | Robust Video Matting | — | real-time | **GPL-3.0** |

Notes that matter:

- The `play-services-mlkit-*` variants are delivered by Google Play Services, so they cost
  **nothing** in the APK. ImageToolbox uses exactly these
  (`com.google.mlkit:segmentation-selfie`, `play-services-mlkit-subject-segmentation`)
  alongside ONNX Runtime for its FOSS build, which is the pattern to copy.
- **MODNet is Apache-2.0 for the models too**, which is unusually permissive for a matting
  network and makes it the first candidate. RVM is GPL-3.0 — compatible with us, but it
  would bind any future relicensing.
- MiDaS-V2 quantized at 16.6 MB and ~1 ms is the striking one: depth good enough for a bokeh
  falloff is nearly free. Verify the exact export's licence before shipping.

**Trimap matting specifically:** MODNet is *trimap-free* by design (that is its paper's
contribution). If a genuine trimap workflow is wanted — user paints foreground / background /
unknown — that is closer to our existing mask compositor than to an ML model, and
`docs/MASKING_SPEC.md` is the place to start rather than a network.

## 7. The 200 MB budget, grounded

Google Play caps an app bundle's **base module at 200 MB compressed download**. Play Feature
Delivery and Play Asset Delivery let you exceed that with on-demand modules.

Measured today:

| Item | Size |
|---|---|
| Current R8 release APK | **15.9 MB** |
| — of which engine assets | 13 MB (LUTs 5.8, profiles 5.6, ICC 0.9) |
| Debug APK (3 ABIs, unminified) | 25.4 MB |

A plausible full build:

| Component | Base APK cost |
|---|---|
| Today's app | 15.9 MB |
| Oboe (static) | ~1 MB |
| CameraX | ~2 MB |
| MediaCodec / AHardwareBuffer | 0 (platform) |
| ML Kit segmentation | **0** (Play Services) |
| LiteRT runtime + GPU delegate | ~3–5 MB |
| MODNet matting | 7 MB, or **0** via Play Asset Delivery |
| MiDaS depth (quantized) | 16.6 MB, or **0** via Play Asset Delivery |
| **Base total** | **~30–48 MB** |

**The 200 MB limit is not the binding constraint** — we would sit at roughly a quarter of
it even bundling every model. If it ever does bind, Play Asset Delivery moves the models out
of the base module entirely. Worth stating plainly so the budget does not distort design
decisions it should not.

## 8. What earlier candidate lists got wrong

Recorded so the same names do not come back around:

| Suggested | Reality |
|---|---|
| Gratepipe | **No such project.** No repo, paper, or package anywhere. |
| Aura (`sschaetz/aura`) | Last commit **2018-05-23**. |
| Video++ (`matt-42/vpp`) | MIT, but last commit **2019-02-07**; and its own docs say it adds no explicit SIMD, relying on compiler auto-vectorisation. |
| PolyMage | Real research (SIGPLAN'15) but a **Python-embedded DSL**, not a C++ EDSL; artifacts from 2016/2018. |
| Faust, CSL | Real and excellent — for **audio**. See §5. |
| RapidRAW's bokeh | **AGPL-3.0**, Rust, WGSL, AI-model-based. AGPL cannot enter a GPLv3 project. |

And the deeper point, from our own measurements in `docs/research/perf-lab.md`: an EDSL's
entire pitch is better loop scheduling. Blocking and tiling the spectral integral — exactly
what a polyhedral compiler does automatically — measured **1.04–1.07×**, because the tables
stay in L1 and there is nothing to recover. Worse, PolyMage-style automatic scheduling wins
by *reassociating reductions*, which is precisely what our thread-invariance gate forbids on
the default path.

## 9. What to verify before any of this becomes a plan

- [ ] The perf-lab device numbers (`tools/perf_lab/build_push_run.sh`, PR #156) — they set
      what the CPU path is actually worth.
- [ ] A camera→Vulkan AHardwareBuffer import round-trip on the S26 Ultra: does the driver
      accept our formats without a blit?
- [ ] A baked-LUT video loop at 1080p30 and 4K30 — measured, not extrapolated from §3.
- [ ] Whether grain and halation can be re-cast as real-time GPU effects, or whether video
      simply ships without them.
- [ ] Licence of the specific MiDaS export chosen (the HF repos vary).
- [ ] Whether `spk_bake_cube_lut`'s 33³ grid is fine enough for the look, or whether video
      needs 65³.

## 10. Verification round 2 — a second candidate architecture, checked

A second document was put forward proposing a **multi-DSL native engine** (Halide for
CPU image pipelines, Slang for Vulkan kernels, ISPC for irregular CPU SIMD, MNN for
neural work, Faust/KFR for audio, OpenCV G-API for orchestration, FFmpeg for media).
Nineteen recommendations across it were each checked against primary sources and then
adversarially challenged against this repo's actual constraints.

**Result: two survived** — Vulkan compute + SPIR-V (which we already run) and LiteRT,
and the latter only in the confined mask shape the tree already stakes out.

The document deserves credit the previous list did not: **every project in it is real,
alive, and GPLv3-inbound-compatible.** No fabrications, nothing dead. Verified
last-commit dates clustered on 2026-08-19 … 2026-08-28. The rejections are therefore
about *fit*, not about health or licence — a much better class of disagreement.

### What the checks found that the document did not say

| Technology | Finding |
|---|---|
| **Tiramisu** | Emits **no ARM code at all**. `gen_halide_obj()` hardcodes `{AVX, SSE41, LargeBuffers}` at two source sites; zero `neon`/`aarch64`/`android` matches in `src/` or `include/`. It is also a *Halide front end*, so adopting it means Halide **plus** a polyhedral layer. Its 2026 revival is specifically on the **autoscheduler** — the component least compatible with a byte-identity gate. |
| **HIPAcc** | x86-only: the sole CPU backend is literally `lib/Backend/CPU_x86.cpp`; word-boundary NEON count across `lib/` and `include/` is **zero**. Default branch last moved **2021-03-20**. Its one Android story was RenderScript, which its own maintainers moved off the default branch *and* Google deprecated. |
| **ArrayFire** | The document says "not my recommendation for the main Android runtime", which implies it is an option one could choose. **There is no Android build** — no Android strings in the tree, not listed as a supported platform. The correct statement is "cannot be built for Android". |
| **KFR** | A licence trap the document navigated and a file-only audit would fail. Root `LICENSE.txt` is the bare **GPL Version 2, June 1991** text — read literally, GPL-2.0-**only**, which is *incompatible* with GPLv3. The README says GPLv2**+**. The `+` is the whole ballgame, and the document flagged it correctly. |
| **ISPC** | Android/NEON support is **real**, not doc-only — genuine `fmla`/`fmul` on `vNN.4s` for all three of our ABIs. Rejecting it on portability would have been wrong; the sound reason is that SPMD vectorisation reassociates reductions, which our gate forbids, and Highway already occupies the slot *with* order-preserving lanes. |

### Where this document is right and §8 above was wrong

One correction to our own record, and it matters.

§8 dismissed the EDSL category on the strength of the 1.04–1.07× tiling measurement.
That is a fair rebuttal of the **scheduling** pitch and a **non-sequitur against the
portability pitch** — and the two got conflated. `docs/research/simd-halide-experiment.md`
makes the portability argument explicitly ("write filming/printing/scan *once* and get
CPU + GPU, instead of hand-writing GLSL plus maintaining a parallel CPU
implementation") and calls Halide-Vulkan "a **maintainability** argument first, a speed
one only if measured to be". This document picked that up; §8 did not.

It bites on the one large unbuilt thing we have: **#148's filming shader** (spectral
upsampling → camera raw → density curves → DIR couplers). Written by hand that is two
implementations of the same physics, kept inside 1e-4 of each other forever. Written
once in Halide it is one. That is the strongest argument for Halide in this project and
neither of our documents was making it.

Two caveats stand: Halide-Vulkan would be **f32-only** on this fleet
(`gpu-device-probe.md` Tier 0: `shaderFloat64 = false` on the Adreno 840), and Halide's
own docs file Vulkan performance tuning under Known TODO.

### Where the document's architecture is wrong

Its split is "Halide for full-resolution capture, Slang/Vulkan for real-time preview".
That contains a hidden premise — that the *same* pipeline runs in both places and only
resolution and schedule differ. §3 above says that premise is false by two to three
orders of magnitude, so the split is not a choice between two options; it is two
implementations of something that cannot run either way.

The architecture has **no third box** for *precompute the look, so the per-frame cost
becomes a texture fetch independent of how expensive the look was to derive*. That is a
structural omission, not a disagreement about numbers, and `spk_bake_cube_lut` +
`test_bake_lut` already fill it.

Worse for the Slang half specifically: the preview kernel it would target **has already
been offloaded and it did not help** — GPU M1 produced no measurable preview speedup
because grain and halation dominate. `gpu-device-probe.md` Tier 2 shows why small frames
cannot benefit: 0.3 MP warm-call median 48.3 ms vs 158.3 ms at 12 MP, almost entirely
fixed overhead.

### The smallest subset worth keeping

1. **Halide, aimed at Vulkan codegen rather than CPU tiling** — and only once #148 is
   actually sized. The value is one source for CPU + GPU, not speed.
2. **A GPU effects layer for the LUT residual** — grain, halation and glare composited
   over a baked cube, which is the honest answer to §3's pointwise limitation. It needs
   no new language: two more `.comp` files through the existing `vulkan_compute.cpp`.

Everything else is dropped, and each for a specific reason rather than a general
suspicion: ISPC (Highway holds the slot with the property ISPC gives up), Slang (two
shaders totalling ~70 lines do not justify a compiler dependency — revisit past roughly
ten), MNN (the LiteRT seam is built and has a 0 MB fallback), Faust/KFR (no audio in the
product), G-API (the third refusal of the same shape as oneTBB and the MediaPipe
calculator graph — `kernels/parallel`'s thread-count invariance is a gate), FFmpeg
(MediaCodec is platform and free, and three purpose-built writers already ship).

### The cost nobody costed

The `engine-parity` job calls `build_run` **38 times**, and each call recompiles the
whole engine with a single `g++` over a glob — 78 source files, 16,908 lines, 38 times
per run, no shared object cache. **Any file a new toolchain adds is paid for 38 times.**
A *codegen* toolchain is worse than linear: its generator must run before that `g++`
line, which today has no build system at all. Adding one AOT layer means converting the
parity job from "g++ a glob" into a real build graph — and that job is this project's
prime directive made executable.

Today a contributor needs three pinned SDK components and a host `g++` to run the gate.
Under the proposed architecture they would also need Halide, ISPC and a Slang compiler.
Raising the cost of running the gate raises the odds of an ungated engine change, which
is this project's specific failure mode.

### One open question this raised

Our two SIMD/codegen efforts both landed on *regular, dense, order-fixed* work — Highway
on the f32 FIR and f64 IIR, Halide on the ks×ks PSF convolution. The genuinely
**irregular** kernels (the Poisson-binomial samplers in `kernels/stats.cpp`, the
DIR-coupler develop loop) got neither; they received plain `parallel_for` in S4/S7. The
answer is still not ISPC — grain's fixed-block seeding is exactly what makes 12 MP
1-vs-8-worker outputs `memcmp`-identical. But *have we ever profiled the irregular
kernels as a class?* is asked nowhere in our documentation, and it should be.

## 11. Honest scope note

The current app is a stills RAW editor whose value is bit-exact parity with an oracle. A
camera app with video, audio, depth, matting and real-time simulation is a different
product with a different core constraint — latency instead of exactness — and it would carry
the existing engine as one component rather than being an extension of it.

That is not an argument against it. It is an argument for deciding it deliberately, with the
§3 number visible, rather than arriving at it one feature at a time.

*Film modeling powered by spektrafilm (GPLv3).*
