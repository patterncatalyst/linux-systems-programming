# 51 — the standard threading library, and what it costs

Chapter 25 already showed that a mutex is a futex whose uncontended fast path
stays in user space, measuring ~98% of acquisitions never reaching the kernel.
This example starts there and pushes on three things ch25 did not cover:

- **how far "nearly free" goes** — with the blocking stripped away and a control
  to subtract, an uncontended `std::mutex` costs *zero* syscalls, not merely few
- **the family** — `condition_variable`, `latch`, `barrier`,
  `counting_semaphore`, and `atomic::wait` are one mechanism, not five
- **what the library adds over the futex** — `std::scoped_lock` will not
  deadlock where `lock_guard` will, and `std::stop_token` answers ch50's
  cancellation problem

## The headline

Three cases, all performing exactly the same 200,000 increments — only the
contention differs:

```
[host]$ for c in baseline uncontended contended; do
          printf "%-12s futex=%s\n" "$c" \
            "$(strace -f -c -e trace=futex cpp/build/release/stdthread $c 2>&1 | awk '/futex/{print $4}')"
        done
baseline     futex=1
uncontended  futex=1
contended    futex=2360
```

`baseline` uses no mutex at all, and it still reports 1 — that call is the
dynamic loader, not a lock. `uncontended` performs 200,000 `lock_guard`
acquisitions and reports **the same 1**. Its entire syscall profile is 61
calls, all `execve`/`mmap`/`openat`/`read`/`close`:

```
[host]$ strace -f -c cpp/build/release/stdthread uncontended 2>&1 | tail -3
  0.00    0.000000           0         1           execve
------ ----------- ----------- --------- --------- ----------------
100.00    0.000084           1        61         1 total
```

An uncontended `std::mutex` is an atomic compare-exchange in user space. The
cost people attribute to "locking" is really the cost of **contention**.

## The subcommands

| subcommand | what it measures | futex calls (3 runs) |
| --- | --- | --- |
| `baseline` | 200,000 increments, no mutex — the control | 1, 1, 1 |
| `uncontended` | the same 200,000, each under a `lock_guard`, one thread | 1, 1, 1 |
| `contended` | the same 200,000, split across 8 threads | 2360, 2199, 2469 |
| `condvar` | `condition_variable::wait` with a predicate | 5, 5, 5 |
| `latch` | `std::latch::arrive_and_wait` (single-use) | 5, 8, 5 |
| `barrier` | `std::barrier` (reusable, with a completion function) | 5, 4, 6 |
| `semaphore` | `std::counting_semaphore::acquire` | 4, 4, 4 |
| `atomic-wait` | `std::atomic<int>::wait` / `notify_one` (C++20) | 4, 4, 4 |
| `stoptoken` | `jthread` + `request_stop()` — the ch50 contrast | 2, 2, 2 |
| `deadlock-safe` | `scoped_lock(a,b)` vs `scoped_lock(b,a)`, 100,000 acquisitions | — |
| `deadlock-naive` | the same with `lock_guard` — **hangs on purpose** | — |

Five different type names for the blocking primitives, one mechanism
underneath. A thread that must sleep until further notice has to tell the
kernel; there is no other way to do it.

Every non-hanging subcommand prints `digest=0x481984990deee5ff` over the payload
`The quick brown.` — the value ch46's C++, ch47's Go, ch48's Rust, ch49's
`conc`, and ch50's `pthreads` all produce.

## Deadlock, both directions

```
[host]$ ./demo.sh cpp run deadlock-safe
deadlock-safe: completed 100000 acquisitions in opposing orders

[host]$ timeout 5 cpp/build/release/stdthread deadlock-naive; echo "exit=$?"
deadlock-naive: two lock_guards in opposing orders; this will hang
exit=124
```

`std::scoped_lock` takes all its mutexes at once with a deadlock-avoidance
algorithm — try, back off, retry — rather than locking them in argument order.
So `scoped_lock(a, b)` on one thread and `scoped_lock(b, a)` on the other is
safe. `lock_guard` in those same orders is the textbook deadlock.

And the kernel will tell you exactly what happened. While the naive version
hangs:

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

Three threads, not two: both workers plus the main thread blocked in `join()`.
All of them parked in the same kernel function, readable unprivileged.

**`deadlock-naive` never returns.** It is its own subcommand for the same reason
ch50's `cancel-swallow` was, and `verify.lua` only ever invokes it under
`timeout(1)`.

## Layout

```
51-std-threading/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-F hard, G skip-if-present
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/stdthread.cpp
```

C++-only, like ch49 and ch50. Standard library only — no Boost, no Conan, no
network, no VM, no root.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run uncontended
[host]$ ./demo.sh cpp run contended
[host]$ timeout 5 cpp/build/release/stdthread deadlock-naive    # hangs by design
```

Bare `./demo.sh` builds and runs `baseline`. To count syscalls yourself, wrap
any subcommand:

```
[host]$ strace -f -c -e trace=futex cpp/build/release/stdthread contended
```

## Verification

```
[host]$ LSP_LANG=cpp lua verify.lua
```

Gates A–F are hard:

- **A** — every non-hanging subcommand computes the expected digest, and the
  three cost cases each report `acc=200000` so their syscall counts are
  comparable in the first place.
- **B** — the headline: `uncontended` costs no more futex calls than the
  no-mutex `baseline`, while `contended` — the identical work — costs hundreds,
  more than two orders of magnitude more.
- **C** — `condvar`, `latch`, `barrier`, `semaphore`, and `atomic-wait` each
  issue at least one futex call.
- **D** — `deadlock-safe` completes 100,000 acquisitions in opposing orders.
- **E** — `deadlock-naive` times out (exit 124), and while hung every task's
  `wchan` reads a futex wait.
- **F** — `stoptoken`'s worker returns normally after `request_stop()`: no
  `PTHREAD_CANCELED`, no forced unwind.

**G** (clang parity) runs if `clang++` is present, otherwise prints an
informational `SKIP`.

Two gates degrade to a `SKIP` rather than a FAIL if the host will not
cooperate: if `strace` cannot attach (ptrace hardening), gates B and C are
skipped; if `/proc/<tid>/wchan` is unreadable, the wchan half of gate E is.
Neither ever becomes a false PASS.

**Not gated:** the absolute futex count for the contended case. It is
scheduling-dependent — 1844–3111 observed across runs on the reference host —
so only the *sign* of the comparison is asserted, per ch39. `verify.lua` prints
a line saying so.
