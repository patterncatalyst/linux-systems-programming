---
title: "r21 / ch52 — Boost.Thread — plan (internal)"
published: false
---

# r21 ch52 — example `52-boost-thread` + chapter + diagrams

Fourth chapter of Part 14, and the first to leave the standard library.

**The framing: Boost.Thread is where `std::thread` came from, so what is left in it is
exactly what the standard did not take.** That turns a potential "here is another
threading library" tour into a specific, measurable question — and every answer below was
verified on this host, not read from documentation.

## Decisions

Spine chosen from the audit rather than asked, since the audit answered it: four pillars,
each a thing `std::` still cannot do in C++23 on this toolchain. Autonomous through PR;
the user merges (consistent with r19 and r20).

## Host audit (run 2026-08-18, Fedora 44, glibc 2.43, GCC 16.1.1)

`boost-devel-1.90.0-7.fc44`, `libboost_thread.so.1.90.0` present. **No Conan, no network** —
system Boost only. Compiled with `-lboost_thread -lboost_system -lboost_chrono -pthread`.

| feature | measured |
| --- | --- |
| `BOOST_THREAD_VERSION` | **5** (with `#define BOOST_THREAD_VERSION 5`) |
| `boost::future::then()` continuation | **works** — `async(->21).then(*2)` → 42 |
| `boost::when_all` | **works** — two futures joined, 1 + 2 |
| `std::future::then()` | **does not exist** — provable at compile time with a concept |
| `boost::upgrade_lock` → `upgrade_to_unique_lock` | **works** — shared→unique without releasing |
| `std::upgrade_lock` | **does not exist at all** — the name fails to compile, so it cannot even be SFINAE-tested |
| `boost::thread::interrupt()` | **works** — worker catches `boost::thread_interrupted` |
| `boost::thread::attributes::set_stack_size(256 KiB)` | **works** — worker observes **262144**, identical to ch50's `pthread_attr_setstacksize` |

### The gotcha worth a section

**`boost::thread::attributes` must be passed as a `const` lvalue.** A non-const
`attributes` object is hijacked by the variadic `thread(F&& f, Args&&... args)`
constructor — it deduces `F = thread_attributes` and tries to *call* the attributes
object, producing a wall of template errors from `boost/thread/detail/invoke.hpp` that
never mentions the real problem. Both a lambda and a function pointer fail this way; adding
a dummy argument does not help.

```cpp
boost::thread::attributes attrs;
attrs.set_stack_size(256 * 1024);
const boost::thread::attributes& cattrs = attrs;   // <-- required
boost::thread t(cattrs, &worker);
```

`BOOST_THREAD_PROVIDES_VARIADIC_THREAD` is defined by default at
`BOOST_THREAD_VERSION 5`, which is what makes the variadic overload a candidate.

### THE PAYOFF — cancellation, three ways, all measured

This completes an arc ch50 opened and ch51 continued:

| chapter | model | mechanism | `catch` it and do not rethrow |
| --- | --- | --- | --- |
| ch50 | `pthread_cancel` | glibc **forced unwind** | **process aborts** — `FATAL: exception not rethrown`, exit 134 |
| **ch52** | `boost::thread::interrupt()` | an **ordinary C++ exception**, `boost::thread_interrupted`, thrown at defined interruption points | **survives**, exit 0 |
| ch51 | `std::stop_token` | a **flag**; nothing is thrown | nothing to swallow |

Measured directly: a worker that catches `boost::thread_interrupted` and does not rethrow
prints and the process exits 0. The identical mistake against `pthread_cancel` killed the
process in ch50. Boost sits exactly between the other two, and the reason is that it uses
a normal exception rather than glibc's forced unwind.

## The core demo — `boostthread` (C++-only, `langs: [cpp]`)

One subcommand per pillar. Digest continuity: `digest=0x481984990deee5ff`.

Subcommands: `continuations`, `when-all`, `upgrade`, `interrupt`, `interrupt-swallow`,
`attributes`, `no-std-then` (the compile-time contrast), `versions`.

## Gate tiers

**Hard (A–F), local, unprivileged, offline:**
- A. digest identical across every subcommand.
- B. continuations: `.then()` chains and produces 42; `when_all` joins two futures.
- C. `std::future` has no `.then()` and `boost::future` does — a **compile-time**
  concept check reported at run time, so the contrast is asserted rather than claimed.
