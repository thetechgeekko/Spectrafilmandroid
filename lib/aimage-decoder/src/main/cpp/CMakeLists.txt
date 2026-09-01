cmake_minimum_required(VERSION 3.22.1)
project(sfaimage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")
endif ()
if (NOT CMAKE_CXX_FLAGS_DEBUG MATCHES "-O")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif ()

add_library(sfaimage SHARED
        image_input_gate.cpp
        aimage_decoder_jni.cpp
)

target_include_directories(sfaimage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sfaimage PRIVATE
        -Wall -Wextra -Werror
        -Werror=unguarded-availability
        -ffp-contract=off
)

find_library(jnigraphics-lib jnigraphics)
find_library(log-lib log)
target_link_libraries(sfaimage ${jnigraphics-lib} ${log-lib})

target_link_options(sfaimage PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/sfaimage.map.txt"
)
