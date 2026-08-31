# Attribution & Third-Party Notices

Spektrafilm for Android is licensed under the **GNU General Public License v3.0** (see `LICENSE`).

> **Dedicated to the [pixls.us](https://pixls.us) community** — for keeping open photography and
> open color science alive. Thank you. 💛

Authored and directed by **Akshay** —
[Instagram @akshay.pool](https://www.instagram.com/akshay.pool/) ·
[YouTube @Akshayishere](https://www.youtube.com/@Akshayishere/videos).

## Film modeling powered by spektrafilm

The film-simulation engine in this project is a port of **spektrafilm** by Andrea Volpato.

- Upstream: https://github.com/andreavolpato/spektrafilm
- License: GPLv3
- Citation: see `CITATION.cff` upstream / the project's Zenodo DOI.

Per the upstream request: *film modeling powered by `spektrafilm`*. Any academic use should
cite the upstream repository.

## Color science: colour-science

Color-matching functions, illuminants, chromatic adaptation, and color-space transforms follow
**colour-science** — the reference used by spektrafilm and the ground truth for this port.

- Upstream: https://www.colour-science.org / https://github.com/colour-science/colour
- License: BSD-3-Clause

## Android architecture reference: Image Toolbox

The Android app design is informed by **Image Toolbox** by T8RIN (Malik Mukhametzyanov) — its
modular Compose/Hilt architecture guided this project, and it remains the intended richer host
for future versions.

- Upstream: https://github.com/T8RIN/ImageToolbox
- License: Apache License 2.0 (one-way compatible with GPLv3)

## RAW decoding: LibRaw

<!-- libraw-license-route: UNRESOLVED -->

LibRaw Android distribution route: UNRESOLVED.

Full-resolution RAW/DNG decoding uses **LibRaw**.

- Upstream: https://www.libraw.org / https://github.com/LibRaw/LibRaw
- License: LibRaw is offered under a choice of LGPL-2.1-only or CDDL-1.0. No
  distribution route has been selected for this application's static integration;
  including both upstream license texts records provenance and does not elect a
  route. CDDL-1.0 is not generally treated as GPL-compatible. Release remains
  blocked on the human review and reproducible source/relink package described in
  `docs/LICENSING.md`. The route marker cannot be changed alone: the canonical
  decision record must also identify the human approval and confirm rights to
  license the local patch contributions under the selected route.

## Film & paper profiles

The 28 film/paper JSON profiles and the spectral-upsampling LUT binaries originate from
the spektrafilm project and are redistributed under GPLv3. See `docs/ASSETS.md`.

## ICC color profiles

The bundled ICC profiles under `engine/spektra-core/src/main/assets/spektra/icc/` are
redistributed from two upstream sets (carried over from spektrafilm's asset bundle) so the
export writers can embed a correct color interpretation. They are kept under their original
upstream filenames for traceability. See `engine/spektra-core/src/main/assets/spektra/icc/README.md`.

- **`icc/ellelstone/` — Elle Stone's well-behaved ICC profiles.** Copyright 2016, Elle Stone
  (http://ninedegreesbelow.com/). Licensed under **Creative Commons Attribution-ShareAlike 3.0
  Unported (CC BY-SA 3.0)**; attribution required. Upstream:
  https://github.com/ellelstone/elles_icc_profiles — license text:
  `icc/ellelstone/LICENSE-CC-BY-SA-3.0`. The profiles remain under their own
  license; no GPL compatibility or relicensing claim is made here.
- **`icc/saucecontrol/` — Compact ICC Profiles** by Clinton Ingram. Licensed under the **MIT
  License**. Upstream: https://github.com/saucecontrol/Compact-ICC-Profiles — license text:
  `icc/saucecontrol/LICENSE-MIT`.
