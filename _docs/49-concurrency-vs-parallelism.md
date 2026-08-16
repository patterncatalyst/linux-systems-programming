---
title: "Concurrency vs parallelism: one workload, four execution models, and observables that are counts rather than clocks"
order: 49
part: "Compendium: C++ Concurrency"
description: "conc runs one identical workload under four execution models -- sequential, concurrent (pinned to one CPU by sched_setaffinity), parallel (one thread per CPU), and both (2x oversubscribed) -- and reports structure rather than time: cpus_allowed from sched_getaffinity as a permission, distinct_cpus unioned from sched_getcpu as an outcome, a max_inflight high-water mark, and interleaves counted from gaps in a monotonic ticket sequence. All four converge on digest=0x481984990deee5ff, the same value ch46's C++, ch47's Go, and ch48's Rust toolbox produce. Verified on the Fedora 44 host: verify.lua PASS 43/FAIL 0, concurrent reporting cpus_allowed=1 distinct_cpus=1 max_inflight=16 interleaves=16 while parallel reports distinct_cpus=16, C++26 Contracts built four times for four distinct behaviors under GCC 16, and a hardened build trapping on __n < this->size() where the unchecked build is asserted statically to contain no bounds check at all."
duration: "55 minutes"
---

Two words get used as if they were one, and the confusion is expensive. A
program is **concurrent** when it is structured to have several tasks in
flight; it is **parallel** when the machine is actually running several at
once. Those are different claims about different things — one about your
code, one about the hardware underneath it — and the reason the distinction
survives being restated in every book on the subject is that the four
combinations are all real. A program can be neither. It can be concurrent
without ever being parallel. It can be parallel with barely any concurrency
in its structure. It can be both.

This chapter opens Part 14, a C++-only compendium that will take one workload
through every concurrency model the language offers — pthreads, `std::thread`
and friends, Boost.Thread, C++20 coroutines, Boost.Fiber, Boost.Asio — and
end in a measured comparison of all six. Before any of that, the vocabulary
has to be nailed down, and nailed down the way this book nails anything down:
by writing a program that produces the distinction as an observable, and then
asserting on it. `conc` runs *one* workload under four execution models and
reports what the machine actually did.

{% include excalidraw.html
   file="49-concurrency-vs-parallelism"
   alt="A 2x2 grid built from two horizontal bands. The upper band, not parallel with one CPU occupied, holds the sequential cell -- one task in flight on one CPU, max_inflight=1, interleaves=0, neither concurrent nor parallel -- and the amber concurrent cell: 16 tasks in flight pinned to CPU 0, cpus_allowed=1, distinct_cpus=1, max_inflight=16, interleaves=19, labelled CONCURRENT, NOT PARALLEL. A side note explains sched_setaffinity with CPU_SET(0) removes parallelism by request rather than by accident. The lower band, parallel with many CPUs occupied, holds the parallel cell -- 16 tasks on 16 CPUs unpinned, distinct_cpus=16, max_inflight=16, one task per CPU -- and the amber both cell: 32 tasks oversubscribed onto 16 CPUs, distinct_cpus=16, interleaves=7557, max_inflight=25 greater than cpus_allowed=16, labelled CONCURRENT AND PARALLEL. A second side note explains sched_getcpu is sampled per worker and unioned into a bitmap, so distinct_cpus is an outcome while cpus_allowed is a permission. All four cells converge on an amber box reading digest=0x481984990deee5ff, consensus=yes, the answer does not depend on the execution model."
   caption="Figure 49.1 — the two axes as a 2x2 grid: concurrency is structure (tasks in flight), parallelism is execution (CPUs occupied), and all four cells exist because the axes are independent" %}

> **Tools used** — `g++` and `cmake` (host; both gated by
> `scripts/check-host.sh` as `g++ >= 14` and `cmake >= 3.25`), `ninja` (host;
> gated), `clang++` (host; gated by `check-host.sh`, and used here for the
> parity gate G and for the optimizer experiment below), `nm` from binutils
> (host; gated as `nm (binutils)` — it is a real gate here, not a
> convenience), `lua` (host; gated as `lua >= 5.4`, runs `verify.lua`), and
> `python3` (host; gated, runs `scripts/test-all-examples.py`). No VM, no
> root, no LGTM stack, no network: `examples/49-concurrency-vs-parallelism`
> is `mode: local` in `examples/manifest.yaml` and builds from the standard
> library and Linux scheduler syscalls alone.

