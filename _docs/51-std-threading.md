---
title: "The standard threading library: what a lock costs, and why the answer is zero until it isn't"
order: 51
part: "Compendium: C++ Concurrency"
description: "stdthread performs the identical 200,000 increments three ways -- with no mutex, with an uncontended std::mutex, and with the same mutex across eight threads -- and counts the syscalls each one issues with strace. The uncontended case matches the no-mutex baseline exactly at 1 futex call, which is the dynamic loader rather than a lock, while the contended case costs 1844-3111. std::condition_variable, std::latch, std::barrier, std::counting_semaphore, and std::atomic::wait are then each shown reaching the same futex(2), and a std::lock_guard deadlock is reproduced under timeout(1) with every task's /proc wchan reading futex_do_wait, against a std::scoped_lock version that completes 100,000 acquisitions in opposing orders. Verified on the Fedora 44 host with glibc 2.43: verify.lua PASS 38/FAIL 0, local, unprivileged, offline."
duration: "60 minutes"
---

Chapter 25 already settled the headline question. It built a bounded work queue
three ways, put `strace` on it, and showed a mutex's futex traffic scaling from
5,186 calls to 488,818 as contention rose — about 98% of lock operations
resolving as a pure userspace compare-exchange in the uncontended run. A mutex
is a futex; the fast path never leaves user space.

This chapter starts from that result rather than re-deriving it, and pushes on
three things Chapter 25 did not cover.

**First, how far "nearly free" actually goes.** Chapter 25's uncontended run
still made thousands of futex calls, because a work queue with condition
variables genuinely blocks. Strip the blocking away and measure a bare lock,
and the number is not "low" — it is *indistinguishable from not locking at
all*, which requires a different measurement technique to establish.

**Second, the family.** C++20 added `latch`, `barrier`, `counting_semaphore`,
and `atomic::wait` to the `mutex` and `condition_variable` Chapter 25 used.
They look like a family of distinct facilities and they are one mechanism, and
seeing that is what stops you from reasoning about their costs separately.

**Third, what the standard library does that the futex does not.**
`std::scoped_lock` will not deadlock where `std::lock_guard` will, and
`std::stop_token` answers the cancellation problem Chapter 50 ended on. Neither
is visible from the syscall layer.

{% include excalidraw.html
   file="51-futex-cost-model"
   alt="Two bands showing the same 200,000 lock/unlock pairs measured two ways. The upper user-space band, labelled atomic compare-exchange on the futex word with no kernel involved, holds a baseline box for 200,000 plain increments with no mutex reporting 1 futex call and noting that the 1 is the loader rather than a lock; an amber uncontended box for one thread doing 200,000 lock/unlock pairs where the compare-exchange succeeds every time, also reporting 1 futex call, marked identical to the baseline; and a side note that the whole syscall profile is 61 calls of execve, mmap, openat, read and close, the dynamic loader, to which the 200,000 pairs contributed nothing. The lower amber kernel band, labelled futex(2) the syscall ch25 built by hand, holds an amber contended box for 8 threads doing the same 200,000 pairs where the compare-exchange fails often leading to sleep and wake, reporting 1844 to 3111 futex calls across 5 runs with the magnitude noted as varying; a box listing the blocking primitives condition_variable, latch, barrier, counting_semaphore and atomic::wait at 4 to 6 futex calls each; and a side note that these are five type names for one mechanism, because a thread that must sleep until further notice has to tell the kernel and there is no other way. An amber arrow labelled add contention runs from the uncontended box down to the contended box."
   caption="Figure 51.1 — the same 200,000 lock/unlock pairs, two ways: uncontended locking is indistinguishable from no locking at all, and contention is what reaches the kernel" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated, parity gate G), **`strace`** (host; gated by `check-host.sh` — it is a
> hard gate here, not an illustration: gates B and C are syscall counts it
> produces), `timeout` from coreutils (host; used to bound the deliberate
> deadlock), `lua` (host; gated, runs `verify.lua`), and `python3` (host;
> gated, runs `scripts/test-all-examples.py`). No VM, no root, no LGTM stack,
> no network: `examples/51-std-threading` is `mode: local` in
> `examples/manifest.yaml`.

