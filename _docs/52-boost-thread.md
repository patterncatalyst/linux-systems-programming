---
title: "Boost.Thread: what the standard took, what it left, and the third way to cancel a thread"
order: 52
part: "Compendium: C++ Concurrency"
description: "boostthread asks what survived C++11's near-wholesale standardization of Boost.Thread, and answers with four capabilities std:: still lacks in C++23 on this toolchain: future::then() and when_all composing without blocking, where a concept evaluated against std::future<int> and boost::future<int> on every run reports no and yes; upgrade_lock promoting a reader to a writer in place, where std::upgrade_lock is not merely absent but undeclared; thread::interrupt() throwing an ordinary boost::thread_interrupted at defined interruption points; and thread::attributes::set_stack_size yielding exactly 262144, byte-identical to the pthreads chapter's pthread_attr_setstacksize. The cancellation arc closes here: catch (...) without a rethrow aborts the process under the pthreads chapter's pthread_cancel (exit 134) and leaves it alive under Boost (exit 0), while a busy loop with no interruption point ignores interrupt() entirely. Verified against system Boost 1.90.0 on the Fedora 44 host: verify.lua PASS 37/FAIL 0, no Conan, no network."
duration: "55 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Threads in Modern C++](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/04-threads-in-modern-cpp/)** there.

The full, runnable C++ treatment of concurrency and parallelism (threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns) now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
