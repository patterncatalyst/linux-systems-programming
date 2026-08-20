cmake_minimum_required(VERSION 3.25)
project(fiberdemo CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# CMake 4 removed its own FindBoost in favour of Boost's installed
# BoostConfig.cmake; setting the policy selects that and silences the
# transition warning. Guarded so older CMake still configures. (Same as ch52.)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.80 REQUIRED COMPONENTS fiber context)

add_executable(fiberdemo src/fiberdemo.cpp)
target_link_libraries(fiberdemo PRIVATE Boost::fiber Boost::context Threads::Threads)

# -O0 for this whole translation unit, deliberately.
#
# The guard-page cases depend on a recursion that really consumes stack. At -O2
# both compilers are entitled to inline burn_stack into a loop, drop the
# volatile array's storage, or turn the tail call into a jump -- any of which
# means the fiber never approaches its guard page and guard-overflow silently
# stops demonstrating anything. ch49 learned this the expensive way when an
# unconsumed burn loop was deleted and the example measured the optimizer
# instead of the scheduler.
#
# Nothing else in this file is performance-sensitive: every observable is a
# byte count, a tid count, or an exit signal.
target_compile_options(fiberdemo PRIVATE -O0)
