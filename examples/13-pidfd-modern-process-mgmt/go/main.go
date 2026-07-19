// pmon v2 — a tiny process supervisor grown chapter by chapter.
//
// v0 (`run`) spawns a command, waits, and mirrors its exit status.
// v1 (`supervise --engine sigchld`) restarts a crashing child, driven by
// SIGCHLD delivered through signal.Notify and reaped with wait4(WNOHANG).
// v2 (`supervise --engine pidfd`, the DEFAULT) drops the SIGCHLD dependence
// entirely: unix.PidfdOpen turns the child into a pollable file descriptor,
// a goroutine parked in unix.Poll reports the exit over a channel, a raw
// waitid(P_PIDFD, ...) reaps exactly that child, and the stop path signals
// through the fd with unix.PidfdSendSignal — no pid-reuse race anywhere.
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

// childStatus is the reaped state of one child.
type childStatus struct {
	exited bool // true: value is the exit status; false: value is the signal
	value  int
}

// reportExit prints the exit-observation line and returns the mirrored code.
func reportExit(pid int, st childStatus) int {
	if st.exited {
		fmt.Printf("pmon: child=%d exited status=%d\n", pid, st.value)
		return st.value
	}
	fmt.Printf("pmon: child=%d killed signal=%d\n", pid, st.value)
	return 128 + st.value
}

func fromWaitStatus(ws syscall.WaitStatus) childStatus {
	if ws.Signaled() {
		return childStatus{exited: false, value: int(ws.Signal())}
	}
	return childStatus{exited: true, value: ws.ExitStatus()}
}

func spawn(cmdline []string) (*exec.Cmd, error) {
	c := exec.Command(cmdline[0], cmdline[1:]...)
	c.Stdin, c.Stdout, c.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := c.Start(); err != nil {
		return nil, fmt.Errorf("spawn: %s: %w", cmdline[0], err)
	}
	return c, nil
}

// ---------------------------------------------------------------------------
// v0: run — spawn, wait, mirror.
// ---------------------------------------------------------------------------

func cmdRun(cmdline []string) int {
	c, err := spawn(cmdline)
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
		return 1
	}
	pid := c.Process.Pid
	fmt.Fprintf(os.Stderr, "pmon: run child=%d\n", pid)
	st := childStatus{exited: true, value: 0}
	if err := c.Wait(); err != nil {
		var ee *exec.ExitError
		if !errors.As(err, &ee) {
			fmt.Fprintf(os.Stderr, "pmon: wait: %v\n", err)
			return 1
		}
		st = fromWaitStatus(ee.Sys().(syscall.WaitStatus))
	}
	return reportExit(pid, st)
}

// ---------------------------------------------------------------------------
// v2 plumbing: waitid(P_PIDFD) with an explicit siginfo_t layout — x/sys
// keeps the union opaque, and the CLD_* fields are what we need.
// ---------------------------------------------------------------------------

type siginfoChld struct {
	signo  int32
	errno  int32
	code   int32
	_      int32 // alignment padding before the union on 64-bit
	pid    int32
	uid    uint32
	status int32
	_      [100]byte // pad siginfo_t out to its full 128 bytes
}

const cldExited = 1 // si_code: child called _exit

// reapPidfd blocks in waitid(P_PIDFD) until the process behind pidfd is
// reaped — it can never collect a recycled pid.
func reapPidfd(pidfd int) (childStatus, error) {
	var si siginfoChld
	for {
		_, _, errno := unix.Syscall6(unix.SYS_WAITID, uintptr(unix.P_PIDFD),
			uintptr(pidfd), uintptr(unsafe.Pointer(&si)), uintptr(unix.WEXITED), 0, 0)
		if errno == unix.EINTR {
			continue
		}
		if errno != 0 {
			return childStatus{}, fmt.Errorf("waitid: %w", errno)
		}
		if si.code == cldExited {
			return childStatus{exited: true, value: int(si.status)}, nil
		}
		return childStatus{exited: false, value: int(si.status)}, nil
	}
}

// tryReap collects the child if it has already exited (WNOHANG).
func tryReap(pid int) (childStatus, bool, error) {
	var ws syscall.WaitStatus
	for {
		wpid, err := syscall.Wait4(pid, &ws, syscall.WNOHANG, nil)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return childStatus{}, false, fmt.Errorf("wait4: %w", err)
		}
		if wpid != pid {
			return childStatus{}, false, nil // coalesced/stale SIGCHLD
		}
		return fromWaitStatus(ws), true, nil
	}
}

func blockingReap(pid int) (childStatus, error) {
	var ws syscall.WaitStatus
	for {
		_, err := syscall.Wait4(pid, &ws, 0, nil)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return childStatus{}, fmt.Errorf("wait4: %w", err)
		}
		return fromWaitStatus(ws), nil
	}
}

