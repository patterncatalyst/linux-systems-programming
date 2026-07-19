// fwatch v3 — the ch09 watcher, now sandboxed.
//
// `watch --sandbox DIR` applies two independent kernel access-control layers
// before the watch loop starts:
//
//   Landlock — a ruleset restricting filesystem READS to DIR. Unprivileged,
//              self-imposed, enforced by the kernel against this process and
//              every descendant.
//   seccomp  — a syscall allowlist (a hand-assembled classic-BPF program,
//              installed via the raw seccomp(2) syscall) covering only what
//              the watch loop needs. Everything else returns EPERM.
//
// GO RUNTIME CAVEAT: both mechanisms are, by default, per-THREAD kernel
// state. The Go runtime schedules goroutines across OS threads it creates
// and destroys on its own schedule (GC workers, sysmon, threads parked in
// blocking syscalls) — applying either sandbox to only the calling thread
// would leave those other threads unrestricted, and a goroutine the runtime
// later migrates onto one of them would silently run outside the sandbox.
// This program avoids that two ways:
//
//   - runtime.LockOSThread() pins main's goroutine to the OS thread that
//     installs the sandbox, so the thread issuing the syscalls below never
//     changes out from under it.
//   - Both landlock_restrict_self and the seccomp(2) syscall are called with
//     their respective TSYNC flag, which applies the new state to every
//     thread in the process atomically — not just the calling one — so
//     threads that already exist (and the ones any of them later clone) are
//     covered too.
//
// Landlock has no x/sys/unix wrapper; every call goes through the raw
// syscall numbers x/sys/unix does export (SYS_LANDLOCK_*). The ABI is probed
// at runtime (LANDLOCK_CREATE_RULESET_VERSION), never assumed.
package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const debounce = 100 * time.Millisecond

// ---------------------------------------------------------------------------
// Landlock — raw syscalls (x/sys/unix has the numbers, not wrappers).
// ---------------------------------------------------------------------------

const (
	landlockAccessFSReadFile = 1 << 2
	landlockAccessFSReadDir  = 1 << 3

	landlockCreateRulesetVersion = 1 << 0
	landlockRulePathBeneath      = 1
	landlockRestrictSelfTSYNC    = 1 << 3
)

// Mirrors struct landlock_ruleset_attr from <linux/landlock.h>.
type landlockRulesetAttr struct {
	HandledAccessFS  uint64
	HandledAccessNet uint64
	Scoped           uint64
}

// Mirrors struct landlock_path_beneath_attr from <linux/landlock.h>.
type landlockPathBeneathAttr struct {
	AllowedAccess uint64
	ParentFd      int32
	_             [4]byte // struct padding to keep the layout syscall-compatible
}

// landlockABI probes the running kernel's supported Landlock ABI version.
// Returns 0 if Landlock isn't built in / enabled at boot.
func landlockABI() int {
	v, _, errno := unix.Syscall(unix.SYS_LANDLOCK_CREATE_RULESET, 0, 0,
		uintptr(landlockCreateRulesetVersion))
	if errno != 0 {
		return 0
	}
	return int(v)
}

// applyLandlock restricts filesystem reads to dir (READ_FILE + READ_DIR,
// both available since ABI v1) and enforces it with restrict_self+TSYNC so
// every OS thread the Go runtime is already running is covered too.
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

	// TSYNC first (syncs the domain to every thread already running);
	// fall back to per-thread-only on a kernel too old to support it.
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
// seccomp — a hand-assembled classic-BPF allowlist, installed via seccomp(2).
// ---------------------------------------------------------------------------

const (
	bpfLD  = 0x00
	bpfW   = 0x00
	bpfABS = 0x20
	bpfJMP = 0x05
	bpfJEQ = 0x10
	bpfK   = 0x00
	bpfRET = 0x06

	// Offsets into struct seccomp_data (<linux/seccomp.h>): { int nr;
	// __u32 arch; __u64 instruction_pointer; __u64 args[6]; }.
	seccompDataNrOff   = 0
	seccompDataArchOff = 4
)

type sockFilter struct {
	Code uint16
	Jt   uint8
	Jf   uint8
	K    uint32
}

// Mirrors struct sock_fprog from <linux/filter.h>; the trailing padding
// keeps Filter's pointer naturally aligned on amd64.
type sockFprog struct {
	Len uint16
	_   [6]byte
	Filter *sockFilter
}

