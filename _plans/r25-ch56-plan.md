---
title: "r25 / ch56 — the capstone comparison — plan (internal)"
published: false
---

# r25 ch56 — example `56-capstone-comparison` + chapter + diagrams

**Closes Part 14** (ch49–56) and closes the book's C++ compendium. The part index
promises "a measured comparison of all six", and Chapter 49 left a named debt:

> It is scheduled for Chapter 56's comparison, and it goes in there **if and only if
> the toolchain has caught up** by then. No committed dead code in the meantime.
> — `_docs/49-concurrency-vs-parallelism.md`

## Overlap check (done FIRST, as in r22/r23/r24)

Each model already has a chapter that owns it, and the capstone must not re-derive any:

| model | owned by | what that chapter measured with |
| --- | --- | --- |
| pthreads | ch50 | `gettid`, `pthread_getattr_np`, `pthread_cancel` |
| `std::thread` | ch51 | `strace -f -c -e trace=futex` |
| Boost.Thread | ch52 | `future::then`/`when_all`, `thread::interrupt()` |
| C++20 coroutines | ch53 | `operator new` inside `promise_type` |
| Boost.Fiber | ch54 | `stack_traits::default_size()` |
| Boost.Asio | ch55 | strands, futex counts, `gettid` |

**So what is genuinely new here?** Not any model — the **uniform instrument**. Every
prior chapter measured its own model with its own instrument on its own workload, so no
two numbers in the compendium are strictly comparable. ch56 runs **one workload** through
**all six** and measures them with **the same two instruments**, which is the only way the
comparison is a comparison rather than a table of quotations.

## Host audit (2026-08-22, Fedora 44, kernel 7.1.8-200.fc44)

GCC 16.1.1, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, Conan 2.30.0, Lua 5.4.8,
Boost 1.90.0 (`boost-devel-1.90.0-7.fc44`), 16 logical / 8 physical CPUs.

### The ch49 debt, re-probed — the standard library has NOT caught up
```
senders=undefined        # __cpp_lib_senders
execution=201902         # <execution> is still the C++17 parallel-algorithms header
```
`tbb-devel` still absent. So P2300 **in the standard library** remains unavailable, exactly
as at r18.

### D1 (user, 2026-08-22): LIVE senders via NVIDIA stdexec + Conan
Chosen over forward-looking prose. The audit turned up one obstacle and cleared it:

- **stdexec is NOT in ConanCenter.** `conan search stdexec -r conancenter` →
  `ERROR: Recipe 'stdexec' not found`; `*exec*` → 0 recipes. Not in Fedora either
  (`dnf search stdexec` → no matches).
- Upstream ships its own `conanfile.py`, but it is named **`p2300`** and takes its version
  from `git.get_commit()` — it expects a git checkout and has no stable version.
- There are **no GitHub releases**, only `nvhpc-*` snapshot tags. Latest: **`nvhpc-26.05`**.
- **Therefore: an in-repo Conan recipe pinning the tag tarball + sha256.** Verified:
  `https://github.com/NVIDIA/stdexec/archive/refs/tags/nvhpc-26.05.tar.gz`
  `sha256 9d2396fecd604698c1eae58f0cb6e4517aa727013846240d1a7b2f35e49884dc` (640361 bytes)
- **It compiles and runs on both compilers at the pinned tag** (probe:
  `sync_wait(just(40) | then(+2))` and `on(static_thread_pool(4).get_scheduler(), …)`):

| compiler | result |
| --- | --- |
| g++ 16.1.1 `-std=c++23` | `sync_wait=42` `on_pool=7` |
| clang++ 22.1.8 `-std=c++23` | `sync_wait=42` `on_pool=7` |

  License Apache 2.0, header-only, `-pthread`.

**The chapter must state precisely what this is**: a live demo of the *reference
implementation* of P2300, not of the standard library. Chapter 49's footer claim
(`__cpp_lib_senders` undefined) stays true and must be repeated here, not quietly dropped.

## D2 (user, 2026-08-22): one workload, six models, one instrument set

**The workload.** N tasks, each computing the compendium's FNV-1a digest over the 16-byte
payload K times and folding the result into a shared accumulator. No I/O, no ports, no
timing (ch39). The accumulator is the correctness observable and it must come out
identical in every model — which is the digest through-line ch46–ch55 already carries.

**The two uniform instruments** (the actual novelty):
1. `distinct_tids` — `gettid()` unioned into a set. ch50's instrument, ch49's vocabulary.
2. **futex calls** — `strace -f -c -e trace=futex`. ch51's instrument, reused by ch55.

**Two further axes are tables, not new measurements**, assembled from named constants
carried forward with their provenance — the pattern ch54 established with
`kCh50ThreadStackBytes` / `kCh53CoroutineFrameBytes`:
3. bytes per paused computation — 8388608 (ch50) / 131072 (ch54) / 32 (ch53).
4. cancellation shape — forced unwind (ch50) / flag (ch51) / exception (ch52) /
   completion (ch55). Four shapes, already measured, now in one table.

The chapter must be explicit that axes 3 and 4 are **assembled**, not re-measured here.

## Arms

`pthreads` · `std-thread` · `boost-thread` · `coroutine` · `fiber` · `asio` ·
`senders` (opt-in) · `sequential` (the control) · `versions`.

Digest continuity: `digest=0x481984990deee5ff` (unchanged since ch46).

## CI and offline discipline — the binding constraint

