# 31-per-language-toolbelts — toolbelt

Chapter 31 closes the debugging arc by putting each language's *native*
profiling toolbelt to work on the same program. There is nothing wrong with
this code — the point is not to find a defect, but to point a real profiler
at a real binary and watch it name the function that is actually costing the
cycles or the bytes.

```
app <hot|alloc> [--n N]
```

- `hot` — counts primes in `[2, n)` by unoptimized trial division (every
  candidate divisor up to `sqrt(i)`). One function, `spin_hot` /
  `hotSpin`, does all the work — it is the whole point of a CPU profile.
- `alloc` — builds a 1000-key string index from `n` iterations of
  round-robin overwrite, each iteration allocating a fresh key and value
  string. One function, `alloc_churn` / `allocChurn`, is the only
  allocation site — it is the whole point of an allocation profile.
- `--n N` overrides the default size (`hot`: 3,000,000; `alloc`: 200,000);
  must be a positive integer.

## Behavior (identical across C++, Go, and Rust)

- On success, exactly one stdout line:
  `app: mode=<hot|alloc> n=<n> result=<r> ms=<t>`
  where `result` is the prime count (`hot`) or the summed byte length of the
  final 1000-entry index (`alloc`), and `ms` is the wall time of the
  workload. The algorithm is identical byte-for-byte across languages, so
  `result` is too: `app hot` always answers `216816` at the default size,
  `app alloc` always answers `5000`.
- Bad usage (missing/unknown mode, non-positive `--n`, unknown flag) prints
  a diagnostic plus `usage: app <hot|alloc> [--n N]` on stderr and exits 2.
- Go's binary additionally accepts `--cpuprofile FILE`, `--memprofile FILE`,
  and `--trace FILE` — see "Why Go's CLI has extra flags" below. Its usage
  banner lists them; C++ and Rust do not accept them (unknown-argument
  errors bad-usage the same as any other unrecognized flag).

### Why Go's CLI has extra flags

`perf` attaches to *any* ELF binary from the outside — nothing in the C++ or
Rust source needs to know it is being profiled. Go's CPU and heap profilers
are different: `runtime/pprof` and `runtime/trace` are libraries the program
calls into, so writing a profile is part of the binary's own behavior. That
asymmetry is real and is the reason the usage banners differ even though the
`<hot|alloc> [--n N]` contract is identical.

## Try it

```console
$ ./demo.sh cpp run hot
app: mode=hot n=3000000 result=216816 ms=745
$ ./demo.sh go run hot
app: mode=hot n=3000000 result=216816 ms=799
$ ./demo.sh rust run hot
app: mode=hot n=3000000 result=216816 ms=458
$ ./demo.sh cpp run alloc
app: mode=alloc n=200000 result=5000 ms=21
$ ./demo.sh cpp run bogus
app: unknown mode: bogus
usage: app <hot|alloc> [--n N]
```

(Timings vary by host and load; `result` does not — same algorithm, same
input, same answer, in all three languages.)

## The toolbelts, run for real

Every command below was actually run against the binaries in this directory
(`cpp/build/release/app`, `go/bin/app`, `rust/target/release/app`) on this
host (Fedora 44, GCC 16.1.1, clang 22.1.8, go1.26.5, rustc 1.97.1, perf
7.1.3, gdb 17.2). Numbers are host- and load-dependent; the shapes and the
named functions are the point.

### C++: perf record/report, clang-tidy, -fanalyzer

`spin_hot` has C linkage on purpose (`extern "C"`), so perf and gdb print it
as the plain symbol `spin_hot` instead of an Itanium-mangled name.

```console
$ perf stat -e cycles,instructions -- ./cpp/build/release/app hot
app: mode=hot n=3000000 result=216816 ms=745

 Performance counter stats for './cpp/build/release/app hot':

     3,212,149,799      cycles:u
     3,185,629,622      instructions:u

       0.747341237 seconds time elapsed
```

```console
$ perf record -o cpp.perf.data --call-graph fp -- ./cpp/build/release/app hot
[ perf record: Captured and wrote 0.191 MB cpp.perf.data (3093 samples) ]
$ perf report -i cpp.perf.data --stdio
# Samples: 3K of event 'cpu/cycles/Pu'
# Event count (approx.): 3220157668
#
# Children      Self  Command  Shared Object         Symbol
# ........  ........  .......  ....................  ................
#
    99.93%    99.93%  app      app                   [.] spin_hot
            |
            ---spin_hot
```

**Flamegraph note**: the classic Brendan Gregg `stackcollapse-perf.pl` /
`flamegraph.pl` scripts are not installed on this host (no `FlameGraph/` tree
on `PATH` or under any locatable directory), so per the fallback this chapter
documents, the C++ step stops at `perf report --stdio` above rather than
producing an SVG. (Compare the Rust section, where `cargo flamegraph` — a
Rust-native reimplementation of the same collapse+render pipeline — *is*
installable and produces a real SVG.)

