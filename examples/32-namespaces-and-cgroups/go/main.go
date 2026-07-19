// pmon v6 — namespaces & cgroups (chapter 32).
//
// Subcommand:
//
//	pmon containerize [--hostname NAME] [--mem-max BYTES|max]
//	                  [--cpu-max "QUOTA PERIOD"|max] [--cgroup NAME]
//	                  -- CMD [ARGS...]
//
// Go note — why this can't be the straight-line fork(2) the C++/Rust builds
// use:
//
// A Go program is multithreaded before main() runs, and unshare(2) changes
// only the CALLING THREAD's namespaces — the first attempt below called
// unix.Unshare directly on a runtime.LockOSThread()-pinned goroutine, then
// re-exec'd the binary through os/exec so the fork would land on that same,
// already-unshared thread. It built, and the pid/hostname proof lines were
// real, but the memory-hog negative control failed on real hardware with
// "fork/exec: cannot allocate memory" (see the chapter's Errors section) —
// a genuine kernel restriction: unshare(CLONE_NEWPID) lets the caller's
// process create exactly ONE child in the fresh PID namespace before further
// forks there return ENOMEM, and os/exec's own vfork/pidfd fast path
// performs an internal probe fork on first use that silently consumes that
// one shot.
//
// The fix — and the one actually shipped here — is the same move Chapter
// 14 made for capabilities: stop calling the namespace syscall directly in
// live Go code, and instead describe it declaratively on SysProcAttr, so the
// runtime's own fork/exec trampoline (which never has this problem — it
// creates the namespaces AND the real child in the same clone(2)) applies
// it. `Cloneflags` on the re-exec's SysProcAttr creates the child already
// inside fresh mount/uts/net/pid namespaces, in one step, with no prior
// unshare() and no LockOSThread needed at all. The re-exec target — pmon
// itself, invoked with the hidden "__ns_init__" marker — is therefore where
// the mount-private remount and sethostname now run: it is already living
// inside the new namespaces the instant its code starts, since they were
// established at clone(2) time and carried across its own execve.
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"

	"golang.org/x/sys/unix"
)

// Hidden re-exec marker — not a public subcommand, so it stays out of usage().
const reexecMarker = "__ns_init__"

// Names as printed in "killed signal=<n> (<NAME>)". A hand-rolled table
// (identical across all three implementations and to pmon's earlier
// chapters) rather than the runtime's Signal.String(), whose text
// ("killed", "terminated") is prose, not the C convention.
var signalNames = [32]string{
	"", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
	"FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
	"STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
	"XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
}

func signalName(sig int) string {
	if sig >= 1 && sig < len(signalNames) {
		return signalNames[sig]
	}
	return fmt.Sprintf("SIG%d", sig)
}

// ---- cgroup helpers --------------------------------------------------

// hasToken reports whether tokens (a whitespace-separated list, as
// cgroupfs writes it) contains word as a whole token — "cpu" must not
// match inside "cpuset".
func hasToken(tokens, word string) bool {
	for _, t := range strings.Fields(tokens) {
		if t == word {
			return true
		}
	}
	return false
}

// The root cgroup is exempt from the "no internal process" constraint, so
// controllers can be enabled there even though it holds every process on
// the box — this is how a fresh sibling cgroup gets memory/cpu control
// without first relocating ourselves into a leaf.
func ensureRootControllers() error {
	data, err := os.ReadFile("/sys/fs/cgroup/cgroup.subtree_control")
	if err != nil {
		return fmt.Errorf("read cgroup.subtree_control: %w", err)
	}
	have := string(data)
	var add string
	if !hasToken(have, "memory") {
		add += "+memory "
	}
	if !hasToken(have, "cpu") {
		add += "+cpu "
	}
	if add == "" {
		return nil
	}
	if err := os.WriteFile("/sys/fs/cgroup/cgroup.subtree_control", []byte(add), 0o644); err != nil {
		return fmt.Errorf("enable controllers: %w", err)
	}
	return nil
}

