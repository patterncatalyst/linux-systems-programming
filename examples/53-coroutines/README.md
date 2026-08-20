# 53 — C++20 coroutines: what a suspended computation costs

Chapter 27 already built a working coroutine engine — `promise_type`, the
awaitable protocol, `await_suspend` returning a handle for symmetric transfer,
and an epoll reactor that parks and resumes handles. **That chapter owns the
mechanism, and this example does not repeat any of it.**

This one asks the question ch27 never did, and it is a systems question: a
coroutine frame and a thread both hold one paused computation, so what does each
one cost?

```
[host]$ ./demo.sh cpp run versus-thread
versus-thread: coroutine_frame=32 bytes thread_stack=8388608 bytes (ch50)
versus-thread: ratio=262144x frames_per_thread_stack=262144
```

Chapter 50 measured that 8 MiB with `pthread_getattr_np`. The 32 bytes is
measured here. **262,144 coroutine frames fit in one thread's stack** — which is
the entire reason coroutines exist.

## The cases

| subcommand | what it measures |
| --- | --- |
| `frames` | frame size for three shapes: 32 / 304 / 4128 bytes |
| `halo` | whether the compiler elides the frame allocation — **it depends** |
| `generator` | C++23 `std::generator`, which ch27 had to hand-roll |
| `versus-thread` | frame bytes against ch50's measured thread stack |
| `lifetime` | the trap: a `coroutine_handle` owns nothing |
| `versions` | what the toolchain reports |

Every case prints `digest=0x481984990deee5ff` — the value ch46 through ch52 all
produce.

## How the frame is measured

A coroutine frame is allocated by the compiler, not by you. The only portable
way to see its size is to intercept the allocation:

```cpp
        static void* operator new(std::size_t n) {
            g_last_frame = n;
            ++g_alloc_count;
            return ::operator new(n);
        }
```

Declare `operator new` inside `promise_type` and every frame allocation for that
coroutine type comes through it.

```
[host]$ ./demo.sh cpp run frames
frames: trivial=32 small=304 large=4128
```

The frame holds the promise, resume/destroy pointers, a state index, and **the
locals that are live across a suspension** — nothing else. A coroutine carrying
a `char[4096]` across a `co_await` has a 4128-byte frame; one carrying nothing
has 32. Cost is what you carry, not a flat overhead.

## HALO: permitted, guaranteed by nobody

`[dcl.fct.def.coroutine]` says an implementation **may** elide the frame
allocation when the frame provably does not outlive its caller. May. There is no
conforming way to depend on it — and on this host the two compilers disagree
completely.

1000 calls to an eager coroutine that never suspends, the easiest possible case:

| compiler | -O0 | -O1 | -O2 | -O3 |
| --- | --- | --- | --- | --- |
| **g++ 16.1.1** | 1000 | 1000 | 1000 | 1000 |
| **clang++ 22.1.8** | 1000 | **0** | **0** | — |

Same source, same standard, same `-O2`: a thousand heap allocations, or none.
"The optimizer will remove it" is a claim about your compiler, not about C++.

`verify.lua` deliberately **does not gate on whether elision happens** — that
would encode one compiler's choice as a rule. It gates that the count is
measured and internally consistent, and gate F compares the two compilers.

## The lifetime trap

```
[host]$ ./demo.sh cpp run lifetime
lifetime: abandoned alloc=1 free=0 leaked=yes
lifetime: owned     alloc=1 free=1 leaked=no
```

A `std::coroutine_handle` is a raw pointer with a nicer name. It does not own
the frame, it is not RAII, copying it copies a pointer, and destroying it frees
nothing. Exactly one `destroy()` per frame, and the type system will not remind
you.

Note the observable is the **free** count, not the allocation count — both
halves allocate one frame, so allocations cannot tell them apart. The question
is whether anyone called `destroy()`.

## Layout

```
53-coroutines/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-E hard, F skip-if-present
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/coro.cpp
```

C++23 standard library only — **no threads, no Boost, no network**. Local and
unprivileged. C++23 rather than C++20 for `<generator>`.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run frames
[host]$ ./demo.sh cpp run halo
[host]$ ./demo.sh cpp run versus-thread
```

To reproduce the HALO matrix yourself:

```
[host]$ for o in O0 O1 O2 O3; do
          g++ -std=c++23 -$o -o /tmp/h cpp/src/coro.cpp && /tmp/h halo | head -1
        done
```

## Verification

```
[host]$ LSP_LANG=cpp lua verify.lua
```

- **A** — every case computes the expected digest.
- **B** — frames track live-across-suspend locals: trivial is overhead-only, the
  `char[4096]` case exceeds 4096, and the middle sits between. **Relationships
  are gated, never exact byte counts** — those are ABI-dependent.
- **C** — HALO is measured and internally consistent. Deliberately *not* an
  assertion that elision does or does not happen, so a future GCC that starts
  eliding will not fail the gate.
- **D** — `std::generator` produces the correct 20th Fibonacci term (4181) and
  is destroyed mid-suspension.
- **E** — the frame is at least three orders of magnitude below ch50's 8388608,
  and the ratio is the arithmetic of the two measured numbers rather than a
  quoted figure.

**F** (clang parity + the HALO contrast) runs if `clang++` is present.
