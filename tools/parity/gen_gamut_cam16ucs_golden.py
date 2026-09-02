#!/usr/bin/env python3
# Spektrafilm for Android — generate the CAM16-UCS (Li 2017) output-gamut-compression parity
# golden (model/gamut_compression.cpp::compress_rgb_cam16ucs_chroma) from the upstream
# oracle utils/gamut_compression.py. GPLv3. Film modeling powered by spektrafilm.
#
# The oracle's OkLch chroma reduction (gamut_compression.py::compress_rgb_oklch_chroma,
# dispatched by compress_rgb with algorithm=="oklch") IS the spec for the native port.
# Per pixel: RGB --(per-space matrix, illuminant = the space's OWN whitepoint, NO
# chromatic adaptation, cctf off)--> XYZ --> OkLab --> OkLch(L,C,h); one-sided
# lightness compression on L (reinhard_knee, params below); look up the baked
# C_max(L,h) for the destination RGB cube; reinhard_knee on C/C_max; reconstruct.
# lightness_compression is pinned EXPLICITLY to the OutputGamutCompressSpec default
# (0.7, 1.0, 2.2) so the golden is self-describing and independent of the spec default.
#
# These cases pin compress_rgb_oklch_chroma across ALL SIX engine output color spaces
# (spk_color_space 0..5) over a spread of in/out-of-gamut linear-RGB pixels and several
# knee triples, captured directly from the oracle.
#
# Binary format (little-endian):
#   int32 num_cases
#   per case:
#     int32 space_index          # spk_color_space (0..5); selects the C++ per-space
#                                #   RGB<->XYZ matrix + C_max table (see mapping below)
#     f64   threshold, limit, power   # the reinhard_knee (chroma) triple
#     int32 npix
#     f64   rgb_in[npix*3]        # interleaved RGB, row-major (linear, in that space)
#     f64   expected_out[npix*3]  # oracle compress_rgb_oklch_chroma output
#   (lightness_compression is constant (0.7,1.0,2.2) for every case — not stored.)
#
# space_index -> colour-science RGB_COLOURSPACES key the oracle is called with
# (verified against colour 0.4.7; idx 5 "linear sRGB" has no distinct colour key and,
# because the oracle runs with cctf off, is byte-identical to idx 0 "sRGB"):
#   0 SRGB        -> "sRGB"
#   1 ADOBE_RGB   -> "Adobe RGB (1998)"
#   2 PROPHOTO    -> "ProPhoto RGB"
#   3 REC2020     -> "ITU-R BT.2020"
#   4 ACES2065_1  -> "ACES2065-1"
#   5 LINEAR_SRGB -> "sRGB"   (== idx 0)
#
# Regenerate: python3 tools/parity/gen_gamut_oklch_golden.py
#   (env: PYTHONPATH=/home/user/spektrafilm/src ; needs numpy + scipy + colour 0.4.7)
#
# Import note (identical to gen_gamut_aces_golden.py): gamut_compression.py imports
# `matplotlib.path` at module level. matplotlib is not installed in the parity env, but
# `colour` degrades gracefully without it — so we import colour FIRST (real), then shim
# a minimal matplotlib.path so the module's top level imports, and register the module
# in sys.modules before exec (its dataclass decorator introspects sys.modules under
# `from __future__ import annotations`).
import importlib.util
import pathlib
import struct
import sys
import types

import numpy as np
import colour  # noqa: F401  — real import FIRST; degrades gracefully without matplotlib

_mpl = types.ModuleType("matplotlib")
_mpl.__path__ = []
_mplp = types.ModuleType("matplotlib.path")
_mplp.Path = object
_mpl.path = _mplp
sys.modules["matplotlib"] = _mpl
sys.modules["matplotlib.path"] = _mplp

import os
ORACLE = os.environ.get(
    "SPEKTRAFILM_SRC", "/home/user/spektrafilm/src"
) + "/spektrafilm/utils/gamut_compression.py"
_spec = importlib.util.spec_from_file_location("gamut_compression", ORACLE)
gc = importlib.util.module_from_spec(_spec)
sys.modules["gamut_compression"] = gc  # dataclass decorator needs it registered
_spec.loader.exec_module(gc)

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
OUT = REPO / "engine/spektra-core/src/main/cpp/tests/gamut_cam16ucs_cases.bin"

# Pinned EXPLICITLY to the OutputGamutCompressSpec default so the golden does not
# depend on the oracle's default staying put.
LIGHTNESS_COMPRESSION = (0.7, 1.0, 2.2)