func bpfStmt(code uint16, k uint32) sockFilter { return sockFilter{Code: code, K: k} }
func bpfJump(code uint16, k uint32, jt, jf uint8) sockFilter {
	return sockFilter{Code: code, Jt: jt, Jf: jf, K: k}
}

// buildAllowlist assembles: check arch == x86_64 (else kill), then one
// equality test per allowed syscall number that jumps to RET_ALLOW on match;
// falling off the end returns RET_ERRNO(denyErrno).
func buildAllowlist(allowed []uint32, denyErrno uint32) []sockFilter {
	prog := []sockFilter{
		bpfStmt(bpfLD|bpfW|bpfABS, seccompDataArchOff),
		bpfJump(bpfJMP|bpfJEQ|bpfK, unix.AUDIT_ARCH_X86_64, 1, 0),
		bpfStmt(bpfRET, unix.SECCOMP_RET_KILL_PROCESS),
		bpfStmt(bpfLD|bpfW|bpfABS, seccompDataNrOff),
	}
	for i, nr := range allowed {
		remaining := len(allowed) - i - 1
		prog = append(prog, bpfJump(bpfJMP|bpfJEQ|bpfK, nr, uint8(remaining+1), 0))
	}
	prog = append(prog, bpfStmt(bpfRET, unix.SECCOMP_RET_ERRNO|(denyErrno&unix.SECCOMP_RET_DATA)))
	prog = append(prog, bpfStmt(bpfRET, unix.SECCOMP_RET_ALLOW))
	return prog
}

// installSeccomp loads an allowlisting filter with SECCOMP_FILTER_FLAG_TSYNC
// so every OS thread already running in this process is covered, not just
// the calling one. Returns the number of syscalls admitted.
func installSeccomp(allowed []uint32, denyErrno uint32) (int, error) {
	// seccomp(2) requires no_new_privs (or CAP_SYS_ADMIN), same as Landlock;
	// harmless to set again if applyLandlock already did.
	if err := unix.Prctl(unix.PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); err != nil {
		return 0, fmt.Errorf("prctl(PR_SET_NO_NEW_PRIVS): %w", err)
	}
	prog := buildAllowlist(allowed, denyErrno)
	fprog := sockFprog{Len: uint16(len(prog)), Filter: &prog[0]}
	_, _, errno := unix.Syscall(unix.SYS_SECCOMP, uintptr(unix.SECCOMP_SET_MODE_FILTER),
		uintptr(unix.SECCOMP_FILTER_FLAG_TSYNC), uintptr(unsafe.Pointer(&fprog)))
	if errno != 0 {
		return 0, errno
	}
	return len(allowed), nil
}

// Syscalls the watch loop issues, empirically confirmed with `strace -f`
// across a full watch session (goroutine reading the inotify fd through the
// netpoller, signal.Notify, time.After/time.Timer) — plus the small,
// well-known set the Go runtime itself needs for scheduling, GC, and stack
// management on any goroutine (futex, mmap/munmap/mprotect, clone, sigaltstack,
// rt_sigaction). Anything else returns EPERM once the filter loads.
var watchSyscalls = []uint32{
	uint32(unix.SYS_READ), uint32(unix.SYS_WRITE), uint32(unix.SYS_CLOSE),
	uint32(unix.SYS_EPOLL_CREATE1), uint32(unix.SYS_EPOLL_CTL), uint32(unix.SYS_EPOLL_PWAIT),
	uint32(unix.SYS_EPOLL_WAIT), uint32(unix.SYS_INOTIFY_INIT1), uint32(unix.SYS_INOTIFY_ADD_WATCH),
	uint32(unix.SYS_EVENTFD2), uint32(unix.SYS_FCNTL),
	uint32(unix.SYS_RT_SIGACTION), uint32(unix.SYS_RT_SIGPROCMASK), uint32(unix.SYS_RT_SIGRETURN),
	uint32(unix.SYS_SIGALTSTACK), uint32(unix.SYS_TGKILL),
	uint32(unix.SYS_MMAP), uint32(unix.SYS_MUNMAP), uint32(unix.SYS_MPROTECT), uint32(unix.SYS_MADVISE),
	uint32(unix.SYS_BRK), uint32(unix.SYS_FUTEX), uint32(unix.SYS_CLONE), uint32(unix.SYS_SCHED_YIELD),
	uint32(unix.SYS_SCHED_GETAFFINITY), uint32(unix.SYS_GETPID), uint32(unix.SYS_GETTID),
	uint32(unix.SYS_CLOCK_GETTIME), uint32(unix.SYS_CLOCK_NANOSLEEP), uint32(unix.SYS_NANOSLEEP),
	uint32(unix.SYS_GETRANDOM), uint32(unix.SYS_OPENAT), uint32(unix.SYS_FSTAT), uint32(unix.SYS_NEWFSTATAT),
	uint32(unix.SYS_PSELECT6), uint32(unix.SYS_SET_ROBUST_LIST), uint32(unix.SYS_GETRUSAGE),
	uint32(unix.SYS_EXIT), uint32(unix.SYS_EXIT_GROUP),
}

