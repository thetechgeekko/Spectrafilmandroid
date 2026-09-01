# engine:spektra-core

Native (C++/NDK) port of spektrafilm's `runtime` + `model` packages, exposed to the app through
a thin JNI + Kotlin facade.

> **Status: shipped for the pinned reviewed baseline.** The engine passes oracle tolerance
> (`max_abs ≤ 1e-4`, `rms ≤ 1e-5`) and same-build worker-count identity; this is not universal
> cross-build/ABI/CPU/GPU byte identity. Latest-upstream coverage remains owned by the parity-manifest
> ticket. The C++ header, JNI bridge, and Kotlin facade/params are the current boundary; see
> [`docs/EXECUTION_INDEX.md`](../../docs/EXECUTION_INDEX.md) for authority and
> [`docs/BIT_IDENTICAL_EXPORT_ROADMAP.md`](../../docs/BIT_IDENTICAL_EXPORT_ROADMAP.md) for the
> numeric contract.

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
