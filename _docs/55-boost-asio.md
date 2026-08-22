---
title: "Boost.Asio: the strand, the work guard, and who actually runs your handler"
order: 55
part: "Compendium: C++ Concurrency"
description: "asiodemo measures what a library adds on top of the reactor Chapters 9, 22, and 27 built by hand -- and re-derives none of it. A freshly constructed io_context::run() returns after zero handlers, because it returns on no outstanding work rather than on completion. The same handler posted through a strand by 8 threads leaves a deliberately unsynchronized counter at exactly 2000 with max_inflight=1 and overlaps=0; posted straight to the io_context it overlaps 1492 times and loses increments for real -- a data race visible as a wrong number, whose magnitude is reported and never gated because it is UB. A strand and a std::mutex both produce the correct 20000, at 52 futex calls against 1022 counted with Chapter 51's strace method: a mutex blocks a thread, a strand defers a handler. Four buffers written as one sequence cost one sendmsg carrying 4 iovecs against four sendto, Part 5's writev machinery without an iovec array. Four shapes of run() report 1, 8, 4, and 4 distinct tids by gettid(). Cancellation arrives as a completion carrying operation_aborted -- a third shape after Chapter 51's flag and Chapter 52's exception. Verified against system Boost 1.90.0 on the Fedora 44 host: verify.lua PASS 48/FAIL 0, no Conan, no network, nothing bound to a port."
duration: "60 minutes"
---

Three chapters of this book have already built a reactor. Chapter 9 wrote the
epoll loop, level- and edge-triggered, with `EPOLLONESHOT` and timerfd and
signalfd. Chapter 22 wrote the nonblocking state machine around it and the
`EAGAIN` bookkeeping that makes partial reads and partial writes correct.
Chapter 27 drove a hand-rolled coroutine reactor over the same epoll fd.

So the interesting question about Boost.Asio is not "how does it do readiness".
It does readiness the way you already know, on this platform through `epoll_wait`
in exactly the shape Chapter 9 built. The interesting question is what a library
has that a hand-written loop does not — and the answer turns out to be a
vocabulary for two things the hand-written loop never had to name, because it
only ever had one thread: **who runs a handler, and whether two of them can run
at once.**

This chapter measures that vocabulary. Nothing below re-derives epoll,
readiness, or `EAGAIN`.

{% include excalidraw.html
   file="55-strand-not-a-mutex"
   alt="The same handler, the same 8 threads and the same io_context posted two ways, and a lower band pricing the alternative. The upper left amber box, post(strand, handler), reports an unsynchronized counter reaching 2000 of 2000 and correct, with max_inflight equals 1 and overlaps equals 0, annotated that 8 threads ran handlers and no two ran at once. The upper right dashed ghost box, post(io_context, handler), reports the same counter reaching only 1897 of 2000 with max_inflight equals 6 and overlaps equals 1441 -- a real data race visible as a wrong number rather than a sanitizer warning -- annotated that the lost count is undefined behaviour and is reported rather than gated, while the overlap is what verify.lua asserts because it is measured with atomics and well defined. The lower band contrasts two correct mechanisms: std::mutex, which BLOCKS a thread into futex FUTEX_WAIT, at 982 futex calls for 20,000 increments, against an amber strand, which DEFERS the handler and returns the thread to the pool, at 61 futex calls for the identical 20,000 increments. A closing amber note observes that a strand costs no lock and no blocked thread, and pays instead in ordering. Footnotes record that the futex counts were measured with ch51's method, strace -f -c -e trace=futex, and are gated as a ratio rather than as magnitudes because they move with scheduling, and that everything was measured on the Fedora 44 reference host with Boost 1.90.0 and GCC 16.1.1."
   caption="Figure 55.1 — one handler, two executors: a strand serializes without a lock, and the arm without one loses increments for real" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated, parity gate G), `strace` (host; gated, counts the futex calls in gate D
> and the socket writes in gate E), `timeout` from coreutils (host; bounds every
> case), `lua` (host; gated, runs `verify.lua`), and `python3` (host; gated, runs
> `scripts/test-all-examples.py`). **System Boost 1.90.0** from Fedora's
> `boost-devel` — Asio itself is header-only, and the link line adds only
> `boost_system` for the error categories. **No Conan, no network, and nothing
> binds a port**: the I/O cases use an AF_UNIX socketpair. No VM, no root;
> `examples/55-boost-asio` is `mode: local`.