`validate.yml` builds on fedora:44 **without Conan**, and the runner must stay green
offline. So:

- **The six-model core takes zero third-party fetch.** System Boost only, exactly as
  ch52/ch54/ch55.
- **The senders arm is an isolated Conan sub-target**, on ch46's pattern: its own
  `conanfile.py` + committed `conan.lock`, its own `conan` CMake preset, and
  `./demo.sh cpp build` never evaluates it.
- **Gate E is skip-if-absent** (`print("SKIP: …")`, never `checks.skip()`, which would
  abort the script) so a machine with no Conan and no network still passes A–D, F, G.

## Gate tiers

**Hard (A–D, F):**
- A. digest identical across every arm, and equal to ch46–ch55's.
- B. **one workload, one answer**: every model folds to the *same* accumulator value. If
  two models disagree, the comparison is meaningless, so this gates first.
- C. tids: `sequential` reports exactly 1; the multi-threaded arms report > 1. Sign gated,
  bounded retry (ch49/ch54's rule) — never `== 8`.
- D. futex, the uniform axis: the blocking arms reach the kernel materially more often than
  the deferring ones. **Ratio only, never magnitudes** — ch55 measured 52 vs 1022 on three
  runs that never repeated a number.
- F. clang parity on the digest.

**Gated-if-present (E):** the senders arm, when Conan + network produced it. Same
accumulator value, same digest.

**Not gated:** timings, magnitudes, anything needing a port, and the *ranking* of models by
futex count beyond the blocking/deferring split.

## Risks

- **Seven arms in one program is the real scope risk.** Mitigate: one subcommand per arm,
  each minimal, each deliberately NOT re-deriving its chapter. The novelty is the uniform
  instrument, so an arm that grows a second idea is out of scope.
- **`senders` is not the standard library.** Must be labelled as the reference
  implementation everywhere it appears, including the footer. Getting this wrong would make
  ch49's measured claim look false.
- **The pinned tag is a snapshot, not a release.** Upstream has no releases; record the tag
  AND the sha256, and state that the lockfile is what makes it reproducible.
- **Boost.Thread's continuations need defines** — `BOOST_THREAD_VERSION=5`,
  `BOOST_THREAD_PROVIDES_FUTURE`, `…_FUTURE_CONTINUATION`, `…_FUTURE_WHEN_ALL_WHEN_ANY`
  (copy ch52's `CMakeLists.txt` block verbatim).
- **strace may be ptrace-restricted** → gate D degrades to an informational SKIP, as ch51
  and ch55 do.
- **Do not re-derive ch50–ch55.** No stack-size walkthrough, no HALO, no guard page, no
  epoll, no strand tutorial. Cite them.

## Steps

S1 mint + strip · S2 `cpp/src/capstone.cpp` (six arms + control) + CMakeLists ·
S3 senders sub-target (`cpp/conan/`, recipe pinning `nvhpc-26.05` + sha256, lockfile,
`conan` preset) · S4 `verify.lua` · S5 manifest · S6 transcripts · S7
`_docs/56-capstone-comparison.md` · S8 2 diagrams + catalogue · S9 gate matrix ·
S10 PR, stop there.

## Acceptance

`verify.lua` PASS / FAIL 0 with B, C, D and F running for real; runner 1 passed;
`validate.py` OK; **the core verified green with Conan unavailable** (prove the SKIP path,
not just the happy path); every chapter code block verbatim; the footer states the six-way
table as measured, the two assembled tables with their provenance, and the exact status of
P2300 — reference implementation live at `nvhpc-26.05`, standard library still
`__cpp_lib_senders` undefined.

## Status

- [x] Overlap check - [x] Host audit - [x] D1 feasibility proven (both compilers, pinned tag)
- [x] S1 - [x] S2 - [x] S3 - [x] S4 - [x] S5 - [x] S6 - [x] S7 - [x] S8 - [x] S9 - [ ] PR

### COMPLETE (2026-08-26)

Gate matrix, all run on the host:
- `verify.lua` → **PASS 57 / FAIL 0** (B, C, D, E, F all real)
- **SKIP path proven** → **PASS 52 / FAIL 0** with `conan` hidden from PATH and
  `cpp/build/conan` deleted; gate E printed its informational SKIP. This is the
  acceptance criterion that mattered and it is what fedora:44 CI does.
- runner → 1 passed · `validate.py` → OK · 9/9 chapter code blocks verbatim ·
  banned words none · ch39/46/49-55 cross-refs resolve

Measured, and what the chapter quotes:
- all seven models → `total=0xb75768f6610642a0`; digest `0x481984990deee5ff`
- tids: 1 / 1 / 1 (sequential, coroutine, fiber) · 8 (pthreads, std-thread,
  boost-thread, asio, senders)
- futex: 1 / 1 / 1 · 230 / 427 / 451 · **51** (strand) · 562 (senders).
  Spans over further runs: locks 230-549, strand 51-65, senders 562-582.

Deviations from the plan, both deliberate:
1. **Diagram spec IS committed** at `assets/diagrams/specs/56.py`, against the
   established convention of throwing specs away. Regenerating Figure 55.1 by
   hand in r24 cost real effort; this makes the next number correction a re-run.
   Flagged to the user rather than done silently.
2. The `conan` preset builds **Release**, not RelWithDebInfo. CMakeDeps gates
   include dirs behind `$<$<CONFIG:Release>:...>`, so a mismatch makes
   `find_package` succeed while the headers stay invisible. Became the
   chapter's "an error the build catches".
