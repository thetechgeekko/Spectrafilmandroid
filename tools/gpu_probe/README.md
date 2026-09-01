# GPU device probe (#135 E3 → #127; M2 filming/printing → #147)

Standalone arm64 probes that measure the fp32 Vulkan per-pixel engine kernels
against f64 CPU references mirroring each shader 1:1, on real device hardware.
Produces the "does fp32 sit inside the oracle tolerance (`max_abs ≤ 1e-4`,
`rms ≤ 1e-5`)" numbers that `docs/research/gpu-bit-exact.md` §10.3 calls the
decider for #127 / option B. **Historical scope:** these probe binaries wire nothing into the app.
Their results later supported a separately qualified resident Fast GPU product route, so the old
preview-only policy is superseded. Strict Exact CPU remains the parity-bearing fallback; this probe
alone does not qualify spatial/stochastic work, other devices, or export performance.

Two binaries:
- `gpu_probe` (M1, #135 E3): the SCAN integral via the **unmodified**
  `spk::gpu::scan_spectral` host + vendored `engine/.../gpu/scan_spectral.comp`.
- `gpu_probe_m2` (M2, #147): the FILMING chain (ProPhoto→tc/b → Mitchell-cubic
  tc_lut interp → log10 → density-curve interp → pointwise DIR couplers) and the
  PRINTING chain (film CMY → 81-band dichroic-filtered spectral integral →
  midgray/round-trip → paper density-curve interp), as **probe-local** shaders
  `filming.comp` / `printing.comp` — engine sources stay byte-untouched. Tables
  are folded on the host through the engine's own builders
  (`build_filming_tc_lut`, `normalize_density_curves`,
  `compute_dir_couplers_matrix`+`np_interp_array`, `digest_printing_params`+
  `resolve_neutral_cc`+`compute_midgray_exposure_factor`); each run also
  executes the REAL engine stage on-device and prints mirror-vs-engine (CHAIN),
  engine-vs-golden (SETUP, must PASS) and GPU-vs-mirror/engine/golden numbers.

Run (phone on USB, NDK r27 installed):

```bash
bash tools/gpu_probe/build_push_run.sh
```

- Tier 0 `caps`: device identity, `shaderFloat64/16`, the full float-controls block.
- Tier 1 `run`: golden density plane + 64³ CMY sweep + NaN case; `max_abs`/`rms`
  vs f64, determinism ×5 (byte-compare).
- Tier 2 `perf`: warm-call wall time at 0.3 MP / 12 MP (includes the host's
  per-call buffer+pipeline rebuild — offload cost, not pure kernel time).
- Tier 3 (auto, needs glslc): `precise` (NoContraction) and `mediump`
  (RelaxedPrecision) shader variants, same Tier 1 run.
- M2 (needs glslc): `film` + `print` subcommands, golden inputs
  (`tools/parity/goldens/{scan,print}_portra`) + 64³ sweeps + NaN case,
  determinism ×5, and the same `precise`/`mediump` brackets
  (`gpu_probe_m2_precise` / `_mediump`).

Scan tables are extracted through the engine's own loaders/constants and folded
per the `gpu/vulkan_compute.h` contract (base density + illuminant +
normalization into `icmf`; NaN bands zeroed — the engine's w=NaN→0 semantics).
Results land in `tools/gpu_probe/captures/` (untracked); the committed writeup
is `docs/research/gpu-device-probe.md`.

Film modeling powered by spektrafilm (GPLv3).
