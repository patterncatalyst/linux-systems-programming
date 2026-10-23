---
title: "Examples"
layout: plan
permalink: /examples/
description: "Every runnable example in this repo. Each ships the same behavior in C++23, Go, and Rust."
---

Each hands-on chapter has a matching runnable example under
[`examples/`](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples).
Every example ships the **same observable behavior in C++23, Go, and Rust**, with one
`verify.lua` asserting that behavior across all three. Run one with
`./demo.sh [cpp|go|rust] [build|run]` from its directory; see
[Lab setup]({{ '/docs/02-lab-setup/' | relative_url }}) for the examples that need the KVM lab.

| Example | What it shows |
|---|---|
| [01-hello-syscalls](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/01-hello-syscalls) | Hello syscalls |
| [04-syscalls-and-the-abi](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/04-syscalls-and-the-abi) | Syscalls and the abi |
| [05-errors-three-ways](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/05-errors-three-ways) | Errors three ways |
| [06-concurrency-three-ways](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/06-concurrency-three-ways) | Concurrency three ways |
| [07-fds-raii-and-the-vfs](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/07-fds-raii-and-the-vfs) | Fds raii and the vfs |
| [08-page-cache-and-durability](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/08-page-cache-and-durability) | Page cache and durability |
| [09-event-loops-epoll-inotify](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/09-event-loops-epoll-inotify) | Event loops epoll inotify |
| [10-io-uring](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/10-io-uring) | Io uring |
| [11-process-lifecycle](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/11-process-lifecycle) | Process lifecycle |
| [12-signals-and-safe-handling](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/12-signals-and-safe-handling) | Signals and safe handling |
| [13-pidfd-modern-process-mgmt](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/13-pidfd-modern-process-mgmt) | Pidfd modern process mgmt |
| [14-identity-and-privilege](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/14-identity-and-privilege) | Identity and privilege |
| [15-virtual-memory](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/15-virtual-memory) | Virtual memory |
| [16-mmap-and-shared-mappings](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/16-mmap-and-shared-mappings) | Mmap and shared mappings |
| [17-allocators-and-gc-runtimes](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/17-allocators-and-gc-runtimes) | Allocators and gc runtimes |
| [18-pipes-fifos-splice](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/18-pipes-fifos-splice) | Pipes fifos splice |
| [19-unix-sockets-fd-passing](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/19-unix-sockets-fd-passing) | Unix sockets fd passing |
| [20-shared-memory-coordination](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/20-shared-memory-coordination) | Shared memory coordination |
| [21-tcp-sockets](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/21-tcp-sockets) | Tcp sockets |
| [22-scaling-the-server](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/22-scaling-the-server) | Scaling the server |
| [23-udp-and-peer-discovery](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/23-udp-and-peer-discovery) | Udp and peer discovery |
| [24-time-timers-deadlines](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/24-time-timers-deadlines) | Time timers deadlines |
| [25-shared-state-and-the-futex](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/25-shared-state-and-the-futex) | Shared state and the futex |
| [26-atomics-and-lock-free](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/26-atomics-and-lock-free) | Atomics and lock free |
| [27-async-runtimes-and-coroutines](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/27-async-runtimes-and-coroutines) | Async runtimes and coroutines |
| [28-gdb-and-remote-debugging](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/28-gdb-and-remote-debugging) | Gdb and remote debugging |
| [29-valgrind-sanitizers-miri](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/29-valgrind-sanitizers-miri) | Valgrind sanitizers miri |
| [30-ebpf-observation-toolkit](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/30-ebpf-observation-toolkit) | Ebpf observation toolkit |
| [31-per-language-toolbelts](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/31-per-language-toolbelts) | Per language toolbelts |
| [32-namespaces-and-cgroups](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/32-namespaces-and-cgroups) | Namespaces and cgroups |
| [33-seccomp-and-landlock](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/33-seccomp-and-landlock) | Seccomp and landlock |
| [34-our-programs-in-containers](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/34-our-programs-in-containers) | Our programs in containers |
| [35-virtualization-and-kvm](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/35-virtualization-and-kvm) | Virtualization and kvm |
| [36-proc-sys-and-the-agent](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/36-proc-sys-and-the-agent) | Proc sys and the agent |
| [37-use-method-60s-analysis](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/37-use-method-60s-analysis) | Use method 60s analysis |
| [38-opentelemetry-to-lgtm](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/38-opentelemetry-to-lgtm) | Opentelemetry to lgtm |
| [39-benchmarking-without-lies](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/39-benchmarking-without-lies) | Benchmarking without lies |
| [40-low-latency-fastpath](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/40-low-latency-fastpath) | Low latency fastpath |
| [41-capstone-fleet](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/41-capstone-fleet) | Final-project fleet |
| [42-embedded-lua-scripting](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/42-embedded-lua-scripting) | Embedded lua scripting |
| [43-rust-macros-for-systems](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/43-rust-macros-for-systems) | Rust macros for systems |
| [44-go-runtime-for-systems](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/44-go-runtime-for-systems) | Go runtime for systems |
| [45-linux-analysis-suites](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/45-linux-analysis-suites) | Linux analysis suites |
| [46-cpp-toolbox](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/46-cpp-toolbox) | Cpp toolbox |
| [47-go-toolbox](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/47-go-toolbox) | Go toolbox |
| [48-rust-toolbox](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/48-rust-toolbox) | Rust toolbox |
| [49-concurrency-vs-parallelism](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/49-concurrency-vs-parallelism) | Concurrency vs parallelism |
| [50-pthreads](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/50-pthreads) | Pthreads |
| [51-std-threading](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/51-std-threading) | Std threading |
| [52-boost-thread](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/52-boost-thread) | Boost thread |
| [53-coroutines](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/53-coroutines) | Coroutines |
| [54-boost-fiber](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/54-boost-fiber) | Boost fiber |
| [55-boost-asio](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/55-boost-asio) | Boost asio |
| [56-capstone-comparison](https://github.com/{{ site.github_username }}/{{ site.github_repo }}/tree/main/examples/56-capstone-comparison) | Final-project comparison |
