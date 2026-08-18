---
title: "r20 / ch51 — std threading — plan (internal)"
published: false
---

# r20 ch51 — example `51-std-threading` + chapter + diagrams

Third chapter of Part 14. ch49 set the vocabulary, ch50 went under `std::thread` to the
pthread. ch51 covers the standard library's synchronization primitives — and the risk
here is writing an API tour, which this book must not do.

**The angle that makes it a systems chapter: what does each standard primitive become on
this kernel, and what does it cost?** The answer is a futex when it blocks and *nothing
at all* when it does not, and both halves are measurable with a syscall counter.

## Decisions (answered 2026-08-17)

- **D1 — futex-cost spine.** Organize around what each primitive becomes: the
  uncontended-vs-contended mutex as the headline, then `condition_variable`, `latch`,
  `barrier`, `counting_semaphore`, and `atomic::wait` each measured reaching the same
  syscall. Extends ch25 (futexes) and ch26 (atomics) rather than restating them.
- **D2 — deadlock both ways, with a hard timeout.** Gate that `std::scoped_lock`
  completes with opposing lock orders, AND that the naive version deadlocks under
  `timeout` (exit 124) with every thread showing `wchan=futex_do_wait` in procfs.
- **D3 — Autonomous through PR; the user merges.**

## Host audit (run 2026-08-17, Fedora 44, kernel 7.1.5-201.fc44, glibc 2.43)

Measured with `strace -f -c -e trace=futex`, not inferred:

| run | same total work | futex syscalls |
| --- | --- | --- |
| baseline: no mutex, no threads, 200k increments | 200,000 | **1** |
| `std::mutex` uncontended, 1 thread, 200k lock/unlock | 200,000 | **1** |
| `std::mutex` contended, 8 threads x 25k lock/unlock | 200,000 | **1844–3111** (5 runs) |

**The single futex call in the uncontended run is not the mutex.** The baseline with no
mutex at all reports the same 1, and the uncontended run's *entire* syscall profile is
61 calls — `execve`, `mmap`, `openat`, `read`, `close`, i.e. the dynamic loader. 200,000
lock/unlock pairs contributed nothing to it. Uncontended `std::mutex` is an atomic CAS
and no syscall whatsoever.

Stability: uncontended reported exactly **1 on all 5 runs**. Contended varied 1844–3111 —
magnitude unstable, sign rock solid. Gate on the sign (see risks).

Other primitives, futex calls per run: `condition_variable` 5, `latch` 6,
`counting_semaphore` 4, `jthread`+`stop_token` 2.

### Deadlock, both directions (measured)

| variant | result |
| --- | --- |
| `std::scoped_lock(a,b)` vs `scoped_lock(b,a)`, 50k iters each | **completes**, exit 0 |
| naive `lock_guard` in opposing orders | **deadlocks**, `timeout` exit **124** |

While hung, every task in `/proc/<pid>/task/*/`:

```
tid=2716241 state=S wchan=futex_do_wait
tid=2716242 state=S wchan=futex_do_wait
tid=2716243 state=S wchan=futex_do_wait
```

Three threads — the two workers and main blocked in `join` — all parked in the same
kernel function, readable unprivileged. `wchan` closes the loop: the abstraction's
failure mode, named in kernel terms.

### Feature macros (GCC 16.1.1)

`__cpp_lib_jthread` 201911, `__cpp_lib_latch` 201907, `__cpp_lib_barrier` 201907,
`__cpp_lib_semaphore` 201907, `__cpp_lib_atomic_wait` 201907, `__cpp_lib_shared_mutex`
201505, `__cpp_lib_scoped_lock` 201703, `__cpp_lib_hardware_interference_size` 201703
(both destructive and constructive = **64**).

`strace` is a **hard-gated tool** in `scripts/check-host.sh` (line ~229), so it may carry
a gate and be named in the tools box.

## The core demo — `stdthread` (C++-only, `langs: [cpp]`)

One subcommand per measurement, so the deadlock subcommand's deliberate hang can never
take the others with it. Digest continuity: `digest=0x481984990deee5ff`.

Subcommands: `baseline`, `uncontended`, `contended`, `condvar`, `latch`, `barrier`,
`semaphore`, `atomic-wait`, `stoptoken`, `deadlock-safe`, `deadlock-naive`.

## Gate tiers

**Hard (A–F), all local, unprivileged, offline:**
- A. digest identical across every non-hanging subcommand.
- B. **the headline**: `uncontended` futex count <= 2 AND equal to `baseline`'s, while
  `contended` (same 200,000 lock/unlock pairs) is > 500. Counted with
  `strace -f -c -e trace=futex`.
- C. the blocking primitives each reach futex: `condvar`, `latch`, `barrier`,
  `semaphore`, `atomic-wait` all report >= 1 futex call.
- D. `deadlock-safe` completes under `timeout` with exit 0.
- E. `deadlock-naive` times out (exit 124) AND every task in `/proc/<pid>/task/*/wchan`
  reads `futex_do_wait` while it is hung.
