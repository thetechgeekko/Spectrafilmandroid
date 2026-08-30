# Spektrafilm for Android — native engine build.
# GPLv3. Builds libspektra.so (engine + JNI bridge).
cmake_minimum_required(VERSION 3.22.1)
project(spektra LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release: optimize for the per-pixel spectral loops. -ffast-math speeds the 81-band
# integration; -fno-finite-math-only is REQUIRED — the scanning stage relies on NaN
# propagation through density_to_light to match spektrafilm's profile null handling.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -fno-finite-math-only")
# DEBUG BUILDS MUST STILL BE OPTIMISED. CMake's default CMAKE_CXX_FLAGS_DEBUG is
# "-g" with NO -O flag, i.e. -O0. For a numeric engine that is 3-10x slower, which
# makes every on-device timing from a debug APK meaningless — a full-resolution
# export measured minutes on device while the same work took seconds on a host
# build, purely because of this. -O2 matches the flags the host parity suite is
# validated with (release uses -O3 -ffast-math, the documented divergence), so a
# debug APK now behaves numerically like the parity build and performs sensibly.
if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif()


# EVERY SOURCE MUST BE LISTED HERE BY HAND. The host parity build compiles with a
# glob (`kernels/*.cpp model/*.cpp ...` — see the build line in CLAUDE.md), so a new
# .cpp picked up there is NOT picked up here. A file missing from this list therefore
# passes the whole 39-gate parity suite locally and fails only in the Android
# `ninja ... spektra` step, which is a slow and confusing way to find out. That is
# exactly how kernels/fft.cpp and kernels/fft_convolve.cpp broke CI on 551c57f.
add_library(spektra SHARED
        spektra.cpp
        spektra_jni.cpp
        # Kernels:
        kernels/interp.cpp
        kernels/gaussian.cpp
        kernels/gaussian_hwy.cpp            # Highway SIMD FIR (compiles to nothing unless SPK_ENABLE_HIGHWAY)
        kernels/exponential_filter.cpp      # float64 Gaussian + exponential mixture (spatial)
        kernels/spectral_upsampling.cpp     # Hanatos2025 RGB->spectrum (M3)
        kernels/stats.cpp                   # Poisson + Binomial samplers (grain, M6)
        kernels/lut3d.cpp                   # opt-in PCHIP 3D LUT accel (M7, default OFF)
        kernels/lut3d_cache.cpp             # engine memo for built 3D LUTs (PERF, byte-identical)
        kernels/parallel.cpp                # deterministic fork-join threading (per-pixel stages + spatial/LUT maps)
        kernels/fft.cpp                     # deterministic radix-2 complex FFT (fixed butterfly order)
        kernels/fft_convolve.cpp            # O(n log n) diffusion PSF convolution (#160)
        kernels/tonecurve.cpp               # optional 1D tone curve on final display RGB
        kernels/half.cpp                    # fp16 (NEON) <-> f32 conversion (preview proxy storage)
        # LUT/asset loaders:
        io/npy_lut.cpp                       # .npy / .lut binary loaders (M3)
        # Model (photographic math):
        model/spectral.cpp
        model/conversions.cpp
        model/density_curves.cpp
        model/emulsion.cpp
        model/glare.cpp
        model/diffusion.cpp                  # in-emulsion scatter + halation (spatial)
        model/grain.cpp                      # AgX particle grain model (M6)
        model/morph_curves.cpp               # opt-in s023 print density-curve morph (default OFF)
        model/gamut_compression.cpp          # opt-in gamut compression: ACES RGC v1.3 output + radial-to-locus input (default OFF)
        model/color_output.cpp               # XYZ->output RGB + CCTF (M3)
        # Profiles + runtime stages:
        profiles/profile.cpp                 # profile JSON loader (M3)
        runtime/stages/scanning.cpp          # scan_film negative -> RGB (M3)
        # Filming stage (rgb -> raw -> negative density), ported in M4:
        model/couplers.cpp                   # DIR couplers (M4)
        runtime/params.cpp                   # minimal digested filming params (M4)
        runtime/stages/filming.cpp           # rgb -> raw -> film density CMY (M4)
        # Printing (enlarger) stage + dichroic filters, ported in M4
        # (negative -> enlarger expose -> print develop -> scan print):
        model/color_filters.cpp              # dichroic Y/M/C enlarger filters (M4)
        runtime/stages/printing.cpp          # film density -> print density CMY (M4)
        runtime/print_digest.cpp             # native neutral CC + midgray digest (M5)
        runtime/color_reference.cpp          # scanner black/white correction (ColorReferenceService)
        runtime/stages/crop_resize.cpp       # crop + cubic resize geometry stage (M7)
        runtime/stages/autoexposure.cpp      # center-weighted metering -> EV gain (M7)
        gpu/vulkan_compute.cpp               # OPT-IN Vulkan compute fast-path (stub unless SPK_ENABLE_VULKAN)
        ml/segmentation.cpp                  # OPT-IN LiteRT subject/sky masking (stub unless SPK_ENABLE_LITERT)
)

target_include_directories(spektra PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Android system libs: log for diagnostics, android + jnigraphics for AAssetManager/Bitmap.
find_library(log-lib log)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)

# std::thread for the per-pixel fork-join (kernels/parallel). On Android/bionic the
# pthread symbols live in libc, so Threads::Threads is effectively a no-op there, but
# requesting it keeps the build correct on hosts where pthread is a separate lib.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

target_link_libraries(spektra ${log-lib} ${android-lib} ${jnigraphics-lib} Threads::Threads)

# OPT-IN GPU (Vulkan compute) fast-path — docs/PERF_ROADMAP.md #1. OFF by default:
# the default library is byte-identical (gpu/vulkan_compute.cpp compiles to a stub that
# reports unavailable). When -DSPK_ENABLE_VULKAN=ON, the real compute path is compiled
# and libvulkan is linked. Preview-only / non-bit-exact by design (GPU float != oracle),
# so the export + parity-gated path never call it.
option(SPK_ENABLE_VULKAN "Build the Vulkan compute GPU fast-path (preview-only)" OFF)
if(SPK_ENABLE_VULKAN)
    find_library(vulkan-lib vulkan)
    target_compile_definitions(spektra PRIVATE SPK_ENABLE_VULKAN=1)
    target_link_libraries(spektra ${vulkan-lib})
endif()

# OPT-IN Highway SIMD kernels (#124; plan in docs/research/highway-vendoring.md).
# OFF by default: without it kernels/gaussian_hwy.cpp compiles to nothing and the
# library is byte-identical to today. When ON, google/highway 1.4.0 is fetched at
# configure time (release asset + SHA256, the lib:libraw FetchContent pattern) and
# the f32 separable-FIR Gaussian gains explicit SIMD lanes.
#
# DETERMINISM: the ported passes vectorise ACROSS OUTPUT PIXELS only — each output's
# accumulation order over the kernel taps is unchanged — so the SIMD path is
# bit-identical to the scalar one, not an approximation. HWY_COMPILE_ONLY_STATIC
# pins ONE target per ABI (no runtime dispatch), making instruction selection an
# asserted contract rather than a compiler-lowering accident; see the static_assert
# in kernels/gaussian_hwy.cpp.
option(SPK_ENABLE_HIGHWAY "Build the Highway SIMD kernels (opt-in, bit-identical)" OFF)
set(SPK_HIGHWAY_VERSION "1.4.0" CACHE STRING "Vendored google/highway release tag")
set(SPK_HIGHWAY_URL
    "https://github.com/google/highway/releases/download/${SPK_HIGHWAY_VERSION}/highway-${SPK_HIGHWAY_VERSION}.tar.gz"
    CACHE STRING "Highway release-asset tarball URL")
# Release asset (immutable), verified two ways in docs/research/highway-vendoring.md.
set(SPK_HIGHWAY_SHA256 "36f672ab48ddb3c8555e9e89e16fe400cd7d16c6eb455a1a3d0c146a63ababdc"
    CACHE STRING "SHA256 of the Highway release asset")
# Escape hatch for offline/air-gapped builds: point at an extracted checkout.
set(SPK_HIGHWAY_SOURCE_DIR "" CACHE PATH
    "Pre-vendored highway source dir (contains hwy/highway.h); skips FetchContent")
if(SPK_ENABLE_HIGHWAY)
    # HWY_ENABLE_TESTS=OFF is MANDATORY: ON downloads googletest at *configure* time.
    set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(HWY_FORCE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    if(SPK_HIGHWAY_SOURCE_DIR AND EXISTS "${SPK_HIGHWAY_SOURCE_DIR}/hwy/highway.h")
        message(STATUS "spektra: using pre-vendored Highway at ${SPK_HIGHWAY_SOURCE_DIR}")
        add_subdirectory("${SPK_HIGHWAY_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/hwy-build" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(
            highway_upstream
            URL "${SPK_HIGHWAY_URL}"
            URL_HASH SHA256=${SPK_HIGHWAY_SHA256}
        )
        FetchContent_GetProperties(highway_upstream)
        if(NOT highway_upstream_POPULATED)
            FetchContent_Populate(highway_upstream)
        endif()
        add_subdirectory("${highway_upstream_SOURCE_DIR}" "${highway_upstream_BINARY_DIR}"
                         EXCLUDE_FROM_ALL)
    endif()
    target_compile_definitions(spektra PRIVATE
        SPK_ENABLE_HIGHWAY=1
        HWY_COMPILE_ONLY_STATIC=1   # one target per ABI, no runtime dispatch
        HWY_DISABLE_PCLMUL_AES=1)   # x86_64: keep the baseline at SSE4 w/o PCLMUL/AES
    target_link_libraries(spektra hwy)
endif()

# OPT-IN Intel oneTBB parallel backend (default OFF). When ON, kernels/parallel.h
# schedules the per-pixel stages through tbb::parallel_for (simple_partitioner over the
# same fixed chunk boundaries -> still bit-identical across thread counts). Default
# builds keep the dependency-free fork-join and are unchanged. Provide oneTBB via the
# toolchain/find_package or a pinned FetchContent (mirroring lib:libraw) when enabling.
# ---------------------------------------------------------------------------
# OPT-IN per-ABI microarchitecture tuning (perf-lab).
#
# The shipped flags target the ABI baseline: arm64-v8a means ARMv8.0, so the
# compiler may not use anything a 2018 phone lacked, even on a 2026 flagship.
# -mcpu lets it schedule for the real part and use the later ISA revisions
# (dotprod, fp16 arithmetic, LSE atomics) it already knows about.
#
# WHY THIS IS OFF BY DEFAULT AND SHOULD STAY THAT WAY UNTIL MEASURED: raising
# the baseline produces an APK that SIGILLs on any device older than the chosen
# core. It also changes instruction selection, so it can move the last bits of a
# float result and must be re-gated against the parity suite before it ships.
# The safe shipping form of this idea is -mtune (schedule for a core, keep the
# baseline ISA), which is what SPK_TUNE_ONLY selects.
option(SPK_TUNE_ARM64 "arm64: tune codegen for a specific core (perf lab)" OFF)
set(SPK_TUNE_ARM64_CPU "cortex-a76" CACHE STRING
    "Core to pass to -mcpu/-mtune when SPK_TUNE_ARM64 is on")
option(SPK_TUNE_ONLY "Use -mtune (schedule only, baseline ISA kept) rather than -mcpu" ON)

if(SPK_TUNE_ARM64 AND ANDROID_ABI STREQUAL "arm64-v8a")
    if(SPK_TUNE_ONLY)
        target_compile_options(spektra PRIVATE -mtune=${SPK_TUNE_ARM64_CPU})
        message(STATUS "spektra: -mtune=${SPK_TUNE_ARM64_CPU} (baseline ISA kept)")
    else()
        target_compile_options(spektra PRIVATE -mcpu=${SPK_TUNE_ARM64_CPU})
        message(WARNING
            "spektra: -mcpu=${SPK_TUNE_ARM64_CPU} RAISES THE ISA BASELINE. The "
            "resulting library will crash with SIGILL on older arm64 devices, and "
            "instruction selection changes must be re-gated against the parity "
            "suite before shipping.")
    endif()
endif()

option(SPK_USE_TBB "Use Intel oneTBB as the parallel-for backend" OFF)
if(SPK_USE_TBB)
    find_package(TBB REQUIRED)
    target_compile_definitions(spektra PRIVATE SPK_USE_TBB=1)
    target_link_libraries(spektra TBB::tbb)
endif()

# OPT-IN LiteRT (TFLite) ML masking (default OFF). When ON, ml/segmentation.cpp runs a
# bundled segmentation model via libtensorflowlite_c (subject/sky masks — a feature,
# not part of the bit-exact render path). Default builds compile the stub and link
# nothing extra; enabling additionally requires vendoring the model + runtime.
option(SPK_ENABLE_LITERT "Build the LiteRT ML segmentation path" OFF)
if(SPK_ENABLE_LITERT)
    find_library(tflite-lib tensorflowlite_c)
    target_compile_definitions(spektra PRIVATE SPK_ENABLE_LITERT=1)
    target_link_libraries(spektra ${tflite-lib})
endif()

# 16 KB page support (Android 15): force every PT_LOAD segment to a 16 KB-aligned
# p_align so the lib mmaps on devices with a 16 KB page size. NDK r27+ does this by
# default, but pin it explicitly so the alignment holds on any NDK the build picks up.
target_link_options(spektra PRIVATE "-Wl,-z,max-page-size=16384")

# M2: link LibRaw here (or keep it in lib:libraw and pass decoded buffers in).
# target_link_libraries(spektra raw)

# --- Host parity tests (NOT part of libspektra.so) ---------------------------
# The authoritative gate list lives in .github/workflows/ci.yml (`engine-parity`,
# one build_run per test) and is mirrored — with a count drift-guard — by
# tools/parity/run_engine_parity.sh, which builds and runs the whole suite
# locally with the same argv as CI. Do not enumerate tests here; the two files
# above are the source of truth.
# Statistical grain gates (RNG streams differ from the oracle; verified by
# distribution + stats rather than byte goldens): tests/test_grain.cpp
# (non-sublayer: mean preservation, noise std ±15%) and
# tests/test_grain_sublayer.cpp (sublayer + micro-structure; also asserts a
# distinct noise character vs the non-sublayer path). References regenerated by
# tests/gen_grain_ref.py / tests/gen_grain_sublayer_ref.py (oracle, fixed seed).
# Being statistical, neither can see an element-wise change in the samplers, so
# kernels/stats.cpp's degenerate-CDF short-circuit is gated separately and
# element-wise by tests/test_binomial_shortcircuit.cpp.
