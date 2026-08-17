cmake_minimum_required(VERSION 3.25)
project(pthreads CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# THREADS_PREFER_PTHREAD_FLAG makes this -pthread rather than -lpthread, which
# matters: -pthread also sets the preprocessor state glibc expects. On glibc
# 2.34+ the pthread symbols live in libc proper, but the flag is still the
# supported spelling and keeps the build correct on older systems.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(pthreads src/pthreads.cpp)
target_link_libraries(pthreads PRIVATE Threads::Threads)
