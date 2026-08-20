---
title: "Boost.Fiber: stackful suspension, the guard page, and the scheduler you have to choose"
order: 54
part: "Compendium: C++ Concurrency"
description: "fiberdemo measures what a real stack costs and what it buys. boost::context::stack_traits reports a 131072-byte default fiber stack, sitting between ch53's measured 32-byte coroutine frame and ch50's measured 8388608-byte thread stack -- 8 MiB to 128 KiB to 32 bytes, with both ratios computed from measured numbers. The capability that stack buys is demonstrated rather than described: a fiber suspends from inside an ordinary non-coroutine function six frames deep and resumes there, which no stackless coroutine can do. protected_fixedsize_stack is shown costing exactly one page more than fixedsize_stack, gated against page_size(), and a 60x overflow dies of SIGSEGV on that guard page rather than corrupting the next mapping. The same 16 fibers report one distinct tid under round_robin and four under shared_work, making ch49's grid a scheduler choice. Verified against system Boost 1.90.0 on the Fedora 44 host: verify.lua PASS 32/FAIL 0, no Conan, no network."
duration: "55 minutes"
---

Chapter 53 ended with a number: a C++20 coroutine frame is 32 bytes. It holds a
promise, two function pointers, a state index, and whatever locals cross a
suspension — and nothing else, because a coroutine has no stack.

That is the whole design, and it has a consequence Chapter 53 mentioned only in
passing. `co_await` is valid only in the body of a coroutine. If a function
five frames down wants to suspend, every frame above it has to be a coroutine
too — the problem usually called *function colouring*. A stackless coroutine
cannot suspend from inside an ordinary call tree, at any price.

A fiber can, and this chapter is about what that costs.

{% include excalidraw.html
   file="54-three-ways-to-pause"
   alt="Three bands pricing one paused computation. The top kernel-scheduled band holds a thread from ch50 at 8388608 bytes of stack, annotated as preempted anywhere with a tid in /proc and a kernel task_struct, suspending wherever the scheduler decides with no cooperation needed. The middle amber band, user-scheduled and stackful with a real machine stack switched by boost::context, holds a fiber from ch54 at 131072 bytes or 128 KiB with 64 fitting in one thread stack, annotated in amber that it suspends from anywhere in an ordinary call tree as measured at 6 frames deep, with no co_await, no coroutine and no function colouring, and that the stack is exactly what buys that. The bottom user-scheduled stackless band holds a coroutine frame from ch53 at 32 bytes with 4096 fitting in one fiber stack, annotated that it suspends only at co_await and only in a coroutine body, so every frame in the suspend path must itself be a coroutine. An amber arrow labelled divide by 64 runs from thread to fiber and an arrow labelled divide by 4096 from fiber to frame, closing on an amber box reading 8 MiB to 128 KiB to 32 bytes, each number measured on this host."
   caption="Figure 54.1 — the three ways to hold a paused computation, all three measured: cost and capability move together, and in opposite directions" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated, parity gate G), `timeout` from coreutils (host; bounds the deliberate
> fault), `lua` (host; gated, runs `verify.lua`), and `python3` (host; gated,
> runs `scripts/test-all-examples.py`). **System Boost 1.90.0** from Fedora's
> `boost-devel`, linking `boost_fiber` and `boost_context` — **no Conan, no
> network**. No VM, no root: `examples/54-boost-fiber` is `mode: local`.

## The number, and what sits either side of it

```
[host]$ ./demo.sh cpp run stacks
stacks: default=131072 minimum=14528 page=4096 unbounded=yes
```

`boost::context::stack_traits::default_size()` reports **131072** — 128 KiB per
fiber. That is the middle term of a comparison Part 14 has been assembling for
five chapters:

```
[host]$ ./demo.sh cpp run versus
versus: thread=8388608 fiber=131072 coroutine_frame=32
versus: fibers_per_thread_stack=64 frames_per_fiber_stack=4096
versus: fiber_between_the_two=yes
```

Neither of the outer numbers is quoted. Chapter 50 read 8388608 out of
`pthread_getattr_np`; Chapter 53 read 32 out of an `operator new` overloaded
inside a `promise_type`. Both are named constants in this program so the
comparison cites real prior measurements:

```cpp
constexpr std::size_t kCh50ThreadStackBytes = 8388608;
constexpr std::size_t kCh53CoroutineFrameBytes = 32;
```

And the ratios are computed rather than written into the prose:

