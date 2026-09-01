# Spektrafilm for Android — lib:libraw native build.
# Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
# This module statically uses dual-offered LibRaw; the distribution route is a
# fail-closed release decision tracked by the compliance tooling and ticket #166.
#
# Builds libsfraw.so: the RAW/DNG decoder + JNI bridge, with LibRaw compiled in
# statically. The rawpy option contract is mirrored; exact decoder-version parity
# remains a separate qualification gate (#189).
#
# --------------------------------------------------------------------------
# VENDORING LIBRAW (reproducible and fail-closed)
# --------------------------------------------------------------------------
# The shared resolver pins the official 0.22.2 archive and SHA-256, applies the
# hashed patch series, verifies source-level security contracts, and refuses a
# runtime decoder stub. SFRAW_LIBRAW_SOURCE_DIR is debug/offline-only; shipping
# builds must use the official hashed archive.
# --------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22.1)
project(sfraw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release: the demosaic/colour loops are the hot path. -O3 is safe; we keep IEEE
# semantics here because the CAT02 RAW white-balance path is locked to exact
# float32 oracle bits at its two declared cast boundaries.
if (NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endif ()
if (CMAKE_CXX_FLAGS_RELWITHDEBINFO MATCHES "(^| )-O2( |$)")
    string(REPLACE "-O2" "-O3" CMAKE_CXX_FLAGS_RELWITHDEBINFO
            "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif ()

# DEBUG BUILDS MUST STILL BE OPTIMISED. CMake's default CMAKE_CXX_FLAGS_DEBUG is
# "-g" with NO -O flag, i.e. -O0. For LibRaw's demosaic and colour
# loops that is several times slower than the shipping build.
# This mirrors the guard in engine/spektra-core/src/main/cpp/CMakeLists.txt.
# Without it every on-device timing taken from a debug APK measures the compiler
# rather than the code, and the two builds are not comparable: on SM-S948W the
# same 12 MP frame decoded in 3616 ms on debug and 546 ms on release. Most of
# that gap is this missing flag (a residual remains: release is -O3, debug -O2).
if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif ()

# --------------------------------------------------------------------------
# Pinned LibRaw release + reviewed local patch series.
# --------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/LibRawVendor.cmake")
sfraw_resolve_libraw(LIBRAW_DIR)

# --------------------------------------------------------------------------
# LibRaw static library.
# --------------------------------------------------------------------------
if (EXISTS "${LIBRAW_DIR}/libraw/libraw.h")
    # LibRaw groups its sources under src/ (decoders, demosaic, metadata,
    # preprocessing, postprocessing, tables, utils, write, x3f, integration) plus
    # a top-level libraw_c_api.cpp / libraw_datastream.cpp. This glob matches the
    # authoritative object layout in the pinned LibRaw 0.22.2 archive. The optional
    # integration/*.cpp glue (DNG SDK / RawSpeed) is guarded by USE_DNGSDK /
    # USE_RAWSPEED and compiles to (near) empty TUs when those are off — so it is
    # harmless to include.
    file(GLOB_RECURSE LIBRAW_SOURCES
            "${LIBRAW_DIR}/src/*.cpp"
    )
    # CRITICAL: LibRaw ships *_ph.cpp "placeholder" TUs (postprocessing_ph.cpp,
    # preprocessing_ph.cpp, write_ph.cpp) that provide STUB implementations
    # (e.g. dcraw_process() -> LIBRAW_NOT_IMPLEMENTED, dcraw_make_mem_image() ->
    # NULL) for building a postprocessing-free "lite" LibRaw. They are mutually
    # exclusive with the real implementations in postprocessing_utils.cpp /
    # mem_image.cpp / etc., and LibRaw's own Makefile.am (lib_libraw_a_SOURCES)
    # never lists them. Compiling both yields duplicate symbols; inside a static
    # archive the link is silently ambiguous (which definition wins is undefined),
    # so the placeholder dcraw_process() could be selected and decode would return
    # LIBRAW_NOT_IMPLEMENTED at runtime. We MUST exclude them to get a real decoder.
    #
    # Also drop any "*_common" helper packs (older LibRaw demosaic-pack layout)
    # and placeholder TUs that are #included by siblings, not compiled directly.
    list(FILTER LIBRAW_SOURCES EXCLUDE REGEX ".*(_ph|_common|placeholder)\\.cpp$")
    list(SORT LIBRAW_SOURCES)

    # Record the exact translation units selected from the hashed archive. This
    # generated file is an audit/SBOM artifact and exposes future layout drift.
    set(_libraw_source_manifest "")
    foreach (_source IN LISTS LIBRAW_SOURCES)
        file(RELATIVE_PATH _relative_source "${LIBRAW_DIR}" "${_source}")
        string(APPEND _libraw_source_manifest "${_relative_source}\n")
    endforeach ()
    file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libraw-sources-0.22.2.txt"
            CONTENT "${_libraw_source_manifest}")

    add_library(raw STATIC ${LIBRAW_SOURCES})
    target_include_directories(raw PUBLIC "${LIBRAW_DIR}")

    # ----------------------------------------------------------------------
    # LibRaw build knobs.
    # ----------------------------------------------------------------------
    # We disable the heavy optional deps we do not need (jasper/lcms). Keep
    # zlib enabled for the compressed-DNG subset that pinned LibRaw 0.22.2
    # actually implements:
    #
    #   * DEFLATE/ZIP DNG (Compression 8) with SampleFormat=3
    #     floating-point samples. USE_ZLIB gates this path and the Android NDK
    #     supplies libz. Integer/linear deflate is not implemented safely by
    #     this pinned decoder and surfaces as SFRAW_ERR_DEFLATE_DNG so the app
    #     can choose a platform/next-codec fallback.
    #
    #   * Lossy-JPEG-compressed DNG (DNG 1.4 lossy, Compression 0x884C; legacy 6).
    #     LibRaw decodes these via libjpeg; guarded by USE_JPEG. The NDK does
    #     NOT ship libjpeg, so this stays OFF unless a libjpeg target is
    #     provided (SFRAW_LIBRAW_JPEG_TARGET). When OFF, lossy-JPEG Expert RAW
    #     DNGs cannot be unpacked by LibRaw; the decoder reports a specific
    #     error so the app can fall back to the platform ImageDecoder.
    #
    # LIBRAW_NODLL -> static (no dllimport/export); integration glue stays out.
    set(SFRAW_LIBRAW_DEFS
            NO_JASPER
            LIBRAW_NODLL
            # Embedded ICC data is copied during open_buffer(), before the
            # wrapper can enforce pixel limits. Keep that metadata allocation
            # bounded for an Android process (LibRaw defaults to 256 MiB).
            LIBRAW_MAX_PROFILE_SIZE_MB=16
            # Cumulative member-allocator work while identify() parses metadata;
            # prevents repeated TIFF entries from amplifying a small payload.
            LIBRAW_MAX_METADATA_ALLOC_SIZE_MB=16
            # Operational ceiling for per-tile decoder setup. Tiny hostile DNG
            # tiles otherwise amplify a bounded image into millions of codec
            # initializations. A geometry-derived limit preserves compliant
            # 16x16 tiling; this is the absolute fail-closed ceiling.
            LIBRAW_MAX_TILE_STREAM_COUNT=131072
            # Deterministic zero-filled backing storage for truncated inputs.
            LIBRAW_CALLOC_RAWSTORE)

    # zlib / floating-point deflate DNG: ON by default (NDK provides libz).
    option(SFRAW_WITH_ZLIB
            "Build LibRaw with floating-point deflate-DNG (zlib) support" ON)
    sfraw_is_shipping_config(_sfraw_shipping_config)
    if (_sfraw_shipping_config AND NOT SFRAW_WITH_ZLIB)
        message(FATAL_ERROR
                "sfraw: shipping builds require SFRAW_WITH_ZLIB=ON for "
                "floating-point deflate DNG support")
    endif ()
    # libjpeg / lossy-DNG: OFF by default (NDK has no libjpeg). Point
    # SFRAW_LIBRAW_JPEG_TARGET at a CMake target that provides libjpeg
    # (headers + library) to enable lossy-JPEG DNG decoding.
    set(SFRAW_LIBRAW_JPEG_TARGET "" CACHE STRING
            "CMake target providing libjpeg (headers+lib); enables lossy-JPEG DNG")

    if (SFRAW_WITH_ZLIB)
        list(APPEND SFRAW_LIBRAW_DEFS USE_ZLIB)
        message(STATUS
                "sfraw: floating-point deflate DNG support ENABLED (zlib)")
    else ()
        list(APPEND SFRAW_LIBRAW_DEFS NO_ZLIB)
        message(WARNING
                "sfraw: floating-point deflate DNG support DISABLED; "
                "Compression 8 inputs will use typed fallback.")
    endif ()

    if (SFRAW_LIBRAW_JPEG_TARGET STREQUAL "")
        list(APPEND SFRAW_LIBRAW_DEFS NO_JPEG)
        message(STATUS "sfraw: lossy-JPEG DNG support DISABLED (no libjpeg in NDK); "
                "lossy-JPEG Expert RAW reports SFRAW_ERR_LOSSY_JPEG_DNG for "
                "app-side ImageDecoder fallback.")
    else ()
        list(APPEND SFRAW_LIBRAW_DEFS USE_JPEG)
        target_link_libraries(raw ${SFRAW_LIBRAW_JPEG_TARGET})
        message(STATUS "sfraw: lossy-JPEG DNG support ENABLED (${SFRAW_LIBRAW_JPEG_TARGET})")
    endif ()

    target_compile_definitions(raw PUBLIC ${SFRAW_LIBRAW_DEFS})
    target_compile_options(raw PRIVATE -w)  # silence LibRaw's own warnings

    # Link zlib into the static lib's consumers. On Android the NDK sysroot has
    # libz; find_library locates it. On a host build ZLIB::ZLIB is used if found.
    if (SFRAW_WITH_ZLIB)
        if (ANDROID)
            find_library(z-lib z)
            if (z-lib)
                target_link_libraries(raw ${z-lib})
            else ()
                target_link_libraries(raw z)  # NDK sysroot always has -lz
            endif ()
        else ()
            find_package(ZLIB)
            if (ZLIB_FOUND)
                target_link_libraries(raw ZLIB::ZLIB)
            else ()
                target_link_libraries(raw z)
            endif ()
        endif ()
    endif ()

    set(SFRAW_HAVE_LIBRAW TRUE)
    message(STATUS "sfraw: LibRaw sources found (${LIBRAW_DIR}); RAW decode ENABLED")
else ()
    message(FATAL_ERROR
            "sfraw: verified LibRaw 0.22.2 source is absent at ${LIBRAW_DIR}; "
            "refusing to produce a release-capable decoder stub")
endif ()

# --------------------------------------------------------------------------
# OpenMP (#158).
# --------------------------------------------------------------------------
# LibRaw guards 43 `#pragma omp` sites behind LIBRAW_USE_OPENMP, and
# libraw_types.h only defines that `#if defined(_OPENMP)`. Without -fopenmp every
# one of them is inert — which is how `dcraw_process` came to be 2760 ms, 77% of a
# 3.6 s full-resolution decode, running single-threaded on an 8-core phone
# (docs/research/perf-lab.md §17.2). The pragmas sit in exactly the files that
# matter: ahd_demosaic.cpp (the default, since raw_decoder.cpp never sets
# user_qual) and postprocessing_aux.cpp — both inside dcraw_process.
#
# OpenMP was initially expected to preserve pixels, but the repeated compressed-
# Fuji device corpus disproved that assumption: scheduling changes output bytes.
# It remains a debug qualification lever, not a shipping optimization.
#
# -static-openmp links libomp INTO the .so instead of shipping a separate
# libomp.so; it is an NDK driver flag, so it is only added for Android. Detection
# is a real compile+link probe with the same flags, so a toolchain without OpenMP
# degrades to the previous single-threaded build with a warning instead of
# failing the build.
# Exact output has priority over speculative decode throughput. LibRaw issue #845
# reports scheduling-dependent compressed-Fuji output, so shipping configurations
# remain serial until #158 qualifies OpenMP against the repeated camera corpus.
# Debug builds may opt in to exercise the patched OpenMP code under sanitizers.
option(SFRAW_ENABLE_OPENMP
        "Experimental: compile LibRaw with OpenMP; not qualified for exact release"
        OFF)
sfraw_is_shipping_config(_sfraw_shipping_config)
if (SFRAW_ENABLE_OPENMP AND _sfraw_shipping_config)
    message(FATAL_ERROR
            "sfraw: OpenMP is not yet corpus-qualified for bit-identical release "
            "decode (LibRaw #845); use the serial exact path")
endif ()
set(SFRAW_OPENMP_FLAGS -fopenmp)
if (ANDROID)
    list(APPEND SFRAW_OPENMP_FLAGS -static-openmp)
endif ()
set(SFRAW_OPENMP_ACTIVE OFF)
if (SFRAW_ENABLE_OPENMP)
    include(CheckCXXSourceCompiles)
    string(REPLACE ";" " " SFRAW_OPENMP_FLAGS_STR "${SFRAW_OPENMP_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${SFRAW_OPENMP_FLAGS_STR}")
    check_cxx_source_compiles(
            "#include <omp.h>\nint main() { return omp_get_max_threads() > 0 ? 0 : 1; }"
            SFRAW_HAVE_OPENMP)
    unset(CMAKE_REQUIRED_FLAGS)
    if (SFRAW_HAVE_OPENMP)
        set(SFRAW_OPENMP_ACTIVE ON)
        message(STATUS "sfraw: LibRaw OpenMP ENABLED (${SFRAW_OPENMP_FLAGS_STR}) — #158")
    else ()
        message(WARNING "sfraw: -fopenmp unavailable; LibRaw demosaic stays "
                "single-threaded and the #158 lever is NOT pulled")
    endif ()
endif ()
if (SFRAW_OPENMP_ACTIVE AND TARGET raw)
    target_compile_options(raw PRIVATE ${SFRAW_OPENMP_FLAGS})
endif ()

# --------------------------------------------------------------------------
# Our decoder + JNI bridge.
# --------------------------------------------------------------------------
add_library(sfraw SHARED
        native_allocation_registry.cpp
        raw_decoder.cpp
        raw_decoder_jni.cpp
)

# Clang/GCC may otherwise fuse multiply-add operations at -O3 even without
# -ffast-math. The oracle accumulates each float64 matrix product in a fixed
# order, so contraction must be disabled for exact ACES/ProPhoto float32 bits.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sfraw PRIVATE -ffp-contract=off)
endif ()

target_include_directories(sfraw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The OpenMP runtime has to reach the final .so, not just the static archive.
if (SFRAW_OPENMP_ACTIVE)
    target_compile_options(sfraw PRIVATE ${SFRAW_OPENMP_FLAGS})
    target_link_options(sfraw PRIVATE ${SFRAW_OPENMP_FLAGS})
endif ()

if (SFRAW_HAVE_LIBRAW)
    target_link_libraries(sfraw raw)
    target_include_directories(sfraw PRIVATE "${LIBRAW_DIR}")
endif ()

# Android system libs: log for diagnostics; jnigraphics for optional Bitmap output;
# android for AAssetManager / fd helpers. Guarded so the same CMakeLists can also be
# configured for a host (non-Android) build of the decoder (parity tests).
if (ANDROID)
    find_library(log-lib log)
    find_library(android-lib android)
    find_library(jnigraphics-lib jnigraphics)
    target_link_libraries(sfraw ${log-lib} ${android-lib} ${jnigraphics-lib})

    # 16 KB page support (Android 15): force 16 KB-aligned PT_LOAD segments so the
    # lib mmaps on 16 KB-page devices. NDK r27+ does this by default; pin it anyway.
    target_link_options(sfraw PRIVATE "-Wl,-z,max-page-size=16384")
endif ()
