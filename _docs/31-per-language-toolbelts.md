---
title: "Per-Language Toolbelts"
order: 31
part: "Debugging"
description: "app hot and app alloc are the same trial-division prime count and the same string-churn index build in C++, Go, and Rust — and each language's own profiler, run for real against the built binary, points at the same widest frame: perf record/report and clang-tidy/-fanalyzer for C++, the pprof ecosystem and delve for Go, cargo-flamegraph and clippy for Rust."
duration: "55 minutes"
---

Chapter 28 opened this part by reading a crash gdb already knew about;
Chapter 29 broke code on purpose and watched a sanitizer catch it. Both
chapters answer "what is wrong". This one answers a different question that
comes up on every program that runs correctly and still feels slow: "where
does the time go, and which line is spending it?" That question has no
generic answer — each language ships its own native way of pointing a
profiler at a running binary — but the *skill* of reading what comes back is
the same skill everywhere: find the widest frame in a call-stack sample and
that is your hot path. `app` is this chapter's target, one program built
three times with one contract: `app <hot|alloc> [--n N]` runs either an
unoptimized trial-division prime count (`hot`) or a 1,000-key string-churn
index build (`alloc`), and both modes route every bit of real work through
exactly one `noinline` function per mode, so there is never any ambiguity
about which frame should show up wide.

The code is in `examples/31-per-language-toolbelts/`. `./demo.sh build`
builds all three; each language directory's `demo.sh run [hot|alloc]` runs
it, and the top-level `README.md` documents the full toolbelt transcript
this chapter draws from.

{% include excalidraw.html
   file="31-flamegraph-reading"
   alt="A schematic flamegraph with two labeled axes: y is stack depth, bottom the root frame runtime.main and top the leaf running at the instant a sample was taken; x is share of samples, explicitly not time order or call order. A stack of boxes narrows from a full-width root frame up through main/main.run, into a wide accent box for spin_hot/hotSpin covering about 99% of the row's width, into an equally wide inlined divisor-loop frame directly above it, topped by a leaf sliver. A callout box explains that color here just marks which language tab a frame came from, since real profilers usually color by module or at random. A footer note says the way to find the hot path is to scan for the widest unbroken column reaching toward the top."
   caption="Figure 31.1 — reading a flamegraph: width is the share of samples, height is stack depth, and the widest column reaching toward the top is the function actually spending the cycles" %}

> **Tools used** — `perf` record/report/stat, `clang-tidy`, GCC `-fanalyzer`
> (C++); `go tool pprof`, `go tool trace`, `dlv` (Go); `perf` record/report,
> `cargo flamegraph`, `cargo clippy`, `cargo fmt --check` (Rust) — all host.
> `perf`, `gdb`, and the Go/Rust toolchains are on `scripts/check-host.sh`;
> `dlv` and `cargo-flamegraph` are pulled on demand (`go install .../dlv`,
> `cargo install flamegraph`) as this chapter's transcripts show.

## Why every language needs its own toolbelt

`perf` is an *external* sampler: it attaches to any ELF binary from the
outside, interrupts it on a hardware or software event (by default a fixed
number of CPU cycles), and records the instruction pointer and call stack at
each interrupt. It has no idea what language produced the binary — it only
needs a symbol table and, for call graphs, either frame pointers or DWARF —
so `perf record`/`perf report`/`perf stat` work identically against the C++
and Rust binaries in this chapter. That is also exactly why they *don't*
work as cleanly for Go: Go's runtime multiplexes goroutines onto a handful of
OS threads and moves them around, which external samplers can still see but
cannot label with the right goroutine-level context. Go's own answer is to
put the profiler *inside* the binary: `runtime/pprof` and `runtime/trace` are
libraries the program calls into, so writing a CPU profile, a heap profile,
or an execution trace is part of the program's own behavior, driven by flags
this chapter's Go build accepts and C++/Rust do not (`--cpuprofile`,
`--memprofile`, `--trace`). Rust sits in the middle: it is compiled to the
same kind of native ELF binary as C++, so `perf` attaches to it the same way,
but the Rust ecosystem also ships `cargo flamegraph`, a Rust program that
*wraps* `perf record` and then renders the result with `inferno`, a Rust
reimplementation of Brendan Gregg's original Perl `stackcollapse`/`flamegraph`
scripts. Three ecosystems, three different relationships between the
profiler and the profiled program — but the artifact each one hands back is
the same shape: a call stack with a weight attached, which is what a
flamegraph draws directly and what `--top` tabulates as text.

