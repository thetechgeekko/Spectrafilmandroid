cmake_minimum_required(VERSION 3.22)

project(sfraw_libraw_host_regressions LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SFRAW_HOST_ENABLE_SANITIZERS
       "Instrument LibRaw and the public-seam regressions with ASan/UBSan" ON)
option(SFRAW_HOST_ENABLE_TSAN
       "Instrument LibRaw and the concurrency regression with ThreadSanitizer" OFF)
option(SFRAW_HOST_ENABLE_OPENMP
       "Build the host LibRaw regression target with OpenMP when available" ON)
option(SFRAW_HOST_REQUIRE_OPENMP
       "Fail configure unless the host regression really builds OpenMP paths" OFF)
option(SFRAW_HOST_BUILD_FUZZER
       "Link the LibRaw public-seam harness with Clang libFuzzer" OFF)

# This is the same resolver used by the Android target. It verifies the release
# archive, exact 0.22.2 headers, and Spektrafilm's reviewed local patches before
# returning a source directory. A stale 0.21.x FetchContent tree therefore cannot
# satisfy this host test by accident.
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/LibRawVendor.cmake")
sfraw_resolve_libraw(SFRAW_HOST_LIBRAW_DIR)

file(GLOB_RECURSE SFRAW_HOST_LIBRAW_SOURCES CONFIGURE_DEPENDS
     "${SFRAW_HOST_LIBRAW_DIR}/src/*.cpp")
list(FILTER SFRAW_HOST_LIBRAW_SOURCES
     EXCLUDE REGEX ".*(_ph|_common|placeholder)\\.cpp$")

if(NOT SFRAW_HOST_LIBRAW_SOURCES)
  message(FATAL_ERROR "Resolved LibRaw tree has no compilable sources")
endif()

if(SFRAW_HOST_ENABLE_SANITIZERS AND SFRAW_HOST_ENABLE_TSAN)
  message(FATAL_ERROR "ASan/UBSan and ThreadSanitizer require separate builds")
endif()

