---
title: "r19 / ch50 — pthreads — plan (internal)"
published: false
---

# r19 ch50 — example `50-pthreads` + chapter + diagrams

Second chapter of Part 14 (Compendium: C++ Concurrency). ch49 set the vocabulary
(concurrency is structure, parallelism is execution) and measured it with `std::thread`.
ch50 goes **under** `std::thread` to the thing it is on Linux: a pthread.

The thesis: `std::thread` is a portable wrapper that deliberately exposes none of the
per-thread controls Linux actually offers. Every one of those controls is reachable
through `native_handle()`, and every one of them has an effect visible in `/proc` or in
a scheduler syscall. This is the chapter where a thread stops being an abstraction and
starts being a task with a tid, a name, a stack, an affinity mask, and a policy.

## Decisions (answered 2026-08-16)

- **D1 — Six facets.** identity, naming, stack sizing, per-thread affinity, scheduling
  policy, cancellation. Scheduling is included even though the interesting call
  (`SCHED_FIFO`) *fails* unprivileged — the refusal is the observable.
- **D2 — Hard-gate both cancellation behaviors.** Destructors run during
  `pthread_cancel`, AND swallowing the forced unwind aborts with
  `FATAL: exception not rethrown`.
- **D3 — No forced C++26 section.** Instead "what the standard still does not give you",
  grounded in measured facts, with `native_handle()` as the sanctioned bridge.
- **D4 — Autonomous through PR; the user merges.**

## Host audit (run 2026-08-16, Fedora 44, kernel 7.1.5-201.fc44)

All measured with real probe programs, not inferred:

| fact | measured value |
| --- | --- |
| glibc | **2.43** |
| `PTHREAD_STACK_MIN` | **16384** |
| main thread: `gettid()` vs `getpid()` | **equal** (2483509 both) — the main thread's tid *is* the pid |
| main thread stack (`pthread_getattr_np`) | 8376320 (~7.99 MiB) — RLIMIT_STACK-derived, *not* a round number |
| worker default stack | **8388608** (exactly 8 MiB) — differs from main's |
| `pthread_setname_np`, 15 chars | 0 (ok) |
| `pthread_setname_np`, 16 chars | **ERANGE (34)** — kernel `comm` is 16 bytes *including* the NUL |
| name visible at | `/proc/self/task/<tid>/comm` |
| `pthread_attr_setstacksize(256 KiB)` | 0; worker then observes **exactly 262144** |
| `pthread_attr_setstacksize(8 KiB)` | **EINVAL (22)** — below `PTHREAD_STACK_MIN` |
| `pthread_setaffinity_np(cpu 3)` | 0; worker `sched_getcpu()`=3 while **main stayed on cpu 1** |
| `pthread_getschedparam` | policy=0 (`SCHED_OTHER`), prio=0 |
| `pthread_setschedparam(SCHED_FIFO, 50)` unprivileged | **EPERM (1)** |
| `pthread_cancel` + `pthread_cleanup_push` | handler ran; `pthread_join` retval == `PTHREAD_CANCELED` |
| `pthread_cancel` vs C++ RAII | **destructors run** — glibc unwinds |
| `catch (...)` without rethrow during cancel | **`FATAL: exception not rethrown`, exit 134** |
| `std::thread::native_handle_type` | `static_assert(is_same_v<..., pthread_t>)` **passes** |
| `__cpp_lib_jthread` | 201911 |

Toolchain unchanged from r18: GCC 16.1.1, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0,
16 logical / 8 physical CPUs. Local, unprivileged, offline. No Boost yet (ch52 starts that).

## The core demo — `pthreads` (C++-only, `langs: [cpp]`)