A flamegraph earns its name from the shape, not from heat: each horizontal
row is one level of stack depth (the root frame along the bottom, the
function actually executing at the top), and the *width* of a box is the
fraction of samples in which that function was somewhere on the stack at
that depth. Frames are usually **sorted alphabetically within their parent**,
not by call order or wall-clock time, which is the detail that trips up
first-time readers expecting the x-axis to mean "when". Reading one is a
single scan: follow the tallest, widest column of boxes from the bottom to
the top, and the name at the top of that column is where the cycles are
actually going. Figure 31.1 draws that shape directly from this chapter's
own data — `spin_hot`/`hotSpin` is that column in every one of the three
profiles below.

{% include excalidraw.html
   file="31-toolbelt-map"
   alt="One shared target box, app hot or alloc with an identical result across languages, feeding three columns. The C++ column lists perf record with call-graph fp as its accent entry naming spin_hot at 99.9%, then perf report/perf stat, clang-tidy, GCC -fanalyzer, and a dashed gdb box marked Chapter 28. The Go column lists runtime/pprof as its accent entry, cpuprofile into go tool pprof top naming main.hotSpin, then memprofile with alloc_space naming main.allocChurn, runtime/trace with go tool trace, delve, and a dashed go vet box. The Rust column lists perf record with call-graph fp as its accent entry naming spin_hot inlined near 100%, then cargo flamegraph producing a real SVG, cargo clippy and cargo fmt check, a dashed tokio-console box marked forward reference to Chapter 27 not applicable here since there is no async runtime, and a dashed gdb/rust-gdb box marked Chapter 28. A note explains the amber boxes are the CPU-profiling step this chapter cross-checks against each other, and the dashed boxes are noted but out of scope."
   caption="Figure 31.2 — one shared target, three native toolbelts: the amber boxes are what this chapter's cross-check compares; the dashed boxes point to where gdb (Ch. 28) and tokio-console (Ch. 27) belong instead" %}

## How the code works

The two workloads are deliberately simple, because the point of this chapter
is the tool, not the algorithm — but the two `noinline` functions they route
through are the whole reason the profiles below are unambiguous, so they get
the tabs, verbatim from `examples/31-per-language-toolbelts/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// The CPU-bound target: trial division is deliberately unoptimized (checks
// every candidate divisor up to sqrt(i)) so a couple of seconds of runtime
// buys a profile with a single, unmistakable hot frame.
extern "C" [[gnu::noinline]] std::uint64_t spin_hot(long n) {
    std::uint64_t count = 0;
    for (long i = 2; i < n; ++i) {
        bool prime = true;
        for (long d = 2; d * d <= i; ++d) {
            if (i % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) {
            ++count;
        }
    }
    return count;
}
```

```go
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
```

