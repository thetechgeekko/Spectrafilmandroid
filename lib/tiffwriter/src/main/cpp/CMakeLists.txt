# Spektrafilm for Android — lib:tiffwriter native build.
# Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
#
# Builds libsftiff.so: a self-contained, dependency-free 16-bit baseline TIFF
# writer (RGB, 16 bit/sample, uncompressed or PackBits, with ICC + EXIF) plus its
# JNI bridge. No third-party deps (the writer is hand-rolled — see tiff_writer.cpp
# for why LibRaw is not reused for writing).
#
# Mirrors lib:libraw's plain-AGP CMake setup so it configures/builds standalone
# the moment the project lead adds it to settings.gradle.kts. The core writer
# (tiff_writer.cpp) is host-buildable (no Android deps) for the g++ unit test in
# tests/test_tiff_writer.cpp.

cmake_minimum_required(VERSION 3.22.1)
project(sftiff LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
endif ()

# DEBUG BUILDS MUST STILL BE OPTIMISED. CMake's default CMAKE_CXX_FLAGS_DEBUG is
# "-g" with NO -O flag, i.e. -O0. For the per-pixel byte-swap and
# PackBits loops that is several times slower than the shipping build.
# This mirrors the guard in engine/spektra-core/src/main/cpp/CMakeLists.txt.
# Without it every on-device timing taken from a debug APK measures the compiler
# rather than the code. The same missing flag in lib:libraw was worth 6.6x on
# SM-S948W (3616 ms debug vs 546 ms release to decode one 12 MP frame), which is
# how it was found. Release here is -O2, so debug and release now match.
if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif ()

add_library(sftiff SHARED
        tiff_writer.cpp
        tiff_writer_jni.cpp
)

target_include_directories(sftiff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Android system libs: log for diagnostics. Guarded so this CMakeLists can also be
# configured for a host build of the writer if ever desired (the test uses g++
# directly and does not go through CMake).
if (ANDROID)
    find_library(log-lib log)
    target_link_libraries(sftiff ${log-lib})

    # 16 KB page support (Android 15): force 16 KB-aligned PT_LOAD segments so the
    # lib mmaps on 16 KB-page devices. NDK r27+ does this by default; pin it anyway.
    target_link_options(sftiff PRIVATE "-Wl,-z,max-page-size=16384")
endif ()
