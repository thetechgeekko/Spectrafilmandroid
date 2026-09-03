cmake_minimum_required(VERSION 3.22.1)
project(sfaimage_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()
add_executable(test_image_input_gate
        ../../main/cpp/image_input_gate.cpp
        test_image_input_gate.cpp
)
target_include_directories(test_image_input_gate PRIVATE ../../main/cpp)
if (MSVC)
    target_compile_options(test_image_input_gate PRIVATE /W4 /WX)
else ()
    target_compile_options(test_image_input_gate PRIVATE -Wall -Wextra -Werror)
endif ()
add_test(NAME image_input_gate COMMAND test_image_input_gate)