# spk_color_space index -> colour RGB_COLOURSPACES key (see header).
SPACES = [
    (0, "sRGB"),
    (1, "Adobe RGB (1998)"),
    (2, "ProPhoto RGB"),
    (3, "ITU-R BT.2020"),
    (4, "ACES2065-1"),
    (5, "sRGB"),  # LINEAR_SRGB == sRGB (cctf off)
]


def knees():
    # Same probe set as gen_gamut_aces_golden.py. The first triple (0.0, 1.0, 6.0) is
    # the OutputGamutCompressSpec default *gamut* knee (the "oracle default gamut knee").
    yield (0.0, 1.0, 6.0)   # oracle production / spec default
    yield (0.2, 1.0, 4.0)   # alt threshold + power
    yield (0.0, 1.2, 8.0)   # alt limit (asymptote past the cube edge)
    yield (0.1, 1.0, 2.0)   # gentle low-power knee


def rgb_block():
    """One representative pixel set reused for every output space (48 pixels).

    Covers: black/white cube corners, neutrals across the tone range, RGB primaries
    and CMY secondaries, in-gamut saturated mids, out-of-gamut saturated hues (both
    super-unity and negative channels), super-white, near-black, all-negative, plus
    seeded wide-gamut random pixels. The same block runs through each space so each
    space's own C_max(L, h) table is exercised on identical inputs.
    """
    hand = np.array([
        [0.0, 0.0, 0.0],          # black — anchored to exactly 0
        [1.0, 1.0, 1.0],          # white cube corner
        [0.5, 0.5, 0.5],          # neutral mid
        [0.18, 0.18, 0.18],       # neutral shadow (mid-grey card)
        [0.9, 0.9, 0.9],          # neutral highlight
        [0.04, 0.04, 0.04],       # deep neutral
        [1.0, 0.0, 0.0],          # primary R
        [0.0, 1.0, 0.0],          # primary G
        [0.0, 0.0, 1.0],          # primary B
        [0.0, 1.0, 1.0],          # secondary C
        [1.0, 0.0, 1.0],          # secondary M
        [1.0, 1.0, 0.0],          # secondary Y
        [0.8, 0.2, 0.1],          # in-gamut warm
        [0.2, 0.7, 0.3],          # in-gamut green
        [0.1, 0.3, 0.8],          # in-gamut blue
        [0.6, 0.55, 0.2],         # in-gamut olive
        [1.3, -0.2, 0.1],         # OOG super-red, negative green
        [-0.15, 1.2, -0.1],       # OOG super-green, negative flanks
        [0.1, -0.2, 1.3],         # OOG super-blue, negative green
        [1.2, 1.2, 0.0],          # OOG secondary Y
        [0.0, 1.2, 1.2],          # OOG secondary C
        [1.2, 0.0, 1.2],          # OOG secondary M
        [1.5, 1.5, 1.5],          # super-white neutral (lightness roll-off)
        [2.0, 0.1, -0.5],         # extreme OOG
        [1.1, 0.9, 0.2],          # warm above unity
        [0.05, 0.02, 0.2],        # dark blue in-gamut
        [0.4, 0.4, 0.41],         # near-neutral
        [-0.3, -0.2, -0.1],       # all-negative
        [1e-8, 1e-8, 1e-8],       # near-black
        [0.7, 0.35, 0.9],         # in/near-gamut violet
    ], dtype=float)
    rng = np.random.default_rng(20260702)
    rand = rng.uniform(-0.4, 1.6, size=(18, 3))  # wide-gamut spread
    return np.vstack([hand, rand])  # 48 pixels


def main():
    block = rgb_block()
    recs = []
    for idx, key in SPACES:
        for t, l, p in knees():
            out = np.asarray(
                gc.compress_rgb_cam16ucs_chroma(
                    block, output_color_space=key,
                    threshold=t, limit=l, power=p,
                    lightness_compression=LIGHTNESS_COMPRESSION,
                ),
                dtype=float,
            )
            recs.append((idx, t, l, p, block, out))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "wb") as fh:
        fh.write(struct.pack("<i", len(recs)))
        for idx, t, l, p, rgb, out in recs:
            fh.write(struct.pack("<i", idx))
            fh.write(struct.pack("<ddd", t, l, p))
            fh.write(struct.pack("<i", rgb.shape[0]))
            fh.write(rgb.astype("<f8").tobytes())
            fh.write(out.astype("<f8").tobytes())
    npix = sum(r[4].shape[0] for r in recs)
    print("wrote %d cases (%d pixels) -> %s (%d bytes)"
          % (len(recs), npix, OUT.name, OUT.stat().st_size))


if __name__ == "__main__":
    main()
