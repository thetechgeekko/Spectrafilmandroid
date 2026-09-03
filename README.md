# Spektrafilm for Android

> **Development status:** the current tree has active post-v0.9.0 implementation work and remains
> under a production-release hold. Public positioning relative to the owner's Latent camera project
> is an unresolved human decision in
> [Make README and project status truthful for the next release](https://github.com/thetechgeekko/Spektrafilm-android/issues/144).
> Live engineering work and known gates are in the [execution index](docs/EXECUTION_INDEX.md).

<details>
<summary>Earlier owner status note (retained verbatim pending that decision)</summary>

Hello fellow geeks this app was a concept to do Spectral simulation BUT getting it perfect took a long time even after using Claude Max plan and running multiple agents to complete the app. So basically its a AI slop (And i am sorry for that). Even after getting good results from the app i was not satisfied. As i wanted a app which actually takes raw and processes it at extreme super fast speed just like taking a HDR+ photo in GCAM. As soon as i exported one photo from this app i was confident that i could do better and relentlessly worked on a different app which can take straight film emulated high images with a swipe to change presets, and have a one another way of taking the picture the HDR way not like Google's exposure fusion which gives pop effect to a photo. Researching day and night , but no luck . First i tried taking raw capture straight from sensor just like how motioncam does, yes, it is very hard until you read AOSP and everything was laid out there but you have to add your own tricks like using multiple workers to process the raw stream, package the raw buffer, merge bracketed buffer, get depth map , do film emulation and export the image as jpeg. I also tried taking raw frames+pcm audio+gyro data and convert it with film emulation Its still under WIP as there are so many bugs and i do not want to use AI for this as this is one of the projects that I wanted to do since 2016. So, therefore from this moment this app will be superseded by "Latent" a Film emulation Camera app. If i had any free time i will update the app and make it faster if someone needs it.

</details>

Spectral film simulation on your phone — a native port of the
[spektrafilm](https://github.com/andreavolpato/spektrafilm) engine, with a Jetpack Compose editor.

*Film modeling powered by [spektrafilm](https://github.com/andreavolpato/spektrafilm). Dedicated to
the [pixls.us](https://pixls.us) community.*

<table>
  <tr>
    <td><img src="docs/screenshots/editor.jpg" width="200" alt="Editor"></td>
    <td><img src="docs/screenshots/presets.jpg" width="200" alt="Presets"></td>
    <td><img src="docs/screenshots/masks.jpg" width="200" alt="Masks"></td>
    <td><img src="docs/screenshots/tone-curve.jpg" width="200" alt="Tone curve"></td>
  </tr>
  <tr>
    <td align="center"><sub>Editor</sub></td>
    <td align="center"><sub>Presets</sub></td>
    <td align="center"><sub>Local masks</sub></td>
    <td align="center"><sub>Tone curve</sub></td>
  </tr>
</table>

## What it is

Most "film looks" are a color LUT — a lookup table that nudges your pixels toward a mood. This is not
that. Spektrafilm runs your photo through a physically-based simulation of the actual analog process:
it reconstructs a spectrum for each pixel, exposes a virtual emulsion that has real spectral
sensitivities, develops the dyes through the film's measured density curves, prints that negative
through a virtual enlarger onto paper, and scans the result. Negative, enlarger, print, scan — the
whole chain, the way it really happens.

The engine is a parity-first C++ port of Andrea Volpato's research project. Its stages are checked
against the pinned Python oracle within the declared numeric tolerance, with byte-identical output
across worker counts for the same build. That does not imply identical bytes across CPU
architectures, compiler builds, CPU and GPU routes, or encoded containers. The science, film-stock
measurements, and spectral data are upstream work; this project brings them to Android with an
editor that should feel familiar if you've used Lightroom.

## What you can do with it

**Choose a film and a paper.** 28 film and paper profiles — color negative, slide, motion-picture,
print film, and RGB papers — listed by friendly name and grouped by category, each with its ISO,
color balance, and era. The print path works for any film/paper pairing, not just preset
combinations.

**Start from a look, then make it yours.** 28 built-in presets cover researched film-and-print
combinations. You can save your own, and import or export them to share.

**Tune the film pipeline.** The current pinned port exposes exposure and auto-metering (7 patterns),
DIR couplers, halation and in-emulsion scatter, diffusion filters, the enlarger's dichroic filters
and print exposure, grain (a stochastic particle model with sublayers and micro-structure), and the
scanner. Latest-upstream coverage and any inert or unavailable controls are tracked explicitly; the
README does not treat an older parity baseline as permanent full-feature parity.

**Edit like a photographer.** A tone curve (master plus per-channel red/green/blue), contrast,
saturation and vibrance, and local masks — radial, gradient, and luminance/color range with
eyedroppers — that adjust exposure, color, clarity, texture, sharpness and tone in just one
part of the frame. Local edits sit on top of the film render; the
simulation underneath stays untouched.

**Get white balance right.** An eyedropper sets neutral from a tap, warmth and tint work on any
photo, and "balance to film stock" warms the input to a tungsten stock's reference light — the
digital equivalent of an 85 filter — so tungsten film doesn't render a daylight scene blue.

**Bring in RAW, send out real files.** Supported RAW and DNG files use the pinned, patched LibRaw
decoder; unsupported compressed inputs may use Android's display-referred platform fallback and
are not claimed to be RAW/oracle-identical. The usual photo picker and a built-in demo image are
also available. Rendered JPEG, PNG8/16, and TIFF16/32F exports offer six output spaces. PNG16 and
rendered TIFF carry the selected ICC profile; bitmap JPEG/PNG8 tagging needs API 26+, falls back to
plain sRGB tagging on API 24–25, and cannot faithfully tag ACES in its 8-bit path. Scene-linear
TIFF32F is a separate pre-simulation export: verbatim decoded input primaries, deliberately untagged
and EXIF-Uncalibrated for grading elsewhere. An experimental Ultra HDR container exists, but its
honest gain-map/transfer release contract remains open. Source EXIF carry-through currently applies
to JPEG. You can also bake the engine look as a 3D LUT (`.cube` or CLF); that synthetic-lattice
operation excludes source-dependent and spatial/stochastic effects.

**Keep your originals.** Edits are stored as a sidecar keyed to the source file and re-applied when
you reopen or export. The original RAW is never modified.

## Install

Download the latest public APK from the
[Releases](https://github.com/thetechgeekko/Spektrafilm-android/releases/latest) page, allow installs from
unknown sources, and open it. CI artifacts are development evidence, not production-signed releases.
Minimum Android 7.0 (API 24). The native engine ships for arm64-v8a, armeabi-v7a, and x86_64.

## How it was made

This app stands on open color science and open source, and the credit belongs to the projects below.

- **[spektrafilm](https://github.com/andreavolpato/spektrafilm)** by **Andrea Volpato** is the engine
  this project ports — the spectral science, the film-stock profiles, and the LUTs are all his. If
  this is useful to you, please star spektrafilm and read his write-up on
  [discuss.pixls.us](https://discuss.pixls.us/t/spectral-film-simulations-from-scratch/48209).
- **[Image Toolbox](https://github.com/T8RIN/ImageToolbox)** by **T8RIN (Malik Mukhametzyanov)** —
  the Android image-editor architecture that shaped this app's design.
- **[colour-science](https://www.colour-science.org/)** — the color-science library whose color
  matching functions, illuminants, and transforms define what "correct" means here.
- **[LibRaw](https://www.libraw.org/)** — on-device RAW/DNG decoding.
- The **[pixls.us](https://pixls.us)** community, for keeping open photography and open color science
  alive and welcoming. This app is dedicated to you.

### A note on accuracy

The port was done parity-first. We ran the real Python engine headless as an oracle, captured golden
vectors of intermediate results, then ported each stage to C++ and gated it against those vectors.
The current gate is oracle tolerance (`max_abs <= 1e-4`, `rms <= 1e-5`) plus same-build
worker-count invariance. A `tools/parity` harness and CI enforce 39 cases at O2 and at the shipping
compiler flags.

| Stage | Difference vs the original |
|-------|----------------------------|
| Hanatos2025 spectral upsampling | ~1.1e-7 |
| Filming (expose → develop) + DIR couplers | ~1.2e-7 / 2.4e-7 |
| Printing (enlarger + dichroic filters) | ~2.4e-7 / 5.6e-7 |
| Scanning (spectral → XYZ → RGB) | ~6e-8 |
| Halation + scatter + coupler diffusion | ~1.5e-7 |
| Grain (stochastic) | mean-preserving; noise std matched |

These are representative stage measurements, not a promise of universal byte identity. Exactness
levels, current limitations, and the full implementation route are in the
[execution index](docs/EXECUTION_INDEX.md).

## Author

Built and directed by **Akshay**.

- Instagram: [@akshay.pool](https://www.instagram.com/akshay.pool/)
- YouTube: [@Akshayishere](https://www.youtube.com/@Akshayishere/videos)

If the app brings you something, say hi and share your renders.

## Documentation

- [`docs/EXECUTION_INDEX.md`](docs/EXECUTION_INDEX.md) — current authority order and live-work loop
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — engine and app architecture
- [`docs/PRODUCTION_READINESS_PLAN.md`](docs/PRODUCTION_READINESS_PLAN.md) — release acceptance architecture
- [`docs/BIT_IDENTICAL_EXPORT_ROADMAP.md`](docs/BIT_IDENTICAL_EXPORT_ROADMAP.md) — exactness and performance contract
- [`docs/RAW_DNG.md`](docs/RAW_DNG.md) — RAW/DNG decode notes
- [`tools/parity/`](tools/parity/) — the golden-vector parity harness
- [`NOTICE.md`](NOTICE.md) — attributions

## License

GPL-3.0 — see [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md). Because this is a derivative of the
GPLv3 spektrafilm engine, the whole app is GPLv3. Please keep it open.
