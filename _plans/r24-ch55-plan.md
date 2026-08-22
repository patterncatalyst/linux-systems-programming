---
title: "r24 / ch55 — Boost.Asio — plan (internal)"
published: false
---

# r24 ch55 — example `55-boost-asio` + chapter + diagrams

Seventh chapter of Part 14. Scope was settled in `_plans/r18-ch49-plan.md` ("ch55 scope,
settled 2026-08-16") when the outline row was found to list six topics for eight chapters
and ch55 duplicated ch54's description.

## Overlap check (done FIRST, as in r22 and r23)

- **ch09** owns epoll readiness, level/edge, `EPOLLONESHOT`, timerfd/signalfd.
- **ch22** owns nonblocking sockets, `EAGAIN`, and the hand-written state machine.
- **ch27** owns a hand-rolled coroutine reactor *over* epoll.
- `grep -i "boost::asio" _docs/*.md` → **only forward-references** (ch49, ch53). No prior
  chapter uses Asio.

**So ch55 must not re-derive epoll, readiness, or `EAGAIN`.** Those three chapters built
the reactor by hand; ch55 is about what a *library* adds on top of it, and every section
below is an Asio concept with no hand-rolled counterpart earlier in the book.

## Host audit (2026-08-20, Fedora 44, Boost 1.90.0, GCC 16.1.1)

Header-only Asio + `-lboost_system -pthread`. **No Conan, no network** (the demos use a
`local::connect_pair` socketpair, so nothing binds a port).

### 1. `executor_work_guard` — why `run()` returns immediately

| case | `run()` returned after |
| --- | --- |
| no work posted | **0 handlers** |
| work guard held, one post, guard reset | **1 handler** |

`io_context::run()` returns when there is no *outstanding work*, not when you are done.
The single most common Asio surprise, and it is a two-line measurement.

### 2. The strand — THE finding

2000 posts, 8 threads running one `io_context`, a **deliberately non-atomic** `long`
counter incremented in every handler:

| variant | counter | max_inflight | overlaps |
| --- | --- | --- | --- |
| `post(strand, …)` | **2000** (correct) | **1** | **0** |
| `post(io_context, …)` | **1983** — 17 increments LOST | 6 | 1315 |

Same code, same threads; the only difference is which executor the handler was posted to.
The unguarded version exhibits a **real, observable data race** — a wrong number, not a
sanitizer warning. This is the book's preferred kind of demonstration.

### 3. A strand is not a mutex, and it is measurably cheaper

Same 20000 increments across 8 threads, counted with ch51's method
(`strace -f -c -e trace=futex`):

| mechanism | futex calls | result |
| --- | --- | --- |
| strand | **53** | 20000 (correct) |
| `std::mutex` | **1106** | 20000 (correct) |

~20x fewer. Both correct. The reason is structural: a mutex *blocks a thread* on
contention (ch51 measured that as `FUTEX_WAIT`), while a strand *defers the handler* and
lets the thread pick up other work. Serialization without blocking.

### 4. Scatter-gather is one syscall

Four buffers written to a socket, counted by syscall:

| form | write-family syscalls |
| --- | --- |
| `asio::write(sock, array<const_buffer,4>)` | **1 `sendmsg`** |
| four separate `asio::write` calls | **4 `sendto`** |

Asio maps a buffer *sequence* onto the kernel's iovec form. 4 → 1, and it ties straight
back to Part 5's I/O chapters.

### 5. Threading topologies, by tid

400 handlers, tids collected with `gettid()` (ch50's instrument, ch49's vocabulary):

| topology | distinct tids |
| --- | --- |
| one thread, one `io_context` | **1** |
| N threads on one `io_context` | **4** |
| one `io_context` per thread | **4** |
| `asio::thread_pool(4)` | **4** |

## The core demo — `asiodemo` (C++-only, `langs: [cpp]`)

Subcommands: `work-guard`, `strand`, `nostrand`, `strand-cost`, `mutex-cost`, `gather`,
`separate`, `topology-<one|pool|percore|threadpool>`, `cancel`, `versions`.

Digest continuity: `digest=0x481984990deee5ff`.

## Gate tiers

**Hard (A–F):**
- A. digest identical across every subcommand.
- B. work guard: `run()` returns after 0 handlers with no work; > 0 with a guard.
- C. **the strand**: `strand` reports the exact expected count with `max_inflight=1` and
  `overlaps=0`. `nostrand` reports `overlaps > 0` — gate the *overlap*, NOT the lost
  count. A lost increment is a data race and therefore UB; its magnitude is not
  reproducible and 2000 is a legitimate outcome. (ch49's rule about never gating on UB
  output.) Report the count, assert only that handlers overlapped.
- D. strand vs mutex: both produce the correct count, and the strand issues materially
  fewer futex calls. Gate the *sign* (strand < mutex/2), never the magnitudes. Degrade
  to SKIP if strace cannot attach, as ch51 does.
- E. gather: the 4-buffer write costs exactly one write-family syscall and the separate
  form costs four. Gate the counts — they are deterministic, unlike timings.
- F. topologies: `one` reports exactly 1 tid; the three multi-threaded forms report > 1.

**Gated-if-present (G):** clang parity.

**Not gated:** anything needing a network port; timings; the exact lost-increment count.

## Risks

- **`nostrand`'s data race is UB.** It may legitimately produce 2000 on some run. Gate
  `overlaps > 0` (which is a real, non-UB observation about scheduling) and merely
  *report* the counter. Never assert it is wrong.
- **strace may be ptrace-restricted** → gate D degrades to an informational SKIP.
- **Syscall choice may vary** (`sendmsg` vs `writev` depending on socket type). Gate the
  COUNT of write-family syscalls, not the specific name.
- **No ports.** Use `local::connect_pair` so nothing binds and the example stays
  `mode: local` and offline.
- **Do not re-derive ch09/ch22/ch27.** No epoll walkthrough, no `EAGAIN` state machine,
  no hand-rolled reactor. Cite them; build on them.

## Steps

S1 mint + strip · S2 `cpp/src/asiodemo.cpp` + CMakeLists · S3 `verify.lua` · S4 manifest ·
S5 transcripts · S6 `_docs/55-boost-asio.md` · S7 2 diagrams + catalogue · S8 gate matrix ·
S9 PR, stop there.

## Acceptance

`verify.lua` PASS/FAIL 0 with C, D, E, F running for real; runner 1 passed; `validate.py`
OK; every chapter code block verbatim; footer states Boost 1.90.0, the strand/mutex futex
counts, the syscall counts, and the tid counts as measured.

## CHECKPOINT (2026-08-20) — example green, chapter NOT yet written

**Power-loss checkpoint. Everything below is committed and pushed.**

### State: DONE and green
- `examples/55-boost-asio/` complete: `cpp/src/asiodemo.cpp` (13 subcommands), CMakeLists,
  both demo.sh, `verify.lua`.
- `LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua` → **PASS 48 / FAIL 0**, every
  gate running for real including strace (D, E) and clang parity (G).
- `python3 scripts/test-all-examples.py --only 55-boost-asio` → 1 passed.
- Registered in `examples/manifest.yaml` (`langs: [cpp]`, `mode: local`, `timeout: 900`).
- Both diagrams emitted AND catalogued: `55-strand-not-a-mutex`,
  `55-topologies-and-gather`.
- `validate.py` → OK.

### REMAINING WORK — DONE (2026-08-21)
1. ~~`examples/55-boost-asio/README.md`~~ rewritten.
2. ~~`_docs/55-boost-asio.md`~~ written — 32 KB, `part: "Compendium: C++ Concurrency"`,
   full spine, both `Figure 55.x` includes, Tools used box.
3. Gate matrix, all re-run on the host this session:
   - `LSP_LANG=cpp REPO_ROOT=… lua verify.lua` → **PASS 48 / FAIL 0** (C, D, E, F, G real)
   - `python3 scripts/test-all-examples.py --only 55-boost-asio` → **1 passed**
   - `python3 scripts/validate.py` → **OK**
   - verbatim-block check → 12/12 blocks match `asiodemo.cpp` / `verify.lua` / `CMakeLists.txt`
   - banned words → none; cross-refs ch09/22/27/39/46/49/50/51/52/53/54 all resolve
4. PR — remaining.

### Numbers as re-measured 2026-08-21 (what the chapter quotes)
`nostrand` counter=1862 lost=138 max_inflight=6 overlaps=1492; five further runs lost
22, 110, 104, 44, 24 with overlaps 1375-1507. Futex: strand 52 vs mutex 1022 in the gated
run, plus 52/54/55 against 1022/992/965. Figure 55.1 was rendered from an earlier run
(1897 of 2000, 1441 overlaps, 982/61 futex) — the chapter says so explicitly rather than
re-rendering, because the varying magnitude IS the lesson about not gating on UB.
Deterministic and unchanged: gather=1 `sendmsg`, separate=4 `sendto`, 56 bytes; tids
1 / 8 / 4 / 4; digest `0x481984990deee5ff` on all 13 cases and under clang.

### Captured transcripts (all real runs this session — quote these)
```
versions: Boost 1.90.0 asio_header_only=yes
work-guard: without_work_handlers=0 with_guard_handlers=1 posted=1
work-guard: returned_immediately=yes guard_kept_it_alive=yes
strand: posts=2000 counter=2000 expected=2000 correct=yes
strand: max_inflight=1 overlaps=0 serialized=yes
nostrand: posts=2000 counter=1897 expected=2000 lost=103
nostrand: max_inflight=6 overlaps=1441 serialized=no
gather: buffers=4 wrote=56 peer_read=56 intact=yes
separate: buffers=4 wrote=56 peer_read=56 intact=yes
cancel: handlers_run=1 handler_ran=yes error=Operation canceled aborted=yes
topology-one: handlers=400 distinct_tids=1
topology-pool: handlers=400 distinct_tids=8
topology-percore: handlers=400 distinct_tids=4
topology-threadpool: handlers=400 distinct_tids=4
```

### Stability data (measured, use for the "gate the sign" prose)
`nostrand` over 5 runs: lost = 67, 37, 41, 35, 34 — **magnitude varies every run**, which
is exactly why the lost count is reported and never gated. `overlaps` was 1308–1392 on
those same runs, always >> 0, which is what IS gated.

futex counts over 3 runs each: `strand-cost` 69 / 50 / 51 · `mutex-cost` 964 / 871 / 957.
Sign rock solid (~15-20x), magnitude never. Gate is `strand < mutex/2`.

Scatter-gather syscalls (deterministic, gated exactly): `gather` → **1** (`sendmsg` with
4 iovecs); `separate` → **4** (`sendto`). Plain `write(2)` excluded — that is stdout.

### Notes for the chapter
- ch09/ch22/ch27 own epoll, EAGAIN, and the hand-rolled reactor. **Do not re-derive.**
  Cite them; ch55 is about what the library adds above that layer.
- `topology-pool` reports **8**, not 4 — it reuses `run_on_threads` (kHandlerThreads=8).
  Gate is `> 1`, so this is fine; just quote 8 accurately.
- The three cancellation models now measured across the book: ch51 `stop_token` (a flag),
  ch52 `thread_interrupted` (an ordinary exception), ch55 `cancellation_signal` (a
  COMPLETION carrying `operation_aborted` — the handler still runs exactly once, so
  ownership rules never change). That is a genuine third shape, worth a section.
