---
title: "C++20 coroutines: what a suspended computation costs, and the allocation that may not happen"
order: 53
part: "Compendium: C++ Concurrency"
description: "coro measures what Chapter 27 built but never priced. A coroutine frame is measured by overloading operator new inside promise_type -- 32 bytes trivial, 304 carrying four ints and a char[256], 4128 carrying a char[4096] -- against the 8388608-byte thread stack Chapter 50 measured with pthread_getattr_np, a ratio of 262144x. Heap allocation elision turns out to be a permission rather than a guarantee: across 1000 calls to the easiest possible case, g++ 16.1.1 elides none at -O0 through -O3 while clang++ 22.1.8 elides every one from -O1 up, so verify.lua gates that the count is measured rather than that elision occurs. Plus C++23 std::generator exercised to the 20th Fibonacci term and the coroutine_handle lifetime trap counted through operator delete. Verified on the Fedora 44 host: verify.lua PASS 32/FAIL 0, C++23 stdlib only, no threads, no network."
duration: "55 minutes"
---

Chapter 27 built a working coroutine engine. It defined `promise_type`s, wrote
the awaitable protocol, returned a handle from `await_suspend` for symmetric
transfer, and parked and resumed handles on an epoll reactor — a real async
server whose connections suspended on I/O instead of blocking threads. That
chapter owns the mechanism, and this one does not repeat a line of it.

What Chapter 27 never asked is what a coroutine *costs*.

That turns out to be a systems question with a specific answer, and the answer
is the reason the feature exists. A thread and a coroutine frame both hold one
paused computation. Chapter 50 measured a thread's stack at 8388608 bytes. This
chapter measures the frame.

{% include excalidraw.html
   file="53-frame-versus-stack"
   alt="One paused computation priced two ways across two bands. The upper band, a thread as measured in ch50, holds a box for std::thread and pthread listing 8388608 bytes of stack mmap'd up front per thread plus a kernel task_struct and a tid in /proc; a note that the stack is virtual so untouched pages are never backed and it usually works, but still collides with overcommit limits, cgroup accounting and 32-bit address spaces; and a box noting a thread can be interrupted anywhere because preemptive scheduling moves it whether or not the code cooperates. The lower amber band, a coroutine frame measured via operator new in the promise, holds an amber box listing frame sizes of 32 bytes trivial, 304 with four ints and a char[256], and 4128 with a char[4096], heap allocated unless elided; a note that the frame holds only the promise, resume and destroy pointers, a state index, and the locals live across a suspend, so cost is what you carry; and a box noting it suspends only at co_await, cooperatively, returning control to the caller with nothing preempted or blocked. An amber box across the bottom reads 262,144 coroutine frames fit in one thread's stack."
   caption="Figure 53.1 — a thread and a coroutine frame each hold one paused computation; only one of them reserves 8 MiB of address space and a kernel task to do it" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated — it carries a real gate here, not a formality: the heap-elision
> contrast below is a comparison between the two compilers), `lua` (host;
> gated, runs `verify.lua`), and `python3` (host; gated, runs
> `scripts/test-all-examples.py`). No VM, no root, no network, and no Boost:
> `examples/53-coroutines` is `mode: local` and builds from the C++23 standard
> library alone.

C++-only, like the four chapters before it. C++23 rather than C++20, for
`<generator>`.

## Measuring a thing the language hides

A coroutine frame is allocated by the compiler. You do not call anything, there
is no type whose `sizeof` you can take, and the standard deliberately declines
to specify the layout. So the size is not directly observable — except through
one door the standard does leave open:

```cpp
        static void* operator new(std::size_t n) {
            g_last_frame = n;
            ++g_alloc_count;
            return ::operator new(n);
        }
```

Declare `operator new` inside `promise_type` and every frame allocation for that
coroutine type routes through it. The compiler tells you the size because it has
to ask you for the memory.

```
[host]$ ./demo.sh cpp run frames
frames: trivial=32 small=304 large=4128
frames: overhead_only=yes tracks_live_locals=yes carries_4k_buffer=yes
```

Three coroutines, three frames. The first suspends immediately and holds
nothing:

```cpp
Counted frame_trivial() { co_await std::suspend_always{}; }
```

**32 bytes.** That is the whole fixed cost of a C++20 coroutine: a promise, two
function pointers (resume and destroy), and a state index saying where to
continue. Not a page, not a stack — thirty-two bytes.

The second carries four `int`s and a 256-byte buffer across its suspension and
comes to 304. The third carries a `char[4096]` and comes to 4128. The pattern is
the point:

```cpp
Counted frame_large() {
    char buf[4096]{};
    co_await std::suspend_always{};
    (void)buf[0];
}
```

The frame holds exactly what is **live across a suspension**. A local used only
before the first `co_await`, or only between two of them, need not be stored at
all — the compiler is doing liveness analysis and the frame is the result. Frame
cost is what you carry, not a flat overhead, which is why the trivial and large
cases differ by two orders of magnitude.

That also makes frame size something you control. A coroutine that holds a large
buffer across an await pays for it on every suspension; one that acquires the
buffer after resuming does not.

## The number this chapter exists for

```
[host]$ ./demo.sh cpp run versus-thread
versus-thread: coroutine_frame=32 bytes thread_stack=8388608 bytes (ch50)
versus-thread: ratio=262144x frames_per_thread_stack=262144
versus-thread: three_orders_of_magnitude=yes
```

The 8388608 is not a round number someone remembered — it is Chapter 50's
measurement, taken with `pthread_getattr_np` on this host, and it is a named
constant in the source so the comparison cites a real prior result:

```cpp
constexpr std::size_t kCh50ThreadStackBytes = 8388608;
```

262,144 coroutine frames fit in the address space one thread reserves for its
stack.

Two qualifications, because the comparison is easy to overstate. First,
that 8 MiB is *virtual* — untouched pages are never backed by physical memory,
which is why thread-per-connection servers work at all. Second, the two are not
interchangeable: a thread can be preempted anywhere, and a coroutine suspends
only where you wrote `co_await`. Chapter 49's vocabulary is the precise way to
say it — a thread buys you parallelism, a coroutine frame buys you only
concurrency.

But the address space is real, and so is the kernel `task_struct` and the tid
in `/proc` that Chapter 50 catalogued. Ten thousand threads is a system
configuration question. Ten thousand coroutine frames is 320 KB.

## HALO: permitted, guaranteed by nobody

Here is where the received wisdom fails a measurement.

{% include excalidraw.html
   file="53-halo-is-permitted"
   alt="Heap allocation elision presented as a permission rather than a guarantee, in two bands. The upper band, what the standard says, quotes dcl.fct.def.coroutine that the implementation MAY elide the frame allocation when the frame provably does not outlive the caller, emphasising that may means there is no conforming way to depend on it; beside a note that the test case is the easiest one there is, with suspend_never at both ends so the coroutine runs to completion inside the call and its frame provably dies there, so if elision happens anywhere it happens here. The lower amber band, what the two compilers on this host actually do across 1000 calls each, shows g++ 16.1.1 allocating 1000 at -O0, -O1, -O2 and -O3 and eliding none at any level, beside an amber box showing clang++ 22.1.8 allocating 1000 at -O0 but zero at -O1 and -O2, eliding every one from -O1 up. A closing amber box reads: same source, same standard, same -O2, a thousand heap allocations or none, so the optimizer will remove it is a claim about your compiler and not about C++."
   caption="Figure 53.2 — the same source at the same optimization level allocates 1000 frames under GCC and none under clang; elision is permitted by the standard and required of nobody" %}

The coroutine frame is heap-allocated by default, and the standard permits an
implementation to skip that allocation when it can prove the frame does not
outlive its caller. This is HALO — Heap Allocation eLision Optimization — and it
is the usual answer to "isn't a heap allocation per coroutine expensive?"

The test case is the easiest one that exists. `suspend_never` at both ends, so
the coroutine runs to completion inside the call and its frame provably dies
there:

```cpp
Eager eager_add(int* sink) {
    *sink += 1;
    co_return;
}
```

A thousand calls, counted:

```
[host]$ ./demo.sh cpp run halo
halo: calls=1000 allocations=1000 elided=0
halo: work_done=1000 fully_elided=no
```

A thousand allocations. None elided. That is `g++ 16.1.1` at `-O2`, and it does
not change at any optimization level:

```
[host]$ for o in O0 O1 O2 O3; do g++ -std=c++23 -$o -o /tmp/h cpp/src/coro.cpp && /tmp/h halo | head -1; done
halo: calls=1000 allocations=1000 elided=0
halo: calls=1000 allocations=1000 elided=0
halo: calls=1000 allocations=1000 elided=0
halo: calls=1000 allocations=1000 elided=0
```

The same source under clang:

```
[host]$ for o in O0 O1 O2; do clang++ -std=c++23 -$o -o /tmp/h cpp/src/coro.cpp && /tmp/h halo | head -1; done
halo: calls=1000 allocations=1000 elided=0
halo: calls=1000 allocations=0 elided=1000
halo: calls=1000 allocations=0 elided=1000
```

