# Spektrafilm Android — Session Handoff

---

## THE COUPLER STAGE IS 44% ALLOCATION AND 0.4% ARITHMETIC (2026-08-30)

Owner asked to start on the ~79% of an export that is `grain + halation + dir_couplers`,
beginning with couplers as the lowest-risk GPU target. **The plan did not survive the
measurement, and that is the result worth keeping.** Full write-up: `perf-lab.md` §23.

### The GPU kernel was built, never ran once, and was removed

`gpu/dir_couplers.comp` + Vulkan kernel + one-time self-check + partial-progress reporting
+ `spk_gpu_couplers_*` counters. It worked — `test_gpu_host` went green under lavapipe.

Then the engagement assertion said `state=0 pixels=0`. **Production takes the SPATIAL
coupler variant** (`digest_filming_params(..., spatial_effects=true)` sets
`dir_couplers.diffusion_size_um = 20.0`); the pointwise fused loop the shader replaced is
only reached if a user zeroes `dir_diffusion_size_um`. Every `max_abs` the test printed
would have passed unchanged on a silent CPU fallback — without the engagement check this
would have shipped as a "validated offload" no render ever entered.

Reverted in full, on the precedent this branch already set with the Highway f64 halation
tier: built, measured, taken out. The patch is preserved at
`scratchpad/gpu_couplers_attempt.patch` if it is ever wanted.

### Where the time actually is (host, 12.5 MP, release flags)

| phase | ms | share |
|---|---|---|
| alloc `correction` (300 MB) | 1246.2 | 14.8% |
| **loop 1** silver → correction | **12.9** | **0.2%** |
| copy `gauss(correction)` (300 MB) | 1772.1 | 21.0% |
| gaussian filter | 1538.4 | 18.2% |
| alloc `tail` (300 MB) | 685.7 | 8.1% |
| exponential filter | 3171.9 | 37.5% |
| **blend** | **21.1** | **0.2%** |

Allocation + one copy **43.8%**; the two filters **55.8%**; the per-pixel loops — the whole
GPU target — **0.4%**. A perfect offload of both loops removes 34 ms of an 8448 ms stage.
The 1246 ms is page faults, not the memset: loop 1 writes the same 300 MB in 12.9 ms once
the pages are resident.

### What shipped: one reordering, no copy

The two filters are independent, so running the exponential tail FIRST leaves `correction`
intact for the Gaussian, which can then blur it in place. The full-resolution copy existed
only to preserve `correction` across a blur that came before its other reader.

- **~22% off the stage.** A single old-then-new pass said 59.3%; that is a page-fault
  ordering artifact. Alternating the arms gives 50.4% / **21.9%** / 27.4%, and 21.9% (new
  arm cold, old arm warm) is the conservative one — it matches the independent phase
  estimate of 21%. Fifth time on this branch a number described an unwritten condition;
  first time it was caught before publishing rather than after.
- **900 MB → 600 MB peak** at 12.5 MP (three f64 planes → two). Unconditional.
- **Byte-identical, proved directly:** both orders run in one process, `memcmp` on the f64
  results, ten filter configurations (FIR-only, IIR-only, the `SMALL_SIGMA_MAX=3` boundary,
  tail-weight 0 and 1) across two image shapes incl. a non-power-of-two, plus 12.5 MP.
- Gated by `test_spatial`, which digests with `spatial_effects=true` and so runs its golden
  at the production `diffusion_size_um = 20.0`. Parity 39/39 ALL OK, both legs.

### What this redirects (the part the owner should read)

1. **Halation does NOT have the same win** — checked, not assumed. `apply_halation_um`'s
   blend consumes the *original* `raw`, so its `core` copy is genuinely required.
2. **The remaining 23% of the stage is still allocation** and cannot be reordered away. It
   needs an engine-level f64 scratch pool surviving across renders;
   `exponential_filter_per_channel_d` allocates its own 300 MB `comp` per call too.
3. **The GPU case for this stage is much weaker than "19% of an export" suggested.** 19% is
   the STAGE; the part a per-pixel shader can touch is 0.4% of it. Serious GPU work here has
   to target the separable f64 filters — and those are the *same two functions* halation
   uses, so `gaussian_blur_per_channel_d` + `exponential_filter_per_channel_d` is a single
   target worth roughly **47% of an export**, not two separate ones.

---

## ROOT CAUSE FOUND, AND THE PRINT ROUTE WAS NEVER FINE (2026-08-29, `7387879`)

**R8 removed `kotlin.Triple.getFirst/getSecond/getThird` and the `kotlin.Pair` pair from
the release dex, so 19 engine params marshalled as 0.0 in every release APK this project
has ever shipped.** Found by the device session with the `debug.spektra.dumpparams` dump,
on the first render. Fixed in `7387879`.

`proguard-rules.pro` keeps `com.spectrafilm.engine.**`, so the engine's own getters
survived and returned real `Triple`s — but no *bytecode* calls `Triple.getFirst`. Its only
caller is `spektra_jni.cpp`, by literal string, which R8 cannot see, so it shrank them as
unreachable. **`-dontobfuscate` prevents renaming, not removal.** A second, independent
defect made it silent: `unbox_float(nullptr)` returns `0.0f`, and `read_triple_f` wrote
that into the output *unconditionally*, destroying the defaults rather than leaving them.

### The correction that matters: the print route was ALSO broken

The device read "print is fine" from file size — 5.6 MB versus 76 KB. Reproducing the
exact zeros on the host says otherwise (`tools/r8_check/r8_zeros_repro.cpp`, 512×512,
portra_400):

| case | spread R/G/B | mean R/G/B |
|---|---|---|
| slide, correct params | 0.470 / 0.385 / 0.383 | 0.520 / 0.300 / 0.220 |
| **slide, R8 zeros** | **0.000 / 0.000 / 0.000** | 0.863 / 0.525 / 0.416 — **flat** |
| print, correct params | 0.830 / 0.830 / 0.811 | 0.477 / 0.361 / 0.345 |
| **print, R8 zeros** | **0.036 / 0.035 / 0.031** | **0.054 / 0.057 / 0.069** |

The slide constant lands at 8-bit **[220 134 106]**; the device reported **[220 135 106]**
from a different scene through a different JPEG path. One code apart in green — the
mechanism, confirmed numerically rather than by inference.

Print did not survive: spread collapses **23×**, mean drops to near-black. It is not
*constant*, so it still compresses to megabytes and passes a file-size glance — which is
the only reason it read as healthy. **Both routes were broken in every release build; one
was merely broken visibly.** Any "print is the trustworthy leg" reasoning — including
using it as the clean baseline for a GPU re-measurement — is invalid.

### What this invalidates

Every on-device number taken before `7387879`, not only the slide-route ones: the 25.2×
GPU scan ratio (a degenerate frame *and* 19 wrong params), the effects ladder (halation and
coupler vectors zeroed), and #119's baseline doc. `docs/AUDIT.md` §D also needs revisiting
— the 2026-06-04 on-device validation of a minified build passed while this was live,
because it confirmed the app *ran*, not that its numbers were right.

---

## REPLY 2 — THE ENGINE IS EXONERATED ON THE SHIPPING TOOLCHAIN (2026-08-29)