// setupCgroup creates (or reuses) /sys/fs/cgroup/<name>, applies the
// limits, and moves the calling process into it. Every subsequent
// fork/exec inherits cgroup membership automatically, so CMD is under the
// limits from its first instruction.
func setupCgroup(name, memMax, cpuMax string) (string, error) {
	if err := ensureRootControllers(); err != nil {
		return "", err
	}
	path := "/sys/fs/cgroup/" + name
	if err := os.Mkdir(path, 0o755); err != nil && !os.IsExist(err) {
		return "", fmt.Errorf("mkdir %s: %w", path, err)
	}
	if err := os.WriteFile(path+"/memory.max", []byte(memMax), 0o644); err != nil {
		return "", fmt.Errorf("write memory.max: %w", err)
	}
	if err := os.WriteFile(path+"/cpu.max", []byte(cpuMax), 0o644); err != nil {
		return "", fmt.Errorf("write cpu.max: %w", err)
	}
	// Best-effort: no swap headroom, so a breach of memory.max is a real
	// OOM kill instead of a silent slowdown via swap. Not every kernel
	// exposes swap accounting the same way, so a failure here isn't fatal.
	_ = os.WriteFile(path+"/memory.swap.max", []byte("0"), 0o644)
	pid := strconv.Itoa(os.Getpid())
	if err := os.WriteFile(path+"/cgroup.procs", []byte(pid), 0o644); err != nil {
		return "", fmt.Errorf("write cgroup.procs: %w", err)
	}
	return path, nil
}

// parsePSISomeAvg10 extracts X from "some avg10=X avg60=Y avg300=Z total=T".
func parsePSISomeAvg10(psi string) string {
	idx := strings.Index(psi, "some ")
	if idx < 0 {
		return "?"
	}
	rest := psi[idx:]
	a := strings.Index(rest, "avg10=")
	if a < 0 {
		return "?"
	}
	rest = rest[a+len("avg10="):]
	if sp := strings.IndexByte(rest, ' '); sp >= 0 {
		rest = rest[:sp]
	}
	return rest
}

// ---- containerize (outer launcher) ------------------------------------

func cmdContainerize(args []string) int {
	hostname := "pmon-containerized"
	memMax := "max"
	cpuMax := "max 100000"
	cgroupName := ""
	var cmd []string

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--hostname":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "containerize: --hostname needs a value")
				return 2
			}
			hostname = args[i+1]
			i++
		case "--mem-max":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "containerize: --mem-max needs a value")
				return 2
			}
			memMax = args[i+1]
			i++
		case "--cpu-max":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "containerize: --cpu-max needs a value")
				return 2
			}
			cpuMax = args[i+1]
			i++
		case "--cgroup":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "containerize: --cgroup needs a value")
				return 2
			}
			cgroupName = args[i+1]
			i++
		case "--":
			cmd = args[i+1:]
			i = len(args)
		default:
			fmt.Fprintf(os.Stderr, "containerize: unexpected argument: %s\n", args[i])
			return 2
		}
	}
	if len(cmd) == 0 {
		fmt.Fprintln(os.Stderr, "containerize: missing -- CMD")
		return 2
	}
	if os.Getuid() != 0 {
		fmt.Fprintln(os.Stderr, "containerize: must run as root")
		return 1
	}
	if cgroupName == "" {
		cgroupName = fmt.Sprintf("pmon-%d", os.Getpid())
	}

	cgroupPath, err := setupCgroup(cgroupName, memMax, cpuMax)
	if err != nil {
		fmt.Fprintf(os.Stderr, "containerize: %v\n", err)
		return 1
	}

	self, err := os.Executable()
	if err != nil {
		fmt.Fprintf(os.Stderr, "containerize: os.Executable: %v\n", err)
		return 1
	}
	// The hostname has to travel to the re-exec'd child: it, not this
	// process, is the one that ends up inside the fresh UTS namespace (see
	// the package comment).
	initArgs := append([]string{reexecMarker, hostname, "--"}, cmd...)
	c := exec.Command(self, initArgs...)
	c.Stdin, c.Stdout, c.Stderr = os.Stdin, os.Stdout, os.Stderr
	// Cloneflags asks the runtime's own fork/exec trampoline to create the
	// child already inside fresh mount/uts/net/pid namespaces, atomically,
	// in the SAME clone(2) that creates the process — no separate unshare(2)
	// on our side, and so none of the "one child per fresh PID namespace"
	// restriction that broke the direct-unshare attempt.
	c.SysProcAttr = &syscall.SysProcAttr{
		Cloneflags: syscall.CLONE_NEWNS | syscall.CLONE_NEWUTS | syscall.CLONE_NEWNET | syscall.CLONE_NEWPID,
	}
	if err := c.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "containerize: exec %s: %v\n", cmd[0], err)
		return 1
	}

	waitErr := c.Wait()

	if psi, err := os.ReadFile(cgroupPath + "/memory.pressure"); err == nil {
		fmt.Printf("pmon: cgroup mem.pressure some=%s\n", parsePSISomeAvg10(string(psi)))
	}

	exitCode := 0
	switch {
	case waitErr == nil:
		fmt.Println("pmon: child exited status=0")
	default:
		var ee *exec.ExitError
		if errors.As(waitErr, &ee) {
			ws := ee.Sys().(syscall.WaitStatus)
			if ws.Signaled() {
				sig := int(ws.Signal())
				fmt.Printf("pmon: child killed signal=%d (%s)\n", sig, signalName(sig))
				exitCode = 128 + sig
			} else {
				exitCode = ws.ExitStatus()
				fmt.Printf("pmon: child exited status=%d\n", exitCode)
			}
		} else {
			fmt.Fprintf(os.Stderr, "containerize: wait: %v\n", waitErr)
			exitCode = 1
		}
	}

	_ = os.Remove(cgroupPath) // best-effort; a leftover cgroup is harmless

	return exitCode
}

