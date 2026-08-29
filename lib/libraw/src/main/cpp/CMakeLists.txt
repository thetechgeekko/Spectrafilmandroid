# Spektrafilm for Android — lib:libraw native build.
# Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
# This module uses LibRaw (LGPL-2.1), linked into the GPLv3 app.
#
# Builds libsfraw.so: the rawpy-parity RAW/DNG decoder + JNI bridge, with LibRaw
# compiled in statically.
#
# --------------------------------------------------------------------------
# VENDORING LIBRAW (reproducible, network-only)
# --------------------------------------------------------------------------
# LibRaw is obtained at configure time via CMake FetchContent, pinned to a
# specific release tarball + SHA256 (see SFRAW_LIBRAW_VERSION / _URL / _SHA256
# below). A clean checkout therefore builds with just a working network — no
# git submodule, no committed third-party blob.
#
# To pin a different release, bump the three SFRAW_LIBRAW_* variables.
#
# Optional offline override: if LibRaw is already present on disk (e.g. a CI
# cache or a hand-vendored copy), point -DSFRAW_LIBRAW_SOURCE_DIR=<dir> at a
# stock LibRaw checkout (the directory that contains libraw/libraw.h) and
# FetchContent is skipped.
# --------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22.1)
project(sfraw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release: the demosaic/colour loops are the hot path. -O3 is safe; we keep IEEE
# semantics here because the Von-Kries / white-balance multipliers must match the
# desktop rawpy output to within golden-vector tolerance.
if (NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3")
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
# Pinned LibRaw release.
# --------------------------------------------------------------------------
set(SFRAW_LIBRAW_VERSION "0.21.4" CACHE STRING "Pinned LibRaw release version")
set(SFRAW_LIBRAW_URL
        "https://www.libraw.org/data/LibRaw-${SFRAW_LIBRAW_VERSION}.tar.gz"
        CACHE STRING "LibRaw release tarball URL")
# SHA256 of LibRaw-0.21.4.tar.gz (libraw.org). Pin so the fetch is verified.
set(SFRAW_LIBRAW_SHA256
        "6be43f19397e43214ff56aab056bf3ff4925ca14012ce5a1538a172406a09e63"
        CACHE STRING "SHA256 of the pinned LibRaw tarball")

# Optional: a pre-vendored LibRaw checkout (skips FetchContent when set/exists).
set(SFRAW_LIBRAW_SOURCE_DIR "" CACHE PATH
        "Pre-vendored LibRaw source dir (contains libraw/libraw.h); skips FetchContent")

set(LIBRAW_DIR "")

if (SFRAW_LIBRAW_SOURCE_DIR AND EXISTS "${SFRAW_LIBRAW_SOURCE_DIR}/libraw/libraw.h")
    set(LIBRAW_DIR "${SFRAW_LIBRAW_SOURCE_DIR}")
    message(STATUS "sfraw: using pre-vendored LibRaw at ${LIBRAW_DIR}")
else ()
    include(FetchContent)
    message(STATUS "sfraw: fetching LibRaw ${SFRAW_LIBRAW_VERSION} from ${SFRAW_LIBRAW_URL}")
    # NOTE: no DOWNLOAD_EXTRACT_TIMESTAMP here — that option is CMake 3.24+, and the
    # Android-SDK-bundled CMake is 3.22.1, which folds the unknown keyword into
    # URL_HASH and fails. 3.22 extracts fine without it.
    FetchContent_Declare(
            libraw_upstream
            URL "${SFRAW_LIBRAW_URL}"
            URL_HASH SHA256=${SFRAW_LIBRAW_SHA256}
    )
    # We only need the sources extracted; LibRaw's own build system is not used.
    FetchContent_GetProperties(libraw_upstream)
    if (NOT libraw_upstream_POPULATED)
        FetchContent_Populate(libraw_upstream)
    endif ()
    set(LIBRAW_DIR "${libraw_upstream_SOURCE_DIR}")
endif ()

# --------------------------------------------------------------------------
# LibRaw static library.
# --------------------------------------------------------------------------
if (EXISTS "${LIBRAW_DIR}/libraw/libraw.h")
    # LibRaw groups its sources under src/ (decoders, demosaic, metadata,
    # preprocessing, postprocessing, tables, utils, write, x3f, integration) plus
    # a top-level libraw_c_api.cpp / libraw_datastream.cpp. This glob matches the
    # authoritative object list in LibRaw's Makefile.am for 0.21.x. The optional
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

    add_library(raw STATIC ${LIBRAW_SOURCES})
    target_include_directories(raw PUBLIC "${LIBRAW_DIR}")

    # ----------------------------------------------------------------------
    # LibRaw build knobs.
    # ----------------------------------------------------------------------
    # We disable the heavy optional deps we don't need (jasper/lcms). But two
    # codecs are REQUIRED for Samsung "Expert RAW" and other modern compressed
    # DNGs and MUST stay enabled:
    #
    #   * DEFLATE/ZIP-compressed DNG (DNG Compression tag 8). LibRaw decodes
    #     these via zlib's inflate(); guarded by USE_ZLIB. Samsung Expert RAW
    #     very commonly uses deflate-compressed (often 16-bit linear) tiles, so
    #     WITHOUT zlib LibRaw's unpack() fails with LIBRAW_DATA_ERROR /
    #     LIBRAW_UNSUPPORTED_THUMBNAIL and the image cannot be decoded. The
    #     Android NDK ships zlib (libz) in its sysroot, so this is cheap and
    #     safe -> ENABLED. (Note: LibRaw's *internal* dcraw deflate path is
    #     compiled in regardless, but the DNG/Adobe-coder deflate path is what
    #     USE_ZLIB gates, which is the one Expert RAW exercises.)
    #
    #   * Lossy-JPEG-compressed DNG (DNG 1.4 lossy, Compression 0x884C / 7).
    #     LibRaw decodes these via libjpeg; guarded by USE_JPEG. The NDK does
    #     NOT ship libjpeg, so this stays OFF unless a libjpeg target is
    #     provided (SFRAW_LIBRAW_JPEG_TARGET). When OFF, lossy-JPEG Expert RAW
    #     DNGs cannot be unpacked by LibRaw; the decoder reports a specific
    #     error so the app can fall back to the platform ImageDecoder.
    #
    # LIBRAW_NODLL -> static (no dllimport/export); integration glue stays out.
    set(SFRAW_LIBRAW_DEFS NO_JASPER NO_LCMS LIBRAW_NODLL)

    # zlib / deflate-DNG: ON by default (NDK provides libz).
    option(SFRAW_WITH_ZLIB "Build LibRaw with deflate-DNG (zlib) support" ON)
    # libjpeg / lossy-DNG: OFF by default (NDK has no libjpeg). Point
    # SFRAW_LIBRAW_JPEG_TARGET at a CMake target that provides libjpeg
    # (headers + library) to enable lossy-JPEG DNG decoding.
    set(SFRAW_LIBRAW_JPEG_TARGET "" CACHE STRING
            "CMake target providing libjpeg (headers+lib); enables lossy-JPEG DNG")

    if (SFRAW_WITH_ZLIB)
        list(APPEND SFRAW_LIBRAW_DEFS USE_ZLIB)
        message(STATUS "sfraw: deflate-compressed DNG support ENABLED (zlib)")
    else ()
        list(APPEND SFRAW_LIBRAW_DEFS NO_ZLIB)
        message(WARNING "sfraw: deflate-compressed DNG support DISABLED; "
                "Samsung Expert RAW deflate DNGs will fail to unpack.")
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
    message(WARNING
            "LibRaw not found at ${LIBRAW_DIR}. "
            "libsfraw.so will build but decode() will return an error at runtime. "
            "Provide network access (FetchContent) or -DSFRAW_LIBRAW_SOURCE_DIR.")
    set(SFRAW_HAVE_LIBRAW FALSE)
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
# This changes no pixel. That is precisely why it is preferred over lowering
# user_qual, which would alter the decoded image and so be a quality decision
# rather than a performance one. Verify byte-identical decode output before and
# after rather than assuming it.
#
# -static-openmp links libomp INTO the .so instead of shipping a separate
# libomp.so; it is an NDK driver flag, so it is only added for Android. Detection
# is a real compile+link probe with the same flags, so a toolchain without OpenMP
# degrades to the previous single-threaded build with a warning instead of
# failing the build.
option(SFRAW_ENABLE_OPENMP "Compile LibRaw with OpenMP (unlocks its demosaic pragmas)" ON)
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
        raw_decoder.cpp
        raw_decoder_jni.cpp
)

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
