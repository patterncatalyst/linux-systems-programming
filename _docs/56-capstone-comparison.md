---
title: "The capstone: one workload, seven models, and the two instruments that make them comparable"
order: 56
part: "Compendium: C++ Concurrency"
description: "capstone closes Part 14 by fixing the flaw in every chapter before it: ch50-ch55 each measured one model, with its own instrument, on its own workload, so no two numbers in the compendium were strictly comparable. Here one workload -- 8 tasks x 5000 rounds folding into a single accumulator, shared verbatim by every arm -- runs through all six models plus a sequential control, and all of them produce the identical total 0xb75768f6610642a0, which is gated before any comparison is drawn. Measured with gettid(), the three single-threaded models hold eight tasks in flight on exactly one OS thread while the four multi-threaded ones report eight. Measured with ch51's strace method, the same 40000 folds cost 1 futex call under any single-threaded model, 230 to 451 under a lock, and 51 through an Asio strand. A seventh arm settles Chapter 49's conditional promise about P2300: __cpp_lib_senders is still undefined, so it links NVIDIA's stdexec reference implementation, pinned by an in-repo Conan recipe to tag nvhpc-26.05 by sha256, isolated so the six-model core needs no Conan and no network. Verified on the Fedora 44 host: verify.lua PASS 57/FAIL 0 with Conan present and PASS 52/FAIL 0 with it hidden."
duration: "65 minutes"
---

Part 14 has measured six ways to run more than one thing at a time. Chapter 50
took pthreads apart with `pthread_getattr_np` and `/proc`. Chapter 51 counted
futex calls. Chapter 52 weighed Boost.Thread's four pillars. Chapter 53 caught a
coroutine frame in an overloaded `operator new`. Chapter 54 priced a fiber's
stack. Chapter 55 watched a strand serialize handlers without a lock.

Every one of those numbers is real. And almost none of them can be compared with
any of the others, because each chapter measured **its own model, with its own
instrument, on its own workload**, chosen to show that model at its most
characteristic. A futex count from Chapter 51 and a tid count from Chapter 54 are
not two readings on one scale. They are two different scales.

This chapter is the correction. One workload, every model, the same two
instruments.

{% include excalidraw.html
   file="56-one-workload-seven-models"
   alt="The capstone comparison. A dark header box records the shared workload -- 8 tasks times 5000 rounds equals 40000 folds into one accumulator -- and notes that every model below produced the identical total 0xb75768f6610642a0, checked by gate B before any comparison is drawn. Below it three tiers, separated by the two uniform instruments. The top band, one OS thread, concurrent but not parallel in ch49's terms with no lock anywhere because there is nothing to race with, holds sequential the control, coroutine from ch53 with 32-byte frames, and fiber from ch54 with 128 KiB stacks, each reporting distinct_tids equals 1 and exactly 1 futex call. The middle band, many OS threads plus a lock where a thread that loses the race is parked and has to be woken, holds pthreads from ch50 at 230 futex calls using pthread_mutex_t, std::jthread from ch51 at 427 using std::mutex, Boost.Thread from ch52 at 451 using boost::mutex with when_all, and a dashed ghost box for the opt-in P2300 senders arm at 562, labelled NVIDIA stdexec and NOT the stdlib -- all four reporting 8 distinct tids. The lower amber band, many OS threads plus a strand where the handler is deferred so no thread ever blocks, holds Boost.Asio's strand from ch55 at 8 distinct tids and 51 futex calls, running on the same 8 threads as the row above but serialized without a lock, with an amber arrow down from the lock band labelled one eighth the futex traffic of the row above. Footnotes name the instruments -- gettid() from ch50, strace -f -c -e trace=futex from ch51 -- and record that both were applied identically to all seven models, which is the only thing that makes this a comparison rather than a table of quotations, and that futex magnitudes move on every run and are never gated while the three tiers are."
   caption="Figure 56.1 — six models, one control, and one opt-in seventh, all running the same 40000 folds and all measured the same two ways" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated, parity gate F), `strace` (host; gated, supplies instrument 2),
