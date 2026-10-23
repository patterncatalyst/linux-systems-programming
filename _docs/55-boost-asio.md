---
title: "Boost.Asio: the strand, the work guard, and who actually runs your handler"
order: 55
part: "Compendium: C++ Concurrency"
description: "asiodemo measures what a library adds on top of the reactor Chapters 9, 22, and 27 built by hand -- and re-derives none of it. A freshly constructed io_context::run() returns after zero handlers, because it returns on no outstanding work rather than on completion. The same handler posted through a strand by 8 threads leaves a deliberately unsynchronized counter at exactly 2000 with max_inflight=1 and overlaps=0; posted straight to the io_context it overlaps 1492 times and loses increments -- a data race visible as a wrong number, whose magnitude is reported and never gated because it is UB. A strand and a std::mutex both produce the correct 20000, at 52 futex calls against 1022 counted with Chapter 51's strace method: a mutex blocks a thread, a strand defers a handler. Four buffers written as one sequence cost one sendmsg carrying 4 iovecs against four sendto, Part 5's writev machinery without an iovec array. Four shapes of run() report 1, 8, 4, and 4 distinct tids by gettid(). Cancellation arrives as a completion carrying operation_aborted -- a third shape after Chapter 51's flag and Chapter 52's exception. Verified against system Boost 1.90.0 on the Fedora 44 host: verify.lua PASS 48/FAIL 0, no Conan, no network, nothing bound to a port."
duration: "60 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Async Runtimes and Event Loops](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/12-async-runtimes-and-event-loops/)** there.

The full, runnable C++ treatment of concurrency and parallelism (threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns) now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
