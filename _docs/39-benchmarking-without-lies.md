---
title: "Benchmarking without lies: warmup, coordinated omission, and the percentile the mean hides"
order: 39
part: "Performance and Low Latency"
description: "benchlab measures the ch21 chatterd frame codec two ways in the same binary — a real run with warmup discard, per-iteration timing, and a coordinated-omission-corrected p99, and --lie's single unwarmed wall-clock sample — cross-checked with perf stat cycles/instructions and an objdump proof that an unwatched microbenchmark just gets compiled away."
duration: "50 minutes"
---

Chapter 37 gave you the USE method's 60-second checklist for a system already
in trouble, and Chapter 38 wired that telemetry into the LGTM stack you can
query back. Both assume the numbers arriving are real. This chapter asks the
harder question: when *you* produce the number — a microbenchmark of your own
function, timed on your own laptop — what makes it trustworthy? The uncomfortable
answer is that most benchmarks people paste into commit messages and PR
descriptions are not measuring what they claim to. `benchlab` is built to show
exactly how, by measuring the same thing two ways in the same binary: once
properly, and once as the anti-pattern this chapter argues against, so you can
watch the story change under your own two hands.

The code is in `examples/39-benchmarking-without-lies/`. The run script there
builds/sets up and runs it; its `README.md` covers what it does and how to
drive it.

{% include excalidraw.html
   file="39-benchmark-pipeline"
   alt="Two bands. Top, benchlab real path: a warmup loop of W untimed calls to run_once feeding a timed loop of N iterations each timed with the monotonic clock into a raw samples array with a running checksum, splitting into a sort-and-percentile path producing min/median/p99/max and a parallel co_correct path producing co_p99_ns, co_n, and checksum, both landing in a printed table. Bottom, benchlab --lie: one untimed call straight to a wall-clock delta straight to a printed elapsed_ns, no percentiles because there is nothing to compute one from. A note reads same codec, same fixed workload — the harness, not the code under test, is what changes the story."
   caption="Figure 39.1 — the same codec measured two ways: warmup, per-iteration timing, and coordinated-omission correction versus one unwarmed wall-clock sample" %}

> **Tools used** — `perf stat`, `cpupower`, `objdump`, `g++` (host, ad hoc
> compile for the dead-code-elimination demonstration). `perf` and `g++`/`gcc`
> are checked by `scripts/check-host.sh`; `cpupower` and `objdump` ship with
> Fedora's `kernel-tools` and `binutils`, which this book already requires.

## Warmup, and why a cold call is not a measurement

`benchlab --op roundtrip --iters N --warmup W` runs `W` throwaway calls to
`run_once` first, discarding every one, before it times a single sample. The
reason is that the *first* calls to almost anything are not representative of
the steady state a real caller experiences. The allocator has not yet carved
out the arena it will reuse for the rest of the run; the branch predictor has
not yet learned which way `decode_frame`'s length check usually goes; on a
JIT'd or GC'd runtime the picture is worse still (Go's escape analysis and GC
pacer are both steady-state phenomena — irrelevant here since the codec is
allocation-light, but the discipline generalizes). Skip the warmup and your
"benchmark" is measuring cold-start cost, which is a real and useful number,
just not the one anyone means by "how fast is `encode_frame`."

Run-to-run variance is the second reason a single sample is worthless. Ten
runs of the same fixed workload on the same host produce ten different
numbers, because the scheduler, the cache state, and (on this laptop) the
CPU's own clock all move between runs. `benchlab`'s answer is to never trust
one number: it collects `N` individually-timed samples and reports the
distribution — min, median, p99, max — not a single average.

## Fixing the clock: the governor is part of the experiment

Before any of those samples mean anything, the thing that's supposed to be
constant across the run — the CPU's clock rate — has to actually be close to
constant. Modern Intel chips run under `intel_pstate`, which picks a frequency
per core, per instant, based on load and a policy called the governor. On the
reference host for this chapter:

