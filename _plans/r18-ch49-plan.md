---
title: "r18 / ch49 — concurrency vs parallelism — plan (internal)"
published: false
---

# r18 ch49 — example `49-concurrency-vs-parallelism` + chapter + 2 diagrams + Part 14 scaffolding

**Opens Part 14 "Compendium: C++ Concurrency" (ch 49–56, C++-only).** First chapter of a new part,
so this iteration also ships the part index and the outline row — ch48 closed Part 13.

Framing chapter: concurrency is *structure* (a program dealing with many things at once),
parallelism is *execution* (a machine doing many things at once). The whole compendium's vocabulary
is set here; ch50–56 (pthreads → std threading → Boost.Thread → C++20 coroutines → Boost.Fiber →
stack-based threading for I/O → capstone comparison) all lean on it.

## Host audit (run 2026-08-07, Fedora 44, kernel 7.1.5-201.fc44)
- GCC **16.1.1**, clang **22.1.8**, CMake **4.3.0**, Ninja 1.13.0, Conan **2.30.0**.
- CPU: 16 logical / 8 physical cores, 2 threads-per-core, **1 NUMA node** (i7-11800H).
- Boost **1.90** system-installed (`boost-devel`), incl. `libboost_{context,fiber,thread}.so` →
  ch52/ch54 have their libraries already; **no Conan fetch needed for Boost on this host**.
- `libtbb.so.12` runtime present but **`tbb-devel` ABSENT** → `std::execution::par` (C++17 parallel
  algorithms) will not link (`-ltbb` has no `.so` symlink). Relevant because `<execution>` on GCC 16
  is the C++17 header, NOT P2300.

### C++26 "Fab Four" support, measured on this host (per [[cpp26-compendium-coverage]])
| feature | GCC 16.1.1 | clang 22.1.8 | verdict for ch49 |
| --- | --- | --- | --- |
| **Contracts (P2900)** | **WORKS** — `-fcontracts`, `__cpp_contracts = 202502`, and all four semantics via `-fcontract-evaluation-semantic=[ignore\|observe\|enforce\|quick_enforce]` | not probed | **LIVE, HARD-GATED** |
| **`std::execution` senders (P2300)** | **NO** — `__cpp_lib_senders` undefined; `<execution>` is C++17 par (`__cpp_lib_execution = 201902`) | — | forward-looking prose; stdexec-via-Conan is an option (see decision D2) |
| **Static reflection (P2996)** | **NO** — `__cpp_impl_reflection` undefined | **NO** — `-freflection` is an unknown argument | forward-looking prose only |
| **Safety hardening** | **PARTLY, and better than expected** — Fedora 44 turns libstdc++ assertions on **by default**: an unguarded `v[7]` on a size-3 `std::vector` already traps (`Assertion '__n < this->size()' failed`, SIGABRT/134) with **no** `-D_GLIBCXX_ASSERTIONS` needed. `__cpp_lib_hardened` undefined (the standard macro is not there yet). `-ftrivial-auto-var-init=[uninitialized\|pattern\|zero]` covers the P2795 erroneous-behavior story. | — | **LIVE, HARD-GATED** (the distro-default trap is a genuine finding worth a section) |

Captured contract transcripts already in hand (all four semantics, real runs): `ignore` → violation
passes silently, exit 0; `observe` → prints `[assertion_kind: pre, semantic: observe, mode:
predicate_false, terminating: no]` for pre *and* post, continues, exit 0; `enforce` → prints the
violation with `terminating: yes` then `terminate called`, exit 134; `quick_enforce` → terminates
with **no** diagnostic, exit 134. GCC also rejects `post (r: r > x)` on a non-`const` value
parameter — a real P2900 rule, and a good "the compiler teaches you the model" moment.

## The core demo
One binary, `conc`, running the **same workload** under four execution models, reporting
**structure, never wall-clock**:
1. `sequential` — one worker at a time, no interleaving.
2. `concurrent` — N workers interleaved, but the process pinned to **one** CPU.
3. `parallel` — N workers, unpinned, free to occupy many CPUs.
4. `both` — N workers, unpinned, with interleaving *and* multi-CPU occupancy.

Observables (all integer/string, all deterministic in the direction asserted):
- **distinct CPUs observed**, sampled per worker via `sched_getcpu()` and unioned into a set —
  `distinct_cpus=1` under pinning, `distinct_cpus>1` unpinned. This is the concurrency/parallelism
  distinction made *machine-checkable* rather than argued.