C++-only, like the two chapters before it. Every code block below is a plain
fenced `cpp`, `lua`, or `console` block.

## Making the comparison legitimate first

A syscall count is only meaningful against another syscall count for the *same
work*. So the three cost cases are built to do exactly the same thing:

```cpp
constexpr long kTotalOps = 200000;
constexpr int kContendedThreads = 8;
```

200,000 increments in every case. `baseline` does them with no mutex at all.
`uncontended` does them one at a time under a `lock_guard` on a single thread.
`contended` splits the same 200,000 across eight threads sharing one mutex.
Same operations, same final value, same digest — the only variable is how much
the threads collide.

The harness asserts that before it compares anything:

```lua
checks.expect_match(out["contended"], "acc=200000",
  "cpp: contended performed the same 200000 increments across 8 threads -- same work, "
  .. "so the futex counts below are comparable")
```

That assertion is not ceremony. A version of this example that accidentally
gave the contended case a different iteration count would still have produced a
dramatic-looking table, and the table would have meant nothing.

## The baseline is not zero

The obvious gate to write is "the uncontended case makes zero futex calls." It
would fail, and it *should* fail:

```
[host]$ strace -f -c -e trace=futex cpp/build/release/stdthread baseline
baseline     futex=1
```

That is the case with no mutex in it. A program that has linked libstdc++ has
already run the dynamic loader and glibc's startup before `main()` begins, and
somewhere in there is one futex call. Asserting zero would be asserting
something about the loader, not about locking.

So the control exists precisely to be subtracted:

```cpp
void case_baseline() {
    long n = 0;
    for (long i = 0; i < kTotalOps; ++i) {
        ++n;
    }
    report("baseline", n);
}
```

## Uncontended locking is not "cheap" — it is absent

```
[host]$ for c in baseline uncontended contended; do
          printf "%-12s futex=%s\n" "$c" \
            "$(strace -f -c -e trace=futex cpp/build/release/stdthread $c 2>&1 | awk '/futex/{print $4}')"
        done
baseline     futex=1
uncontended  futex=1
contended    futex=2360
```

200,000 `lock_guard` acquisitions and releases, and the futex count is
identical to the run that never locked anything.

That is not rounding or noise. Here is the *entire* syscall profile of the
uncontended run:

```
[host]$ strace -f -c cpp/build/release/stdthread uncontended 2>&1 | tail -3
  0.00    0.000000           0         1           execve
------ ----------- ----------- --------- --------- ----------------
100.00    0.000084           1        61         1 total
```

Sixty-one syscalls total, and every one of them is `execve`, `mmap`, `openat`,
`read`, or `close` — the dynamic loader starting the process. The 200,000
lock/unlock pairs contributed nothing to it.

The mechanism is Chapter 25's, unchanged: `std::mutex` is a `pthread_mutex_t`
is a futex word, and locking a free one is an atomic compare-exchange that
either succeeds in user space or fails into `futex(FUTEX_WAIT)`. What is new
here is the *strength* of the statement the measurement supports. Chapter 25
could say 98% of acquisitions stayed in user space. This says 100% of them did,
and the way to establish that is the baseline subtraction above — comparing
against a control rather than against zero, so that the loader's syscalls do
not get charged to the mutex.

```cpp
void case_uncontended() {
    std::mutex m;
    long n = 0;
    for (long i = 0; i < kTotalOps; ++i) {
        std::lock_guard<std::mutex> g(m);
        ++n;
    }
    report("uncontended", n);
}
```

This is worth holding onto, because it inverts a common instinct. Adding a
mutex to a data structure that is rarely touched by more than one thread at a
time does not cost you syscalls. It costs you an atomic operation and a cache
line. The advice "locks are expensive, avoid them" is measuring the wrong
thing; the accurate version is "*waiting* is expensive, avoid contention."

## Contention is what reaches the kernel

Same 200,000 pairs, eight threads:

```cpp
void case_contended() {
    std::mutex m;
    long n = 0;
    std::vector<std::jthread> threads;
    threads.reserve(kContendedThreads);
    for (int t = 0; t < kContendedThreads; ++t) {
        threads.emplace_back([&m, &n] {
            for (long i = 0; i < kTotalOps / kContendedThreads; ++i) {
                std::lock_guard<std::mutex> g(m);
                ++n;
            }
        });
    }
    threads.clear();  // jthread joins on destruction
    report("contended", n);
}
```

