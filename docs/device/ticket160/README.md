# #160 — diffusion FFT transform size, measured on the phone

`tools/perf_lab/fft_conv_device_bench.cpp`, built by
`tools/perf_lab/build_fft_conv_device_bench.sh` at the shipping release flags
(`-O3 -ffast-math -fno-finite-math-only`), pushed to `/data/local/tmp` and run
through `adb shell`. SM-S948W, API 36, plugged in, thermal status 0, best of 2.

One convolution of ONE channel. The diffusion stage runs three, so a whole-stage
figure is 3x these.

```
adb push build/perf-lab/fft_conv_bench /data/local/tmp/
adb shell /data/local/tmp/fft_conv_bench --reps 2 640:480:273 1536:1152:651 4080:3060:1725
```

| case | ks | N | tiles | device ms | desktop ms |
|---|---:|---:|---:|---:|---:|
| 640 px preview | 273 | **512 (model)** | 6 | **49.7** | — |
| | | 1024 | 1 | 58.7 | — |
| 1536 px | 651 | 1024 | 20 | 686.9 | — |
| | | **2048 (model)** | 2 | **402.6** | — |
| | | 4096 | 1 | 1376.1 | — |
| 12 MP | 1725 | 2048 | 130 | 8970.9 | 9298 |
| | | **4096 (model)** | 4 | **1766.4** | 1861 |
| | | 8192 | 1 | 3106.9 | 3094 |

**The cost model picks the measured fastest size in all three cases.** That was
the open question: the ranking is size-dependent in both directions, so a model
that ranked wrong on ARM would have cost multiples, and until this bench existed
the model could only be compared against itself (`debug.spektra.fftmax` moves the
ceiling, and the model still picks below it — the app can never be made to run a
size the model rejected).

Two things worth noting:

- **12 MP device times are within 5% of the desktop's**, on a phone with a
  quarter of the cores. That is the signature of a memory-bound kernel: the
  column pass moves the whole spectrum per transform, so both machines are
  waiting on bandwidth, not arithmetic. It is also why the cost exponent (2.8,
  far above `log2 N`) transfers unchanged.
- **The old largest-N rule cost 5.1x at 12 MP** (8971 ms against 1766) and would
  have cost 3.4x at 1536 px (1376 against 403). Raising the ceiling alone would
  have made the 1536 case worse, which is exactly why the ceiling is a bound and
  the cost model is the choice.

The 1536 px row does not reproduce the desktop table in `fft_convolve.cpp`
because that table used a square 1536x1536 (25 tiles at N=1024); this bench uses
the 4:3 frame the app actually renders (20 tiles), which is enough to move the
model's pick from 1024 to 2048. Same model, different case — not a disagreement.

*Film modeling powered by spektrafilm (GPLv3).*