*CORRECTION (appended after this section was written): direct delivery to another
session **does** work — just not via `SendMessage`, which fails with an auth error every
time. `create_trigger` with `persistent_session_id` set to the target session delivers
fine, and the device session had already told us so ("this trigger path DOES reach you...
SendMessage is what fails, not cross-session delivery") in a routine sitting in our own
trigger list. Three replies were routed through this file on a false premise before that
was noticed. Everything below was delivered directly as `trig_01GzYNzr9sDDFe5gYcfdUutu`;
this file is now the archive, not the only channel.*

### Your experiment 1 is accepted, and it moved the search off the engine

Grain ON → constant, grain OFF → real image, one toggle, same session. That is clean and
I am treating it as established. Your 512×512 loupe result also correctly kills the size
axis — and you were right that it makes the grain finding *stronger*, because it removes
the confound I complained about. Good.

**Correcting myself on one thing, and correcting you on another.**

Me: I said the cause was device-side and told you not to bisect the engine. That still
holds, but I can now say something much stronger than "it does not reproduce on my host".

You: you dismissed `grainDensityMin` because it only feeds `scan_film`'s LUT domain. **It
does not.** `spektra.cpp:763-765` copies `p->grain_density_min` straight into
`g.density_min`, the GRAIN MODEL's own parameter, and `model/grain.cpp` uses it
throughout — `density_max[c] = density_max_curves[c] + density_min[c]` at line 175, added
before sampling and subtracted after (lines 188-208, 239-289). That is squarely on the
direct path and it is grain-only, which is exactly the shape of your bug. Your instinct to
flag it was better than your reason for dropping it. It is still not the mechanism *here*
(both sides run the documented default), but it is now a live suspect if the app ever
passes a non-default — see the dump below.

### I built the shipping toolchain and it still does not reproduce

Rather than keep asserting "not on my host", I installed the real thing and ran the same
case three ways. All at `-O3 -ffast-math -fno-finite-math-only`, slide route, grain ON,
512×512, 8 workers, with a deliberately hostile scene (exact zeros, **negative** pixels,
64.0 speculars — the things a real ACES RAW has and my earlier synthetic scenes did not):

| build | spread R / G / B | flat? |
|---|---|---|
| x86_64 gcc 13 | 0.712398 / 0.541815 / 0.432103 | no |
| **aarch64** gcc 13 (qemu) | 0.712398 / 0.541815 / 0.432103 | no |
| **aarch64 NDK r27 clang 18** (qemu) | 0.712398 / 0.541815 / 0.432103 | no |

The third row is the compiler, architecture and flags the APK actually ships. **Identical
to six decimals across all three, and none is flat.** T=1 vs T=8 also identical.

The table above shows one case; the **full 12-case sweep** (5 scenes x T=1/T=8, plus the
print-route and grain-off controls) then finished on aarch64 and **every line matches the
x86_64 run to all six reported decimals** — including the print control at
0.986413/0.988814/0.979199 and grain-off at 0.924929/0.577006/0.461372. Stated precisely:
that is agreement of the reported statistic, NOT byte-equality of the images, which
CLAUDE.md correctly says does not hold across architectures.

So: the engine sources, built as we ship them, do not produce this bug. It is not arm64
codegen, not `-ffast-math`, not thread count, not hostile input values, and not any of the
66 configurations from the earlier sweep. Combined with your grain-toggle result, what is
left is **what the app hands the engine**, or the real DNG's pixel content.

### So I built you the tool for the next cut

You read the grain params off the UI. That is evidence about the UI. **#143 is an entire
open batch of "params that lie"** — controls whose displayed value and marshalled value
disagree — and you already found two independent smells yourself: scan white level reading
1.000 where the tooltip says 0.98, and scan black 0.000 where it says 0.01. Something is
writing non-defaults. The UI is not a trustworthy witness here.

`spektra_jni.cpp` now dumps what actually crosses the boundary, gated on a system property
so it is inert otherwise:

```
adb shell setprop debug.spektra.dumpparams 1
# then one slide+grain export, and:
adb logcat -s Spektra | grep '^.*params '
```

Five lines: route + gates, grain (including `density_min`, `uniformity`,
`particle_scale`, sublayers, `n_sub`), grain 2, scanner corrections + levels, and
profiles/output. **Compare those against the UI.** If they disagree, that is the bug and
it is in the app, not the engine. If they agree with the documented defaults, the remaining
suspect is the DNG content and I want a stripped repro frame.

### Your two proposed cuts

1. **Sublayers OFF — yes, do it.** `test_grain` and `test_grain_sublayer` are separate
   gates precisely because they are separate code paths (`apply_grain_to_density` vs
   `apply_grain_to_density_layers`, `filming.cpp:660-690`), and splitting them costs one
   export. Run it *with* the param dump on.
2. **Print-route reconfirm in the same session — yes**, worth one export, for the reason
   you give: the print-is-fine leg should not rest on a 90-minute-old run.

Experiment 2 (masks): agreed, excluded by construction, do not spend an export.

### I also fixed the thing that broke CI twice

You could not have hit this, but it is why I now trust the above. `tools/arm64_check/check_android_link.sh`
links `libspektra.so` for arm64 with real NDK clang, the real shipping flags, the
CMakeLists **enumerated** source list, `-Wl,--no-undefined`, and the 16 KB page flag, then
checks LOAD alignment is `0x4000`. It also fails if a `.cpp` exists on disk but is missing
from CMakeLists — the exact `551c57f` failure the host glob build hides.

Control-tested both ways: dropping an unlisted source in makes it fail and name the file;
removing it makes it pass. The host suite never compiles `spektra_jni.cpp` at all, so this
is also what verified today's JNI change before it was pushed.

---

## REPLY TO THE DEVICE/LAPTOP SESSION (2026-08-29, head `22e69a3`)

*Direct session-to-session messaging has now failed three times with the same auth
error — a cloud session's credential is accepted for its own work but not for
delivery to another session. This file is the channel. Read this before acting on
the order below, which it partly supersedes.*

### (a) The flat-render bug: your lead is dead, and your discriminator is confounded

**Your own caveat killed the lead, and you were right to raise it.** `scanning.cpp:493`
sits inside `if (params.use_lut && !gpu_lin_done)`. `use_lut` defaults false and your
failing renders are the direct path, so that block never executes on them. Not a weak
lead — an unreachable one.

I also excluded the obvious second suspect before testing anything: the scan-route film
memo is gated `!scan_tap_bypass && !grain && p->disable_buffer_memos == 0`. **Grain ON
disables the memo**, so a stale-buffer explanation cannot apply to exactly the renders
that fail.

Then I ran the real engine on the host, reporting per-channel output spread, across:

| axis | values |
|---|---|
| route | slide + print |
| grain | on + off |
| size | 512, 640, 1024, 2048, 2560 (export path, `preview_max_size=0`) |
| auto-exposure | off + on |
| film | portra_400 (negative) + provia_100f, velvia_100, ektachrome_100 (positive) |
| scene | ~4-stop and ~10-stop |

**46 configurations, zero flats** (66 including the scanner-correction sweep below).
Slide + grain + 2560 + AE on + positive stock gives 0.93 spread per channel. The engine
is clean on every axis you named.

Two consequences:

- **AE cannot be the size mechanism.** Metering runs on a max-256 downscale
  (`spektra.h:525`); measured EV moves 0.014 across 512 → 2560.
- **Your table changes three things at once** at the 640 boundary: grain starts
  running, the preview path becomes the export path, and size crosses 2048. You read
  that as "the discriminator is grain". Separating the three, the engine survives all
  of them — so the grain correlation is an artifact of the test matrix, not a mechanism.

The Kotlin side is also clean: `scanFilm` is pure plumbing, nothing slide-specific runs
post-engine.

So the cause is device-side and I have not matched it. **Do not bisect the engine
on-device.** Run these three instead, in order:

1. **The one you already flagged as outstanding: grain OFF + slide + full-res export.**
   If it is still flat, grain is not the discriminator at all and the correlation
   collapses. Highest information per export of anything available.
2. **Slide + full res + all local masks/adjustments OFF** (#141 is a known
   mask-compositor export defect — exclude it).
3. **Report the film stock, and whether scanner black/white correction was on** — for
   the record, not because I still suspect it. The engine builds an affine from measured
   black/white references for **positive film on the slide route only**
   (`spektra.cpp:1043-1063`), and a degenerate reference pair collapses an affine to a
   constant, which is the right symptom class. **Swept it: also negative.** With the
   correction ON, provia at 512→2560 stays at 0.97–0.99 spread (the correction is
   plainly active — it moves spread from 0.91 to 0.97 and shifts the mean), and on
   portra it is a strict no-op, byte-for-byte identical to OFF, exactly as the engine
   comment claims. So that hypothesis is dead too.

**Running total: 66 host configurations, zero flats.**

**File the ticket regardless** — a whole route producing a constant at export size is
release-blocking. Attach the host negative result so nobody re-runs those 46 configs.

### (b) `SPK_DIFFUSION_FFT_MAX`: yes, but not until you pull `22e69a3`

Two problems, one of them mine.

**The memory numbers you quoted were my stale comment.** 134/537 MB is the *pre-r2c*
formula; the real-to-complex change dropped the spectra from N×N to N×(N/2+1) and the
text was never updated. Actual is `2*N*(N/2+1)*2*8 + N*N*8`:

| N | real scratch | my old comment said |
|---|---|---|
| 2048 | **100.7 MB** | 134 MB |
| 4096 | **402.8 MB** | 537 MB |

Your tile counts (130 vs 4) are exactly right.

**The experiment cannot detect its own failure mode.** `fft_convolve_same` catches
`bad_alloc` and returns false, and the caller falls through to the **direct
O(w·h·ks²) loop** — silently. If the device cannot hand out 402.8 MB mid-export,
raising the cap does not OOM and does not error; it reverts to the ~10.9-hour path.
"N=4096 didn't help" and "N=4096 never ran" then produce identical timings.

So `22e69a3` splits the call site into *cost model chose direct* vs *FFT was refused*
and counts the second: **`spk::diffusion_fft_fallbacks()`** /
`diffusion_reset_fft_fallbacks()` in `model/diffusion.h`. Both stale comments corrected
in place. No numerics change — the counter increments on a branch already taken.

**So: yes to patch-measure-revert, once you pull that, and report the fallback count
beside the timing.** Nonzero means you measured the direct path. 402.8 MB on top of a
12 MP export is a big ask, so nonzero is a likely outcome, not a remote one — and if it
does fall back, the fix is not a bigger constant, it is **f32 spectra**, which buy
N=4096's block size at roughly N=2048's memory.

### (c) Task 3 on the print route only: agreed

Your reasoning is right — the ladder measures per-effect cost, print is the default
route and it works, garbage rows would only be re-run. One addition: read
`spk::diffusion_fft_fallbacks()` after the Pro-Mist row, so we learn whether the device
takes the FFT path at all at shipping settings, independently of the cap experiment.

### Your task 1 number: your reading is right, with one caveat you already named

25.2× on the scan stage, 1.21× on the export, scan = 964 of 6227 ms. Amdahl caps a
perfect scan offload at 1.18×, which your 1.21× already meets. **The scan-stage port is
done and does not justify the rewrite on its own.**

But **both arms rendered a constant**, and a constant-output frame is a suspiciously
friendly workload for a memory-bound stage. Until the flat bug is understood, treat
25.2× as provisional and re-run one rep on the **print** route, which renders correctly.

None of this touches the full-chain question: grain 1862 + dir_couplers 1264 +
halation 860 = 4000 of the 6227 ms, and none of it has ever been near a GPU.

### Your two UI bugs

Both real, both worth filing. The status pill is worse than cosmetic — `ExportMask`
swallowing pointer input for 46 minutes while the pill claims to still be exporting is
a lie plus a lockout.

The second one is **my error**, now fixed: Slide mode is Simulation → **Output**, not
Scanner.

---

## ORDER FOR THE DEVICE/LAPTOP SESSION (2026-08-29, head `6dd2126`)

*Written here because `SendMessage` fails from a cloud session with an auth error.
**That is a limitation of `SendMessage`, not of cross-session delivery** — see the
correction at the top of this file: `create_trigger` with `persistent_session_id` reaches
another session fine, and is how later replies were actually delivered. Do not repeat the
mistake of concluding a peer is unreachable because one tool refused.
Pull to `6dd2126` first — that commit is the one carrying this order.*

### Why task 1 is first

On release the engine is **5504 of 6251 ms — 88% of an export**. Deleting decode, grade
and encode *entirely* still leaves 5.5 s against a 1–2 s target. The CPU side is finished
as a lever, and the route is decided: **our own 81-band GLSL shaders, vkdt as the
architecture guide** (`docs/research/vkdt-decision.md` §11). But **nobody has ever
measured GPU against CPU on a big engine stage at export resolution on this hardware.**
Every GPU argument in that document rests on a number that does not exist yet.

### 1. GPU vs CPU, scan route, full resolution — the decisive one

No new code needed: the experimental GPU export toggle and the persistent Vulkan host
already ship.

- **RELEASE build.** Not debug — see the warning below.
- Full-res export, scan route, GPU export toggle **OFF**, then **ON**. Three reps each.
- Send the whole `stage timings` line for both, not just the total. The `scan=` slot is
  the one that matters.
- **Report the ratio even if it is bad.** A 1.2× is as decisive as a 10× — it collapses
  the rewrite case, and that is worth knowing before anyone writes a shader.

### 2. Confirm #160 on device

Black Pro-Mist was O(n²) and 98.2% of a render: **30.7 s for one 640px preview** at the
app's own defaults, extrapolating to ~10.9 hours at 12 MP. Now FFT + real-to-complex on
the CPU (17663 → 195 ms on host, 90.7×). Flip Pro-Mist on, one preview render, one full
export, report `camera_diffusion=`. `SPK_DIFFUSION_FFT=0` forces the old direct path for
an on-device A/B; `SPK_DIFFUSION_FFT_MAX` tunes the transform cap.

### 3. The all-effects ladder, on release

Print route, full res, one export each: baseline / + Pro-Mist / + lens blur / + glare /
+ highlight boost / ALL ON. `stage timings` line for each. **Two traps that have both
bitten already:** zero slots are SKIPPED (a missing slot means off, not free), and
`scan_spatial` and `glare_field` are SUB-MEASURES nested inside `scan` — do not add the
printed slots up.

### 4. #119 wizard

Unblocked. The stale "Scan film" instruction is fixed — the control is **"Slide mode
(skip print)"**, the last row of Simulation → **Output** (below "Saving CCTF encoding",
`MainActivity.kt:3197`). An earlier revision of this line said Simulation → Scanner,
which is wrong. Black Pro-Mist is "Camera diffusion filter", last row of the **Film**
sub-tab.