Now the compare-exchange fails constantly, and every failure is a trip into the
kernel to sleep on the futex word — plus another, on the unlock side, to wake a
waiter.

Measured across five runs on the reference host: **1844, 2199, 2360, 2469,
3111**. The magnitude moves with scheduling and is not something to assert on.
The gate takes the sign:

```lua
  checks.expect_match(tostring(n_con ~= nil and n_unc ~= nil and n_con > n_unc * 100), "true",
    "cpp: contended exceeds uncontended by more than two orders of magnitude")
```

Chapter 39's discipline applies to syscall counts exactly as it does to
durations: the number that varies run to run is not the finding. "More than two
orders of magnitude" has never once been false here; "2360" would be false on
the next run.

One detail worth noticing: 200,000 lock operations produced roughly 2,400 futex
calls, not 200,000. Break those down by operation and the picture sharpens
further:

```
[host]$ strace -f -e trace=futex cpp/build/release/stdthread contended 2>&1 \
          | grep -oE "FUTEX_[A-Z_]+" | sort | uniq -c | sort -rn | head -3
    877 FUTEX_WAKE_PRIVATE
    554 FUTEX_WAIT_PRIVATE
      4 FUTEX_WAIT_BITSET
```

**554 sleeps out of 200,000 acquisitions.** Under deliberate eight-way
contention, 99.7% of lock operations never reached the kernel at all — glibc
spins briefly before it sleeps, precisely because the syscall is the expensive
part. The `_PRIVATE` suffix is the same optimization Chapter 25 covered: a
futex known not to be shared across processes skips the kernel's shared-mapping
lookup.

## Five type names, one mechanism

The C++20 additions look like a family of distinct facilities. Underneath they
are the same syscall:

| case | primitive | futex calls (3 runs) |
| --- | --- | --- |
| `condvar` | `std::condition_variable::wait` | 5, 5, 5 |
| `latch` | `std::latch::arrive_and_wait` | 5, 8, 5 |
| `barrier` | `std::barrier::arrive_and_wait` | 5, 4, 6 |
| `semaphore` | `std::counting_semaphore::acquire` | 4, 4, 4 |
| `atomic-wait` | `std::atomic<int>::wait` | 4, 4, 4 |

This is not an implementation coincidence to be filed away. A thread that must
sleep until some other thread does something *cannot* be implemented in user
space alone — the whole point is to stop consuming CPU, and only the kernel can
deschedule you. Any primitive with that shape reaches a syscall, and on Linux
that syscall is `futex`.

`std::atomic::wait` is the one that makes the relationship visible in the API
itself:

```cpp
    std::atomic<int> flag{0};
    long n = 0;
    std::jthread waiter([&] {
        flag.wait(0);  // block while flag == 0
        n = flag.load();
    });
    settle();
    flag.store(7);
    flag.notify_one();
    waiter.join();
```

`wait(0)` means "sleep while the value is still 0." That is `FUTEX_WAIT`'s
contract from Chapter 25 with the syscall boilerplate removed — the expected
value is passed in precisely so the kernel can re-check it atomically against
the sleep, closing the lost-wakeup race. C++20 standardized the futex, minus
the parts that are not portable.

The `latch` and `barrier` distinction is worth one line since they are easy to
confuse: a latch counts down once and stays at zero, while a barrier resets
after each phase and can run a completion function on whichever thread trips
it:

```cpp
    std::atomic<long> phases{0};
    std::barrier sync{2, [&phases]() noexcept { phases.fetch_add(1, std::memory_order_relaxed); }};
```

The completion function must be `noexcept` — the standard requires it, because
there is no sensible place to deliver an exception thrown while every
participating thread is mid-rendezvous.

### A note on the measurement scaffolding

Each blocking case has to get its waiter into the wait before the signal
arrives, or it might measure nothing:

```cpp
void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
```

