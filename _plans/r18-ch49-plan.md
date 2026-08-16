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
Boost.Asio → capstone comparison) all lean on it. See "ch55 scope" at the end of this file.

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

## DECISIONS (user, gate — 2026-08-07)
- **D1 = FOUR MODELS.** `sequential` / `concurrent` (pinned) / `parallel` (unpinned) / `both`. The
  2×2 grid is the chapter's argument; `both` is what real systems are, so no corner is left to prose.
- **D2 = SCENARIO 1, STDLIB-ONLY.** P2300 is forward-looking prose, clearly labeled with its measured
  status (`__cpp_lib_senders` undefined, `__cpp_lib_execution = 201902` is C++17 par), cross-referenced
  to ch56. **No Conan, no stdexec, no network.** ch49 is stdlib + Linux scheduler syscalls only.
- **D3 = HARD-GATE `distinct_cpus>1` WITH BOUNDED RETRY.** Assert "at least one of K runs observed
  >1 CPU" — a real effect, not a timing. Affinity facts (`cpus_allowed`) asserted too, as the
  deterministic companion. Fallback in Risks stands if it proves flaky under load.
- **D4 = AUTONOMOUS merge.** Build → verify → PR → merge on green CI, pausing only for decisions the
  plan does not cover.

## Status
- [x] S1 - [x] S2 - [x] S3 - [x] S4 - [x] S5 - [x] S6 - [x] S7 - [x] S8 - [ ] PR

### COMPLETE (2026-08-16) — supersedes the checkpoint block below
README rewritten, `_docs/49-concurrency-vs-parallelism.md` written, gates re-run green:
`LSP_LANG=cpp lua verify.lua` → **PASS 43 / FAIL 0**, runner → 1 passed, `validate.py` → OK,
banned-words clean, 15/16 chapter code blocks verified as verbatim substrings of real sources (the
16th is the deleted pre-fix burn-loop line, quoted deliberately as history).

Two corrections made while writing, both from measurement:
- **The dead-code claim in `conc.cpp` was overstated and is now a measured 2x2 table.** Rebuilt all
  four variants (serial-chain vs loop-invariant body) x (sink vs no sink) under g++ 16.1.1 and
  clang++ 22.1.8 at -O2, three `--model concurrent` runs each. Result: only chain+sink survives
  **both** compilers. chain+nosink works under GCC but collapses to `max_inflight=1` under clang;
  loop-invariant collapses under both regardless of the sink. The old comment said "without a sink
  both compilers delete the whole thing" — not reproducible. Comment and README now carry the table.
- **`nm` was used as a gate but not gated by `check-host.sh`.** Added a `nm (binutils)` check, since
  the Tools-used-box rule requires every named tool to appear there. Verified `[ ok ] nm (binutils)
  GNU nm version 2.46.1-1.fc44`.

### RESUME HERE (checkpoint 2026-08-08, now superseded)
**Done and green.** `LSP_LANG=cpp lua verify.lua` → **PASS 43 / FAIL 0** (gate G included — clang++
is present on this host, so the parity gate ran for real rather than skipping).
`test-all-examples --only 49-concurrency-vs-parallelism` → 1 passed. `validate.py` → OK.

**Remaining work, in order:**
1. **`examples/49-concurrency-vs-parallelism/README.md` is STILL THE `_template` TEXT** — it was
   about to be overwritten when the session was checkpointed. Rewrite it (four-model table, the
   observables, the C++26 support table, run/verify sections). This is the only known-stale file.
2. **S6 — write `_docs/49-concurrency-vs-parallelism.md`.** Front matter `part:` must be exactly
   `"Compendium: C++ Concurrency"`. Full spine + the two Figure 49.x includes
   (`49-concurrency-vs-parallelism` = Fig 49.1, `49-execution-model-lanes` = Fig 49.2).
3. Re-run validate.py + the runner, verify every chapter code block is a verbatim substring of a
   source file or a real transcript, banned-words check, then PR and (per D4) merge on green CI.

### Captured transcripts to quote in the chapter (all real runs, this host)
```
conc report: model=sequential workers=4  cpus_allowed=16 distinct_cpus=1  max_inflight=1  interleaves=0    consensus=yes digest=0x481984990deee5ff
conc report: model=concurrent workers=16 cpus_allowed=1  distinct_cpus=1  max_inflight=16 interleaves=19   consensus=yes digest=0x481984990deee5ff
conc report: model=parallel   workers=16 cpus_allowed=16 distinct_cpus=16 max_inflight=16 interleaves=3816 consensus=yes digest=0x481984990deee5ff
conc report: model=both       workers=32 cpus_allowed=16 distinct_cpus=16 max_inflight=25 interleaves=7557 consensus=yes digest=0x481984990deee5ff
```
Stability over 8 consecutive runs each: `concurrent` interleaves 16–20 (always > 0), `parallel`
distinct_cpus 14–16 (always > 1), `sequential` interleaves 0 / max_inflight 1 on all 5 runs. The
bounded retry in gate B never fired.