**Do not run the wizard yet if it would record slide-route rows** — see the flat-render
bug below.

### STAY ON RELEASE, and this is not a formality

Three of the four native modules compiled at **`-O0`** in debug until `19cb57e` — no
`CMAKE_CXX_FLAGS_DEBUG` guard, so CMake's default `-g` applied. That is why decode moved
6.68× between builds while the engine moved 1.48×. **Every number taken before that fix
was a debug number.** Do not flip to debug for `run-as`; prefs inspection is not worth
turning every measurement back into a debug measurement.

### What landed today, so you are not re-deriving it

- **`4da9b19`** — diffusion FFT + r2c. `kernels/fft.{h,cpp}` (`FftPlan` + `RfftPlan`),
  `kernels/fft_convolve.{h,cpp}`. Parity suite is **39 tests** now, green on both legs.
- **`cec55d4`** — CI runs our GLSL under **lavapipe** (`mesa-vulkan-drivers` +
  `libvulkan-dev`). `test_gpu_host` validates at 2.4–3.6e-06 against the CPU reference,
  in 5 s, with no GPU. It gates the shader's **math and determinism** — **not**
  performance, and **not** arm64 transcendental precision. Which is exactly why task 1
  still needs your device.
- **`2b5ac31`** — 44 bands vs our 81: the scan route survives 10 nm, the print route does
  not (15–17 codes). That is why we keep 81.