- **interleave events** — a count of observed worker-to-worker handoffs, proving the pinned run is
  genuinely concurrent (progress alternates) and the sequential run is not (zero handoffs).
- **affinity mask size** from `sched_getaffinity`, printed as `cpus_allowed=N`.
- the same **FNV-1a digest** over the merged result in every mode — the four models compute the
  *identical answer*, which is the framing chapter's actual thesis: the execution model is a
  property of the machine, not of the result.

Pinning is done **in-process** with `sched_setaffinity`, not with `taskset(1)`, so the gate has no
external-tool dependency and the chapter can show the syscall.

**No timings are asserted anywhere.** Wall-clock appears in prose only, explicitly labeled
non-reproducible, and cross-referenced to ch39's benchmarking discipline.

### Gate tiers
- **A HARD** build + digest parity: all four modes emit the identical `digest=0x…`; `conc --model
  sequential|concurrent|parallel|both` each exit 0.
- **B HARD** parallelism is real: `parallel` reports `distinct_cpus>1` on this 16-CPU host.
- **C HARD** concurrency without parallelism: `concurrent` reports `distinct_cpus=1` **and**
  `interleaves>0` — the two facts together are the chapter's whole point, and neither alone proves it.
- **D HARD** sequential is neither: `interleaves=0`.
- **E HARD** contracts (C++26, GCC 16): a second tiny binary built four times, once per semantic;
  assert `ignore` exit 0 with no diagnostic, `observe` exit 0 **with** the
  `semantic: observe, ... terminating: no` line, `enforce` nonzero **with** `terminating: yes`,
  `quick_enforce` nonzero **without** a diagnostic. Four distinct observable behaviors, not four
  exit codes.
- **F HARD** hardening: unguarded out-of-bounds `std::vector` subscript traps on this Fedora host and
  the assertion text names `__n < this->size()`; assert the trap **and** the message.
