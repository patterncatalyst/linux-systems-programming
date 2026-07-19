// fwatch.go — the book's recurring file watcher (ch07 polling -> ch09
// inotify/epoll -> ch33 Landlock+seccomp), reduced here to what the fleet
// needs: watch a directory, print one line per create/write/delete event,
// and optionally run under a Landlock ruleset that restricts reads to that
// directory. Full seccomp allow-listing was proven once in ch33 and is not
// re-derived here; capability-bounding-set drop (caps.go) plus Landlock are
// this capstone's sandboxing layers, applied by pmon before it execs fwatch.
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

// ---------------------------------------------------------------------------
// Landlock — raw syscalls; x/sys/unix has the SYS_LANDLOCK_* numbers but no
// wrapper. Read-only ruleset scoped to one directory (ch33's technique).
// ---------------------------------------------------------------------------

const (
	landlockAccessFSReadFile = 1 << 2
	landlockAccessFSReadDir  = 1 << 3

	landlockCreateRulesetVersion = 1 << 0
	landlockRulePathBeneath      = 1
	landlockRestrictSelfTSYNC    = 1 << 3
)

type landlockRulesetAttr struct {
	HandledAccessFS  uint64
	HandledAccessNet uint64
	Scoped           uint64
}

type landlockPathBeneathAttr struct {
	AllowedAccess uint64
	ParentFd      int32
	_             [4]byte
}

func landlockABI() int {
	v, _, errno := unix.Syscall(unix.SYS_LANDLOCK_CREATE_RULESET, 0, 0,
		uintptr(landlockCreateRulesetVersion))
	if errno != 0 {
		return 0
	}
	return int(v)
}

func applyLandlock(dir string) error {
	attr := landlockRulesetAttr{
		HandledAccessFS: landlockAccessFSReadFile | landlockAccessFSReadDir,
	}
	rs, _, errno := unix.Syscall(unix.SYS_LANDLOCK_CREATE_RULESET,
		uintptr(unsafe.Pointer(&attr)), unsafe.Sizeof(attr), 0)
	if errno != 0 {
		return fmt.Errorf("landlock_create_ruleset: %w", errno)
	}
	rulesetFd := int(rs)
	defer unix.Close(rulesetFd)

	dirFd, err := unix.Open(dir, unix.O_PATH|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
	if err != nil {
		return fmt.Errorf("open %s: %w", dir, err)
	}
	pb := landlockPathBeneathAttr{
		AllowedAccess: landlockAccessFSReadFile | landlockAccessFSReadDir,
		ParentFd:      int32(dirFd),
	}
	_, _, errno = unix.Syscall6(unix.SYS_LANDLOCK_ADD_RULE, uintptr(rulesetFd),
		uintptr(landlockRulePathBeneath), uintptr(unsafe.Pointer(&pb)), 0, 0, 0)
	unix.Close(dirFd)
	if errno != 0 {
		return fmt.Errorf("landlock_add_rule: %w", errno)
	}

	if err := unix.Prctl(unix.PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); err != nil {
		return fmt.Errorf("prctl(PR_SET_NO_NEW_PRIVS): %w", err)
	}
	_, _, errno = unix.Syscall(unix.SYS_LANDLOCK_RESTRICT_SELF, uintptr(rulesetFd),
		uintptr(landlockRestrictSelfTSYNC), 0)
	if errno != 0 {
		_, _, errno = unix.Syscall(unix.SYS_LANDLOCK_RESTRICT_SELF, uintptr(rulesetFd), 0, 0)
		if errno != 0 {
			return fmt.Errorf("landlock_restrict_self: %w", errno)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// watch loop — inotify on the directory (non-recursive, matches ch09).
// ---------------------------------------------------------------------------

func fwatchWatch(dir string, sandbox bool, timeoutMs int) int {
	if sandbox {
		abi := landlockABI()
		if abi == 0 {
			fmt.Fprintln(os.Stderr, "fwatch: Landlock not supported by this kernel")
			return 1
		}
		if err := applyLandlock(dir); err != nil {
			fmt.Fprintf(os.Stderr, "fwatch: landlock: %v\n", err)
			return 1
		}
		fmt.Fprintf(os.Stderr, "fwatch: landlock ABI=%d enforced dir=%s\n", abi, dir)
	}

	fd, err := unix.InotifyInit1(unix.IN_CLOEXEC | unix.IN_NONBLOCK)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: inotify_init1: %v\n", err)
		return 1
	}
	defer unix.Close(fd)

	mask := uint32(unix.IN_CREATE | unix.IN_MODIFY | unix.IN_DELETE | unix.IN_CLOSE_WRITE)
	if _, err := unix.InotifyAddWatch(fd, dir, mask); err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: inotify_add_watch %s: %v\n", dir, err)
		return 1
	}

	fmt.Fprintf(os.Stderr, "fwatch: watching %s (sandbox=%v)\n", dir, sandbox)

	deadline := time.Time{}
	if timeoutMs > 0 {
		deadline = time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)
	}
	sig := installSignalFlag()

	buf := make([]byte, 4096)
	for {
		if sig.Load() {
			fmt.Println("(signal)")
			return 0
		}
		if !deadline.IsZero() && time.Now().After(deadline) {
			fmt.Println("(timeout)")
			return 0
		}
		pfd := []unix.PollFd{{Fd: int32(fd), Events: unix.POLLIN}}
		n, err := unix.Poll(pfd, 200)
		if err != nil {
			if err == unix.EINTR {
				continue
			}
			fmt.Fprintf(os.Stderr, "fwatch: poll: %v\n", err)
			return 1
		}
		if n == 0 {
			continue
		}
		nr, err := unix.Read(fd, buf)
		if err != nil || nr <= 0 {
			continue
		}
		off := 0
		for off+16 <= nr {
			nameLen := int(le32(buf[off+12 : off+16]))
			maskv := le32(buf[off+4 : off+8])
			name := ""
			if nameLen > 0 {
				end := off + 16 + nameLen
				for i := off + 16; i < end; i++ {
					if buf[i] == 0 {
						name = string(buf[off+16 : i])
						break
					}
				}
			}
			fmt.Printf("event: %s %s\n", eventName(maskv), filepath.Join(dir, name))
			off += 16 + nameLen
		}
	}
}

func le32(b []byte) uint32 {
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}

func eventName(mask uint32) string {
	switch {
	case mask&unix.IN_CREATE != 0:
		return "create"
	case mask&unix.IN_DELETE != 0:
		return "delete"
	case mask&unix.IN_CLOSE_WRITE != 0:
		return "modify"
	case mask&unix.IN_MODIFY != 0:
		return "modify"
	default:
		return "other"
	}
}

func fwatchSnapshot(dir string) int {
	entries, err := os.ReadDir(dir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: readdir %s: %v\n", dir, err)
		return 1
	}
	for _, e := range entries {
		if e.Type().IsRegular() {
			fmt.Println(e.Name())
		}
	}
	return 0
}
