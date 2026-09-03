# Halide experiment — diffusion PSF convolution (#155)

GPLv3. Film modeling powered by spektrafilm.

**Experiment, not shipped code.** Nothing here is wired into the engine; `main`'s render path
is untouched. Findings, measured numbers and the honest caveats live in
[`docs/research/simd-halide-experiment.md`](../../docs/research/simd-halide-experiment.md).

## What is here

- `gen_diffusion_conv.py` — Halide AOT generator reproducing `model/diffusion.cpp`'s
  `mode='same'` PSF convolution verbatim in shape (float64, flipped-kernel indexing). The
  reflect-padding stays in C++ so the generator owns exactly the O(w·h·ks²) inner loop.
- `bench_diffusion_conv.cpp` — A/B bench against a reference transcribed from the engine,
  reporting speedup **and** `max_abs` drift (vectorising a reduction reassociates the sum, so
  this path is opt-in and tolerance-checked, never a drop-in for the parity path).

## Run it (host)

```bash
pip install halide
python3 tools/halide/gen_diffusion_conv.py /tmp/halide_out host
HL=$(python3 -c 'import halide,os;print(os.path.dirname(halide.__file__))')/include
g++ -std=c++17 -O2 -march=native -pthread -I/tmp/halide_out -I"$HL" \
    tools/halide/bench_diffusion_conv.cpp /tmp/halide_out/spk_diffusion_conv_host.a \
    -o /tmp/bench_diffusion_conv -ldl
HL_NUM_THREADS=1 /tmp/bench_diffusion_conv 3   # both serial = the honest comparison
```

`-march=native` on the reference and `HL_NUM_THREADS=1` on Halide matter: without them the
comparison flatters Halide by giving it wider vectors and threads the reference does not get
(the engine already parallelises rows around the scalar convolution).

## On device

`bash tools/simd_bench/build_push_run.sh` cross-compiles this bench **and** the Highway FIR test
for arm64, pushes both to the phone and prints the tables. Needs the NDK and an attached device.

## Other targets

`gen_diffusion_conv.py <outdir> <target ...>` accepts any Halide target — `arm-64-android`,
`arm-64-android-vulkan`, `arm-64-android-hvx`, `host-cuda`, … . See the research doc's
"Halide's other backends" section for what each is actually good for here (short version:
Vulkan is real and emits SPIR-V from this same source; HVX is a dead end for f64 spectral math).