### Execution deltas from the plan (already applied)
- **The burn loop was dead code in the first build.** `local` was never consumed, so both compilers
  deleted the whole nested loop; workers finished in microseconds, never overlapped, and every model
  reported `max_inflight=1 interleaves=0` — a *sequential* run no matter which model was asked for.
  Fixed with an atomic `sink` the workers xor into, plus a real serial dependency chain in the loop
  body. **This is a section of the chapter, not just a bug fix**: the first measurement measured the
  optimizer, not the scheduler.
- **`-fcontracts` is needed at LINK time, not just compile time.** Without it on the link line the
  `observe` and `enforce` targets fail with `undefined reference to
  handle_contract_violation(std::contracts::contract_violation const&)`. `target_link_options` added.
- **Gate F redesigned after measurement.** The plan assumed Fedora has `_GLIBCXX_ASSERTIONS` on by
  default; measured, it is on at `-O0` and **off at `-O2`**, so the CMake `release` preset did *not*
  trap. Now built twice (`oob_unchecked` / `oob_hardened`). The unchecked side is gated
  **statically** (`nm` shows no `__glibcxx_assert_fail`) because asserting on what a UB read prints
  would be unsound. Better story than planned: the safety net is on where you debug, off where you ship.
- **`parallel` is one thread per CPU, `both` is 2x oversubscribed** — that is what separates the two
  bottom cells of the grid, since the ticket-gap metric registers interleaving under true
  parallelism as well. Grid semantics documented in verify.lua's header.

### Toolchain facts measured this session (do not re-derive)
GCC 16.1.1, clang 22.1.8, CMake 4.3.0, Conan 2.30.0, Boost 1.90 system-installed (`libboost_context/
fiber/thread` all present → ch52/ch54 need no Conan fetch). 16 logical / 8 physical CPUs, 1 NUMA node.
`tbb-devel` ABSENT → `std::execution::par` will not link; do not reach for it.
C++26: `__cpp_contracts = 202502` (works, four semantics), `__cpp_lib_senders` undefined,
`__cpp_lib_execution = 201902` (C++17 par), `__cpp_impl_reflection` undefined, clang 22 has no
`-freflection`.

### ch55 scope, settled 2026-08-16 (forward note, not this iteration)
The outline row and this plan's ch50–56 list disagreed about ch55: the row named six topics for
eight chapters, and line 13 called ch55 "stack-based threading for I/O" — which is what ch54
(Boost.Fiber) already is. **ch55 is now Boost.Asio: concurrency for I/O.** Topics to cover:

- `io_context` threading topologies — one thread; N threads on one context; one context per thread;
  `thread_pool` as an alternative executor. Each has a different observable CPU/thread footprint,
  which is the ch49 vocabulary applied to an I/O runtime.
- **strands** — serialized handler execution without a mutex; implicit vs explicit
  (`boost/asio/strand.hpp`, `io_context_strand.hpp`).
- **`executor_work_guard`** — why `io_context::run()` returns early without one.
- **cancellation signals/slots** (`cancellation_signal.hpp`) — cooperative cancellation, and how it
  differs from ch51's `std::stop_token`.
- **scatter-gather buffers** — `const_buffer`/`mutable_buffer` sequences over `readv`/`writev`,
  which ties straight back to Part 5's I/O chapters.

All present on this host, verified: `boost-devel-1.90.0-7.fc44`, `/usr/include/boost/asio/`
(`strand.hpp`, `io_context_strand.hpp`, `cancellation_signal.hpp`, `executor_work_guard.hpp`,
`thread_pool.hpp`, `any_io_executor.hpp`), plus `/usr/lib64/libboost_cobalt.so.1.90.0` if a
coroutine-flavored section is wanted. **No Conan, no network.**

Research from primary sources only — Boost's own docs, the installed headers, and man pages for the
syscalls underneath (see [[no-third-party-book-sourcing]]). Every example ours.

### Unrelated cleanup spotted (not this iteration)
ch39 is titled "Benchmarking without lies" and ships a `--lie` flag. CLAUDE.md's banned-words rule
now forbids exactly that ("Name a deliberately-bad variant after its defect (`--naive`,
`--unwarmed`), never `--lie`"). Pre-existing, shipped before the rule; worth its own small PR.