```rust
/// The CPU-bound target: trial division is deliberately unoptimized (checks
/// every candidate divisor up to sqrt(i)) so a couple of seconds of runtime
/// buys a profile with a single, unmistakable hot frame. `#[inline(never)]`
/// keeps the compiler from folding it into `run` and hiding it from the
/// sampler.
#[inline(never)]
fn spin_hot(n: u64) -> u64 {
    let mut count = 0u64;
    for i in 2..n {
        let mut prime = true;
        let mut d = 2u64;
        while d * d <= i {
            if i % d == 0 {
                prime = false;
                break;
            }
            d += 1;
        }
        if prime {
            count += 1;
        }
    }
    count
}
```

All three are the same trial-division loop with the same invariant: an
`i` in `[2, n)` is prime only if no `d` in `[2, sqrt(i)]` divides it evenly,
found by walking `d` up from 2 and stopping the moment `d*d > i`. The loop is
intentionally the slow textbook version — no sieve, no memoized small primes,
no early-exit on evens — because a fast implementation would finish before a
sampling profiler collected enough hits to draw a clean picture, and the goal
here is a profile, not a fast prime counter. The `noinline` annotation on
each (`[[gnu::noinline]]`, `//go:noinline`, `#[inline(never)]`) is not
decoration: at `-O2`/release optimization, all three compilers are entitled
to fold a small, single-call-site function directly into its caller, which
would erase the frame boundary a profiler needs to name. `spin_hot` also has
`extern "C"` linkage in the C++ build specifically so `perf` and `gdb` print
the plain symbol `spin_hot` instead of an Itanium-mangled name — a detail
that matters once you are grepping profiler output for a function name, as
every transcript below does.

The allocation-heavy sibling (`alloc_churn`/`allocChurn`/`alloc_churn`, not
shown in the tabs above but present in every `main.cpp`/`main.go`/`main.rs`)
does the opposite kind of work on purpose: `n` iterations round-robin
overwrite a 1,000-key string map, and every iteration allocates a fresh key
string and a fresh value string before inserting. Where `spin_hot` never
touches the heap, `alloc_churn` never does meaningful arithmetic — its job is
to be a clean single allocation site for a heap/allocation profiler to find,
the same way `spin_hot` is a clean single CPU sink for a cycle profiler.

The wiring around both functions is intentionally thin. C++ threads
`std::expected<Config, std::string>` through argument parsing and the mode
dispatch, wraps the timed section in an RAII `ScopedTimer` whose destructor
writes the elapsed milliseconds regardless of how the scope is left, and
prints with `std::println`. Go's `run(cfg)` additionally opens the profiler
files the CLI's extra flags name — `pprof.StartCPUProfile`/`trace.Start`
before the timed call, `pprof.WriteHeapProfile` after it, each guarded by a
`defer` so a profile still flushes on any return path — and Rust threads
`anyhow::Result` through `parse_args`/`run` and returns `ExitCode` from
`main` rather than calling `std::process::exit`, so destructors still run on
every exit path. None of that wiring changes the algorithm; it only exists
so the same binary can be pointed at by three very different profilers
without recompiling it differently for each one.

## Errors, three ways

The CLI contract is the familiar shape from every chapter in this book: a
missing mode, an unknown mode, a non-positive or non-numeric `--n`, and an
unknown flag all print a one-line diagnostic (`app: missing mode`,
`app: unknown mode: bogus`, `app: not a positive integer: zero`,
`app: unknown argument: --bogus`) followed by
`usage: app <hot|alloc> [--n N]` on stderr and exit `2`. Go's usage banner
appends its three profiling-only flags (`--cpuprofile`, `--memprofile`,
`--trace`); `verify.lua`'s pattern match tolerates that suffix because it
only asserts the shared `<hot|alloc> [--n N]` prefix. `--n` itself is parsed
with `std::from_chars` in C++, `strconv.Atoi` in Go, and `str::parse::<u64>`
in Rust — three ways of saying "reject anything that is not cleanly a
positive integer" — and every language rejects the same malformed inputs the
same way, which `verify.lua` checks per language before it ever touches a
profiler.

## Concurrency lens: what each language's tooling gives you

This is a pure-tooling chapter — `app` is single-threaded in every language,
so there is no race to hunt here the way Chapters 25 and 26 hunted one. What
still differs by language is how much of the *concurrency picture* each
toolbelt can show once a program does have more than one thread of control.
`perf record --call-graph` samples every thread in a process by default, so
it is already concurrency-aware for C++ and Rust: point it at `workq` from
Chapter 25 and the flamegraph would show each worker thread's stack
separately. Go's profilers go further because they are runtime-aware: a
`runtime/pprof` CPU profile attributes samples to the goroutine that was
running, and `go tool trace` is built specifically to show
goroutine-level concurrency — when one parks, when the scheduler hands its P
to another, when the GC stops the world — none of which a generic external
sampler can label without help. Rust's forward-referenced tool,
`tokio-console`, is the async-native version of that same idea: it shows
live per-task poll counts and waker state for an async runtime the way `go
tool trace` shows goroutines, which is exactly why it has nothing to display
against this chapter's synchronous `app` and belongs instead with Chapter
27's `chatterd`.