> `timeout` from coreutils (host; bounds every arm), `lua` (host; gated, runs
> `verify.lua`), `python3` (host; gated, runs `scripts/test-all-examples.py`),
> and **`conan`** (host; gated, and needed *only* for the opt-in seventh arm —
> gate E prints a SKIP without it). **System Boost 1.90.0** from Fedora's
> `boost-devel` for the six-model core: **no Conan, no network, nothing bound to
> a port**. No VM, no root; `examples/56-capstone-comparison` is `mode: local`.

## One workload, or it is not a comparison

The workload lives in a header every arm includes verbatim:

```cpp
// The unit of work. Deterministic in (task, round) and independent of every
// other unit, so any interleaving any model produces folds to the same total.
inline std::uint64_t unit(int task, int round) {
    const std::uint64_t seed =
        kFnvOffsetBasis ^ (static_cast<std::uint64_t>(task) * 1000003ULL +
                           static_cast<std::uint64_t>(round));
    return fnv1a(kPayload, sizeof kPayload, seed);
}
```

Eight tasks, five thousand rounds each, forty thousand folds into one
accumulator. Two properties make it usable as a common denominator.

First, **the fold is commutative**. It is addition on `uint64_t`, so the total
does not depend on the order the units are applied in — and the models genuinely
differ in that order. A comparison whose answer changed with the scheduling would
be measuring the scheduling.

Second, **there is contention to measure**. Forty thousand updates to one
accumulator is Chapter 51's experiment at a smaller scale, and it is what makes
the second instrument report anything at all. A workload where each task kept a
private subtotal would be a perfectly good workload and would have made this
chapter impossible.

Then the gate that has to come first:

```
[host]$ ./demo.sh cpp run sequential
sequential: tasks=8 rounds=5000 folds=40000 total=0xb75768f6610642a0 correct=yes

[host]$ ./demo.sh cpp run fiber
fiber: tasks=8 rounds=5000 folds=40000 total=0xb75768f6610642a0 correct=yes
```

```lua
  checks.expect_match(tostring(total), tostring(reference),
    "cpp: " .. m .. " folded to the IDENTICAL total (" .. tostring(total) ..
    ") -- six models, one workload, one answer, so the comparison compares like with like")
```

Every arm produced `0xb75768f6610642a0`. This is gated before anything else is
compared, and the reason is worth stating plainly: a model that produced a
different total would not be a differently-priced answer to the same question. It
would be a wrong answer, and every number downstream of it would be describing
two different programs.

## Instrument 1: who actually ran it

`gettid()` — Chapter 50's instrument, unioned into a set the way Chapter 49
unioned CPU ids:

```
[host]$ ./demo.sh cpp run sequential
sequential: distinct_tids=1
[host]$ ./demo.sh cpp run coroutine
coroutine: distinct_tids=1
[host]$ ./demo.sh cpp run fiber
fiber: distinct_tids=1
[host]$ ./demo.sh cpp run pthreads
pthreads: distinct_tids=8
[host]$ ./demo.sh cpp run std-thread
std-thread: distinct_tids=8
[host]$ ./demo.sh cpp run boost-thread
boost-thread: distinct_tids=8
[host]$ ./demo.sh cpp run asio
asio: distinct_tids=8
```

The interesting rows are the ones reporting **1**. `coroutine` and `fiber` both
hold eight tasks in flight; both interleave them; neither occupies more than one
CPU. That is Chapter 49's distinction between *concurrency* and *parallelism*
reduced to a count — and having it fall out of the same instrument that reports 8
for the thread-based arms is the point of running them all through one program.

It also draws the line where it actually falls. The division is not
threads-versus-coroutines or standard-versus-Boost. It is **whether the model
puts your work on more than one kernel-scheduled thread**, and three quite
different suspension mechanisms — a plain loop, a stackless frame, a stackful
fiber — land on the same side of it.

## Instrument 2: what the synchronization cost

Chapter 51's method, unchanged, pointed at all seven arms:

```
[host]$ strace -f -c -e trace=futex cpp/build/release/capstone std-thread
```

| tier | arm | futex calls | what the model does when work collides |
| --- | --- | --- | --- |
| one thread | `sequential` | **1** | nothing — there is nothing to collide with |
| one thread | `coroutine` | **1** | resumes the next frame |
| one thread | `fiber` | **1** | switches to the next stack |
| a lock | `pthreads` | **230** | **blocks** the thread on `pthread_mutex_t` |
| a lock | `std-thread` | **427** | **blocks** the thread on `std::mutex` |
| a lock | `boost-thread` | **451** | **blocks** the thread on `boost::mutex` |
| a strand | `asio` | **51** | **defers** the handler, returns the thread |