That sleep is scaffolding, not measurement. No assertion anywhere depends on
its length — if it were too short the waiter would simply not block, the futex
count would drop to zero, and gate C would fail loudly rather than passing on a
technicality. It is deliberately named `settle` rather than something that
sounds like a timing parameter.

## Deadlock, and the standard library's answer

{% include excalidraw.html
   file="51-deadlock-and-scoped-lock"
   alt="Two mutexes and two threads producing two outcomes. The amber upper band, titled std::lock_guard in opposing orders as the textbook deadlock, shows thread 1 taking lock_guard x(a) then lock_guard y(b) and thread 2 taking lock_guard x(b) then lock_guard y(a) in the opposite order, leading to an amber box stating each holds what the other needs, timeout(1) exits 124 and it never returns, with no error, no exception, and no diagnostic. A dashed box below adds that the kernel names it via /proc/<pid>/task/*/wchan, with tids 2744896, 2744897 and 2744898 all reading futex_do_wait. The lower band, titled std::scoped_lock as taking them all at once so order stops mattering, shows thread 1 with scoped_lock lock(a, b) and thread 2 with scoped_lock lock(b, a) still in the opposite order, leading to an amber box stating it runs try-all, back off, retry rather than argument order, that 100,000 acquisitions completed with exit 0, and that this is the property lock_guard cannot give you."
   caption="Figure 51.2 — the textbook two-mutex deadlock reproduced and observed in procfs, beside the std::scoped_lock version that survives the same opposing orders" %}

Two mutexes, two threads, opposing acquisition orders. The textbook says this
deadlocks. The textbook is right:

```cpp
    std::thread t1([] {
        for (;;) {
            std::lock_guard<std::mutex> x(a);
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            std::lock_guard<std::mutex> y(b);
        }
    });
```

```
[host]$ timeout 5 cpp/build/release/stdthread deadlock-naive; echo "exit=$?"
deadlock-naive: two lock_guards in opposing orders; this will hang
exit=124
```

Exit 124 is `timeout(1)` reporting that it had to kill the process. Note what
did *not* happen: no error, no exception, no diagnostic, no return code from
any lock operation. A deadlocked program is not a failing program — it is a
program that stopped, silently, forever. That is what makes deadlock hard to
diagnose from inside the process.

From outside, the kernel is perfectly willing to say what happened:

```
[host]$ p=$(pgrep -x stdthread)
[host]$ for t in /proc/$p/task/*; do
          printf "tid=%-8s state=%-2s wchan=%s\n" "$(basename $t)" \
            "$(awk '/^State:/{print $2}' $t/status)" "$(cat $t/wchan)"
        done
tid=2744896  state=S  wchan=futex_do_wait
tid=2744897  state=S  wchan=futex_do_wait
tid=2744898  state=S  wchan=futex_do_wait
```

`wchan` is the kernel function a sleeping task is blocked in, and Chapter 36
read this file for other purposes. Here it closes the loop that Figure 51.1
opened: the abstraction was `std::mutex`, the failure is `futex_do_wait`, and
you can read it with `cat`, unprivileged, with no debugger attached.

Three threads park, not two — the two workers plus the main thread blocked in
`join()`. Every one of them is waiting on a futex that no one will ever wake.

### `scoped_lock` is not `lock_guard` with more arguments

```cpp
    std::jthread t2([&] {
        for (long i = 0; i < kDeadlockIters; ++i) {
            // The OPPOSITE order, deliberately. scoped_lock does not care.
            std::scoped_lock lock(b, a);
            done.fetch_add(1, std::memory_order_relaxed);
        }
    });
```

```
[host]$ ./demo.sh cpp run deadlock-safe
deadlock-safe: completed 100000 acquisitions in opposing orders
```

`std::scoped_lock` with two or more mutexes does not lock them in argument
order. It runs a deadlock-avoidance algorithm — lock the first, *try* the rest,
and on any failure release everything and start again from a different one. So
`scoped_lock(a, b)` and `scoped_lock(b, a)` running concurrently cannot
deadlock, and the argument order genuinely does not matter.

That is the property `lock_guard` cannot give you no matter how carefully you
write it, and it is the reason `scoped_lock` exists as a separate type rather
than as a variadic `lock_guard`. The single-mutex case is the one where they
are interchangeable.

