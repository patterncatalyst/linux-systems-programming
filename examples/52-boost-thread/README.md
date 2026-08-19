# 52 — Boost.Thread: what the standard did not take

Boost.Thread is where `std::thread` came from — C++11 standardized it almost
wholesale. So the question worth asking in 2026 is not "how do I use
Boost.Thread" but **what survived standardization**, and the answer is specific,
still missing from C++23, and measurable.

```
[host]$ ./demo.sh cpp run versions
versions: Boost 1.90.0 BOOST_THREAD_VERSION=5
versions: std::future_has_then=no boost::future_has_then=yes
```

That second line is a `concept` the compiler evaluated against both types on
this run. The chapter does not claim `std::future` lacks continuations — it
compiles the question.

## The four pillars

| pillar | what Boost has | what `std::` has |
| --- | --- | --- |
| `continuations` / `when-all` | `future::then()`, `when_all` — compose without blocking | `std::future` has neither |
| `upgrade` | `upgrade_lock` → `upgrade_to_unique_lock`, a **third lock state** | `std::shared_mutex` has no upgrade path; `std::upgrade_lock` is undeclared |
| `interrupt` / `interrupt-busy` | `thread::interrupt()` — an **ordinary C++ exception** at defined points | `stop_token` is a flag; nothing is thrown |
| `attributes` | `thread::attributes::set_stack_size` — portable stack sizing | nothing at all (ch50's territory) |

Every pillar prints `digest=0x481984990deee5ff` over `The quick brown.` — the
value ch46's C++, ch47's Go, ch48's Rust, ch49's `conc`, ch50's `pthreads`, and
ch51's `stdthread` all produce.

## Cancellation, three ways — the payoff

This example completes an arc ch50 opened. **One mistake — `catch (...)` with no
rethrow — against three cancellation models:**

| chapter | mechanism | swallow it and don't rethrow |
| --- | --- | --- |
| ch50 | `pthread_cancel` — glibc **forced unwind** | **process aborts**, `FATAL: exception not rethrown`, exit 134 |
| **ch52** | `thread::interrupt()` — an **ordinary exception** | **survives**, exit 0 |
| ch51 | `stop_token` — a **flag** | nothing to swallow |

```
[host]$ ./demo.sh cpp run interrupt-swallow; echo "exit=$?"
interrupt-swallow: caught the interruption and did NOT rethrow
interrupt-swallow: process survived -- boost interruption is an ordinary exception, not glibc's forced unwind
exit=0
```

The difference is not how careful the caller is — it is what the mechanism *is*.
A forced unwind must propagate; an ordinary exception need not.

Interruption points are a **defined list** (`this_thread::sleep_for`,
`condition_variable::wait`, `thread::join`, `interruption_point()`, among
others). A thread that never reaches one cannot be interrupted at all:

```
[host]$ ./demo.sh cpp run interrupt-busy
interrupt-busy: interrupt() delivered, worker_stopped=no
interrupt-busy: a loop with no interruption point never checks, so it never stops
```

That is the trade, and it is gated. ch50's `pthread_cancel` *can* yank a thread
out of a compute loop because it is asynchronous; Boost gives that up in
exchange for interruption happening only where you can see it happening.

## The gotcha that will bite you

`boost::thread::attributes` **must be passed as a `const` lvalue**:

```cpp
const boost::thread::attributes& cattrs = attrs;   // required
boost::thread worker(cattrs, &stack_probe);
```

With `BOOST_THREAD_PROVIDES_VARIADIC_THREAD` (the default at version 5), a
*non-const* `attributes` object is captured by the variadic
`thread(F&& f, Args&&... args)` constructor, which deduces
`F = thread_attributes` and then tries to **call** it. The result is a wall of
template errors out of `boost/thread/detail/invoke.hpp` that never names the
real problem. A lambda fails this way, a function pointer fails this way, and
adding a dummy argument does not help.

## Layout

```
52-boost-thread/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-F hard, G skip-if-present
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/boostthread.cpp
```

**System Boost only — no Conan, no network.** Fedora's `boost-devel` (1.90.0
here) provides everything. Local, unprivileged, no VM, no root.

The `BOOST_THREAD_VERSION` and continuation macros live in `CMakeLists.txt`
rather than the `.cpp`, deliberately: Boost reads them while its own headers are
being preprocessed, so a define landing after the first Boost include is
silently ignored — and the symptom is a missing `.then()` rather than a warning.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run versions
[host]$ ./demo.sh cpp run continuations
[host]$ ./demo.sh cpp run upgrade
[host]$ ./demo.sh cpp run interrupt
[host]$ ./demo.sh cpp run interrupt-swallow
[host]$ ./demo.sh cpp run attributes
```

Bare `./demo.sh` builds and runs `versions`.

## Verification

```
[host]$ LSP_LANG=cpp lua verify.lua
```

Gates A–F are hard:

- **A** — every pillar computes the expected digest.
- **B** — `.then()` chains `async(21)` into 42; `when_all` joins two futures.
- **C** — the `std::future` contrast is **compile-time**: a concept evaluated
  against both types, so the claim is checked by the compiler on every run.
- **D** — a concurrent reader takes a shared lock while the upgrade lock is
  held, then the upgrade promotes in place and writes.
- **E** — interruption is caught and the worker joins normally; the swallow
  case exits 0 where ch50's equivalent exited 134; and a busy loop with no
  interruption point ignores `interrupt()` entirely.
- **F** — a 256 KiB Boost attribute yields exactly **262144** via
  `pthread_getattr_np` — byte-identical to ch50's POSIX measurement, because on
  Linux it is that call.

**G** (clang parity) runs if `clang++` is present, otherwise prints an
informational `SKIP`.

**Not gated:** Boost's experimental executors. This example covers what the
standard did *not* take, and executors are the part C++26's `std::execution` is
actively replacing — forward-looking in ch49, compared for real in ch56.
