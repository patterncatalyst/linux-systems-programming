cmake_minimum_required(VERSION 3.25)
project(coro CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# C++23 for <generator>. Coroutines themselves are C++20, but ch27 covered
# those hand-rolled; the C++23 standard generator is one of this chapter's
# measurements. No threads, no Boost, no network -- this chapter is about what
# a suspended computation costs, and none of that needs a second thread.
add_executable(coro src/coro.cpp)
