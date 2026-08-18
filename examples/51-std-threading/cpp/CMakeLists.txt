cmake_minimum_required(VERSION 3.25)
project(stdthread CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# No optimization games here: the whole chapter is about which syscalls the
# standard primitives issue, and that does not change with -O level. But the
# increment loops must not be deleted -- every one of them writes to a
# variable that report() prints, which is what keeps them alive.
add_executable(stdthread src/stdthread.cpp)
target_link_libraries(stdthread PRIVATE Threads::Threads)
