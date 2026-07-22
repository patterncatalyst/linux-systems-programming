// caps.go — capability-bounding-set drop, the cheap, always-available half of
// this chapter's sandboxing story (the other half, Landlock, lives in
// fwatch.go and was fully proven with seccomp alongside it in ch33; this
// capstone does not re-derive the seccomp BPF program, only reuses the
// mechanism it already established).
//
// PR_CAPBSET_DROP removes a capability from the *bounding set*: even a future
// execve of a setuid-root binary can never regain it, for this process and
// every descendant (the bounding set is inherited across fork/exec and can
// only shrink). It is meaningful regardless of whether the calling process
// currently holds any capabilities at all — that is exactly pmon's case when
// deployed unprivileged on a lab guest.
package main

import (
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

// lastKnownCap is CAP_CHECKPOINT_RESTORE (40) on a 6.x+ kernel; probing past
// the kernel's actual last cap just returns EINVAL, which we ignore.
const lastKnownCap = 40

func dropCapabilityBoundingSet() {
	dropped := 0
	for c := 0; c <= lastKnownCap; c++ {
		if err := unix.Prctl(unix.PR_CAPBSET_DROP, uintptr(c), 0, 0, 0); err == nil {
			dropped++
		}
	}
	if err := unix.Prctl(unix.PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: prctl(PR_SET_NO_NEW_PRIVS): %v\n", err)
		return
	}
	fmt.Fprintf(os.Stderr, "pmon: capabilities bounding_set_dropped=%d no_new_privs=1\n", dropped)
}