This example ships `langs: [cpp]` — the first C++-only example in the book
outside the Deep Dives and the toolbox appendices. That is a deliberate
choice rather than an omission. Go has goroutines and Rust has `async` plus
threads; each language has essentially one blessed answer, and Chapter 6
already put all three side by side. The whole premise of Part 14 is that C++
offers *several* answers and makes you pick, so every code block below is a
plain fenced `cpp` or `console` block with no `codetabs` include to reach
for.

## The two axes, and why they are independent

Concurrency is a property of a program's structure: how many units of work
are outstanding at once. You can have sixteen tasks in flight on a machine
with one CPU — they take turns, and the program is fully concurrent while
never once being parallel.

Parallelism is a property of an execution: how many CPUs are simultaneously
occupied by your work. You can saturate sixteen CPUs with a workload whose
structure has almost no concurrency in it, if the runtime splits one loop
across cores for you.

Because they are independent, the interesting question is never "is this
concurrent or parallel" but "which of the two am I looking at, and which one
did I actually ask for". `conc` answers that with four fields:

| field | how it is obtained | what it means |
| --- | --- | --- |
| `cpus_allowed` | `CPU_COUNT` over `sched_getaffinity(2)` | a **permission** — how many CPUs the process *may* use |
| `distinct_cpus` | popcount of a bitmap unioned from `sched_getcpu()` samples | an **outcome** — how many CPUs workers were *observed* on |
| `max_inflight` | high-water mark of a live-worker counter | tasks in flight — the concurrency axis |
| `interleaves` | gaps in each worker's run of monotonic tickets | observed handoffs between workers |

The `cpus_allowed` / `distinct_cpus` split is the one to internalize.
Chapter 34 already showed the same failure in a different costume: a
container whose cgroup `cpu.max` allowed it two CPUs, while
`hardware_concurrency()` cheerfully reported the host's count. A permission is what you are
allowed to do. An outcome is what happened. Only one of the two is evidence.

### Why none of these is a duration

Nothing in this chapter is timed, and that is not an oversight. Chapter 39
made the case at length: wall-clock numbers are not reproducible across
hosts, across runs on the same host, or across a machine that decided to
scale its clocks halfway through. A gate built on "the parallel run was
faster" would fail on a loaded CI box and pass on a bug.

Counts and set cardinalities are reproducible in the only sense that matters
for a gate — their *signs* are stable even when their magnitudes are not. On
this reference host, `interleaves` for the `concurrent` model has come back
as 16, 17, 18, 19, 20, and 41 across runs. Not one of those is a number worth
asserting on. `interleaves > 0`, on the other hand, has never once been false,
and it is exactly the claim the chapter needs.

## The four models

Each model runs the identical `worker()` function. They differ only in how
those workers are placed on the machine.

```cpp
    if (model == "sequential") {
        // One task at a time on one thread: neither concurrent nor parallel.
        workers = 4;
        run_sequential(obs, workers);
    } else if (model == "concurrent") {
        // N runnable threads, one CPU allowed: concurrent, NOT parallel.
        if (!pin_to_one_cpu()) {
            std::fprintf(stderr, "conc: sched_setaffinity failed\n");
            return 1;
        }
        workers = n;
        run_threaded(obs, workers);
    } else if (model == "parallel") {
        // One thread per CPU, unpinned: the tasks need not share a CPU.
        workers = n;
        run_threaded(obs, workers);
    } else if (model == "both") {
        // Twice as many threads as CPUs, unpinned: tasks share CPUs AND
        // occupy many of them at once.
        workers = 2 * n;
        run_threaded(obs, workers);
    } else {
```

`sequential` deliberately does not spawn threads at all — each worker runs to
completion on the calling thread before the next starts. That is what makes
its interleave count exactly zero rather than merely small:

```cpp
// The sequential model deliberately does NOT spawn threads: each worker runs
// to completion on this thread before the next one starts. That is what makes
// its interleaves count exactly zero -- one worker's tickets are never
// separated by another's.
void run_sequential(Observations& obs, int workers) {
    for (int i = 0; i < workers; ++i) {
        worker(obs, i);
    }
}
```

`concurrent` is the case the whole chapter exists for. It spawns one thread
per CPU and then takes the CPUs away:

```cpp
// Restrict this process to a single CPU. This is what turns "N runnable
// threads" into "N runnable threads that must take turns" -- concurrency
// with the parallelism removed, by request rather than by accident.
bool pin_to_one_cpu() {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
}
```