// ---------------------------------------------------------------------------
// v0: snapshot + diff
// ---------------------------------------------------------------------------

func cmdSnapshot(dir string) error {
	entries, err := os.ReadDir(dir)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return fmt.Errorf("%s: no such directory", dir)
		}
		return fmt.Errorf("%s: %w", dir, err)
	}
	for _, e := range entries {
		info, err := os.Lstat(filepath.Join(dir, e.Name()))
		if err != nil {
			continue
		}
		if !info.Mode().IsRegular() {
			continue
		}
		st := info.Sys().(*syscall.Stat_t)
		mtimeNs := int64(st.Mtim.Sec)*1_000_000_000 + int64(st.Mtim.Nsec)
		fmt.Printf("%s\t%d\t%d\n", e.Name(), info.Size(), mtimeNs)
	}
	return nil
}

func loadSnapshot(path string) (map[string]string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	out := make(map[string]string)
	for _, line := range strings.Split(string(data), "\n") {
		tab2 := strings.LastIndexByte(line, '\t')
		if tab2 <= 0 {
			continue
		}
		tab1 := strings.LastIndexByte(line[:tab2], '\t')
		if tab1 < 0 {
			continue
		}
		out[line[:tab1]] = line[tab1+1:]
	}
	return out, nil
}

func cmdDiff(oldPath, newPath string) error {
	oldSnap, err := loadSnapshot(oldPath)
	if err != nil {
		return err
	}
	newSnap, err := loadSnapshot(newPath)
	if err != nil {
		return err
	}
	seen := make(map[string]bool, len(oldSnap)+len(newSnap))
	names := make([]string, 0, len(oldSnap)+len(newSnap))
	for name := range oldSnap {
		seen[name] = true
		names = append(names, name)
	}
	for name := range newSnap {
		if !seen[name] {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	for _, name := range names {
		o, inOld := oldSnap[name]
		n, inNew := newSnap[name]
		switch {
		case !inOld:
			fmt.Printf("created %s\n", name)
		case !inNew:
			fmt.Printf("deleted %s\n", name)
		case o != n:
			fmt.Printf("modified %s\n", name)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// v1: watch — goroutine + channels; the runtime owns the epoll loop.
// ---------------------------------------------------------------------------

type fsEvent struct {
	kind string
	name string
}

func classify(mask uint32) (string, bool) {
	switch {
	case mask&(unix.IN_CREATE|unix.IN_MOVED_TO) != 0:
		return "created", true
	case mask&(unix.IN_DELETE|unix.IN_MOVED_FROM) != 0:
		return "deleted", true
	case mask&(unix.IN_MODIFY|unix.IN_ATTRIB) != 0:
		return "modified", true
	}
	return "", false
}

func mergeKinds(oldKind, newKind string) string {
	switch {
	case oldKind == "":
		return newKind
	case newKind == "deleted":
		return "deleted"
	case oldKind == "created":
		return "created"
	default:
		return "modified"
	}
}

func readEvents(f *os.File, ch chan<- fsEvent) {
	defer close(ch)
	buf := make([]byte, 4096)
	for {
		n, err := f.Read(buf)
		if err != nil {
			return
		}
		for off := 0; off+unix.SizeofInotifyEvent <= n; {
			raw := (*unix.InotifyEvent)(unsafe.Pointer(&buf[off]))
			nameBytes := buf[off+unix.SizeofInotifyEvent : off+unix.SizeofInotifyEvent+int(raw.Len)]
			off += unix.SizeofInotifyEvent + int(raw.Len)
			name := strings.TrimRight(string(nameBytes), "\x00")
			if name == "" {
				continue
			}
			if kind, ok := classify(raw.Mask); ok {
				ch <- fsEvent{kind: kind, name: name}
			}
		}
	}
}

type pendingEvent struct {
	kind string
	due  time.Time
}

func runWatchLoop(dir string, timeoutMs int) error {
	ifd, err := unix.InotifyInit1(unix.IN_NONBLOCK | unix.IN_CLOEXEC)
	if err != nil {
		return fmt.Errorf("inotify_init1: %w", err)
	}
	watchMask := uint32(unix.IN_CREATE | unix.IN_MODIFY | unix.IN_ATTRIB |
		unix.IN_DELETE | unix.IN_MOVED_FROM | unix.IN_MOVED_TO)
	if _, err := unix.InotifyAddWatch(ifd, dir, watchMask); err != nil {
		unix.Close(ifd)
		if errors.Is(err, unix.ENOENT) {
			return fmt.Errorf("%s: no such directory", dir)
		}
		return fmt.Errorf("%s: %w", dir, err)
	}
	inotifyFile := os.NewFile(uintptr(ifd), "inotify")
	defer inotifyFile.Close()

	events := make(chan fsEvent, 64)
	go readEvents(inotifyFile, events)

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	defer signal.Stop(sigs)

	fmt.Fprintf(os.Stderr, "fwatch: watching %s\n", dir)
	timeout := time.After(time.Duration(timeoutMs) * time.Millisecond)

	pending := make(map[string]*pendingEvent)
	flushTimer := time.NewTimer(time.Hour)
	flushTimer.Stop()
	defer flushTimer.Stop()

	flush := func(all bool) {
		now := time.Now()
		names := make([]string, 0, len(pending))
		for name, p := range pending {
			if all || !p.due.After(now) {
				names = append(names, name)
			}
		}
		sort.Strings(names)
		for _, name := range names {
			fmt.Printf("event: %s %s\n", pending[name].kind, name)
			delete(pending, name)
		}
	}
	rearm := func() {
		if len(pending) == 0 {
			flushTimer.Stop()
			return
		}
		earliest := time.Time{}
		for _, p := range pending {
			if earliest.IsZero() || p.due.Before(earliest) {
				earliest = p.due
			}
		}
		flushTimer.Reset(time.Until(earliest))
	}

	for {
		select {
		case ev, ok := <-events:
			if !ok {
				return errors.New("inotify reader stopped unexpectedly")
			}
			oldKind := ""
			if p, exists := pending[ev.name]; exists {
				oldKind = p.kind
			}
			pending[ev.name] = &pendingEvent{
				kind: mergeKinds(oldKind, ev.kind),
				due:  time.Now().Add(debounce),
			}
			rearm()
		case <-flushTimer.C:
			flush(false)
			rearm()
		case <-timeout:
			flush(true)
			fmt.Println("fwatch: exiting (timeout)")
			return nil
		case <-sigs:
			flush(true)
			fmt.Println("fwatch: exiting (signal)")
			return nil
		}
	}
}

func cmdWatch(dir string, timeoutMs int, sandbox bool) error {
	if sandbox {
		runtime.LockOSThread() // keep the sandbox-installing thread stable

		abi := landlockABI()
		if abi <= 0 {
			return errors.New("Landlock not supported by this kernel")
		}
		if err := applyLandlock(dir); err != nil {
			return fmt.Errorf("landlock: %w", err)
		}
		fmt.Fprintf(os.Stderr, "fwatch: landlock ABI=%d enforced\n", abi)

		n, err := installSeccomp(watchSyscalls, uint32(unix.EPERM))
		if err != nil {
			return fmt.Errorf("seccomp: %w", err)
		}
		fmt.Fprintf(os.Stderr, "fwatch: seccomp filter installed (%d syscalls allowed)\n", n)
	}
	return runWatchLoop(dir, timeoutMs)
}

// ---------------------------------------------------------------------------
// probes — negative controls, one Landlock, one seccomp, exercised alone.
// ---------------------------------------------------------------------------

// Exit codes distinct from the ordinary 0/1/2 contract: a verifier can tell
// "confirmed denied" (the PASSING case for a negative control) apart from
// "some other error" or "was NOT denied" (a sandbox bug).
const (
	probeDenied    = 20
	probeNotDenied = 21
)

func cmdProbeOutside(sandboxDir, outsidePath string) int {
	runtime.LockOSThread()
	abi := landlockABI()
	if abi <= 0 {
		fmt.Fprintln(os.Stderr, "fwatch: probe: Landlock not supported by this kernel")
		return 1
	}
	if err := applyLandlock(sandboxDir); err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: probe: landlock: %v\n", err)
		return 1
	}
	fmt.Fprintf(os.Stderr, "fwatch: landlock ABI=%d enforced\n", abi)

	f, err := os.Open(outsidePath)
	if err == nil {
		f.Close()
		fmt.Printf("fwatch: probe outside %s: opened (landlock did NOT block this)\n", outsidePath)
		return probeNotDenied
	}
	if errors.Is(err, os.ErrPermission) {
		fmt.Printf("fwatch: probe outside %s: EACCES (%v)\n", outsidePath, err)
		return probeDenied
	}
	fmt.Printf("fwatch: probe outside %s: unexpected error: %v\n", outsidePath, err)
	return 1
}

func cmdProbeForbiddenSyscall() int {
	runtime.LockOSThread()
	// socket(2) is deliberately absent from the allowlist.
	n, err := installSeccomp(watchSyscalls, uint32(unix.EPERM))
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: probe: seccomp: %v\n", err)
		return 1
	}
	fmt.Fprintf(os.Stderr, "fwatch: seccomp filter installed (%d syscalls allowed)\n", n)

	s, err := unix.Socket(unix.AF_INET, unix.SOCK_STREAM, 0)
	if err == nil {
		unix.Close(s)
		fmt.Println("fwatch: probe forbidden-syscall: socket() unexpectedly succeeded")
		return probeNotDenied
	}
	if errors.Is(err, unix.EPERM) {
		fmt.Printf("fwatch: probe forbidden-syscall: EPERM (%v)\n", err)
		return probeDenied
	}
	fmt.Printf("fwatch: probe forbidden-syscall: unexpected error: %v\n", err)
	return 1
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() {
	fmt.Fprintln(os.Stderr, "usage: fwatch <command>")
	fmt.Fprintln(os.Stderr, "  snapshot DIR                              one line per regular file")
	fmt.Fprintln(os.Stderr, "  diff OLD NEW                              compare two snapshots")
	fmt.Fprintln(os.Stderr, "  watch DIR [--timeout-ms T]                unsandboxed watch")
	fmt.Fprintln(os.Stderr, "  watch --sandbox DIR [--timeout-ms T]      Landlock+seccomp sandboxed watch")
	fmt.Fprintln(os.Stderr, "  probe --sandbox DIR --outside PATH        negative control: open PATH (outside DIR) under Landlock")
	fmt.Fprintln(os.Stderr, "  probe --forbidden-syscall                 negative control: socket(2) under a seccomp allowlist that omits it")
	os.Exit(2)
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		usage()
	}

	switch args[0] {
	case "snapshot":
		if len(args) != 2 {
			usage()
		}
		if err := cmdSnapshot(args[1]); err != nil {
			fmt.Fprintf(os.Stderr, "fwatch: snapshot: %v\n", err)
			os.Exit(1)
		}
		return

	case "diff":
		if len(args) != 3 {
			usage()
		}
		if err := cmdDiff(args[1], args[2]); err != nil {
			fmt.Fprintf(os.Stderr, "fwatch: diff: %v\n", err)
			os.Exit(1)
		}
		return

	case "watch":
		sandbox := false
		var dir string
		timeoutMs := 2000
		for i := 1; i < len(args); i++ {
			switch {
			case args[i] == "--sandbox" && i+1 < len(args):
				sandbox = true
				i++
				dir = args[i]
			case args[i] == "--timeout-ms" && i+1 < len(args):
				i++
				v, err := strconv.Atoi(args[i])
				if err != nil || v <= 0 {
					usage()
				}
				timeoutMs = v
			case dir == "":
				dir = args[i]
			default:
				usage()
			}
		}
		if dir == "" {
			usage()
		}
		if err := cmdWatch(dir, timeoutMs, sandbox); err != nil {
			fmt.Fprintf(os.Stderr, "fwatch: watch: %v\n", err)
			os.Exit(1)
		}
		return

	case "probe":
		var sandboxDir, outside string
		forbiddenSyscall := false
		for i := 1; i < len(args); i++ {
			switch {
			case args[i] == "--sandbox" && i+1 < len(args):
				i++
				sandboxDir = args[i]
			case args[i] == "--outside" && i+1 < len(args):
				i++
				outside = args[i]
			case args[i] == "--forbidden-syscall":
				forbiddenSyscall = true
			default:
				usage()
			}
		}
		switch {
		case forbiddenSyscall && sandboxDir == "" && outside == "":
			os.Exit(cmdProbeForbiddenSyscall())
		case sandboxDir != "" && outside != "" && !forbiddenSyscall:
			os.Exit(cmdProbeOutside(sandboxDir, outside))
		default:
			usage()
		}

	default:
		usage()
	}
}
