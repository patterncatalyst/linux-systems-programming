// workq — a bounded MPMC job queue and worker pool (Go 1.26).
//
// P producers push N items total into a bounded blocking queue (a buffered
// channel — the CSP-flavored blocking queue), C consumers pop and fold each
// item's payload into a checksum. A closer goroutine waits on the producers'
// WaitGroup and closes the channel, which ends every consumer's range loop.
//
// The correct path gives each consumer its own accumulator (no shared writes);
// --buggy funnels every consumer into ONE unsynchronized shared counter +
// checksum, which `go build -race` (see demo.sh) reports as a data race.
//
// Item payloads are a pure function of index and folded with XOR, so the
// correct checksum is deterministic for a given (seed, N) regardless of P/C.
package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

const (
	golden      = 0x9E3779B97F4A7C15
	mix1        = 0xBF58476D1CE4E5B9
	mix2        = 0x94D049BB133111EB
	seedDefault = 0x0123456789ABCDEF
	usage       = "usage: workq --producers P --consumers C --items N [--buggy] [--seed S] [--cap K]"
)

// payload is the splitmix64 finalizer over seed + (i+1)*golden. uint64 wraps,
// matching the C++ and Rust implementations byte for byte.
func payload(seed, i uint64) uint64 {
	x := seed + (i+1)*golden
	x = (x ^ (x >> 30)) * mix1
	x = (x ^ (x >> 27)) * mix2
	x = x ^ (x >> 31)
	return x
}

type config struct {
	producers int
	consumers int
	items     int
	cap       int
	seed      uint64
	buggy     bool
}

func parseSeed(s string) (uint64, error) {
	return strconv.ParseUint(s, 0, 64) // base 0: accepts 0x-prefixed hex
}

// parseArgs returns (cfg, msg, ok). On failure ok is false; a non-empty msg is
// printed after the "workq: " prefix, and the usage line always follows.
func parseArgs(args []string) (config, string, bool) {
	c := config{seed: seedDefault, cap: 256}
	var hasP, hasC, hasN bool
	i := 0
	need := func() (string, bool) {
		if i+1 >= len(args) {
			return "", false
		}
		i++
		return args[i], true
	}
	for ; i < len(args); i++ {
		a := args[i]
		switch a {
		case "--buggy":
			c.buggy = true
		case "--producers", "--consumers", "--items", "--cap":
			v, ok := need()
			if !ok {
				return c, a + " needs a value", false
			}
			n, err := strconv.Atoi(v)
			if err != nil {
				return c, "not an integer: " + v, false
			}
			switch a {
			case "--producers":
				c.producers, hasP = n, true
			case "--consumers":
				c.consumers, hasC = n, true
			case "--items":
				c.items, hasN = n, true
			case "--cap":
				c.cap = n
			}
		case "--seed":
			v, ok := need()
			if !ok {
				return c, a + " needs a value", false
			}
			n, err := parseSeed(v)
			if err != nil {
				return c, "not an integer: " + v, false
			}
			c.seed = n
		default:
			return c, "unknown flag: " + a, false
		}
	}
	if !hasP || !hasC || !hasN {
		return c, "", false
	}
	if c.producers < 1 || c.consumers < 1 {
		return c, "--producers and --consumers must be >= 1", false
	}
	if c.items < 0 {
		return c, "--items must be >= 0", false
	}
	if c.cap < 1 {
		return c, "--cap must be >= 1", false
	}
	return c, "", true
}

func run(c config) {
	queue := make(chan uint64, c.cap) // buffered channel == bounded blocking queue
	var produced int64

	// Correct path: one accumulator slot per consumer, no shared writes.
	localCounts := make([]int, c.consumers)
	localSums := make([]uint64, c.consumers)

	// Buggy path: a single shared counter + checksum, mutated with no lock.
	var sharedConsumed int
	var sharedChecksum uint64

	start := time.Now()

	var producers sync.WaitGroup
	for p := 0; p < c.producers; p++ {
		producers.Add(1)
		go func(p int) {
			defer producers.Done()
			for i := p; i < c.items; i += c.producers {
				queue <- payload(c.seed, uint64(i))
				atomic.AddInt64(&produced, 1)
			}
		}(p)
	}
	// Closer: end the consumers' range once every item has been pushed.
	go func() {
		producers.Wait()
		close(queue)
	}()

	var consumers sync.WaitGroup
	for idx := 0; idx < c.consumers; idx++ {
		consumers.Add(1)
		go func(idx int) {
			defer consumers.Done()
			for v := range queue {
				if c.buggy {
					sharedConsumed++    // DATA RACE: unsynchronized shared counter
					sharedChecksum ^= v // DATA RACE: unsynchronized shared checksum
				} else {
					localCounts[idx]++
					localSums[idx] ^= v
				}
			}
		}(idx)
	}
	consumers.Wait()

	ms := time.Since(start).Milliseconds()

	consumed := sharedConsumed
	checksum := sharedChecksum
	if !c.buggy {
		consumed = 0
		checksum = 0
		for i := 0; i < c.consumers; i++ {
			consumed += localCounts[i]
			checksum ^= localSums[i]
		}
	}

	fmt.Printf("workq: produced=%d consumed=%d checksum=%016x ms=%d\n",
		atomic.LoadInt64(&produced), consumed, checksum, ms)
}

func main() {
	c, msg, ok := parseArgs(os.Args[1:])
	if !ok {
		if msg != "" {
			fmt.Fprintf(os.Stderr, "workq: %s\n", msg)
		}
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}
	run(c)
}
