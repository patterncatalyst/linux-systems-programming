cmake_minimum_required(VERSION 3.25)
project(embedded_lua CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Both sol2 and lua come from Conan (see conanfile.txt) via CMakeDeps, which
# exports namespaced CONFIG-mode targets (sol2::sol2, lua::lua) rather than
# the CMake-bundled FindLua.cmake module's LUA_LIBRARIES/LUA_INCLUDE_DIR
# variables -- find_package(lua CONFIG REQUIRED), not find_package(Lua).
find_package(sol2 CONFIG REQUIRED)
find_package(lua CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE sol2::sol2 lua::lua Threads::Threads)
