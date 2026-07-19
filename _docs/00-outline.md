---
title: "Outline"
order: 0
part: "Setting Up"
description: "The full arc of this book — fourteen parts, forty-nine chapters, six programs that grow with you, three languages the whole way."
---

This book teaches Linux systems programming three times at once — in modern
**C++23**, **Go**, and **Rust** — for developers who already program and want to
understand what their code asks of the kernel. Every hands-on chapter ships a
runnable example implemented idiomatically in all three languages, presented in
switchable code tabs, and every example doubles as a standalone demo.

Two threads run through the whole book and never stop:

- **Errors, three ways** — how `errno` becomes `std::expected`, a wrapped Go
  `error`, or a Rust `Result`, and what a *policy* for `EINTR`, `EAGAIN`, and
  partial failure looks like in each language.
- **The concurrency lens** — which of the three concurrency models each
  chapter's example leans on (`std::jthread` and atomics, goroutines and
  channels, ownership and `Send`/`Sync`), and why.

## The six programs you'll build

Rather than forty-nine throwaway snippets, the examples converge on six
artifacts that grow chapter by chapter:

| Artifact | What it becomes | Chapter arc |
|---|---|---|
| `pmon` | a process supervisor: fork/exec, signals, pidfds, privilege dropping, namespaces, pid 1 | 11 → 14, 18 → 19, 32, 34, 41 |
| `fwatch` | a file watcher: polling → inotify/epoll → io_uring → sandboxed with Landlock | 07 → 10, 33, 41 |
| `shmkv` | an mmap-backed key-value store shared across processes — and across languages | 16, 20 |
| `chatterd` | a peer-to-peer chat daemon: TCP → epoll at scale → UDP discovery → async → low-latency | 21 → 27, 34, 38, 40, 41 |
| `sysagent` | a metrics agent reading /proc, /sys, and cgroups, exporting OpenTelemetry | 36 → 38, 41 |
| `bugfarm` | a corpus of seeded bugs for learning gdb, valgrind, sanitizers, and eBPF tools | 28 → 31 |

## The parts

| Part | Theme | Chapters |
|---|---|---|
| **Setting Up** | Fedora 44 host, three toolchains, the `systems-*` KVM lab, the LGTM stack | 00–03 |
| **Foundations** | syscalls and the ABI; errors three ways; concurrency three ways | 04–06 |
| **Files and I/O** | fds and the VFS, page cache and durability, epoll event loops, io_uring | 07–10 |
| **Processes, Signals, Privilege** | process lifecycle, safe signals, pidfds, uids and capabilities | 11–14 |
| **Memory** | virtual memory, mmap and shared mappings, allocators and GC runtimes | 15–17 |
| **IPC** | pipes and splice, UNIX sockets and fd passing, shared-memory coordination | 18–20 |
| **Networking** | TCP servers, scaling with epoll, UDP peer discovery across two VMs, time and deadlines | 21–24 |
| **Concurrency in Depth** | futexes, atomics and lock-free rings, async runtimes and coroutines | 25–27 |
| **Debugging** | gdb and remote debugging, valgrind vs sanitizers vs miri, eBPF observation tools, per-language toolbelts | 28–31 |
| **Containers and Virtualization** | namespaces and cgroups, seccomp and Landlock, our programs in containers, KVM and the isolation spectrum | 32–35 |
| **Observability** | /proc and /sys by hand, the USE method, OpenTelemetry into Grafana/Loki/Tempo/Mimir | 36–38 |
| **Performance and Low Latency** | honest benchmarking, pinned-core fast paths, the capstone fleet | 39–41 |
| **Deep Dives** | embedding Lua, Rust macros for systems code, the Go runtime | 42–44 |
| **Appendices: Tooling** | Cockpit, SystemTap, and PCP; the C++, Go, and Rust toolboxes | 45–48 |

## Where things run

Examples run **locally** on your Fedora 44 machine unless they need root, kernel
observation, or a second host — those run in the disposable KVM lab
(`systems-target`, and `systems-peer` for two-host networking) that Chapter 2
provisions. Observability chapters use a local Podman LGTM stack (Grafana,
Loki, Tempo, Mimir) plus Performance Co-Pilot from its container image. Nothing
experimental ever runs un-snapshotted on your workstation.

> **Note** — every example section carries a verification status. A demo is
> marked *verified* only when it has produced its claimed observable effect on
> the stated environment — never merely because it compiled.