## The first surprise: `run()` returns immediately

Before any of the interesting machinery, the thing that catches everyone once:

```
[host]$ ./demo.sh cpp run work-guard
work-guard: without_work_handlers=0 with_guard_handlers=1 posted=1
work-guard: returned_immediately=yes guard_kept_it_alive=yes
work-guard: run() returns on no OUTSTANDING WORK, not on completion
```

Zero handlers. A freshly constructed `io_context` has nothing outstanding, so
`run()` finds nothing to do and returns — and the async operations you were
about to start never happen, because the loop they needed already exited.

`run()` returns on *no outstanding work*, not on *you are finished*. The claim
that keeps it alive is an `executor_work_guard`:

```cpp
    asio::io_context guarded;
    auto guard = asio::make_work_guard(guarded);
    int posted = 0;
    asio::post(guarded, [&posted, &guard] {
        ++posted;
        guard.reset();  // release the claim; run() may now return
    });
    const std::size_t ran_with = guarded.run();
```

This is worth naming as a structural difference rather than a gotcha. Chapter
9's hand-written loop was a `while (true)` — its lifetime was the loop's own
control flow, and it ran until you broke out of it. An `io_context`'s lifetime is
instead a *refcount on pending work*, which is the right design for a library
that cannot see your control flow, and the wrong intuition for anyone arriving
from a loop they wrote themselves.

## The strand

Here is the chapter's central measurement. Two thousand handlers, eight threads
all calling `run()` on one `io_context`, and a counter that is deliberately a
plain `long` — not atomic, not under any lock:

```cpp
struct Observed {
    std::atomic<int> inflight{0};
    std::atomic<int> max_inflight{0};
    std::atomic<int> overlaps{0};
    long unguarded = 0;  // intentionally unsynchronized -- that is the point
};
```

The handler body is a single function used by both arms, so the *only* variable
in the experiment is which executor it was posted to:

```cpp
// The handler body, shared by both variants so the ONLY difference between
// them is which executor it was posted to.
auto make_handler(Observed& o) {
    return [&o] {
        const int now = o.inflight.fetch_add(1, std::memory_order_relaxed) + 1;
        bump_max(o, now);
        if (now > 1) {
            o.overlaps.fetch_add(1, std::memory_order_relaxed);
        }
        // Widen the window so overlap is observable rather than theoretical.
        for (volatile int k = 0; k < 200; ++k) {
        }
        ++o.unguarded;
        o.inflight.fetch_sub(1, std::memory_order_relaxed);
    };
}
```

Through a strand:

```
[host]$ ./demo.sh cpp run strand
strand: posts=2000 counter=2000 expected=2000 correct=yes
strand: max_inflight=1 overlaps=0 serialized=yes
strand: 8 threads were running handlers, and no two ran at once
```

Exactly 2000, from an unsynchronized increment performed by eight threads. Not
2000 because the race happened not to bite — `max_inflight=1` says no two
handlers were ever simultaneously inside the body, and `overlaps=0` says the
condition never once held. There was no race to lose.

Now the same handler, posted straight to the `io_context`:

```
[host]$ ./demo.sh cpp run nostrand
nostrand: posts=2000 counter=1862 expected=2000 lost=138
nostrand: max_inflight=6 overlaps=1492 serialized=no
nostrand: same handler, same threads -- only the executor changed
```

One hundred and thirty-eight increments gone. Not a warning from a sanitizer, not
a theoretical hazard in a comment — a **wrong number**, produced by the same
function that produced the right one a moment ago.

That is the whole idea of a strand. It is an executor with one guarantee: handlers
submitted through it never run concurrently, and never overlap, no matter how many
threads are pulling from the underlying context. Serialization without a lock.

### What the gate asserts, and what it refuses to

Notice which number `verify.lua` is willing to stand behind:

```lua
checks.expect_match(tostring(ns_overlaps ~= nil and ns_overlaps > 0), "true",
  "cpp: the SAME handler posted straight to the io_context overlaps (" ..
  tostring(ns_overlaps) .. " overlapping entries, max_inflight=" .. tostring(ns_inflight) ..
  ") -- only the executor changed")
```

The overlap, and only the overlap. The lost-increment count is reported by the
program and asserted by nothing, for a reason Chapter 49 made a rule: **an
unsynchronized increment is a data race, a data race is undefined behavior, and
you cannot gate on the output of undefined behavior.** A run that loses nothing
at all is entirely legal, and a test that failed on it would be wrong, not the
run.