Sixteen runnable threads, one CPU. The structure is unchanged; the hardware
is gone. If concurrency and parallelism were the same thing, this
configuration would be impossible to write.

## How the code works

### Counting tasks in flight

`max_inflight` is a high-water mark maintained with a compare-exchange loop
rather than a lock, because a lock around the counter would itself serialize
the workers and perturb the thing being measured:

```cpp
void bump_max_inflight(Observations& obs, int now) {
    int seen = obs.max_inflight.load(std::memory_order_relaxed);
    while (now > seen && !obs.max_inflight.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        // compare_exchange_weak refreshes `seen` on failure; loop until either
        // we win the race or someone else already recorded a larger mark.
    }
}
```

`memory_order_relaxed` is correct throughout: these counters are observations
about the run, not synchronization between workers. Nothing reads them to
decide what to do next. Chapter 26 covered when relaxed is and is not enough;
this is the easy end of that spectrum.

### Counting interleaving without a clock

The interleave count is the subtle one. Every worker takes a monotonic ticket
at every sample point. If a worker's consecutive tickets are consecutive
integers, nobody else took one in between. A gap means some other worker was
scheduled between this worker's two samples — which is precisely the
operational definition of "in flight at the same time":

```cpp
        const std::uint64_t t = obs.ticket.fetch_add(1, std::memory_order_relaxed);
        if (have_prev && t != prev_ticket + 1) {
            obs.interleaves.fetch_add(1, std::memory_order_relaxed);
        }
        prev_ticket = t;
        have_prev = true;
```

No timestamps, no sleeps, no assumptions about scheduling quanta. The
sequential model produces contiguous ticket runs by construction and reports
exactly zero.

### Sampling the CPU rather than trusting it

```cpp
        // sched_getcpu() is a vDSO read of the CPU this thread is on right
        // now. It can change between this line and the next -- that is the
        // point. We union every value ever seen rather than trusting one.
        const int cpu = sched_getcpu();
        if (cpu >= 0 && cpu < 64) {
            obs.cpu_mask.fetch_or(std::uint64_t{1} << cpu, std::memory_order_relaxed);
        }
```

`sched_getcpu()` is a vDSO read, so it costs a function call rather than a
syscall — Chapter 4 covered why that matters when you are calling something
240 times per worker. Its value is stale the instant it returns. That is fine,
because the program never acts on a single reading; it unions every reading
ever taken into a bitmap and reports the popcount. A stale sample still
proves the thread was on that CPU at some point, which is all
`distinct_cpus` claims.

### The digest, and why every model computes it

Every worker independently hashes the same sixteen bytes and every model
prints the result:

```cpp
// ch46/ch47/ch48's exact 16-byte payload ("The quick brown."), reused
// byte-for-byte so this chapter lands on the same FNV-1a digest as the three
// toolbox appendices -- now carried into Part 14.
constexpr std::uint8_t kPayload[16] = {0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63,
                                       0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e};
```

That is the chapter's thesis in one field. The execution model is a property
of the machine, not of the answer. If a change to the model changes the
digest, the program has a bug — and `consensus` catches the case where
workers disagree among themselves rather than merely differing from the
expected constant.

## The section that nearly wasn't: measuring the optimizer instead of the scheduler

The first working version of this program reported `max_inflight=1
interleaves=0` for the `concurrent` model. Sixteen threads, and the report
claimed one task in flight. The threads were being created — the model was
correct. The workload had been deleted.

Each worker burns CPU between sample points so that it is a genuinely
runnable task the scheduler has to place somewhere. The original burn loop
hashed a *constant*:

```cpp
local = fnv1a(kPayload, sizeof kPayload) ^ (local >> 1);
```

`fnv1a(kPayload, 16)` does not depend on the loop variable, `local` was never
read afterwards, and the compiler is fully entitled to notice both facts.
Workers finished in microseconds and never overlapped, so every observable
reported a sequential run no matter which model was asked for.

The fix was two changes: make the loop body a real serial dependency chain,
and consume the result. The shipped version does both.

```cpp
        for (int b = 0; b < kBurnPerSample; ++b) {
            local = fnv1a(reinterpret_cast<const std::uint8_t*>(&local), sizeof local) + b;
        }
```

```cpp
    obs.sink.fetch_xor(local, std::memory_order_relaxed);
```

The interesting part is that neither change is sufficient alone, and you only
find that out by building all four combinations under both compilers. Run at
`-O2`, three runs each, reporting `max_inflight` for `--model concurrent`:

| burn-loop body | `sink` | g++ 16.1.1 | clang++ 22.1.8 |
| --- | --- | --- | --- |
| serial chain | present | 16, 16, 16 | 16, 16, 16 |
| serial chain | absent | 16, 16, 16 | 1, 1, 1 |
| loop-invariant | present | 1, 2, 2 | 1, 1, 1 |
| loop-invariant | absent | 1, 1, 1 | 1, 1, 1 |

Only the top row survives both compilers. Drop the sink and GCC still keeps
the chain while clang eliminates it; keep the sink but make the body
loop-invariant and both compilers hoist their way back to a near-empty
worker. A single-compiler check would have shipped the second row and called
it verified.

The general lesson generalizes past this example: **a benchmark or a
concurrency demo whose work has no observable consumer is not measuring what
you think.** Chapter 39 made this point about timing harnesses; it applies
just as sharply to a program whose output is a set of counters. This is also
the strongest argument for gate G, the clang parity check, being in the
harness at all — it was added to catch digest drift between compilers, and
it would have caught this.

## Errors, three ways

This example is C++-only, so the book's usual three-language comparison
collapses to one — but the three *kinds* of failure are all present, and they
are handled differently on purpose.

**A syscall that fails.** `sched_setaffinity` can fail (`EINVAL` for an empty
mask, `EPERM` under a restrictive policy). The `concurrent` model cannot mean
anything if the pin did not take, so it refuses to continue rather than
reporting a run that silently measured something else:

```cpp
        if (!pin_to_one_cpu()) {
            std::fprintf(stderr, "conc: sched_setaffinity failed\n");
            return 1;
        }
```

**A reading that is merely unavailable.** `sched_getcpu()` returns a negative
value on failure, and CPU ids above 63 do not fit the bitmap. Both are
skipped rather than fatal — a missed sample makes `distinct_cpus` an
undercount, and an undercount can only make the gates harder to pass, never
easier. Failing softly in the safe direction is the right call for a sample;
it would be the wrong call for the pin.

**A disagreement between workers.** If two workers derive different digests
from the same bytes, something is deeply wrong and no report is worth
printing. That becomes the process exit status:

```cpp
    return obs.consensus.load() ? 0 : 1;
```

The pattern across all three: fail hard where a wrong answer would be
indistinguishable from a right one, fail soft where the failure can only cost
precision, and never let either read as success.

## Concurrency lens

The observation machinery is itself concurrent, which makes it a small
worked example of the thing it measures.

Every counter in `Observations` is a `std::atomic` under
`memory_order_relaxed`, and there is not a mutex anywhere in the program.
That is a deliberate choice with a measurable consequence: a mutex around the
in-flight counter would serialize workers at exactly the sample points where
the program is trying to observe them overlapping, and the `concurrent`
model's interleave count would drop toward the sequential model's. The
instrument would change the reading.

Relaxed ordering is sufficient here for a reason worth stating precisely:
these atomics carry no data dependency to any other memory. No worker reads
`max_inflight` or `cpu_mask` to decide what to do next. They are write-mostly
observation state, read once after every thread has joined — and `join()` is
itself the synchronization edge that makes those final reads well-defined.
Chapter 26's acquire/release machinery is not needed because nothing here
publishes anything to anyone.

One place does need care: `bump_max_inflight` uses `compare_exchange_weak` in
a loop rather than a plain `fetch_max`-style read-modify-write, because the
weak form is permitted to fail spuriously and refreshes `seen` when it does.
Writing `if (now > obs.max_inflight.load()) obs.max_inflight.store(now)`
would look equivalent and would lose updates under exactly the contention
this program is built to create.

The `sink` deserves one more note in this lens. `fetch_xor` was chosen over
`fetch_add` so that the sink's final value does not depend on the order in
which workers finish. It is never asserted on, but a value that varies with
scheduling is a trap waiting for someone to assert on it later.

## Build, run, observe

Everything is local, unprivileged, and offline.

```
[host]$ cd examples/49-concurrency-vs-parallelism
[host]$ ./demo.sh cpp build
[host]$ for m in sequential concurrent parallel both; do ./demo.sh cpp run --model "$m"; done
```

On the sixteen-CPU reference host:

```
conc report: model=sequential workers=4 cpus_allowed=16 distinct_cpus=1 max_inflight=1 interleaves=0 consensus=yes digest=0x481984990deee5ff
conc report: model=concurrent workers=16 cpus_allowed=1 distinct_cpus=1 max_inflight=16 interleaves=16 consensus=yes digest=0x481984990deee5ff
conc report: model=parallel workers=16 cpus_allowed=16 distinct_cpus=16 max_inflight=16 interleaves=3816 consensus=yes digest=0x481984990deee5ff
conc report: model=both workers=32 cpus_allowed=16 distinct_cpus=16 max_inflight=23 interleaves=7639 consensus=yes digest=0x481984990deee5ff
```

Read the four lines as the four cells of Figure 49.1:

- **`sequential`** — `cpus_allowed=16` (permitted plenty) but
  `distinct_cpus=1`, `max_inflight=1`, `interleaves=0`. Neither axis. Note
  that it was *allowed* sixteen CPUs and used one; permission is not outcome.
- **`concurrent`** — `cpus_allowed=1` because the program asked for that,
  `distinct_cpus=1` confirming the request took effect, and yet
  `max_inflight=16` with `interleaves=16`. Sixteen tasks in flight, zero
  parallelism. This line is the chapter.
- **`parallel`** — `distinct_cpus=16` with `max_inflight=16`, one task per
  CPU. The interleave count jumps three orders of magnitude, which is worth
  understanding rather than glossing: ticket gaps register *any* overlap, and
  genuinely parallel workers interleave in the ticket sequence constantly.
  The metric measures overlap, not contention.
- **`both`** — thirty-two workers on sixteen CPUs. `distinct_cpus=16` and
  `max_inflight=23 > cpus_allowed=16`, which is the arithmetic proof of
  oversubscription: more tasks were simultaneously live than there are CPUs
  to run them on. Both axes at once.

The magnitudes move between runs — `max_inflight` for `both` has been
observed at 23, 24, and 25, and `interleaves` for `concurrent` between 16 and
41. The signs do not, and the signs are what `verify.lua` gates.