```cpp
    const std::size_t vs_thread = fiber_stack > 0 ? kCh50ThreadStackBytes / fiber_stack : 0;
    const std::size_t vs_frame =
        kCh53CoroutineFrameBytes > 0 ? fiber_stack / kCh53CoroutineFrameBytes : 0;
```

Sixty-four fibers fit in the address space one thread reserves. Four thousand
and ninety-six coroutine frames fit in one fiber's stack.

The ordering is not arbitrary, and the reason is worth stating as a rule: **a
fiber costs more than a frame because it has a stack, and less than a thread
because the kernel never sees it.** No `task_struct`, no tid, no entry in
`/proc/<pid>/task/` — Chapter 50's entire catalogue of per-thread kernel state
is absent. Switching between fibers is a register save, a stack pointer swap,
and a jump, entirely in user space.

## What the stack actually buys

The cost is the less interesting half. Here is the capability:

```cpp
void leaf() {
    g_yield_depth = g_depth;
    boost::this_fiber::yield();  // suspend from an ORDINARY function
    g_resume_depth = g_depth;
}

void level(int remaining) {
    ++g_depth;
    if (remaining == 0) {
        leaf();
    } else {
        level(remaining - 1);
    }
}
```

```
[host]$ ./demo.sh cpp run deep
deep: yielded_at_depth=6 resumed_at_depth=6 same=yes
deep: suspended from a plain function 6 frames below the fiber body -- no co_await, no coroutine, no colouring
```

`leaf` is an ordinary function. So is `level`. Neither returns a special type,
neither contains `co_await`, and nothing in the chain between them and the fiber
body knows a fiber is involved. The suspension happens six frames down and the
resumption happens in the same place, because `boost::context` saved and
restored the whole stack.

This is what a stackless coroutine cannot do. Not "does less efficiently" —
cannot express. To suspend from `leaf` in the C++20 model, `leaf` must be a
coroutine, so `level` must `co_await` it, so `level` must be a coroutine, and
so on to the top. The colour propagates all the way up the call chain, and it
propagates through any third-party function in between that you cannot edit.

That is the trade in one sentence: **128 KiB buys the right to suspend
anywhere; 32 bytes restricts you to suspending in functions you have marked in
advance.**

## The guard page

Giving something a stack raises the question of what happens when it runs off
the end. Boost ships three stack allocators, and the difference between two of
them is exactly one page:

```
[host]$ ./demo.sh cpp run stacks
stacks: requested=65536 fixedsize=65536 protected=69632 guard_delta=4096
stacks: exact_request=yes guard_is_one_page=yes
```

`fixedsize_stack` allocates exactly what you asked for.
`protected_fixedsize_stack` allocates one page more and maps it `PROT_NONE`, so
the first access past the end of the stack faults instead of quietly writing
into whatever the allocator put next.

Both directions are measured. A fiber that stays inside its 64 KiB:

```
[host]$ ./demo.sh cpp run guard-ok
guard-ok: 5 frames on a 65536-byte protected stack completed normally
```

And the same fiber recursing sixty times past it:

```
[host]$ ./demo.sh cpp run guard-overflow; echo "exit=$?"
guard-overflow: recursing 1000 frames on a 65536-byte protected stack; this is expected to die on the guard page
exit=139
```

139 is 128 + 11 — SIGSEGV, delivered on the guard page.

This is Chapter 49's `oob` demonstration one layer up, and it inherits that
chapter's discipline about what may be asserted. The gate checks the **exit
signal** and nothing else:

```lua
checks.expect_match(tostring(code ~= nil and code >= 128 and code ~= 124), "true",
  "cpp: recursing 60x past that stack dies of a signal (exit=" .. tostring(code) ..
  "; 139 = SIGSEGV on the guard page) rather than corrupting the next mapping")
```

Whatever a process prints after running off its stack is undefined, so nothing
asserts on the output. The `code ~= 124` is doing real work too: 124 is what
`timeout(1)` returns when it has to kill something, and a hung run must not be
allowed to pass a test for faulting promptly.

### Why this file is compiled `-O0`

```cmake
target_compile_options(fiberdemo PRIVATE -O0)
```

The overflow depends on a recursion that genuinely consumes stack. At `-O2`
both compilers may inline `burn_stack` into a loop, drop the `volatile` array's
storage, or turn the self-call into a jump — and the fiber then never
approaches its guard page. `guard-overflow` would exit 0 and quietly stop
demonstrating anything.

Chapter 49 learned this at some expense, when a burn loop whose result nothing
consumed was deleted outright and the example spent a session measuring the
optimizer instead of the scheduler. The `volatile` array and the write to it
are the same defence one level down; `-O0` on this translation unit is the
belt to that suspenders.

