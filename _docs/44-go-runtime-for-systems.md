---
title: "The Go runtime from a systems programmer's seat: the scheduler, the collector, and the netpoller"
order: 44
part: "Deep Dives"
description: "go-runtime-for-systems is a single Go binary that observes its own runtime across four themes -- the GMP scheduler and signal-based async preemption, GC pacing under GOGC/GOMEMLIMIT with Go 1.26's Green Tea collector on by default, the epoll-backed netpoller that turns 2,000 blocked goroutines into roughly a dozen OS threads, and the effective knobs a process is actually running under -- each theme printing a self-computed ok/FAIL verdict. Verified on the Fedora 44 host on go1.26.5 with verify.lua PASS 16/FAIL 0, and the same preemption check flipped to FAIL on demand by GODEBUG=asyncpreemptoff=1."
duration: "45 minutes"
---

Chapter 43 spent its whole budget on Rust's compiler doing extra work at
build time -- a proc-macro crate that runs once, produces tokens, and gets
out of the way. This chapter is the mirror image: a runtime that never gets
out of the way, because it is doing something every second the program
runs. Every earlier chapter that used Go leaned on its runtime without
looking at it -- goroutines that seemed to run in parallel, garbage that
seemed to collect itself, sockets that seemed to block without blocking
anything. `go-runtime-for-systems` is the chapter where that runtime stops
being invisible: a single stdlib-only binary that asks its own scheduler,
collector, and netpoller direct questions and prints the answers as
computed verdicts -- `ok` or `FAIL`, never just an exit code. It's also the
last chapter of Part 12: the Rust-only dive into macros and `miri`, and
this Go-only look under the runtime's hood, are the two language-specific
excursions the tri-language chapters made room for.

{% include excalidraw.html
   file="44-gmp-scheduler"
   alt="A global run queue band at top holds overflow G's. Below, two P columns (P0.M0 running a spinner G, P1.M1 running a ticker G) each show a local run queue (256 slots + runnext), plus a box for P2 through P15 on this 16-CPU host. A dashed amber steal half arrow runs P1 into P0. To the right, a dashed sysmon box (no P, its own M) lists retake, forcegc, netpoll backstop, and the Go 1.25+ GOMAXPROCS recheck, with a solid amber SIGURG to asyncPreempt arrow into P0's running M. Caption: M:N -- many G onto few M via P; GOMAXPROCS=1 still preempts since sysmon's signal needs no spare P."
   caption="Figure 44.1 — the GMP model: G's queued locally (256 slots + runnext) and globally, GOMAXPROCS P's bound to M's, work-stealing between them, and sysmon's SIGURG reaching in from outside the P pool" %}

> **Tools used** — `go build` / `go run` (host, the `go 1.26.x` check in
> `scripts/check-host.sh`). `GODEBUG` is a runtime environment variable the Go
> binary itself reads at startup, not a separate tool this chapter installs
> or shells out to; `/proc/self/status` is opened and parsed by the program's
> own `readThreadCount` helper (below), not scraped by a host command. No VM,
> no root, no LGTM — `examples/44-go-runtime-for-systems` is `mode: local` in
> `examples/manifest.yaml`.

## The GMP model: many goroutines, few threads

Go's scheduler has three kinds of object, and the names are the whole
mental model. A **G** is a goroutine: a small growable stack, an
instruction pointer, and a status — cheap enough that `sched` spawns
thousands without a second thought. An **M** is a machine: an actual OS
thread, the thing `clone(2)` creates. A **P** is a processor: a
scheduling context, exactly `GOMAXPROCS` of them, and the resource token
that makes this an M:N scheduler — an M cannot run Go code without
holding a P. Each P owns a local run queue (a 256-slot ring buffer plus a
single-slot `runnext` fast path); a global run queue is the overflow and
injection target every P eventually checks. When a P runs dry, it
steals — `findRunnable`/`stealWork` picks a random other P and takes
**half** its local queue, not one goroutine at a time.

