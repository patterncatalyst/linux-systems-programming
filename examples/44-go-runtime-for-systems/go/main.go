// go-runtime-for-systems: a program that observes its own Go runtime across
// four themes -- scheduler (GMP), GC pacing, netpoller/syscalls, and effective
// knobs -- printing self-computed verdict lines so the behavior is checkable,
// not just describable. Every "<label> ok"/"<label> FAIL" line is a fixed
// string the program always prints; only the numeric fields and the trailing
// verdict word vary by run. Stdlib only.
package main

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
)

// ---- shared helpers -------------------------------------------------------

// verdict renders the fixed "ok"/"FAIL" token from a boolean.
func verdict(ok bool) string {
	if ok {
		return "ok"
	}
	return "FAIL"
}

// pollNumGoroutine polls runtime.NumGoroutine(), yielding the current
// goroutine via runtime.Gosched() between samples, until cond is satisfied or
// maxTries is exhausted. It never sleeps or waits on a wall clock -- only
// cooperative scheduler yields -- so it is bounded in iteration count, not in
// time. If cond is never satisfied, the last-observed value is returned and
// the caller's own comparison correctly reports FAIL.
func pollNumGoroutine(cond func(int) bool, maxTries int) int {
	n := runtime.NumGoroutine()
	for i := 0; i < maxTries && !cond(n); i++ {
		runtime.Gosched()
		n = runtime.NumGoroutine()
	}
	return n
}

// readThreadCount reads this process's own OS thread count from
// /proc/self/status's "Threads:" field. Self-observation only -- no external
// tool is shelled out to.
func readThreadCount() int {
	f, err := os.Open("/proc/self/status")
	if err != nil {
		return -1
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := scanner.Text()
		if !strings.HasPrefix(line, "Threads:") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return -1
		}
		v, err := strconv.Atoi(fields[1])
		if err != nil {
			return -1
		}
		return v
	}
	return -1
}

func envOrUnset(key string) string {
	v := os.Getenv(key)
	if v == "" {
		return "unset"
	}
	return v
}

// ---- theme 1: sched --------------------------------------------------------

func runSched() bool {
	fmt.Println("=== sched ===")
	ok := true

	gomaxprocs := runtime.GOMAXPROCS(0)
	numcpu := runtime.NumCPU()
	fmt.Printf("sched: env gomaxprocs=%d numcpu=%d\n", gomaxprocs, numcpu)

	baseline := runtime.NumGoroutine()
	fmt.Printf("sched: baseline goroutines=%d\n", baseline)

	// --- rendezvous: N goroutines concurrently alive under the scheduler ---
	const workers = 8
	arrived := make(chan struct{}, workers)
	release := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(workers)
	for i := 0; i < workers; i++ {
		go func() {
			defer wg.Done()
			arrived <- struct{}{}
			<-release
		}()
	}
	for i := 0; i < workers; i++ {
		<-arrived
	}
	// At this instant main + 8 blocked workers are guaranteed alive.
	during := runtime.NumGoroutine()
	rendOk := during >= baseline+workers
	fmt.Printf("sched: rendezvous workers=%d baseline=%d during=%d during>=baseline+workers %s\n",
		workers, baseline, during, verdict(rendOk))
	ok = ok && rendOk

	close(release)
	wg.Wait()
	after := pollNumGoroutine(func(n int) bool { return n <= baseline+2 }, 500000)
	drainOk := after <= baseline+2
	fmt.Printf("sched: rendezvous drained baseline=%d after=%d after<=baseline+2 %s\n",
		baseline, after, verdict(drainOk))
	ok = ok && drainOk

	// --- workpool: work distribution, every job processed exactly once ---
	const poolWorkers = 8
	const jobs = 64
	jobCh := make(chan int, jobs)
	var done int64
	var wg2 sync.WaitGroup
	wg2.Add(poolWorkers)
	for w := 0; w < poolWorkers; w++ {
		go func() {
			defer wg2.Done()
			for range jobCh {
				atomic.AddInt64(&done, 1)
			}
		}()
	}
	for j := 0; j < jobs; j++ {
		jobCh <- j
	}
	close(jobCh)
	wg2.Wait()
	doneN := atomic.LoadInt64(&done)
	poolOk := doneN == jobs
	fmt.Printf("sched: workpool workers=%d jobs=%d done=%d done==jobs %s\n",
		poolWorkers, jobs, doneN, verdict(poolOk))
	ok = ok && poolOk

	// --- preempt: async (signal-based) preemption under GOMAXPROCS=1 ---
	const spinnerOps = 2_000_000_000
	origProcs := runtime.GOMAXPROCS(1)

	var spinDone int32
	var spinSink uint64
	var tick int64

	// The ticker goroutine is launched first and the spinner second so that
	// the spinner -- the most recently spawned goroutine -- claims the P's
	// single-slot "runnext" fast path and is what the scheduler runs first
	// when main below yields. This keeps the outcome deterministic instead
	// of depending on which of two equally non-yielding goroutines happens
	// to win an unspecified scheduling race.
	go func() {
		for atomic.LoadInt32(&spinDone) == 0 {
			atomic.AddInt64(&tick, 1)
		}
	}()

	go func() {
		// Pure integer arithmetic: no function calls, no channel ops --
		// nothing that is a cooperative yield point. Only Go's async
		// (signal-based) preemption can interrupt this goroutine.
		var x uint64 = 0x9E3779B97F4A7C15
		for i := int64(0); i < spinnerOps; i++ {
			x = x*6364136223846793005 + 1
		}
		atomic.StoreUint64(&spinSink, x)
		atomic.StoreInt32(&spinDone, 1)
	}()

	for atomic.LoadInt32(&spinDone) == 0 {
		runtime.Gosched()
	}
	tickVal := atomic.LoadInt64(&tick)
	runtime.GOMAXPROCS(origProcs)

	preemptOk := tickVal > 0
	fmt.Printf("sched: preempt forced_gomaxprocs=1 spinner_ops=%d ticker_incr=%d ticker_incr>0 %s\n",
		spinnerOps, tickVal, verdict(preemptOk))
	ok = ok && preemptOk

	if ok {
		fmt.Println("sched: PASS")
	} else {
		fmt.Println("sched: FAIL")
	}
	return ok
}