## The scheduler is a choice you have to make

{% include excalidraw.html
   file="54-scheduler-is-a-choice"
   alt="The same 16 fibers under two schedulers, one line apart. The left column, the default boost::fibers::algo::round_robin, shows 16 fibers on one per-thread queue where a fiber never leaves the thread that created it, reporting distinct_tids equals 1, annotated with ch49's vocabulary as 16 tasks in flight on one CPU occupied, concurrent but not parallel, and noted as a legitimate choice because no migration means no cross-thread synchronisation to reason about. The right amber column, algo::shared_work, shows the same 16 fibers on one shared queue that 4 threads pull from so a fiber can resume elsewhere, reporting distinct_tids equals 4, annotated as concurrent and parallel, the bottom-right cell of ch49's grid reached by choosing it, at the price that your fiber's data is now touched by several threads and you own that problem. An amber arrow crosses between the columns, and an amber box at the bottom holds the single line use_scheduling_algorithm of algo::shared_work, noting it is yours to write rather than a runtime's to decide, contrasting ch44's Go."
   caption="Figure 54.2 — one call to use_scheduling_algorithm moves the same 16 fibers from one OS thread to four; in C++ that decision is yours rather than a runtime's" %}

Chapter 44 covered Go's answer to M:N scheduling: the GMP runtime owns the
decision, does it automatically, and mostly will not be argued with. Boost.Fiber
takes the opposite position. You pick the scheduler, per thread, and the choice
is visible from outside the process.

The same 16 fibers doing the same work, under the default:

```
[host]$ ./demo.sh cpp run roundrobin
roundrobin: fibers=16 distinct_tids=1
```

One OS thread. `round_robin` keeps a queue per thread and never migrates a
fiber, so sixteen tasks are in flight and exactly one CPU is occupied. Chapter
49 named that cell precisely: **concurrent, not parallel**.

Then one line changes:

```cpp
            // Every participating thread must opt in. shared_work keeps ONE
            // queue that all of them pull from, so a fiber can resume on a
            // different thread than the one it suspended on.
            boost::fibers::use_scheduling_algorithm<boost::fibers::algo::shared_work>();
```

```
[host]$ ./demo.sh cpp run sharedwork
sharedwork: fibers=16 worker_threads=4 distinct_tids=4
```

The same fibers, now on four threads. The tids come from `gettid()` — the same
kernel task id Chapter 50 established, unioned into a set the way Chapter 49
unioned CPU ids.

Neither answer is the right one. `round_robin` is a real choice with a real
benefit: a fiber that never migrates is a fiber whose data is only ever touched
by one thread, which removes an entire category of synchronization from your
program. `shared_work` buys parallelism and hands you back that category as a
problem. What Boost.Fiber does — and what Go deliberately does not — is make
you decide.

The harness gates the sign rather than the magnitude:

```lua
checks.expect_match(tostring(sw_tids ~= nil and sw_tids > 1), "true",
  "cpp: the SAME 16 fibers under shared_work migrated across " .. tostring(sw_tids) ..
  " OS threads -- one call to use_scheduling_algorithm is the only difference")
```

`> 1`, never `== 4`. A loaded host could legitimately keep the work on fewer
threads, and there is a bounded retry behind that assertion for the same reason
Chapter 49's parallel gate had one. It reported 4 on all five consecutive runs
here, so the retry is insurance rather than a crutch.

## How the code works

Each measurement is its own subcommand, and by now the reason is familiar:
`guard-overflow` dies of SIGSEGV, and a subcommand that dies must never be able
to take the others with it. Chapter 50's `cancel-swallow` and Chapter 51's
`deadlock-naive` are the same pattern.

The tid collection is shared between the two scheduler cases so that the
comparison is genuinely between schedulers:

```cpp
void fiber_body() {
    for (int i = 0; i < kYieldsPerFiber; ++i) {
        note_tid();
        boost::this_fiber::yield();
    }
}
```

Both cases run *this* function, sixteen times, fifty yields each. Nothing else
differs. If the fiber bodies were written separately, a difference in the work
could explain a difference in the tid count, and the measurement would prove
nothing about scheduling.

The `yield()` inside is what makes migration possible at all. A fiber that
never suspends never returns to the scheduler, so `shared_work` would have no
opportunity to move it — the same cooperative-scheduling property Chapter 52's
`interrupt-busy` case demonstrated from the other direction.

## Errors, three ways