Every allocation gone, from `-O1` up.

Same source. Same standard. Same `-O2`. One compiler allocates a thousand times
and the other allocates zero times. The standard's wording is `may`:
`[dcl.fct.def.coroutine]` permits the elision and requires it of no one, so both
compilers are conforming and neither is broken.

What that means practically: **"the optimizer will remove it" is a claim about
your compiler, not about C++.** If you are writing coroutine-heavy code on a
hot path and you have reasoned that the frame allocation disappears, that
reasoning is portable only as far as your toolchain. Measure it the way this
example does — the instrumentation is six lines and works anywhere.

### What the gate does about it

This put a design question to the harness. The tempting gate is "assert elision
does not happen," since that is what GCC does here. It would be wrong: it would
encode one compiler's current behavior as a rule, and a future GCC that
implements HALO would fail a test for doing something better.

So gate C asserts only that the measurement is coherent:

```lua
checks.expect_match(tostring(allocs ~= nil and calls ~= nil and allocs <= calls), "true",
  "cpp: the allocation count is internally consistent (allocations=" .. tostring(allocs) ..
  " <= calls=" .. tostring(calls) .. ")")
```

The count never exceeds the number of calls, the reported `elided` figure is the
arithmetic of the two, and all thousand coroutine bodies ran regardless. Whether
elision fires is printed, not asserted. The comparison between compilers is gate
F, and it asserts a relationship — clang elides at least as many as GCC — which
stays true whichever way GCC moves.

## C++23's `std::generator`

Chapter 27 hand-rolled a generator: a `promise_type` with `yield_value`, a
handle wrapper, an iterator, and the begin/end machinery to make a range-for
work. C++23 ships that:

```cpp
std::generator<std::uint64_t> fibonacci() {
    std::uint64_t a = 0, b = 1;
    while (true) {
        co_yield a;
        const std::uint64_t next = a + b;
        a = b;
        b = next;
    }
}
```

```
[host]$ ./demo.sh cpp run generator
generator: took=20 twentieth_fib=4181 expected=4181 correct=yes
generator: an infinite coroutine consumed finitely, then destroyed mid-suspension
```

`__cpp_lib_generator` reports `202207` on this toolchain, so this is exercised
rather than described.

Two things in that eleven-line function are worth noticing. It is an **infinite**
loop with no termination condition, consumed finitely by breaking out of the
range-for — and that is not a leak. Breaking destroys the generator, which
destroys the frame, which abandons a coroutine suspended at `co_yield`.
Destroying a suspended coroutine is well-defined: locals in scope at the
suspension point are destroyed, and the frame is freed.

And the state is *in the loop*, not in a class. The hand-rolled equivalent in
Chapter 27 needed the generator's state stored as members somewhere; here `a`
and `b` are ordinary locals that happen to live across a `co_yield`. That is
what the frame is for, and it is the ergonomic argument for coroutines
independent of any performance claim.

## The lifetime trap

`std::coroutine_handle` is a raw pointer with a nicer name. It does not own the
frame. Copying it copies a pointer. Destroying it does nothing at all.

```
[host]$ ./demo.sh cpp run lifetime
lifetime: abandoned alloc=1 free=0 leaked=yes
lifetime: owned     alloc=1 free=1 leaked=no
```

The observable there is the **free** count, not the allocation count, and the
distinction matters:

```cpp
    // The observable is not how many frames were ALLOCATED -- both halves
    // allocate one. It is how many were FREED. operator delete on the promise
    // runs only when someone calls destroy(), so counting it distinguishes a
    // frame that was cleaned up from one that was abandoned.
```

Both halves allocate exactly one frame, so an allocation counter cannot tell a
leak from a clean run. `operator delete` on the promise runs only when someone
calls `destroy()`, which makes it the right thing to count.

The fix is the ordinary one — put the handle in a type that owns it:

```cpp
    // A coroutine handle is NOT an owning type. If nothing calls destroy(),
    // the frame leaks -- see the `lifetime` case.
    ~Counted() {
        if (handle) {
            handle.destroy();
        }
    }
```

This is why every coroutine tutorial hands you a wrapper type before it hands
you a coroutine. The wrapper is not ceremony; it is the only thing standing
between you and a leak the type system will not warn about. Note also that
`Counted` deletes its copy operations — a copied handle would give two owners
for one frame and a double `destroy()`.

## How the code works

Each measurement is its own subcommand, following the pattern from Chapters 50
through 52. Nothing here hangs or aborts, so the separation is for clarity
rather than containment.

