---
title: "Shared state and the futex"
order: 25
part: "Concurrency in Depth"
description: "workq builds a bounded MPMC job queue three ways (std::mutex + condvars, a buffered channel, Mutex + Condvar) and we look under all of them at the one primitive doing the work: the futex, whose uncontended fast path is a userspace CAS with no syscall and whose slow path is FUTEX_WAIT/WAKE, proven by strace scaling from 5,186 to 488,818 calls, then broken under ThreadSanitizer and go -race while Rust refuses to compile the race at all."
duration: "50 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Shared State and the Futex](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/05-shared-state-and-the-futex/)** there.

The full, runnable C++ treatment of concurrency and parallelism (threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns) now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
