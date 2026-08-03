package main

import "testing"

// hotLoop is a deliberately dominant hot function: pure integer arithmetic
// across a large fixed iteration count, with no function calls inside the
// loop for the profiler to attribute self-time to instead. It exists so
// `go tool pprof -top` reliably names "hotLoop" in the top few frames of a
// CPU profile taken from BenchmarkHotLoop, independent of host noise or
// exact timings -- the pprof gate (verify.lua gate E) asserts the NAME, not
// a duration.
func hotLoop(n int) uint64 {
	var x uint64 = 0x9e3779b97f4a7c15
	for i := 0; i < n; i++ {
		x ^= uint64(i)
		x *= 6364136223846793005
		x = x<<13 | x>>51
	}
	return x
}

// hotLoopSink defeats dead-code elimination of hotLoop's result.
var hotLoopSink uint64

const hotLoopIters = 2_000_000

func BenchmarkHotLoop(b *testing.B) {
	for i := 0; i < b.N; i++ {
		hotLoopSink = hotLoop(hotLoopIters)
	}
}

// BenchmarkHotLoopParallel runs hotLoop from b.RunParallel, giving the
// concurrency-lens section (and a benchstat -cpu=1 vs -cpu=N comparison,
// see the chapter) something GOMAXPROCS-sensitive to compare against the
// serial BenchmarkHotLoop above.
func BenchmarkHotLoopParallel(b *testing.B) {
	b.RunParallel(func(pb *testing.PB) {
		var local uint64
		for pb.Next() {
			local = hotLoop(hotLoopIters)
		}
		hotLoopSink = local
	})
}
