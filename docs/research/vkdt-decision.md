# The vkdt question, costed against measured numbers

*Owner asked for this after the export profile came in. It is a decision document,
not a plan — nothing here is committed to or started.*

GPLv3 throughout. Film modeling powered by spektrafilm (GPLv3).

## 1. Why the question is live now, and what changed

For most of this project the performance work looked inside the engine because the
engine is what the parity gate covers. The export profile ended that: **67% is the
engine, 29% is the decoder, and 4% was everything else** — and that last 4% has now
been taken (`docs/research/perf-lab.md` §17.5, ~1.6 s recovered, none of it
parity-gated, none of it needing a GPU).

So the cheap work is done. What is left, on a 12.5 MP export:

```
  simulate   8150 ms   66.9%    the engine — parity-gated
  process    2760 ms   22.6%    dcraw_process, inside decode
  copy        223 ms    1.8%
  unaccounted 287 ms    2.4%    inside decode
  rest       ~770 ms    6.3%    fileread, unpack, memimg, grade, encode, adapt, colour
  ------------------------------
  total     12189 ms
```

The target is 1–2 s. **That is the whole reason this question exists**: no arrangement
of the non-engine work reaches it.

## 2. The arithmetic a rewrite would have to beat

Take the most favourable honest case for staying put — OpenMP lands well and `process`
drops 4×, `copy` and the unaccounted 287 ms both get the same treatment `grade` just
got:

```
  simulate   8150 ms    unchanged — CPU
  process     690 ms    optimistic 4x from OpenMP
  everything else ~900 ms
  -------------------
  total     ~9740 ms
```

**~9.7 s, and 84% of it is the engine.** Incremental CPU work cannot reach 1–2 s, and
this is not a close call. Either the engine moves to the GPU or the target moves.

That is the strongest form of the case for a GPU pipeline, and it is a real one.

## 3. The parity problem — state it exactly, because it is the crux

The prime directive is parity with the spektrafilm oracle, defined in `CLAUDE.md` as two
separate things:

1. **within the band** — `max_abs ≤ 1e-4`, `rms ≤ 1e-5` against the oracle;
2. **byte-identical across thread counts** (`SPK_NUM_THREADS` 1 ≡ 8).

A GPU pipeline can satisfy (1). We have measured it: the scan and print-expose offloads
land at **3.1e-06** against the CPU export — 32× inside the band. GPU float is not the
obstacle people assume.

(2) is where it breaks, and it breaks in a way worth being precise about. The
thread-invariance contract exists because the CPU fork-join splits work into chunks and
we require the split not to change the answer. On a GPU the equivalent guarantee — that
the result does not depend on workgroup scheduling — holds for pointwise and
row-independent work, and does **not** hold for anything with a reduction or a
cross-lane dependency unless it is written to. It is a property of each kernel, not of
"the GPU".

So the honest framing is not *"parity dies"*. It is: **the gate would have to be
re-derived per stage**, and one gate — grain's — is already statistical rather than
byte-exact, which is why grain keeps coming up as the least-obstructed target.

## 4. What we actually know about the GPU here, measured

| | result | source |
|---|---|---|
| scan + print-expose offload correctness | 3.1e-06 vs CPU | perf-lab, `test_gpu_host` |
| GPU preview offload speed, 0.195 MP | 1.00× | #146 |
| GPU preview offload speed, 0.78 MP | 0.95× | #146 |
| GPU preview offload speed, 3.12 MP | 1.007× (slower) | #146 |

That is a **null result across a 16× range**, and it is the single most important
counterweight to the rewrite case. The GPU path is genuinely active and genuinely
correct, and it bought nothing — because `scan` is a small fraction of a frame while
grain and halation dominate and stayed on the CPU.

**It does not follow that a full GPU engine would also buy nothing.** #146 offloaded
~1 s of an 8.1 s engine. But it does mean the rewrite case currently rests on *zero*
measurements of the GPU doing the work that actually costs.

## 5. The gap, and the experiment that closes it

**Nobody has measured GPU against CPU on a big engine stage at full resolution.**

That is a startling thing to be true before contemplating a rewrite, and it is cheap to
fix, because the machinery already exists: `gpu/vulkan_compute.cpp` is a persistent
Vulkan compute host, already validated, already shipping behind a toggle.

The decisive experiment is **one stage, at export resolution**:

- **grain — 3384 ms, 42% of the engine.** The largest single stage, and the one whose
  parity gate (`test_grain`, `test_grain_sublayer`) is **already statistical** — mean
  and noise-std against a committed reference, not bytes. So a GPU implementation can be
  validated by the gate that already exists rather than needing a new contract.
- halation (1905 ms) is second and is a separable IIR blur, the classic GPU shape.

If GPU grain at 12.5 MP comes back at 10×, the engine is 8150 → ~5100 ms and the rewrite
case is overwhelming. If it comes back at 2×, the case collapses, because a full rewrite
buys maybe 2× on 67% of the export and costs months.

**We do not currently know which.** That is the whole decision, and it costs one stage
to find out.

## 6. What vkdt would fix, and what it would not

Recorded honestly, and flagging where this needs primary-source verification rather than
recollection.

**Would fix.** A node-graph GPU pipeline puts demosaic on the GPU, which is the 2760 ms
`dcraw_process`. It also removes the CPU↔GPU round trips that a partial offload pays,
which is a real cost our incremental path keeps re-paying.