```console
[host]$ cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
powersave
[host]$ cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
performance powersave
[host]$ cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq
800000
4600000
```

That is an 800 MHz to 4.6 GHz range on one core, entirely at the governor's
discretion under `powersave`. `cpupower frequency-info` reads the same policy
back with more context:

```console
[host]$ cpupower frequency-info
analyzing CPU 1:
  driver: intel_pstate
  ...
  energy performance preference: balance_performance
  hardware limits: 800 MHz - 4.60 GHz
  available cpufreq governors: performance powersave
  current policy: frequency should be within 800 MHz and 4.60 GHz.
                  The governor "powersave" may decide which speed to use
                  within this range.
  current CPU frequency: 2.05 GHz (asserted by call to kernel)
  boost state support:
    Supported: yes
    Active: yes
```

`powersave` with turbo **Active: yes** means the core running your benchmark
loop can legitimately be executing the same instruction stream at 800 MHz in
one sample and north of 4.6 GHz two samples later, purely because the
governor decided the load justified a boost — or a neighboring core's thermal
budget forced a step down. That is a confound with nothing to do with your
code, and it lands directly in `max_ns` and in the tail percentiles this
chapter cares about. The fix, on a host you control, is to pin the governor
before you benchmark: `echo performance | sudo tee
/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` (or the equivalent
`sudo cpupower frequency-set -g performance`) locks every core to its highest
non-turbo sustained frequency for the duration of the run. This chapter's own
numbers below were captured *without* making that change — deliberately, so
that the variance the next two sections discuss is the real variance this
host produces under its default policy, not variance edited away before you
could see it. If you reproduce these numbers and they differ, check your
governor first.

## Coordinated omission: the request that didn't get to start

