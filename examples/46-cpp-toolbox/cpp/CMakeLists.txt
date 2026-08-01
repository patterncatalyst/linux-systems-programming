cmake_minimum_required(VERSION 3.25)
project(toolbox CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(toolbox
    src/toolbox.cpp
    src/digest.cpp
    src/smell.cpp
)
target_include_directories(toolbox PRIVATE src)

# The Conan sub-target (cpp/conan/) pulls one small dependency (fmt) via a
# Conan-generated CMakeDeps config. It is added ONLY when the "conan" preset
# turns this option on (which also points CMAKE_TOOLCHAIN_FILE at Conan's
# generated toolchain) -- every other preset, including "release" (what
# ./demo.sh cpp build and fedora:44 CI build-smoke use), never evaluates
# this subdirectory and never needs Conan installed.
option(TOOLBOX_ENABLE_CONAN "Build the Conan-provided fmt sub-target" OFF)
if(TOOLBOX_ENABLE_CONAN)
    add_subdirectory(conan)
endif()