- **G gated-if-present** clang parity: if `clang++` is present, `conc` built by clang produces the
  identical digest (ch46's parity idea, one line of gate).
- P2300 / P2996: **CROSS-REF + forward-looking prose only**, no gate, no committed dead code.

verify.lua HARD-gates A–F; G is `tool_present("clang++")` skip-if-present, informational SKIP only.

### Isolation / anti-flake design
- `distinct_cpus>1` for the parallel model is the one assertion with a scheduler dependency. Mitigated
  by: 16 CPUs, a workload long enough to be sampled repeatedly per worker, and a **bounded retry** —
  the gate asserts "at least one of K runs saw >1 CPU", which is a real effect, not a timing.
  If it still proves flaky on a loaded host, the fallback is to assert `cpus_allowed=16` for the
  unpinned model and `cpus_allowed=1` for the pinned one (an affinity fact, fully deterministic) and
  demote `distinct_cpus` to reported-not-gated. **Decision D3 below.**
- The contract binaries are separate translation units under `cpp/contracts/`, built only by the gate,
  so a `terminating: yes` abort can never affect the main demo.
- The hardening fixture likewise lives in its own TU and is expected to abort; it is never linked into
  `conc`.

## Steps
S1 Part 14 scaffolding: `_parts/14-compendium-cpp-concurrency.md` (order 14, part_name
   "Compendium: C++ Concurrency") + `_docs/00-outline.md` "The parts" row. BLOCKING (front matter
   `part:` must match exactly).
S2 scaffold + strip (new-example.sh 49-concurrency-vs-parallelism, delete go/rust, single-lang demo.sh). BLOCKING.
S3 sources under cpp/: CMakeLists + CMakePresets (ch46 house style), `conc.cpp` (four models,
   sched_getcpu/sched_setaffinity/sched_getaffinity, FNV-1a digest over the same 16-byte payload as
   ch46–48 → `0x481984990deee5ff`, closing the easter egg into Part 14), `contracts/demo.cpp`,
   `hardening/oob.cpp`, README.md. Depends S2.
S4 verify.lua (A–F hard, G skip-if-present) + demo contract, LSP_LANG=cpp. Depends S3.
S5 build + capture: all gates, all four contract semantics, the hardening trap, clang parity. Depends S4.
S6 chapter `_docs/49-concurrency-vs-parallelism.md`. Depends S5. Parallel S7.
S7 2 diagrams (`49-concurrency-vs-parallelism` Fig 49.1, `49-execution-model-lanes` Fig 49.2)
   + README rows. Depends S3. Parallel S6.
S8 manifest (langs:[cpp], mode:local, timeout 600). After S2.
Collisions: manifest.yaml (S8), diagrams/README.md (S7), 00-outline.md (S1) only.

## Acceptance criteria
1. `_parts/14-*.md` exists with `part_name: "Compendium: C++ Concurrency"`; outline "The parts" table
   gains the Part 14 row; chapter front matter `part:` matches the part_name **exactly**.
2. no go/rust dir; manifest `49-concurrency-vs-parallelism` langs:[cpp] mode:local no requires.
3. `./demo.sh cpp build` exits 0 offline (system Boost not even needed for ch49 — stdlib only).
4. all four models print the identical `digest=0x481984990deee5ff` (== ch46/47/48).
5. `concurrent` → `distinct_cpus=1` **and** `interleaves>0`; `parallel` → `distinct_cpus>1`;
   `sequential` → `interleaves=0`.
6. four contract semantics → four distinct observable behaviors (diagnostic present/absent ×
   terminating/not), asserted on the diagnostic text, not just exit codes.
7. hardening fixture traps with the `__n < this->size()` assertion text on this host.
8. verify.lua PASS N / FAIL 0; A–F real effects; G fire-with-token OR informational SKIP.
9. full spine (Tools-used box, figure, How the code works, Errors-three-ways, Concurrency lens,
   Build-run-observe, cross-check, What you learned, status footer).
10. every chapter cpp/console/cmake block = verbatim substring of source or a real transcript.
11. validate.py OK; two Figure 49.x includes; both diagrams catalogued.
12. banned-words clean; Tools-used box == tools exercised; `test-all-examples --only
    49-concurrency-vs-parallelism` PASS; footer `status--verified` reflects A–F, explicit
    `status--unverified` for P2300/P2996 and anything not run.
13. C++26 deltas called out per [[cpp26-compendium-coverage]], each marked with its **measured**
    support status — contracts and hardening as live, senders and reflection as forward-looking.

## Risks
`distinct_cpus>1` scheduler-dependent (bounded retry; affinity-fact fallback per D3); contract
diagnostic wording could drift across GCC releases (assert the stable bracketed fields
`semantic:`/`terminating:`, not the whole line); Fedora's default libstdc++ assertions are a *distro*
choice, not upstream (chapter must say so — a reader on another distro sees UB instead, which is
itself the lesson); new-part front-matter mismatch breaks the site (S1 is blocking);
`-fcontracts` is GCC-only, so gate E must not run under clang (guard on compiler id);
`tbb-devel` absent → do not reach for `std::execution::par` anywhere.

## Verification outlook (depth gate — 2 scenarios)
Baseline is **strong**: contracts and hardening both demo live on GCC 16, which is more C++26 than
ch46 could show, and the four-model core needs nothing but the stdlib and Linux scheduler syscalls.
- **Scenario 1 STDLIB-ONLY (recommended).** No Conan, no Boost, no network. Gates A–F hard, G
  skip-if-present. P2300/P2996 as clearly-labeled forward-looking prose. Ships the framing chapter
  with the strongest offline core and leaves sender/receiver to ch56's capstone comparison, where the
  memory note says it belongs as "a distinct concurrency model … mark support status accurately".
- **Scenario 2 STDEXEC-VIA-CONAN (opt-in, network).** Add NVIDIA `stdexec` through Conan 2 + a
  lockfile (ch46 already established that pattern) so P2300 gets a *live* `sync_wait(just(42))`-class
  demo in the framing chapter. Cost: a network fetch, a third-party dep in a framing chapter, and
  duplication of what ch56 is scheduled to do properly.

## DECISIONS NEEDED (user gate)
- **D1 — scope of the framing demo.** Four models (sequential/concurrent/parallel/both) as above, or
  trim to three (drop `both` as redundant with `parallel`)?
- **D2 — P2300 senders.** Scenario 1 (forward-looking prose, defer live demo to ch56) or Scenario 2
  (pull stdexec via Conan now)?
- **D3 — `distinct_cpus>1` gate.** Hard-gate it with bounded retry, or gate the deterministic
  affinity facts (`cpus_allowed=1` vs `16`) and report `distinct_cpus` without gating?
- **D4 — merge mode.** Autonomous (as r17) or stop at PR for review?

## Status
- [ ] S1 - [ ] S2 - [ ] S3 - [ ] S4 - [ ] S5 - [ ] S6 - [ ] S7 - [ ] S8 - [ ] gate/PR
