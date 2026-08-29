---
title: "Roadmap"
layout: plan
render_with_liquid: false
---

# Roadmap

**Status: the book is complete.** 57 chapters across 15 parts, each with a
runnable example — 56 of them — in C++23, Go, and Rust.

This page is the release history. It records what shipped in each iteration and,
more usefully, what "shipped" was allowed to mean.

## What a finished chapter means here

Every chapter in this book carries a status footer, and those footers are the
point rather than a formality. The rule the whole project runs on:

> A demo is *verified* only when it produced its claimed observable effect on the
> stated environment.

Not "it compiled". Not "it exited 0". The effect had to happen, on a named
kernel, with named tool versions, and the footer says which parts of the chapter
were exercised and which were reasoned about. Where something could not be
demonstrated, the footer says so in the same breath — that is what the
<span class="status status--unverified">unverified</span> marker is for, and
several chapters carry both markers because parts of them are in each category.

Three consequences that shaped nearly every chapter:

- **Every number quoted as coming from a run came from a run.** Where a number
  moves between runs — futex counts, lost updates under a data race — the
  chapters say so and the automated gates assert the *sign* rather than the
  magnitude.
- **Nothing is gated on undefined behavior.** A chapter that demonstrates a data
  race reports what it lost and asserts something well-defined instead.
- **No chapter is gated on a duration.** Timings are not reproducible across
  machines, so every claim in the book is a count, a size, or a value that is
  either right or wrong.

Each example is checked by a `verify.lua` that asserts behavior rather than exit
status, and the whole set is run by a Python orchestrator against a local host
and a two-VM KVM lab.

## The parts

| Part | Chapters | Subject |
|---|---|---|
| 0 | 0–3 | Setting Up |
| 1 | 4–6 | Foundations |
| 2 | 7–10 | Files and I/O |
| 3 | 11–14 | Processes, Signals, Privilege |
| 4 | 15–17 | Memory |
| 5 | 18–20 | IPC |
| 6 | 21–24 | Networking |
| 7 | 25–27 | Concurrency in Depth |
| 8 | 28–31 | Debugging |
| 9 | 32–35 | Containers and Virtualization |
| 10 | 36–38 | Observability |
| 11 | 39–41 | Performance and Low Latency |
| 12 | 42–44 | Deep Dives |
| 13 | 45–48 | Appendices: Tooling |
| 14 | 49–56 | Compendium: C++ Concurrency |

## Release history

| Iteration | What shipped | Chapters |
|---|---|---|
| r01 | Site identity: tri-language systems-programming hero and footer | — |
| r02 | The machinery: KVM lab scripts, the tri-language example template, the Lua verify harness, the Python runner, validators, LGTM infrastructure | — |
| r03 | Part 0 — course map, prerequisites, the KVM lab, the observability stack | 0–3 |
| r04 | Parts 1–2, and the conventions the rest of the book inherited: the Tools-used box and the project's `CLAUDE.md` | 4–10 |
| r05 | Parts 3–5 — processes, signals and privilege; memory; IPC | 11–20 |
| r06 | Parts 6–7 — networking, and concurrency in depth | 21–27 |
| r07 | Part 8 — debugging | 28–31 |
| r08 | Part 9 — containers and virtualization | 32–35 |
| r09 | Parts 10–11 — observability, and performance and low latency | 36–40 |
| r10 | The capstone fleet: a supervised, capability-dropped, Landlock-sandboxed, OTLP-observed fleet across two lab VMs | 41 |
| r11 | An embedded Lua policy engine | 42 |
| r12 | Rust macros for systems | 43 |
| r13 | The Go runtime, for systems programmers | 44 |
| r14 | Linux analysis suites | 45 |
| r15 | The C++ toolbox | 46 |
| r16 | The Go toolbox | 47 |
| r17 | The Rust toolbox | 48 |
| r18 | Opens Part 14 — concurrency versus parallelism, the distinction the compendium rests on | 49 |
| r19 | pthreads | 50 |
| r20 | The standard threading library | 51 |
| r21 | Boost.Thread | 52 |
| r22 | C++20 coroutines | 53 |
| r23 | Boost.Fiber | 54 |
| r24 | Boost.Asio | 55 |
| r25 | The capstone comparison — one workload through every model, closing Part 14 | 56 |
| r26 | The full-matrix verification run across every example, and this page | — |

## About Part 14

The compendium was added after the main book was finished, and it exists because
the earlier chapters kept running into the same gap. Concurrency appears
throughout Part 7 and everywhere I/O does, but always in service of some other
subject. Part 14 takes one workload through every concurrency model C++ offers —
pthreads, the standard threading library, Boost.Thread, C++20 coroutines,
Boost.Fiber, and Boost.Asio — and ends by measuring all of them the same way,
which is the only thing that makes their numbers comparable.

That final chapter also settles a debt the part opened with. Chapter 49 measured
that this toolchain's standard library has no P2300 `std::execution`, and
promised a live comparison in Chapter 56 *if and only if* the toolchain had
caught up. It had not — so Chapter 56 demonstrates the reference implementation
instead, and says plainly that this is what it is doing.

## What is not here

- **eBPF is tooling, never a subject.** The book observes its own userspace
  programs with `bpftrace`, `bcc-tools`, and `bpftool`. It never writes kernel
  eBPF, and it never asks you to load experimental code on your own machine.
- **No third-party books are drawn on.** Every claim traces to a primary source —
  a manual page, a standard, a specification, a kernel document — or to a
  measurement taken on the reference host.
- **No benchmarks.** For the reason given above: nothing in the book is gated on
  a duration, so nothing in it should be read as a performance claim.
