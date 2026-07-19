cmake_minimum_required(VERSION 3.25)
project(sysagent CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(app
  src/main.cpp
  src/procfs.cpp
  src/httpd.cpp
)
target_link_libraries(app PRIVATE Threads::Threads)