The magnitudes bear that out. Five consecutive runs on this host lost 22, 110,
104, 44, and 24 increments — Figure 55.1 records yet another one, at 1897 of
2000. Every one of those is the same defect and none of them is a number to
quote as *the* answer. Over those same runs `overlaps` was 1375 to 1507 and
`max_inflight` 5 to 6, never remotely near zero, which is why the overlap is the
assertion: it is measured with atomics, it is well defined, and it is a real
statement about scheduling rather than about UB.

## A strand is not a mutex

The obvious objection is that a mutex would also have produced 2000. It would.
Both arms of the next case are correct:

```
[host]$ ./demo.sh cpp run strand-cost
strand-cost: posts=20000 counter=20000 correct=yes

[host]$ ./demo.sh cpp run mutex-cost
mutex-cost: posts=20000 counter=20000 correct=yes
```

The difference is not correctness, it is what each one does to a thread that
loses the race, and Chapter 51 already built the instrument to see it:

```
[host]$ strace -f -c -e trace=futex cpp/build/release/asiodemo strand-cost
[host]$ strace -f -c -e trace=futex cpp/build/release/asiodemo mutex-cost
```

| mechanism | futex calls | counter |
| --- | --- | --- |
| strand | **52** | 20000 (correct) |
| `std::mutex` | **1022** | 20000 (correct) |

Roughly twenty times fewer trips into the kernel for byte-identical work. The
reason is structural, and it is the sentence to remember from this chapter:

**A mutex blocks a thread. A strand defers a handler.**

Chapter 51 measured exactly what blocking costs — a contended `std::mutex` falls
out of its userspace fast path into `futex(FUTEX_WAIT)`, the thread sleeps, and
someone has to `FUTEX_WAKE` it. That thread is doing nothing in the meantime. A
strand never blocks anybody: if a handler cannot run now because another one from
the same strand is running, the strand queues it and the thread goes back to the
`io_context` for other work. Nothing sleeps, so nothing needs waking.

The gate takes the sign and refuses the magnitude:

```lua
    checks.expect_match(tostring(n_strand < n_mutex / 2), "true",
      "cpp: the strand reached the kernel far less often than the mutex for identical work " ..
      "(strand=" .. tostring(n_strand) .. " futex calls, mutex=" .. tostring(n_mutex) ..
      ") -- a mutex BLOCKS a thread, a strand DEFERS a handler")
```

Three runs each on this host gave 52, 54, 55 against 1022, 992, 965 — the ratio
is rock solid and the individual counts move with scheduling every time, which is
why `strand < mutex/2` is the assertion and `== 52` would be a flake waiting to
happen.

What the strand pays instead is **ordering**. `asio::post` through a strand does
not run your handler now even when the calling thread is idle and nothing else
holds the strand; it hands the handler to the queue. If you need the work to
happen synchronously when it safely can, that is what `asio::dispatch` is for —
named here, not exercised.

## Who actually runs the handlers

{% include excalidraw.html
   file="55-topologies-and-gather"
   alt="Two Asio facts measured with earlier chapters' instruments. The upper band shows four shapes of run() over the same 400 posted handlers: one thread on one io_context reporting distinct_tids equals 1 and needing no synchronisation at all; an amber N-threads-on-one-context shape, 8 times ioc.run() over a shared queue, reporting distinct_tids equals 8, whose handlers need a strand; one io_context per thread reporting distinct_tids equals 4, where each loop owns its own descriptors; and asio::thread_pool(4) reporting distinct_tids equals 4 with no run() call written anywhere. A note records that the tids come from gettid(), ch50's instrument and ch49's vocabulary. The lower amber band contrasts asio::write given an array of four const_buffers, costing one socket write-family syscall -- a single sendmsg carrying 4 iovecs, 56 bytes, one trip into the kernel -- against four separate asio::write calls costing four sendto, the same 56 bytes in four trips. Footnotes observe that this is the readv and writev machinery Part 5 covered, reached by handing Asio a buffer sequence instead of assembling an iovec array by hand, and that these syscall counts are deterministic and are therefore gated exactly, with plain write(2) excluded because it is this program's own stdout."
   caption="Figure 55.2 — the same 400 handlers under four shapes of run(), and four buffers reaching the wire in one syscall instead of four" %}