Three tiers, and the same forty thousand folds in every row.

The top tier is the one worth pausing on. A single-threaded model reaches the
futex **not at all** — not rarely, not cheaply, but zero times beyond the one
call the loader makes. Whatever a coroutine frame or a fiber stack costs
elsewhere, on this axis it costs nothing, because synchronization is a price you
pay for parallelism and these models do not buy any.

The bottom two tiers restate Chapter 55's finding under controlled conditions. A
mutex and a strand both produce the correct total; the mutex parks a thread the
kernel then has to wake, and the strand queues a handler and hands the thread
back. Same eight threads, same work, an eighth of the kernel traffic.

The gate takes the tiers and refuses the magnitudes:

```lua
  checks.expect_match(tostring(n["asio"] ~= nil and n["std-thread"] ~= nil and
                               n["asio"] * 2 < n["std-thread"]), "true",
    "cpp: Asio's STRAND cost less than half what std::mutex cost for byte-identical work " ..
    "(strand=" .. tostring(n["asio"]) .. ", mutex=" .. tostring(n["std-thread"]) ..
    ") -- a mutex blocks a thread, a strand defers a handler (ch55)")
```

Across runs on this host the lock arms moved between 230 and 549 and the strand
between 51 and 65. The ratio never came close to failing and no individual number
ever repeated, which is exactly the shape of a measurement whose sign you may
assert and whose magnitude you may not.

## The seventh model, and what it is not

Chapter 49 left this chapter a debt, and was careful about its terms:

> It is scheduled for Chapter 56's comparison, and it goes in there **if and only
> if the toolchain has caught up** by then. No committed dead code in the
> meantime.

It has not caught up. The `versions` arm re-measures the same feature-test macros
Chapter 49 did, so the claim is gated output rather than prose:

```
[host]$ ./demo.sh cpp run versions
versions: Boost 1.90.0
versions: senders_macro=undefined stdlib_p2300=no
versions: execution_macro=201902 (201902 = the C++17 parallel algorithms header)
versions: the P2300 arm is built only by the opt-in Conan sub-target
```

`__cpp_lib_senders` is still undefined, and GCC 16's `<execution>` is still the
C++17 parallel-algorithms header. So the seventh arm links **NVIDIA's stdexec,
the reference implementation of P2300**, and every place it could be mistaken for
the standard library says otherwise — the program's own output, the gate message,
and the recipe's docstring:

```
[host]$ ./cpp/build/conan/capstone-senders senders
senders: tasks=8 rounds=5000 folds=40000 total=0xb75768f6610642a0 correct=yes
senders: distinct_tids=8
senders: implementation=NVIDIA-stdexec (reference), stdlib_p2300=no
```

Same total. Eight tids, 562 futex calls: squarely in the lock tier, which it
should be, because this arm deliberately shares an accumulator under a mutex like
the thread arms do rather than composing per-task values the way sender code
normally would. That is not idiomatic and the source says so — a different
workload would have made the comparison meaningless, and being able to explain
the choice is worth more here than being idiomatic.

### Pinning something that has no releases

Getting stdexec at all took more care than the other six models put together.
It is **not in ConanCenter**, it is not packaged by Fedora, and upstream's own
`conanfile.py` is named `p2300` and takes its version from `git.get_commit()` —
there is no stable version to depend on. Nor are there any GitHub releases, only
`nvhpc-*` snapshot tags.

So the example carries its own recipe:

```python
    def source(self):
        get(self,
            url="https://github.com/NVIDIA/stdexec/archive/refs/tags/nvhpc-26.05.tar.gz",
            sha256="9d2396fecd604698c1eae58f0cb6e4517aa727013846240d1a7b2f35e49884dc",
            strip_root=True)
```

The tag names the snapshot; **the sha256 is what makes it reproducible**, because
a tag can be moved and a hash cannot.

### Isolation is the load-bearing part

The six-model core must not acquire a network dependency because of an arm most
readers will never build. That is Chapter 46's pattern, and here it is enforced
in the build:

```cmake
find_package(stdexec QUIET)
if(stdexec_FOUND)
  add_executable(capstone-senders src/senders.cpp)
  target_link_libraries(capstone-senders PRIVATE STDEXEC::stdexec Threads::Threads)
  message(STATUS "stdexec found -- building the P2300 arm (capstone-senders)")
else()
  message(STATUS "stdexec not found -- P2300 arm skipped (six-model core is unaffected)")
endif()
```

and in the gate, which prints a SKIP rather than aborting the script:

```lua
if not tool_present("conan") then
  print("SKIP: conan not found -- gate E (the P2300 senders arm) not asserted. The six-model " ..
    "core above is unaffected, which is the point of keeping this arm isolated.")
```

Both paths were run. With Conan available: `PASS 57 / FAIL 0`. With `conan`
hidden from `PATH` and the Conan build tree deleted: `PASS 52 / FAIL 0`, gate E
skipped, everything else real. The second run is the one that matters, because it
is what CI does on a fedora:44 image that has never heard of Conan.

## Two axes this chapter assembles rather than measures

{% include excalidraw.html
   file="56-two-assembled-axes"
   alt="The two axes ch56 assembles rather than measures. The left band, what a paused computation costs, runs a chain of three boxes joined by amber arrows: a thread at 8388608 bytes measured in ch50 with pthread_getattr_np, annotated that the kernel schedules it so it costs kernel state; an arrow labelled divide by 64 down to an amber fiber at 131072 bytes measured in ch54 with stack_traits::default_size(), annotated as user-scheduled but keeping a real stack; and an arrow labelled divide by 4096 down to a coroutine frame at 32 bytes measured in ch53 with an operator new inside promise_type, annotated as having no stack so it costs only what crosses a suspension. The right band, how cancellation arrives, stacks the four shapes the compendium measured: ch50's pthread_cancel as a forced unwind where swallowing it without rethrowing aborts the process; ch51's std::stop_token as a flag the target polls, where nothing is thrown so there is nothing to swallow; ch52's thread::interrupt() as an ordinary exception thrown at defined interruption points; and an amber ch55 cancellation_signal as a completion carrying operation_aborted, where the handler still runs exactly once so ownership never changes. Footnotes state that neither band is measured in ch56 but assembled from earlier chapters' own measurements and carried into the table arm as named constants beside the instrument that produced them, that cost and capability move together and in opposite directions so the more a paused computation costs the fewer restrictions there are on where it may pause, and that P2300 senders are absent from both bands on purpose because __cpp_lib_senders is still undefined."
   caption="Figure 56.2 — the compendium's other two axes, carried in as named constants beside the instrument that produced each one" %}

Not every axis can be re-measured under one roof. What a paused computation costs
in bytes needs a different instrument per model — `pthread_getattr_np` for a
thread, `stack_traits::default_size()` for a fiber, an overloaded `operator new`
for a coroutine frame — and a program that tried to unify them would be inventing
a number rather than reporting one.

So the `table` arm assembles them instead, from named constants that cite their
provenance:

```cpp
constexpr std::size_t kCh50ThreadStackBytes = 8388608;    // pthread_getattr_np
constexpr std::size_t kCh54FiberStackBytes = 131072;      // stack_traits::default_size()
constexpr std::size_t kCh53CoroutineFrameBytes = 32;      // operator new in promise_type
```

```
[host]$ ./demo.sh cpp run table
table: thread=8388608 (ch50, pthread_getattr_np)
table: fiber=131072 (ch54, stack_traits::default_size)
table: coroutine_frame=32 (ch53, operator new in promise_type)
table: fibers_per_thread_stack=64 frames_per_fiber_stack=4096
table: ch50 pthread_cancel=forced-unwind (swallowing it aborts the process)
table: ch51 stop_token=flag (nothing is thrown; the target polls)
table: ch52 thread::interrupt=exception (ordinary, at defined points)
table: ch55 cancellation_signal=completion (operation_aborted, handler still runs)
```

Both ratios are arithmetic on measured numbers rather than figures written into
prose, which is the discipline Chapter 54 introduced when it first put these
three side by side. **Calling this out matters more than the numbers do**: a
capstone that silently mixed fresh measurements with quoted ones would be exactly
the kind of table this chapter exists to replace.

