// overheadbench: three host-local microbenchmarks used to talk about
// virtualization/container overhead in chapter 35 —
//
//	(a) syscall latency: a tight getpid(2) loop, ns/call
//	(b) memory bandwidth: sequential read of a big buffer, GB/s
//	(c) small-file IO: create/write/fsync/unlink loop, ops/s
//
//	overheadbench [--bench syscall|mem|io|all] [--iters N]
//
// Prints one line per bench: "bench=<name> metric=<value> unit=<unit>".
// This binary always measures wherever it runs (host, VM, or container) —
// the chapter runs the *same* binary in all three places and tabulates the
// numbers; this example itself only asserts the host numbers are sane.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"golang.org/x/sys/unix"
)

const usage = "usage: overheadbench [--bench syscall|mem|io|all] [--iters N]"

// Defaults are chosen so each bench runs for roughly tens to a few hundred
// milliseconds on a modern host; identical across all three languages.
const (
	syscallDefaultIters = uint64(200_000)
	memDefaultPasses    = uint64(16)
	memBufBytes         = uint64(128) * 1024 * 1024 // 128 MiB
	ioDefaultIters      = uint64(200)
	ioFileBytes         = 4096
)

type bench int

const (
	benchSyscall bench = iota
	benchMem
	benchIo
	benchAll
)

type config struct {
	bench bench
	iters *uint64
}

func parseArgs(args []string) (config, error) {
	cfg := config{bench: benchAll}
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--bench":
			i++
			if i >= len(args) {
				return config{}, fmt.Errorf("--bench requires a value")
			}
			switch args[i] {
			case "syscall":
				cfg.bench = benchSyscall
			case "mem":
				cfg.bench = benchMem
			case "io":
				cfg.bench = benchIo
			case "all":
				cfg.bench = benchAll
			default:
				return config{}, fmt.Errorf("unknown --bench value: %s", args[i])
			}
		case "--iters":
			i++
			if i >= len(args) {
				return config{}, fmt.Errorf("--iters requires a value")
			}
			n, err := strconv.ParseUint(args[i], 10, 64)
			if err != nil || n == 0 {
				return config{}, fmt.Errorf("--iters value must be a positive integer: %s", args[i])
			}
			cfg.iters = &n
		default:
			return config{}, fmt.Errorf("unknown argument: %s", args[i])
		}
	}
	return cfg, nil
}

// benchSyscallRun: tight getpid(2) loop, ns/call.
func benchSyscallRun(iters uint64) float64 {
	start := time.Now()
	for i := uint64(0); i < iters; i++ {
		// unix.Getpid issues the raw getpid(2) syscall on every call (the Go
		// runtime does not cache it) — every iteration really crosses into
		// the kernel, the syscall-boundary cost this chapter contrasts
		// against VM (vmexit trap) and container (near-zero extra) overhead.
		unix.Getpid()
	}
	ns := float64(time.Since(start).Nanoseconds())
	if ns <= 0 {
		ns = 1
	}
	return ns / float64(iters)
}

// benchMemRun: sequential read bandwidth over a big buffer, GB/s.
func benchMemRun(passes uint64) float64 {
	words := memBufBytes / 8
	buf := make([]uint64, words)
	for i := range buf {
		buf[i] = uint64(i) * 2654435761
	}

	var sum uint64
	start := time.Now()
	for p := uint64(0); p < passes; p++ {
		for _, w := range buf {
			sum += w
		}
	}
	secs := time.Since(start).Seconds()
	if secs <= 0 {
		secs = 1e-9
	}

	// Consume `sum` after the clock stops so the compiler cannot prove the
	// whole scan is dead and discard it; this doesn't affect the measured time.
	if sum == 0xFFFFFFFFFFFFFFFF {
		fmt.Fprintln(os.Stderr, "unreachable sink:", sum)
	}

	bytes := float64(memBufBytes) * float64(passes)
	return bytes / secs / 1e9
}

// benchIoRun: create/write/fsync/unlink loop, ops/s.
func benchIoRun(iters uint64) (float64, error) {
	// Relative to the CWD (the demo.sh working directory), not the system
	// temp dir: on many dev hosts that's tmpfs, where fsync is nearly free
	// and the number stops meaning anything as "disk IO overhead". The
	// example directory itself is normally on a real filesystem, which is
	// the point of comparing this number across host/VM/container.
	dir, err := os.MkdirTemp(".", "overheadbench-io-*")
	if err != nil {
		return 0, fmt.Errorf("mkdirtemp: %w", err)
	}
	defer os.RemoveAll(dir)

	path := dir + "/probe"
	payload := make([]byte, ioFileBytes)
	for i := range payload {
		payload[i] = 'x'
	}

	start := time.Now()
	for i := uint64(0); i < iters; i++ {
		f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
		if err != nil {
			return 0, fmt.Errorf("open: %w", err)
		}
		n, err := f.Write(payload)
		if err != nil || n != len(payload) {
			f.Close()
			return 0, fmt.Errorf("write: %w", err)
		}
		if err := f.Sync(); err != nil {
			f.Close()
			return 0, fmt.Errorf("fsync: %w", err)
		}
		if err := f.Close(); err != nil {
			return 0, fmt.Errorf("close: %w", err)
		}
		if err := os.Remove(path); err != nil {
			return 0, fmt.Errorf("unlink: %w", err)
		}
	}
	secs := time.Since(start).Seconds()
	if secs <= 0 {
		secs = 1e-9
	}
	return float64(iters) / secs, nil
}

type row struct {
	name  string
	unit  string
	value float64
}

func runBenches(cfg config) ([]row, error) {
	wantSyscall := cfg.bench == benchSyscall || cfg.bench == benchAll
	wantMem := cfg.bench == benchMem || cfg.bench == benchAll
	wantIo := cfg.bench == benchIo || cfg.bench == benchAll

	var rows []row
	if wantSyscall {
		iters := syscallDefaultIters
		if cfg.iters != nil {
			iters = *cfg.iters
		}
		rows = append(rows, row{"syscall", "ns/call", benchSyscallRun(iters)})
	}
	if wantMem {
		passes := memDefaultPasses
		if cfg.iters != nil {
			passes = *cfg.iters
		}
		rows = append(rows, row{"mem", "GB/s", benchMemRun(passes)})
	}
	if wantIo {
		iters := ioDefaultIters
		if cfg.iters != nil {
			iters = *cfg.iters
		}
		v, err := benchIoRun(iters)
		if err != nil {
			return nil, fmt.Errorf("io bench failed: %w", err)
		}
		rows = append(rows, row{"io", "ops/s", v})
	}
	return rows, nil
}

func main() {
	cfg, err := parseArgs(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}

	rows, err := runBenches(cfg)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	for _, r := range rows {
		fmt.Printf("bench=%s metric=%.2f unit=%s\n", r.name, r.value, r.unit)
	}
}