The convention this leaves you with: use `lock_guard` (or `scoped_lock`) for
one mutex, `scoped_lock` for two or more, and never take a second mutex inside
a scope that already holds one unless you can state the global lock order out
loud.

## The `stop_token` payoff

Chapter 50 ended on an argument: `pthread_cancel` unwinds through an exception
that an ordinary `catch (...)` can fatally swallow, and C++20 answered with
cooperative cancellation instead. Here is what that answer looks like running:

```cpp
void case_stoptoken() {
    std::atomic<long> loops{0};
    std::jthread worker([&loops](std::stop_token stop) {
        while (!stop.stop_requested()) {
            loops.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    settle();
    worker.request_stop();
    worker.join();
```

```
[host]$ ./demo.sh cpp run stoptoken
stoptoken: stop_requested honored, worker returned normally
stdthread report: case=stoptoken acc=1 digest=0x481984990deee5ff
```

The worker returned. Not cancelled, not unwound, not `PTHREAD_CANCELED` —
it reached the top of its loop, saw the flag, and left through the normal exit.
Nothing was thrown, so there is nothing a catch-all can swallow, and there is
no list of cancellation points to memorize.

The trade is real and worth stating plainly: this does strictly less than
`pthread_cancel`. A thread blocked in a `read(2)` that never checks the token
will not stop, and no amount of `request_stop()` will move it. Chapter 12's
self-pipe and Chapter 13's `pidfd` are the shapes you reach for when a blocked
thread has to be interruptible; `stop_token` is for threads that loop.

Note also that `jthread` joins in its destructor and requests a stop first,
which is why `case_contended` can do this:

```cpp
    threads.clear();  // jthread joins on destruction
```

That single line is a `join()` on eight threads. `std::thread`'s destructor
calls `std::terminate` if the thread is still joinable — a design that catches
the bug loudly, at the cost of making every early return a leak hazard.
`jthread` is the fix, and the reason to reach for it by default.

## How the code works

Each measurement is its own subcommand:

```cpp
    if (what == "baseline") {
        case_baseline();
    } else if (what == "uncontended") {
        case_uncontended();
    } else if (what == "contended") {
        case_contended();
```

The structure carries weight for the same reason it did in Chapter 50:
`deadlock-naive` never returns. If the cases shared a process, that one would
hang the whole program and the harness would time out on everything.

The deadlock case is marked so the compiler knows:

```cpp
[[noreturn]] void case_deadlock_naive() {
```

The `std::abort()` at the end of that function is unreachable — the joins never
return — and is there so the `[[noreturn]]` contract holds on every path a
compiler can see.

The digest is carried forward unchanged from Chapters 46 through 50:

```cpp
void report(const char* what, long acc) {
    std::printf("stdthread report: case=%s acc=%ld digest=0x%016llx\n", what, acc,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}
```

`acc` is printed beside it deliberately. The digest proves the payload was
hashed; `acc` proves the case actually did its work, which is what makes the
syscall comparison legitimate.

## Errors, three ways

**A failure with no error to report.** Deadlock is the pure case: no return
code, no exception, no `errno`. The only signal is that time passes and nothing
happens. This is why the gate is a `timeout` exit status and a procfs
reading — there is nothing inside the process to check.

**A failure the harness must not turn into a false pass.** `strace` can be
blocked by ptrace hardening, and `/proc/<tid>/wchan` can be unreadable. Both
would make the interesting gates unmeasurable, and both degrade to a printed
`SKIP`:

```lua
if not have_strace or n_base == nil then
  print("SKIP: strace could not count syscalls (absent or ptrace-restricted) -- " ..
        "gates B and C not asserted")
else
```

A skipped gate that announces itself is a different thing from a gate that
quietly passed, and Chapter 48 established the pattern: `SKIP:` lines beside the
`PASS` count.

**A failure the standard library turns into a crash on purpose.**
`std::thread`'s destructor calls `std::terminate` on a still-joinable thread.
That is a deliberate choice to make a silent resource bug into a loud one, and
`jthread` exists so you rarely meet it.

## Concurrency lens

The counters in this example are `std::atomic` under `memory_order_relaxed`,
and the reasoning is the same as Chapter 49's: they are observation state, read
once after every thread has joined, with `join()` supplying the
synchronization edge.