- D. upgrade lock: a thread holding `upgrade_lock` promotes to unique without releasing,
  and a concurrent reader is excluded only after the promotion.
- E. interruption: `boost::thread_interrupted` is caught; and the swallow case **exits 0**
  where ch50's equivalent aborted. Assert both.
- F. attributes: a Boost thread created with a 256 KiB attr observes >= 262144 via
  `pthread_getattr_np` — the same number ch50 measured through the POSIX API.

**Gated-if-present (G):** clang parity.

**Not gated:** anything needing network or Conan; Boost's experimental executors.

## Risks

- **`BOOST_THREAD_VERSION` and the continuation macros must be defined before any Boost
  header.** Put them in `CMakeLists.txt` as `target_compile_definitions`, not in the
  `.cpp`, so a stray include order cannot silently disable `.then()` and turn gate B into
  a compile error the reader cannot diagnose.
- **The attributes gotcha will bite the reader too.** It is a section, not a footnote.
- **Boost version drift**: gate on behavior, and print `BOOST_VERSION` in the output so
  the footer can state exactly what was measured (1.90.0 here).
- **Interruption points are a defined list.** `sleep_for` is one; a tight compute loop is
  not. The demo must park on a real interruption point, and the chapter must say so —
  otherwise a reader's variant will hang and look like a Boost bug.

## Steps

S1 mint + strip · S2 `cpp/src/boostthread.cpp` + CMakeLists (find_package Boost) + demo.sh ·
S3 `verify.lua` · S4 manifest · S5 capture transcripts · S6 `_docs/52-boost-thread.md` ·
S7 2 diagrams + catalogue · S8 gate matrix · S9 PR, stop there.

## Acceptance

`verify.lua` PASS/FAIL 0 with E and F running for real; runner 1 passed; `validate.py` OK;
every chapter code block a verbatim substring of a real source or transcript; footer states
Boost 1.90.0, glibc 2.43, and the three-way cancellation table as measured.

## Outcome (2026-08-19)

All steps complete. `verify.lua` → **PASS 37 / FAIL 0** (clang parity ran for real), runner
→ 1 passed, `validate.py` → OK, banned-words clean, **24/24 chapter code blocks verbatim**.

### Delta: a fifth pillar was added after an empirical check

The plan described interruption points as "a defined list" and said a tight compute loop
"will not stop", flagged as prose. Tested it instead — and it is true, so it became a
gated subcommand (`interrupt-busy`) rather than a claim: `interrupt()` returns
successfully, `worker_stopped=no`, the thread spins on. That turns the three-way
cancellation comparison from power-ranked into a genuine **trade**:

| model | reach | mishandled |
| --- | --- | --- |
| `pthread_cancel` | any cancellation point, asynchronous | kills the process (134) |
| `thread::interrupt()` | defined interruption points only | thread keeps running |
| `stop_token` | wherever the target polls | thread keeps running |

Building it surfaced a real bug worth keeping: the first version dumped core (exit 139)
because the detached busy thread outlived `main()` and kept touching its stack locals.
Fixed by making the shared state `static` and leaving via `_exit(0)`. That is now stated
in the chapter as a lesson rather than hidden.

### Other deltas

- **`CMP0167` needed setting.** CMake 4.3 removed its own `FindBoost`; without the policy
  the build works but warns it found Boost by a path CMake has announced it is deleting.
  Guarded with `if(POLICY CMP0167)` so older CMake still configures.
- **The `attributes` const-lvalue gotcha was worse than expected** and is now a chapter
  section with the real error text. A function pointer fails identically to a lambda, and
  a dummy argument does not disambiguate — the variadic overload swallows that too.
- **`std::upgrade_lock` is undeclared, not absent.** Tried to write the same
  concept-based contrast used for `.then()` and it does not compile at all:
  `'upgrade_lock' in namespace 'std' does not name a template type`. A concept can ask
  whether a type has a member; it cannot ask whether a name exists. Gate D therefore has
  no std-side half, and the chapter says why.
- **Dropped an unverifiable paper number.** An early draft cited the Concurrency TS as
  "N4107 and successors"; that could not be checked offline, so the sentence now describes
  the TS without a paper number ([[no-third-party-book-sourcing]] discipline — primary
  sources or nothing).
