// toolbox -- the reference C++23 program for ch46 (CMake presets, Conan
// lockfiles, GCC/clang parity, gdb pretty-printers, clang-tidy/clang-format,
// ccache/mold).
//
// Subcommands:
//   toolbox report           -- deterministic FNV-1a digest over a fixed
//                                embedded payload, integer/string output
//                                only (no floats, addresses, timing, or
//                                unordered-container iteration) so GCC and
//                                clang builds are byte-identical.
//   toolbox defect overflow  -- deliberately triggers signed-integer
//                                overflow, for the UBSan recipe.
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "digest.hpp"
#include "smell.hpp"

namespace {

void print_usage() {
    std::fprintf(stderr, "usage: toolbox <report|defect overflow>\n");
}

int cmd_report() {
    const Digest d = fnv1a(kPayload.data(), kPayload.size());
    const std::string label = decorate_label("report");
    std::printf("toolbox report: label=%s payload_len=%zu digest=0x%016llx\n", label.c_str(),
                kPayload.size(), static_cast<unsigned long long>(d.fnv));
    return 0;
}

int cmd_defect_overflow() {
    // `volatile` blocks the compiler from constant-folding the overflow at
    // compile time -- the point is a *runtime* UBSan trap, not a compiler
    // diagnostic. This one statement is the entire seeded defect.
    volatile int max_value = INT_MAX;
    std::printf("toolbox defect overflow: max_value=%d\n", max_value);
    std::fflush(stdout);            // UBSan's trap handler may skip normal atexit flushing.
    int overflowed = max_value + 1; // UB: signed integer overflow.
    std::printf("unreachable: overflowed=%d\n", overflowed);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "report") {
        return cmd_report();
    }
    if (cmd == "defect") {
        if (argc < 3) {
            print_usage();
            return 2;
        }
        const std::string_view sub = argv[2];
        if (sub == "overflow") {
            return cmd_defect_overflow();
        }
        std::fprintf(stderr, "unknown defect: %.*s\n", static_cast<int>(sub.size()), sub.data());
        return 2;
    }
    print_usage();
    return 2;
}