// ---- theme 2: gc ------------------------------------------------------------

// gcHeldLive keeps the forced-GC allocation batch reachable (a real, held
// allocation, not dead-code-eliminated).
var gcHeldLive [][]byte

// churnBlackhole and churnByteSink are package-level so the churn workload's
// allocations are guaranteed to escape to the heap and become real garbage
// each round, rather than being optimized into a reused stack slot.
var churnBlackhole []byte
var churnByteSink byte

func churn() {
	const rounds = 2000
	const chunkBytes = 32 * 1024
	for i := 0; i < rounds; i++ {
		churnBlackhole = make([]byte, chunkBytes)
		churnByteSink ^= churnBlackhole[0]
	}
}

func measureCycles(pct int) int {
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	prev := debug.SetGCPercent(pct)
	churn()
	runtime.ReadMemStats(&after)
	debug.SetGCPercent(prev)
	return int(after.NumGC - before.NumGC)
}

func measureCyclesMemLimit(limit int64) int {
	runtime.GC()
	prevPct := debug.SetGCPercent(800) // held fixed and loose in both runs
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	prevLimit := debug.SetMemoryLimit(limit)
	churn()
	runtime.ReadMemStats(&after)
	debug.SetMemoryLimit(prevLimit)
	debug.SetGCPercent(prevPct)
	return int(after.NumGC - before.NumGC)
}

func runGC() bool {
	fmt.Println("=== gc ===")
	ok := true

	// Disable automatic GC for the whole theme so every NumGC delta below
	// is caused only by explicit calls this program makes.
	prevPercent := debug.SetGCPercent(-1)
	defer debug.SetGCPercent(prevPercent)

	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	baselineNumGC := ms.NumGC
	percentStr := "off"
	if prevPercent != -1 {
		percentStr = strconv.Itoa(prevPercent)
	}
	fmt.Printf("gc: baseline gcpercent=%s numgc=%d\n", percentStr, baselineNumGC)

	// --- forced delta: exactly one runtime.GC() call, exact NumGC delta ---
	const allocObjs = 100000
	const objBytes = 640
	const allocBytes = allocObjs * objBytes
	gcHeldLive = make([][]byte, 0, allocObjs)
	for i := 0; i < allocObjs; i++ {
		gcHeldLive = append(gcHeldLive, make([]byte, objBytes))
	}
	fmt.Printf("gc: alloc bytes=%d objects=%d auto_gc=disabled\n", allocBytes, allocObjs)

	runtime.ReadMemStats(&ms)
	numGCBefore := ms.NumGC
	runtime.GC()
	runtime.ReadMemStats(&ms)
	numGCAfter := ms.NumGC
	delta := numGCAfter - numGCBefore
	forcedOk := delta == 1
	fmt.Printf("gc: forced numgc_before=%d numgc_after=%d delta=%d delta==1 %s\n",
		numGCBefore, numGCAfter, delta, verdict(forcedOk))
	ok = ok && forcedOk

	// Release the held-live batch now that the forced-delta check is done.
	gcHeldLive = nil

	// --- GOGC effect: looser percent -> fewer cycles for the same churn ---
	gogc100 := measureCycles(100)
	gogc800 := measureCycles(800)
	gogcOk := gogc800 < gogc100
	fmt.Printf("gc: gogc100_cycles=%d gogc800_cycles=%d gogc800<gogc100 %s\n",
		gogc100, gogc800, verdict(gogcOk))
	ok = ok && gogcOk

	// --- GOMEMLIMIT effect: tight soft limit -> more cycles, GOGC held at 800 ---
	memUnset := measureCyclesMemLimit(math.MaxInt64)
	memTight := measureCyclesMemLimit(8 << 20)
	memOk := memTight > memUnset
	fmt.Printf("gc: memlimit_unset_cycles=%d memlimit_tight_cycles=%d memlimit_tight>memlimit_unset %s\n",
		memUnset, memTight, verdict(memOk))
	ok = ok && memOk

	if ok {
		fmt.Println("gc: PASS")
	} else {
		fmt.Println("gc: FAIL")
	}
	return ok
}

