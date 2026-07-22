// caps.cpp — PR_CAPBSET_DROP for every known capability, then
// PR_SET_NO_NEW_PRIVS. See the Go caps.go for the full rationale: this
// shrinks the bounding set inherited by every fork/exec descendant, whether
// or not the calling process currently holds any capability at all.
#include "caps.hpp"

#include <cstdio>

#include <sys/prctl.h>

namespace caps {

namespace {
constexpr int kLastKnownCap = 40; // CAP_CHECKPOINT_RESTORE, current as of 6.x
}

void drop_bounding_set() {
    int dropped = 0;
    for (int c = 0; c <= kLastKnownCap; ++c) {
        if (::prctl(PR_CAPBSET_DROP, c, 0, 0, 0) == 0) {
            ++dropped;
        }
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        std::fprintf(stderr, "pmon: prctl(PR_SET_NO_NEW_PRIVS) failed\n");
        return;
    }
    std::fprintf(stderr, "pmon: capabilities bounding_set_dropped=%d no_new_privs=1\n", dropped);
}

} // namespace caps
