---
title: "pthreads: the thread under std::thread, its two identities, and the six controls the standard does not give you"
order: 50
part: "Compendium: C++ Concurrency"
description: "pthreads runs one facet per subcommand over the per-thread controls POSIX offers and the C++ standard library does not expose: gettid() versus the opaque pthread_t (and why the main thread's tid equals the pid), pthread_setname_np landing in /proc/self/task/<tid>/comm with a 16-character name refused as ERANGE, pthread_attr_setstacksize giving exactly 262144 against an 8388608 default with sub-PTHREAD_STACK_MIN requests refused as EINVAL, per-thread pthread_setaffinity_np putting two workers on two CPUs while the caller's mask is untouched, SCHED_OTHER at priority 0 with an unprivileged SCHED_FIFO refused as EPERM, and pthread_cancel unwinding C++ destructors, plus the trap where a catch (...) that does not rethrow aborts the process with FATAL: exception not rethrown. Verified on the Fedora 44 host with glibc 2.43: verify.lua PASS 45/FAIL 0, local, unprivileged, offline, with std::thread::native_handle_type static_asserted to be pthread_t."
duration: "55 minutes"
---

> **This chapter has moved.** The C++ concurrency material has its own dedicated, runnable book: **[C++ Concurrency and Parallelism](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/)**.
>
> It maps to **[Threads in Modern C++](https://patterncatalyst.github.io/cpp-concurrency-and-parallelism/docs/04-threads-in-modern-cpp/)** there.

The full, runnable C++ treatment of concurrency and parallelism, covering threads and `jthread`, mutexes and the memory model, atomics and lock-free structures, futures and coroutines, fibers, Asio, NUMA and mechanical sympathy, and the low-latency patterns, now lives in that book, each chapter with a Docker or KVM-lab demo and a Lua `verify.lua` behind it. This book keeps its Linux systems-programming material; the C++ concurrency compendium was extracted so it could grow on its own.