**Would not fix.** It does not answer §5's question either — it *assumes* the answer.
Adopting vkdt is a bet that GPU grain, GPU halation and GPU DIR-couplers are fast, made
without measuring any of them.

**Would cost.** The parity gate is built around our CPU stage structure. `engine-parity`
recompiles 78 files 38 times, twice over (`-O2` and shipping flags); a node-graph engine
does not slot into that. Every stage's gate would need re-deriving, and §3 says that is
per-kernel work, not a one-time policy change.

**Verify before relying on any of this:** vkdt's actual licence terms as vendored, what
its graph does and does not let you pin numerically, and whether its demosaic is one we
would accept as a *quality* matter — that last one is the same `user_qual` question in
different clothes, and it is the owner's call, not an engineering one.

## 7. Recommendation

**Do not decide vkdt from principle. Measure GPU grain at export resolution first.**

The rewrite case is real — §2's arithmetic is not escapable, and 84%-engine at ~9.7 s is
the honest ceiling of the incremental path. But the case currently rests on an untested
assumption, and the one measurement that would test it is cheap, uses machinery that
already exists, and targets the stage with the least parity friction in the entire
engine.

If that number is good, this document becomes the plan. If it is bad, it saves months.

Either way it should be taken before the commitment, not after — which is the same
discipline that retired the Highway f64 tier, the "Use performance cores" setting, and
the first cut of the 90/270 transpose.

## 8. Reading the actual source: vkdt already implements spektrafilm — on a different grid

Checked against a clone of `hanatos/vkdt` at `b95b3a0`, not from recollection.

**`src/pipe/modules/filmsim/readme.md`, first line:** *"this is an implementation of
Andrea Volpato's spektrafilm."* Same upstream our engine is a port of. The module has
our whole pipeline as Vulkan compute shaders — `expose.comp`, `develop.comp`,
`scatter.comp`, `halin/halout.comp`, `dirlut.comp` (DIR couplers), `negprint.comp`,
`print.glsl`, `scan.glsl` — and its parameter list maps almost one-to-one onto ours,
down to `preflash`, `cp amt`, `lang r/g/b`, `cp rad`, `hal bnc`, `hal dec`, `scat amt`,
`g fast`, `g slow`, `exhaust`, `hl boost`. Those are the same features our
`preflash_e2e`, `provia_couplers_e2e` and `highlight_boost_e2e` gates cover.

The whole module is **1979 lines**, because most of the math is precomputed into
`filmsim.lut` by `mklut-profiles.py` — which runs against spektrafilm's own
`profile/*.json`.

**Licence is fine.** vkdt's code is 2-clause BSD (13 files carry GPL, none in filmsim),
which is compatible with our GPLv3. The filmsim *profiles* are CC BY-SA 4.0 by Andrea
Volpato — the same data-licensing situation we are already in.

### But it is not our model, and the readme understates by how much

`filmsim.glsl` declares its wavelength grid outright:

```
  lambda_arr[11] = 380, 390, 400 ... 800, 810      // 44 bands @ 10 nm
```

Ours, from `model/spectral.h`: *"Mirrors spektrafilm/config.py exactly:
SpectralShape(380, 780, 5), i.e. 380..780 nm @ 5 nm -> 81 samples. This governs every
spectral..."*

**vkdt runs the spectral integrals at half our resolution** — 44 bands at 10 nm against
81 at 5 nm — and over a different range. That is a different quadrature in *every*
spectral step: camera-raw formation, dye density to transmittance, enlarger exposure,
scan to XYZ. It will not sit inside `max_abs ≤ 1e-4`; it is not a precision question but
a different discretisation.

The readme discloses "some changes for efficiency (for instance the grain model was
swapped out)". The 5 nm → 10 nm halving is a larger change than the grain swap and is
not named there. Not a criticism of vkdt — it is a real-time editor and the trade is
obviously right for it — but it is decisive for us.

Two smaller structural differences, recorded for completeness:

- **Base-2 density math.** vkdt uses `exp2(-ds)`; our engine has a dedicated
  `kernels/exp10.h` because spektrafilm's densities are base-10. Convertible in
  principle, a different numerical path in practice.
- **Subgroup reductions.** `filmsim.glsl` reduces with `subgroupAdd`. Summation order
  depends on `gl_SubgroupSize`, which differs between GPU vendors — so results would
  vary by *device*, not just from the CPU. Our contract currently requires invariance
  across thread counts; this would add a device axis we have never had to defend.

### What this changes about the decision

It removes the vaguest version of the question and replaces it with a sharp one.

**"Adopt vkdt's filmsim" now means "ship a 44-band model instead of an 81-band one."**
That is a *product* decision about output, not an engineering one about speed, and it is
the owner's to make — the same class as `user_qual` in #158, and much bigger.

The third route is now the well-founded one rather than the speculative one:

> **Write our own 81-band GPU shaders, using vkdt's module as the architecture guide.**

vkdt has already proven the pipeline maps onto Vulkan compute, shown how to lay out the
LUT, and shown which stages decompose into which kernels — at 1979 readable lines under
a compatible licence. That is an enormous head start on design while leaving our grid,
our grain model, and our parity gate intact.

What it does not tell us is still §5's question: what GPU actually buys on our biggest
stage. Nothing here substitutes for measuring that.
