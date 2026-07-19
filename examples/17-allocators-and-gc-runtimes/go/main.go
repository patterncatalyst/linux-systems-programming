// allocbench — allocation-heavy index build/query (chapter 17: allocators
// and GC runtimes).
//
//	allocbench [--allocs N] [--variant default|arena]
//
// Both variants build the same string->string index (1000 distinct keys,
// overwritten round-robin) from N iterations of short-lived intermediate
// strings, then query every distinct key back. What differs is who pays for
// the intermediates:
//
//   - default: every key/frag/value is a freshly allocated string. All of
//     that garbage is the GC's problem — watch gc_cycles climb with N.
//   - arena: Go has no std::pmr / bumpalo equivalent, so the idiom is
//     sync.Pool: each iteration borrows a *scratch (three bytes.Buffers and
//     a number-formatting []byte) from the pool, builds the strings in
//     place with strconv.AppendInt (no per-digit garbage), copies only the
//     final key/value out, and returns the scratch. The churn objects are
//     recycled instead of collected.
//
// Reports: "allocbench: variant=<v> allocs=<n> peak_rss=<kb>KB ms=<t> gc_cycles=<c>"
// where gc_cycles is the delta of runtime.MemStats.NumGC across the
// build+query phase (the Go-only field — C++ and Rust have no collector),
// and peak_rss comes from unix.Getrusage (ru_maxrss, KB).
//
// The GC's appetite is tunable without touching this code: GOGC scales how
// much garbage may pile up between cycles, and GOMEMLIMIT puts a soft cap
// on the runtime's total memory (see the README).
package main

import (
	"bytes"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"time"

	"golang.org/x/sys/unix"
)

const (
	defaultAllocs = 200_000
	keyspace      = 1_000 // distinct keys; the rest is churn
	repeat        = 4     // value = frag repeated `repeat` times
)

type config struct {
	allocs int
	arena  bool
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: allocbench [--allocs N] [--variant default|arena]")
}

func parseArgs(args []string) (config, error) {
	cfg := config{allocs: defaultAllocs}
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "--allocs" && i+1 < len(args):
			i++
			n, err := strconv.Atoi(args[i])
			if err != nil || n <= 0 {
				return cfg, fmt.Errorf("not a positive integer: %s", args[i])
			}
			cfg.allocs = n
		case args[i] == "--variant" && i+1 < len(args):
			i++
			switch args[i] {
			case "default":
				cfg.arena = false
			case "arena":
				cfg.arena = true
			default:
				return cfg, fmt.Errorf("unknown variant: %s", args[i])
			}
		default:
			return cfg, fmt.Errorf("unknown argument: %s", args[i])
		}
	}
	return cfg, nil
}

// buildDefault: every intermediate is a fresh allocation; the collector
// sweeps up behind each iteration.
func buildDefault(allocs int) map[string]string {
	index := make(map[string]string, keyspace)
	for i := 0; i < allocs; i++ {
		idx := i % keyspace
		key := "key-" + strconv.Itoa(idx)
		frag := "value-" + strconv.Itoa(idx) + "-" + strconv.Itoa(i%97) + "/"
		value := ""
		for r := 0; r < repeat; r++ {
			value += frag // deliberate churn: each += is a new string
		}
		index[key] = value
	}
	return index
}

// scratch is the pooled churn object: reusable buffers for key, frag, and
// value plus a []byte for strconv.AppendInt so digits never allocate.
type scratch struct {
	key, frag, value bytes.Buffer
	num              []byte
}

var scratchPool = sync.Pool{New: func() any { return &scratch{num: make([]byte, 0, 20)} }}

func (s *scratch) reset() {
	s.key.Reset()
	s.frag.Reset()
	s.value.Reset()
}

func (s *scratch) writeNum(b *bytes.Buffer, v int) {
	s.num = strconv.AppendInt(s.num[:0], int64(v), 10)
	b.Write(s.num)
}

// buildArena: same logical work, but the intermediates live in pooled
// buffers that are recycled instead of collected. Only the final key/value
// strings are copied out into the long-lived index.
func buildArena(allocs int) map[string]string {
	index := make(map[string]string, keyspace)
	for i := 0; i < allocs; i++ {
		s := scratchPool.Get().(*scratch)
		s.reset()
		idx := i % keyspace
		s.key.WriteString("key-")
		s.writeNum(&s.key, idx)
		s.frag.WriteString("value-")
		s.writeNum(&s.frag, idx)
		s.frag.WriteByte('-')
		s.writeNum(&s.frag, i%97)
		s.frag.WriteByte('/')
		for r := 0; r < repeat; r++ {
			s.value.Write(s.frag.Bytes())
		}
		index[s.key.String()] = s.value.String()
		scratchPool.Put(s)
	}
	return index
}

func query(index map[string]string, allocs int) (uint64, error) {
	distinct := min(allocs, keyspace)
	var total uint64
	for idx := 0; idx < distinct; idx++ {
		key := "key-" + strconv.Itoa(idx)
		value, ok := index[key]
		if !ok {
			return 0, fmt.Errorf("missing key: %s", key)
		}
		total += uint64(len(value))
	}
	if total == 0 {
		return 0, fmt.Errorf("query summed zero bytes")
	}
	return total, nil
}

func peakRSSKB() (int64, error) {
	var ru unix.Rusage
	if err := unix.Getrusage(unix.RUSAGE_SELF, &ru); err != nil {
		return 0, fmt.Errorf("getrusage: %w", err)
	}
	return ru.Maxrss, nil // kilobytes on Linux
}

func run(cfg config) error {
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)

	t0 := time.Now()
	var index map[string]string
	if cfg.arena {
		index = buildArena(cfg.allocs)
	} else {
		index = buildDefault(cfg.allocs)
	}
	if _, err := query(index, cfg.allocs); err != nil {
		return err
	}
	ms := time.Since(t0).Milliseconds()

	runtime.ReadMemStats(&after)
	peak, err := peakRSSKB()
	if err != nil {
		return err
	}

	variant := "default"
	if cfg.arena {
		variant = "arena"
	}
	fmt.Printf("allocbench: variant=%s allocs=%d peak_rss=%dKB ms=%d gc_cycles=%d\n",
		variant, cfg.allocs, peak, ms, after.NumGC-before.NumGC)
	return nil
}

func main() {
	cfg, err := parseArgs(os.Args[1:])
	if err != nil {
		fmt.Fprintf(os.Stderr, "allocbench: %v\n", err)
		usage()
		os.Exit(2)
	}
	if err := run(cfg); err != nil {
		fmt.Fprintf(os.Stderr, "allocbench: %v\n", err)
		os.Exit(1)
	}
}
