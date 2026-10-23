---
title: "The final project: one workload, seven models, and the two instruments that make them comparable"
order: 56
part: "Compendium: C++ Concurrency"
description: "This final project closes Part 14 by fixing the flaw in every chapter before it: ch50-ch55 each measured one model, with its own instrument, on its own workload, so no two numbers in the compendium were strictly comparable. Here one workload -- 8 tasks x 5000 rounds folding into a single accumulator, shared verbatim by every arm -- runs through all six models plus a sequential control, and all of them produce the identical total 0xb75768f6610642a0, which is gated before any comparison is drawn. Measured with gettid(), the three single-threaded models hold eight tasks in flight on exactly one OS thread while the four multi-threaded ones report eight. Measured with ch51's strace method, the same 40000 folds cost 1 futex call under any single-threaded model, 230 to 451 under a lock, and 51 through an Asio strand. A seventh arm settles Chapter 49's conditional promise about P2300: __cpp_lib_senders is still undefined, so it links NVIDIA's stdexec reference implementation, pinned by an in-repo Conan recipe to tag nvhpc-26.05 by sha256, isolated so the six-model core needs no Conan and no network. Verified on the Fedora 44 host: verify.lua PASS 57/FAIL 0 with Conan present and PASS 52/FAIL 0 with it hidden."
duration: "65 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Full Example: Ingest, Match, Publish](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/33-ingest-match-publish/)** there.

The full, runnable C++ treatment of concurrency and parallelism (threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns) now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
