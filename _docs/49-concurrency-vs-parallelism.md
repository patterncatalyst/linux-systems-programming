---
title: "Concurrency vs parallelism: one workload, four execution models, and observables that are counts rather than clocks"
order: 49
part: "Compendium: C++ Concurrency"
description: "conc runs one identical workload under four execution models -- sequential, concurrent (pinned to one CPU by sched_setaffinity), parallel (one thread per CPU), and both (2x oversubscribed) -- and reports structure rather than time: cpus_allowed from sched_getaffinity as a permission, distinct_cpus unioned from sched_getcpu as an outcome, a max_inflight high-water mark, and interleaves counted from gaps in a monotonic ticket sequence. All four converge on digest=0x481984990deee5ff, the same value ch46's C++, ch47's Go, and ch48's Rust toolbox produce. Verified on the Fedora 44 host: verify.lua PASS 43/FAIL 0, concurrent reporting cpus_allowed=1 distinct_cpus=1 max_inflight=16 interleaves=16 while parallel reports distinct_cpus=16, C++26 Contracts built four times for four distinct behaviors under GCC 16, and a hardened build trapping on __n < this->size() where the unchecked build is asserted statically to contain no bounds check at all."
duration: "55 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Concurrency vs Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/01-concurrency-vs-parallelism/)** there.

The full, runnable C++ treatment of concurrency and parallelism (threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns) now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
