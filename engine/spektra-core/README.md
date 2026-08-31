# engine:spektra-core

Native (C++/NDK) port of spektrafilm's `runtime` + `model` packages, exposed to the app through
a thin JNI + Kotlin facade.

> **Status: shipped.** The engine is fully implemented, bit-exact against the spektrafilm
> oracle (golden-vector parity harness, `max_abs ≤ 1e-4` / `rms ≤ 1e-5`, byte-identical across
> thread counts), NEON-accelerated (`kernels/exp10.h`), and ships in released APKs (v0.7.0+).
> The C++ header, JNI bridge, and Kotlin facade/params are the stable contract; see
> `../../docs/PORTING_PLAN.md` for the upstream module map and the parity strategy.

Native allocation tokens, data leases, checked direct-buffer windows, cancellation checkpoints,
and render/close verification are specified in
[`docs/JNI_LIFETIME_SAFETY.md`](../../docs/JNI_LIFETIME_SAFETY.md).

## Layout

```
spektra-core/
├── build.gradle.kts                     # externalNativeBuild(CMake)
├── src/main/cpp/
│   ├── CMakeLists.txt                   # builds libspektra.so
│   ├── spektra.h                        # ★ engine C API (the contract)
│   ├── spektra.cpp                      # engine entry (simulate / simulate_preview)
│   ├── spektra_jni.cpp                  # JNI ↔ C API bridge
│   ├── model/                           # density_curves, couplers, grain, diffusion, ...
│   ├── runtime/                         # pipeline + stages/ (filming/printing/scanning,
│   │                                    #   crop_resize, autoexposure)
│   ├── kernels/                         # gaussian, interp/lut3d, stats, exp10, parallel, half
│   ├── io/                              # npy/.lut asset loaders
│   ├── profiles/                        # profile JSON loaders
│   └── tests/                           # host g++ parity tests (see CLAUDE.md)
├── src/main/kotlin/com/spectrafilm/engine/
│   ├── SpektraEngine.kt                 # ★ Kotlin facade (loads .so, marshals buffers)
│   └── SpektraParams.kt                 # ★ params mirror of RuntimePhotoParams
└── src/main/assets/spektra/             # profiles/ luts/ filters/ icc/  (see docs/ASSETS.md)
```

## Contract

- C API: see [`src/main/cpp/spektra.h`](src/main/cpp/spektra.h).
- Kotlin: [`SpektraEngine`](src/main/kotlin/com/spectrafilm/engine/SpektraEngine.kt),
  [`SpektraParams`](src/main/kotlin/com/spectrafilm/engine/SpektraParams.kt).

The Kotlin/C API mirror spektrafilm's `simulate(image, params)` /
`simulate_preview(image, params)` and the `RuntimePhotoParams` dataclass tree so behavior can be
checked stage-by-stage against the upstream Python.
