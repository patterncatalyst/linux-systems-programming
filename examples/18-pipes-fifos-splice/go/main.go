// pmon v4 — the process supervisor becomes a log pipeline.
//
// v0 (`run`) spawns a command, waits, and mirrors its exit status.
// v1 (`supervise --engine sigchld`) restarts a crashing child via SIGCHLD
// through signal.Notify, reaped with wait4(WNOHANG).
// v2 (`supervise --engine pidfd`, the DEFAULT) supervises through
// unix.PidfdOpen + unix.Poll + waitid(P_PIDFD) — no pid-reuse race.
// v4 (this chapter) keeps both engines and adds pipes:
//   - supervise captures the child's stdout AND stderr through two pipes
//     into a log file as "[out] ..."/"[err] ..." lines. Where C++/Rust add
//     the pipe fds to their poll set, Go inverts idiomatically: one
//     goroutine per pipe feeds a channel of prefixed lines into the same
//     select loop that already multiplexes exit, signals, and the deadline;
//   - `tail --log F --fifo PATH` creates a FIFO and relays log bytes into
//     whatever reader attaches — io.Copy per reader session, rewinding the
//     log offset to start+written on EPIPE so nothing is lost; SIGPIPE
//     ignored, the detach reported as "pmon: tail reader detached".
package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	chunk    = 64 * 1024
	pollTick = 50 * time.Millisecond
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

// ---------------------------------------------------------------------------
// v0: run — spawn, wait, mirror (stdio inherited, no capture).
// ---------------------------------------------------------------------------

func cmdRun(cmdline []string) int {
	c := exec.Command(cmdline[0], cmdline[1:]...)
	c.Stdin, c.Stdout, c.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := c.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: spawn: %s: %v\n", cmdline[0], err)
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
// v4 capture: two pipes, line-buffered into the log with [out]/[err] tags.
// ---------------------------------------------------------------------------

type logLine struct {
	prefix string
	text   string
}

// scanInto reads one child stream line by line into the shared channel.
// Scanner also delivers a trailing partial line, so it lands in the log
// with a '\n' like everything else.
func scanInto(wg *sync.WaitGroup, r io.Reader, prefix string, lines chan<- logLine) {
	defer wg.Done()
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, chunk), chunk) // bound the line buffer
	for sc.Scan() {
		lines <- logLine{prefix: prefix, text: sc.Text()}
	}
}

func writeLine(logw *os.File, l logLine) error {
	if _, err := fmt.Fprintf(logw, "[%s] %s\n", l.prefix, l.text); err != nil {
		return fmt.Errorf("write log: %w", err)
	}
	return nil
}

// capture wires both stdio streams of an un-started command into the log.
// lines closes when both pipes hit EOF — i.e. the capture is complete.
func capture(c *exec.Cmd) (<-chan logLine, error) {
	outp, err := c.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}
	errp, err := c.StderrPipe()
	if err != nil {
		return nil, fmt.Errorf("stderr pipe: %w", err)
	}
	lines := make(chan logLine, 64)
	var wg sync.WaitGroup
	wg.Add(2)
	go scanInto(&wg, outp, "out", lines)
	go scanInto(&wg, errp, "err", lines)
	go func() {
		wg.Wait()
		close(lines)
	}()
	return lines, nil
}

