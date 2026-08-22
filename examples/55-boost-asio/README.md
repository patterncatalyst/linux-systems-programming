# 55 — Boost.Asio: what a library adds on top of a reactor

Chapter 9 built an epoll loop by hand. Chapter 22 wrote the nonblocking/`EAGAIN`
state machine. Chapter 27 drove a hand-rolled coroutine reactor over epoll.
Those three own the readiness model, and **nothing here repeats it**.

This example measures the concepts Asio has that a hand-written loop does not:
who runs a handler, whether two can run at once, and what that guarantee costs.

## `run()` returns when there is no work — not when you are done

```
[host]$ ./demo.sh cpp run work-guard
work-guard: without_work_handlers=0 with_guard_handlers=1 posted=1
work-guard: returned_immediately=yes guard_kept_it_alive=yes
```

A freshly constructed `io_context` has no outstanding work, so `run()` returns
after **zero** handlers and every async operation you were about to start never
happens. `executor_work_guard` is the claim that says "more is coming".

## The strand — the finding

2000 posts, 8 threads on one `io_context`, a deliberately non-atomic `long`
counter incremented in every handler. The handler body is *the same function* in
both runs; only the executor it was posted to differs.

```
[host]$ ./demo.sh cpp run strand
strand: posts=2000 counter=2000 expected=2000 correct=yes
strand: max_inflight=1 overlaps=0 serialized=yes

[host]$ ./demo.sh cpp run nostrand
nostrand: posts=2000 counter=1862 expected=2000 lost=138
nostrand: max_inflight=6 overlaps=1492 serialized=no
```

Serialization without a mutex, and — on the other arm — a real data race visible
as a **wrong number** rather than a sanitizer warning.

**The lost count is never gated.** An unsynchronized increment is a data race and
therefore UB, so a run that loses nothing is legal; across five consecutive runs
here it was 22, 110, 104, 44, 24. What is gated is the **overlap**, which is
measured with atomics and is well defined.

## A strand is not a mutex, and it is measurably cheaper

The same 20000 increments, counted with Chapter 51's method
(`strace -f -c -e trace=futex`):

| mechanism | futex calls | result |
| --- | --- | --- |
| strand | **52** | 20000 (correct) |
| `std::mutex` | **1022** | 20000 (correct) |

Both are correct. The difference is structural: a mutex **blocks a thread** on
contention, which is what Chapter 51 measured as `FUTEX_WAIT`; a strand **defers
the handler** and returns the thread to the pool to pick up other work.

## Scatter-gather is one syscall

```
[host]$ ./demo.sh cpp run gather
gather: buffers=4 wrote=56 peer_read=56 intact=yes

[host]$ ./demo.sh cpp run separate
separate: buffers=4 wrote=56 peer_read=56 intact=yes
```

Identical bytes on the wire, counted by syscall: the 4-element `const_buffer`
array costs **one `sendmsg` carrying 4 iovecs**; four separate `asio::write`
calls cost **four `sendto`**. This is Part 5's `readv`/`writev` machinery,
reached by handing Asio a buffer sequence instead of assembling an iovec array.

## Who runs the handlers

400 posts, tids from `gettid()` (Chapter 50's instrument, Chapter 49's
vocabulary):

| topology | distinct tids |
| --- | --- |
| `topology-one` — one thread, one context | **1** |
| `topology-pool` — 8 threads, one context | **8** |
| `topology-percore` — one context per thread | **4** |
| `topology-threadpool` — `asio::thread_pool(4)` | **4** |

Same handlers every time. The only variable is the shape of the `run()` call.

## Cancellation, the third shape

```
[host]$ ./demo.sh cpp run cancel
cancel: handlers_run=1 handler_ran=yes error=Operation canceled aborted=yes
```

Chapter 51 measured `std::stop_token` (a flag the target polls). Chapter 52
measured `boost::thread::interrupt()` (an ordinary exception). Asio's is a
**completion carrying `operation_aborted`** — a one-hour timer's handler runs
immediately, exactly once, so the ownership rules never change.

## Layout

```
55-boost-asio/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-F hard, G skip-if-present
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/asiodemo.cpp
```

**System Boost only — no Conan, no network.** Asio is header-only here; the link
line is `Boost::system` (for the error categories) plus `Threads::Threads`. The
translation unit is compiled `-DBOOST_ASIO_NO_DEPRECATED` so nothing accidentally
demonstrates an interface Asio has replaced.

**Nothing binds a port.** The I/O cases use `asio::local::connect_pair`, an
AF_UNIX socketpair, so the example stays `mode: local`, offline, and
unprivileged.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ for c in versions work-guard strand nostrand strand-cost mutex-cost \
                 gather separate cancel topology-one topology-pool \
                 topology-percore topology-threadpool; do
          ./demo.sh cpp run "$c"; done
```

## Verification

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

- **A** — every one of the 13 cases computes `digest=0x481984990deee5ff`,
  byte-identical to Chapters 46 through 54.
- **B** — a fresh `io_context::run()` returns after zero handlers; with a work
  guard held it runs the posted one.
- **C** — through a strand the unsynchronized counter is exactly 2000 with
  `max_inflight=1` and `overlaps=0`; posted straight to the context the same
  handler overlaps. **The overlap is gated, the lost count is not** — never gate
  on the output of undefined behavior (Chapter 49's rule).
- **D** — both arms produce the correct 20000, and the strand issues materially
  fewer futex calls. Gated as a **ratio** (`strand < mutex/2`), never as
  magnitudes. Degrades to an informational SKIP if `strace` cannot attach.
- **E** — the 4-buffer sequence costs exactly **1** socket write-family syscall
  and the separate form exactly **4**. These are deterministic, so they are gated
  exactly. Plain `write(2)` is excluded — that is this program's own stdout.
- **F** — `topology-one` reports exactly 1 tid; the three multi-threaded shapes
  report more than 1. Sign gated, never magnitude.

**G** (clang parity on the digest and on `overlaps=0`) runs if `clang++` is
present.

**Not gated:** timings (Chapter 39 — this book does not gate on durations); the
exact lost-increment count; anything needing a network port.
