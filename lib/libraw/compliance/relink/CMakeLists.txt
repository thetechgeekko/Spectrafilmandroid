# Recipient-controlled LibRaw relink project for the Spektrafilm Android JNI ABI.
# Copyright (C) 2026 Spektrafilm Android contributors. GPL-3.0-only.
#
# This project deliberately does not include LibRawVendor.cmake and does not
# impose the production patched-tree aggregate. A recipient must be able to
# point it at a modified, interface-compatible LibRaw source tree.

cmake_minimum_required(VERSION 3.22.1)
project(sfraw_recipient_relink LANGUAGES CXX)

if (NOT ANDROID)
    message(FATAL_ERROR
        "sfraw recipient relink is an Android-NDK project; configure it with "
        "the NDK android.toolchain.cmake file")
endif ()

set(LIBRAW_SOURCE_DIR "" CACHE PATH
    "Explicit recipient-controlled LibRaw source tree (required)")
if (LIBRAW_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "LIBRAW_SOURCE_DIR is required")
endif ()
get_filename_component(LIBRAW_SOURCE_DIR "${LIBRAW_SOURCE_DIR}" ABSOLUTE)
if (NOT EXISTS "${LIBRAW_SOURCE_DIR}/libraw/libraw.h" OR
        NOT EXISTS "${LIBRAW_SOURCE_DIR}/libraw/libraw_version.h")
    message(FATAL_ERROR
        "LIBRAW_SOURCE_DIR does not contain a complete LibRaw source tree: "
        "${LIBRAW_SOURCE_DIR}")
endif ()

set(SFRAW_WRAPPER_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../src/main/cpp" CACHE PATH
    "Spektrafilm JNI wrapper source directory")
get_filename_component(SFRAW_WRAPPER_SOURCE_DIR
    "${SFRAW_WRAPPER_SOURCE_DIR}" ABSOLUTE)
foreach (_wrapper_file IN ITEMS
        native_allocation_registry.cpp
        native_allocation_registry.h
        raw_decoder.cpp
        raw_decoder.h
        raw_decoder_jni.cpp
        raw_decoder_jni_safety.h
        raw_result_publication.cpp
        raw_result_publication.h)
    if (NOT EXISTS "${SFRAW_WRAPPER_SOURCE_DIR}/${_wrapper_file}")
        message(FATAL_ERROR "missing recipient relink wrapper source: ${_wrapper_file}")
    endif ()
endforeach ()

message(WARNING
    "Building an UNOFFICIAL RECIPIENT RELINK of libsfraw.so from "
    "${LIBRAW_SOURCE_DIR}; this output is not an official Spektrafilm release")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif ()
if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif ()

file(GLOB_RECURSE LIBRAW_SOURCES CONFIGURE_DEPENDS
    "${LIBRAW_SOURCE_DIR}/src/*.cpp")
list(FILTER LIBRAW_SOURCES EXCLUDE REGEX
    ".*(_ph|_common|placeholder)\\.cpp$")
list(SORT LIBRAW_SOURCES)
if (NOT LIBRAW_SOURCES)
    message(FATAL_ERROR "LIBRAW_SOURCE_DIR contains no LibRaw C++ sources")
endif ()

add_library(raw STATIC ${LIBRAW_SOURCES})
target_include_directories(raw PUBLIC "${LIBRAW_SOURCE_DIR}")
target_compile_definitions(raw PUBLIC
    NO_JASPER
    NO_JPEG
    USE_ZLIB
    LIBRAW_NODLL
    LIBRAW_MAX_PROFILE_SIZE_MB=16
    LIBRAW_MAX_METADATA_ALLOC_SIZE_MB=16
    LIBRAW_MAX_TILE_STREAM_COUNT=131072
    LIBRAW_CALLOC_RAWSTORE)
target_compile_options(raw PRIVATE -w)
find_library(SFRAW_ZLIB z REQUIRED)
target_link_libraries(raw PUBLIC "${SFRAW_ZLIB}")

add_library(sfraw SHARED
    "${SFRAW_WRAPPER_SOURCE_DIR}/native_allocation_registry.cpp"
    "${SFRAW_WRAPPER_SOURCE_DIR}/raw_decoder.cpp"
    "${SFRAW_WRAPPER_SOURCE_DIR}/raw_decoder_jni.cpp"
    "${SFRAW_WRAPPER_SOURCE_DIR}/raw_result_publication.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/unofficial_relink_marker.cpp")
set_target_properties(sfraw PROPERTIES OUTPUT_NAME sfraw)
target_include_directories(sfraw PRIVATE
    "${SFRAW_WRAPPER_SOURCE_DIR}"
    "${LIBRAW_SOURCE_DIR}")
target_compile_definitions(sfraw PRIVATE SFRAW_RECIPIENT_RELINK=1)
target_link_libraries(sfraw PRIVATE raw)

find_library(SFRAW_LOG log REQUIRED)
find_library(SFRAW_ANDROID android REQUIRED)
find_library(SFRAW_JNIGRAPHICS jnigraphics REQUIRED)
target_link_libraries(sfraw PRIVATE
    "${SFRAW_LOG}" "${SFRAW_ANDROID}" "${SFRAW_JNIGRAPHICS}")

# Preserve the production Java/JNI loader contract and Android 15+ 16 KiB
# segment alignment. The marker source distinguishes this user-built binary.
target_link_options(sfraw PRIVATE
    "-Wl,-soname,libsfraw.so"
    "-Wl,-z,max-page-size=16384")

string(CONCAT SFRAW_UNOFFICIAL_NOTICE
    "UNOFFICIAL RECIPIENT RELINK of libsfraw.so\n"
    "LibRaw source: ${LIBRAW_SOURCE_DIR}\n")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/UNOFFICIAL-RECIPIENT-RELINK.txt"
    CONTENT "${SFRAW_UNOFFICIAL_NOTICE}")
