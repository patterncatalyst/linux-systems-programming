# 49 — concurrency vs parallelism

The framing example for Part 14, the C++ concurrency compendium. It runs **one
workload under four execution models** and reports observables that describe
*structure* rather than elapsed time:

```
conc report: model=concurrent workers=16 cpus_allowed=1 distinct_cpus=1 max_inflight=16 interleaves=19 consensus=yes digest=0x481984990deee5ff
```

Concurrency is **structure** — how many tasks are in flight. Parallelism is
**execution** — how many CPUs are occupied. They are independent axes, which is
why all four cells below exist.

| `--model` | workers | what it asks the machine for | reading |
| --- | --- | --- | --- |
| `sequential` | 4 | one task at a time, on this thread, no threads spawned | neither concurrent nor parallel |
| `concurrent` | one per CPU | N runnable threads, pinned to CPU 0 by `sched_setaffinity(2)` | **concurrent, not parallel** |
| `parallel` | one per CPU | N threads, unpinned — tasks need not share a CPU | parallel, minimal task sharing |
| `both` | two per CPU | 2N threads, unpinned — oversubscribed | **concurrent and parallel** |

## The observables

Nothing here is a duration. Wall-clock numbers are not reproducible across
hosts or runs and this book does not gate on them (see ch39); every field below
is a count or a set cardinality, which are.

| field | how it is obtained | what it means |
| --- | --- | --- |
| `cpus_allowed` | `CPU_COUNT` over `sched_getaffinity(2)` | a **permission** — how many CPUs the process may use |
| `distinct_cpus` | popcount of a bitmap unioned from `sched_getcpu()` at every sample point | an **outcome** — how many CPUs workers were actually seen on |
| `max_inflight` | high-water mark of the live-worker counter | tasks in flight |
| `interleaves` | gaps in each worker's run of monotonic tickets | observed handoffs between workers |
| `digest` | FNV-1a over the 16 bytes `The quick brown.` | the answer, which must not depend on the model |
| `consensus` | every worker derives the digest independently and compares | `no` means the workers disagreed |

`cpus_allowed` versus `distinct_cpus` is the distinction the whole chapter turns
on: being permitted 16 CPUs is not the same as occupying them.

The digest is `0x481984990deee5ff` — deliberately the same value ch46's C++,
ch47's Go, and ch48's Rust toolbox land on, over the same payload bytes.

## Layout

```
49-concurrency-vs-parallelism/
├── demo.sh            # dispatcher (C++ only)
├── verify.lua         # gates A-F hard, G skip-if-absent
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/
        ├── conc.cpp                   # the four-model demo (C++23)
        ├── contracts/contracts_demo.cpp   # C++26 Contracts, four semantics
        └── hardening/oob.cpp              # built twice: checked and unchecked
```

This is a **C++-only example** — the first in the book. A Go or Rust variant
would answer a different question: each of those languages has one blessed
concurrency model, where the whole point here is that C++ offers several and
you have to choose. `verify.lua` skips for any `LSP_LANG` other than `cpp`.

Stdlib and Linux scheduler syscalls only. **No Boost, no Conan, no network** —
ch50 onward add the libraries.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run --model sequential
[host]$ ./demo.sh cpp run --model concurrent
[host]$ ./demo.sh cpp run --model parallel
[host]$ ./demo.sh cpp run --model both
```

Bare `./demo.sh` builds and runs `--model both`. `TARGET=<vm>` deploys the
binary to a lab VM via `scripts/lab/deploy-to-vm.sh`, though nothing here needs
one — the demo runs locally and unprivileged.

The two side programs are separate binaries so a trapping run can never take
the four-model demo with it:

```
[host]$ ./cpp/build/release/contracts_{ignore,observe,enforce,quick_enforce}
[host]$ ./cpp/build/release/oob_hardened     # traps, names the predicate
[host]$ ./cpp/build/release/oob_unchecked    # undefined behavior; never asserted on
```

## Verification

```
[host]$ LSP_LANG=cpp lua verify.lua
```

Hard gates, all offline:

- **A** — all four models produce the identical digest and `consensus=yes`.
- **B** — `parallel` occupied more than one CPU. This is the one
  scheduler-dependent assertion, so it retries a bounded number of times: "at
  least one of K runs saw more than one CPU" is a real effect, where a single
  run on a saturated host could legitimately see one.
- **C** — `concurrent` reports `cpus_allowed=1` *and* `distinct_cpus=1` *and*
  `max_inflight > 1` *and* `interleaves > 0`. No single one of those four facts
  proves the case alone.
- **D** — `sequential` reports `max_inflight=1` and `interleaves=0`; `both`
  reports many CPUs, real interleaving, and `max_inflight > cpus_allowed`.
- **E** — C++26 Contracts (P2900) on GCC 16: one source, four binaries, one per
  `-fcontract-evaluation-semantic`, asserting four *distinct observable
  behaviors* (diagnostic present or absent, crossed with terminating or not) —
  never four exit codes.
- **F** — safety hardening: the hardened build traps and names the failed
  predicate `__n < this->size()`. The unchecked build's runtime behavior is
  undefined and is never asserted on; instead `nm` asserts **statically** that
  it contains no `__glibcxx_assert_fail` at all.

**G** (clang parity — a clang-built `conc` must produce the same digest) runs
if `clang++` is on `PATH` and prints an informational `SKIP` otherwise.

Not gated: `std::execution` senders (P2300) and static reflection (P2996).
Measured on the reference host, neither exists — `__cpp_lib_senders` is
undefined and GCC 16's `<execution>` is still the C++17 parallel-algorithms
header, while `__cpp_impl_reflection` is undefined in GCC 16 and clang 22
rejects `-freflection`. The chapter covers both as clearly-labeled
forward-looking prose.

## Note on measuring the scheduler

The workers burn CPU in a loop that hashes its own previous result, and that
result is consumed through an atomic `sink`. Neither is decoration. Measured at
`-O2` on this host, for `--model concurrent`, three runs each:

| burn-loop body | `sink` | g++ 16.1.1 | clang++ 22.1.8 |
| --- | --- | --- | --- |
| serial chain | present | `max_inflight=16` | `max_inflight=16` ← shipped |
| serial chain | absent | `max_inflight=16` | `max_inflight=1` |
| loop-invariant | present | `max_inflight=1–2` | `max_inflight=1` |
| loop-invariant | absent | `max_inflight=1` | `max_inflight=1` |

Drop either one and at least one compiler optimizes the workload away — workers
finish in microseconds, never overlap, and `concurrent` reports
`max_inflight=1 interleaves=0`, the signature of a sequential run, whichever
model was asked for. An earlier version of this example measured the optimizer
instead of the scheduler.