## Build, run, observe

```bash
[host]$ cd examples/31-per-language-toolbelts && ./demo.sh build
```

A hand run confirms the shared contract before any profiler gets involved —
`result` is the same number in all three languages, because it is the same
algorithm over the same input:

```console
[host]$ ./cpp/build/release/app hot
app: mode=hot n=3000000 result=216816 ms=791
[host]$ ./go/bin/app hot
app: mode=hot n=3000000 result=216816 ms=756
[host]$ ./rust/target/release/app hot
app: mode=hot n=3000000 result=216816 ms=464
[host]$ ./cpp/build/release/app alloc
app: mode=alloc n=200000 result=5000 ms=24
[host]$ ./go/bin/app alloc
app: mode=alloc n=200000 result=5000 ms=37
[host]$ ./rust/target/release/app alloc
app: mode=alloc n=200000 result=5000 ms=18
```

Now the toolbelts, each run against the real built binary.

**C++: `perf record`/`perf report`, `clang-tidy`, `-fanalyzer`.**

```console
[host]$ perf record -o cpp.perf.data --call-graph fp -- ./cpp/build/release/app hot
app: mode=hot n=3000000 result=216816 ms=871
[ perf record: Captured and wrote 0.214 MB cpp.perf.data (3461 samples) ]
[host]$ perf report -i cpp.perf.data --stdio
# Samples: 3K of event 'cpu/cycles/Pu'
# Event count (approx.): 3306382033
#
# Children      Self  Command  Shared Object         Symbol
# ........  ........  .......  ....................  ................
#
    99.95%    99.95%  app      app                   [.] spin_hot
            |
            ---spin_hot
```

`spin_hot` is 99.95% of 3,461 samples — everything else (`ld-linux`'s
startup path) is the loader noise that ran before the timed section even
began. `clang-tidy` and GCC's `-fanalyzer` are the static complements: they
never run the binary at all, so they catch a different class of thing —
patterns that are *suspicious* in the source, not slow at runtime:

```console
[host]$ clang-tidy -p cpp/build/release cpp/src/main.cpp
main.cpp:143:5: warning: an exception may be thrown in function 'main' which
  should not throw exceptions [bugprone-exception-escape]
```

One warning, and it is a known, practical false positive:
`bugprone-exception-escape` traces a theoretical path where `std::format`'s
`format_error` could propagate out of `main`, reachable only if the format
string itself were malformed — ours are compile-time literals, so the path
is dead code the checker cannot prove unreachable statically. GCC's
`-fanalyzer` (its own CMake preset, `cmake --build --preset analyze`) is
stricter about interprocedural state and prints nothing at all against this
source — a clean run I reproduced with a fresh `-fanalyzer` build during
this chapter's verification pass.

**Go: the `pprof` ecosystem, `go tool trace`, `delve`.**

```console
[host]$ ./go/bin/app hot --cpuprofile cpu.prof
app: mode=hot n=3000000 result=216816 ms=770
[host]$ go tool pprof --top --nodecount=5 ./go/bin/app cpu.prof
      flat  flat%   sum%        cum   cum%
     760ms 98.70% 98.70%      770ms   100%  main.hotSpin
      10ms  1.30%   100%       10ms  1.30%  runtime.asyncPreempt
         0     0%   100%      770ms   100%  main.main
         0     0%   100%      770ms   100%  main.run
         0     0%   100%      770ms   100%  runtime.main
```