The exception is `case_contended`'s `long n`, which is a **plain** `long` — not
an atomic:

```cpp
            for (long i = 0; i < kTotalOps / kContendedThreads; ++i) {
                std::lock_guard<std::mutex> g(m);
                ++n;
            }
```

That is correct, and it is the whole point of the case. Every access happens
under the mutex, so the mutex provides the mutual exclusion *and* the
happens-before edges. Making it atomic would be redundant, and worse, it would
muddy the measurement: an atomic increment under a lock would still be an
atomic operation the uncontended case does not perform, and the two cases have
to differ in exactly one respect.

That `acc=200000` in the output is what proves it. A racy increment would come
back short — that is the classic symptom — so the assertion doubles as a
correctness check on the locking itself.

The deadlock cases use `std::thread` rather than `std::jthread` for the naive
version, deliberately. `jthread` would request a stop and join in its
destructor, which is the right default everywhere else and would obscure what
is being demonstrated here.

## Build, run, observe

```
[host]$ cd examples/51-std-threading
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run uncontended
[host]$ strace -f -c -e trace=futex cpp/build/release/stdthread contended
```

Do the counting yourself rather than trusting the table — that is the entire
skill this chapter is teaching, and `strace -c` is the whole tool.

To watch the deadlock, run it under `timeout` in one terminal and read procfs
in another:

```
[host]$ timeout 5 cpp/build/release/stdthread deadlock-naive
[host]$ cat /proc/$(pgrep -x stdthread)/task/*/wchan
```