Then the verification harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: both was genuinely oversubscribed (max_inflight=24 > cpus_allowed=16)
ok: cpp: the hardened build traps on the out-of-bounds subscript
ok: cpp: the trap names the failed bounds predicate, not just 'aborted'
ok: cpp: the unchecked build contains no __glibcxx_assert_fail -- the check is compiled out
ok: cpp: the hardened build does contain __glibcxx_assert_fail
ok: cpp: clang-built conc runs
ok: cpp: clang and GCC agree on the digest (ch46's parity idea, one line)
info: std::execution senders (P2300) not gated -- __cpp_lib_senders is undefined on this toolchain; GCC 16's <execution> is the C++17 parallel-algorithms header
info: static reflection (P2996) not gated -- __cpp_impl_reflection undefined in GCC 16, and clang 22 rejects -freflection
PASS 43 / FAIL 0
```

Gate B — "`parallel` occupied more than one CPU" — is the only
scheduler-dependent assertion in the file, and it retries a bounded number of
times rather than trusting one run. A single run on a fully saturated host
could legitimately observe one CPU; "at least one of five runs saw more than
one" is a real effect. On this host the retry has never fired: eight
consecutive runs reported 14 to 16 distinct CPUs.

## C++26, measured rather than assumed

Part 14 carries a standing obligation: call out where C++26 changes the
picture, and never claim a feature runs without having run it. Two of the four features this book tracks are live on the
reference toolchain and two are not, and the split is measured.

{% include excalidraw.html
   file="49-execution-model-lanes"
   alt="Two lanes labelled C++26 in ch49, measured not assumed. The amber live lane, marked live on this toolchain GCC 16.1.1 hard-gated and verified offline, holds Contracts P2900 with -fcontracts and __cpp_contracts=202502, one source compiled into four binaries for ignore which is silent, observe which reports and runs on, enforce which reports and aborts, and quick_enforce which aborts with no diagnostic, summarized as four behaviors not four exit codes; and Safety hardening noting _GLIBCXX_ASSERTIONS on Fedora 44 is on by default at -O0 and off at -O2, that the hardened build traps naming the predicate __n less than this->size(), and that the unchecked build is asserted statically with no __glibcxx_assert_fail symbol, with a side note explaining the out-of-bounds read is undefined behavior so gating on what it prints would be unsound. The dashed forward-looking lane, marked not implemented here with no gate and no committed dead code, holds std::execution senders P2300 with __cpp_lib_senders undefined and GCC 16's execution header still the C++17 parallel-algorithms header at __cpp_lib_execution 201902, live comparison deferred to ch56; and static reflection P2996 with __cpp_impl_reflection undefined, no caret-caret-T operator in GCC 16, and clang 22 rejecting -freflection as an unknown argument. A closing rule for both lanes states support status is measured on this host and stated in the footer, never inferred from a standard's publication date."
   caption="Figure 49.2 — C++26 in this chapter, split by what was measured: Contracts and safety hardening run for real and are hard-gated; senders and static reflection are forward-looking prose with nothing executed" %}

### Contracts (P2900): four semantics, four behaviors

GCC 16 implements contracts behind `-fcontracts`, and `__cpp_contracts`
reports `202502`. The function under test is one line:

```cpp
int scaled(const int x) pre(x > 0) post(r: r > x) { return x * 2; }
```

`x` must be `const`. GCC rejects the postcondition otherwise, and the reason
is a genuine rule rather than an implementation quirk: a postcondition naming
a value parameter is talking about the value the *caller* passed, so the
function must not have been able to reassign it in the meantime. The compiler
teaching the model is the best kind of error message.

The point of P2900 is that whether a contract is checked is a build-time
policy, not a property of the code. The same source is compiled four times:

```cmake
  foreach(sem ignore observe enforce quick_enforce)
    add_executable(contracts_${sem} src/contracts/contracts_demo.cpp)
    target_compile_options(contracts_${sem} PRIVATE
      -std=c++26 -fcontracts -fcontract-evaluation-semantic=${sem})
```

`-fcontracts` is needed on the **link** line too, not just when compiling —
the `observe` and `enforce` semantics call into a violation handler that the
GCC driver only links in when the flag is present, and without it those two
targets fail with `undefined reference to
handle_contract_violation(std::contracts::contract_violation const&)`.

Four real runs, four distinct behaviors:

```
[host]$ ./cpp/build/release/contracts_ignore; echo "exit=$?"
contracts: scaled(21) = 42
contracts: violating the precondition now...
contracts: scaled(-1) = -2
contracts: still running after the violation
exit=0
```

```
[host]$ ./cpp/build/release/contracts_observe 2>&1; echo "exit=$?"
contracts: scaled(21) = 42
contracts: violating the precondition now...
contract violation in function int scaled(int) at .../contracts_demo.cpp:16: x > 0
[assertion_kind: pre, semantic: observe, mode: predicate_false, terminating: no]
contract violation in function int scaled(int) at .../contracts_demo.cpp:16: r > x
[assertion_kind: post, semantic: observe, mode: predicate_false, terminating: no]
contracts: scaled(-1) = -2
contracts: still running after the violation
exit=0
```

```
[host]$ ./cpp/build/release/contracts_enforce 2>&1; echo "exit=$?"
contracts: scaled(21) = 42
contracts: violating the precondition now...
contract violation in function int scaled(int) at .../contracts_demo.cpp:16: x > 0
[assertion_kind: pre, semantic: enforce, mode: predicate_false, terminating: yes]
terminate called without an active exception
exit=134
```

```
[host]$ ./cpp/build/release/contracts_quick_enforce 2>&1; echo "exit=$?"
contracts: scaled(21) = 42
contracts: violating the precondition now...
terminate called without an active exception
exit=134
```

Note what `observe` reveals that a precondition-only demo would hide: the
postcondition `r > x` is *also* violated, because `scaled(-1)` returns `-2`
and `-2 > -1` is false. One bad input trips both ends of the contract, and
the observe semantic is the only one that lets you see the second.

`enforce` and `quick_enforce` both exit 134, which is why the gate asserts on
behavior rather than status: the difference between them is that `enforce`
prints the diagnostic naming the failed predicate and `quick_enforce` prints
nothing at all. Two identical exit codes, two different semantics. Gating on
the number would have proved nothing.

### Safety hardening: on where you debug, off where you ship

`std::vector::operator[]` performs no bounds check in the standard — an
out-of-range subscript is undefined behavior, full stop. libstdc++ can add a
check when `_GLIBCXX_ASSERTIONS` is defined, and Fedora 44's `g++` defines it
for you. But only when it is not optimizing:

```
g++ -std=c++23            -> _GLIBCXX_ASSERTIONS defined     -> traps
g++ -std=c++23 -O2        -> _GLIBCXX_ASSERTIONS NOT defined -> no check
g++ -std=c++23 -O2 -D_GLIBCXX_ASSERTIONS -> defined          -> traps
```

The safety net is on in the build where you are already watching, and off in
the build you ship, unless you ask for it. `CMakeLists.txt` builds one
unchanged source twice to make that difference the observable:

```cmake
add_executable(oob_unchecked src/hardening/oob.cpp)

# oob_hardened: the same source with the assertions asked for explicitly.
# Traps at run time and names the failed predicate.
add_executable(oob_hardened src/hardening/oob.cpp)
target_compile_definitions(oob_hardened PRIVATE _GLIBCXX_ASSERTIONS)
```

```
[host]$ ./cpp/build/release/oob_hardened; echo "exit=$?"
hardening: size=3, reading v[7]...
/usr/include/c++/16/bits/stl_vector.h:1253: constexpr std::vector<_Tp, _Alloc>::reference std::vector<_Tp, _Alloc>::operator[](size_type) [with _Tp = int; _Alloc = std::allocator<int>; reference = int&; size_type = long unsigned int]: Assertion '__n < this->size()' failed.
exit=134
```

The unchecked build is where the verification discipline earns its keep. It
would be easy to run it, observe that it prints `hardening: v[7] = 0` and
exits 0, and gate on that — and it would be wrong, because that output is
undefined behavior. It is not a result. Tomorrow's compiler, allocator, or
stack layout may print something else or crash, and the gate would be
asserting that undefined behavior stays undefined in one particular way.

So the unchecked side is asserted **statically** instead:

```lua
local nm_unchecked = checks.run("nm -C cpp/build/release/oob_unchecked 2>/dev/null | grep -c glibcxx_assert_fail || true")
checks.expect_match(nm_unchecked.out, "^%s*0%s*$",
  "cpp: the unchecked build contains no __glibcxx_assert_fail -- the check is compiled out")
```

Zero occurrences of `__glibcxx_assert_fail` in the unchecked binary, one or
more in the hardened one. That is a fact about the compiled artifact, it is
true regardless of what the UB does at run time, and it is exactly the claim
being made — the check is absent.

### Not implemented here: senders and reflection

`std::execution` (P2300) and static reflection (P2996) are the other two
features this book tracks, and on this toolchain neither exists. That is
measured, not assumed: `__cpp_lib_senders` is undefined and GCC 16's
`<execution>` is still the C++17 parallel-algorithms header
(`__cpp_lib_execution = 201902`), while `__cpp_impl_reflection` is undefined
in GCC 16 and clang 22 rejects `-freflection` as an unknown argument.

`verify.lua` says so in its own output rather than staying quiet about it:

```
info: std::execution senders (P2300) not gated -- __cpp_lib_senders is undefined on this toolchain; GCC 16's <execution> is the C++17 parallel-algorithms header
info: static reflection (P2996) not gated -- __cpp_impl_reflection undefined in GCC 16, and clang 22 rejects -freflection
```

The sender/receiver model matters for this part of the book — it is the first
standard vocabulary for expressing "what work, on what execution resource"
as a *value* you can compose, which is precisely the concurrency-versus-
parallelism split expressed in the type system. It is scheduled for Chapter
56's comparison, and it goes in there if and only if the toolchain has caught
up by then. No committed dead code in the meantime.

## Cross-check: the same answer, four models and two compilers

Every model produced `digest=0x481984990deee5ff`, and so does a clang-built
binary:

```
[host]$ cd cpp && cmake --preset release-clang && cmake --build --preset release-clang --target conc
[host]$ ./build/release-clang/conc --model parallel
conc report: model=parallel workers=16 cpus_allowed=16 distinct_cpus=16 max_inflight=16 interleaves=3811 consensus=yes digest=0x481984990deee5ff
```

That constant should look familiar. It is the same value Chapter 46's C++
`toolbox`, Chapter 47's Go `toolbox`, and Chapter 48's Rust `toolbox` all
produce, over the same sixteen payload bytes. Three languages and now four
execution models under two compilers, one number.

The cross-check is not decoration. It is the concrete form of the claim the
chapter opened with: how work is scheduled onto a machine is orthogonal to
what the work computes. If restructuring a program from sequential to
parallel changes its answer, the restructuring introduced a bug — and that
is a property you can assert on in CI, unlike "it got faster".

## What you learned

- **Concurrency is structure, parallelism is execution.** How many tasks are
  in flight versus how many CPUs are occupied. All four combinations exist,
  which is why the words cannot be swapped.
- **`cpus_allowed` is a permission; `distinct_cpus` is an outcome.** Only
  the second is evidence. `sched_getaffinity` tells you what you are allowed;
  only sampling `sched_getcpu` tells you what happened.
- **`sched_setaffinity` lets you remove parallelism deliberately.** Pinning
  to one CPU turns N runnable threads into N threads that must take turns,
  which is how the "concurrent, not parallel" cell gets built on purpose
  rather than found by accident.
- **Count things, don't time them.** Ticket-sequence gaps, in-flight
  high-water marks, and bitmap popcounts are reproducible in sign even when
  their magnitudes drift. Durations are not, which is why nothing here is
  gated on one.
- **Work with no consumer is not work.** A burn loop whose result nobody
  reads, or whose body is loop-invariant, gets optimized away — and a
  concurrency demo then measures the optimizer. Checking under a second
  compiler is what turns that from a suspicion into a table.
- **Assert on behavior, not on exit codes.** `enforce` and `quick_enforce`
  both exit 134; the difference is whether a diagnostic was printed. Four
  contract semantics means four observable behaviors.
- **Never gate on the output of undefined behavior.** The unchecked build's
  `v[7]` is UB, so the gate asserts statically that the bounds check is
  absent from the binary rather than asserting on what the UB printed.
- **State toolchain support as measured, not as published.** Contracts and
  hardening run here; senders and reflection do not, on this compiler, today
  — and the footer says which is which.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, 16 logical / 8 physical
CPUs, 1 NUMA node, g++ 16.1.1 20260515, clang 22.1.8, CMake 4.3.0, Ninja
1.13.0, GNU nm 2.46.1, Lua 5.4.8; local, unprivileged, no network):
<code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. &amp;&amp; pwd) lua verify.lua</code>
reported <code>PASS 43 / FAIL 0</code> with gate G running for real rather
than skipping, and
<code>python3 scripts/test-all-examples.py --only
49-concurrency-vs-parallelism</code> reported <code>1 passed, 0 failed, 0
skipped</code>. Every <code>conc report:</code> line quoted above is a real
run from this session, as are all four contract transcripts, both
<code>oob</code> transcripts, and the clang parity line; run-to-run variation
in <code>interleaves</code> and <code>max_inflight</code> is stated in the
text rather than smoothed over, and no assertion in <code>verify.lua</code>
depends on a magnitude. Confirmed live: all four models produced
<code>digest=0x481984990deee5ff</code> with <code>consensus=yes</code>,
byte-identical to Chapter 46's C++, Chapter 47's Go, and Chapter 48's Rust
digest; <code>concurrent</code> reported <code>cpus_allowed=1
distinct_cpus=1 max_inflight=16 interleaves=16</code>; <code>parallel</code>
reported <code>distinct_cpus=16</code>; <code>both</code> reported
<code>max_inflight=23 &gt; cpus_allowed=16</code>; <code>sequential</code>
reported <code>max_inflight=1 interleaves=0</code>; the four contract
binaries produced four distinct behaviors (<code>ignore</code> silent and
exit 0, <code>observe</code> reporting both the <code>pre</code> and the
<code>post</code> violation with <code>terminating: no</code> and exit 0,
<code>enforce</code> reporting with <code>terminating: yes</code> and exit
134, <code>quick_enforce</code> exit 134 with no contract diagnostic);
<code>oob_hardened</code> aborted with <code>Assertion '__n &lt;
this-&gt;size()' failed</code> while <code>nm -C</code> found
<code>__glibcxx_assert_fail</code> once in that binary and zero times in
<code>oob_unchecked</code>. The optimizer table in the dead-code section is a
real 2x2 matrix built this session: four source variants compiled with both
<code>g++ -std=c++23 -O2</code> and <code>clang++ -std=c++23 -O2</code> and
run three times each under <code>--model concurrent</code>. Not exercised:
<span class="status status--unverified">unverified</span> —
<code>std::execution</code> senders (P2300) and static reflection (P2996) are
not implemented on this toolchain (<code>__cpp_lib_senders</code> and
<code>__cpp_impl_reflection</code> both undefined; GCC 16's
<code>&lt;execution&gt;</code> reports <code>__cpp_lib_execution =
201902</code>, the C++17 parallel-algorithms header; clang 22 rejects
<code>-freflection</code> as an unknown argument), so both are covered as
clearly-labeled forward-looking prose with no gate and no committed code, and
the live sender comparison is deferred to Chapter 56.</p>
