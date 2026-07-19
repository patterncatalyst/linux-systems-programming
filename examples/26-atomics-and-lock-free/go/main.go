// spscring: a single-producer/single-consumer lock-free ring buffer.
//
//	spscring --capacity K --items N [--pad on|off]
//
// A producer goroutine pushes N u64 values (0..N-1) through a bounded ring of
// K slots; the consumer (main goroutine) pops and sums them. head and tail are
// atomic.Uint64 monotonic counters (Lamport's SPSC queue) — no mutex.
//
// What Go's sync/atomic gives you: unlike C++ or Rust there is NO per-operation
// memory-order argument. Every Load/Store/Add is sequentially consistent, and
// the Go memory model promises that an atomic write is "synchronized before"
// the atomic read that observes it — that happens-before edge is exactly the
// acquire/release pairing we need, just without the ability to relax it. So the
// plain buf[] writes before tail.Store are visible to the consumer that reads
// tail, and the -race detector (see demo.sh) understands that edge and stays
// quiet on this deliberately lock-free code.
//
// --pad puts head and tail on separate cache lines to remove the false sharing
// that otherwise ping-pongs one line between the two cores.
//
// Prints exactly one line:
//
//	spscring: items=<N> sum=<s> throughput_mops=<m> pad=<on|off>
package main

import (
	"fmt"
	"os"
	"strconv"
	"sync/atomic"
	"time"
)

const usage = "usage: spscring --capacity K --items N [--pad on|off]"

type args struct {
	capacity uint64
	items    uint64
	pad      bool
}

func parseArgs(argv []string) (args, bool) {
	var a args
	haveCap, haveItems := false, false
	for i := 0; i < len(argv); i++ {
		next := func() (string, bool) {
			if i+1 >= len(argv) {
				return "", false
			}
			i++
			return argv[i], true
		}
		switch argv[i] {
		case "--capacity":
			v, ok := next()
			if !ok {
				return a, false
			}
			n, err := strconv.ParseUint(v, 10, 64)
			if err != nil || n == 0 {
				return a, false
			}
			a.capacity, haveCap = n, true
		case "--items":
			v, ok := next()
			if !ok {
				return a, false
			}
			n, err := strconv.ParseUint(v, 10, 64)
			if err != nil {
				return a, false
			}
			a.items, haveItems = n, true
		case "--pad":
			v, ok := next()
			if !ok {
				return a, false
			}
			switch v {
			case "on":
				a.pad = true
			case "off":
				a.pad = false
			default:
				return a, false
			}
		default:
			return a, false
		}
	}
	if !haveCap || !haveItems {
		return a, false
	}
	return a, true
}

// Two control-block layouts. ctrlPadded forces head and tail onto separate
// 64-byte cache lines; ctrlPacked leaves them adjacent (false sharing).
type ctrlPadded struct {
	head atomic.Uint64
	_    [56]byte // pad head out to a full cache line
	tail atomic.Uint64
	_    [56]byte
}

type ctrlPacked struct {
	head atomic.Uint64
	tail atomic.Uint64
}

func bench(capacity, items uint64, pad bool) (sum uint64, mops float64) {
	buf := make([]uint64, capacity)

	// Grab *atomic.Uint64 pointers into whichever layout we chose; the struct
	// stays alive because these pointers reference it.
	var head, tail *atomic.Uint64
	if pad {
		c := &ctrlPadded{}
		head, tail = &c.head, &c.tail
	} else {
		c := &ctrlPacked{}
		head, tail = &c.head, &c.tail
	}

	start := time.Now()

	done := make(chan struct{})
	go func() { // producer
		var t uint64
		for i := uint64(0); i < items; i++ {
			for t-head.Load() == capacity { // ring full: spin
			}
			buf[t%capacity] = i
			t++
			tail.Store(t)
		}
		close(done)
	}()

	// consumer (main goroutine)
	var h uint64
	for i := uint64(0); i < items; i++ {
		for h == tail.Load() { // ring empty: spin
		}
		sum += buf[h%capacity]
		h++
		head.Store(h)
	}

	<-done
	secs := time.Since(start).Seconds()
	if secs < 1e-9 {
		secs = 1e-9
	}
	mops = float64(items) / secs / 1e6
	return sum, mops
}

func main() {
	a, ok := parseArgs(os.Args[1:])
	if !ok {
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}

	sum, mops := bench(a.capacity, a.items, a.pad)

	padStr := "off"
	if a.pad {
		padStr = "on"
	}
	fmt.Printf("spscring: items=%d sum=%d throughput_mops=%.2f pad=%s\n",
		a.items, sum, mops, padStr)
}
