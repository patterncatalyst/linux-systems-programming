# 50 — pthreads

Chapter 49 measured threads from above, with `std::thread`. This example goes
**under** `std::thread` to the thing it is on Linux: a pthread. Each subcommand
exercises one per-thread control that POSIX offers and the C++ standard library
does not expose, and reports something the kernel or glibc produced.

```
[host]$ ./demo.sh cpp run naming
naming: tid=2675834 via_pthread='ch50-worker' via_proc='ch50-worker' agree=yes
naming: 16char_rc=34 (ERANGE) name_after='ch50-worker' unchanged=yes
pthreads report: facet=naming digest=0x481984990deee5ff
```

## The facets

| subcommand | control | observable |
| --- | --- | --- |
| `identity` | `gettid(2)` vs `pthread_t` vs `getpid(2)` | main's tid **equals** the pid; workers get distinct tids; each names a `/proc/self/task/<tid>/` directory |
| `naming` | `pthread_setname_np` | the name read back from **both** `pthread_getname_np` and `/proc/self/task/<tid>/comm`; a 16-char name refused with `ERANGE` |
| `stack` | `pthread_attr_setstacksize` | `pthread_getattr_np` reports exactly 262144 for a 256 KiB request vs 8388608 by default; a sub-`PTHREAD_STACK_MIN` request refused with `EINVAL` |
| `affinity` | `pthread_setaffinity_np` | two threads pinned to two CPUs report two different `sched_getcpu()` values, and the caller's own mask is untouched |
| `sched` | `pthread_getschedparam` / `setschedparam` | default is `SCHED_OTHER` at priority 0; an unprivileged `SCHED_FIFO` request is refused with `EPERM` |
| `cancel` | `pthread_cancel` | the C++ destructor **and** the `pthread_cleanup_push` handler both run; `pthread_join` reports `PTHREAD_CANCELED` |
| `cancel-swallow` | the same, mishandled | a `catch (...)` that does not rethrow aborts the process: `FATAL: exception not rethrown`, exit 134 |
| `bridge` | `std::thread::native_handle()` | naming a `std::thread` from outside lands in that thread's procfs `comm` — `native_handle_type` **is** `pthread_t`, checked by `static_assert` |

One subcommand per facet is deliberate: `cancel-swallow` ends in `abort()` by
design, and a facet that aborts must never be able to take the others with it.

Every facet prints `digest=0x481984990deee5ff` over the payload
`The quick brown.` — the same value ch46's C++, ch47's Go, ch48's Rust, and
ch49's `conc` produce. As always, the answer does not depend on how the work
was scheduled, or here on which per-thread knob was turned.

## Two identities, and why it matters

A Linux thread has two names and they are not interchangeable:

- **`pthread_t`** is an opaque glibc handle. You may compare it with
  `pthread_equal` and nothing else. It is not a number, and printing it is not
  portable.
- **`gettid()`** is the kernel's task id: a real integer, the thing that names
  a directory in `/proc`, the thing `perf`, `ftrace`, `top -H`, and gdb all
  report.

The main thread is the case that catches people out — its tid *equals* the
process pid, because a process on Linux is its first thread.

## Layout

```
50-pthreads/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-H hard, I skip-if-present, E skip-if-1-CPU
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/pthreads.cpp
```

C++-only, like ch49. Go and Rust wrap these same pthreads and deliberately hide
exactly these controls, so a three-language version would have two directories
with nothing to say. `verify.lua` skips for any `LSP_LANG` other than `cpp`.

Standard library and pthreads only. Local, unprivileged, offline — **no Boost,
no Conan, no network, no VM, no root**.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run identity
[host]$ ./demo.sh cpp run naming
[host]$ ./demo.sh cpp run stack
[host]$ ./demo.sh cpp run affinity
[host]$ ./demo.sh cpp run sched
[host]$ ./demo.sh cpp run cancel
[host]$ ./demo.sh cpp run bridge
[host]$ ./demo.sh cpp run cancel-swallow   # aborts on purpose, exit 134
```

Bare `./demo.sh` builds and runs `identity`. `TARGET=<vm>` deploys to a lab VM,
though nothing here needs one.

The facets are short-lived by design, but the `cancel` one parks its worker on
a cancellation point for ~200 ms, which is long enough to catch from another
terminal:

```
[host]$ ./cpp/build/release/pthreads cancel & sleep 0.1
[host]$ ps -L -o pid,tid,comm,psr -p "$(pgrep -x pthreads)"
    PID     TID COMMAND         PSR
2676217 2676217 pthreads          2
2676217 2676219 pthreads         14
```

Two rows for one process: `TID` is the same integer `gettid()` returns, and the
first row's TID equals the PID because a process is its first thread. `PSR` is
the CPU each thread is on — the `affinity` facet is what makes those two numbers
something you chose rather than something the scheduler picked. `COMMAND` is the
procfs `comm` field, which defaults to the process name; the `naming` and
`bridge` facets are the ones that change it.

## Verification

```
[host]$ LSP_LANG=cpp lua verify.lua
```

Gates A–H are hard and all run offline and unprivileged:

- **A** — every facet computes the expected digest.
- **B** — identity: main tid == pid, worker tids distinct, `/proc/self/task/<tid>` present.
- **C** — naming: both read-back paths agree, and 16 chars is refused with `ERANGE` leaving the old name in place.
- **D** — stack: the custom thread got at least what was asked and less than the default; a sub-minimum request is refused with `EINVAL` rather than rounded up.
- **E** — affinity: two threads on two different CPUs, caller's mask unchanged. Prints an informational `SKIP` (never a FAIL) on a host allowed fewer than 2 CPUs.
- **F** — sched: `SCHED_OTHER` at priority 0, and `SCHED_FIFO` refused with `EPERM`. **The refusal is the gate** — asserting a *successful* `SCHED_FIFO` would need root and would not run on a reader's laptop.
- **G** — cancel, both directions: the unwind runs destructors and cleanup handlers, and swallowing that unwind kills the process with `exception not rethrown`.
- **H** — bridge: `native_handle_type` is `pthread_t`, and a `std::thread` named through it shows that name in procfs.

**I** (clang parity on the digest) runs if `clang++` is on `PATH` and prints an
informational `SKIP` otherwise.

Not gated: a *successful* `SCHED_FIFO` change, which needs `CAP_SYS_NICE` or an
`RLIMIT_RTPRIO` allowance. The `EPERM` above is what an unprivileged run can
prove, and `verify.lua` says so in its own output rather than staying quiet.