// ---- theme 3: netpoll -------------------------------------------------------

func runNetpoll() bool {
	fmt.Println("=== netpoll ===")
	ok := true

	baselineG := runtime.NumGoroutine()
	baselineT := readThreadCount()
	fmt.Printf("netpoll: baseline goroutines=%d threads=%d\n", baselineG, baselineT)

	const numPipes = 2000
	readers := make([]*os.File, numPipes)
	writers := make([]*os.File, numPipes)
	for i := 0; i < numPipes; i++ {
		r, w, err := os.Pipe()
		if err != nil {
			fmt.Fprintln(os.Stderr, "netpoll: os.Pipe error:", err)
			os.Exit(1)
		}
		readers[i] = r
		writers[i] = w
	}

	var wg sync.WaitGroup
	wg.Add(numPipes)
	for i := 0; i < numPipes; i++ {
		r := readers[i]
		go func() {
			defer wg.Done()
			buf := make([]byte, 1)
			r.Read(buf)
		}()
	}

	total := pollNumGoroutine(func(n int) bool { return n >= baselineG+numPipes }, 500000)
	totalOk := total == baselineG+numPipes
	fmt.Printf("netpoll: parked goroutines=%d baseline=%d total=%d total==baseline+2000 %s\n",
		numPipes, baselineG, total, verdict(totalOk))
	ok = ok && totalOk

	// Bounded settle: let the runtime finish registering the parked fds with
	// the netpoller before sampling the OS thread count.
	for i := 0; i < 1000; i++ {
		runtime.Gosched()
	}
	threads := readThreadCount()
	bound := 4*runtime.GOMAXPROCS(0) + 16
	threadsOk := threads <= bound
	fmt.Printf("netpoll: parked threads=%d bound=%d threads<=bound %s\n",
		threads, bound, verdict(threadsOk))
	ok = ok && threadsOk

	for i := 0; i < numPipes; i++ {
		writers[i].Write([]byte{1})
	}
	wg.Wait()
	for i := 0; i < numPipes; i++ {
		readers[i].Close()
		writers[i].Close()
	}

	after := pollNumGoroutine(func(n int) bool { return n <= baselineG+2 }, 500000)
	drainOk := after <= baselineG+2
	fmt.Printf("netpoll: drained baseline=%d after=%d after<=baseline+2 %s\n",
		baselineG, after, verdict(drainOk))
	ok = ok && drainOk

	if ok {
		fmt.Println("netpoll: PASS")
	} else {
		fmt.Println("netpoll: FAIL")
	}
	return ok
}

// ---- theme 4: knobs ---------------------------------------------------------

func runKnobs() bool {
	fmt.Println("=== knobs ===")

	fmt.Printf("knobs: GOMAXPROCS=%d NumCPU=%d\n", runtime.GOMAXPROCS(0), runtime.NumCPU())
	fmt.Printf("knobs: GOGC_env=%s\n", envOrUnset("GOGC"))
	fmt.Printf("knobs: GOMEMLIMIT_env=%s\n", envOrUnset("GOMEMLIMIT"))
	fmt.Printf("knobs: GODEBUG_env=%s\n", envOrUnset("GODEBUG"))

	// SetMemoryLimit with a negative input is a genuine non-mutating query.
	limit := debug.SetMemoryLimit(-1)
	limitStr := "unset"
	if limit != math.MaxInt64 {
		limitStr = strconv.FormatInt(limit, 10)
	}
	fmt.Printf("knobs: GOMEMLIMIT_effective_bytes=%s\n", limitStr)

	fmt.Println("knobs: PASS")
	return true
}

// ---- entry point -------------------------------------------------------------

func main() {
	cmd := "all"
	if len(os.Args) > 1 {
		cmd = os.Args[1]
	}

	var ok bool
	switch cmd {
	case "sched":
		ok = runSched()
	case "gc":
		ok = runGC()
	case "netpoll":
		ok = runNetpoll()
	case "knobs":
		ok = runKnobs()
	case "all":
		okS := runSched()
		okG := runGC()
		okN := runNetpoll()
		okK := runKnobs()
		ok = okS && okG && okN && okK
		if ok {
			fmt.Println("ALL: PASS")
		} else {
			fmt.Println("ALL: FAIL")
		}
		if !ok {
			os.Exit(1)
		}
		return
	default:
		fmt.Fprintln(os.Stderr, "usage: app [sched|gc|netpoll|knobs|all]")
		os.Exit(2)
	}
	if !ok {
		os.Exit(1)
	}
}
