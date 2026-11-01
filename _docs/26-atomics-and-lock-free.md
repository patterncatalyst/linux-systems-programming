---
title: "Atomics and lock-free: memory ordering, the SPSC ring, and false sharing"
order: 26
part: "Concurrency in Depth"
description: "spscring drops the lock entirely: a single-producer/single-consumer ring coordinated by two atomic counters, with acquire/release explained on the real head/tail, why a non-atomic ring tears, the ABA problem SPSC sidesteps, and false sharing measured with perf: a padded line beats an unpadded one by ~34% on this host."
duration: "55 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Atomics and the Memory Model](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/08-atomics-and-the-memory-model/)** there.

The full, runnable C++ treatment of concurrency and parallelism, covering threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns, now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