A hand-written epoll loop answers "who runs the handler" by construction: the
thread that called `epoll_wait`, because there is only one. Asio makes it a
choice, and the choice is visible from outside the process with the same
instrument Chapter 50 used and the same vocabulary Chapter 49 established:

```
[host]$ ./demo.sh cpp run topology-one
topology-one: handlers=400 distinct_tids=1
[host]$ ./demo.sh cpp run topology-pool
topology-pool: handlers=400 distinct_tids=8
[host]$ ./demo.sh cpp run topology-percore
topology-percore: handlers=400 distinct_tids=4
[host]$ ./demo.sh cpp run topology-threadpool
topology-threadpool: handlers=400 distinct_tids=4
```

Four hundred identical handlers every time. Only the shape of the `run()` call
changes, and each shape has a different consequence for the code inside the
handler:

- **`one`** — one thread, one context. Every handler runs on the caller;
  `distinct_tids=1`. Chapter 49's *concurrent, not parallel*, and it needs no
  synchronization whatsoever, because there is nothing to synchronize against.
- **`pool`** — eight threads all calling `run()` on one context: a shared queue,
  `distinct_tids=8`. This is the shape that makes strands necessary.
- **`percore`** — one `io_context` per thread, `distinct_tids=4`. No sharing and
  no cross-thread handoff; each loop owns its own descriptors, which buys back
  the `one` shape's freedom from synchronization at the cost of not being able to
  balance load between the loops.
- **`threadpool`** — `asio::thread_pool(4)` owns its threads, `distinct_tids=4`,
  and there is no `run()` call written anywhere in that branch.

Gate F takes exactly one exact number and three signs:

```lua
checks.expect_match(out["topology-one"], "distinct_tids=1",
  "cpp: one thread calling run() on one io_context ran every handler itself")
```

`one` must be exactly 1, because that one is structural — a single thread cannot
report two tids. The other three are gated `> 1`, because how many of eight
threads actually pick up work before the queue drains is a scheduling outcome.
That is also why `pool` reports 8 rather than the 4 you might expect from its
name: it reuses the eight-thread helper the strand cases use.

## Four buffers, one syscall

The other thing a library does above the reactor is know the kernel's calling
conventions. Four pieces of a response:

```cpp
    const std::array<asio::const_buffer, 4> bufs{asio::buffer(kStatus), asio::buffer(kType),
                                                 asio::buffer(kBlank), asio::buffer(kBody)};
    const std::size_t wrote = asio::write(p.a, bufs);
```

against the same four written one at a time:

```cpp
    for (const std::string* s : {&kStatus, &kType, &kBlank, &kBody}) {
        wrote += asio::write(p.a, asio::buffer(*s));  // four separate calls
    }
```

Identical bytes arrive at the peer either way — 56 of them, `intact=yes` on both
— but counted by syscall they are not the same program at all:

| form | socket write-family syscalls |
| --- | --- |
| `asio::write(sock, array<const_buffer,4>)` | **1** (`sendmsg`, 4 iovecs) |
| four separate `asio::write` calls | **4** (`sendto`) |

An Asio *buffer sequence* is not a convenience wrapper around a loop. It maps
onto the kernel's iovec form, which is Part 5's `readv`/`writev` machinery
reached without assembling an `iovec` array and getting its lengths right by
hand.

These counts are gated exactly rather than as a ratio, and the difference from
gate D is the point: syscall counts here are deterministic, while futex counts
are a scheduling outcome. Gate what is deterministic; gate the sign of what is
not.

One detail in that gate is load-bearing:

```lua
  local r = checks.run("strace -f -c -e trace=writev,sendmsg,sendto " .. BIN .. " " .. case ..
                       " 2>&1 | awk '/writev|sendmsg|sendto/{s+=$4} END{print s+0}'")
```

Plain `write(2)` is excluded. This program prints to stdout, and counting its own
`printf` traffic would swamp a comparison between 1 and 4. The gate also sums
three syscall names rather than matching one, because whether a given socket type
ends up in `sendmsg` or `writev` is a kernel implementation detail — the *count*
is the claim, not the spelling.

## Cancellation, the third shape

Part 14 has now measured three different answers to "stop that":

| chapter | mechanism | what arrives |
| --- | --- | --- |
| 51 | `std::stop_token` | a **flag** the target polls |
| 52 | `boost::thread::interrupt()` | an ordinary **exception**, at defined points |
| 55 | `asio::cancellation_signal` | a **completion** carrying an error code |

