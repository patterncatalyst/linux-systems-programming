cmake_minimum_required(VERSION 3.25)
project(fwatch_seccomp_landlock CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SECCOMP REQUIRED IMPORTED_TARGET libseccomp)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE PkgConfig::SECCOMP)