None of that explains how a tight loop with no yield point ever gets
interrupted. Before Go 1.14 it didn't: a bare `for {}` could wedge the
runtime indefinitely (golang/go#10958). The fix is `sigPreempt =
_SIGURG` — chosen because no real program uses `SIGURG` for anything
else. `preemptone()` sets a cooperative flag first, then, if async
preemption is enabled, calls `preemptM`, sending `SIGURG` to the specific
thread running the goroutine; the signal handler redirects into
`asyncPreempt`, which saves registers, parks the goroutine at a safe
point, and lets the scheduler run something else. `sched` proves this
exists: with `runtime.GOMAXPROCS(1)` forced, one goroutine spins
2,000,000,000 iterations of pure integer arithmetic — comfortably past
the runtime's 10ms forced-preemption window — while a second does
nothing but increment a counter in its own tight loop. On a purely
cooperative scheduler the second goroutine would never run at all under
one P; this run's `ticker_incr=388757095` says it ran the whole spin.

```go
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
```

The goroutine that spins is launched *second*, deliberately: the most
recently spawned goroutine claims `runnext`, so it — not the ticker — is
guaranteed to run first when `main` yields below. Behind this sits
**sysmon**, a dedicated M with no P of its own, looping with a
20µs-to-10ms doubling backoff. Each tick it can retake a P stuck in a
syscall or a goroutine past `forcePreemptNS` (10ms), force a GC if none
has run in two minutes, trigger the SIGURG preemption above, poll the
network as a backstop if nobody has in over 10ms, and — new since Go
1.25 — re-evaluate `GOMAXPROCS` at most once a second. That last item is
the other headline scheduler change here: Go 1.25 made the default
`GOMAXPROCS` **container-aware**, reading cgroup v2 `cpu.max` (or v1's
quota/period pair) and taking the minimum of logical CPU count, affinity
mask, and cgroup throughput limit — floor of 2, fractional limits round
up. `GODEBUG=containermaxprocs=0` reverts to the old, `NumCPU()`-only
behavior; Go 1.26 adds only observability metrics on top, no algorithm
change. On this 16-CPU host with nothing limiting this process, `knobs:
GOMAXPROCS=16 NumCPU=16` — nothing narrows either.

## The pacer's bargain: GOGC, GOMEMLIMIT, and Green Tea

Go's collector is concurrent, tri-color, mark-and-sweep, and
non-generational: marking runs alongside the mutator, bounded by two
short stop-the-world phases (sweep and mark termination, both normally
sub-millisecond), with a hybrid write barrier keeping the tri-color
invariant intact. The **pacer** decides when to start the next cycle, and
`GOGC` is the dial: target heap size is `Live + (Live + roots) * GOGC /
100`, so `GOGC=100` (the default) roughly doubles the live heap before
the next collection. Loosen `GOGC` and the pacer waits longer — double
the memory overhead, roughly halve the GC CPU cost. `GOGC=off`
(`debug.SetGCPercent(-1)`) removes the percentage trigger entirely,
exactly what `gc` does for its whole run, so every `NumGC` delta below is
caused only by an explicit call this program makes.

```go
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
```

`measureCycles` isolates the `GOGC` effect: normalize with one
`runtime.GC()`, set a percent, run a fixed 64 MiB churn workload, read the
`NumGC` delta. Run at 100 and at 800 against identical churn, and this
session shows the textbook shape: `gogc100_cycles=21` against
`gogc800_cycles=2` — the looser percent needs a tenth as many collections
for the same garbage. `measureCyclesMemLimit` isolates a second knob,
`GOMEMLIMIT` (`debug.SetMemoryLimit`, since Go 1.19): a **soft** ceiling
on runtime-managed memory, respected *even with `GOGC=off`* — why "GOGC
off, GOMEMLIMIT set" is coherent rather than contradictory: don't collect
on a heap-growth ratio at all, only when about to breach an absolute
ceiling. The docs call it deliberately soft ("no guarantees... only some
reasonable amount of effort"), since a hard limit risks the GC thrashing
to stay under it. Holding `GOGC=800` fixed and varying only the limit —
unset (`math.MaxInt64`) against a tight 8 MiB — this session measured
`memlimit_unset_cycles=2` against `memlimit_tight_cycles=122`: the tight
limit forces sixty times as many collections as `GOGC` alone would ever
trigger.

Go 1.26 changes what's doing the marking, without touching any of the
above. **Green Tea**, opt-in in Go 1.25 (`GOEXPERIMENT=greenteagc`), is
enabled by default in 1.26 — the release notes: "now enabled by default
after incorporating feedback." Instead of chasing individual object
pointers, it organizes marking around 8 KiB pages, scanning in memory
order with per-page bitmaps — cache-friendly, and on Ice-Lake/Zen-4-class
CPUs it also uses vector instructions to scan whole pages in a handful of
cycles. The measured effect, per Google's benchmarks, is 10-40% less GC
CPU overhead on GC-heavy programs (if GC was 10% of total CPU, a 1-4%
total-CPU win, not 10-40%). An opt-out, `GOEXPERIMENT=nogreenteagc`,
still exists in 1.26 but is slated for removal in 1.27. None of this
shows in `gctrace=1`'s line format, which is unchanged; Green Tea is a
build-time signal only, visible via `go version -m <binary>` or
`debug.ReadBuildInfo()`, never in a trace line.

## The netpoller: why 2,000 goroutines are not 2,000 threads

A classic blocking syscall and a network read look identical from inside
a goroutine — `conn.Read(buf)` either way — but they take different
paths through the runtime, which is why Go can run thousands of
concurrently-blocked goroutines on a double-digit thread count. A real
blocking read calls `entersyscall`, transitioning the goroutine to
`_Gsyscall` and optimistically **keeping its P**, betting the syscall
returns fast; if it doesn't, sysmon's `retake` notices — within ~20µs if
other work is waiting, otherwise within `forcePreemptNS` (10ms)
regardless — steals the P away, and `handoffp` wakes a parked M or, if
none is idle, `clone(2)`s a new one. Block N goroutines this way and the
thread count tracks N.

A network or pipe read on a non-blocking fd exits differently.
`internal/poll`'s read loop calls the raw syscall once; on `EAGAIN` it
calls `pollDesc.waitRead` → `netpollblock` → `gopark`, dropping the
goroutine into `_Gwaiting`, detached **with no P or M held at all** — its
identity is stored as a bare pointer inside the fd's `pollDesc`, and the M
that was running it is immediately free to find other work. The wakeup
side is one process-wide `epoll` instance (`epoll_create1` plus an
`eventfd` for the runtime's own wake-itself-up calls), each fd armed
edge-triggered. The wrapper is named `EpollWait` but the syscall it
issues is `epoll_pwait` — what an `strace -f` on any Go binary shows,
never `epoll_wait`. `findRunnable` polls this netpoller opportunistically
before resorting to work-stealing, and blockingly when there's nothing
else to do — an M with no work drops its P and calls `netpoll(delay)`,
temporarily *becoming* the netpoller thread.

{% include excalidraw.html
   file="44-goroutines-not-threads"
   alt="Top band: 2000 goroutines blocked on os.Pipe reads feed one shared netpoller (epoll_pwait, per-fd pollDesc) labeled parked, no M; the OS-thread box reads threads=12, bound=80. Bottom band, the contrast: N goroutines calling syscall.Read on a raw, never-epoll-registered fd each block one M in read(2); the thread box shows the count climbing toward N via sysmon retake, handoffp, startm/newm. Caption: goroutines != threads -- 2000 parked, 12 threads, bound 80."
   caption="Figure 44.2 — the netpoller contrast: 2,000 goroutines parked in one shared epoll with no M held, against a raw blocking read that pins one M per blocked goroutine" %}

The `netpoll` theme measures exactly this: 2,000 `os.Pipe()` pairs, one
goroutine per pipe blocked on a `Read` of the unwritten end.

```go
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
```

Thread count is read from `/proc/self/status`'s own `Threads:` field — an
OS-level cross-check independent of anything the Go runtime claims about
itself:

```go
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
```

This session: `netpoll: baseline goroutines=1 threads=12` before the
fan-out, then `netpoll: parked goroutines=2000 baseline=1 total=2001` and
`netpoll: parked threads=12 bound=80 threads<=bound ok` after it — 2,000
more live goroutines, *zero* additional threads, well inside a bound
(`4*GOMAXPROCS+16 = 80`) set orders of magnitude below 2,000 so it only
fails if something regresses to one-thread-per-blocked-goroutine. Go 1.26
also removes something from this picture: a dedicated `_Psyscall` P
status. A P whose M was in a syscall used to carry that status directly;
as of 1.26, `pp.status` only ever takes
`_Pidle`/`_Prunning`/`_Pgcstop`/`_Pdead`, and whether a P's M is really in
a syscall is read off the **goroutine's** status instead
(`setBlockOnExitSyscall`) — the release notes attribute a ~30% cut in
baseline cgo-call overhead to this, a narrower change than the still-open
proposal for a second dedicated syscall thread per M (golang/go#58492).

## How the code works

Every theme follows the same shape: compute something, print a
fixed-text comparison label with the measured numbers substituted in, and
end the line with a verdict word the program itself decides —
`fmt.Printf("... %s\n", verdict(cond))`, where `verdict` renders `"ok"`
or `"FAIL"` from a boolean. `verify.lua` matches the label text plus the
literal `ok`; if the condition flips, `FAIL` prints instead and the
pattern simply stops matching. The check is the program's own
arithmetic, not the test's, and three techniques above make that
arithmetic trustworthy rather than merely plausible:
`debug.SetGCPercent(-1)` turns "allocate then measure" into an exact
integer comparison, `readThreadCount` cross-checks against the kernel
rather than the Go runtime's own bookkeeping, and the ticker-before-spinner
launch order removes the one unspecified scheduling race that would
otherwise exist. None of the four themes uses `time.Sleep`; where a
bounded settle is needed, `pollNumGoroutine` loops on `runtime.Gosched()`
up to a fixed retry cap, polling cooperatively rather than the wall
clock.

## Errors, three ways

With only one language, "three ways" means three moments an error
surfaces, not three languages producing the same one. **A computed
assertion**: every `<label> FAIL` line *is* an error — not a panic, not a
bare non-zero exit, but the program's own boolean, printed where a human
or `verify.lua` can see exactly which comparison failed. **A wrapped
runtime error**: `readThreadCount` returns `-1` on any failure along its
path — the `/proc/self/status` open failing, the `Threads:` line
missing, `strconv.Atoi` choking on a malformed field — rather than
panicking, because a self-observation helper failing to observe itself
shouldn't crash the process it's describing; `runNetpoll`'s `os.Pipe()`
call, by contrast, is a genuine unrecoverable setup failure and calls
`os.Exit(1)`, since 2,000 goroutines with nothing to block on isn't
worth continuing. **An engineered failure**: run the same preemption
probe with `GODEBUG=asyncpreemptoff=1` and the check that reads
`ticker_incr>0 ok` every normal run instead prints `ticker_incr>0 FAIL` —
signal-based preemption is what let the ticker progress, and disabling
it reproduces the pre-1.14 world where a non-yielding loop starves
everything else on its P.

That contrast run almost didn't terminate while this example was being
built. With async preemption disabled, the only way the ticker gets to
run is if the spinner voluntarily yields its P — which it never does by
design. Launch the spinner first and the ticker second under
`asyncpreemptoff=1`, and the ticker claims `runnext`, starts, and blocks
forever on a flag only the never-scheduled spinner can set: a genuine
livelock, not a slow test. The fix is the launch order in the excerpt
above — ticker first, spinner second — so the ticker observes `tick`
incrementing on a normal run, or never runs at all under
`asyncpreemptoff=1`, correctly reporting `ticker_incr=0`. The bug and
the fix teach the same lesson as SIGURG itself: a scheduler decision
that looks arbitrary usually isn't.

## Concurrency lens

This whole chapter is the concurrency lens, so the sharpest statement is
the one line every theme proves from a different angle: **M:N — many
goroutines multiplexed onto few OS threads, through P.** `sched`'s
rendezvous proves goroutines can be concurrently *alive* without true
hardware parallelism — eight workers blocked on a channel send are all
`_Gwaiting`, all real, all counted by `runtime.NumGoroutine()`. `netpoll`
proves the ratio directly: 2,000 goroutines parked with **no M at all** —
`gopark` detaches a blocked goroutine from its thread entirely, and the
epoll wakeup path hands it back a P only once runnable again. And the
preemption probe answers the question the M:N model raises on its own:
if a P is the resource that lets an M run Go code, how does anything
interrupt a goroutine refusing to yield on the *only* P
(`GOMAXPROCS=1`)? SIGURG doesn't need a spare P — it's a signal delivered
straight to the OS thread running the offending goroutine, dispatched by
sysmon, which holds no P of its own. Preemption and scheduling are two
different resources here: P's are contended, the signal that forces a
yield point is not.

## Build, run, observe

```console
[host]$ cd examples/44-go-runtime-for-systems && ./demo.sh go build
```

`./demo.sh go run all` runs all four themes in order:

```console
[host]$ ./demo.sh go run all
=== sched ===
sched: env gomaxprocs=16 numcpu=16
sched: baseline goroutines=1
sched: rendezvous workers=8 baseline=1 during=9 during>=baseline+workers ok
sched: rendezvous drained baseline=1 after=1 after<=baseline+2 ok
sched: workpool workers=8 jobs=64 done=64 done==jobs ok
sched: preempt forced_gomaxprocs=1 spinner_ops=2000000000 ticker_incr=388757095 ticker_incr>0 ok
sched: PASS
=== gc ===
gc: baseline gcpercent=100 numgc=0
gc: alloc bytes=64000000 objects=100000 auto_gc=disabled
gc: forced numgc_before=0 numgc_after=1 delta=1 delta==1 ok
gc: gogc100_cycles=21 gogc800_cycles=2 gogc800<gogc100 ok
gc: memlimit_unset_cycles=2 memlimit_tight_cycles=122 memlimit_tight>memlimit_unset ok
gc: PASS
=== netpoll ===
netpoll: baseline goroutines=1 threads=12
netpoll: parked goroutines=2000 baseline=1 total=2001 total==baseline+2000 ok
netpoll: parked threads=12 bound=80 threads<=bound ok
netpoll: drained baseline=1 after=1 after<=baseline+2 ok
netpoll: PASS
=== knobs ===
knobs: GOMAXPROCS=16 NumCPU=16
knobs: GOGC_env=unset
knobs: GOMEMLIMIT_env=unset
knobs: GODEBUG_env=unset
knobs: GOMEMLIMIT_effective_bytes=unset
knobs: PASS
ALL: PASS
```

`GODEBUG=gctrace=1` puts the runtime's own pacer on stderr, one line per
collection:

```console
[host]$ GODEBUG=gctrace=1 ./go/bin/app gc
gc 1 @0.004s 3%: 0.081+0.95+0.006 ms clock, 1.3+0/2.1/0.012+0.099 ms cpu, 67->67->63 MB, 8532210231531 MB goal, 0 MB stacks, 0 MB globals, 16 P (forced)
gc 2 @0.006s 2%: 0.011+0.10+0.003 ms clock, 0.18+0/0.14/0.024+0.060 ms cpu, 63->63->0 MB, 8532210231530 MB goal, 0 MB stacks, 0 MB globals, 16 P (forced)
gc 3 @0.011s 2%: 0.013+0.12+0.013 ms clock, 0.22+0.020/0.11/0.014+0.21 ms cpu, 3->4->0 MB, 4 MB goal, 0 MB stacks, 0 MB globals, 16 P
```
The format, verbatim from the runtime's doc comment: `gc # @#s #%: #+#+#
ms clock, #+#/#/#+# ms cpu, #->#-># MB, # MB goal, # MB stacks, # MB
globals, # P` — the three heap numbers are start/end/live size, the CPU
triplet breaks assist/background/idle time, and a trailing `(forced)`
marks a line from an explicit `runtime.GC()` call. Lines 1 and 2 above are
both `(forced)` and both print a `goal` field north of eight and a half
trillion megabytes. That's not a bug: with `SetGCPercent(-1)`, there's no
heap-growth ratio to compute a goal from, so the field is arithmetically
meaningless while GC is off. The moment a real percent is restored (line
3), the same field reports a sane `4 MB`.

`GODEBUG=schedtrace=1000,scheddetail=1` dumps the scheduler's own state
once a second, plus a full per-P/per-M/per-G snapshot at startup:

```console
[host]$ GODEBUG=schedtrace=1000,scheddetail=1 ./go/bin/app sched
SCHED 0ms: gomaxprocs=16 idleprocs=13 threads=5 spinningthreads=1 needspinning=0 idlethreads=1 runqueue=0 gcwaiting=false nmidlelocked=0 stopwait=0 sysmonwait=false
  P0: status=1 schedtick=3 syscalltick=0 m=0 runqsize=0 gfreecnt=0 timerslen=0
  P14: status=1 schedtick=0 syscalltick=0 m=4 runqsize=0 gfreecnt=0 timerslen=0
  P15: status=1 schedtick=2 syscalltick=0 m=2 runqsize=0 gfreecnt=0 timerslen=0
  M4: p=14 curg=nil mallocing=0 throwing=0 preemptoff= locks=0 dying=0 spinning=true blocked=false lockedg=nil
  M0: p=0 curg=1 mallocing=0 throwing=0 preemptoff= locks=2 dying=0 spinning=false blocked=false lockedg=nil
  G1: status=2(chan receive) m=0 lockedg=nil
  G2: status=4(force gc (idle)) m=nil lockedg=nil
  G5: status=4(GOMAXPROCS updater (idle)) m=nil lockedg=nil
```
`threads=5` at startup, before the theme's own work spawns goroutines, is
`mcount()` — sysmon's M plus the small handful the runtime keeps idle.
`G5`'s `GOMAXPROCS updater (idle)` line is the Go 1.25+ periodic re-check
goroutine, visible here as a real, named goroutine rather than an
abstraction.

The chapter's payoff run flips the preemption check on purpose:

```console
[host]$ GODEBUG=asyncpreemptoff=1 ./go/bin/app sched
=== sched ===
sched: env gomaxprocs=16 numcpu=16
sched: baseline goroutines=1
sched: rendezvous workers=8 baseline=1 during=9 during>=baseline+workers ok
sched: rendezvous drained baseline=1 after=1 after<=baseline+2 ok
sched: workpool workers=8 jobs=64 done=64 done==jobs ok
sched: preempt forced_gomaxprocs=1 spinner_ops=2000000000 ticker_incr=0 ticker_incr>0 FAIL
sched: FAIL
```
The first three checks are unaffected — they don't depend on async
preemption. Only `ticker_incr>0` flips, exactly as the mechanism predicts:
`ticker_incr=0`, since with `SIGURG`-based preemption disabled the spinner
never yields its single P and the ticker never runs.

The gate the runner checks:

```console
[host]$ python3 scripts/test-all-examples.py --only 44-go-runtime-for-systems
building 1 example-lang combinations (jobs=1)...
  build 44-go-runtime-for-systems [go]: ok

verifying...
  verify 44-go-runtime-for-systems [go]: PASS

example                    go
44-go-runtime-for-systems  PASS

1 passed, 0 failed, 0 skipped (logs in build-logs/)
```

```console
[host]$ LSP_LANG=go lua verify.lua
...
PASS 16 / FAIL 0
```

## Cross-check: 2,000 blocked goroutines against a bounded thread count proves the netpoller, not just the claim

The chapter's central claim — goroutines are not threads — is checkable
two independent ways, and both agree. The first is
`runtime.NumGoroutine()`, the Go runtime's own bookkeeping: `netpoll:
parked goroutines=2000 baseline=1 total=2001 total==baseline+2000 ok`
says all 2,000 spawned goroutines are alive and blocked, none errored out
early. The second is `/proc/self/status`'s `Threads:` field, read by
`readThreadCount` directly from the kernel — a number the Go runtime has
no way to fake, since it's the kernel's own thread-group accounting:
`netpoll: parked threads=12 bound=80 threads<=bound ok`. Twelve threads,
not two thousand, comfortably inside a bound (`4*GOMAXPROCS+16=80`) set
two orders of magnitude below the goroutine count specifically so it only
fails if something regresses to one-thread-per-blocked-goroutine.
`verify.lua`'s `PASS 16 / FAIL 0` folds this in alongside the other three
themes — `netpoll` alone contributes three of those sixteen assertions,
none of them "the binary exited 0."

## What you learned

- **G, M, and P are three resources, and only P is scarce by design**:
  `GOMAXPROCS` P's are the token that lets an M run Go code at all;
  goroutines (G) are cheap and plentiful; OS threads (M) come and go —
  work-stealing moves *half* a queue at a time between P's when one runs
  dry.
- **Signal-based async preemption is what makes `GOMAXPROCS=1` still
  fair**: `SIGURG`, sent by `preemptM` to the specific thread running a
  non-yielding goroutine, lets a second goroutine progress
  (`ticker_incr=388757095` this run) without needing a spare P —
  `GODEBUG=asyncpreemptoff=1` reliably reproduces the pre-1.14 world where
  it can't (`ticker_incr=0 FAIL`).
- **`GOGC` and `GOMEMLIMIT` are independent, composable dials**: loosening
  `GOGC` from 100 to 800 cut this run's cycle count from 21 to 2 for
  identical churn; layering an 8 MiB `GOMEMLIMIT` on a loose `GOGC=800`
  raised it from 2 to 122 — the soft memory ceiling still bites when the
  percentage trigger is effectively off.
- **The netpoller is why goroutines outnumber threads by two orders of
  magnitude**: 2,000 goroutines blocked on pipe reads, parked with no M
  held, left this session's thread count at 12 against a bound of 80 — a
  raw blocking `syscall.Read` on the same 2,000 goroutines would instead
  push the thread count toward 2,000.
- **Go 1.26 shipped two real changes without touching any of the above
  defaults**: Green Tea's page-granular marking is on by default
  (invisible in `gctrace`, a build-time signal only), and the dedicated
  `_Psyscall` P status is gone, syscall-ness now read off the goroutine —
  a ~30% cut in baseline cgo-call overhead.
- **A verdict line is a claim the program checks about itself**: every
  `<label> ok`/`FAIL` line is the runtime's own arithmetic, deterministic
  enough (via `SetGCPercent(-1)`, `/proc/self/status`, and a launch order
  that avoided a real livelock during development) that `verify.lua` only
  has to match the fixed label text plus the literal word `ok`.

Part 12 closes here. Two deep dives, two languages, one shared discipline:
Chapter 43 proved Rust's `unsafe` promises with `miri`; this chapter proved
Go's runtime promises with the runtime's own instrumentation, turned back
on itself.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
host this session, on <code>go1.26.5</code>:
<code>python3 scripts/test-all-examples.py --only
44-go-runtime-for-systems</code> reported <code>1 passed, 0 failed</code>
(go column only; the book's second single-language example), and
<code>LSP_LANG=go lua verify.lua</code> reported
<code>PASS 16 / FAIL 0</code>. <code>./demo.sh go run all</code> produced
the exact transcript quoted above: <code>ALL: PASS</code> with every theme
verdict <code>ok</code>, including <code>NumGC</code> advancing by exactly
<code>delta=1</code> across one forced <code>runtime.GC()</code> call,
<code>gogc800_cycles=2</code> under <code>gogc100_cycles=21</code> for
identical churn, <code>memlimit_tight_cycles=122</code> over
<code>memlimit_unset_cycles=2</code> with <code>GOGC</code> held fixed at
800, and 2,000 goroutines parked on the netpoller with the OS thread count
at 12 against a bound of 80 — all on this 16-logical-CPU host
(<code>gomaxprocs=16 numcpu=16</code>). A fresh
<code>GODEBUG=gctrace=1</code> capture reproduced the two
<code>(forced)</code> lines with the arithmetically meaningless
<code>8532210231531 MB goal</code> under <code>SetGCPercent(-1)</code>,
followed by a sane <code>4 MB goal</code> once a real percent was
restored; a fresh <code>GODEBUG=schedtrace=1000,scheddetail=1</code>
capture showed the startup <code>threads=5</code> summary line and
per-P/M/G detail including the Go 1.25+
<code>GOMAXPROCS updater (idle)</code> goroutine. The deliberate contrast
run, <code>GODEBUG=asyncpreemptoff=1 ./go/bin/app sched</code>, reproduced
<code>ticker_incr=0 ticker_incr>0 FAIL</code> and <code>sched: FAIL</code>
exactly, with the other three <code>sched</code> checks unaffected. Green
Tea GC's default-on status in Go 1.26 and the removal of the dedicated
<code>_Psyscall</code> P state are cited from the official Go 1.26 release
notes and runtime source, not measured directly — Green Tea leaves no
signal in <code>gctrace</code> output by design. Not exercised:
<code>examples/manifest.yaml</code> marks this <code>mode: local</code> —
no VM or LGTM path applies; the <code>asyncpreemptoff=1</code> run is a
deliberate FAIL demo, never asserted green in <code>verify.lua</code>.</p>
