// toolbelt — three focused profiling targets, one binary per language, each
// exercised by that language's native profiler (chapter 31: per-language
// toolbelts).
//
//	app <hot|alloc> [--n N] [--cpuprofile FILE] [--memprofile FILE] [--trace FILE]
//
// hot:   counts primes in [2, n) by trial division. hotSpin is the only
//
//	function that does real work — it dominates a CPU profile.
//
// alloc: builds a 1000-key string index by round-robin overwrite from n
//
//	iterations of freshly allocated strings. allocChurn is the only
//	allocation site — it dominates an allocation profile.
//
// Output: "app: mode=<hot|alloc> n=<n> result=<r> ms=<t>" on success.
//
// --cpuprofile/--memprofile/--trace are Go-only: unlike perf (which attaches
// to any binary from the outside), Go's runtime/pprof and runtime/trace are
// libraries the program calls into, so writing a profile or a trace is part
// of this binary's own behavior. C++ and Rust have no equivalent flags —
// their profiling is done externally with perf — which is why the usage
// banner differs per language even though the core <hot|alloc> [--n N]
// contract is identical.
package main

import (
	"fmt"
	"os"
	"runtime"
	"runtime/pprof"
	"runtime/trace"
	"strconv"
	"time"
)

const (
	defaultHotN   = 3_000_000
	defaultAllocN = 200_000
	keyspace      = 1_000 // distinct keys; the rest is churn
)

type config struct {
	mode       string
	n          int
	cpuProfile string
	memProfile string
	trace      string
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app <hot|alloc> [--n N] [--cpuprofile FILE] [--memprofile FILE] [--trace FILE]")
}

func parseArgs(args []string) (config, error) {
	if len(args) == 0 {
		return config{}, fmt.Errorf("missing mode")
	}
	cfg := config{mode: args[0]}
	switch cfg.mode {
	case "hot":
		cfg.n = defaultHotN
	case "alloc":
		cfg.n = defaultAllocN
	default:
		return cfg, fmt.Errorf("unknown mode: %s", cfg.mode)
	}

	for i := 1; i < len(args); i++ {
		switch {
		case args[i] == "--n" && i+1 < len(args):
			i++
			v, err := strconv.Atoi(args[i])
			if err != nil || v <= 0 {
				return cfg, fmt.Errorf("not a positive integer: %s", args[i])
			}
			cfg.n = v
		case args[i] == "--cpuprofile" && i+1 < len(args):
			i++
			cfg.cpuProfile = args[i]
		case args[i] == "--memprofile" && i+1 < len(args):
			i++
			cfg.memProfile = args[i]
		case args[i] == "--trace" && i+1 < len(args):
			i++
			cfg.trace = args[i]
		default:
			return cfg, fmt.Errorf("unknown argument: %s", args[i])
		}
	}
	return cfg, nil
}

// The CPU-bound target: trial division is deliberately unoptimized (checks
// every candidate divisor up to sqrt(i)) so a couple of seconds of runtime
// buys a profile with a single, unmistakable hot frame. //go:noinline keeps
// the compiler from folding it into run and hiding it from the sampler.
//
//go:noinline
func hotSpin(n int) uint64 {
	var count uint64
	for i := 2; i < n; i++ {
		prime := true
		for d := 2; d*d <= i; d++ {
			if i%d == 0 {
				prime = false
				break
			}
		}
		if prime {
			count++
		}
	}
	return count
}

// The allocation-heavy target: n iterations of churn, each a fresh key and
// value string, round-robin overwriting a 1000-entry index. Every
// allocation in the program happens inside this one function.
//
//go:noinline
func allocChurn(n int) (uint64, error) {
	index := make(map[string]string, keyspace)
	for i := 0; i < n; i++ {
		key := fmt.Sprintf("k%d", i%keyspace)
		value := fmt.Sprintf("%x", i)
		index[key] = value
	}

	want := n
	if want > keyspace {
		want = keyspace
	}
	if len(index) != want {
		return 0, fmt.Errorf("index has %d entries, want %d", len(index), want)
	}
	var total uint64
	for _, v := range index {
		total += uint64(len(v))
	}
	if total == 0 {
		return 0, fmt.Errorf("summed zero bytes")
	}
	return total, nil
}

func run(cfg config) (uint64, error) {
	if cfg.cpuProfile != "" {
		f, err := os.Create(cfg.cpuProfile)
		if err != nil {
			return 0, fmt.Errorf("create cpuprofile: %w", err)
		}
		defer f.Close()
		if err := pprof.StartCPUProfile(f); err != nil {
			return 0, fmt.Errorf("start cpuprofile: %w", err)
		}
		defer pprof.StopCPUProfile()
	}
	if cfg.trace != "" {
		f, err := os.Create(cfg.trace)
		if err != nil {
			return 0, fmt.Errorf("create trace: %w", err)
		}
		defer f.Close()
		if err := trace.Start(f); err != nil {
			return 0, fmt.Errorf("start trace: %w", err)
		}
		defer trace.Stop()
	}

	var result uint64
	var err error
	switch cfg.mode {
	case "hot":
		result = hotSpin(cfg.n)
	case "alloc":
		result, err = allocChurn(cfg.n)
	}
	if err != nil {
		return 0, err
	}

	if cfg.memProfile != "" {
		f, err := os.Create(cfg.memProfile)
		if err != nil {
			return 0, fmt.Errorf("create memprofile: %w", err)
		}
		defer f.Close()
		runtime.GC() // materialize a consistent snapshot before writing
		if err := pprof.WriteHeapProfile(f); err != nil {
			return 0, fmt.Errorf("write memprofile: %w", err)
		}
	}
	return result, nil
}

func main() {
	cfg, err := parseArgs(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: %v\n", err)
		usage()
		os.Exit(2)
	}

	t0 := time.Now()
	result, err := run(cfg)
	ms := time.Since(t0).Milliseconds()
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("app: mode=%s n=%d result=%d ms=%d\n", cfg.mode, cfg.n, result, ms)
}