Two promise types exist because the two questions need different suspension
behavior. `Counted` uses `suspend_always` at initial suspend, so the coroutine
does not start until resumed and the frame provably outlives the call — there is
nothing for the optimizer to elide, which is exactly what the `frames` case
wants:

```cpp
        // suspend_always at initial: the coroutine does not start until it is
        // resumed, so the frame provably outlives this call and there is
        // nothing for the optimizer to elide. `frames` wants the allocation.
        std::suspend_always initial_suspend() noexcept { return {}; }
```

`Eager` uses `suspend_never` at both ends for the opposite reason: the `halo`
case needs the frame to be provably dead at the end of the call, so that elision
is *permitted* and the question "does the compiler take the opportunity" is
meaningful.

Getting that wrong would have produced a much less interesting result. A `halo`
case built on `Counted` would report 1000 allocations under both compilers, and
the correct conclusion would be "elision is impossible here" rather than
anything about the compilers.

## Errors, three ways

**An error the compiler cannot see.** The abandoned frame is a leak with no
diagnostic — no warning, no sanitizer trip in a normal build, no type error.
`coroutine_handle` is specified as a non-owning primitive precisely so that
library authors can build ownership on top of it, and the cost is that using it
raw is unchecked.

**An error that is not an error.** Destroying a coroutine suspended at
`co_yield` — what the `generator` case does on every `break` — looks like
abandoning work mid-flight. It is well-defined and correct, and treating it as
something to avoid would rule out the entire generator idiom.

**An error in reasoning rather than in code.** Assuming HALO fires is the one
this chapter is really about. The code compiles, the tests pass, the behavior is
right; only the performance model is wrong, and only on one of two compilers.
That class of error is invisible to every tool except a measurement.

## Concurrency lens

This is the first chapter in Part 14 with no concurrency in it at all, and that
is worth stating rather than glossing over.

`coro` creates no threads. Every coroutine here runs on the main thread,
suspends to it, and resumes from it. There is no shared mutable state, no
atomic, no lock, and nothing in the example that a data race could touch.

That is the correct shape for the subject. A coroutine is a **suspension**
mechanism, not a concurrency mechanism — it lets one thread hold many paused
computations, and by itself it introduces no parallelism whatsoever. Chapter
49's grid is the precise statement: coroutines give you concurrency as
structure, and nothing on the parallelism axis.

Concurrency arrives only when something schedules the resumptions, and that
something is a separate piece of machinery. Chapter 27's epoll reactor is one;
Chapter 55's Boost.Asio is another. The frame measured here is what those
schedulers move around, which is why it is worth knowing its size before meeting
either.

The one thing worth carrying forward: a coroutine's state lives in a heap frame
rather than on a stack, so two coroutines resumed on two different threads
touch two different heap allocations. The frame is not thread-affine the way a
stack is — which is what makes work-stealing schedulers possible, and what makes
it your job to ensure a frame is not resumed on two threads at once.

## Build, run, observe

```
[host]$ cd examples/53-coroutines
[host]$ ./demo.sh cpp build
[host]$ for c in versions frames halo generator versus-thread lifetime; do
          ./demo.sh cpp run "$c"; done
```

The HALO matrix is worth reproducing rather than reading, since it is the
chapter's least portable claim:

```
[host]$ for o in O0 O1 O2 O3; do
          g++ -std=c++23 -$o -o /tmp/h cpp/src/coro.cpp && /tmp/h halo | head -1
        done
```

Then the harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: a trivial coroutine's frame is bookkeeping only (trivial=32 bytes) -- promise, resume/destroy pointers, state index
ok: cpp: a coroutine holding a char[4096] across a suspend has a frame larger than 4096 (large=4128) -- the buffer really is in there
ok: cpp: the allocation count is internally consistent (allocations=1000 <= calls=1000)
ok: cpp: C++23 std::generator produced the correct 20th Fibonacci term
ok: cpp: a coroutine frame (32 bytes) is at least 1000x smaller than a thread stack -- ratio=262144x
ok: cpp: an abandoned coroutine_handle leaks its frame -- allocated once, freed never
ok: cpp: clang elides at least as many frame allocations as GCC (clang=0 gcc=1000) -- same source, same standard, different answer to a question the standard leaves open
PASS 32 / FAIL 0
```

## Cross-check: two chapters, one comparison

Every case reported `digest=0x481984990deee5ff`, and clang agrees — that
constant now spans Chapters 46 through 53.

The sharper cross-check is the one this chapter is built on. Chapter 50 called
`pthread_getattr_np` and measured 8388608 bytes. Chapter 53 overloaded
`operator new` and measured 32. Neither number was quoted from documentation,
both came from this host, and the ratio between them is computed in the program
rather than written into the prose:

```cpp
    const std::size_t ratio = frame > 0 ? kCh50ThreadStackBytes / frame : 0;