The cancellation row is the compendium's most useful piece of trivia and its
clearest illustration that these models disagree about fundamentals. Four
libraries, four different answers to "stop that": an unwind you must not swallow,
a flag you must poll, an exception you may catch, and a completion that arrives
whether or not anything went wrong.

## How the code works

Each model is its own arm, and each arm is the smallest correct way to run the
shared workload in that model. That restraint is deliberate: the novelty here is
the uniform instrument, so an arm that grew a second idea would be re-deriving a
chapter that already exists.

The coroutine arm needs no lock, and the reason is structural:

```cpp
void arm_coroutine() {
    note_tid_once();
    std::uint64_t acc = 0;
    std::vector<FoldTask> tasks;
    tasks.reserve(kTasks);
    for (int t = 0; t < kTasks; ++t) {
        tasks.push_back(fold_coroutine(t, acc));
    }
```

Eight coroutines share `acc` by reference with nothing guarding it, and that is
correct because a coroutine runs until it suspends. Between two `co_await`s it
has the accumulator to itself. The fiber arm is unguarded for the same reason one
level up, and both would become instantly wrong under a scheduler that put them
on more than one thread — which is Chapter 54's `shared_work` warning, and why
`distinct_tids` is worth gating rather than assuming.

The tid instrument had to be kept out of the futex instrument's way:

```cpp
inline void note_tid_once() {
    thread_local bool noted = false;
    if (noted) {
        return;
    }
    noted = true;
    const std::lock_guard<std::mutex> lock(g_tid_mutex);
    g_tids.insert(static_cast<long>(gettid()));
}
```

The tid set is guarded by a mutex, and a mutex taken forty thousand times would
have landed squarely in the futex counts instrument 2 is trying to report. Taken
once per thread it is eight acquisitions against forty thousand folds — noise
rather than the measurement. An instrument that perturbs the other instrument is
a real hazard when you unify several of them in one program, and this is the
whole of the defence.

## Errors, three ways

**An error the build catches.** The `conan` preset builds `Release`, not
`RelWithDebInfo`, and it has to. CMakeDeps writes its include directories behind
a generator expression:

```
set_property(TARGET STDEXEC::stdexec
             APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
             $<$<CONFIG:Release>:${stdexec_INCLUDE_DIRS_RELEASE}>)
```

Configure that with a build type the Conan profile did not generate and the
expression evaluates to nothing. `find_package(stdexec)` still **succeeds**, the
target still exists, CMake prints "stdexec found", and the compile then dies on
`fatal error: exec/static_thread_pool.hpp: No such file or directory`. The
failure is loud, but it points at the header rather than at the mismatch, which
is the kind of build error worth having met once before.

**An error the gate catches.** Two models producing different totals. Nothing
about that is a crash — each arm exits 0 and prints a plausible number — and
without gate B the chapter would have gone on to compare their futex counts as if
they had done the same work. This is the failure mode a capstone is uniquely
exposed to, and the only defence is checking the answers before comparing the
prices.

**An error nothing catches.** Choosing a model for a reason this table does not
measure. Every number here is a count of threads or of syscalls; none of them
says anything about how hard the code is to read, how the model behaves when a
task throws, or what happens when the workload is I/O-bound rather than
CPU-bound. Chapter 55's `asio` arm looks excellent on both axes and would be a
strange choice for a program with no I/O in it. The table is evidence, not a
verdict.

## Concurrency lens

The compendium's through-line, now visible in one place: **each model prices the
same three things differently, and the prices move together.**

*Where a computation may pause.* A thread may be preempted anywhere and pays
8 MiB of stack for the privilege. A fiber suspends anywhere in an ordinary call
tree and pays 128 KiB. A coroutine suspends only at `co_await`, only in a
coroutine body, and pays 32 bytes. Cost buys freedom, in that order, every time.

*Who is allowed to run it.* Instrument 1 splits the six models cleanly in two,
and nothing else about them predicts which side they land on. The models that
stay on one thread need no synchronization at all — not cheap synchronization,
none — and the models that spread across eight need every bit of it.

*What it costs when work collides.* Instrument 2 says a blocked thread is the
expensive event, not a shared variable. `pthreads`, `std-thread`, `boost-thread`
and `senders` all block and all land in the hundreds. Asio's strand touches the
same accumulator from the same eight threads and lands at fifty, because it never
blocks anybody.