```
[host]$ ./demo.sh cpp run cancel
cancel: handlers_run=1 handler_ran=yes error=Operation canceled aborted=yes
```

A timer with an hour left on it completed immediately. The operation did not
vanish and the handler was not dropped — it ran, exactly once, with
`ec == asio::error::operation_aborted`.

That is a genuinely different shape from the other two, and its practical
consequence is about ownership rather than about control flow: because a
cancelled operation still completes exactly once, everything the handler owns —
the buffer it was going to fill, the `shared_ptr` keeping the connection alive —
is released on the same path whether the operation succeeded or was cancelled.
Chapter 52's exception model has to unwind to achieve that; Chapter 51's flag
model leaves it entirely to you.

## How the code works

Every measurement is its own subcommand, which by Part 14 is a habit with a
reason: `nostrand` deliberately contains a data race, and a process containing
one must not be able to influence the cases that do not.

The socketpair is what keeps the I/O cases offline:

```cpp
struct Pair {
    asio::io_context ioc;
    asio::local::stream_protocol::socket a{ioc};
    asio::local::stream_protocol::socket b{ioc};
    Pair() { asio::local::connect_pair(a, b); }
};
```

`local::connect_pair` is `socketpair(2)` in AF_UNIX with Asio's socket type
wrapped around both ends. Nothing binds, nothing listens, no port is claimed, and
the example runs identically on a machine with no network at all.

The build carries one deliberate definition:

```cmake
target_compile_definitions(asiodemo PRIVATE BOOST_ASIO_NO_DEPRECATED)
```

which removes `io_service`, `deadline_timer`, and the rest of the pre-1.66
spellings from the translation unit entirely. They still work, they are still all
over the internet, and a chapter that accidentally demonstrated one would be
teaching an interface Asio has replaced.

## Errors, three ways

**An error the type system catches.** `asio::write` on a buffer sequence is the
same function as `asio::write` on a single buffer — the sequence concept is a
compile-time property, so handing it four buffers instead of one is not a
different call to get wrong. There is no length array to desynchronize from the
pointer array, which is the classic `writev` bug Part 5 had to be careful about.

**An error delivered as a value.** `operation_aborted` arrives in an
`error_code`, in the completion handler, on the normal path. So does every other
Asio error: the overload of `drain` used here passes an `error_code` in and reads
it back, because reading to EOF on a closed socket *reports* an error that is not
one:

```cpp
std::size_t drain(asio::local::stream_protocol::socket& s) {
    std::vector<char> in(4096);
    boost::system::error_code ec;
    return asio::read(s, asio::buffer(in), ec);
}
```

The throwing overload would have raised `eof` there. Choosing the `error_code`
overload is the same decision Chapter 22 made with `EAGAIN` — some conditions the
kernel reports as errors are simply how the operation ends.

**An error nothing catches.** Forgetting the strand. `nostrand` compiles cleanly,
runs to completion, exits 0, and produces a plausible number that happens to be
wrong; no exception, no error code, no crash, and on a single-threaded
`io_context` it would even be correct. Nothing in the type system distinguishes
`post(strand, h)` from `post(ioc, h)` — both are valid executors — so this is a
defect that only counting can find, which is why this example counts.

## Concurrency lens

The rule the whole chapter reduces to: **a handler is not thread-safe merely
because Asio gave it to you.** Under `topology-one` you can touch anything you
like without a lock. Under `topology-pool` the identical handler is a shared
mutable-state problem, and the number of threads calling `run()` is the only
thing that changed.

What a strand gives you is worth stating precisely, because it is narrower than
"a lock". It guarantees that handlers *on that strand* do not run concurrently,
and that they do not overlap. It guarantees nothing about handlers on a different
strand, or about a thread that touches the same data outside a handler entirely.
The strand is a serialization domain, not a mutual-exclusion primitive attached
to your data — which is why Asio expects you to associate a strand with an
*object* (a connection, a session) and then post everything touching that object
through it.

Set against the models of the preceding chapters:

- Chapter 50's pthreads and Chapter 51's `std::thread` synchronize by **blocking a
  thread**. Chapter 51 measured the cost as futex traffic; this chapter reuses
  that instrument to show the strand avoiding it.
- Chapter 54's fibers under `round_robin` get their safety from **never
  migrating** — a fiber's data is touched by one thread by construction. Asio's
  `topology-percore` is the same bargain at the level of an event loop, and
  `topology-pool` is the trade in the other direction.
