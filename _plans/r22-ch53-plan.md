---
title: "r22 / ch53 — C++20 coroutines — plan (internal)"
published: false
---

# r22 ch53 — example `53-coroutines` + chapter + diagrams

Fifth chapter of Part 14.

## The overlap problem, checked FIRST this time

`_docs/27-async-runtimes-and-coroutines.md` already goes deep: `promise_type`,
`initial_suspend`/`final_suspend`, the awaitable protocol, `await_suspend` returning a
handle (**symmetric transfer**), `coroutine_handle`, and a full epoll reactor that parks
and resumes handles. Its Figure 27.1 walks a `co_await ReadAwaitable` suspension end to
end.

**So ch53 must not re-derive any of that.** ch27 owns "coroutines as an async I/O
mechanism over epoll". (ch51 shipped with this exact problem found late; the lesson was
to check before planning, not during drafting.)

## What is left, and it is the systems question ch27 never asked

ch27 showed coroutines *working*. It never asked what one **costs**. That is this
chapter, and it connects straight to ch50:

| suspended computation | cost | source |
| --- | --- | --- |
| a thread | **8388608 bytes** of stack | ch50, measured |
| a coroutine frame | **32 bytes** | measured below |

**262,144× ratio.** That single number is why coroutines exist, and neither ch27 nor any
earlier chapter states it.

## Host audit (2026-08-19, Fedora 44, GCC 16.1.1, clang 22.1.8)

Frame sizes, measured by overloading `operator new` inside `promise_type`:

| coroutine | frame |
| --- | --- |
| trivial (`co_await suspend_always` only) | **32 bytes** |
| four `int`s + `char[256]` | **304 bytes** |
| `char[4096]` | **4128 bytes** |

The frame holds exactly the locals that are **live across a suspension**, plus ~32 bytes
of bookkeeping (resume/destroy pointers, promise, state index). It is not a fixed cost;
it is proportional to what you carry across `co_await`.

### THE FINDING — HALO is real, and it is compiler-dependent

Heap Allocation eLision Optimization: when the compiler can prove a frame does not
outlive its caller, it may elide the allocation entirely. 1000 calls to an eager
coroutine that never suspends:

| compiler | -O0 | -O1 | -O2 | -O3 |
| --- | --- | --- | --- | --- |
| **g++ 16.1.1** | 1000 | 1000 | 1000 | 1000 |
| **clang++ 22.1.8** | 1000 | **0** | **0** | — |

clang elides **every** allocation from -O1 up. GCC 16 elides **none, at any level**.
"The optimizer will remove the allocation" is a claim that is true on one of the two
compilers on this host, and this is exactly the kind of thing the book measures rather
than repeats.

### C++23 `std::generator`

`__cpp_lib_generator = 202207` — present and working (`fib()` to the 20th term = 4181).
ch27 hand-rolled a generator; C++23 ships one, and the contrast is worth one section.
`__cpp_impl_coroutine = 201902`, `__cpp_lib_coroutine = 201902`.

## The core demo — `coro` (C++-only, `langs: [cpp]`)

Subcommands: `frames` (sizes for three shapes), `halo` (allocation count, the compiler
split), `generator` (C++23 `std::generator`), `versus-thread` (frame bytes vs ch50's
8 MiB, and how many frames fit in one thread stack), `lifetime` (the dangling-frame
trap), `versions`.

Digest continuity: `digest=0x481984990deee5ff`.

## Gate tiers

**Hard (A–E):**
- A. digest identical across every subcommand.
- B. frames: trivial frame is small (< 128 B) and a `char[4096]` coroutine's frame
  exceeds 4096 — i.e. the frame tracks live-across-suspend locals. Gate the
  RELATIONSHIP, not the exact byte counts (ABI-dependent).
- C. HALO: **counted, and reported per compiler**. Assert only that the count is
  measured and internally consistent (GCC's count == calls; and if clang is present,
  clang's count < GCC's). Never assert "elision happens" — it does not, on GCC.
- D. `std::generator` produces the correct Fibonacci term (4181), so C++23's generator
  is exercised rather than described.
- E. versus-thread: the reported frame size is at least three orders of magnitude below
  ch50's 8388608, and the ratio is printed.

**Gated-if-present (F):** clang parity on the digest AND the HALO contrast.

**Not gated:** exact frame byte counts; anything ch27 owns (epoll integration,
symmetric transfer).

## Risks

- **Re-deriving ch27.** Mitigation: no epoll, no reactor, no awaitable protocol
  tutorial. ch53 measures cost; ch27 owns mechanism. Chapter must cite ch27 early and
  explicitly, the way ch51 ended up citing ch25.
- **Frame sizes are ABI- and compiler-dependent.** Gate relationships (trivial < 128,
  big > 4096), print exact numbers, and state the toolchain in the footer.
- **HALO could start working on a future GCC.** Gate C is written so that would not
  fail it — it asserts the measurement happened and that clang <= GCC, not that GCC
  fails to elide.
- **`std::generator` needs `<generator>`**, C++23, and libstdc++ 14+. Present here;
  a missing header would be a compile error, so no runtime guard is needed.

## Steps

S1 mint + strip · S2 `cpp/src/coro.cpp` · S3 `verify.lua` · S4 manifest · S5 transcripts ·
S6 `_docs/53-coroutines.md` · S7 2 diagrams + catalogue · S8 gate matrix · S9 PR, stop.

## Acceptance

`verify.lua` PASS/FAIL 0 with C and F running for real; runner 1 passed; `validate.py` OK;
every chapter code block verbatim; footer states GCC 16.1.1 / clang 22.1.8, the frame
sizes, and the HALO split as measured.
