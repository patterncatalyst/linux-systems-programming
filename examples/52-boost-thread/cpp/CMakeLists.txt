cmake_minimum_required(VERSION 3.25)
project(boostthread CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# System Boost only -- no Conan, no network. Fedora's boost-devel provides
# 1.90; the floor below is where future continuations became stable.
#
# CMP0167 NEW: CMake 4 removed its own FindBoost module in favour of the
# BoostConfig.cmake that Boost itself installs. Setting the policy explicitly
# selects that config package and silences the transition warning; without it
# CMake still finds Boost, but by a path it has announced it is deleting.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.80 REQUIRED COMPONENTS thread system chrono)

add_executable(boostthread src/boostthread.cpp)

# These MUST be compile definitions rather than #defines in the .cpp file.
# Boost reads them while its own headers are being preprocessed, so a define
# that lands after the first Boost include is silently ignored -- and the
# symptom is not a warning but a missing .then(), i.e. a compile error in user
# code that points nowhere near the cause.
#
#   BOOST_THREAD_VERSION 5          -- the current interface generation
#   ..._PROVIDES_FUTURE             -- boost::future rather than unique_future
#   ..._FUTURE_CONTINUATION         -- .then()
#   ..._FUTURE_WHEN_ALL_WHEN_ANY    -- when_all / when_any
target_compile_definitions(boostthread PRIVATE
  BOOST_THREAD_VERSION=5
  BOOST_THREAD_PROVIDES_FUTURE
  BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION
  BOOST_THREAD_PROVIDES_FUTURE_WHEN_ALL_WHEN_ANY)

target_link_libraries(boostthread PRIVATE
  Boost::thread Boost::system Boost::chrono Threads::Threads)