**An error the hardware catches for you.** The stack overflow is the clean case:
a page mapped `PROT_NONE`, an access, a SIGSEGV, and a process that stops
immediately at the point of the fault. Compare it with the alternative — one
page of unprotected allocation past the end, silently written through, and a
corruption that surfaces somewhere unrelated much later.

**An error the hardware cannot catch.** The guard page is exactly one page. A
single frame larger than 4096 bytes can step *over* it into whatever is mapped
next, and neither Boost nor the MMU will notice. That is not a flaw to be fixed
so much as a limit to know: the guard page catches runaway recursion, which is
the common case, and does not catch a single enormous stack frame.

**An error of configuration.** Choosing `round_robin` when you wanted
parallelism produces a program that is completely correct, passes every test,
and uses one core. Nothing fails. The only way to notice is to count the threads
your fibers actually ran on — which is why this example counts them.

## Concurrency lens

Fibers are cooperatively scheduled, and every consequence in this example
follows from that word.

Under `round_robin` a fiber runs until it yields. There is no preemption, no
time slice, and no `SIGURG` arriving from a sysmon thread the way Chapter 44
described in Go. A fiber that computes without yielding starves every other
fiber on that thread — the same property Chapter 52 measured when a busy loop
ignored `boost::thread::interrupt()`.

That has a pleasant corollary. Between two yields, a fiber under `round_robin`
has exclusive access to everything its thread owns. No other fiber can be
running, so shared state touched only between suspension points needs no mutex
at all. This is the same reason a single-threaded event loop is easy to reason
about, and it is the biggest practical argument for the default scheduler.

`shared_work` gives that up entirely. Once several threads pull from one queue,
a fiber can resume on a thread different from the one it suspended on, and any
data it touches is now genuinely shared. Note what this does to Chapter 51's
cost model: a `std::mutex` inside a fiber under `round_robin` is uncontended by
construction and costs nothing, while the same mutex under `shared_work` can
contend and reach the futex.

There is also a subtler hazard. Thread-local storage is *thread*-local, not
fiber-local, so a fiber that reads a `thread_local` before yielding and again
after may be reading two different variables under `shared_work`. Boost provides
`fiber_specific_ptr` (in `<boost/fiber/fss.hpp>`) for that reason, and reaching
for `thread_local` inside a migratable fiber is a bug that will not reproduce
under `round_robin`.

## Build, run, observe

```
[host]$ cd examples/54-boost-fiber
[host]$ ./demo.sh cpp build
[host]$ for c in versions stacks versus deep roundrobin sharedwork guard-ok; do
          ./demo.sh cpp run "$c"; done
[host]$ ./demo.sh cpp run guard-overflow; echo "exit=$?"    # 139, by design
```

