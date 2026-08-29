# spectral_bands — what does a lower band count cost?

Our engine runs **81 spectral bands at 5 nm** (380–780). vkdt's `filmsim` runs
**44 bands at 10 nm**. Before any conversation about adopting vkdt's model — or
writing our own GPU shaders at a reduced band count — somebody has to answer the
question that decides it: **how different does the picture look?**

This tool answers it with **no engine change, no device and no GPU**.

## Method

1. `coarsen_profiles.py` rebuilds the bundled asset tree with the film/paper
   spectral arrays band-limited to a coarser grid, but **kept on the native
   81-sample grid**. Wavelength-indexed arrays (`log_sensitivity`,
   `channel_density`, `base_density`, `midscale_neutral_density`) are partitioned
   into non-overlapping blocks of `--stride` samples; each block is replaced by its
   band value and that value is replicated back across the block. A stride-2 tree
   therefore carries exactly the information a 41-band / 10 nm model would, while
   running through the **unmodified** engine.
2. `band_probe.cpp` renders the same fixture through two asset trees and reports the
   error **distribution** in 8-bit display codes. Output is `SPK_CS_SRGB` with cctf
   encoding on, so a delta of `d` is `d * 255` codes directly — the units the
   question is actually asked in.

Band values are computed on the **linear physical quantity**, which is what a careful
low-band port would do: `log_sensitivity → log10(mean(10**S))`, densities →
`-log10(mean(10**-D))`. `--mode naive` averages in the log/density domain instead
(the lazy port) and `--mode decimate` just takes the first sample of each block; the
three bracket the answer.

NaN (JSON `null`) entries keep their exact positions — the null **mask** is identical
between the two trees, so the only variable is the value.

## Running it

```bash
bash tools/spectral_bands/run_band_probe.sh
```

Roughly 40 s, almost all of it the one-time g++ build of the engine.

`SPK_BAND_STRIDES` (default `1 2 3 4`), `SPK_BAND_MODE` (default `linear`) and
`SPK_BAND_WORK` (default `/tmp/spk_band_probe`) override the defaults.

## Reading the output

**The stride-1 control must come back at 0.00 codes on every line.** It renders the
shipped assets against a tree that went through the identical copy + JSON round-trip,
so it proves the harness itself perturbs nothing and the whole delta in the other
runs is band width. If the control is nonzero, stop — the measurement is invalid.

## What it does not cover

Only the **profile** spectral arrays are coarsened. The compiled-in reference tables
(CIE CMFs, D50, the dichroic filters, the TH-KG3 enlarger illuminant) and the
Hanatos2025 upsampling LUT stay at 5 nm, because a real low-band port would resample
those accurately from smooth analytic or finely-measured sources rather than losing
information. The dye and sensitiser curves in the profiles are the genuinely
band-limited quantity, and they are what this isolates. The dichroic filters have
sharp edges and would deserve their own look if a low-band port were ever built.

Results are recorded in `docs/research/vkdt-decision.md` §9.
