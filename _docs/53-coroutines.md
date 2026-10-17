---
title: "C++20 coroutines: what a suspended computation costs, and the allocation that may not happen"
order: 53
part: "Compendium: C++ Concurrency"
description: "coro measures what Chapter 27 built but never priced. A coroutine frame is measured by overloading operator new inside promise_type -- 32 bytes trivial, 304 carrying four ints and a char[256], 4128 carrying a char[4096] -- against the 8388608-byte thread stack Chapter 50 measured with pthread_getattr_np, a ratio of 262144x. Heap allocation elision turns out to be a permission rather than a guarantee: across 1000 calls to the easiest possible case, g++ 16.1.1 elides none at -O0 through -O3 while clang++ 22.1.8 elides every one from -O1 up, so verify.lua gates that the count is measured rather than that elision occurs. Plus C++23 std::generator exercised to the 20th Fibonacci term and the coroutine_handle lifetime trap counted through operator delete. Verified on the Fedora 44 host: verify.lua PASS 32/FAIL 0, C++23 stdlib only, no threads, no network."
duration: "55 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Coroutines in Depth](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/11-coroutines-in-depth/)** there.

The full, runnable C++ treatment of concurrency and parallelism — threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns — now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