```

That is the shape this book keeps returning to. Chapter 51 measured a mutex
against a baseline instead of against zero; Chapter 52 measured a Boost stack
attribute against Chapter 50's POSIX one and got the same 262144. A comparison
is worth something when both sides are measured.

## What you learned

- **A coroutine frame is 32 bytes of fixed cost** on this toolchain — a promise,
  resume and destroy pointers, and a state index. Not a page, not a stack.
- **Frame size is what you carry across a suspension.** Locals live across a
  `co_await` are in the frame; locals that are not, are not. 32 bytes versus
  4128 for the same shape of coroutine.
- **You can measure it.** Declare `operator new` inside `promise_type` and the
  compiler tells you the size, because it has to ask you for the memory.
- **262,144 frames fit in one thread's stack.** That ratio is why the feature
  exists — with the qualification that thread stacks are virtual, and that a
  coroutine buys concurrency without parallelism.
- **HALO is permitted, not guaranteed.** `[dcl.fct.def.coroutine]` says *may*.
  On this host g++ 16.1.1 elides none at any `-O` level and clang++ 22.1.8
  elides all of them from `-O1` — same source, same standard, both conforming.
- **So measure elision on the compiler you ship.** "The optimizer will remove
  it" is a statement about a toolchain, not about the language.
- **Gate what stays true.** The harness asserts the allocation count is coherent
  and that clang elides at least as many as GCC, never that elision does or does
  not happen — so a future GCC that improves will not fail a test.
- **C++23's `std::generator` replaces the hand-rolled one** from Chapter 27, and
  destroying an infinite generator mid-suspension is well-defined rather than a
  leak.
- **`coroutine_handle` owns nothing.** Count `operator delete`, not
  `operator new`, to tell a leak from a clean run — both allocate once, and only
  one of them frees.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, g++ 16.1.1 20260515,
clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, Lua 5.4.8; local, unprivileged, no
network, no VM, C++23 standard library only): <code>LSP_LANG=cpp
REPO_ROOT=$(cd ../.. &amp;&amp; pwd) lua verify.lua</code> reported <code>PASS 32
/ FAIL 0</code> with gate F -- the clang parity and heap-elision contrast --
running for real rather than skipping, and <code>python3
scripts/test-all-examples.py --only 53-coroutines</code> reported <code>1
passed, 0 failed, 0 skipped</code>. Every transcript quoted above is a real run
from this session. Confirmed live: all six cases produced
<code>digest=0x481984990deee5ff</code>, byte-identical to Chapters 46 through
52; <code>frames</code> reported <code>trivial=32 small=304 large=4128</code>;
<code>versus-thread</code> reported <code>coroutine_frame=32 bytes
thread_stack=8388608 bytes</code> giving <code>ratio=262144x</code>, where the
8388608 is Chapter 50's own <code>pthread_getattr_np</code> measurement carried
forward as a named constant and the ratio is computed in the program rather than
written into the prose; <code>generator</code> reported
<code>twentieth_fib=4181 correct=yes</code> with
<code>__cpp_lib_generator=202207</code>; and <code>lifetime</code> reported
<code>abandoned alloc=1 free=0</code> against <code>owned alloc=1 free=1</code>.
The heap-elision matrix is a real sweep taken this session from the shipped
source: <code>g++ -std=c++23</code> at <code>-O0</code>, <code>-O1</code>,
<code>-O2</code> and <code>-O3</code> each reported <code>allocations=1000
elided=0</code>, while <code>clang++ -std=c++23</code> reported
<code>allocations=1000</code> at <code>-O0</code> and <code>allocations=0
elided=1000</code> at both <code>-O1</code> and <code>-O2</code>. Not exercised:
<span class="status status--unverified">unverified</span> — whether heap
allocation elision occurs is deliberately <em>not</em> gated anywhere in this
example. It is permitted by <code>[dcl.fct.def.coroutine]</code> and required of
no implementation, so a gate either way would encode one compiler's current
behaviour as a rule; the harness asserts only that the count is measured and
internally consistent, and that clang elides at least as many frames as GCC.
Frame sizes are ABI- and compiler-dependent and are printed rather than
asserted -- the gates check the relationships between them (trivial under 128
bytes, the <code>char[4096]</code> case over 4096, and more than an order of
magnitude between the two).</p>
