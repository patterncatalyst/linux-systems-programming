cmake_minimum_required(VERSION 3.25)
project(containers_app CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE Threads::Threads)