- Chapter 53's coroutines suspend only at `co_await` and only in a coroutine body.
  Asio's completion handlers have the same property from a different direction: a
  handler runs to completion, so anything it does between entry and return is
  atomic with respect to other handlers *on the same strand*.

And the ordering caveat is real. A strand serializes but does not order handlers
against anything outside itself, and `post` is explicitly "later, not now". Code
that assumed a handler had already run because the call to `post` returned is
broken in the same way as code that assumed `run()` would keep going with nothing
outstanding.

## Build, run, observe

```
[host]$ cd examples/55-boost-asio
[host]$ ./demo.sh cpp build
[host]$ for c in versions work-guard strand nostrand strand-cost mutex-cost \
                 gather separate cancel topology-one topology-pool \
                 topology-percore topology-threadpool; do
          ./demo.sh cpp run "$c"; done
```

The two cases whose claim is a syscall count are worth running under `strace`
yourself, since that is where the evidence actually lives:

```
[host]$ strace -f -c -e trace=futex cpp/build/release/asiodemo strand-cost
[host]$ strace -f -c -e trace=futex cpp/build/release/asiodemo mutex-cost
[host]$ strace -f -c -e trace=writev,sendmsg,sendto cpp/build/release/asiodemo gather
[host]$ strace -f -c -e trace=writev,sendmsg,sendto cpp/build/release/asiodemo separate
```

