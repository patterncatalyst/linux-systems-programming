// pmon v1 — a mini process supervisor (chapter 12: signals and safe handling).
//
//	pmon run -- CMD [ARGS...]
//	pmon supervise [--max-restarts N] [--backoff-ms B] -- CMD [ARGS...]
//
// The Go take on async-signal-safe design: the runtime already owns the
// signal handlers (they do nothing but forward a note to the runtime), and
// os/signal turns delivery into ordinary channel sends. Supervision is then
// a single select over three channels: signals from signal.Notify, child
// exits from a wait goroutine, and a backoff timer. No user code ever runs
// in signal context.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

const usageText = `usage: pmon run -- CMD [ARGS...]
       pmon supervise [--max-restarts N] [--backoff-ms B] -- CMD [ARGS...]`

func usage() {
	fmt.Fprintln(os.Stderr, usageText)
	os.Exit(2)
}

func die(err error) {
	fmt.Fprintf(os.Stderr, "pmon: error: %v\n", err)
	os.Exit(1)
}

// childExit is the decoded wait status of a reaped child.
type childExit struct {
	status   int
	signo    int
	signaled bool
}

func decode(ps *os.ProcessState) childExit {
	ws, ok := ps.Sys().(syscall.WaitStatus)
	if ok && ws.Signaled() {
		return childExit{signo: int(ws.Signal()), signaled: true}
	}
	return childExit{status: ps.ExitCode()}
}

func report(pid int, ce childExit) {
	if ce.signaled {
		fmt.Printf("pmon: child %d killed signal=%d\n", pid, ce.signo)
	} else {
		fmt.Printf("pmon: child %d exited status=%d\n", pid, ce.status)
	}
}

// spawn starts CMD with inherited stdio and prints the started line.
func spawn(argv []string) (*exec.Cmd, error) {
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", argv[0], err)
	}
	fmt.Printf("pmon: started pid %d\n", cmd.Process.Pid)
	return cmd, nil
}

func runOnce(argv []string) int {
	cmd, err := spawn(argv)
	if err != nil {
		die(err)
	}
	_ = cmd.Wait() // error carries the nonzero status; ProcessState has it all
	ce := decode(cmd.ProcessState)
	report(cmd.Process.Pid, ce)
	if ce.signaled {
		return 128 + ce.signo
	}
	return ce.status
}

func signame(s os.Signal) string {
	switch s {
	case unix.SIGTERM:
		return "SIGTERM"
	case unix.SIGINT:
		return "SIGINT"
	default:
		return s.String()
	}
}

type superviseOpts struct {
	maxRestarts int
	backoffMs   int64
	argv        []string
}

func supervise(opts superviseOpts) int {
	sigCh := make(chan os.Signal, 8)
	signal.Notify(sigCh, unix.SIGTERM, unix.SIGINT, unix.SIGHUP)

	// One in-flight wait goroutine per live child; the buffered channel lets
	// it finish even when the supervisor consumes the exit via stopChild.
	waitCh := make(chan childExit, 1)
	start := func() *exec.Cmd {
		cmd, err := spawn(opts.argv)
		if err != nil {
			die(err)
		}
		go func() {
			_ = cmd.Wait()
			waitCh <- decode(cmd.ProcessState)
		}()
		return cmd
	}
	// SIGTERM the child and consume its exit. Used by shutdown and reload.
	stopChild := func(cmd *exec.Cmd) {
		_ = unix.Kill(cmd.Process.Pid, unix.SIGTERM) // ESRCH: already exiting
		<-waitCh
	}

	cmd := start()
	restarts := 0
	backoff := opts.backoffMs
	var timer *time.Timer // non-nil: no child alive, waiting out a backoff

	for {
		var timerCh <-chan time.Time
		if timer != nil {
			timerCh = timer.C
		}
		select {
		case s := <-sigCh:
			switch s {
			case unix.SIGTERM, unix.SIGINT:
				if timer == nil {
					stopChild(cmd)
				}
				fmt.Printf("pmon: shutting down (%s)\n", signame(s))
				return 0
			case unix.SIGHUP:
				fmt.Println("pmon: reload requested")
				if timer == nil {
					stopChild(cmd)
				} else {
					timer.Stop()
					timer = nil
				}
				restarts = 0
				backoff = opts.backoffMs
				cmd = start()
			}
		case ce := <-waitCh:
			// The child exited on its own.
			report(cmd.Process.Pid, ce)
			if !ce.signaled && ce.status == 0 {
				return 0
			}
			if restarts >= opts.maxRestarts {
				fmt.Printf("pmon: giving up after %d restarts\n", opts.maxRestarts)
				return 1
			}
			restarts++
			fmt.Printf("pmon: restart #%d (backoff %dms)\n", restarts, backoff)
			timer = time.NewTimer(time.Duration(backoff) * time.Millisecond)
			backoff *= 2
		case <-timerCh:
			// Backoff elapsed: bring the child back.
			timer = nil
			cmd = start()
		}
	}
}

func parseSupervise(args []string) (superviseOpts, error) {
	opts := superviseOpts{maxRestarts: 5, backoffMs: 100}
	i := 0
	numericFlag := func(name string, min int64) (int64, error) {
		if i+1 >= len(args) {
			return 0, fmt.Errorf("%s needs a value", name)
		}
		i++
		v, err := strconv.ParseInt(args[i], 10, 64)
		if err != nil || v < min {
			return 0, fmt.Errorf("bad value for %s", name)
		}
		return v, nil
	}
	for i < len(args) {
		switch args[i] {
		case "--":
			i++
			opts.argv = args[i:]
			if len(opts.argv) == 0 {
				return opts, fmt.Errorf("no command after --")
			}
			return opts, nil
		case "--max-restarts":
			v, err := numericFlag("--max-restarts", 0)
			if err != nil {
				return opts, err
			}
			opts.maxRestarts = int(v)
		case "--backoff-ms":
			v, err := numericFlag("--backoff-ms", 1)
			if err != nil {
				return opts, err
			}
			opts.backoffMs = v
		default:
			return opts, fmt.Errorf("unknown flag %s", args[i])
		}
		i++
	}
	return opts, fmt.Errorf("no command after --")
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		usage()
	}
	sub, rest := args[0], args[1:]
	switch sub {
	case "run":
		if len(rest) < 2 || rest[0] != "--" {
			usage()
		}
		os.Exit(runOnce(rest[1:]))
	case "supervise":
		opts, err := parseSupervise(rest)
		if err != nil {
			fmt.Fprintf(os.Stderr, "pmon: error: %v\n", err)
			usage()
		}
		os.Exit(supervise(opts))
	default:
		usage()
	}
}
