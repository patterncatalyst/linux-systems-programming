---
title: "r23 / ch54 — Boost.Fiber — plan (internal)"
published: false
---

# r23 ch54 — example `54-boost-fiber` + chapter + diagrams

Sixth chapter of Part 14.

## Overlap check (done FIRST, as in r22)

- **ch44** covers Go's GMP: M:N scheduling, work-stealing, netpoller, 2000 goroutines on
  ~12 threads. So ch54 must not re-derive M:N *generically* — it must show C++'s version
  and what is different about choosing it explicitly.
- **ch53** measured a stackless coroutine frame (32 B) and the HALO split.
- **ch27** owns the coroutine/epoll mechanism.
- `grep -il fiber _docs/*.md` → only outline/ch49/ch50 forward-references, plus ch35
  (unrelated, KVM). **No prior chapter covers stackful context switching.**

## The spine: stackful is the whole difference

ch53 established that a coroutine frame is 32 bytes and holds only what crosses a
suspension. A fiber has a **real stack**. That single fact produces both the cost and
the capability, and it completes Part 14's memory story:

| paused computation | per-unit cost | measured in |
| --- | --- | --- |
| thread | **8388608 B** (8 MiB) | ch50 |
| **fiber** | **131072 B** (128 KiB) | ch54 |
| coroutine frame | **32 B** | ch53 |

8 MiB → 128 KiB → 32 B. Roughly two orders of magnitude at each step, every number
measured on this host rather than quoted.

## Host audit (2026-08-20, Fedora 44, Boost 1.90.0, GCC 16.1.1)

`boost-devel-1.90.0-7.fc44`; `libboost_fiber.so.1.90.0` and `libboost_context.so.1.90.0`
present. **No Conan, no network.** Link: `-lboost_fiber -lboost_context -pthread`.

| fact | measured |
| --- | --- |
| `ctx::stack_traits::default_size()` | **131072** (128 KiB) |
| `ctx::stack_traits::minimum_size()` | **14528** |
| `ctx::stack_traits::page_size()` | 4096 |
| `is_unbounded()` | yes |
| `fixedsize_stack(64 KiB).allocate().size` | **65536** — exactly what was asked |
| `protected_fixedsize_stack(64 KiB)` | **69632** = 65536 + one 4096 guard page |
| fiber runs on | the **calling thread's tid** (same as main) |

### Guard page, both directions (measured)

| case | result |
| --- | --- |
| recurse 5 frames on a 64 KiB protected stack | completes, exit 0 |
| recurse 1000 frames (~4 MiB) on the same stack | **SIGSEGV, exit 139** |

The guard page turns a stack overflow into an immediate fault instead of silent
corruption of whatever is mapped next. This is the ch49 `oob` pattern one layer up.

### M:N, and the scheduler is a choice (measured)

Same 16 fibers, same work, two schedulers, tids collected with `gettid()`:

| scheduler | distinct tids |
| --- | --- |
| default `round_robin` | **1** |
| `algo::shared_work` (4 threads) | **4** |

This is **ch49's grid applied to fibers**: identical concurrency structure, and whether
any parallelism happens is one `use_scheduling_algorithm` call. Unlike Go, where the
runtime decides, C++ makes you choose — and the choice is observable.

### The capability the stack buys (measured)

A fiber can suspend from **deep inside an ordinary call tree**:

```
deep: yielding from depth=6 inside a plain function
deep: resumed at depth=6
```

`leaf()` is not a coroutine and neither is anything above it. `co_await` is only valid
in the body of a coroutine, so a stackless coroutine fundamentally cannot do this — every
frame in the suspend path must itself be a coroutine ("function colouring"). The fiber's
128 KiB is what buys the ability to suspend anywhere.

## The core demo — `fiberdemo` (C++-only, `langs: [cpp]`)

Subcommands: `stacks` (traits + three allocators), `versus` (the 3-way memory table),
`deep` (suspend from a plain function 6 frames down), `roundrobin` / `sharedwork` (tid
counts), `guard-ok` / `guard-overflow` (the fault), `versions`.

`guard-overflow` dies of SIGSEGV **by design** — its own subcommand, invoked only under
the harness's expectation of failure, exactly as ch50's `cancel-swallow` and ch51's
`deadlock-naive` were.

Digest continuity: `digest=0x481984990deee5ff`.

## Gate tiers

**Hard (A–F):**
- A. digest identical across every non-faulting subcommand.
- B. stacks: default is 131072; a 64 KiB request yields exactly 65536; the protected
  allocator adds exactly one page (69632 - 65536 == 4096 == `page_size()`). Gate the
  page-delta relationship, not the absolute default.
- C. versus: fiber stack sits strictly between ch53's 32 B frame and ch50's 8388608 B
  thread stack, and both ratios are computed in the program.