`clang-tidy` (bugprone-*, modernize-*, performance-*, readability-*):

```console
$ clang-tidy -p cpp/build/release cpp/src/main.cpp
main.cpp:143:5: warning: an exception may be thrown in function 'main' which
  should not throw exceptions [bugprone-exception-escape]
    (traced to std::format's format_error, reachable only if the format
     string itself were malformed — it is a compile-time string literal)
```

One warning, and it is a known shape with `std::format`/`std::println`:
`bugprone-exception-escape` traces a theoretical `std::format_error` path
that only fires on a malformed format string — ours are literals, so the
path is unreachable in practice, but the checker cannot prove that
statically. Nothing else fires.

GCC's `-fanalyzer` (its own preset: `cmake --build --preset analyze`) is
clean — zero diagnostics on this source.

### Go: pprof (CPU + heap), go tool trace, delve

```console
$ ./go/bin/app hot --cpuprofile cpu.prof
app: mode=hot n=3000000 result=216816 ms=848
$ go tool pprof --top --nodecount=5 ./go/bin/app cpu.prof
      flat  flat%   sum%        cum   cum%
     840ms 98.82% 98.82%      840ms 98.82%  main.hotSpin
      10ms  1.18%   100%       10ms  1.18%  runtime.futex
         0     0%   100%      840ms 98.82%  main.main
```

```console
$ ./go/bin/app alloc --memprofile mem.prof
app: mode=alloc n=200000 result=5000 ms=30
$ go tool pprof --top --cum --alloc_space --nodecount=6 ./go/bin/app mem.prof
      flat  flat%   sum%        cum   cum%
 2580.29kB 55.74% 55.74%  4116.40kB 88.92%  main.allocChurn
         0     0% 55.74%  4116.40kB 88.92%  main.main
  512.01kB 11.06% 66.80%  1536.11kB 33.18%  fmt.Sprintf
```

`--alloc_space` is required for the heap check: by the time
`pprof.WriteHeapProfile` runs, the churned map from the just-finished
`allocChurn` call is already garbage (it went out of scope when the
function returned), so the default *in-use* view would show almost nothing.
`alloc_space` is pprof's cumulative-since-start counter — it remembers
everything `mallocgc` ever handed out, freed or not, which is the view that
actually answers "where did the allocations happen".

`go tool trace` opens an interactive, browser-based UI by default, which
does not script well — but `-d=parsed` dumps the same event stream as text,
which is enough to confirm the tracer captured `hotSpin`'s execution:

```console
$ ./go/bin/app hot --n 800000 --trace trace.out
$ go tool trace -d=parsed trace.out | grep -B2 hotSpin | head -3
	runtime.asyncPreempt @ 0x484d8a
		/usr/lib/golang/src/runtime/preempt_amd64.s:124
	main.hotSpin @ 0x4d272c
```

Delve, scripted non-interactively (no TUI) with a piped command script:

```console
$ echo -e "break main.hotSpin\ncontinue\nprint n\nbt\ncontinue\nquit" \
    | dlv --allow-non-terminal-interactive=true exec ./go/bin/app -- hot --n 500000
(dlv) Breakpoint 1 set at 0x4d26e1 for main.hotSpin() ./go/main.go:100
(dlv) > [Breakpoint 1] main.hotSpin() ./go/main.go:100 (hits goroutine(1):1 total:1)
(dlv) 500000
(dlv) 0  0x00000000004d26e1 in main.hotSpin
   at ./go/main.go:100
1  0x00000000004d2f95 in main.run
   at ./go/main.go:175
```

(`dlv` is not preinstalled on this host; `go install
github.com/go-delve/delve/cmd/dlv@latest` pulls it from the Go module proxy
into `$(go env GOPATH)/bin`. Where that install is unavailable — no module
proxy reachable — the pprof and trace steps above still stand on their own;
delve is demonstrated here, not gated on by `verify.lua`.)

### Rust: perf record/report, cargo flamegraph, clippy

```console
$ perf record -o rust.perf.data --call-graph fp -- ./rust/target/release/app hot
[ perf record: Captured and wrote 0.124 MB rust.perf.data (1783 samples) ]
$ perf report -i rust.perf.data --stdio
# Samples: 1K of event 'cpu/cycles/Pu'
    99.99%     0.00%  app      [unknown]             [.] 0xcccccccccccccccc
            |
            ---0xcccccccccccccccc
               spin_hot (inlined)
```

`cargo-flamegraph` is not preinstalled, but *is* installable —
`cargo install flamegraph --locked` pulls it (and the `inferno` crate it
wraps, a Rust reimplementation of the classic Perl collapse+render scripts)
from crates.io in well under a minute:

```console
$ cargo flamegraph --output rust-hot.svg -- hot
[ perf record: Captured and wrote 28.245 MB perf.data (461 samples) ]
writing flamegraph to "rust-hot.svg"
$ grep -c spin_hot rust-hot.svg
1
```

The resulting SVG contains a `spin_hot` frame, confirming the same story
`perf report` tells above, rendered instead of tabulated. Where network
access to crates.io is unavailable, the `perf record`/`perf report --stdio`
pair above is the fallback path — same data, no plot.

`tokio-console`: not applicable here — this program has no async runtime
(no `tokio`, no `.await`), so there are no tasks for `tokio-console` to
show. It belongs to the async chapter (27), not this one.

`cargo clippy --release --all-targets` and `cargo fmt --check` are both
clean — no findings.

## Layout

```
31-per-language-toolbelts/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI
├── cpp/         # CMake preset build (release/debug/asan/analyze/*-clang)
├── go/          # go build, demo.sh
└── rust/        # cargo build, demo.sh
```

The C++ `analyze` preset (`cmake --build --preset analyze`) builds with
`-fanalyzer` instead of running it as a separate step — it is just another
`CMakeLists.txt`-driven build, same as `asan`.

## Demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (builds first if needed)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

The top-level `demo.sh` dispatches: `./demo.sh go run hot --n 500` execs
`go/demo.sh run hot --n 500`. The binary is always named `app`. The
profiler invocations above run directly against the built binary paths
(`cpp/build/release/app`, `go/bin/app`, `rust/target/release/app`) rather
than through `demo.sh`, since wrapping `perf`/`pprof`/`dlv` around the demo
dispatcher is not part of the book-wide demo contract.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) skips (exit 77) if the
binary is not yet built or if `perf` is not on `PATH`; otherwise it asserts,
against the real built binary:

- Bad usage (missing mode, unknown mode, non-numeric `--n`, unknown flag)
  exits 2 with the expected diagnostic and usage banner.
- `hot` and `alloc` at default size and at small sizes (`--n 10` /
  `--n 7`) exit 0 with the exact report line, and `result` matches the
  value the same algorithm produces in every language (`216816` / `5000`
  at the defaults; `4` / `7` at the small sizes).
- `perf stat -e cycles,instructions` on `app hot` exits 0 and its output
  parses two numeric counters — this step is identical for all three
  languages, since perf attaches to any ELF binary.
- **C++ and Rust**: `perf record` then `perf report --stdio` exits 0 and
  the report names `spin_hot`.
- **Go**: a CPU profile (`--cpuprofile`) is written and `go tool pprof
  --top` names `main.hotSpin`; a heap profile (`--memprofile`) is written
  and `go tool pprof --top --cum --alloc_space` names `main.allocChurn`.

`go tool trace`, `delve`, `cargo flamegraph`, `clang-tidy`, and
`-fanalyzer` are demonstrated above with real captured output but are not
asserted by `verify.lua` — they either open an interactive UI
(`go tool trace`'s default mode), depend on a piped TTY session
(`delve`), or depend on a tool this session had to install over the
network (`dlv`, `cargo-flamegraph`) that may not be present in every
environment. Gating CI on them would trade a reproducible pass/fail for a
flaky one; the chapter text, not `verify.lua`, carries that evidence.

## Runner

| Step | C++ | Go | Rust |
|---|---|---|---|
| Build | `cmake --preset release` + ninja | `go build` | `cargo build --release` |
| CPU profile | `perf record` / `perf report --stdio` | `runtime/pprof` + `go tool pprof --top` | `perf record` / `perf report --stdio`, or `cargo flamegraph` |
| Allocation profile | *(not asserted; `alloc` mode exits 0)* | `runtime/pprof` heap + `go tool pprof --top --cum --alloc_space` | *(not asserted; `alloc` mode exits 0)* |
| Execution trace | — | `go tool trace -d=parsed` | — |
| Interactive debugger | `gdb` (chapter 28) | `dlv` | `gdb`/`rust-gdb` (chapter 28) |
| Static analysis | `clang-tidy`, `-fanalyzer` | `go vet` | `cargo clippy` |
| Result asserted by `verify.lua` | `spin_hot` named in `perf report --stdio` | `main.hotSpin`/`main.allocChurn` named in `go tool pprof --top` | `spin_hot` named in `perf report --stdio` |

## Mapping to a chapter

This example backs chapter 31 (per-language toolbelts), the closing chapter
of the debugging arc that opened with chapter 28 (gdb and remote debugging)
and chapter 29 (valgrind, sanitizers, miri). Where those chapters aim a
generic tool at a specific defect, this one aims each language's own
profiler at a workload built to have exactly one obvious answer.
