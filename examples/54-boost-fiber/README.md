# 54 — Boost.Fiber: what a real stack buys

Chapter 53 measured a C++20 coroutine frame at **32 bytes** — stackless, holding
only the locals that cross a suspension. A Boost fiber is **stackful**: it owns
a real machine stack, switched by `boost::context`.

That one difference produces both its price and its capability, and this example
measures both.

## The memory trilogy

```
[host]$ ./demo.sh cpp run versus
versus: thread=8388608 fiber=131072 coroutine_frame=32
versus: fibers_per_thread_stack=64 frames_per_fiber_stack=4096
versus: fiber_between_the_two=yes
```

| paused computation | cost | measured in |
| --- | --- | --- |
| thread | 8388608 B (8 MiB) | ch50, `pthread_getattr_np` |
| **fiber** | **131072 B (128 KiB)** | ch54, `stack_traits::default_size()` |
| coroutine frame | 32 B | ch53, `operator new` in `promise_type` |

**8 MiB → 128 KiB → 32 bytes**, roughly two orders of magnitude at each step,
every number measured on this host rather than quoted. Both ratios are computed
by the program from those numbers.

## What the stack buys

The cost is not the interesting half. This is:

```
[host]$ ./demo.sh cpp run deep
deep: yielded_at_depth=6 resumed_at_depth=6 same=yes
deep: suspended from a plain function 6 frames below the fiber body -- no co_await, no coroutine, no colouring
```

`leaf()` is an ordinary function. So is `level()`. Neither is a coroutine — yet
the fiber suspends from inside `leaf()`, six frames below where the fiber body
started, and resumes exactly there.

**A stackless coroutine cannot do this at any price.** `co_await` is only valid
in the body of a coroutine, so every frame between the suspension point and the
scheduler must itself be a coroutine — the "function colouring" problem. 128 KiB
of stack is exactly what buys the ability to suspend anywhere.

## The scheduler is a choice

The same 16 fibers, the same work, one line apart:

```
[host]$ ./demo.sh cpp run roundrobin
roundrobin: fibers=16 distinct_tids=1

[host]$ ./demo.sh cpp run sharedwork
sharedwork: fibers=16 worker_threads=4 distinct_tids=4
```

The default `round_robin` scheduler keeps a per-thread queue and never migrates
a fiber, so 16 tasks are in flight on one CPU — **concurrent, not parallel**, in
ch49's exact terms. One call to
`use_scheduling_algorithm<algo::shared_work>()` puts them on a shared queue that
four threads pull from, and the same fibers now occupy four CPUs.

Chapter 44 covered Go's answer: the GMP runtime decides, automatically, and
mostly will not be argued with. C++ makes you choose — more work, more control,
and the choice is observable in `gettid()`.

## The guard page

```
[host]$ ./demo.sh cpp run stacks
stacks: default=131072 minimum=14528 page=4096 unbounded=yes
stacks: requested=65536 fixedsize=65536 protected=69632 guard_delta=4096
stacks: exact_request=yes guard_is_one_page=yes
```

`fixedsize_stack` gives exactly what you ask for. `protected_fixedsize_stack`
gives exactly one page more — and that page is mapped `PROT_NONE`, so running
off the end of the stack faults instead of corrupting whatever is mapped next:

```
[host]$ ./demo.sh cpp run guard-ok        # 5 frames on a 64 KiB stack
guard-ok: 5 frames on a 65536-byte protected stack completed normally
                                          # exit 0

[host]$ ./demo.sh cpp run guard-overflow  # 1000 frames — 60x past it
guard-overflow: recursing 1000 frames on a 65536-byte protected stack; this is expected to die on the guard page
                                          # exit 139 = SIGSEGV
```

`guard-overflow` dies by design and is its own subcommand for the reason ch50's
`cancel-swallow` and ch51's `deadlock-naive` were. The gate asserts the **exit
signal** and never anything the faulting run printed — output after a stack
overflow is undefined, and gating on it would be gating on UB.

## Layout

```
54-boost-fiber/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-F hard, G skip-if-present
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── demo.sh
    └── src/fiberdemo.cpp
```

**System Boost only — no Conan, no network.** Links `-lboost_fiber
-lboost_context -pthread`. Local, unprivileged, no VM, no root.

The translation unit is compiled `-O0` deliberately. At `-O2` both compilers may
inline the recursion into a loop or drop the `volatile` array's storage, and the
fiber would never approach its guard page — `guard-overflow` would silently stop
demonstrating anything. Chapter 49 learned that lesson when an unconsumed burn
loop was deleted and the example measured the optimizer instead of the scheduler.

## Build and run

```
[host]$ ./demo.sh cpp build
[host]$ ./demo.sh cpp run versus
[host]$ ./demo.sh cpp run deep
[host]$ ./demo.sh cpp run roundrobin
[host]$ ./demo.sh cpp run sharedwork
[host]$ ./demo.sh cpp run guard-overflow   # faults by design, exit 139
```

## Verification

```
[host]$ LSP_LANG=cpp lua verify.lua
```

- **A** — every non-faulting case computes the expected digest.
- **B** — a `fixedsize_stack` returns exactly what was asked; the protected one
  returns exactly one page more. **The guard delta is gated against
  `page_size()`**, never a hardcoded 4096.
- **C** — the fiber stack sits strictly between ch53's 32 B and ch50's
  8388608 B, and both ratios are arithmetic on measured numbers.
- **D** — a fiber suspends from an ordinary function more than one frame deep
  and resumes at the same depth.
- **E** — `round_robin` reports exactly 1 tid; `shared_work` reports more than 1.
  Sign gated, never magnitude — with a bounded retry, since a loaded host could
  legitimately keep the work on fewer threads. Measured 4 of 4 on five runs here.
- **F** — the in-budget fiber exits 0; the overflow dies of a signal. A `124`
  (timeout, i.e. hung rather than faulted) deliberately does **not** pass.

**G** (clang parity) runs if `clang++` is present.

**Not gated:** context-switch timings. This book does not gate on durations
(ch39); the fiber-versus-thread claim here is about memory and about *where* a
suspension may occur, both of which are counts.
