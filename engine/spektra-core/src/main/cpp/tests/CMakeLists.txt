cmake_minimum_required(VERSION 3.16)
project(spektra_native_input_host_tests LANGUAGES CXX)

include(CTest)

option(SPK_NPY_TEST_SANITIZERS "Enable ASan and UBSan for native input tests" OFF)
option(SPK_NATIVE_INPUT_TEST_SANITIZERS
    "Enable ASan and UBSan for all native hostile-input regressions" OFF)
option(SPK_JSON_PROFILE_FUZZER "Build the bounded JSON/profile libFuzzer target" OFF)

find_package(Threads REQUIRED)

add_executable(spektra_jni_safety_regression
    test_jni_safety.cpp
)
target_compile_features(spektra_jni_safety_regression PRIVATE cxx_std_17)
target_include_directories(spektra_jni_safety_regression PRIVATE ..)
target_link_libraries(spektra_jni_safety_regression PRIVATE Threads::Threads)

add_executable(spektra_parallel_exceptions_regression
    test_parallel_exceptions.cpp
    ../kernels/parallel.cpp
)
target_compile_features(spektra_parallel_exceptions_regression PRIVATE cxx_std_17)
target_include_directories(spektra_parallel_exceptions_regression PRIVATE ..)
target_link_libraries(spektra_parallel_exceptions_regression PRIVATE Threads::Threads)

add_executable(spektra_fft_convolve_regression
    test_fft_convolve.cpp
    ../kernels/fft.cpp
    ../kernels/fft_convolve.cpp
    ../kernels/parallel.cpp
)
target_compile_features(spektra_fft_convolve_regression PRIVATE cxx_std_17)
target_compile_definitions(spektra_fft_convolve_regression PRIVATE
    SPK_FFT_CONVOLVE_TEST_HOOKS=1)
target_include_directories(spektra_fft_convolve_regression PRIVATE ..)
target_link_libraries(spektra_fft_convolve_regression PRIVATE Threads::Threads)

add_executable(spektra_tc_lut_cache_regression
    test_tc_lut_cache.cpp
    ../runtime/tc_lut_cache.cpp
)
target_compile_features(spektra_tc_lut_cache_regression PRIVATE cxx_std_17)
target_include_directories(spektra_tc_lut_cache_regression PRIVATE ..)
target_link_libraries(spektra_tc_lut_cache_regression PRIVATE Threads::Threads)

# These small regression executables use assert as their fail-closed test primitive. CMake's
# Release flags define NDEBUG; explicitly undo it so CI/release builds execute every assertion
# instead of compiling the tests into no-ops.
foreach(target spektra_jni_safety_regression spektra_parallel_exceptions_regression
               spektra_tc_lut_cache_regression)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endforeach()

add_executable(spektra_npy_lut_regression
    test_npy_lut.cpp
    ../io/npy_lut.cpp
)
target_compile_features(spektra_npy_lut_regression PRIVATE cxx_std_17)
target_include_directories(spektra_npy_lut_regression PRIVATE ..)

set(SPK_JSON_PROFILE_SOURCES
    ../profiles/profile.cpp
    ../runtime/print_digest.cpp
    ../model/color_output.cpp
    ../model/density_curves.cpp
    ../model/gamut_compression.cpp
    ../kernels/spectral_upsampling.cpp
    ../kernels/interp.cpp
    ../kernels/parallel.cpp
)

add_executable(spektra_json_profile_regression
    test_json_profile.cpp
    ${SPK_JSON_PROFILE_SOURCES}
)
target_compile_features(spektra_json_profile_regression PRIVATE cxx_std_17)
target_include_directories(spektra_json_profile_regression PRIVATE ..)
target_link_libraries(spektra_json_profile_regression PRIVATE Threads::Threads)

if(SPK_NPY_TEST_SANITIZERS OR SPK_NATIVE_INPUT_TEST_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        foreach(target
                spektra_jni_safety_regression
                spektra_parallel_exceptions_regression
                spektra_fft_convolve_regression
                spektra_tc_lut_cache_regression
                spektra_npy_lut_regression
                spektra_json_profile_regression)
            target_compile_options(${target} PRIVATE
                -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
                -fno-sanitize-recover=all)
            target_link_options(${target} PRIVATE
                -fsanitize=address,undefined -fno-sanitize-recover=all)
        endforeach()
    else()
        message(FATAL_ERROR "Native input sanitizers require Clang or GCC")
    endif()
endif()

add_test(NAME spektra.jni-safety.memory-budget
    COMMAND spektra_jni_safety_regression)
set_tests_properties(spektra.jni-safety.memory-budget PROPERTIES TIMEOUT 10)

add_test(NAME spektra.parallel.exception-containment
    COMMAND spektra_parallel_exceptions_regression)
set_tests_properties(spektra.parallel.exception-containment PROPERTIES TIMEOUT 10)

add_test(NAME spektra.fft-convolve.denial-and-parity
    COMMAND spektra_fft_convolve_regression)
set_tests_properties(spektra.fft-convolve.denial-and-parity PROPERTIES TIMEOUT 30)

add_test(NAME spektra.tc-lut-cache.bounded-lru
    COMMAND spektra_tc_lut_cache_regression)
set_tests_properties(spektra.tc-lut-cache.bounded-lru PROPERTIES TIMEOUT 30)

add_test(NAME spektra.npy-lut.hostile-inputs
    COMMAND spektra_npy_lut_regression
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/spektra/luts/spectral_upsampling/irradiance_xy_tc.npy")
set_tests_properties(spektra.npy-lut.hostile-inputs PROPERTIES TIMEOUT 5)

add_test(NAME spektra.json-profile.hostile-inputs
    COMMAND spektra_json_profile_regression
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/spektra/profiles"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/spektra/filters/neutral_print_filters.json")
set_tests_properties(spektra.json-profile.hostile-inputs PROPERTIES TIMEOUT 30)

if(SPK_JSON_PROFILE_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SPK_JSON_PROFILE_FUZZER requires Clang")
    endif()
    add_executable(spektra_json_profile_fuzzer
        fuzz_json_profile.cpp
        ${SPK_JSON_PROFILE_SOURCES}
    )
    target_compile_features(spektra_json_profile_fuzzer PRIVATE cxx_std_17)
    target_include_directories(spektra_json_profile_fuzzer PRIVATE ..)
    target_link_libraries(spektra_json_profile_fuzzer PRIVATE Threads::Threads)
    target_compile_options(spektra_json_profile_fuzzer PRIVATE
        -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all)
    target_link_options(spektra_json_profile_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
endif()
