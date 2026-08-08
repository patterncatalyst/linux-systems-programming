// oob -- C++26-era safety hardening, ch49's gate F.
//
// std::vector::operator[] performs no bounds check in the standard: an
// out-of-range subscript is undefined behavior, full stop. libstdc++ can add
// a check when _GLIBCXX_ASSERTIONS is defined, and on Fedora 44 g++ defines
// it FOR YOU -- but only when it is not optimizing. Measured on this host:
//
//   g++ -std=c++23            -> _GLIBCXX_ASSERTIONS defined     -> traps
//   g++ -std=c++23 -O2        -> _GLIBCXX_ASSERTIONS NOT defined -> no check
//   g++ -std=c++23 -O2 -D_GLIBCXX_ASSERTIONS -> defined          -> traps
//
// So the safety net is on in the build where you are already watching, and
// off in the build you ship, unless you ask for it. CMakeLists.txt builds
// this file twice to make that difference the observable.
//
// Nothing here is #ifdef'd: the whole point is that one unchanged source
// behaves differently depending on how it was compiled.

#include <cstdio>
#include <vector>

int main() {
    std::vector<int> v{1, 2, 3};
    std::printf("hardening: size=%zu, reading v[7]...\n", v.size());
    std::fflush(stdout);

    // Out of bounds on purpose. In the hardened build this line aborts with
    // an assertion naming the failed predicate. In the unchecked build it is
    // undefined behavior -- whatever it prints is not a result, and this
    // example never asserts on it.
    std::printf("hardening: v[7] = %d\n", v[7]);
    std::printf("hardening: survived the out-of-bounds read\n");
    return 0;
}
