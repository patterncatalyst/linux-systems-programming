cmake_minimum_required(VERSION 3.25)
project(capstone CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# CMake 4 removed its own FindBoost; select Boost's installed BoostConfig.cmake.
# Guarded so older CMake still configures. (Same as ch52, ch54, and ch55.)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.80 REQUIRED COMPONENTS system thread fiber context)

add_executable(capstone src/capstone.cpp)
target_link_libraries(capstone PRIVATE
  Boost::system Boost::thread Boost::fiber Boost::context Threads::Threads)

# ch52's block, verbatim: Boost.Thread's continuations and when_all/when_any are
# behind interface-generation macros and are absent without them.
target_compile_definitions(capstone PRIVATE
  BOOST_THREAD_VERSION=5
  BOOST_THREAD_PROVIDES_FUTURE
  BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION
  BOOST_THREAD_PROVIDES_FUTURE_WHEN_ALL_WHEN_ANY)

# ch55's: keep the pre-1.66 io_service/deadline_timer spellings out of scope.
target_compile_definitions(capstone PRIVATE BOOST_ASIO_NO_DEPRECATED)

# ── the opt-in seventh arm ────────────────────────────────────────────────
#
# P2300 senders, against the NVIDIA stdexec reference implementation. Built
# ONLY when a Conan toolchain has provided stdexec -- ./demo.sh cpp build and
# the release preset never look for it, so the six-model core stays offline and
# CI on fedora:44 (no Conan) stays green. ch46 established this isolation.
find_package(stdexec QUIET)
if(stdexec_FOUND)
  add_executable(capstone-senders src/senders.cpp)
  target_link_libraries(capstone-senders PRIVATE STDEXEC::stdexec Threads::Threads)
  message(STATUS "stdexec found -- building the P2300 arm (capstone-senders)")
else()
  message(STATUS "stdexec not found -- P2300 arm skipped (six-model core is unaffected)")
endif()