One program, one subcommand per facet, so a trapping or aborting facet can never take
the others with it. Digest continuity: every facet still reports
`digest=0x481984990deee5ff` over `"The quick brown."` (ch46/47/48/49's payload).

Facets and their observables:

1. **identity** — `pthread_t` (opaque handle) vs `gettid()` (the kernel's task id) vs
   `getpid()`. Assert main's tid == pid, workers' tids differ from both and from each
   other, and each tid has a directory under `/proc/self/task/`.
2. **naming** — `pthread_setname_np`, read back two ways: `pthread_getname_np` and
   `/proc/self/task/<tid>/comm`. Assert both agree, and that a 16-char name is refused
   with `ERANGE` while the previous name survives unchanged.
3. **stack** — one thread at the default, one at 256 KiB via
   `pthread_attr_setstacksize`. Assert the custom thread observes exactly 262144 via
   `pthread_getattr_np`, that it differs from the default, and that a request below
   `PTHREAD_STACK_MIN` is refused with `EINVAL`.
4. **affinity** — `pthread_setaffinity_np` per thread. Assert worker A and worker B
   report *different* `sched_getcpu()` values and that the main thread's mask is
   unchanged — the ch49 contrast: that chapter pinned the whole process, this one pins
   one thread.
5. **sched** — `pthread_getschedparam` reports `SCHED_OTHER`/0; an unprivileged
   `SCHED_FIFO` request is refused with `EPERM`. Gate the refusal, not a success.
6. **cancel** — two sub-cases, both hard-gated per D2.

## Gate tiers

**Hard (A–G), all local, unprivileged, offline:**
- A. digest identical across all six facets + `consensus=yes`.
- B. identity: main tid == pid; worker tids distinct; `/proc/self/task/<tid>` exists.
- C. naming: `pthread_getname_np` and `/proc` `comm` agree; 16 chars → `ERANGE` and the
  old name is intact.
- D. stack: custom == 262144, default != custom, sub-minimum → `EINVAL`.
- E. affinity: two workers report different CPUs; main's `cpus_allowed` unchanged.
- F. sched: policy is `SCHED_OTHER`; `SCHED_FIFO` → `EPERM`.
- G. cancel: cleanup handler ran AND `~Guard` ran AND retval == `PTHREAD_CANCELED`;
  the swallow case aborts with `FATAL: exception not rethrown`.

**Gated-if-present (H):** clang parity on the digest, as ch49.

**Not gated:** anything needing root (`SCHED_FIFO` success, `RLIMIT_RTPRIO`). Named in
the chapter as requiring privilege, with the `EPERM` we *do* get as the evidence.

## Risks

- **Affinity gate E on a 1-CPU host.** Two threads cannot occupy two CPUs if only one
  exists. Guard: read `CPU_COUNT(sched_getaffinity)` first and print an informational
  SKIP for E when it is 1, exactly as ch48 handled absent tools. Never a false PASS.
- **`FATAL: exception not rethrown` is a glibc string.** It is glibc 2.43 here and the
  message has been stable for many releases, but the gate should match loosely
  (`exception not rethrown`) and the footer must name the glibc version.
- **Stack size rounding.** 256 KiB came back exactly, but a page-rounded value is
  permitted. Assert `>= 262144` and `< default`, not strict equality, and say so.
- **Thread names are truncated, not rejected, by the kernel** when set through other
  paths. Our path is `pthread_setname_np`, which returns `ERANGE` instead — assert the
  glibc behavior we measured, and say which layer refuses.

## Steps

- S1 mint `examples/50-pthreads` via `scripts/new-example.sh`, strip go/rust, single-lang
- S2 write `cpp/src/pthreads.cpp` (six facets) + CMakeLists + demo.sh
- S3 write `verify.lua` (gates A–G hard, H skip-if-present, E skip-if-1-CPU)
- S4 register in `examples/manifest.yaml` (`langs: [cpp]`, `mode: local`)
- S5 capture all transcripts from real runs
- S6 write `_docs/50-pthreads.md`
- S7 2 diagrams + catalogue rows in `assets/diagrams/README.md`
- S8 gate matrix: verify.lua, runner, validate.py, verbatim-block check, banned words
- S9 PR (per D4, stop there)

## Acceptance

`LSP_LANG=cpp lua verify.lua` PASS/FAIL 0 with E and H running for real on this host;
runner 1 passed; `validate.py` OK; every chapter code block a verbatim substring of a
real source or a real transcript; footer states glibc 2.43 and the exact gate record.

## Outcome (2026-08-17)

All steps complete. `LSP_LANG=cpp lua verify.lua` → **PASS 45 / FAIL 0**, with gate E
(affinity, needs ≥2 CPUs) and gate I (clang parity) both running for real on this host.
Runner → 1 passed. `validate.py` → OK. Banned-words clean. **15/15 chapter code blocks
verified as verbatim substrings** of `cpp/src/pthreads.cpp` or `verify.lua`; every
`Chapter N` cross-reference resolves to a real file.

Deltas from the plan, all in the safe direction:

- **Gate count 45, not the ~35 estimated** — the six facets each yielded more assertable
  fields than planned once written.
- **Gate D asserts `>=` requested, not `==`.** Measured exactly 262144, but page rounding
  is permitted and asserting equality would gate on an implementation detail.
- **The cancel-swallow gate matches `exception not rethrown` loosely**, without the
  `FATAL:` prefix, since the surrounding text is a glibc string. glibc 2.43 named in the
  footer as planned.
- **`~Guard` runs BEFORE the `pthread_cleanup_push` handler** — the handler was pushed
  first and cleanup handlers pop LIFO, same as destructors. Worth a sentence in the
  chapter; it shows the two mechanisms interleave rather than one excluding the other.
- **The `ps -L` observation step needed correcting before it shipped.** The first draft
  claimed `comm` shows the `pthread_setname_np` name; measured, the `cancel` facet does
  not rename its worker, so both rows show the process name. Text now says which facets
  change it, and the transcript is a real capture.
- **`PTHREAD_STACK_MIN` is a `sysconf` call on glibc, not a compile-time constant** —
  printed at run time in the demo rather than baked in.

No C++26 section, per D3. The "what the standard still does not give you" section states
the measured absence (no naming, stack, affinity, or scheduling API in C++23 on GCC 16 /
clang 22) and lands on `native_handle()` as the sanctioned bridge, `static_assert`ed to
be `pthread_t` here.