- F. `stoptoken` completes and the stop itself needs no unwinding (contrast ch50).

**Gated-if-present (G):** clang parity on the digest.

**Not gated:** absolute futex counts for the contended case (scheduling-dependent);
timings of any kind.

## Risks

- **Contended futex count is scheduling-dependent** (1844–3111 measured). Gate `> 500`,
  never a magnitude. State the observed range in the chapter, per ch39.
- **`strace` may be denied by ptrace hardening** (`kernel.yama.ptrace_scope`). It is 0 or
  1 on this host and strace on a child we spawn works, but the gate should degrade to an
  informational SKIP if strace cannot attach, never a FAIL.
- **The deadlock subcommand hangs by design.** It must (a) be its own subcommand, (b) be
  invoked only under `timeout`, and (c) have the manifest timeout comfortably above it.
  `verify.lua` must never invoke it bare.
- **`wchan` can read `0` or be denied** on some kernels/hardening. Measured
  `futex_do_wait` here; gate loosely on `futex` appearing, and SKIP if wchan is
  unreadable rather than FAIL.
- **The one futex call in the uncontended run must be attributed correctly.** The gate
  compares it to the baseline rather than asserting zero, because asserting zero would
  be wrong — and the chapter must explain that it is loader startup, not the mutex.

## Steps

- S1 mint `examples/51-std-threading`, strip go/rust, single-lang
- S2 write `cpp/src/stdthread.cpp` (subcommands above) + CMakeLists + demo.sh
- S3 write `verify.lua` (A–F hard, G skip-if-present, strace/wchan degrade to SKIP)
- S4 register in `examples/manifest.yaml` (`langs: [cpp]`, `mode: local`, generous timeout)
- S5 capture all transcripts from real runs
- S6 write `_docs/51-std-threading.md`
- S7 2 diagrams + catalogue rows
- S8 gate matrix: verify.lua, runner, validate.py, verbatim-block check, banned words
- S9 PR (per D3, stop there)

## Acceptance

`LSP_LANG=cpp lua verify.lua` PASS/FAIL 0 with B and E running for real; runner 1 passed;
`validate.py` OK; every chapter code block a verbatim substring of a real source or a real
transcript; footer states glibc 2.43, the strace method, and the measured futex ranges.

## Outcome (2026-08-18)

All steps complete. `LSP_LANG=cpp lua verify.lua` → **PASS 38 / FAIL 0**, with gates B
(futex counts), C (blocking primitives), and E (deadlock + wchan) all running for real.
Runner → 1 passed. `validate.py` → OK. Banned-words clean. **18/18 chapter code blocks
verbatim**; all 12 `Chapter N` cross-references resolve.

### Correction made before shipping: ch51 was re-deriving ch25

Late in drafting, a cross-reference check surfaced that **`_docs/25-shared-state-and-the-futex.md`
already makes the uncontended-vs-contended futex argument** — same tool (`strace`), same
conclusion, with measured counts scaling 5,186 → 488,818 and "~98% of lock ops stay in
userspace" stated in its Figure 25.2. ch51's original opening presented that as its own
central discovery.

Reframed rather than cut, because the measurement is still needed as the chapter's
foundation and the numbers here are its own. ch51 now opens by citing ch25's result and
naming three things ch25 does **not** cover:

1. **The strength of the claim.** ch25's uncontended run still made thousands of futex
   calls (a workqueue with condvars genuinely blocks). Strip the blocking and subtract a
   control and the figure is not 98% but **100%** — 1 futex call, identical to a no-mutex
   baseline, whole profile 61 syscalls of loader startup. The baseline-subtraction
   technique is what makes the stronger claim measurable.
2. **The C++20 family** — `latch`, `barrier`, `counting_semaphore`, `atomic::wait` all
   reaching the same syscall. ch25 used only mutex + condvar.
3. **What the library adds over the futex** — `scoped_lock`'s deadlock avoidance and
   `stop_token`, neither visible at the syscall layer.

The "Cross-check" section now presents the two chapters as independent confirmation
(different examples, different techniques, same structure) rather than a fresh finding.

### Other deltas

- **Futex op breakdown added.** `strace -f -e trace=futex | grep -oE "FUTEX_[A-Z_]+"`
  shows the contended run is 877 `FUTEX_WAKE_PRIVATE` + 554 `FUTEX_WAIT_PRIVATE`, so
  only **554 of 200,000 acquisitions actually slept** (99.7% stayed in user space). This
  replaced a softer "roughly 99%" estimate that had been inferred from the total.
- **strace version corrected** in the footer: 7.1, not the 6.19 first written.
- **Manifest timeout 900**, not the Part 14 default 600 — two deliberate deadlocks under
  `timeout` (5s + 6s) on top of ~15 straced runs.
- **wchan read per-file, not `cat`-ed together.** `/proc/<tid>/wchan` has no trailing
  newline, so the first version produced `futex_do_waitfutex_do_waitfutex_do_wait`.
