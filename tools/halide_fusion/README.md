# Halide fusion spike

**Experiment, not shipped code.** Nothing here is wired into the engine.

The owner raised the Lightroom precedent — Adobe uses Halide, its algorithm/schedule
split lets one pipeline target many devices, and **pipeline fusion** avoids writing a
full-resolution temporary after every adjustment. Our engine does write those
temporaries. So: what would fusion buy *us*, and would it hold our determinism contract?

## Running it

```bash
HL=$(python3 -c 'import halide,os;print(os.path.dirname(halide.__file__))')
g++ -std=c++17 -O2 -I"$HL/include" tools/halide_fusion/halide_fusion.cpp \
    -L"$HL/lib64" -lHalide -Wl,-rpath,"$HL/lib64" -o /tmp/halide_fusion
/tmp/halide_fusion 2048
```

`pip install halide` provides the headers and `libHalide.so`.

## What it builds

A chain shaped like filming's O(n) run — per-pixel math, a 1D LUT (density curve), a 3×3
channel mix (couplers), optionally a separable blur, a second LUT, an output map — scheduled
two ways: **materialised** (`compute_root()` between stages, what our engine does today) and
**fused** (inline into the output's loop nest).

**The blur is the point.** Fusion is spectacular on a chain of elementwise stages, because
no full-resolution temporary is ever written. Our chain is not that: it has blurs in the
middle, and fusion cannot cross a stencil for free — it recomputes producers per consumer
tile or needs line buffering. A spike without a stencil would flatter fusion and mislead.

## Read this before trusting a number it prints

**Single realizations on a shared host swing by more than the effect.** A first pass at
2048 read a 192× fusion win that did not reproduce; the harness now takes **best of 5**, and
even then the 1024 and 2048 answers differ in *direction* for the blur case. Treat the sign
and the order of magnitude as the result, not the digits.

Results and the interpretation are in `docs/research/perf-lab.md` §21.
