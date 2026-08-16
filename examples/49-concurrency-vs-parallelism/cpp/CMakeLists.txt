cmake_minimum_required(VERSION 3.25)
project(conc CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# The main demo: one workload, four execution models. C++23, stdlib only --
# no Boost, no Conan, no network. ch50 onward add the libraries.
add_executable(conc src/conc.cpp)
target_link_libraries(conc PRIVATE Threads::Threads)

# ── C++26 Contracts (P2900), GCC only ────────────────────────────────────
#
# One source, four binaries, one per evaluation semantic. Guarded on the
# compiler id because -fcontracts is a GCC 16 spelling; clang 22 does not
# implement P2900 at all, and configuring with clang must not fail the build.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  foreach(sem ignore observe enforce quick_enforce)
    add_executable(contracts_${sem} src/contracts/contracts_demo.cpp)
    target_compile_options(contracts_${sem} PRIVATE
      -std=c++26 -fcontracts -fcontract-evaluation-semantic=${sem})
    # -fcontracts is needed at LINK time as well, not just compile time: the
    # observe and enforce semantics call into a violation handler that the
    # GCC driver only links in when the flag is present on the link line.
    # Without this, those two targets fail with an undefined reference to
    # handle_contract_violation(std::contracts::contract_violation const&).
    target_link_options(contracts_${sem} PRIVATE
      -fcontracts -fcontract-evaluation-semantic=${sem})
    # The contract syntax is C++26; override the project-wide C++23 standard
    # for these targets only, so the four-model demo stays on C++23.
    set_target_properties(contracts_${sem} PROPERTIES CXX_STANDARD 26)
  endforeach()
endif()

# ── Safety hardening (bounds-checked std::vector) ────────────────────────
#
# The same source built twice, because the interesting fact is the DIFFERENCE.
# On Fedora 44, g++ defines _GLIBCXX_ASSERTIONS by default only when it is not
# optimizing: an -O0 build gets a bounds check on std::vector::operator[] and
# an -O2 build silently does not. These two targets make that visible, and
# they are separate from `conc` so a trapping run can never take the
# four-model demo with it.
#
# oob_unchecked: this preset's default optimized flags, no assertions. The
# out-of-bounds read is undefined behavior and its output is not asserted on
# anywhere -- the gate checks statically that the bounds check is ABSENT.
add_executable(oob_unchecked src/hardening/oob.cpp)

# oob_hardened: the same source with the assertions asked for explicitly.
# Traps at run time and names the failed predicate.
add_executable(oob_hardened src/hardening/oob.cpp)
target_compile_definitions(oob_hardened PRIVATE _GLIBCXX_ASSERTIONS)
