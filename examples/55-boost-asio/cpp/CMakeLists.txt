cmake_minimum_required(VERSION 3.25)
project(asiodemo CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# CMake 4 removed its own FindBoost; select Boost's installed BoostConfig.cmake.
# Guarded so older CMake still configures. (Same as ch52 and ch54.)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
# Asio itself is header-only; boost_system supplies the error_category symbols.
find_package(Boost 1.80 REQUIRED COMPONENTS system)

add_executable(asiodemo src/asiodemo.cpp)
target_link_libraries(asiodemo PRIVATE Boost::system Threads::Threads)

# BOOST_ASIO_NO_DEPRECATED keeps the old io_service/deadline_timer spellings out
# of scope, so nothing in this example accidentally demonstrates an interface
# Asio has replaced.
target_compile_definitions(asiodemo PRIVATE BOOST_ASIO_NO_DEPRECATED)