add_library(sfraw_host_sanitizers INTERFACE)
if(SFRAW_HOST_ENABLE_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sfraw_host_sanitizers INTERFACE
                           -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(sfraw_host_sanitizers INTERFACE
                        -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "SFRAW_HOST_ENABLE_TSAN requires Clang or GCC")
  endif()
elseif(SFRAW_HOST_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sfraw_host_sanitizers INTERFACE
                           -fsanitize=address,undefined,float-cast-overflow
                           -fno-omit-frame-pointer)
    target_link_options(sfraw_host_sanitizers INTERFACE
                        -fsanitize=address,undefined,float-cast-overflow
                        -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR
            "SFRAW_HOST_ENABLE_SANITIZERS requires a Clang or GCC host compiler")
  endif()
endif()

add_library(sfraw_host_libraw STATIC ${SFRAW_HOST_LIBRAW_SOURCES})
target_include_directories(sfraw_host_libraw PUBLIC "${SFRAW_HOST_LIBRAW_DIR}")

# Keep the regression build aligned with the Android decoder's dependency
# surface. LIBRAW_CALLOC_RAWSTORE makes partially decoded hostile buffers
# deterministic and is part of the hardened 0.22.2 build contract. X3FTOOLS is
# enabled only in this hostile-input harness so the patched dormant Sigma model
# probe stays covered. This does not qualify the full parser for Android.
target_compile_definitions(sfraw_host_libraw PRIVATE
                           LIBRAW_NODLL
                           LIBRAW_CALLOC_RAWSTORE
                           LIBRAW_MAX_PROFILE_SIZE_MB=16
                           LIBRAW_MAX_METADATA_ALLOC_SIZE_MB=16
                           LIBRAW_MAX_TILE_STREAM_COUNT=131072
                           NO_JASPER
                           NO_JPEG
                           USE_X3FTOOLS
                           USE_ZLIB)
target_compile_options(sfraw_host_libraw PRIVATE -w)
target_link_libraries(sfraw_host_libraw PUBLIC sfraw_host_sanitizers)

find_package(ZLIB REQUIRED)
target_link_libraries(sfraw_host_libraw PUBLIC ZLIB::ZLIB)

if(SFRAW_HOST_REQUIRE_OPENMP AND NOT SFRAW_HOST_ENABLE_OPENMP)
  message(FATAL_ERROR
          "SFRAW_HOST_REQUIRE_OPENMP needs SFRAW_HOST_ENABLE_OPENMP=ON")
endif()

if(SFRAW_HOST_ENABLE_OPENMP)
  find_package(OpenMP QUIET COMPONENTS CXX)
  if(OpenMP_CXX_FOUND)
    target_link_libraries(sfraw_host_libraw PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(sfraw_host_libraw
                               PUBLIC SFRAW_HOST_OPENMP_ACTIVE=1)
    message(STATUS "sfraw host regressions: OpenMP enabled")
  elseif(SFRAW_HOST_REQUIRE_OPENMP)
    message(FATAL_ERROR
            "sfraw host regressions: OpenMP was required but is unavailable")
  else()
    message(WARNING
            "sfraw host regressions: OpenMP unavailable; the vendor resolver's "
            "wavelet patch check still runs, but this build will not execute "
            "OpenMP paths")
  endif()
endif()

add_library(sfraw_public_api_test_support STATIC public_api_test_support.cpp)
target_include_directories(sfraw_public_api_test_support
                           PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(sfraw_public_api_test_support
                      PUBLIC sfraw_host_libraw sfraw_host_sanitizers)

add_executable(sfraw_libraw_public_api_test test_libraw_public_api.cpp)
target_link_libraries(sfraw_libraw_public_api_test
                      PRIVATE sfraw_public_api_test_support)

find_package(Threads REQUIRED)
add_executable(sfraw_ljpeg_concurrency_test test_ljpeg_concurrency.cpp)
target_link_libraries(sfraw_ljpeg_concurrency_test
                      PRIVATE sfraw_public_api_test_support Threads::Threads)

add_executable(sfraw_native_allocation_registry_test
               test_native_allocation_registry.cpp
               "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp/native_allocation_registry.cpp")
target_include_directories(sfraw_native_allocation_registry_test PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp")
target_link_libraries(sfraw_native_allocation_registry_test
                      PRIVATE sfraw_host_sanitizers Threads::Threads)

# Exercise the production wrapper (not only LibRaw itself) so hostile metadata
# cannot allocate an unbounded processed image after the raw-store guard.
add_executable(sfraw_decoder_resource_limits_test
               test_raw_decoder_resource_limits.cpp
               "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp/raw_decoder.cpp")
target_include_directories(sfraw_decoder_resource_limits_test PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp")
target_compile_definitions(sfraw_decoder_resource_limits_test PRIVATE
                           SFRAW_TESTING=1
                           USE_ZLIB=1)
target_link_libraries(sfraw_decoder_resource_limits_test
                      PRIVATE sfraw_public_api_test_support)

# First-party bounded TIFF/DNG diagnostic parser. Force the lightweight
# no-LibRaw branch even on developer hosts that happen to install system LibRaw;
# this keeps the test independent from the pinned dependency target above.
set(SFRAW_RAW_DECODER_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp/raw_decoder.cpp")
add_executable(sfraw_dng_sniffer_test
               "${CMAKE_CURRENT_LIST_DIR}/../cpp/test_dng_sniffer.cpp")
target_include_directories(sfraw_dng_sniffer_test PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp")
target_compile_definitions(sfraw_dng_sniffer_test PRIVATE
                           SFRAW_FORCE_NO_LIBRAW=1
                           USE_ZLIB=1)
target_compile_options(sfraw_dng_sniffer_test PRIVATE
                       -include "${SFRAW_RAW_DECODER_SOURCE}")
target_link_libraries(sfraw_dng_sniffer_test PRIVATE sfraw_host_sanitizers)

if(BUILD_TESTING)
  add_test(
    NAME sfraw.libraw.public-api.hostile-inputs
    COMMAND sfraw_libraw_public_api_test
            "${CMAKE_CURRENT_LIST_DIR}/../corpus")
  set_tests_properties(
    sfraw.libraw.public-api.hostile-inputs
    PROPERTIES
      TIMEOUT 30
      ENVIRONMENT
        "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
  add_test(NAME sfraw.dng-sniffer.hostile-ifd
           COMMAND sfraw_dng_sniffer_test)
  set_tests_properties(
    sfraw.dng-sniffer.hostile-ifd
    PROPERTIES
      TIMEOUT 10
      ENVIRONMENT
        "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
  add_test(NAME sfraw.decoder.resource-limits
           COMMAND sfraw_decoder_resource_limits_test)
  set_tests_properties(
    sfraw.decoder.resource-limits
    PROPERTIES
      TIMEOUT 10
      ENVIRONMENT
        "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1:strict_string_checks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
  add_test(NAME sfraw.ljpeg.concurrent-first-use
           COMMAND sfraw_ljpeg_concurrency_test)
  set_tests_properties(
    sfraw.ljpeg.concurrent-first-use
    PROPERTIES
      TIMEOUT 10
      ENVIRONMENT
        "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;TSAN_OPTIONS=halt_on_error=1")
  add_test(NAME sfraw.jni.native-allocation-registry
           COMMAND sfraw_native_allocation_registry_test)
  set_tests_properties(
    sfraw.jni.native-allocation-registry
    PROPERTIES
      TIMEOUT 10
      ENVIRONMENT
        "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;TSAN_OPTIONS=halt_on_error=1")
endif()

# Compile the fuzzer entry point even in ordinary test builds. This catches API
# drift without requiring libFuzzer to be installed on every developer machine.
add_library(sfraw_libraw_fuzz_entry OBJECT fuzz_libraw_public_api.cpp)
target_include_directories(sfraw_libraw_fuzz_entry
                           PRIVATE "${SFRAW_HOST_LIBRAW_DIR}")
target_link_libraries(sfraw_libraw_fuzz_entry PRIVATE sfraw_host_sanitizers)

if(SFRAW_HOST_BUILD_FUZZER)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SFRAW_HOST_BUILD_FUZZER requires Clang")
  endif()
  if(NOT SFRAW_HOST_ENABLE_SANITIZERS)
    message(FATAL_ERROR
            "The LibRaw fuzzer must instrument the dependency; enable sanitizers")
  endif()

  # Instrument the dependency itself, not only the tiny entry-point TU. Without
  # fuzzer-no-link on LibRaw, libFuzzer can execute inputs but receives no edge
  # coverage from the parser/decoder it is meant to explore.
  target_compile_options(sfraw_host_libraw PRIVATE -fsanitize=fuzzer-no-link)
  target_compile_options(sfraw_libraw_fuzz_entry PRIVATE
                         -fsanitize=fuzzer-no-link)

  add_executable(sfraw_libraw_seed_writer write_fuzz_seeds.cpp)
  target_link_libraries(sfraw_libraw_seed_writer
                        PRIVATE sfraw_public_api_test_support)

  set(SFRAW_HOST_FUZZ_SEED_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-seeds")
  set(SFRAW_HOST_FUZZ_SEED_STAMP
      "${SFRAW_HOST_FUZZ_SEED_DIR}/.sfraw-generated")
  add_custom_command(
    OUTPUT "${SFRAW_HOST_FUZZ_SEED_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${SFRAW_HOST_FUZZ_SEED_DIR}"
    COMMAND $<TARGET_FILE:sfraw_libraw_seed_writer>
            "${CMAKE_CURRENT_LIST_DIR}/../corpus"
            "${SFRAW_HOST_FUZZ_SEED_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${SFRAW_HOST_FUZZ_SEED_STAMP}"
    DEPENDS
      sfraw_libraw_seed_writer
      "${CMAKE_CURRENT_LIST_DIR}/../corpus/tiff-newsubfiletype-float-overflow-844.hex"
    COMMENT "Generating binary LibRaw public-seam fuzz seeds"
    VERBATIM)
  add_custom_target(sfraw_libraw_fuzz_corpus
                    DEPENDS "${SFRAW_HOST_FUZZ_SEED_STAMP}")

  add_executable(sfraw_libraw_fuzzer
                 $<TARGET_OBJECTS:sfraw_libraw_fuzz_entry>)
  target_link_libraries(sfraw_libraw_fuzzer PRIVATE sfraw_host_libraw)
  target_compile_options(sfraw_libraw_fuzzer PRIVATE -fsanitize=fuzzer)
  target_link_options(sfraw_libraw_fuzzer PRIVATE -fsanitize=fuzzer)
  add_dependencies(sfraw_libraw_fuzzer sfraw_libraw_fuzz_corpus)

  add_executable(sfraw_dng_sniffer_fuzzer
                 "${CMAKE_CURRENT_LIST_DIR}/../cpp/fuzz_dng_sniffer.cpp")
  target_include_directories(sfraw_dng_sniffer_fuzzer PRIVATE
                             "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp")
  target_compile_definitions(sfraw_dng_sniffer_fuzzer PRIVATE
                             SFRAW_FORCE_NO_LIBRAW=1
                             USE_ZLIB=1)
  target_compile_options(sfraw_dng_sniffer_fuzzer PRIVATE
                         -include "${SFRAW_RAW_DECODER_SOURCE}"
                         -fsanitize=fuzzer)
  target_link_libraries(sfraw_dng_sniffer_fuzzer PRIVATE sfraw_host_sanitizers)
  target_link_options(sfraw_dng_sniffer_fuzzer PRIVATE -fsanitize=fuzzer)
  add_dependencies(sfraw_dng_sniffer_fuzzer sfraw_libraw_fuzz_corpus)
endif()