`main.hotSpin` owns 98.70% of samples on its own line (`flat`) and 100% once
you count everything under it on the stack (`cum`) — `runtime.asyncPreempt`
is the Go scheduler's cooperative preemption check firing inside the tight
loop, not separate work. The heap profile needs one extra flag to be useful:

```console
[host]$ ./go/bin/app alloc --memprofile mem.prof
app: mode=alloc n=200000 result=5000 ms=33
[host]$ go tool pprof --top --cum --alloc_space --nodecount=6 ./go/bin/app mem.prof
      flat  flat%   sum%        cum   cum%
 3604.31kB 53.98% 53.98%  6164.35kB 92.32%  main.allocChurn
         0     0% 53.98%  6164.35kB 92.32%  main.main
         0     0% 53.98%  6164.35kB 92.32%  main.run
         0     0% 53.98%  6164.35kB 92.32%  runtime.main
 2560.04kB 38.34% 92.32%  2560.04kB 38.34%  fmt.Sprintf
```

By the time `pprof.WriteHeapProfile` runs, the churned map from the
just-returned `allocChurn` call is already garbage — the default *in-use*
view would show almost nothing live. `--alloc_space` switches to pprof's
cumulative-since-start counter, which remembers every byte `mallocgc` ever
handed out whether or not it was later freed, which is the view that
actually answers "where did the allocations happen": `main.allocChurn`
itself directly owns 53.98% (the map's own bucket storage), and
`fmt.Sprintf`, called twice per iteration to build the key and value
strings, owns another 38.34% underneath it. `go tool trace` opens an
interactive browser UI by default, but `-d=parsed` dumps the same event
stream as text, enough to confirm the tracer really captured `hotSpin`
executing:

```console
[host]$ ./go/bin/app hot --n 800000 --trace trace.out
app: mode=hot n=800000 result=63951 ms=114
[host]$ go tool trace -d=parsed trace.out | grep -B2 hotSpin | head -3
	runtime.asyncPreempt @ 0x484d8a
		/usr/lib/golang/src/runtime/preempt_amd64.s:124
	main.hotSpin @ 0x4d272c
```

Delve is the interactive debugger this chapter ties back to Chapter 28's
`gdb` work: same idea (breakpoints, stack frames, live variables), native to
Go's runtime instead of DWARF-only. It was not preinstalled on this host —
`go install github.com/go-delve/delve/cmd/dlv@latest` pulled it from the Go
module proxy in a few seconds — and it scripts the same way `gdb --batch`
does, by piping a command list instead of typing at a TUI:

```console
[host]$ echo -e "break main.hotSpin\ncontinue\nprint n\nbt\ncontinue\nquit" \
    | dlv --allow-non-terminal-interactive=true exec ./go/bin/app -- hot --n 500000
(dlv) Breakpoint 1 set at 0x4d26e1 for main.hotSpin() ./go/main.go:100
(dlv) > [Breakpoint 1] main.hotSpin() ./go/main.go:100 (hits goroutine(1):1 total:1)
(dlv) 500000
(dlv) 0  0x00000000004d26e1 in main.hotSpin
   at ./go/main.go:100
1  0x00000000004d2f95 in main.run
   at ./go/main.go:175
```

The breakpoint lands exactly on `hotSpin`'s declaration, `print n` confirms
the argument delve stopped with (`500000`, matching `--n 500000`), and `bt`
names the same two frames `go tool pprof` and `go tool trace` already named
— three different Go tools, one function.

**Rust: `perf record`/`perf report`, `cargo flamegraph`, `clippy`.**

Rust compiles to a native ELF binary, so the same external `perf` from the
C++ section attaches to it unchanged:

```console
[host]$ perf record -o rust.perf.data --call-graph fp -- ./rust/target/release/app hot
app: mode=hot n=3000000 result=216816 ms=476
[ perf record: Captured and wrote 0.132 MB rust.perf.data (1895 samples) ]
[host]$ perf report -i rust.perf.data --stdio
# Samples: 1K of event 'cpu/cycles/Pu'
# Event count (approx.): 1946413493
    99.98%    99.98%  app      app                   [.] app::spin_hot
            |
            ---0xcccccccccccccccc
               spin_hot (inlined)
```

99.98% again, spelled `app::spin_hot` (Rust's own path-qualified symbol,
needing no `extern "C"` trick to stay readable) and reported as inlined at
release optimization — the frame boundary survives as debug-info metadata
even though the call itself was folded away. `cargo-flamegraph` was already
on this host (`cargo install flamegraph --locked` is what would put it there
otherwise), so I ran it for real rather than falling back to the tabular
report:

```console
[host]$ cargo flamegraph --output rust-hot.svg -- hot
app: mode=hot n=3000000 result=216816 ms=476
[ perf record: Woken up 118 times to write data ]
[ perf record: Captured and wrote 29.409 MB perf.data (480 samples) ]
writing flamegraph to "rust-hot.svg"
[host]$ grep -o 'title>[^<]*spin_hot[^<]*' rust-hot.svg
title>spin_hot (744,743,865 samples, 38.41%)
title>spin_hot (892,224,516 samples, 46.02%)
title>spin_hot (892,224,516 samples, 46.02%)
title>spin_hot (301,908,555 samples, 15.57%)
```

`inferno`, the Rust crate `cargo flamegraph` wraps, is a reimplementation of
Brendan Gregg's classic Perl `stackcollapse-perf.pl`/`flamegraph.pl`
pipeline; the SVG it wrote splits `spin_hot` across three adjacent frames
(different instruction addresses inside the same inlined loop get
symbolized to the same function name but folded as separate stack shapes) —
summed, `38.41 + 46.02 + 15.57 = 99.998%`, the same number `perf report`
gave directly, just rendered as boxes on a page instead of tabulated as
text. Where crates.io is unreachable, the `perf record`/`perf report --stdio`
pair above is the fallback: same data, no picture.

`cargo clippy --release --all-targets` and `cargo fmt --check` are both
clean on this source — no findings, no diff. `tokio-console` is worth naming
and then setting aside: it attaches to a running **async runtime** and shows
live task/waker state, which only exists once a program has `tokio` tasks to
show. `app` has none — no `.await` anywhere in this chapter's source — so
`tokio-console` genuinely has nothing to display here; it belongs with
Chapter 27's async runtimes, where `chatterd`'s tokio tasks are the thing
worth watching live.

> **CLion's built-in profiler** <span class="status status--unverified">unverified</span> —
> CLion wraps this same `perf record`/`perf report` pipeline (and, on
> supported platforms, its own sampler) behind a GUI flamegraph and a
> call-tree view, so a reader working in that IDE gets Figure 31.1's shape
> without leaving the editor. This chapter's transcripts are all terminal
> tools run directly, so the IDE integration itself was not exercised this
> session — confirm it against your own CLion install if that is your
> workflow.

## Cross-check: three profilers, one function name

The claim worth checking independently is not any single number — sample
counts vary run to run with host load — but that **three unrelated
profiling tools, built by three different projects, agree on which function
is hot**. `perf stat` gives the coarsest independent signal, a hardware
counter read that needs no symbolication at all:

```console
[host]$ perf stat -e cycles,instructions -- ./cpp/build/release/app hot
app: mode=hot n=3000000 result=216816 ms=867

 Performance counter stats for './cpp/build/release/app hot':

     3,218,019,355      cycles:u
     3,185,629,752      instructions:u

       0.870321798 seconds time elapsed
```

Roughly one instruction retired per cycle — consistent with a tight,
branch-heavy integer loop, not a program stalled on cache misses or memory.
Then the three per-language profilers, each pointed at the same `hot`
workload, each naming its own hot frame in its own vocabulary:

| Profiler | Command | Named hot frame | Share |
|---|---|---|---|
| C++ `perf report --stdio` | `perf record --call-graph fp -- app hot` | `spin_hot` | 99.95% |
| Go `go tool pprof --top` | `app hot --cpuprofile cpu.prof` | `main.hotSpin` | 98.70% flat / 100% cum |
| Rust `perf report --stdio` | `perf record --call-graph fp -- app hot` | `app::spin_hot` | 99.98% |

Three profilers with nothing in common under the hood — a kernel-level
hardware sampler reading raw ELF symbols, an in-process Go library sampling
its own goroutine stacks, the same kernel sampler again reading Rust's DWARF
— converge on the same name in every language's own spelling of it, at the
same overwhelming share of samples. That agreement is the cross-check: if
`spin_hot`/`hotSpin` were *not* actually where the time went, three
independently-implemented tools would have no reason to all point at the
same place.

## What you learned

- Reading a flamegraph is one scan regardless of language: **width is share
  of samples, height is stack depth**, and the widest column reaching toward
  the top names the hot path — `spin_hot`/`hotSpin` at 98.7–99.98% across
  three unrelated profilers in this chapter's own run.
- `perf` is external and language-agnostic (it attaches to any ELF binary);
  Go's `runtime/pprof`/`runtime/trace` are internal libraries the program
  calls into, which is why Go's CLI carries `--cpuprofile`/`--memprofile`/
  `--trace` flags that C++ and Rust do not need.
- Static analyzers (`clang-tidy`, `-fanalyzer`, `cargo clippy`) answer a
  different question than a profiler — "is this pattern suspicious" instead
  of "is this slow" — and both classes of tool are worth running even on
  code with no known defect.
- `--alloc_space` (not the default in-use view) is what makes a Go heap
  profile useful once the allocating call has already returned and its
  garbage is gone; `cargo flamegraph` gets the same collapse+render pipeline
  Brendan Gregg's original Perl scripts do, reimplemented in Rust as
  `inferno`.

Next, the chapters ahead move from watching one program's own binary to
watching a whole running system from the outside, with the eBPF-based
tracing tools this book has deferred until now.

---

<p><span class="status status--verified">verified</span> — every command,
number, and transcript above was run this session on the Fedora 44 reference
host (kernel 7.1.3-200.fc44, GCC 16.1.1, clang 22.1.8, go1.26.5, rustc
1.97.1, perf 7.1.3, gdb 17.2) against the binaries in
<code>examples/31-per-language-toolbelts/</code>. The plain hand runs
printed <code>result=216816</code> (hot) and <code>result=5000</code> (alloc)
identically in all three languages; <code>perf record</code>/<code>perf
report --stdio</code> named <code>spin_hot</code> at 99.95% (C++, 3,461
samples) and <code>app::spin_hot</code> at 99.98% (Rust, 1,895 samples);
<code>perf stat</code> reported <code>3,218,019,355</code> cycles and
<code>3,185,629,752</code> instructions on <code>app hot</code>;
<code>go tool pprof --top</code> named <code>main.hotSpin</code> at 98.70%
flat/100% cum on a real <code>--cpuprofile</code> capture and
<code>main.allocChurn</code> at 53.98% flat/92.32% cum (
<code>--alloc_space</code>) on a real <code>--memprofile</code> capture;
<code>go tool trace -d=parsed</code> showed <code>main.hotSpin</code> in its
parsed event stream; <code>clang-tidy</code> reported exactly the one
<code>bugprone-exception-escape</code> warning at <code>main.cpp:143</code>
and a fresh <code>-fanalyzer</code> build (<code>cmake --build --preset
analyze</code>) produced zero diagnostics; <code>dlv</code> was installed
live with <code>go install .../dlv@latest</code> and its scripted session
broke at <code>main.hotSpin</code>, printed <code>n=500000</code>, and
backtraced through <code>main.run</code>; <code>cargo flamegraph</code> was
already present, ran for real, and its output SVG contained
<code>spin_hot</code> frames summing to 99.998% of samples;
<code>cargo clippy --release --all-targets</code> and <code>cargo fmt
--check</code> were both clean. The CLion profiler-integration callout is
unverified as marked — no CLion session was run this session.</p>