Notice what did *not* turn out to matter. Standard library versus Boost made no
difference on either axis — `std::mutex` and `boost::mutex` are the same futex
underneath, and Chapter 51's measurements transfer to Chapter 52's library
unchanged. What separated the models was a structural property in each case:
one thread or many, block or defer.

## Build, run, observe

```
[host]$ cd examples/56-capstone-comparison
[host]$ ./demo.sh cpp build
[host]$ for a in versions sequential pthreads std-thread boost-thread \
                 coroutine fiber asio table; do ./demo.sh cpp run "$a"; done
```

Instrument 2 is worth running yourself, since that is where its evidence lives:

```
[host]$ for a in sequential coroutine fiber asio pthreads std-thread boost-thread; do
          printf '%-14s' "$a"
          strace -f -c -e trace=futex cpp/build/release/capstone "$a" 2>&1 |
            awk '/futex/{print $4}' | head -1
        done
```

The opt-in seventh arm, which is the only thing here that needs a network:

```
[host]$ conan export cpp/conan/recipe
[host]$ conan install cpp/conan --output-folder=cpp/build/conan --build=missing \
          --lockfile=cpp/conan/conan.lock -s compiler.cppstd=23
[host]$ cd cpp && cmake --preset conan && cmake --build --preset conan
```

