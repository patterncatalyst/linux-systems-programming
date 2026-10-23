---
title: "The standard threading library: what a lock costs, and why the answer is zero until it isn't"
order: 51
part: "Compendium: C++ Concurrency"
description: "stdthread performs the identical 200,000 increments three ways -- with no mutex, with an uncontended std::mutex, and with the same mutex across eight threads -- and counts the syscalls each one issues with strace. The uncontended case matches the no-mutex baseline exactly at 1 futex call, which is the dynamic loader rather than a lock, while the contended case costs 1844-3111. std::condition_variable, std::latch, std::barrier, std::counting_semaphore, and std::atomic::wait are then each shown reaching the same futex(2), and a std::lock_guard deadlock is reproduced under timeout(1) with every task's /proc wchan reading futex_do_wait, against a std::scoped_lock version that completes 100,000 acquisitions in opposing orders. Verified on the Fedora 44 host with glibc 2.43: verify.lua PASS 38/FAIL 0, local, unprivileged, offline."
duration: "60 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Mutual Exclusion and Locking](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/06-mutual-exclusion-and-locking/)** there.

The full, runnable C++ treatment of concurrency and parallelism now lives in that book: threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