- D. deep: a plain non-coroutine function suspends and resumes at depth > 1.
- E. M:N: `roundrobin` reports exactly 1 distinct tid; `sharedwork` reports > 1. Same
  fibers, so the scheduler is the only variable.
- F. guard: in-budget completes (exit 0); overflow dies of a signal (exit >= 128), and
  the gate asserts the *signal*, never any output the faulting run produced.

**Gated-if-present (G):** clang parity.

**Not gated:** context-switch timings (ch39); the exact `minimum_size()`; anything
requiring more than one core for correctness.

## Risks

- **`sharedwork` tid count is scheduler-dependent.** Measured 4/4 here, but gate `> 1`
  and never `== 4`; a loaded host could legitimately keep the work on fewer threads.
  Bounded retry if it comes back 1, as ch49's gate B did.
- **`guard-overflow` must fault, not hang.** Recursion depth 1000 × ~4 KiB is ~4 MiB
  against a 64 KiB stack — 60x past it, so it cannot accidentally fit. Run under
  `timeout` anyway.
- **Never assert on what the faulting run printed.** Same rule as ch49's `oob_unchecked`:
  the output after a stack overflow is undefined. Assert the exit signal only.
- **`-O0` for the guard case**: an optimizing compiler can turn the recursion into a
  loop and never touch the guard page. Force `-O0` on that translation unit and say why.
- **Do not re-derive ch44.** ch54 shows C++'s explicit scheduler choice; ch44 owns Go's
  automatic one. Cite it, contrast it, do not re-explain M:N from scratch.

## Steps

S1 mint + strip · S2 `cpp/src/fiberdemo.cpp` + CMakeLists · S3 `verify.lua` · S4 manifest ·
S5 transcripts · S6 `_docs/54-boost-fiber.md` · S7 2 diagrams + catalogue · S8 gate matrix ·
S9 PR, stop there.

## Acceptance

`verify.lua` PASS/FAIL 0 with E and F running for real; runner 1 passed; `validate.py` OK;
every chapter code block verbatim; footer states Boost 1.90.0, the three stack numbers,
the guard-page delta, and the two tid counts as measured.

## Outcome (2026-08-20)

All steps complete. `verify.lua` → **PASS 32 / FAIL 0** with gates E (migration), F (the
guard-page fault) and G (clang parity) all running for real. Runner → 1 passed.
`validate.py` → OK. Banned-words clean. **19/19 chapter code blocks verbatim**; all 7
`Chapter N` cross-references resolve.

### The overlap check (second time doing this before planning, and it paid again)

`grep -il fiber _docs/*.md` found only forward-references. ch44 owns Go's *automatic*
M:N; ch54 shows C++'s *explicit* one and contrasts rather than re-explains. ch53 owns
stackless; ch54 is the stackful counterpart and the two compose into the trilogy.

### Measured, and worth keeping

| paused computation | bytes | instrument | chapter |
| --- | --- | --- | --- |
| thread | 8388608 | `pthread_getattr_np` | ch50 |
| fiber | 131072 | `stack_traits::default_size()` | ch54 |
| coroutine frame | 32 | `operator new` in `promise_type` | ch53 |

Three different instruments, three chapters, one ordering — and the ordering falls out of
structure rather than benchmark noise. Both ratios (64, 4096) are computed in the program.

Guard page: `fixedsize_stack(64 KiB)` → exactly 65536; `protected_fixedsize_stack(64 KiB)`
→ 69632, delta **4096 == `page_size()`**. Overflow 60x past it → **exit 139 (SIGSEGV)**.

Scheduler: same 16 fibers → `round_robin` **1 tid**, `shared_work` **4 tids**, stable at
4/4 across five runs.

Deep suspend: yielded and resumed at **depth 6** inside ordinary functions.

### Deltas

- **`-O0` forced on the translation unit**, and documented in `CMakeLists.txt` rather
  than left implicit. At `-O2` the recursion can become a loop and never reach the guard
  page, which would make `guard-overflow` exit 0 and silently stop demonstrating
  anything — ch49's optimizer lesson, one layer down. The `volatile` array is the same
  defence.
- **Gate F rejects exit 124 explicitly.** `timeout(1)` returns 124 when it kills
  something, and a *hung* run must not pass a gate that means "faulted promptly".
- **Guard delta gated against `page_size()`**, not a hardcoded 4096, so it holds on a
  host with different page sizes.
- **`fiber_specific_ptr` header path corrected** before shipping: it is in
  `<boost/fiber/fss.hpp>`, not `fiber_specific_ptr.hpp`. Verified by grep rather than
  assumed. All three named-but-unexercised APIs were confirmed present on this host and
  the footer says they are named, not gated.
