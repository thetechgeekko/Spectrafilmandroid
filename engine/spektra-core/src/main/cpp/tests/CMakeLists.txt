cmake_minimum_required(VERSION 3.16)
project(spektra_npy_host_tests LANGUAGES CXX)

include(CTest)

option(SPK_NPY_TEST_SANITIZERS "Enable ASan and UBSan for the NPY host test" OFF)

add_executable(spektra_npy_lut_regression
    test_npy_lut.cpp
    ../io/npy_lut.cpp
)
target_compile_features(spektra_npy_lut_regression PRIVATE cxx_std_17)
target_include_directories(spektra_npy_lut_regression PRIVATE ..)

if(SPK_NPY_TEST_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(spektra_npy_lut_regression PRIVATE
            -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
            -fno-sanitize-recover=all)
        target_link_options(spektra_npy_lut_regression PRIVATE
            -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
        message(FATAL_ERROR "SPK_NPY_TEST_SANITIZERS requires Clang or GCC")
    endif()
endif()

add_test(NAME spektra.npy-lut.hostile-inputs
    COMMAND spektra_npy_lut_regression
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/spektra/luts/spectral_upsampling/irradiance_xy_tc.npy")
set_tests_properties(spektra.npy-lut.hostile-inputs PROPERTIES TIMEOUT 5)
