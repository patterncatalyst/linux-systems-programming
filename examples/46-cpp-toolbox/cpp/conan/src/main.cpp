// Isolated Conan sub-target (ch46 "Conan 2 lockfiles"): proves the
// CMakeDeps/CMakeToolchain-generated fmt package links and runs. Not
// wired into the toolbox binary or its digest -- this line is not part of
// the GCC-vs-clang parity gate.
#include <fmt/core.h>

int main() {
    fmt::print("toolbox-conan: fmt sub-target ok\n");
    return 0;
}