`benchlab`'s timed loop is a **closed loop**: iteration `N+1` does not start
its clock until iteration `N` has returned. This is the natural way to write
a benchmark loop, and it is also, in Gil Tene's phrase, the exact shape that
produces **coordinated omission**. Suppose iteration 4 stalls — a page fault,
a context switch onto a different core, a neighbor process winning the
scheduler's attention — for 1000x the typical iteration cost. The closed loop
records exactly *one* sample for that stall: iteration 4's latency. But a real
caller is not a closed loop. A client sending one request every 47 ns (this
run's median) does not pause its request rate while iteration 4's stall plays
out — five, ten, a hundred more requests queue up behind it, each waiting its
turn, each experiencing a delay close to the stall's own length. The closed
loop's benchmark omitted every one of those queued requests from its sample
set. It didn't just under-measure the tail; it *erased* the part of the
distribution the tail exists to describe.

{% include excalidraw.html
   file="39-coordinated-omission"
   alt="Two stacked bands. Top, benchlab's raw closed-loop timer: iterations 1 through 3 each a small box around 40 nanoseconds, iteration 4 an accent box labeled STALL page fault slash sched, 1057 nanoseconds, one sample. Bottom, what a fixed-rate caller would have seen: the same three small requests, the same stalling request 4, then two dashed ghost boxes labeled request 5 queued and request 6 queued at roughly 1018 and 979 nanoseconds, backfilled synthetic samples continuing down toward the expected interval floor. A dashed vertical line connects the two request 4 boxes labeled same stall. Captions read the raw p99 over a 1000-sample window buries the single stall sample, while the corrected co_p99 backfills the queued requests so co_p99_ns is always at least p99_ns."
   caption="Figure 39.2 — one stall, recorded once by the closed loop (top) versus what a fixed-rate caller actually experiences (bottom); co_correct backfills the difference" %}

The correction — HdrHistogram calls it `recordValueWithExpectedInterval` — is
mechanical once you see the picture: for every raw sample `v` that exceeds an
*expected interval* `E` (the per-op time a sustained caller would target),
backfill synthetic samples `v - E, v - 2E, ...` down to `E`, standing in for
the requests that would have queued behind the stall. `benchlab` sets `E` to
the run's own raw median, floored at 1 ns, so the correction needs no extra
flag and comes from the same run it corrects — and caps the backfill at
5,000,000 synthetic samples so one pathological stall can't grow the array
without bound. On a run captured this session:

```console
[host]$ perf stat -e cycles,instructions,task-clock \
    ./cpp/build/release/app --op roundtrip --iters 100000 --warmup 5000
benchlab: op=roundtrip iters=100000 warmup=5000
benchlab: n=100000 min_ns=38 median_ns=42 p99_ns=86 max_ns=56595
benchlab: co_p99_ns=16930 expected_interval_ns=42 co_n=107077
benchlab: checksum=000000000079c3e0
```

The raw `p99_ns` is **86 nanoseconds** — a single stall out near `max_ns`
(56,595 ns, almost 1350x the median) barely registers, because it is exactly
one sample out of the 1000-widest slice the p99 looks at. The corrected
`co_p99_ns` is **16,930 nanoseconds** — nearly 200x the raw figure — because
that one 56 microsecond stall backfilled thousands of synthetic queued
samples into the tail, and enough of them landed above the 99th-percentile
cutoff to move it by two orders of magnitude. Both numbers came from the
*same* 100,000-iteration run; the only difference is whether the harness
accounts for the requests coordinated omission would otherwise omit. Reporting
the raw number alone — which is what most naive benchmark loops do — is not
wrong arithmetic, it is measuring the wrong thing.

## HDR-style percentiles, and why the mean lies

None of the numbers above are an arithmetic mean, and that is deliberate.
Latency distributions are heavy-tailed: the vast majority of calls finish in
tens of nanoseconds, and a small minority — the ones a scheduler, a page
fault, or (in a GC'd runtime) a collection pause touches — finish orders of
magnitude slower. Average those together and the mean is dragged toward the
tail by exactly the outliers a reader most needs to see reported accurately,
while simultaneously hiding *how bad* the tail actually gets: a mean of 200 ns
is consistent with "every call took 200 ns" and with "999 calls took 40 ns and
one took 160,200 ns," and those are utterly different systems to operate. A
**percentile** answers a sharper question — "what latency do 99% of calls beat?"
— and doesn't blend the tail into the body. `benchlab`'s `percentile()`
function is the simplest correct version of this idea, nearest-rank over a
fully sorted sample array (`sorted[floor(p * (n-1))]`); a production HdrHistogram
would instead bucket values logarithmically to report percentiles in
constant memory over unbounded sample counts, which is the one simplification
worth naming here — this harness sorts a real in-memory array, which is fine
at these iteration counts and would need revisiting past tens of millions of
samples.

## Microbench pitfalls: the compiler deletes what you don't watch

The final trap has nothing to do with clocks: it's that a sufficiently smart
compiler will happily prove your benchmark's result is never used and delete
the code that produces it, leaving you an accurate measurement of an empty
loop. Every `run_once` call in `benchlab` folds its result into a running
`checksum` for exactly this reason — the accumulation is a real, observable
side effect (it's printed at the end), so nothing upstream of it can be
proven dead. To see the failure mode this guards against, compile a stripped
version of the same idea and read what GCC actually produced:

```console
[host]$ g++ -std=c++23 -O2 -c dce.cpp -o dce.o && objdump -d --no-show-raw-insn dce.o
0000000000000000 <bench_discarded>:
   0:	ret
   1:	nopl   0x0(%rax)
   5:	data16 cs nopw 0x0(%rax,%rax,1)

0000000000000010 <bench_sunk>:
  10:	mov    %edi,%r9d
  13:	test   %edi,%edi
  ...
```

`bench_discarded` runs a checksum function over the workload bytes `iters`
times and never uses the result — GCC 16.1.1 at `-O2` proved the whole loop
has no observable effect and compiled it to a bare `ret`. Nothing ran. A
benchmark harness with this shape reports a real, reproducible, extremely
fast time for code that never executed even once. `bench_sunk` is the
identical loop with one change — the result feeds a `volatile` global — and
the disassembly shows the real loop, vectorized with SSE, doing actual work
on every iteration. `benchlab`'s checksum plays exactly the role of that
`volatile` sink, and it earns its keep twice: it stops the optimizer from
proving the loop dead, and — because the workload is fixed — it doubles as a
correctness oracle. All three languages produce the identical checksum for
the identical fixed workload (`--op encode --iters 20000 --warmup 1000`, this
session):

```console
[host]$ ./cpp/build/release/app --op encode --iters 20000 --warmup 1000 | tail -1
benchlab: checksum=00000000001f6710
[host]$ ./go/bin/app --op encode --iters 20000 --warmup 1000 | tail -1
benchlab: checksum=00000000001f6710
[host]$ ./rust/target/release/app --op encode --iters 20000 --warmup 1000 | tail -1
benchlab: checksum=00000000001f6710
```

A diverging checksum across languages would mean a codec bug, not
measurement noise — the ns timings are expected to vary run to run, the
checksum is not.

Real benchmark frameworks solve the same dead-code problem with a dedicated
API rather than a manual accumulator: Google Benchmark's
`benchmark::DoNotOptimize()`, Rust's `criterion`, and Go's `testing.B` (whose
`b.N`-driven loop the compiler cannot fully see through, plus `go test
-bench . -benchmem` and the companion `benchstat` tool for comparing two
runs statistically rather than eyeballing them) all exist to answer exactly
this question at scale, across many benchmarked functions, with proper
statistical comparison between runs. `benchlab` deliberately uses none of
them: it is a from-scratch harness precisely so the mechanism — warmup,
per-iteration timing, coordinated-omission correction, the dead-code trap —
is visible in ~150 lines you can read end to end, identically shaped in all
three languages, rather than hidden inside a framework's own reporting
format.

## How the code works

The percentile math and the coordinated-omission correction are the two
functions worth reading closely; everything else in `main` is argument
parsing and the two loops already described. `co_correct` takes the raw,
unsorted sample array, the expected interval, and a hard cap, and returns a
new array with synthetic samples backfilled behind every stall:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
std::vector<std::int64_t> co_correct(const std::vector<std::int64_t>& raw,
                                      std::int64_t expected_interval_ns,
                                      std::size_t cap) {
    std::vector<std::int64_t> out;
    out.reserve(raw.size());
    for (std::int64_t v : raw) {
        out.push_back(v);
        if (expected_interval_ns <= 0 || v <= expected_interval_ns) continue;
        std::int64_t missing = v - expected_interval_ns;
        while (missing >= expected_interval_ns && out.size() < cap) {
            out.push_back(missing);
            missing -= expected_interval_ns;
        }
        if (out.size() >= cap) break;
    }
    return out;
}
```

```go
func coCorrect(raw []int64, expectedIntervalNs int64, cap int) []int64 {
	out := make([]int64, 0, len(raw))
	for _, v := range raw {
		out = append(out, v)
		if expectedIntervalNs <= 0 || v <= expectedIntervalNs {
			continue
		}
		missing := v - expectedIntervalNs
		for missing >= expectedIntervalNs {
			if len(out) >= cap {
				return out
			}
			out = append(out, missing)
			missing -= expectedIntervalNs
		}
	}
	return out
}
```

```rust
fn co_correct(raw: &[i64], expected_interval_ns: i64, cap: usize) -> Vec<i64> {
    let mut out = Vec::with_capacity(raw.len());
    'outer: for &v in raw {
        out.push(v);
        if expected_interval_ns <= 0 || v <= expected_interval_ns {
            continue;
        }
        let mut missing = v - expected_interval_ns;
        while missing >= expected_interval_ns {
            if out.len() >= cap {
                break 'outer;
            }
            out.push(missing);
            missing -= expected_interval_ns;
        }
    }
    out
}
```

Every sample `v` is pushed to `out` unconditionally first — a normal,
non-stalling iteration contributes exactly itself, which is why `co_n >= n`
always and equals `n` on a run with no stalls at all. The `if` guard skips
correction entirely for samples at or below the interval — those are the
*fast* calls, nothing queued behind them. Once `v` exceeds the interval, the
`while`/`for` loop walks backward from `v - E` in steps of `E`, pushing one
synthetic sample per step: this is precisely "how many requests, each
spaced `E` apart, would have piled up during a stall of length `v`?" The cap
check inside the loop (not just at the top) matters because a single
pathological stall can demand millions of synthetic samples in one pass —
without the inner check, that one sample could blow the cap on its own
before the outer loop ever gets to see it. C++ mutates a `std::vector` under
manual `reserve`/`push_back`; Go uses `append` against a capacity-preallocated
slice and a `for`-with-condition in place of `while`; Rust needs a labeled
`'outer` loop because breaking the outer `for` from inside the inner `while`
has no unlabeled spelling — three idioms, the same three-branch algorithm,
provably matching because `verify.lua` runs all three against the identical
fixed workload and checks the identical invariants (`co_p99_ns >= p99_ns`,
`co_n >= n`) rather than the identical source text.

`percentile()` is the other half: given a *sorted* array and a fraction `p`,
nearest-rank indexing (`sorted[floor(p * (len - 1))]`) picks the sample at
that rank directly — no interpolation, no bucketing, correct as long as the
array really is sorted first, which `main` guarantees by calling `sort`
(C++'s `std::sort`, Go's `sort.Slice`, Rust's `sort_unstable`) immediately
before computing `median_ns`, `p99_ns`, and again after `co_correct` before
computing `co_p99_ns`. The warmup loop and the timed loop share the same
`run_once` call; the only difference between them is that the warmup loop
discards the per-call scalar into `checksum` without ever starting a clock
around it, and the timed loop wraps each call between two monotonic-clock
reads (`std::chrono::steady_clock`, `time.Now()`'s monotonic reading,
`std::time::Instant`) and appends the delta to `raw` before moving to the
next iteration.

## Errors, three ways

`benchlab` has two failure surfaces, and they map to two different exit
codes across all three languages. A **usage error** — no `--op` given for a
real run, an unrecognized `--op` value, or `--warmup >= --iters` (which would
leave zero samples to time) — prints the two-line usage banner to stderr and
exits **2**; C++ funnels every parse path through `usage_and_exit()`, Go
through `usage()` plus `os.Exit(2)`, Rust through a `usage() -> !` that calls
`std::process::exit(2)` and therefore never returns, which the type checker
enforces. A **codec bug** — `decode_frame` rejecting a frame from a workload
this harness controls and never corrupts — is exit **1**, and it is the one
path `verify.lua` never exercises, because it should be unreachable on this
fixed input; it exists so a future change to the workload or the codec fails
loudly rather than silently producing a bogus checksum. C++ checks the
`std::expected` returned by `decode_frame` and calls `std::exit(1)` on
`std::unexpected`; Go's `decodeFrame` wraps the sentinel errors
(`errShortHeader`, `errBadMagic`, `errBadVersion`, `errLengthMismatch`) with
`%w` so `runOnce` can wrap them again with call-site context and the caller
still `errors.Is`-matches the original; Rust's `decode_frame` returns
`Result<(u8, Vec<u8>), String>` and `run_once` propagates it with a bare `?`,
letting `main`'s `match` turn any `Err` into `ExitCode::from(1)`. A successful
run of either mode — the real path or `--lie` — exits **0**.

## Concurrency lens

`benchlab` itself is deliberately single-threaded — running the timed loop on
one thread is what makes "iteration N+1 starts after N returns" a clean,
analyzable closed loop in the first place; multiply the callers and you'd
need to reason about contention on top of coordinated omission, a strictly
harder problem this chapter isn't taking on. But the *host* underneath a
single-threaded benchmark is never single-threaded, and that's where the
stall in this chapter's own `co_p99_ns=16930` sample almost certainly came
from: the Linux scheduler is free to migrate the benchmark's one thread
between any of this host's 16 logical CPUs between iterations, and a
migration that lands it on a core the kernel had to first bring back up
from a low C-state, or evicts its L1/L2 working set entirely, produces
exactly the multi-microsecond single-iteration stall this chapter measures
and corrects for. Chapter 26's `taskset -c 0,2` pinning is the mitigation:
running `taskset -c 4 ./cpp/build/release/app ...` confines the benchmark
to one core for the whole run, trading the ability to migrate away from a
noisy neighbor for the elimination of migration-induced stalls as a source
of tail latency — worth doing whenever you want the tail to reflect the code
under test rather than the scheduler's decisions about it.

## Build, run, observe

```bash
[host]$ cd examples/39-benchmarking-without-lies && ./demo.sh build
```

```console
[host]$ ./cpp/build/release/app --op roundtrip --iters 100000 --warmup 5000
benchlab: op=roundtrip iters=100000 warmup=5000
benchlab: n=100000 min_ns=39 median_ns=47 p99_ns=73 max_ns=5480
benchlab: co_p99_ns=75 expected_interval_ns=47 co_n=100428
benchlab: checksum=000000000079c3e0
[host]$ ./cpp/build/release/app --lie --op roundtrip
benchlab: lie op=roundtrip (no warmup, single wall-clock sample, ignores variance)
benchlab: lie elapsed_ns=1077 sink=76
```

The real run and the lie run measure the identical codec against the
identical fixed workload — the same `alice\0the quick brown fox jumps over
the lazy dog, three times, for benchlab` `DELIVER` frame — and produce
answers that look nothing alike: a five-line distribution with a correction
term, versus a single number with no way to tell whether it's typical or a
fluke. Neither `p99_ns=` nor `median_ns=` ever appears in `--lie`'s output;
`verify.lua` asserts that absence directly, because it's the entire point of
running `--lie` at all.

The runner drives all three languages and asserts the shared contract:

```console
[host]$ python3 scripts/test-all-examples.py --only 39-benchmarking-without-lies
building 3 example-lang combinations (jobs=1)...
  build 39-benchmarking-without-lies [cpp]: ok
  build 39-benchmarking-without-lies [go]: ok
  build 39-benchmarking-without-lies [rust]: ok

verifying...
  verify 39-benchmarking-without-lies [cpp]: PASS
  verify 39-benchmarking-without-lies [go]: PASS
  verify 39-benchmarking-without-lies [rust]: PASS

example                       cpp   go    rust
39-benchmarking-without-lies  PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

## Cross-check: perf counts the work, the corrected percentiles count the stalls

Two independent signals confirm two different claims. First, `perf stat`
counts CPU work directly, which a wall-clock number alone cannot separate
from scheduling noise:

```console
[host]$ perf stat -e cycles,instructions,task-clock \
    ./cpp/build/release/app --op roundtrip --iters 100000 --warmup 5000
        41,029,644      cycles:u
        95,724,894      instructions:u
             12.03 msec task-clock:u
       0.012523557 seconds time elapsed
```

95,724,894 instructions over 41,029,644 cycles is an IPC of **~2.33** for
105,000 total roundtrip calls (5,000 warmup plus 100,000 timed) — a plausible,
stable figure for a branch-light encode/decode over a ~78-byte frame, and a
number that doesn't move with scheduler noise the way `max_ns` does, because
`cycles`/`instructions` count retired work, not wall time. If a change to the
codec doubled `cycles:u` for the same `instructions:u`, that would be cache or
branch-misprediction cost growing, not timing variance — a distinction the ns
percentiles alone can't make.

Second, the corrected-versus-naive percentiles from that same invocation are
the other half of the cross-check, and they're worth setting side by side
explicitly:

| | raw (naive) | coordinated-omission-corrected |
|---|---|---|
| p99 | 86 ns | 16,930 ns |
| sample count | 100,000 | 107,077 |

Same run, same 100,000 timed iterations, same single 56,595 ns stall visible
in `max_ns` — one number treats that stall as a footnote, the other treats it
as the ~197x-worse tail a real fixed-rate caller would have actually felt.
Neither number is "wrong" in isolation; reporting only the left column while
implying it describes what users experience is the lie this chapter's title
refers to.

## What you learned

- A closed-loop benchmark (iteration N+1 waits for N) records a stall as one
  sample when a real fixed-rate caller would have queued several requests
  behind it — Gil Tene's coordinated omission — and the fix is to backfill
  synthetic samples from the raw value down to the expected interval, which
  is exactly what turned this chapter's `86 ns` raw p99 into a `16,930 ns`
  corrected one from the identical run.
- Percentiles, not the mean, are how you report a heavy-tailed latency
  distribution: nearest-rank indexing over a sorted sample array is the
  simplest correct version, and it's what real HdrHistogram bucketing
  approximates at scale.
- A benchmark whose result nothing uses is a benchmark the compiler is free
  to delete — GCC turned an unwatched loop calling a real function `iters`
  times into a bare `ret`, confirmed by reading the actual `objdump` output;
  folding every call's result into a printed checksum (`benchlab`'s own
  correctness oracle, identical across all three languages) is what keeps
  the loop alive, playing the same role as `benchmark::DoNotOptimize()`,
  criterion's black-boxing, or a `testing.B` sink.
- The CPU frequency governor is part of the experiment, not incidental to
  it: `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor` reporting
  `powersave` with turbo boost active means the clock itself can move
  between samples, and pinning to `performance` before benchmarking removes
  that confound from the tail you're trying to measure.

Chapter 40 takes the discipline this chapter earned — real percentiles, a
pinned clock, a tail you trust — and spends it: stripping a hot path down to
pinned cores, zero allocation, and the low-latency techniques that make the
corrected p99 small in the first place.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host (kernel 7.1.3-200.fc44, 11th-gen Intel i7-11800H, 16 CPUs,
<code>intel_pstate</code>/<code>powersave</code> governor, turbo active) this
session: <code>python3 scripts/test-all-examples.py --only
39-benchmarking-without-lies</code> printed <code>PASS PASS PASS</code> (3
passed, 0 failed, 0 skipped; <code>verify.lua</code> 42 checks total across
cpp/go/rust). The cited real run printed
<code>min_ns=39 median_ns=47 p99_ns=73 max_ns=5480 co_p99_ns=75</code>; the
<code>perf stat</code> run printed <code>41,029,644 cycles:u</code>,
<code>95,724,894 instructions:u</code>, and (that specific run)
<code>p99_ns=86 co_p99_ns=16930 max_ns=56595</code> — the side-by-side
percentile table above is that exact run's output, not a constructed example.
<code>--lie</code> printed <code>elapsed_ns=1077 sink=76</code> with no
<code>p99_ns=</code>/<code>median_ns=</code> anywhere in its output. The
dead-code-elimination excerpt (<code>bench_discarded</code> compiling to a
bare <code>ret</code>, <code>bench_sunk</code> compiling to a real vectorized
loop) came from a throwaway file compiled ad hoc with <code>g++ -std=c++23
-O2</code> (GCC 16.1.1) this session, not from the shipped example. The
checksum <code>00000000001f6710</code> for <code>--op encode --iters 20000
--warmup 1000</code> was confirmed identical across all three built binaries
this session. Not exercised: pinning the governor to <code>performance</code>
(this host's numbers are all under the default <code>powersave</code> policy,
deliberately, as the CPU-frequency section explains) and any run against a
lab VM — this is a host-only, local-mode example with no VM/LGTM
requirement.</p>