Then the harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: 200000 uncontended lock/unlock pairs cost no more futex calls than the no-mutex baseline (baseline=1, uncontended=1) -- an uncontended std::mutex is an atomic CAS, not a syscall
ok: cpp: the identical 200000 pairs, contended, cost hundreds of futex calls (contended=2426) -- the cost is contention, not locking
ok: cpp: contended exceeds uncontended by more than two orders of magnitude
ok: cpp: condvar blocks, and blocking means futex(2) (calls=5)
ok: cpp: std::scoped_lock(a,b) and scoped_lock(b,a) on two threads completed -- argument order does not matter to scoped_lock
ok: cpp: two lock_guards in opposing orders deadlock -- timeout(1) had to kill it
ok: cpp: while deadlocked, procfs reports every thread parked in a futex wait (wchan: futex_do_wait futex_do_wait futex_do_wait )
ok: cpp: the jthread observed request_stop() and returned on its own terms -- no PTHREAD_CANCELED and no forced unwind, unlike ch50's pthread_cancel
info: absolute futex counts for the contended case are NOT gated -- they are scheduling-dependent (1844-3111 observed across runs on the reference host); only the sign of the comparison is asserted
PASS 38 / FAIL 0
```

## Cross-check: one digest, one futex

Every non-hanging case reported `digest=0x481984990deee5ff`, and clang agrees.
That constant now spans Chapter 46's C++ toolbox, Chapter 47's Go, Chapter 48's
Rust, Chapter 49's four execution models, Chapter 50's six per-thread controls,
and Chapter 51's eleven synchronization cases.

There is a second cross-check running through this chapter, and it points
backwards. Chapter 25 measured a mutex's futex traffic scaling from 5,186 calls
to 488,818, and concluded that a mutex is a futex whose fast path stays in user
space. Chapter 26 built lock-free structures on atomics. This chapter agrees
with both and sharpens the first: with the blocking stripped away and a control
to subtract, the uncontended figure is not 98% but 100% — the mutex contributes
no syscalls whatsoever.

That agreement across two chapters, two examples, and two measurement
techniques is worth more than either number alone. Chapter 25's workqueue and
this chapter's bare loop have almost nothing in common except the primitive
underneath, and they report the same structure.

## What you learned

- **An uncontended `std::mutex` costs no syscalls at all.** Chapter 25
  established that the fast path stays in user space; with the blocking
  stripped away and a control to subtract, 200,000 lock/unlock pairs produced
  the same futex count as a run with no mutex, and a 61-syscall profile that is
  entirely loader startup.
- **Measure against a baseline, not against zero.** A program that has linked
  libstdc++ makes syscalls before `main()` runs; the control case exists to be
  subtracted. Asserting zero would have been asserting something about the
  dynamic loader.
- **Contention is the cost, and even then most of it stays in user space.** The
  identical 200,000 operations across eight threads cost 1844–3111 futex calls,
  of which only 554 were actual `FUTEX_WAIT` sleeps — 99.7% of acquisitions
  never reached the kernel, because glibc spins before it sleeps.
- **Gate the sign, not the magnitude.** "More than two orders of magnitude" is
  reproducible; "2360" is not. Chapter 39's rule applies to syscall counts as
  much as to durations.
- **Five type names, one mechanism.** `condition_variable`, `latch`, `barrier`,
  `counting_semaphore`, and `atomic::wait` all reach `futex(2)`, because a
  thread that must sleep until notified can only be descheduled by the kernel.
- **`std::atomic::wait(old)` is `FUTEX_WAIT`** with the expected value passed
  in for the same reason — so the kernel can re-check it atomically against the
  sleep and close the lost-wakeup race.
- **A deadlocked program is not a failing program.** No error, no exception, no
  return code — it simply stops. From outside, `/proc/<tid>/wchan` names the
  kernel function every thread is parked in.
- **`std::scoped_lock` is not variadic `lock_guard`.** It runs a
  deadlock-avoidance algorithm rather than locking in argument order, which is
  why opposing orders across threads are safe with it and fatal without it.
- **`stop_token` does less than `pthread_cancel`, on purpose.** The worker
  returns normally on its own terms; a thread blocked in a syscall that never
  polls the token will not stop, and that limitation is the design.
- **Prefer `jthread`.** It requests a stop and joins in its destructor, where
  `std::thread`'s destructor calls `std::terminate` on a joinable thread.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, <strong>glibc 2.43</strong>,
g++ 16.1.1 20260515, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, strace 7.1, Lua
5.4.8, 16 logical / 8 physical CPUs; local, unprivileged, no network, no VM):
<code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. &amp;&amp; pwd) lua verify.lua</code>
reported <code>PASS 38 / FAIL 0</code> with gates B, C, and E all running for
real rather than skipping, and <code>python3 scripts/test-all-examples.py --only
51-std-threading</code> reported <code>1 passed, 0 failed, 0 skipped</code>.
Every transcript quoted above is a real run from this session. Confirmed live:
all ten non-hanging cases produced <code>digest=0x481984990deee5ff</code>,
byte-identical to Chapters 46 through 50; the three cost cases each reported
<code>acc=200000</code>, so the syscall counts compare equal work; futex counts
from <code>strace -f -c -e trace=futex</code> over three runs each were
<code>baseline</code> 1/1/1, <code>uncontended</code> 1/1/1,
<code>contended</code> 2360/2199/2469 (and 1844 and 3111 in earlier runs, the
range quoted in the text), <code>condvar</code> 5/5/5, <code>latch</code> 5/8/5,
<code>barrier</code> 5/4/6, <code>semaphore</code> 4/4/4,
<code>atomic-wait</code> 4/4/4, <code>stoptoken</code> 2/2/2; the full
<code>strace -c</code> profile of the uncontended run was <code>61</code>
syscalls total; <code>deadlock-safe</code> completed
<code>100000 acquisitions in opposing orders</code>;
<code>timeout 5 … deadlock-naive</code> exited <code>124</code> and, caught
mid-hang, all three tasks in <code>/proc/&lt;pid&gt;/task/*/wchan</code> read
<code>futex_do_wait</code>; and <code>stoptoken</code> printed
<code>stop_requested honored, worker returned normally</code>. Not exercised:
<span class="status status--unverified">unverified</span> — the absolute futex
count for the contended case is deliberately not gated anywhere, because it is
scheduling-dependent; only the sign of the comparison is asserted, and
<code>verify.lua</code> prints an informational line saying so. Two gates would
degrade to a printed <code>SKIP</code> on a host where <code>strace</code>
cannot attach (ptrace hardening) or <code>/proc/&lt;tid&gt;/wchan</code> is
unreadable; neither condition occurred here, so both paths are reasoned rather
than observed.</p>
