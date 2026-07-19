// app work --seconds N — a small, observable workload for the eBPF
// observation toolkit (chapter 30). It does four things in a loop, each one
// deliberate bait for a specific bcc-tools/bpftrace probe:
//
//  1. opens (create+write+close) a fixed file every iteration  -> opensnoop
//  2. fork/execs a short-lived child ("true") every 4th iter    -> execsnoop
//  3. calls the named hot function busy_hash() every iteration  -> funccount
//     / uprobe
//  4. sleeps most of the iteration, so the process is off-CPU    -> offcputime
//
// This file writes no kernel-side eBPF: it is the userspace *target* that
// examples/30-ebpf-observation-toolkit/observe.sh (running as root on the lab
// VM) points bcc-tools and bpftrace at.
//
// Go idioms: the workload loop runs in a goroutine; main blocks on a result
// channel (the goroutine's "join"). Every fallible call wraps its error with
// fmt.Errorf("...: %w", err) so the underlying syscall error survives.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"time"
)

const (
	iterInterval    = 230 * time.Millisecond
	execEveryNIters = 4
	busyRounds      = 300_000
	baitPath        = "/tmp/lsp-ebpf-work-bait.txt"
)

// busy_hash is the named hot function every uprobe/funccount/bpftrace command
// in this chapter targets by name. Go 1.26 emits it as "main.busy_hash" in
// the symbol table; //go:noinline keeps the compiler from folding it into its
// caller and making the uprobe's attach point disappear.
//
//go:noinline
func busy_hash(x uint64) uint64 {
	for i := 0; i < busyRounds; i++ {
		x ^= x << 7
		x ^= x >> 9
	}
	return x
}

// openBait is the opensnoop bait: create/truncate, write a line, close.
// Every call is a fresh open(2) on the same path.
func openBait(iter int) error {
	// 0666: the bait file may get created by a root-run observe.sh one time
	// and an unprivileged demo.sh run the next — world-writable avoids a
	// permission mismatch between those two ownership scenarios.
	f, err := os.OpenFile(baitPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o666)
	if err != nil {
		return fmt.Errorf("open bait: %w", err)
	}
	defer f.Close()
	if _, err := fmt.Fprintf(f, "iter %d\n", iter); err != nil {
		return fmt.Errorf("write bait: %w", err)
	}
	return nil
}

// spawnTrue is the execsnoop bait: exec "true" and wait for it. The Go
// runtime does the fork+exec; the child's PPID is this process either way.
func spawnTrue() (int, error) {
	cmd := exec.Command("true")
	if err := cmd.Run(); err != nil {
		return -1, fmt.Errorf("spawn true: %w", err)
	}
	return cmd.ProcessState.ExitCode(), nil
}

type result struct {
	iters, opens, execs int
	busyCalls           int
	// The accumulated busy_hash() result, printed in the summary line. Go's
	// compiler doesn't dead-code-eliminate package-level calls the way GCC's
	// interprocedural passes can, but keeping the final value observable is
	// the reliable, optimizer-proof way to guarantee every call really ran.
	busyHash uint64
}

func runWorkload(seconds int, done chan<- result) {
	deadline := time.Now().Add(time.Duration(seconds) * time.Second)
	var r result
	acc := uint64(1)
	for time.Now().Before(deadline) {
		if err := openBait(r.iters); err == nil {
			r.opens++
		} else {
			fmt.Fprintln(os.Stderr, "work:", err)
		}

		if r.iters%execEveryNIters == 0 {
			if status, err := spawnTrue(); err == nil {
				r.execs++
				fmt.Printf("work: exec i=%d status=%d\n", r.iters, status)
			} else {
				fmt.Fprintln(os.Stderr, "work:", err)
			}
		}

		acc = busy_hash(acc)
		r.busyCalls++

		time.Sleep(iterInterval)
		r.iters++
	}
	r.busyHash = acc
	done <- r
}

func cmdWork(seconds int) int {
	fmt.Printf("work: start seconds=%d pid=%d bait=%s\n", seconds, os.Getpid(), baitPath)

	// The goroutine is the worker; the channel is its "join" — main blocks
	// on the result rather than sharing counters behind a mutex.
	done := make(chan result, 1)
	go runWorkload(seconds, done)
	r := <-done

	fmt.Printf("work: done seconds=%d iters=%d opens=%d execs=%d busy_calls=%d busy_hash=%d\n",
		seconds, r.iters, r.opens, r.execs, r.busyCalls, r.busyHash)
	return 0
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app work --seconds N")
}

func main() {
	if len(os.Args) < 2 || os.Args[1] != "work" {
		usage()
		os.Exit(2)
	}
	seconds := -1
	args := os.Args[2:]
	for i := 0; i < len(args); i++ {
		if args[i] == "--seconds" && i+1 < len(args) {
			v, err := strconv.Atoi(args[i+1])
			if err != nil || v <= 0 {
				fmt.Fprintf(os.Stderr, "work: bad --seconds value: %s\n", args[i+1])
				os.Exit(2)
			}
			seconds = v
			i++
		} else {
			usage()
			os.Exit(2)
		}
	}
	if seconds <= 0 {
		usage()
		os.Exit(2)
	}
	os.Exit(cmdWork(seconds))
}
