#!/usr/bin/env python3
"""Halide AOT generator for the diffusion PSF convolution (experiment, #155).

Spektrafilm for Android — GPLv3. Film modeling powered by spektrafilm.

WHAT THIS REPRODUCES
    model/diffusion.cpp's `mode='same'` direct convolution, verbatim in shape:
        out[y][x] = sum_{i,j in [0,ks)} padded[y+i][x+j] * kern[ks-1-i][ks-1-j]
    in float64, per channel, over a reflect-padded plane the CALLER still builds
    (padding stays in C++ so this generator owns exactly the O(w*h*ks^2) inner
    loop that dominates a diffusion-on frame).

WHY HALIDE HERE AND NOWHERE ELSE
    The engine is compute-bound ~100x past the roofline knee (docs/EXPORT_FASTPATH.md),
    so Halide's headline lever — fusion/tiling for locality — buys nothing on the
    spectral stages. This 2D stencil is the exception: a dense ks x ks f64
    accumulation per pixel, which is a scheduling problem (vectorise x, unroll taps,
    tile for register reuse) rather than a bandwidth one.

NOT BIT-EXACT BY CONSTRUCTION
    Vectorising the reduction reassociates the sum, so results differ from the
    scalar path in the last bits. This is therefore an OPT-IN experiment measured
    against a tolerance, never a replacement for the parity path. The PSF is
    exp(-r/lambda)-based and radially symmetric but NOT separable, so no algorithmic
    shortcut is available without changing the rendered look.

USAGE
    python3 tools/halide/gen_diffusion_conv.py <outdir> [target ...]
    (default targets: host, arm-64-android)
"""
import sys
import os
import halide as hl


def build_pipeline():
    """Returns (func, [inputs]) for the convolution."""
    padded = hl.ImageParam(hl.Float(64), 2, "padded")   # (pw, ph)
    kern = hl.ImageParam(hl.Float(64), 2, "kern")       # (ks, ks)
    ks = hl.Param(hl.Int(32), "ks")

    x, y = hl.Var("x"), hl.Var("y")
    r = hl.RDom([(0, ks), (0, ks)], "r")                # r.x = j, r.y = i

    out = hl.Func("blurred")
    # kern is indexed flipped, matching diffusion.cpp's flip(kern) convention.
    out[x, y] = hl.f64(0)
    out[x, y] += padded[x + r.x, y + r.y] * kern[ks - 1 - r.x, ks - 1 - r.y]

    # ---- schedule -----------------------------------------------------------
    # f64 gives 2 lanes on NEON/SSE (8 on AVX512), so vectorise x by the natural
    # width and parallelise over rows — the same axis the C++ path splits on, so
    # the comparison is scheduling-vs-scheduling, not threading-vs-nothing.
    xo, xi = hl.Var("xo"), hl.Var("xi")
    out.compute_root().split(x, xo, xi, 8).vectorize(xi, 2).parallel(y)
    # The update (the reduction) carries the work: vectorise across x, keep the
    # tap loops as the serial inner dimensions with the column loop unrolled a
    # little for register reuse of the padded row.
    out.update(0).split(x, xo, xi, 8).vectorize(xi, 2).parallel(y).reorder(xi, r.x, r.y, xo, y)

    return out, [padded, kern, ks]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/halide_out"
    targets = sys.argv[2:] or ["host", "arm-64-android"]
    os.makedirs(outdir, exist_ok=True)

    for tname in targets:
        out, inputs = build_pipeline()
        target = hl.get_host_target() if tname == "host" else hl.Target(tname)
        # Distinct symbol per target so both can live in one tree.
        fname = "spk_diffusion_conv"
        stem = os.path.join(outdir, f"{fname}_{tname.replace('-', '_')}")
        out.compile_to_static_library(stem, inputs, fname, target)
        print(f"generated: {stem}.a  target={target}")


if __name__ == "__main__":
    main()
