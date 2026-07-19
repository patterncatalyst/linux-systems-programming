// pmon run -- CMD [ARGS...] — minimal process monitor for the process
// lifecycle chapter: spawn CMD, wait for it, report its fate and rusage.
//
//	pmon: pid <p> exited status <s>              child exited normally
//	pmon: pid <p> killed by signal <n> (<NAME>)  child died on a signal
//	pmon: rusage maxrss=<kb>KB user=<s>.<ms>s sys=<s>.<ms>s wall=<ms>ms
//
// pmon's own exit code mirrors the child: the exit status, or 128+signal.
// If exec itself fails, stderr gets "pmon: exec <cmd>: <reason>" and pmon
// exits 127 with no report lines.
//
// Where C++ forks and execs by hand, Go's os/exec does the same dance
// internally (fork/exec with a CLOEXEC status pipe — see syscall.forkExec);
// the exec failure that the C++ version ships over its self-pipe surfaces
// here as an error from cmd.Start. The wait-side facts come from
// os.ProcessState: Sys() is the raw wait status, SysUsage() the rusage that
// wait4(2) filled in for exactly this child.
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"time"
	"unicode"
)

// Index 1..31; identical to the C++ and Rust tables. Not strsignal(3) prose.
var sigNames = [...]string{
	"", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS",
	"FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM",
	"STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
	"XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
}

func sigName(n int) string {
	if n >= 1 && n < len(sigNames) {
		return sigNames[n]
	}
	return fmt.Sprintf("SIG%d", n)
}

// "<sec>.<ms>s" from a wait4 timeval, e.g. "0.004s".
func formatCPU(tv syscall.Timeval) string {
	return fmt.Sprintf("%d.%03ds", tv.Sec, tv.Usec/1000)
}

func capitalize(s string) string {
	r := []rune(s)
	if len(r) > 0 {
		r[0] = unicode.ToUpper(r[0])
	}
	return string(r)
}

// reason renders a spawn failure in strerror(3) shape. Go's errno strings are
// lowercase ("no such file or directory"), so the first letter is capitalized
// to match what the C++ and Rust versions print; a PATH miss (exec.ErrNotFound)
// is what execvp(3) reports as ENOENT.
func reason(err error) string {
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return capitalize(errno.Error())
	}
	if errors.Is(err, exec.ErrNotFound) {
		return "No such file or directory"
	}
	return capitalize(err.Error())
}

// waitChild waits for the started command; a child that ran and failed is not
// an error here (exec.ExitError carries the ProcessState we want), while a
// wait-machinery failure is wrapped and propagated.
func waitChild(cmd *exec.Cmd) (*os.ProcessState, error) {
	err := cmd.Wait()
	var exitErr *exec.ExitError
	if err != nil && !errors.As(err, &exitErr) {
		return nil, fmt.Errorf("wait: %w", err)
	}
	return cmd.ProcessState, nil
}

func monitor(name string, args []string) int {
	cmd := exec.Command(name, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr

	start := time.Now()
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: exec %s: %s\n", name, reason(err))
		return 127
	}
	state, err := waitChild(cmd)
	wall := time.Since(start)
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
		return 1
	}

	ws, ok := state.Sys().(syscall.WaitStatus)
	if !ok {
		fmt.Fprintln(os.Stderr, "pmon: no wait status for child")
		return 1
	}
	ru, ok := state.SysUsage().(*syscall.Rusage)
	if !ok {
		fmt.Fprintln(os.Stderr, "pmon: no rusage for child")
		return 1
	}

	var code int
	switch {
	case ws.Exited():
		code = ws.ExitStatus()
		fmt.Printf("pmon: pid %d exited status %d\n", state.Pid(), code)
	case ws.Signaled():
		sig := int(ws.Signal())
		code = 128 + sig
		fmt.Printf("pmon: pid %d killed by signal %d (%s)\n", state.Pid(), sig, sigName(sig))
	default:
		fmt.Fprintf(os.Stderr, "pmon: unexpected wait status %#x\n", uint32(ws))
		return 1
	}

	fmt.Printf("pmon: rusage maxrss=%dKB user=%s sys=%s wall=%dms\n",
		ru.Maxrss, formatCPU(ru.Utime), formatCPU(ru.Stime), wall.Milliseconds())
	return code
}

func main() {
	if len(os.Args) < 4 || os.Args[1] != "run" || os.Args[2] != "--" {
		fmt.Fprintln(os.Stderr, "usage: pmon run -- CMD [ARGS...]")
		os.Exit(2)
	}
	os.Exit(monitor(os.Args[3], os.Args[4:]))
}
