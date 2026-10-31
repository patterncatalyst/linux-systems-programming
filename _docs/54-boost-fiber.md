---
title: "Boost.Fiber: stackful suspension, the guard page, and the scheduler you have to choose"
order: 54
part: "Compendium: C++ Concurrency"
description: "fiberdemo measures what a real stack costs and what it buys. boost::context::stack_traits reports a 131072-byte default fiber stack, sitting between the coroutines chapter's measured 32-byte coroutine frame and the pthreads chapter's measured 8388608-byte thread stack: 8 MiB to 128 KiB to 32 bytes, with both ratios computed from measured numbers. The capability that stack buys is demonstrated rather than described: a fiber suspends from inside an ordinary non-coroutine function six frames deep and resumes there, which no stackless coroutine can do. protected_fixedsize_stack is shown costing exactly one page more than fixedsize_stack, gated against page_size(), and a 60x overflow dies of SIGSEGV on that guard page rather than corrupting the next mapping. The same 16 fibers report one distinct tid under round_robin and four under shared_work, making the concurrency-vs-parallelism chapter's grid a scheduler choice. Verified against system Boost 1.90.0 on the Fedora 44 host: verify.lua PASS 32/FAIL 0, no Conan, no network."
duration: "55 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Fibers and Stackful Coroutines](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/13-fibers-and-stackful-coroutines/)** there.

The full, runnable C++ treatment of concurrency and parallelism now lives in that book: threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