Then the harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: the six-model core builds against SYSTEM Boost only -- no Conan, no network, no third-party package
ok: cpp: sequential folded to the IDENTICAL total (0xb75768f6610642a0) -- six models, one workload, one answer, so the comparison compares like with like
ok: cpp: coroutine held 8 tasks in flight on exactly ONE OS thread -- ch49's 'concurrent, not parallel' as a count
ok: cpp: asio spread the SAME work across 8 OS threads
ok: cpp: fiber never reached the futex (1 calls) -- one thread means nothing to synchronize, whatever the model
ok: cpp: std-thread reached the futex 427 times for the same 40000 folds -- a blocked thread is a thread the kernel has to park and wake (ch51)
ok: cpp: Asio's STRAND cost less than half what std::mutex cost for byte-identical work (strand=51, mutex=427) -- a mutex blocks a thread, a strand defers a handler (ch55)
ok: cpp: and folds to the IDENTICAL total (0xb75768f6610642a0) -- a SEVENTH model, same workload, same answer
ok: cpp: reported as the NVIDIA stdexec REFERENCE implementation -- ch49 measured __cpp_lib_senders as undefined and it still is, so this is not the standard library
PASS 57 / FAIL 0
```

## Cross-check: the same sixteen bytes, one last time

Every arm reported `digest=0x481984990deee5ff`, and so does the clang build. That
value has now survived Chapters 46 through 56 — eleven chapters, seven
concurrency models, two compilers, and one third-party reference implementation
fetched from a pinned tarball.

The stronger cross-check is the one this chapter was built to make. Six models
that share no machinery — kernel threads, a stackless state machine, a stackful
context switch, a handler queue — folded the same forty thousand units into the
same 64-bit total, and disagreed only about who ran them and what that cost. When
the answer is invariant and only the price moves, the price is worth comparing.

## What you learned

- **A comparison requires one workload.** Chapters 50–55 each measured their own
  model on their own workload with their own instrument, which makes their
  numbers real and mutually incomparable.
- **Check the answers before comparing the prices.** All seven models folded to
  `0xb75768f6610642a0`, gated first — a model with a different total is answering
  a different question.
- **The line falls at one-thread-or-many.** `sequential`, `coroutine` and `fiber`
  each held 8 tasks in flight on exactly 1 OS thread; the other four reported 8.
  Concurrency and parallelism, as a count.
- **A single-threaded model reaches the futex zero times.** Not cheaply — not at
  all. Synchronization is a price you pay for parallelism.
- **A blocked thread is the expensive event.** Locks cost 230–451 futex calls;
  Asio's strand cost 51 for byte-identical work, because it defers a handler
  instead of parking a thread.
- **`std::mutex` and `boost::mutex` are the same futex.** The standard-versus-
  Boost axis predicted nothing; the structural properties predicted everything.
- **Assembled numbers must be labelled as assembled.** The paused-computation and
  cancellation tables are carried in as named constants citing the chapter and
  instrument that produced each, never re-derived here.
- **P2300 is still not in the standard library.** `__cpp_lib_senders` remains
  undefined on GCC 16.1.1; the seventh arm is NVIDIA's reference implementation
  and says so in its output, its gate, and its recipe.
- **Pin by hash, not by tag.** stdexec is absent from ConanCenter and publishes no
  releases, so the recipe pins `nvhpc-26.05` by sha256 — a tag can be moved.
- **Isolate the arm that needs a network.** The six-model core builds with system
  Boost alone, and gate E degrades to a SKIP; both paths were run, at 57 and 52
  passing checks respectively.
- **Do not let one instrument perturb another.** The tid set is recorded once per
  thread, because a mutex taken 40000 times would have become the futex
  measurement instead of avoiding it.
- **A table is evidence, not a verdict.** Nothing measured here says anything
  about readability, exception behavior, or an I/O-bound workload.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.8-200.fc44, glibc 2.43, <strong>Boost
1.90.0 from <code>boost-devel-1.90.0-7.fc44</code></strong>, g++ 16.1.1
20260515, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, Conan 2.30.0, Lua 5.4.8, 16
logical / 8 physical CPUs; local, unprivileged, nothing bound to a port, no VM):
<code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. &amp;&amp; pwd) lua verify.lua</code>
reported <code>PASS 57 / FAIL 0</code> with gates B (one workload, one answer),
C (tid counts), D (futex counts under <code>strace</code>), E (the P2300 arm) and
F (clang parity) all running for real, and <code>python3
scripts/test-all-examples.py --only 56-capstone-comparison</code> reported
<code>1 passed, 0 failed, 0 skipped</code>. <strong>The SKIP path was exercised
too</strong>: with <code>conan</code> hidden from <code>PATH</code> and
<code>cpp/build/conan</code> deleted, the same harness reported <code>PASS 52 /
FAIL 0</code> with gate E printing an informational SKIP — which is what CI does
on fedora:44, where Conan is absent. Every transcript quoted above is a real run
from this session. Confirmed live: all nine core arms plus the senders arm
produced <code>digest=0x481984990deee5ff</code>, byte-identical to Chapters 46
through 55 and matched by the clang build; all seven models folded to
<code>total=0xb75768f6610642a0</code>; <code>distinct_tids</code> was
<code>1</code> for <code>sequential</code>, <code>coroutine</code> and
<code>fiber</code> and <code>8</code> for <code>pthreads</code>,
<code>std-thread</code>, <code>boost-thread</code>, <code>asio</code> and
<code>senders</code>; futex counts in the gated run were <code>1 / 1 / 1</code>
for the single-threaded arms, <code>230 / 427 / 451</code> for the lock arms and
<code>51</code> for the strand, with further runs spanning 230-549 for locks and
51-65 for the strand and 562-582 for senders — magnitudes that move every run and
are never gated, unlike the three tiers; <code>versions</code> reported
<code>senders_macro=undefined stdlib_p2300=no</code> and
<code>execution_macro=201902</code>, re-measuring Chapter 49's claim rather than
restating it. The stdexec tarball for tag <code>nvhpc-26.05</code> was verified
at sha256 <code>9d2396fecd604698c1eae58f0cb6e4517aa727013846240d1a7b2f35e49884dc</code>
and compiled and ran under both g++ 16.1.1 and clang++ 22.1.8. Not exercised:
<span class="status status--unverified">unverified</span> — nothing in this
example is timed or gated on a duration (Chapter 39), so no throughput or
latency claim is made or implied about any model; the comparison is entirely
counts. The paused-computation and cancellation tables in the <code>table</code>
arm are <strong>assembled</strong> from Chapters 50, 51, 52, 53, 54 and 55 rather
than re-measured here, and are carried as named constants beside the instrument
that produced each. The senders arm deliberately shares an accumulator under a
mutex rather than composing values, which is not idiomatic sender code and is not
presented as a measurement of what idiomatic sender code would cost.
<code>asio::dispatch</code>, <code>algo::shared_work</code>, and a migrating
coroutine or fiber under a multi-threaded scheduler are named in the chapter but
not built or gated.</p>
