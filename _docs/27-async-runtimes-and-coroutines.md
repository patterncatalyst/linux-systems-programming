---
title: "Async runtimes and coroutines"
order: 27
part: "Concurrency in Depth"
description: "chatterd v4 tells the same broadcast three async ways over the same OS threads: hand-rolled C++20 coroutines suspending on an epoll reactor, Go's G-M-P scheduler and netpoller that needed no rewrite, and tokio's tasks-and-Wakers — with /proc/<pid>/task thread counts (1 vs 17 vs dynamic) and a single-epoll-loop strace showing how each spends threads, plus the framework for when plain threads still win."
duration: "60 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Async Runtimes and Event Loops](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/12-async-runtimes-and-event-loops/)** there.

The full, runnable C++ treatment of concurrency and parallelism — threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns — now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