Then the harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: protected_fixedsize_stack costs exactly one page more (delta=4096 page_size=4096) -- that page is the guard
ok: cpp: a fiber stack (131072) sits strictly between them -- it has a stack, so it costs more than a frame; the kernel never sees it, so it costs less than a thread
ok: cpp: fibers_per_thread_stack=64 is arithmetic on two measured numbers, not a quoted figure
ok: cpp: and it suspended from 6 frames deep inside ORDINARY functions -- no co_await, no coroutine, no function colouring
ok: cpp: 16 fibers under the default round_robin scheduler ran on exactly ONE OS thread -- concurrency with no parallelism, in ch49's terms
ok: cpp: the SAME 16 fibers under shared_work migrated across 4 OS threads -- one call to use_scheduling_algorithm is the only difference
ok: cpp: recursing 60x past that stack dies of a signal (exit=139; 139 = SIGSEGV on the guard page) rather than corrupting the next mapping
PASS 32 / FAIL 0
```

## Cross-check: three chapters, one table

Every case reported `digest=0x481984990deee5ff`, and clang agrees — spanning
Chapters 46 through 54 now.

The three-way memory table is the sharper cross-check, and it only works because
each number came from a different instrument in a different chapter:
`pthread_getattr_np` in Chapter 50, an overloaded `operator new` in Chapter 53,
and `stack_traits::default_size()` here. Three measurements, three techniques,
one ordering — and the ordering falls out of a single structural fact about each
model rather than out of benchmark noise.

That fact is worth restating as the compendium's through-line so far. A thread
is scheduled by the kernel, so it costs kernel state. A fiber is scheduled in
user space but keeps a stack, so it costs a stack. A coroutine keeps no stack,
so it costs only what crosses its suspensions — and pays for that in where it is
allowed to suspend.

## What you learned

- **A fiber is stackful.** 131072 bytes by default on this host, sitting between
  Chapter 53's 32-byte coroutine frame and Chapter 50's 8388608-byte thread
  stack — 64 fibers per thread stack, 4096 frames per fiber stack.
- **Cost and capability move together.** The more a paused computation costs,
  the fewer restrictions there are on where it may pause.
- **The stack buys suspension from anywhere.** A fiber yields from an ordinary
  function six frames deep and resumes there. A stackless coroutine cannot
  express this — `co_await` colours every frame in the suspend path.
- **A fiber is invisible to the kernel.** No `task_struct`, no tid, nothing in
  `/proc/<pid>/task/`. Switching is a register save and a jump in user space.
- **`protected_fixedsize_stack` costs exactly one page more**, mapped
  `PROT_NONE`, and turns a stack overflow into an immediate SIGSEGV instead of
  silent corruption — though a single frame larger than a page can still step
  over it.
- **Gate the signal, not the output.** What a process prints after running off
  its stack is undefined; assert exit ≥ 128, and make sure a `124` timeout
  cannot pass as a fault.
- **Compile the overflow case at `-O0`.** At `-O2` the recursion can become a
  loop and never touch the guard page — Chapter 49's lesson about measuring the
  optimizer instead of the subject.
- **In C++ the scheduler is yours to choose.** The same 16 fibers report one tid
  under `round_robin` and four under `shared_work`, which is Chapter 49's grid
  selected by one line rather than decided by a runtime as in Chapter 44's Go.
- **`round_robin` is a real choice, not a fallback.** A fiber that never
  migrates is a fiber whose data one thread touches — which removes a whole
  category of synchronization, and is why `thread_local` is a latent bug only
  under `shared_work`.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, glibc 2.43, <strong>Boost
1.90.0 from <code>boost-devel-1.90.0-7.fc44</code></strong>, g++ 16.1.1
20260515, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, Lua 5.4.8, 16 logical / 8
physical CPUs; local, unprivileged, <strong>no Conan and no network</strong>, no
VM): <code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. &amp;&amp; pwd) lua verify.lua</code>
reported <code>PASS 32 / FAIL 0</code> with gates E (scheduler migration) and F
(the guard-page fault) and G (clang parity) all running for real, and
<code>python3 scripts/test-all-examples.py --only 54-boost-fiber</code> reported
<code>1 passed, 0 failed, 0 skipped</code>. Every transcript quoted above is a
real run from this session. Confirmed live: all seven non-faulting cases
produced <code>digest=0x481984990deee5ff</code>, byte-identical to Chapters 46
through 53; <code>stacks</code> reported
<code>default=131072 minimum=14528 page=4096</code> with
<code>requested=65536 fixedsize=65536 protected=69632 guard_delta=4096</code>,
the delta gated against <code>stack_traits::page_size()</code> rather than a
hardcoded constant; <code>versus</code> reported
<code>thread=8388608 fiber=131072 coroutine_frame=32</code> with
<code>fibers_per_thread_stack=64</code> and
<code>frames_per_fiber_stack=4096</code>, where the outer two numbers are
Chapter 50's and Chapter 53's own measurements carried forward as named
constants and both ratios are computed in the program; <code>deep</code>
reported <code>yielded_at_depth=6 resumed_at_depth=6 same=yes</code>;
<code>roundrobin</code> reported <code>distinct_tids=1</code> and
<code>sharedwork</code> reported <code>distinct_tids=4</code> for the identical
16 fibers, stable at 4 across five consecutive runs; <code>guard-ok</code>
exited <code>0</code> and <code>guard-overflow</code> exited <code>139</code>
(SIGSEGV). Not exercised: <span class="status
status--unverified">unverified</span> — context-switch timings are deliberately
not measured or gated anywhere in this example, because this book does not gate
on durations (Chapter 39); the fiber-versus-thread and fiber-versus-coroutine
claims here are about memory and about where a suspension may occur, both of
which are counts. <code>pooled_fixedsize_stack</code>, Boost.Fiber's
work-stealing scheduler (<code>algo::work_stealing</code>), and
<code>fiber_specific_ptr</code> are named in the chapter but not exercised --
all three headers were confirmed present on this host
(<code>boost/context/pooled_fixedsize_stack.hpp</code>,
<code>boost/fiber/algo/work_stealing.hpp</code>,
<code>boost/fiber/fss.hpp</code>) but none is built or gated;
the <code>thread_local</code>-under-migration hazard described in the
concurrency lens is reasoned from the scheduling model rather than reproduced.</p>