- **`e99bbea`** — Halide fusion is **0.78–1.51×** on our real shape (stencils kill it),
  not the 18–36× a stencil-free spike reports. Not adopted. NumHalide also not adopted —
  but note the determinism objection against it was **wrong and is retracted**; it is
  byte-identical across Halide thread counts.
- **A hard rule for the shader work** (`perf-lab.md` §21.3): **interpolate every LUT,
  never round an index.** A 1-ULP index difference flips `cast<int>` to the next table
  entry — a 60,000× output amplification. Our CPU LUTs interpolate, so we are immune; a
  GPU port differs by ~1 ULP *everywhere* by construction (fp32 vs f64), so a
  nearest-index fetch in a shader would scatter single-step errors across the frame.

### One caution about today's work

CI broke twice, both times because the change was verified in an environment
**better-equipped than CI**: the host parity build globs sources where the Android
CMakeLists enumerates them, and this container already had `libvulkan-dev`. Guards were
added for both, but the root cause — local green does not imply CI green — is unfixed.
If something here does not build on your side, suspect that first.

---

## Current state (2026-08-28, the GPU line opens: #127 resolved, M1 preview offload)

- **Owner priority: GPU first** (supersedes the baseline-first ordering; #119 stays
  open and wanted). **#127 RESOLVED**: route = Vulkan compute per-kernel; scope = full
  GPU chain, gated per-kernel by on-device E3 measurement; **option B OPENED** — GPU
  export sanctioned under the oracle-verified-on-your-device regime (#149 codifies the
  law revision; CPU stays the parity ground truth + fallback forever). Tickets:
  #146 (M1) → #147 (device measurements, owner's laptop session) → #148 (full-chain
  preview) → #149 (GPU export).
- **On-device numbers are in** (PR #145 merged + #147/PR #150 from the owner's laptop
  sessions): scan 2.15e-06, filming 6.46e-06, printing 7.91e-07 worst-case `max_abs` —
  all 15×+ inside the oracle bar, ×5 byte-identical; fp16 fails (~1e-2); full tables in
  `docs/research/gpu-device-probe.md`.