// drainLines relays whatever the scanners still hold after the child was
// reaped, so every logged byte lands before the exit is reported.
func drainLines(lines <-chan logLine, logw *os.File) error {
	if lines == nil {
		return nil
	}
	for l := range lines {
		if err := writeLine(logw, l); err != nil {
			return err
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// v1/v2 engines with the v4 line channel joined into the same select.
// ---------------------------------------------------------------------------

func cmdSupervise(engine string, maxRestarts int, timeout time.Duration,
	logPath string, cmdline []string) int {
	logw, err := os.OpenFile(logPath, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: open %s: %v\n", logPath, err)
		return 1
	}
	defer logw.Close()

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
		c := exec.Command(cmdline[0], cmdline[1:]...)
		lines, err := capture(c)
		if err != nil {
			fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
			return 1
		}
		if err := c.Start(); err != nil {
			fmt.Fprintf(os.Stderr, "pmon: spawn: %s: %v\n", cmdline[0], err)
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
			linesCh := lines
			waiting := true
			for waiting {
				select {
				case l, ok := <-linesCh:
					if !ok {
						linesCh = nil
						continue
					}
					if err := writeLine(logw, l); err != nil {
						fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
						return 1
					}
				case <-exited:
					waiting = false
				case <-stop:
					stopped = "signal"
					waiting = false
				case <-deadline:
					stopped = "timeout"
					waiting = false
				}
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
			if err := drainLines(linesCh, logw); err != nil {
				fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
				return 1
			}
		} else {
			fmt.Fprintf(os.Stderr, "pmon: engine=sigchld child=%d\n", pid)
			linesCh := lines
			done := false
			for !done && stopped == "" {
				select {
				case l, ok := <-linesCh:
					if !ok {
						linesCh = nil
						continue
					}
					if err := writeLine(logw, l); err != nil {
						fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
						return 1
					}
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
			if err := drainLines(linesCh, logw); err != nil {
				fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
				return 1
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
// v4: tail — follow the log, relay into a FIFO, survive reader churn.
// ---------------------------------------------------------------------------

// ensureFifo creates PATH as a FIFO, tolerating an existing FIFO at PATH.
func ensureFifo(path string) error {
	err := unix.Mkfifo(path, 0o644)
	if err == nil {
		return nil
	}
	if !errors.Is(err, unix.EEXIST) {
		return fmt.Errorf("mkfifo %s: %w", path, err)
	}
	st, serr := os.Stat(path)
	if serr != nil {
		return fmt.Errorf("stat %s: %w", path, serr)
	}
	if st.Mode()&fs.ModeNamedPipe == 0 {
		return fmt.Errorf("%s exists and is not a FIFO", path)
	}
	return nil
}

// openWriter opens the FIFO for writing without blocking forever:
// O_NONBLOCK turns "no reader yet" into ENXIO, which we poll. Once a reader
// is attached the fd goes back to blocking mode for the relay.
func openWriter(path string, stop <-chan os.Signal) (*os.File, bool, error) {
	for {
		select {
		case <-stop:
			return nil, true, nil
		default:
		}
		fd, err := unix.Open(path, unix.O_WRONLY|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
		if err == nil {
			if err := unix.SetNonblock(fd, false); err != nil {
				unix.Close(fd)
				return nil, false, fmt.Errorf("clear O_NONBLOCK on %s: %w", path, err)
			}
			return os.NewFile(uintptr(fd), path), false, nil
		}
		if errors.Is(err, unix.ENXIO) || errors.Is(err, unix.EINTR) {
			time.Sleep(pollTick)
			continue
		}
		return nil, false, fmt.Errorf("open %s: %w", path, err)
	}
}

func cmdTail(logPath, fifo string) int {
	signal.Ignore(syscall.SIGPIPE) // EPIPE stays an error value, never fatal
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)

	if err := ensureFifo(fifo); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
		return 1
	}
	logf, err := os.Open(logPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: open %s: %v\n", logPath, err)
		return 1
	}
	defer logf.Close()
	fmt.Fprintf(os.Stderr, "pmon: tail ready (fifo %s)\n", fifo)

reader:
	for {
		w, stopped, err := openWriter(fifo, stop)
		if err != nil {
			fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
			return 1
		}
		if stopped {
			break
		}
		for {
			select {
			case <-stop:
				w.Close()
				break reader
			default:
			}
			// One copy session: on EPIPE (reader detached) io.Copy reports
			// how many bytes actually reached the pipe, so the log offset is
			// rewound to the first unwritten byte — the next reader loses
			// nothing.
			start, serr := logf.Seek(0, io.SeekCurrent)
			if serr != nil {
				w.Close()
				fmt.Fprintf(os.Stderr, "pmon: seek %s: %v\n", logPath, serr)
				return 1
			}
			n, cerr := io.Copy(w, logf)
			if cerr == nil { // caught up: poll for appended lines
				time.Sleep(pollTick)
				continue
			}
			if errors.Is(cerr, syscall.EPIPE) {
				fmt.Println("pmon: tail reader detached")
				if _, serr := logf.Seek(start+n, io.SeekStart); serr != nil {
					w.Close()
					fmt.Fprintf(os.Stderr, "pmon: seek %s: %v\n", logPath, serr)
					return 1
				}
				w.Close()
				continue reader
			}
			w.Close()
			fmt.Fprintf(os.Stderr, "pmon: relay: %v\n", cerr)
			return 1
		}
	}
	fmt.Println("pmon: exiting (signal)")
	return 0
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() int {
	fmt.Fprint(os.Stderr, `usage: pmon <command>
  run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit
  supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]
            [--log FILE] -- CMD [ARGS...]  restart CMD on abnormal exit;
                                           capture stdout/stderr into FILE
                                           (defaults: pidfd, N=3, T=10000,
                                           FILE=pmon.log)
  tail --log FILE --fifo PATH              relay appended log lines into a
                                           FIFO created at PATH
`)
	return 2
}

func realMain(args []string) int {
	if len(args) == 0 {
		return usage()
	}

	if args[0] == "tail" {
		var logPath, fifo string
		i := 1
		for i+1 < len(args) {
			switch args[i] {
			case "--log":
				logPath = args[i+1]
			case "--fifo":
				fifo = args[i+1]
			default:
				return usage()
			}
			i += 2
		}
		if i != len(args) || logPath == "" || fifo == "" {
			return usage()
		}
		return cmdTail(logPath, fifo)
	}

	sep := -1
	for i, a := range args {
		if a == "--" {
			sep = i
			break
		}
	}
	if sep < 0 || sep+1 >= len(args) {
		return usage() // run and supervise need `-- CMD [ARGS...]`
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
		logPath := "pmon.log"
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
			case "--log":
				logPath = flags[i+1]
			default:
				return usage()
			}
		}
		return cmdSupervise(engine, maxRestarts,
			time.Duration(timeoutMs)*time.Millisecond, logPath, cmdline)
	}
	return usage()
}

func main() {
	os.Exit(realMain(os.Args[1:]))
}