Then the harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: a fresh io_context::run() returns after ZERO handlers -- it returns on no outstanding work, not on completion
ok: cpp: through a strand, an UNSYNCHRONIZED counter incremented by 8 threads is exactly right (counter=2000)
ok: cpp: never more than one strand handler in flight, across 8 threads calling run()
ok: cpp: and zero overlapping entries -- serialization without a mutex
ok: cpp: the SAME handler posted straight to the io_context overlaps (1394 overlapping entries, max_inflight=5) -- only the executor changed
info: the unguarded counter's lost-update count is NOT gated -- an unsynchronized increment is a data race and therefore UB, so a run that loses nothing is legal. The gate asserts OVERLAP, which is measured with atomics and well defined.
ok: cpp: the strand reached the kernel far less often than the mutex for identical work (strand=52 futex calls, mutex=1022) -- a mutex BLOCKS a thread, a strand DEFERS a handler
ok: cpp: four buffers written as ONE sequence cost exactly one socket write-family syscall (got 1) -- the kernel's iovec form, reached without assembling one by hand
ok: cpp: the identical four buffers written separately cost four (got 4)
ok: cpp: one thread calling run() on one io_context ran every handler itself
ok: cpp: topology-pool spread handlers across 8 OS threads -- same handlers, different shape of run()
ok: cpp: a cancelled operation completes with operation_aborted -- the handler still runs exactly once, so ownership rules never change
PASS 48 / FAIL 0
```

## Cross-check: the same sixteen bytes, still

All thirteen cases reported `digest=0x481984990deee5ff`, and the clang build
agrees — the same value Chapters 46 through 54 have been carrying, now across two
compilers and seven concurrency models.

The sharper cross-check is that this chapter borrowed every one of its
instruments rather than inventing any. The futex counter is Chapter 51's, applied
to a mechanism Chapter 51 did not have. `gettid()` is Chapter 50's, applied to an
I/O runtime rather than to threads you created yourself. The iovec claim lands on
Part 5's `writev`. The rule about never asserting on the output of undefined
behavior is Chapter 49's, and it is the reason the most dramatic number in the
chapter — 138 lost increments — appears nowhere in a gate.

That is also the answer to what a library adds. Not readiness: Chapter 9 has
readiness. What Asio adds is a name for the thing a single-threaded loop never
needed to name, and once you can name it, you can count it.

## What you learned

- **`io_context::run()` returns when there is no outstanding work**, not when you
  are finished. A fresh context runs zero handlers and returns immediately;
  `executor_work_guard` is the claim that keeps it alive.
- **A strand is an executor that serializes handlers without a lock.** The same
  handler on 8 threads left a deliberately unsynchronized counter at exactly 2000
  with `max_inflight=1` and `overlaps=0`.
- **Without one, the same code loses increments for real** — 138 of 2000 on the
  quoted run, a data race visible as a wrong number rather than as a warning.
- **Never gate on the output of undefined behavior.** The lost count varied 22 to
  138 across runs and is only reported; the overlap is measured with atomics, is
  well defined, and is what the gate asserts.
- **A mutex blocks a thread; a strand defers a handler.** Both produced the
  correct 20000, at 52 futex calls against 1022 — Chapter 51's instrument pointed
  at Chapter 51's alternative.
- **What a strand costs is ordering, not throughput.** `post` means later even
  when now would have been safe; `dispatch` is the other choice.
- **A buffer sequence is the kernel's iovec form.** Four buffers as one sequence
  cost one `sendmsg` carrying 4 iovecs; the same four written separately cost
  four `sendto`, for identical bytes.
- **Gate what is deterministic exactly, and gate the sign of what is not.**
  Syscall counts are exact gates here; futex counts are a ratio.
- **Who runs your handler is your choice, and it is visible from outside.** The
  same 400 handlers reported 1, 8, 4, and 4 distinct tids under four shapes of
  `run()` — and a handler is not thread-safe merely because Asio handed it to you.
- **Asio's cancellation is a completion, not an exception or a flag.** A cancelled
  operation still completes exactly once, with `operation_aborted`, so ownership
  rules are identical on both paths.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.8-200.fc44, glibc 2.43, <strong>Boost
1.90.0 from <code>boost-devel-1.90.0-7.fc44</code></strong>, g++ 16.1.1
20260515, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, Lua 5.4.8, 16 logical / 8
physical CPUs; local, unprivileged, <strong>no Conan, no network, and nothing
bound to a port</strong>, no VM): <code>LSP_LANG=cpp REPO_ROOT=$(cd ../..
&amp;&amp; pwd) lua verify.lua</code> reported <code>PASS 48 / FAIL 0</code> with
gates C (the strand), D (futex counts under <code>strace</code>), E
(scatter-gather syscall counts under <code>strace</code>), F (topologies) and G
(clang parity) all running for real, and <code>python3
scripts/test-all-examples.py --only 55-boost-asio</code> reported <code>1 passed,
0 failed, 0 skipped</code>. Every transcript quoted above is a real run from this
session. Confirmed live: all thirteen cases produced
<code>digest=0x481984990deee5ff</code>, byte-identical to Chapters 46 through 54
and matched by the clang build; <code>work-guard</code> reported
<code>without_work_handlers=0 with_guard_handlers=1</code>; <code>strand</code>
reported <code>counter=2000 max_inflight=1 overlaps=0</code> and
<code>nostrand</code> reported <code>counter=1862 lost=138 max_inflight=6
overlaps=1492</code> for the identical handler, with five further runs losing 22,
110, 104, 44 and 24 (overlaps 1375-1507) — the variation is why the lost count is
reported and never gated, and Figure 55.1 records a further run at 1897 of 2000
with 1441 overlaps; <code>strand-cost</code> and <code>mutex-cost</code> both
reported <code>counter=20000 correct=yes</code> at 52 versus 1022 futex calls in
the gated run, with three further pairs at 52/54/55 against 1022/992/965;
<code>gather</code> cost exactly <strong>1</strong> socket write-family syscall
(<code>sendmsg</code>) and <code>separate</code> exactly <strong>4</strong>
(<code>sendto</code>), both moving 56 bytes with <code>intact=yes</code>;
<code>topology-one|pool|percore|threadpool</code> reported
<code>distinct_tids=</code> <code>1</code>, <code>8</code>, <code>4</code>,
<code>4</code>; <code>cancel</code> reported <code>handlers_run=1</code> with
<code>error=Operation canceled aborted=yes</code>. Not exercised: <span
class="status status--unverified">unverified</span> — no timing of any kind is
measured or gated in this example (Chapter 39); every claim here is a handler
count, a syscall count, a tid count, or a value that is either correct or not.
<code>asio::dispatch</code>, <code>asio::co_spawn</code> with
<code>use_awaitable</code>, <code>asio::awaitable</code>, and Asio's
<code>ssl</code> and resolver facilities are named or alluded to in the chapter
but are not built, run, or gated. The claim that Asio's Linux backend uses
<code>epoll_wait</code> is Asio's documented implementation rather than something
this example strace's for, since the reactor itself is Chapter 9's subject and is
deliberately not re-derived here. Nothing in this example binds a port or
performs any network I/O: both I/O cases run over an AF_UNIX socketpair from
<code>local::connect_pair</code>.</p>
