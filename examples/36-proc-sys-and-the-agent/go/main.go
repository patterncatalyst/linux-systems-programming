// sysagent v0 (chapter 36: /proc, /sys, and the agent) — a USE-method
// metrics collector reading /proc + /sys/fs/cgroup, no root required.
//
//	sysagent sample [--json] [--interval-ms N]
//	sysagent serve  --port P [--interval-ms N]
//
// `sample` takes one snapshot: CPU utilization from a /proc/stat delta
// spanning --interval-ms (default 200), run-queue + load from
// /proc/loadavg, memory from /proc/meminfo, per-disk I/O from
// /proc/diskstats, network rx/tx from /proc/net/dev, and cgroup PSI
// (falling back to system-wide /proc/pressure) if the kernel exposes it.
// `serve` exposes the identical snapshot as JSON over an HTTP /metrics
// endpoint, one fresh sample per request.
//
// The field names in both the --json and /metrics output are the
// deterministic, cross-language schema documented in README.md — C++ and
// Rust sysagent emit byte-for-byte the same keys.
package main

import (
	"fmt"
	"os"
	"strconv"
)

func usage() {
	fmt.Fprintln(os.Stderr,
		"usage: sysagent sample [--json] [--interval-ms N] | serve --port P [--interval-ms N]")
}

// flags is the minimal hand-rolled scan shared in shape with the C++ and
// Rust argument handling, so the three stay directly comparable.
type flags struct {
	json       bool
	intervalMs int
	port       int
}

func parseFlags(args []string) (flags, bool) {
	f := flags{intervalMs: 200, port: -1}
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--json":
			f.json = true
		case "--interval-ms":
			if i+1 >= len(args) {
				return f, false
			}
			i++
			v, err := strconv.Atoi(args[i])
			if err != nil {
				return f, false
			}
			f.intervalMs = v
		case "--port":
			if i+1 >= len(args) {
				return f, false
			}
			i++
			v, err := strconv.Atoi(args[i])
			if err != nil {
				return f, false
			}
			f.port = v
		default:
			return f, false
		}
	}
	return f, true
}

func cmdSample(args []string) int {
	f, ok := parseFlags(args)
	if !ok || f.intervalMs <= 0 {
		usage()
		return 2
	}
	snap, err := TakeSnapshot(f.intervalMs)
	if err != nil {
		fmt.Fprintln(os.Stderr, "sysagent: error:", err)
		return 1
	}
	if f.json {
		fmt.Println(ToJSON(snap))
	} else {
		fmt.Print(ToText(snap))
	}
	return 0
}

func cmdServe(args []string) int {
	f, ok := parseFlags(args)
	if !ok || f.port <= 0 || f.intervalMs <= 0 {
		usage()
		return 2
	}
	return Serve(f.port, f.intervalMs)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	var code int
	switch os.Args[1] {
	case "sample":
		code = cmdSample(os.Args[2:])
	case "serve":
		code = cmdServe(os.Args[2:])
	default:
		usage()
		code = 2
	}
	os.Exit(code)
}