- **M1 (#146) implemented on this branch**: persistent two-kernel Vulkan host
  (pipeline/buffers created once, grow-only, fence-reused; NaN guard folded into the
  upload), a NEW `scan_spectral_lin.comp` linear variant (unclipped linear RGB out) so
  unsharp/lens-blur/gamut/glare/non-sRGB frames offload too (production default
  scanner unsharp (0.7,0.7) + print glare land on the linear path; glare composes as a
  post-AXPY, M·(xyz+gI) = M·xyz + g(M·I)), preview-only latch
  (`spk_params.gpu_preview` → `allow_gpu_scan`, set ONLY by `spk_simulate_preview`),
  one-time on-device self-check (CPU engine = the reference; fail → CPU for the
  session, JNI logs once), Settings → "GPU engine (Vulkan)" toggle (default OFF,
  `AppSettings.gpuEngine` — distinct from the GLES loupe's `gpuPreview`), Android
  build now compiles the host (`SPK_ENABLE_VULKAN=ON` in gradle; host test builds
  stay stub; SRC sets gained `gpu/*.cpp`). Local gate `tests/test_gpu_host.cpp`
  (SwiftShader + stub modes) ALL OK: export byte-ignores the toggle, ×3 warm-host
  byte-identical, GPU preview ≤1e-4 of export or better than the LUT preview
  (scan route: 7.6e-07 vs the LUT's 4.4e-05).

## Prior state (2026-08-27 evening, AFK batch: #119 prep + full-app audit — PR #137)

- **#119 agent-side prep SHIPPED**: manifest `<profileable android:shell>` (simpleperf on
  release builds), export start/duration logcat breadcrumbs, and
  `tools/baseline/baseline_wizard.sh` — the 8-stage interactive capture runbook the owner
  runs at home (`bash tools/baseline/baseline_wizard.sh`); writes `docs/baseline-s26u.md`.
- **Full three-lane audit executed** (Kotlin app / native engine / build+CI+docs);
  `docs/AUDIT.md` rebuilt to 2026-08-27 truth. Owner decisions filed as **#138–#144**
  (release due; editor-state loss on sub-screen nav; Ultra HDR flat gainmap; mask-compositor
  export OOM; tc_lut_cache growth; params-that-lie batch; README lede).
- **Parity gate is now 38** (was 36): `test_grain` + `test_grain_sublayer` verified green
  locally and wired into `ci.yml` + `run_engine_parity.sh`. Full suite 38/38 ALL OK on this
  tree.
- **Engine hardening landed**: JNI boundary wrapped (bad_alloc → catchable
  OutOfMemoryError, no more SIGABRT), `apply_highlight_boost` map parallelized, and a
  comment truth pass (parallel.h, tc_lut cache key/growth, gamut ordinals, M0 fossils).
- **App fixes landed**: silent failures now logged (recipe save, draft render, preset
  blend, uri permission), HowToUse BackHandler, export temp-file cleanup on all paths,
  LUT write off the main thread, empty custom export size = full res (+test), 11 stale
  comments fixed, 7 dead-code deletions. Docs: 12-file staleness batch.
- **GPU research (#135) answered earlier today** — `docs/research/gpu-bit-exact.md`;
  vkdt filmsim (GPLv3, our exact upstream on Vulkan fp32) is the #127 preview seed.
- Road: owner merges PR #137 → runs the baseline wizard (#119) → #126 targets → #127.

## Prior state (2026-08-27, parallelize the serial per-pixel maps — #122)

- **#122 LANDED (this session): the last serial per-pixel/per-line hot loops now run
  through the deterministic fork-join.** No algorithmic change anywhere — the same
  arithmetic per pixel/row/column, chunk boundaries a pure function of (count, threads),
  byte-identical output for any worker count. 36/36 gates green.
  - **3D-LUT PCHIP apply + normalization** (`kernels/lut3d.cpp`) chunk over pixels —
    preview force-enables both spectral LUTs, so this was serial work on every frame.
  - **Gaussian/exponential filters** (f32 `kernels/gaussian.cpp`, f64
    `kernels/exponential_filter.cpp`): FIR + IIR-horizontal over rows; IIR-vertical over
    columns with chunk-local recurrence state; de/re-interleave + init/copy/axpy over
    elements. Serves halation, lens blur, scanner blur/unsharp, DIR diffusion, grain
    blurs, glare.
  - **Diffusion filter + halation maps** (`model/diffusion.cpp`): the O(w·h·ks²) direct
    convolution + reflect-pad build over rows; scatter/halation mixes over elements.
  - New `parallel_for_weighted(begin, end, unit_work, body)` in `kernels/parallel.h` —
    deterministic chunking with the min-chunk clamp measured in pixel-equivalents, so
    row/column ranges don't collapse to one chunk (`parallel_for` itself unchanged;
    dispatch factored into `detail::parallel_dispatch`).
  - `test_parallel` grew scenarios 6 (print + both spectral LUTs) and 7 (camera
    diffusion filter): all 7 scenarios memcmp-identical 1-vs-8 workers under
    `SPK_PARALLEL_MIN_CHUNK=256`.
  - Measured (host 4-core, 3.1 MP, 1→4 workers): LUT apply 2.5×, Gaussian f64 3.0×/2.8×
    (IIR/FIR), f32 3.1×/3.5×, exponential 2.6×, halation 2.1×, diffusion conv 3.9× —
    checksums identical at 1/4/8. Route-level (640×480, 4 workers, old-vs-new binary
    with identical output checksums): print+LUTs −18%, scan+halation −19%,
    scan+halation+diffusion-0.8 **49.2 s → 13.4 s (3.66×)** — the direct convolution
    dominates that config; a separable/FFT replacement would touch parity numerics and
    is a separate decision (fog item).
- Perf line now waits on **#119** (on-device baseline, HITL — needs the owner + adb),
  which unblocks #126 numeric targets and the #127 GPU-preview decision.

## Prior state (2026-08-27, export fast path part 2 — #121)

- **#121 LANDED: EXPORT_FASTPATH item 4 — the float64 full-res
  intermediates are retired.** Allocation/lifetime work only; zero arithmetic change;
  36/36 gates green; `test_simulate_e2e` scenario G extended to gate the direct path
  (AE-on + spatial-on) byte-identical to the materialized path.
  - Direct float32 filming for one-shot no-op-geometry renders: `PreprocessedInput`
    (spektra.cpp) + `expose_f32_gain` (filming) + `measure_auto_exposure_ev_f32`
    (autoexposure; also used by `spk_meter_exposure_ev`, whose 288 MB scratch is gone).
  - Fused expose/scan passes when nothing intervenes; `raw`/`lin_rgb` exist only for
    active effects, as uninitialized buffers. Free-at-last-use for `pin.rgb` (post-
    expose) and `film_density_cmy` (post-print_expose); geometry passthrough moves.
  - Measured 12 MP VmHWM: print 1.10 → **0.43 GB (−61%)**, scan 0.97 → **0.43 GB**;
    grain-on print 1.55 → 1.28 GB (grain's own acc/layers buffers = future ticket).
    Slightly faster too (cold scan 196.9 → 179.8 ms at 512²/4T).
- Remaining fastpath ticket: **#122** (parallelize LUT apply + spatial filters).
  Then the perf line waits on **#119** (device baseline, HITL).

## Prior state (2026-08-27, export fast path part 1 — #120)

- **Owner priority (standing): performance first.** No camera-feature work until the app is
  "super fast" (#123 closed DEFERRED; map #117 carries the note). The perf line is: #119
  device baseline (HITL) → #120/#121/#122 decision-free fastpath tickets → #126 targets →
  #127 GPU-preview decision.
- **#120 LANDED (this session): EXPORT_FASTPATH items 1+2, bit-exact.**
  - O(1) uniform-axis density lookups (`kernels/uniform_axis.h`; wired into
    `kernels/interp.cpp::interp1d_planar3` + `model/couplers.cpp::fast_interp_channel`).
    Detection at load (strictly ascending + within step/4 of uniform), estimate + fix-up walk
    to the EXACT searchsorted bracket, binary-search fallback otherwise. Host kernels
    (4.19 M px, 1 thread): exposure→density 725.5 → 65.6 ms (11.1×), DIR couplers
    637.3 → 70.3 ms (9.1×); cold scan −9.4% / cold print −6.0% (512², 4T). Outputs
    byte-identical (checksummed over 12.6 M randomized lookups). NaN guard added to
    `fast_interp_channel` (was an out-of-bounds `xa[-1]` read).
  - One-shot memo opt-out: `spk_params.disable_buffer_memos` (default 0 = unchanged), set by
    the JNI for every non-preview render (export + magnifier). Skips the full-buffer FNV key
    hashing, the memo stores, and the slot eviction; preview memoization unchanged; miss-path
    key now computed once (was twice). −275 ms per one-shot render at 3.1 MP (≈ −1.1 s at
    12 MP). New gate: `test_simulate_e2e` scenario G.
  - Suite: 36/36 green locally (`tools/parity/run_engine_parity.sh`); no new gate binary, no
    workflow edit needed.
- Remaining fastpath tickets: **#121** (retire float64 intermediates, peak 1.2 GB → 0.35 GB)
  and **#122** (parallelize LUT apply + spatial filters) — both decision-free AFK tasks.

## Prior state (2026-08-26, fork-engine adoption worktree — #125)

- **Fork engine adoption LANDED (local commit series; see issues #117/#118/#125).** The
  VirtuaTOA/spektrafilm-android engine overlay — verified green in #118 (36/36 gates +
  statistical grain checks + 12 MP 1-vs-8-worker memcmp) — is adopted per the #125 resolution:
  grain-stage parallelization (fixed 8192-px blocks, per-block SplitMix64 seeding, dynamic
  atomic-counter scheduling), the spectral 3D-LUT memo + shared interpolator
  (`kernels/lut3d_cache.{h,cpp}`), the debug `-O2` CMake guard, the AE-off `spk_bake_cube_lut`
  + sRGB-shaped lattice, the `spk_meter_exposure_ev` / `meterExposureEv` / `exposureGain`
  metering API, and `tools/parity/run_engine_parity.sh` (full-suite local replay with a
  ci.yml drift guard). Our `kernels/parallel.{h,cpp}` + `tests/test_parallel.cpp` were kept at
  our HEAD (the `959e786` non-vacuous thread-invariance gate; the fork never touched them).
- **Hardening on top of the overlay:** LUT-memo key segments are now length-prefixed (4-byte LE
  byte-count header; two distinct input sequences can no longer concatenate to one byte
  string), and the stale eviction comment was fixed. New gates: `test_parallel` scenario 5
  (192×160 = 30,720 px → 4 grain blocks, 1-vs-8 workers memcmp-identical — the 64×64 fixture
  runs grain in a single block and could not exercise the scheduler) and `test_bake_lut`'s
  shaper property case (corner byte-equality, shaped≠linear, shaped entries vs a 65³ linear
  reference within a measured 6e-2 interpolation-error bound).
- **Host parity suite = 36 gates** (was 35): `test_lut_cache_e2e` added to ci.yml's
  engine-parity job — the single-line workflow edit the owner authorized on #125.
  `run_engine_parity.sh` drift guard counts 36 == 36. Full local run: `ALL OK`, zero FAIL.
- **GRAIN REPRODUCIBILITY NOTE (accepted behavior change):** the grain field differs from
  releases built before the adoption — per-block seeding replaced the old whole-image serial
  RNG stream. It stays deterministic (same input+params+seed ⇒ same bytes), thread-invariant,
  and inside the oracle's statistical band; grain was never oracle-bit-exact (stochastic stage).
  Parity goldens are grain-off and unaffected.
- **App wiring:** GPU preview bakes with `SpektraEngine.SHAPER_SRGB` and the GLES shader
  decodes through the exact inverse + multiplies the metered AE gain (`uExposureGain`);
  `.cube`/CLF export bakes stay `SHAPER_NONE`. Android SDK absent in the work container, so
  `:app:testDebugUnitTest`/`:app:lint` were NOT run there — run them before merging.

## Previous state (2026-07-02, branch `claude/exciting-hamilton-hya62`)

- **"Exact + fast" pass MERGED (PR #109 + #110).** The PM directive (*"spektrafilm-exact result at
  ultra-fast speed"*) is fully landed: F1–F7 Kotlin robustness fixes, **E1** per-effect spatial
  decouple, **E2** print-route spatial + grain enable, **S1** scan-route film-density memo,
  **S2** print-density memo, **S3** Kotlin retained-result grade cache, **S4** deterministic loop
  parallelization. Every default engine path stays byte-identical; the two intentional look
  changes (RAW-input colorspace correction, print-route now carries film character) are onto the
  oracle. See the changelog entry below for commits + the perf table.
- **Oklch perceptual output-gamut compression MERGED (PR #111) — P2 #6 slice 1.** Opt-in /
  default-OFF, byte-identical off. C++ `OutputGamutCompress::kOklch=3` / facade `OKLCH` / UI
  "Oklch (perceptual, keep hue)": perceptual-hue-preserving chroma compression at constant
  Oklch(L, h) — Reinhard knee on `C / C_max`, `C_max` regenerated in-engine by a 64×720 bisection,
  float64 matrices from colour-science. Bit-exact to the oracle (gate `max_abs 1.077e-14`), gated
  by **`test_gamut_out_oklch`** (golden generated at oracle `27bd085`).
- **Oklrab perceptual output-gamut compression LANDED on this branch (P2 #6 slice 2) — NOT yet
  merged.** Opt-in / default-OFF, byte-identical off. C++ `OutputGamutCompress::kOklrab=4` / facade
  `OKLRAB` / UI "Oklrab (perceptual, even lightness)": the oklch chroma reduction with the per-pixel
  `C_max` lookup indexed by Ottosson 2023's rebased lightness `Lr = f(L)` instead of raw `L` (and the
  `C_max(Lr,h)` table built over an Lr grid, OkLab `L` recovered per row by the inverse remap before
  `Oklab→XYZ`); reconstruction still preserves `L`. Bit-exact to the oracle (gate `max_abs 1.055e-14`
  / probes `1.221e-15`), gated by **`test_gamut_out_oklrab`** (golden at oracle `27bd085`). Commits
  `9cb0a0b` golden / `94d2274` engine+ci / `e1b75d8` app on `claude/exciting-hamilton-hya62`.
- **Host parity suite was 35 gates then** (now 36 — see Current state above), all green (argv
  authoritative in `.github/workflows/ci.yml`);
  `SPK_NUM_THREADS` 1≡8 byte-identical (oklrab compress is serial+stateless); NDK r27 3-ABI build
  path unchanged. App **0.9.0 / versionCode 11** (bumped for the 0.9.0 release, issue #129).
- **This branch now carries the unmerged oklrab commits (slice 2) on top of `origin/main` + the
  `1174fd8` docs commit.** Open a NEW draft PR for them; the remote branch auto-deletes on merge and
  recreates with a plain push. Never stack new work on already-merged history.

## Next — P2 #6 slice 3: `jzazbz` (then slice 4 `cam16ucs`)

Slice 2 `oklrab` is DONE (see the state block above). Clone the same pattern; the templates are now
`tools/parity/gen_gamut_oklrab_golden.py`, `model/gamut_compression.{h,cpp}` (oklch + oklrab
sections), and `tests/test_gamut_out_oklrab.cpp` + its ci.yml argv. **`jzazbz` is harder than
oklrab** — it is NOT a simple L-remap: it needs a JzAzBz forward/inverse (PQ encoding + matrices,
absolute-luminance scaled by `_JZAZBZ_Y_W_CDM2 = 100` cd/m²) and its OWN C_max table geometry
(`L_grid=linspace(0.002, 0.18, 64)`, `chroma_initial_upper=0.3`) plus a per-space Jz-white
normalizer for the lightness knee (`_jzazbz_white_Jz`). See oracle `compress_rgb_jzazbz_chroma` +
the `"jzazbz"` branch of `_get_output_c_max_table` in `utils/gamut_compression.py`.
- **Golden:** new `gen_gamut_oklrab_golden.py`-clone → `gen_gamut_jzazbz_golden.py`; call
  `gc.compress_rgb_jzazbz_chroma`; generate at oracle `27bd085` (already checked out) and pin the SHA.
- **C++:** port JzAzBz forward/inverse into `model/gamut_compression.cpp` (capture the colour-science
  matrices/constants as bit-exact hex, as oklch did for OkLab); enum slot `kJzazbz=5` is reserved.
- **Gate:** `test_gamut_out_jzazbz` + its `ci.yml` `build_run … tests/gamut_jzazbz_cases.bin` line
  (bumps the suite to 37 — `lut_cache_e2e` already made it 36; also sync
  `tools/parity/run_engine_parity.sh`'s table or its drift guard fails). Add `gamut_out_jzazbz`
  to the enumerated lists in CLAUDE.md + the skills.
- **Facade/UI:** add `JZAZBZ` to `enum class OutputGamutCompress` (+ the exhaustive `when` in
  MainActivity — Kotlin will error if you forget) and the Output-gamut dropdown.
- Then **`cam16ucs`** (`kCam16ucs=6`, the heaviest — full CIECAM16 forward/inverse). Default upstream.

Per increment: default path byte-identical, opt-in/default-OFF, feature-on within tol
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`), `SPK_NUM_THREADS` 1≡8, NDK r27 3-ABI build, commit+push on green.
**Ship ONE algo per PR** — subagents died on token limits when given more, so keep each unit small.

### Also open (unchanged)
- **Strategy-B rebaseline cluster** (`PRIORITY_ROADMAP` #20-27; incl. CAT02→CAT16 + xy-clip removal)
  — one coordinated baseline bump; trigger NOT fired (upstream WB-norm `e301791`/`526e200` still
  churning on `reflectance-upsampling-methods`, checked 2026-07-01). Keep the `c1d0e44` pin.
- **Device-gated queue** (user tests on his SM-S948W/Android 16): R8 0.8.0 release smoke; GPU-LUT
  re-arm feel; the E2 print-route look change (film character now in prints — intentional, eyeball
  it); AUDIT §A param-wiring UX decisions.
- **MALLETT2019** — disclosed as a GatedBlock; implement-vs-remove decision still open (`#18`).

---

## Evergreen operating notes (read once per session)

- **Container-reset recovery** (drilled 5+ times): the env re-clones to a stale commit mid-session.
  Recover via `git fetch origin main` (and the branch) → `git remote prune origin` → verify pushed
  work is on origin → `git reset --hard <ref>`. Untracked new files SURVIVE `reset --hard`; tracked
  edits do NOT. Rule: `git add && git commit -c commit.gpgsign=false && git push` the instant a unit
  builds green. `/tmp` and pip envs do not persist.
- **Proxy-desync recovery:** the local git proxy can return a stale snapshot and refuse
  `git fetch origin <branch>` by name — `git fetch origin <full-sha>` or `refs/pull/<N>/head` still
  works → `git reset --hard FETCH_HEAD`. Once a PR is merged the work is safe on real GitHub.
- **PR/branch lifecycle:** the remote branch auto-deletes on merge — recreate with a plain
  `git push` (`--force-with-lease` fails 'stale info'; `git fetch --prune` first). After a merge,
  restart from origin/main and open a NEW PR (never stack on merged history). The user may merge
  mid-session and webhooks don't deliver merges — re-check PR state before pushing. Merging is
  policy-gated (explicit user go-ahead); tag-push releases allowed when asked.
- **Oracle setup:** local clone at `/home/user/spektrafilm`; env = system python3.11 with
  `PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs` (stubs mock heavy IO deps). **e2e /
  param-wiring goldens pinned at `c1d0e44`** (upstream drift began at `a9bccd6` — never regenerate
  from tip); **gamut primitive goldens generated at `27bd085`**. Checkout the pin SHA before
  generating, restore the branch after; new gen scripts must pin the SHA they generate at.
- **Parity gate: 38 host tests**; per-test argv is authoritative in `.github/workflows/ci.yml`
  (copy, never guess) — any doc citing 15/26/31/33/34/35/36 gates is stale. Every engine change: default
  path byte-identical, feature-on within tol, `SPK_NUM_THREADS` 1≡8, NDK r27 3-ABI build green. All
  new engine features ship opt-in / default-OFF.
- **Land engine fixes ONE AT A TIME**, one small item per subagent — parallel agents collide on the
  shared engine files and the PR, and larger tasks blew the subagents' token limits mid-run.
- **`-fno-finite-math-only` is required** (scanning relies on NaN propagation through
  `density_to_light`); **GPU is preview-only, NEVER export** (vendor-varying float, float64 expose
  integrals, implementation-defined NaN handling).
- **Build distributable debug APKs with plain `./gradlew :app:assembleDebug`** — NEVER
  `-Pandroid.injected.build.abi` (stamps `android:testOnly`, blocks tap-install, moves output to
  `intermediates/`). **R8/minified release is NOT exercised by CI** — smoke-test on-device before
  tagging (last validated 2026-06-04 on SM-S948W/Android 16).
- **User directives on record:** do NOT modify `.github/workflows/` ('everything works there'); do
  NOT convert `.lut`→`.bin` (measured net-negative); **GPLv3 attribution "Film modeling powered by
  spektrafilm" must stay**; never put the model identifier in committed artifacts.
- **Toolchain** at `/opt/android-sdk` (NDK 27.0.12077973, CMake 3.22.1, build-tools 35.0.0) may not
  persist across containers — reinstall via `sdkmanager` if gone; `local.properties` is gitignored.
- **Kotlin/UI-only changes never touch the parity suite.** Post-engine grades and masks composite
  once, in-place on `res.data` via `simResultToBitmapGraded` right after simulate — never inside
  `simResultToBitmap` (the export site feeds `res` to both the bitmap and the 16-bit writers, so
  consumer-side mutation double-applies).
- **Engine param honesty:** presets/UI must set only engine-honored fields (halation via
  `halationAmount`/`scatterAmount`/`boostEv` — `halationStrength`/`halationFirstSigmaUm` are baked
  per-profile and ignored); params threaded only inside conditional blocks (e.g. `if(spatial)`) get
  silently dropped on the default path — thread unconditionally and fold into the relevant cache keys.
- **Perf medians are container-specific** — never compare benchmark numbers across boxes (the older
  2-core numbers are not comparable with the current 4-core ones).
- **CI flake:** the android job intermittently fails setup-android with 'Error on ZipFile unknown
  archive' (corrupt SDK download) — not a code failure; re-run the job.
- **Orphaned commit:** §6g ProfileValidator was committed as `660d33a` and pushed but never merged
  (slipped #102, force-dropped from #103) — re-land it if profile import is prioritized.
- **The user is Akshay Sharma**, the app's author (pixls.us megathread), testing on a Galaxy S26
  Ultra (SM-S948W, Android 16, arm64) — device-gated items queue until he tests. His laptop env
  (adb device testing): working copy `C:\Filmcam123\Spectrafilmandroid` (`C:\Spectrafilm` is
  docs-only — a trap); oracle = Python 3.13 venv `C:\Filmcam123\spkenv` + `spkstubs`; arm64 test
  binaries at `C:\Filmcam123\spk_arm64`; `JAVA_HOME` = Android Studio jbr JDK 21.
- `docs/PRIORITY_ROADMAP_2026-06-24.md` defines the P0–P3 item numbering (#1–#27) used throughout
  (P2 #6 = perceptual gamut algos, #18 = MALLETT2019, #20-27 = Strategy-B rebaseline cluster).

---

## Session history (compressed; full prose in this file's git history)

- **2026-08-26 — fork engine adoption (#117/#118/#125).** Adopted the verified
  VirtuaTOA fork overlay: grain parallelization (fixed 8192-px blocks + SplitMix64 per-block
  seeds, dynamic scheduling; #118-verified 12 MP 114.8→35.2 s / 3.1 MP 45.1→11.7 s at 1→8
  workers on a 4-core container, 12 MP memcmp-identical), spectral 3D-LUT memo
  (`kernels/lut3d_cache`), debug `-O2` guard, AE-off + sRGB-shaped `spk_bake_cube_lut`,
  `spk_meter_exposure_ev` API, `run_engine_parity.sh`. Kept OUR `kernels/parallel` +
  `test_parallel` (959e786 gate). Hardened memo keys (length-prefixed segments). New gates:
  multi-block grain scenario in `test_parallel`, shaper property case in `test_bake_lut`;
  `test_lut_cache_e2e` added to ci.yml (owner-authorized single line) — suite 35→36, ALL OK.
  App: GPU preview bakes SHAPER_SRGB + shader-side sRGB encode + metered `uExposureGain`;
  exports stay SHAPER_NONE. Accepted change: grain field differs vs pre-adoption releases
  (deterministic + thread-invariant still).
- **2026-07-02 — P2 #6 slice 2: `oklrab` output-gamut compression (new draft PR, unmerged).** Cloned
  the merged `oklch` pattern: `compress_rgb_oklrab_chroma` = the oklch chroma reduction with the
  `C_max` lookup indexed by Ottosson 2023's rebased lightness `Lr = f(L)` (constants k1=0.206,
  k2=0.03, k3=(1+k1)/(1+k2)); the `C_max(Lr,h)` bisection table is built over an Lr grid with each
  row's OkLab `L` recovered by the inverse remap before `Oklab→XYZ`, and reconstruction preserves the
  original (lightness-compressed) `L`. Reuses oklch's OkLab/RGB↔XYZ hex constants, `cmax_lookup`,
  `reinhard_knee`; table built locally per call (thread-invariant, warm==cold). Golden
  `gen_gamut_oklrab_golden.py` @ oracle `27bd085` (24 cases / 1152 px); gate `test_gamut_out_oklrab`
  `max_abs 1.055e-14` / probes `1.221e-15`. Suite 34→35, defaults byte-identical, oklch/aces/
  output_spaces/simulate_e2e/test_parallel unchanged. Facade `OKLRAB`=4 + Output-gamut dropdown
  ("Oklrab (perceptual, even lightness)"); `:app:compileDebugKotlin` green. Commits `9cb0a0b` /
  `94d2274` / `e1b75d8`.
- **2026-07-02 — "exact + fast" pass (PR #109 + #110).** F1–F7 Kotlin fixes; **E1** per-effect
  spatial gates (`test_spatial_decouple_e2e`, golden `scan_portra_lensblur_nohalation`); **E2**
  print-route filming spatial + grain (`test_print_spatial_e2e`, golden `print_portra_spatial`);
  **S1** scan-route film-density memo + per-param key-completeness tests; **S2** print-density memo
  (keyed on film_density_cmy CONTENT ⊕ printing inputs ⊕ the tc_lut-shaping film params); **S3**
  Kotlin retained-result grade cache (grade-only edits = zero native work); **S4** DIR-develop +
  exposure-interp + expose-tail loops → deterministic `parallel_for`. Both gamut e9e70f8 goldens
  ACCEPTED. Perf (4-core, 8 threads, 512²): cold scan **211 ms** (−13% from S4); warm scan / output-
  only edit 144–159; cold print ~400; warm print y-shift / output-only 153–162 (film + print memo →
  `scan()` alone).
- **2026-06-24 — P2 #5/#7 gamut + #8/#9 + P3 quick-wins (PR #109).** Output ACES-RGC v1.3
  (`test_gamut_out_aces`) + input radial-to-locus xy tc_lut bake (`test_gamut_in_xy`), both
  default-OFF, goldens @ `27bd085`; gamut flags → JNI → facade → two Simulation→Output dropdowns;
  `input_gamut_compress` folded into the tc_lut + film-memo keys only when active. Preset/diagnostics
  IO off-main; undo restoring-flag window fix; P3 quick-wins #10-16. SCOPE finding: CAT02→CAT16 +
  xy-clip removal are UNCONDITIONAL default-path changes (Strategy-B), NOT the opt-in locus bake.
- **2026-06-09 — WB wave + v0.8.0 release prep (PR #103).** Gray-point eyedropper, "Balance to film
  stock" (virtual-85, Bradford-adapt of the profile `reference_illuminant` CCT), auto-exposure
  default ON (matches upstream). versionCode 9→10, versionName 0.7.0→0.8.0. Scanner white/black
  correction gated to the strict-no-op case in UI.
- **2026-06-08 — masking + color/tone foundation (PRs #90–#103).** The keystone arc, all device-
  confirmed: §2 P0 color management (display tag + wide-gamut + ICC embed), §3.1 Contrast, §3.2
  Sat/Vibrance (Oklab post-engine grade), §3.3 couplers relabel, §2 P1 ACES gamut slider (post-
  engine v1), the full masking system (radial/linear/luminance/color-range masks → per-mask Tier-A
  Temp/Tint/Exp/Sat/Hue/Contrast/Whites/Blacks → draw-on-preview overlay → Class-S spatial ops,
  13 of LR's ~14 local ops), CLF/`.cube` LUT export (17/33/65), Lightroom-style export sheet
  (JPEG/UltraHDR/PNG16/TIFF16/TIFF32F/scene-linear), onboarding + slide-mode, the
  spectrafilm-solutions skill + `docs/USER_DRIVEN_SOLUTIONS.md`, and the full Lightroom RE
  (`docs/lightroom-re/`). Zero engine C++ changes in this arc. Design rule established: post-engine
  grades composite once in-place on `res.data`, mask ordinals pinned to crs:MaskBlendMode for XMP
  interop, gray-neutrality from Oklab/Rec-709 rows summing to 1.
- **2026-06-05 — editor + preview-speed wave (PRs #82–#88).** Point tone-curve editor (#88, faithful
  Fritsch–Carlson monotone-cubic Kotlin port); Lightroom UX + draft/final render worker + zoom ROI
  (#85/#86); highlight-boost ported (#82, `diffusion.cpp::apply_highlight_boost`, golden
  `scan_portra_boost` @ `c1d0e44`, `test_highlight_boost_e2e`); half→float LUT-load speedup (#83).
  GPU fit-preview promoted then reverted (broke the editor on SM-S948W). brutalist-re skill added.
- **2026-06-05 — param-wiring audit + print EV-comp (PRs #77–#80).** Downscale AA-prefilter parity
  fix (#77, real ~0.18–0.4 bug); print EV-compensation midgray fix (#80, `runtime/print_digest.cpp`,
  goldens `print_portra_evcomp{,_nonorm}`); R8 on-device validation recorded (#79). Opened the
  5-finding audit ledger (all since closed: boost→#82, MALLETT disclosed, spatial→E1, print→E2,
  dead sliders disclosed).
- **2026-06-04 — oracle pin + inert params + positive-film coupler (PRs #67–#76).** Oracle PINNED at
  **`c1d0e44`** (#67; drift = `a9bccd6`, changed filming raw-scaling). Wired all inert marshalled
  params: spectral blur (#68), hanatos window/surface (#69, surface = per-cell degree-4 2D poly),
  camera UV/IR (#72), enlarger preflash (#73, print-route only, NOT in the film-memo key), scanner
  white/black (#74, new `runtime/color_reference`). Positive-film DIR coupler fix (#75: per-stock
  provia/velvia gamma overrides; ~0.32 divergence on scan_film with couplers ON).
- **2026-06-03 — audit + lifecycle + zoom (PR #60).** Removed stale committed `dist/` APKs + closed
  the ICC license gap; process-scoped `EngineHolder` singleton (immutable engine never closed mid-
  life); profile+tc_lut memo keyed on immutable profile id (byte-exact); Lightroom ROI zoom. GPU
  standing verdict recorded: preview-only accelerator, never export.
- **2026-06-02 — v0.7.0 released.** `release.yml` published the signed APK + `.sha256`; apksigner
  verify passed. Workflow: feature branch → PR → merge (policy-gated); tag-push releases on request.
- **v0.7.0 session — engine completion (PR #59, Windows laptop + Galaxy S25 over adb).** AAssetManager
  direct-load (`SpektraEngine.fromAssets`, `#ifdef __ANDROID__`, skips the ~17 MB first-run extract)
  and `use_enlarger_lut` wired (opt-in PCHIP LUT mirroring the scanner LUT, `test_enlarger_lut_e2e`)
  — last reserved engine LUT flag gone. On-device parity runner: NDK clang `--target=aarch64-linux-
  android24`, push test + `libc++_shared.so` + assets + goldens, run under adb (`max_abs 5.96e-08`).
- **RAW export OOM (PR #56, device-confirmed v0.6.3).** Full-res RAW input + engine output moved off
  the ART managed heap via `malloc` + `NewDirectByteBuffer`; `LinearImage`/`SimResult` made
  `AutoCloseable`. Root cause: `ByteBuffer.allocateDirect` is a non-movable `byte[]` on the ~256 MB
  managed heap, not native memory — two ~140 MB full-res buffers cannot coexist there.
- **Since v0.4.0 (merged).** LR-RE feature wave (#35–#42 preset amount / copy-paste / resets /
  tone-curve stage), MotionCam `.mcraw` parser (#37/#38), perf scaffolding all opt-in/off (#46–#52:
  Vulkan compute + SPIR-V scan port, fp16 NEON, oneTBB, LiteRT stub), big-file RAW fixes
  (#43/#44/#56), Neutral (Adobe-like) preset (#55). GPU speedup remains UNPROVEN/hardware-blocked.

## Doc map (what to read for what)

`CLAUDE.md` build/parity/arch · `docs/AUDIT.md` open items + severity · `CHANGELOG.md` release notes ·
`docs/PRIORITY_ROADMAP_2026-06-24.md` the #1–#27 priority numbering ·
`docs/UPSTREAM_SYNC_2026-06-24.md` Strategy-A/B port plan · `docs/IMPROVEMENT_BACKLOG.md` LR-RE'd
feature list · `docs/PERF_ROADMAP.md` perf plan+policy · `docs/USER_DRIVEN_SOLUTIONS.md` +
`.claude/skills/spectrafilm-solutions/` the user-need catalog · `docs/RESEARCH_*` / `docs/lightroom-re/`
RE studies · `docs/research/` settled deep-dives (gpu-bit-exact.md, highway-vendoring.md) ·
`docs/PRESETS.md` / `docs/FILM_STOCKS.md` content · `docs/maps/` source-project maps.