// ---------------------------------------------------------------------------
// v1/v2: supervise — two engines, one restart policy.
// ---------------------------------------------------------------------------

func cmdSupervise(engine string, maxRestarts int, timeout time.Duration, cmdline []string) int {
	stop := make(chan os.Signal, 2)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	var chld chan os.Signal
	if engine == "sigchld" {
		chld = make(chan os.Signal, 16)
		signal.Notify(chld, syscall.SIGCHLD) // registered BEFORE the first spawn
	}
	deadline := time.After(timeout)

	restarts := 0
	for {
		c, err := spawn(cmdline)
		if err != nil {
			fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
			return 1
		}
		pid := c.Process.Pid

		var st childStatus
		stopped := "" // "", "signal", "timeout"

		if engine == "pidfd" {
			pidfd, err := unix.PidfdOpen(pid, 0)
			if err != nil {
				fmt.Fprintf(os.Stderr, "pmon: pidfd_open: %v\n", err)
				return 1
			}
			fmt.Fprintf(os.Stderr, "pmon: engine=pidfd child=%d pidfd=%d\n", pid, pidfd)
			exited := make(chan struct{})
			go func() { // the pidfd becomes readable exactly when the child exits
				fds := []unix.PollFd{{Fd: int32(pidfd), Events: unix.POLLIN}}
				for {
					if _, err := unix.Poll(fds, -1); err == unix.EINTR {
						continue
					}
					close(exited)
					return
				}
			}()
			select {
			case <-exited:
			case <-stop:
				stopped = "signal"
			case <-deadline:
				stopped = "timeout"
			}
			if stopped != "" {
				_ = unix.PidfdSendSignal(pidfd, unix.SIGTERM, nil, 0)
				<-exited
			}
			st, err = reapPidfd(pidfd)
			unix.Close(pidfd)
			if err != nil {
				fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
				return 1
			}
		} else {
			fmt.Fprintf(os.Stderr, "pmon: engine=sigchld child=%d\n", pid)
			done := false
			for !done && stopped == "" {
				select {
				case <-chld:
					s, reaped, err := tryReap(pid)
					if err != nil {
						fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
						return 1
					}
					if reaped {
						st, done = s, true
					}
				case <-stop:
					stopped = "signal"
				case <-deadline:
					stopped = "timeout"
				}
			}
			if stopped != "" {
				_ = syscall.Kill(pid, syscall.SIGTERM)
				if st, err = blockingReap(pid); err != nil {
					fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
					return 1
				}
			}
		}
		_ = c.Process.Release() // child already reaped; drop Go's process handle

		reportExit(pid, st)
		if stopped != "" {
			fmt.Printf("pmon: exiting (%s)\n", stopped)
			return 0
		}
		if st.exited && st.value == 0 {
			return 0
		}
		if restarts >= maxRestarts {
			fmt.Printf("pmon: giving up after %d restarts\n", maxRestarts)
			return 1
		}
		restarts++
		fmt.Printf("pmon: restart %d/%d\n", restarts, maxRestarts)
	}
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() int {
	fmt.Fprint(os.Stderr, `usage: pmon <command>
  run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit
  supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]
            -- CMD [ARGS...]               restart CMD on abnormal exit
                                           (defaults: pidfd, N=3, T=10000)
`)
	return 2
}

func realMain(args []string) int {
	if len(args) == 0 {
		return usage()
	}
	sep := -1
	for i, a := range args {
		if a == "--" {
			sep = i
			break
		}
	}
	if sep < 0 || sep+1 >= len(args) {
		return usage() // both subcommands need `-- CMD [ARGS...]`
	}
	flags := args[1:sep]
	cmdline := args[sep+1:]

	switch args[0] {
	case "run":
		if len(flags) != 0 {
			return usage()
		}
		return cmdRun(cmdline)
	case "supervise":
		engine := "pidfd"
		maxRestarts := 3
		timeoutMs := 10000
		for i := 0; i < len(flags); i += 2 {
			if i+1 >= len(flags) {
				return usage()
			}
			switch flags[i] {
			case "--engine":
				if flags[i+1] != "pidfd" && flags[i+1] != "sigchld" {
					return usage()
				}
				engine = flags[i+1]
			case "--max-restarts":
				n, err := strconv.Atoi(flags[i+1])
				if err != nil || n < 0 {
					return usage()
				}
				maxRestarts = n
			case "--timeout-ms":
				t, err := strconv.Atoi(flags[i+1])
				if err != nil || t <= 0 {
					return usage()
				}
				timeoutMs = t
			default:
				return usage()
			}
		}
		return cmdSupervise(engine, maxRestarts, time.Duration(timeoutMs)*time.Millisecond, cmdline)
	}
	return usage()
}

func main() {
	os.Exit(realMain(os.Args[1:]))
}
