// app — a tiny PID-1 container entrypoint (chapter 34: our programs in
// containers).
//
//	app serve   run as the container's PID 1: reap children, forward SIGTERM
//	            to the supervised worker, report the REAL container limits.
//	app naive   the trap: no signal handling installed at all. A process
//	            that is PID 1 of its own PID namespace (i.e. the container's
//	            entrypoint) is special-cased by the kernel: any signal for
//	            which it has installed no explicit handler is dropped
//	            instead of running that signal's default action. Only
//	            SIGKILL (and SIGSTOP) still work. `naive` never calls
//	            signal.Notify, so `podman stop` cannot end it gracefully --
//	            the engine has to wait out the stop timeout and fall back to
//	            SIGKILL.
//	app worker  the long-running child `serve` supervises (self-reexec).
//	app job     a short-lived child `serve` spawns periodically (self-reexec).
//
// Reaping, the Go way: unlike the C++ and Rust versions there is no
// SIGCHLD/signalfd anywhere here. os/exec's Cmd.Wait() calls wait4(2) for
// you; parking one goroutine per spawned child in Wait() *is* the reap loop.
//
// Container-aware resource detection: runtime.GOMAXPROCS(0), as of the
// go.mod "go 1.26" language version, defaults to the average cgroup CPU
// throughput limit (cpu.max quota/period) when it is lower than the host's
// cpu count -- this is the one language of the three that gets this right
// automatically. Compare this program's number against the C++ version's
// (which reports the host's cpu count no matter what `--cpus` says).
package main

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
)

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app serve|naive|worker|job")
	os.Exit(2)
}

func die(err error) {
	fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
	os.Exit(1)
}

// readTrim reads a whole file, trailing whitespace trimmed. Empty string
// (not an error) if the file cannot be opened -- cgroup files may be absent
// on unusual hosts, and callers fall back to "unknown".
func readTrim(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimRight(string(b), "\n\r ")
}

// ownCgroupPath is the cgroup v2 unified path this process is itself in:
// /proc/self/cgroup has exactly one "0::<path>" line. Falls back to root if
// unreadable (e.g. a non-cgroup-v2 host).
func ownCgroupPath() string {
	f, err := os.Open("/proc/self/cgroup")
	if err != nil {
		return "/"
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		if line := sc.Text(); strings.HasPrefix(line, "0::") {
			return strings.TrimPrefix(line, "0::")
		}
	}
	return "/"
}

// readOwnCgroupFile reads a cgroup v2 controller file for THIS process's own
// cgroup. Inside a container the cgroup namespace already makes
// "/sys/fs/cgroup" the container's own slice, so the root-relative fallback
// covers that case too.
func readOwnCgroupFile(name string) string {
	base := "/sys/fs/cgroup" + ownCgroupPath()
	if v := readTrim(base + "/" + name); v != "" {
		return v
	}
	return readTrim("/sys/fs/cgroup/" + name)
}

// cpuMaxDisplay returns "max" or "<quota>/<period>", derived from cpu.max's
// raw "max 100000" / "200000 100000" content.
func cpuMaxDisplay() string {
	raw := readOwnCgroupFile("cpu.max")
	if raw == "" {
		return "unknown"
	}
	quota, period, ok := strings.Cut(raw, " ")
	if !ok {
		return raw
	}
	if quota == "max" {
		return "max"
	}
	return quota + "/" + period
}

func memMaxDisplay() string {
	if v := readOwnCgroupFile("memory.max"); v != "" {
		return v
	}
	return "unknown"
}

func printContainerLine() {
	fmt.Printf("container: cpu.max=%s effective_parallelism=%d mem.max=%s\n",
		cpuMaxDisplay(), runtime.GOMAXPROCS(0), memMaxDisplay())
}

func workerRun() int {
	for tick := 1; ; tick++ {
		time.Sleep(2 * time.Second)
		fmt.Printf("app: worker pid=%d tick=%d\n", os.Getpid(), tick)
	}
}

func jobRun(seq string) int {
	fmt.Printf("app: job pid=%d seq=%s done\n", os.Getpid(), seq)
	return 0
}

// selfPath resolves the running binary's own path so serve can re-exec it as
// "worker"/"job" -- there is no fork(2)-without-exec in Go, so this is the
// idiomatic way to spawn "more of this program" as a real child process.
func selfPath() string {
	p, err := os.Executable()
	if err != nil {
		die(fmt.Errorf("resolve self path: %w", err))
	}
	return p
}

func serve() int {
	printContainerLine()
	fmt.Printf("app: pid=%d ppid=%d\n", os.Getpid(), os.Getppid())

	sigCh := make(chan os.Signal, 4)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)

	self := selfPath()

	worker := exec.Command(self, "worker")
	worker.Stdout, worker.Stderr = os.Stdout, os.Stderr
	if err := worker.Start(); err != nil {
		die(fmt.Errorf("start worker: %w", err))
	}
	fmt.Printf("app: worker started pid=%d\n", worker.Process.Pid)
	workerDone := make(chan struct{})
	go func() {
		_ = worker.Wait() // reaps the worker when it eventually exits
		close(workerDone)
	}()

	// One goroutine parked in Wait() per spawned job: that goroutine IS the
	// reaper for that job, no SIGCHLD involved.
	seq := 0
	spawnJob := func() {
		seq++
		job := exec.Command(self, "job", strconv.Itoa(seq))
		job.Stdout, job.Stderr = os.Stdout, os.Stderr
		if err := job.Start(); err != nil {
			fmt.Printf("app: job spawn failed: %v\n", err)
			return
		}
		go func() {
			_ = job.Wait()
			status := job.ProcessState.ExitCode()
			fmt.Printf("app: reaped pid=%d status=%d\n", job.Process.Pid, status)
		}()
	}

	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()

	for {
		select {
		case s := <-sigCh:
			// This is the pid-1 duty naive skips: stop spawning new jobs,
			// forward the signal to the worker, wait for it, and only then
			// exit -- so nothing is left behind when the container stops.
			ticker.Stop()
			name := "SIGINT"
			if s == syscall.SIGTERM {
				name = "SIGTERM"
			}
			if err := worker.Process.Signal(syscall.SIGTERM); err == nil {
				<-workerDone
			}
			fmt.Printf("app: shutting down (%s)\n", name)
			return 0
		case <-ticker.C:
			spawnJob()
		}
	}
}

// THE OTHER HALF OF THE TRAP: naive installs no signal handling whatsoever.
// As this container's PID 1, the kernel will not run SIGTERM's default
// action (terminate) for it -- there is no handler, so the signal is simply
// dropped. `podman stop` has no way to ask it to leave; only SIGKILL ends it.
func naiveRun() int {
	printContainerLine()
	fmt.Printf("app: pid=%d ppid=%d\n", os.Getpid(), os.Getppid())
	for tick := 1; ; tick++ {
		time.Sleep(time.Second)
		fmt.Printf("app: naive heartbeat tick=%d\n", tick)
	}
}

func main() {
	args := os.Args[1:]
	if len(args) < 1 {
		usage()
	}
	switch args[0] {
	case "serve":
		os.Exit(serve())
	case "naive":
		os.Exit(naiveRun())
	case "worker":
		os.Exit(workerRun())
	case "job":
		if len(args) < 2 {
			usage()
		}
		os.Exit(jobRun(args[1]))
	default:
		usage()
	}
}