// ---- __ns_init__ (the re-exec'd pid-1 stub) ---------------------------
//
// args: <hostname> -- CMD [ARGS...]. Already running inside the fresh
// mount/uts/net/pid namespaces (Cloneflags put THIS process there at
// clone(2) time, before this code's own execve even happened) — but the
// mount-private remount and sethostname are still ours to do, since the
// parent process never entered these namespaces itself. Then prints the
// proof lines and execs into the real CMD, replacing itself — CMD keeps
// PID 1.
func cmdNsInit(args []string) int {
	if len(args) < 2 || args[1] != "--" {
		fmt.Fprintln(os.Stderr, "__ns_init__: expected <hostname> -- CMD")
		return 2
	}
	hostname := args[0]
	cmd := args[2:]
	if len(cmd) == 0 {
		fmt.Fprintln(os.Stderr, "__ns_init__: missing CMD")
		return 2
	}

	// Detach mount propagation recursively so nothing here (or CMD) leaks a
	// mount event back to the host's mount table.
	if err := unix.Mount("none", "/", "", unix.MS_REC|unix.MS_PRIVATE, ""); err != nil {
		fmt.Fprintf(os.Stderr, "__ns_init__: mount MS_PRIVATE: %v\n", err)
		return 1
	}
	// sethostname(2) lands in the fresh UTS namespace, not the host's.
	if err := unix.Sethostname([]byte(hostname)); err != nil {
		fmt.Fprintf(os.Stderr, "__ns_init__: sethostname: %v\n", err)
		return 1
	}

	if os.Getpid() == 1 {
		fmt.Println("pmon: child sees pid 1")
	}
	if hn, err := os.Hostname(); err == nil {
		fmt.Printf("pmon: hostname=%s\n", hn)
	}
	path, err := exec.LookPath(cmd[0])
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: exec %s: %v\n", cmd[0], err)
		return 127
	}
	if err := syscall.Exec(path, cmd, os.Environ()); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: exec %s: %v\n", cmd[0], err)
		return 127
	}
	return 0 // unreachable: exec never returns on success
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage:")
	fmt.Fprintln(os.Stderr, "  pmon containerize [--hostname NAME] [--mem-max BYTES|max]")
	fmt.Fprintln(os.Stderr, "                    [--cpu-max \"QUOTA PERIOD\"|max] [--cgroup NAME]")
	fmt.Fprintln(os.Stderr, "                    -- CMD [ARGS...]")
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	var code int
	switch os.Args[1] {
	case "containerize":
		code = cmdContainerize(os.Args[2:])
	case reexecMarker:
		code = cmdNsInit(os.Args[2:])
	default:
		usage()
		code = 2
	}
	os.Exit(code)
}
