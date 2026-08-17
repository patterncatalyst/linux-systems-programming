---
title: "pthreads: the thread under std::thread, its two identities, and the six controls the standard does not give you"
order: 50
part: "Compendium: C++ Concurrency"
description: "pthreads runs one facet per subcommand over the per-thread controls POSIX offers and the C++ standard library does not expose: gettid() versus the opaque pthread_t (and why the main thread's tid equals the pid), pthread_setname_np landing in /proc/self/task/<tid>/comm with a 16-character name refused as ERANGE, pthread_attr_setstacksize giving exactly 262144 against an 8388608 default with sub-PTHREAD_STACK_MIN requests refused as EINVAL, per-thread pthread_setaffinity_np putting two workers on two CPUs while the caller's mask is untouched, SCHED_OTHER at priority 0 with an unprivileged SCHED_FIFO refused as EPERM, and pthread_cancel unwinding C++ destructors -- plus the trap where a catch (...) that does not rethrow aborts the process with FATAL: exception not rethrown. Verified on the Fedora 44 host with glibc 2.43: verify.lua PASS 45/FAIL 0, local, unprivileged, offline, with std::thread::native_handle_type static_asserted to be pthread_t."
duration: "55 minutes"
---

Chapter 49 measured threads from above. `std::thread` spawned them,
`sched_getcpu` reported where they landed, and the whole argument — that
concurrency is structure and parallelism is execution — went through without
ever asking what a thread *is* on this operating system.

This chapter asks. On Linux, a `std::thread` is a pthread; on this toolchain
that is not an analogy but a type identity you can assert at compile time. And
a pthread can do a great deal that `std::thread` cannot express: it has a name
the kernel will show you, a stack whose size you choose, an affinity mask of
its own, a scheduling policy, and a cancellation mechanism the C++ committee
looked at and declined to standardize. Every one of those is reachable from
standard C++ through exactly one door, `native_handle()`, and every one of them
has an effect you can go and observe in `/proc`.

{% include excalidraw.html
   file="50-thread-two-identities"
   alt="One thread drawn twice, in two bands. The upper band, the C++ view described as portable and deliberately narrow, holds a std::thread box listing an opaque std::thread::id, join/detach/joinable, hardware_concurrency, and no name, no stack, no affinity; an amber native_handle box calling it the sanctioned escape hatch and noting native_handle_type IS pthread_t here by static_assert at compile time; and a std::jthread plus stop_token box describing cooperative cancellation as a flag the target checks with no unwinding and no exception, the answer to pthread_cancel. The lower band, the Linux view of the same thread as the kernel and glibc see it, holds a pthread_t box calling it an opaque glibc handle to be compared with pthread_equal and explicitly not a number and not a tid; an amber gettid box calling it the kernel task id, a real integer that names a /proc/self/task/<tid>/ directory, noting that for the main thread tid equals pid; and three summary boxes for naming via pthread_setname_np into procfs comm with 15 characters maximum and 16 refused as ERANGE, stack via pthread_attr_setstacksize giving 262144 with anything below PTHREAD_STACK_MIN of 16384 refused as EINVAL, and affinity applying to one thread with scheduling reporting SCHED_OTHER priority 0 and SCHED_FIFO refused as EPERM. An amber box states every facet still reports digest 0x481984990deee5ff. An arrow runs from std::thread to native_handle to jthread across the top, and an amber arrow drops from native_handle down into the gettid box."
   caption="Figure 50.1 — one thread, two identities: what the C++ standard library exposes, what Linux actually offers, and the single door between them" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated, used for the parity gate I), `lua` (host; gated as `lua >= 5.4`, runs
> `verify.lua`), and `python3` (host; gated, runs
> `scripts/test-all-examples.py`). `ps` from procps-ng is used in one optional
> observation step below and is part of the Fedora base install rather than a
> gate. No VM, no root, no LGTM stack, no network: `examples/50-pthreads` is
> `mode: local` in `examples/manifest.yaml` and builds from the standard
> library and pthreads alone.

Like Chapter 49, this example ships `langs: [cpp]`. Go and Rust both sit on
these same pthreads and both deliberately hide exactly these controls — that is
the *point* of their threading models — so a three-language version would have
two directories with nothing to say. Every code block below is a plain fenced
`cpp`, `lua`, or `console` block.

## Two identities, and neither is the other

Start with the thing that trips people up in production logs.

A Linux thread has two names. `pthread_t` is an opaque handle that glibc hands
you. The standard says you may compare it with `pthread_equal` and nothing
else — it is not a number, it is not guaranteed to be printable, and on some
platforms it is a pointer. `gettid()` is different in kind: it returns the
kernel's task id, a genuine integer, and it is the id that names a directory in
`/proc`, that `perf` records, that `ftrace` emits, that gdb's `info threads`
shows in its LWP column, and that `top -H` sorts by.

If you log `pthread_self()` and then try to find that thread in `perf`, you
will not find it. The two identities do not convert.

```
[host]$ ./demo.sh cpp run identity
identity: main pid=2675825 tid=2675825 same=yes
identity: worker 0 tid=2675826
identity: worker 1 tid=2675827
identity: worker 2 tid=2675828
identity: worker_tids_distinct=yes main_task_dir=present
pthreads report: facet=identity digest=0x481984990deee5ff
```

`main pid=2675825 tid=2675825 same=yes` is the line worth stopping on. The main
thread's task id *equals* the process id, and that is not a coincidence or an
implementation detail — on Linux a process **is** its first thread. Everything
else the kernel calls a thread is a task in the same thread group, sharing that
leader's pid as its tgid. Chapter 11 built the process side of this picture;
this is the same structure viewed from the other end.

The consequence is a small family of bugs: code that calls `getpid()` inside a
worker thread expecting "which thread am I" gets the *process* id and is
therefore identical in every thread. `gettid()` is the answer, and since glibc
2.30 it is a plain libc function rather than a `syscall(SYS_gettid)` incantation.

Each of those tids also names a real directory:

```cpp
bool task_dir_exists(pid_t tid) {
    return access(("/proc/self/task/" + std::to_string(tid)).c_str(), F_OK) == 0;
}
```

`/proc/self/task/` is the per-thread mirror of `/proc/self/`. Chapter 36 read
`/proc` by hand for process-wide facts; this subtree is where the per-thread
ones live, and the next facet writes to it.

## Naming: the kernel will remember what you call a thread

`std::thread` has no name. Not a limited one, not an awkward one — none at all.
That is a real operational cost: a core dump, a `top -H`, or a gdb backtrace
from a program with twenty `std::thread`s shows twenty identical rows.

`pthread_setname_np` fixes it, and the fix is not glibc bookkeeping — the name
lands in the kernel's `task_struct` and becomes readable from procfs:

```cpp
std::string comm_of(pid_t tid) {
    std::ifstream f("/proc/self/task/" + std::to_string(tid) + "/comm");
    std::string s;
    std::getline(f, s);
    return s;
}
```

The example sets a name and then reads it back through *both* available paths —
glibc's own getter and the kernel's file — to show they are the same string:

```
[host]$ ./demo.sh cpp run naming
naming: tid=2675834 via_pthread='ch50-worker' via_proc='ch50-worker' agree=yes
naming: 16char_rc=34 (ERANGE) name_after='ch50-worker' unchanged=yes
pthreads report: facet=naming digest=0x481984990deee5ff
```

The second line is the part worth knowing before you need it. The kernel's
`comm` field is **16 bytes including the terminating NUL**, so fifteen
characters is the most that fits. glibc does not truncate an over-long name; it
refuses the whole call with `ERANGE` and leaves the previous name in place:

```cpp
        // 16 characters: one too many for a 16-byte field that must hold a NUL.
        too_long_rc = pthread_setname_np(pthread_self(), "0123456789abcdef");
        after_too_long = comm_of(tid);
```

If you do not check the return value — and almost nobody checks the return
value of a naming call — an over-long name is a silent no-op. Your thread keeps
whatever it was called before, which is the process name, which is the
uninformative default you were trying to escape. The gate asserts both halves:
that the call returned `ERANGE`, and that the old name survived.

Which layer refuses matters here. glibc is what returns `ERANGE`; the kernel's
`prctl(PR_SET_NAME)` interface silently truncates instead. Take a different
route to the same field and you get a different failure mode for the same
mistake.

## Stack size: the control `std::thread` never offered

`std::thread` gives you no say in stack size. You get the default, and on glibc
the default is not one number but two:

```
[host]$ ./demo.sh cpp run stack
stack: main=8376320 default_worker=8388608 custom=262144 requested=262144 set_rc=0
stack: custom_smaller_than_default=yes custom_at_least_requested=yes
stack: PTHREAD_STACK_MIN=16384 below_min_rc=22 (EINVAL)
pthreads report: facet=stack digest=0x481984990deee5ff
```

The main thread reports 8376320 bytes and a created thread reports 8388608.
Those differ because they come from different places: the main thread's stack
is grown on demand by the kernel up to `RLIMIT_STACK`, so what
`pthread_getattr_np` reports is derived from that rlimit and is not a round
number. A created thread's stack is a plain `mmap` of exactly 8 MiB. Two
thread-shaped things in one process with two different stack stories.

Why care? Because 8 MiB is *virtual* address space per thread, and a program
that spawns a few thousand threads is asking for tens of gigabytes of mappings.
It usually works — the pages are untouched and therefore never backed — but it
interacts badly with `vm.overcommit_memory`, with cgroup memory accounting, and
with 32-bit address spaces. Anyone who has built a thread-per-connection server
has met this.

```cpp
    std::size_t custom_size = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    const int set_rc = pthread_attr_setstacksize(&attr, kRequested);
    pthread_t c{};
    pthread_create(&c, &attr, stack_probe, &custom_size);
    pthread_join(c, nullptr);
    pthread_attr_destroy(&attr);
```

256 KiB requested, exactly 262144 observed. The gate asserts `>=` rather than
`==` deliberately: a page-rounded value is permitted by the standard and would
be just as correct, so asserting strict equality would be gating on an
implementation detail this chapter does not actually care about.

Going too small is refused rather than rounded up:

```cpp
    pthread_attr_t small;
    pthread_attr_init(&small);
    const int too_small_rc = pthread_attr_setstacksize(&small, 8192);
    pthread_attr_destroy(&small);
```

`EINVAL`, because 8192 is below this platform's `PTHREAD_STACK_MIN` of 16384.
That floor is not arbitrary — it has to hold the thread descriptor, TLS, and
enough room for a signal frame. Note that `PTHREAD_STACK_MIN` is not a compile
time constant on glibc; it expands to a `sysconf` call, which is why it is
printed at run time here rather than baked in.

## Affinity: per thread, not per process

Chapter 49 pinned with `sched_setaffinity(0, ...)`, and that zero means *this
whole process*. It was the right call there — the goal was to remove
parallelism from the entire program to isolate the concurrency axis. But it is
a blunt instrument, and it is not what you want when a low-latency fast path
should own a core while background work stays out of its way.

`pthread_setaffinity_np` moves one thread:

```cpp
void* pin_probe(void* raw) {
    auto* a = static_cast<PinArg*>(raw);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(a->cpu), &set);
    a->rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    // sched_yield gives the kernel an obvious moment to honour the new mask;
    // it is not required for correctness, only for a prompt observation.
    sched_yield();
    a->observed = sched_getcpu();
    return nullptr;
}
```

```
[host]$ ./demo.sh cpp run affinity
affinity: worker_a asked=0 rc=0 observed_cpu=0
affinity: worker_b asked=1 rc=0 observed_cpu=1
affinity: workers_on_different_cpus=yes main_cpus_allowed=16 unchanged=yes
pthreads report: facet=affinity digest=0x481984990deee5ff
```

Two threads, two CPUs, and `main_cpus_allowed=16 unchanged=yes` — the calling
thread's mask was never touched. Run Chapter 49's `concurrent` model and the
process-wide call would have left `cpus_allowed=1` for everything.

This is the mechanism behind Chapter 40's pinned fast path, and it is worth
knowing that the C++ standard has no spelling for it at all. There is no
`std::thread::set_affinity`, no executor property in C++23 that reaches it.
`native_handle()` is the whole story.

The gate skips rather than fails on a host allowed fewer than two CPUs, because
two threads cannot occupy two CPUs that do not exist:

```lua
if aff:find("need at least 2 to pin two threads apart") then
  print("SKIP: fewer than 2 CPUs allowed -- gate E not asserted")
else
```

An informational `SKIP` beside the PASS count, never a silent pass — the same
discipline Chapter 48 used for absent tools.

## Scheduling: the facet whose interesting call fails

Scheduling policy is per-thread too, and `std::thread` cannot touch it either.

```
[host]$ ./demo.sh cpp run sched
sched: get_rc=0 policy=0 is_other=yes priority=0
sched: SCHED_FIFO prio 50 -> rc=1 (EPERM) policy_after=0 unchanged=yes
pthreads report: facet=sched digest=0x481984990deee5ff
```

The default is `SCHED_OTHER` at priority 0. That zero is required, not
incidental: under the fair scheduler the `sched_param` priority *must* be zero,
and the knob that actually influences `SCHED_OTHER` scheduling is nice, which
lives behind a different call entirely. A `sched_param` with a nonzero priority
and `SCHED_OTHER` is rejected. Two things both called priority, only one of them
in this struct.

Then the interesting call, which fails:

```cpp
    sched_param rt{};
    rt.sched_priority = 50;
    const int fifo_rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt);
```

`EPERM`. Real-time policy needs `CAP_SYS_NICE` or an `RLIMIT_RTPRIO` allowance,
and an unprivileged process has neither.

The refusal is the observable, and that is a deliberate design decision for this
example rather than a limitation of it. A demo that needed root to show anything
would not run on a reader's laptop, would not run in the runner, and would have
to be gated behind the VM lab. What an unprivileged process *can* prove is that
the policy is what it says, that the request was refused with the specific errno
the manual documents, and that the refusal was clean — `unchanged=yes`, the
thread is still on `SCHED_OTHER`. The gate asserts exactly that, and `verify.lua`
prints a line saying which stronger claim it is not making:

```
info: a SUCCESSFUL SCHED_FIFO change is not gated -- it needs CAP_SYS_NICE or an RLIMIT_RTPRIO allowance; the EPERM refusal above is what an unprivileged run can prove
```

## Cancellation, and why C++ said no

Here is the facility C++ looked at and declined.

{% include excalidraw.html
   file="50-cancellation-two-roads"
   alt="Two bands comparing cancellation mechanisms. The amber upper band, titled pthread_cancel as asynchronous and it unwinds, runs left to right: a worker parked box noting cancellation points like sleep and read; an amber pthread_cancel box saying glibc throws a forced unwind through the target's stack; a RAII really runs box listing that the Guard destructor ran, the cleanup handler ran, and join returned PTHREAD_CANCELED; and a side box observing this is better than its reputation because destructors are not skipped. Below them a dashed ghost box reads: but catch parenthesis dot dot dot without a rethrow swallows it, FATAL colon exception not rethrown leading to SIGABRT exit 134, and ordinary defensive C++ turns a cancel into a dead process. The lower band, titled std::jthread plus stop_token as cooperative and C++20's answer, runs left to right: request_stop sets a flag with no exception and no unwind; stop_token::stop_requested is checked by the target when it chooses to; and an amber box concluding the target decides where it is safe to stop, with no cancellation points, no forced unwind, and nothing a catch-all can accidentally swallow."
   caption="Figure 50.2 — two roads out of a running thread: pthread_cancel does unwind and does run destructors, but the mechanism is an exception a catch-all can swallow fatally; stop_token is the cooperative alternative C++20 chose" %}

`pthread_cancel` has a reputation for being unsafe in a way that turns out to be
half wrong, and the half that is wrong matters:

```
[host]$ ./demo.sh cpp run cancel
cancel: worker parked on a cancellation point
cancel: ~Guard(raii) ran during unwinding
cancel: pthread_cleanup handler ran (holding a-resource)
cancel: pthread_cancel rc=0 joined_retval_is_canceled=yes
pthreads report: facet=cancel digest=0x481984990deee5ff
```

Cancellation on glibc **unwinds the stack**. The C++ destructor ran. The
`pthread_cleanup_push` handler ran. `pthread_join` reported `PTHREAD_CANCELED`
rather than a normal return value. RAII is not silently skipped, which is more
than most descriptions of `pthread_cancel` will tell you.

Note the order in the output: `~Guard` ran *before* the cleanup handler, because
the handler was pushed first and cleanup handlers pop last-in-first-out just
like destructors. The two mechanisms interleave correctly rather than one
running to the exclusion of the other.

So what is the objection? The mechanism. That unwind is implemented as a special
exception, and glibc requires it to reach the top of the thread. Ordinary,
defensive, reviewed-and-approved C++ can stop it:

```cpp
void* swallow_worker(void*) {
    try {
        std::printf("cancel: worker parked inside catch (...)\n");
        std::fflush(stdout);
        for (;;) {
            sleep(1);
        }
    } catch (...) {
        // Looks like ordinary defensive C++. It is fatal here: the forced
        // unwind must be allowed to propagate, and glibc aborts if it is not.
        std::printf("cancel: caught the forced unwind and did not rethrow\n");
        std::fflush(stdout);
    }
    return nullptr;
}
```

```
[host]$ ./demo.sh cpp run cancel-swallow
cancel: worker parked inside catch (...)
cancel: caught the forced unwind and did not rethrow
FATAL: exception not rethrown
Aborted (core dumped)
```

Exit 134. Not the thread — the *process*. A `catch (...)` with no rethrow is a
pattern that appears in every large C++ codebase, usually at a thread entry
point, usually written precisely to stop one thread's failure from killing the
program. Combine it with `pthread_cancel` and it does the exact opposite of what
it was written to do.

That is the argument C++20 answered with `std::jthread` and `std::stop_token`.
Cooperative cancellation is a flag: `request_stop()` sets it, the target polls
`stop_requested()` wherever it decides is safe, and there is no unwinding, no
cancellation points to enumerate, and nothing for a catch-all to swallow. It
does less than `pthread_cancel` — a thread blocked in a syscall that never
checks the token will not stop — and that reduction is the whole design.

Both cancellation behaviors are hard-gated here, including the abort:

```lua
local swallow = checks.run("./demo.sh cpp run cancel-swallow 2>&1")
checks.expect_match(tostring(swallow.exit ~= 0), "true",
  "cpp: swallowing the forced unwind kills the process (exit=" .. tostring(swallow.exit) .. ")")
checks.expect_match(swallow.out, "exception not rethrown",
  "cpp: glibc names the reason -- a catch (...) that does not rethrow is fatal here")
```

The match is deliberately loose — `exception not rethrown` rather than the full
`FATAL: exception not rethrown` — because the surrounding text is a glibc
string and this gate is about the behavior, not about glibc's punctuation.

## The bridge: `std::thread` *is* a pthread here

Everything above used raw `pthread_create`, but none of it had to. The two
worlds compose, and the compiler will confirm it:

```cpp
static_assert(std::is_same_v<std::thread::native_handle_type, pthread_t>,
              "on this platform std::thread::native_handle() is a pthread_t");
```

That is not a runtime check that happened to pass. It is a compile-time
assertion that the standard type and the POSIX type are the same type on this
platform. So you can create a thread the standard way and control it the POSIX
way:

```
[host]$ ./demo.sh cpp run bridge
bridge: native_handle_is_pthread_t=yes setname_rc=0 tid=2675865 proc_comm='std-thread-x'
pthreads report: facet=bridge digest=0x481984990deee5ff
```

A `std::thread`, named from the outside through its native handle, and the name
is visible in that thread's procfs entry.

Using `native_handle()` is not a hack, and it is worth saying so plainly because
it often gets treated as one in review. It is in the standard *specifically* so
that platform facilities the standard does not wrap remain reachable. The
standard's own position is that it will not try to portably abstract thread
naming, stack sizing, affinity, or scheduling — and that you should still be
able to do all four.

The caveats are real but narrow. `native_handle_type` is implementation-defined,
so the `static_assert` above is doing load-bearing work rather than decoration —
on Windows the same call yields a `HANDLE` and none of this chapter compiles.
And the handle is only valid while the `std::thread` is joinable; using it after
`join()` is a use-after-free with a POSIX-shaped name.

## How the code works

Each facet is its own subcommand, and that structure is load-bearing rather
than tidy:

```cpp
    if (facet == "identity") {
        facet_identity();
    } else if (facet == "naming") {
        facet_naming();
    } else if (facet == "stack") {
        facet_stack();
```

`cancel-swallow` ends in `abort()` by design. If the facets shared a process,
that one would take the other seven with it and the harness would be asserting
on a corpse. Chapter 49 split its contract semantics into four binaries for the
same reason; this chapter splits one binary into subcommands, which is cheaper
and sufficient because only one facet is fatal.

The digest is carried forward unchanged:

```cpp
void report(const char* facet) {
    std::printf("pthreads report: facet=%s digest=0x%016llx\n", facet,
                static_cast<unsigned long long>(fnv1a(kPayload, sizeof kPayload)));
}
```

Every facet prints it, and gate A checks all seven. The claim is the same one
Chapter 49 made about execution models, narrowed one level: turning a per-thread
knob — renaming a thread, shrinking its stack, pinning it, cancelling it —
changes where and how the work runs and not what it computes.

## Errors, three ways

The three failure kinds in this program get three different treatments, and the
choice is the interesting part.

**A refusal that is the point.** `pthread_setschedparam` returning `EPERM` is
not an error path to be handled and hidden — it is the facet's result. It gets
printed with its errno name and asserted on. Failing is what an unprivileged
process is *supposed* to do here, and a demo that silently skipped the call
would have taught nothing.

**A refusal that is a trap.** `pthread_setname_np` returning `ERANGE` is a real
error that almost every caller ignores, because naming a thread feels
consequence-free. The example checks it and shows what ignoring it costs — the
thread silently keeps its old name:

```cpp
    std::printf("naming: 16char_rc=%d (%s) name_after='%s' unchanged=%s\n", too_long_rc,
                too_long_rc == ERANGE ? "ERANGE" : std::strerror(too_long_rc), after_too_long.c_str(),
                after_too_long == via_proc ? "yes" : "no");
```

**A failure that cannot be handled at all.** Swallowing the forced unwind does
not produce an errno, an exception you can catch, or a return code. glibc
prints one line and aborts. There is no error handling for this one — only
avoidance, which is precisely why the language grew a different mechanism.

The pattern: report the refusals that are informative, check the refusals that
are silent, and recognize the failures whose only handling is a different design.

## Concurrency lens

This example is about threads, so the lens turns on the example itself.

The facets synchronize almost entirely through `pthread_join` and
`std::thread::join`, and that is a deliberate simplification: `join` is a full
synchronization edge, so a worker can write to a plain `std::string` or
`std::vector` slot and the joining thread can read it afterwards with no atomics
and no mutex. `facet_naming` does exactly that with three ordinary variables
captured by reference. The pattern is safe because of the join, not despite it —
remove the join and every one of those reads becomes a data race.

One facet does need an atomic, and it is worth seeing why:

```cpp
    std::thread t([&] {
        tid = gettid();
        while (!named.load(std::memory_order_acquire)) {
            sched_yield();
        }
        name = comm_of(gettid());
    });

    // Name a std::thread from the outside, through its native handle.
    const int rc = pthread_setname_np(t.native_handle(), "std-thread-x");
    named.store(true, std::memory_order_release);
```

The bridge facet has a genuine ordering requirement that no `join` can supply:
the worker must not read its own `comm` until the *main* thread has finished
naming it. That is a happens-before edge in the middle of both threads' lives,
which is what release/acquire is for — the release store publishes the naming,
the acquire load consumes it. Chapter 26 built this machinery; here is the
smallest real use of it in the book.

The spin uses `sched_yield()` rather than a busy loop or a sleep. A raw spin
would burn a CPU for no reason on a possibly single-CPU host, and a sleep would
put a duration into a chapter that gates on none. `sched_yield` says "someone
else should run now," which is exactly the intent.

The affinity facet's `sched_yield()` is a different thing wearing the same name.
There it is not synchronization at all — the affinity call has already
succeeded and the mask is already installed. It only gives the kernel an
obvious moment to act on the new mask before `sched_getcpu()` samples, so the
observation is prompt. The comment in the source says so, because a `yield`
with no explanation reads like a synchronization bug to the next person.

## Build, run, observe

Local, unprivileged, offline.

```
[host]$ cd examples/50-pthreads
[host]$ ./demo.sh cpp build
[host]$ for f in identity naming stack affinity sched cancel bridge; do ./demo.sh cpp run "$f"; done
[host]$ ./demo.sh cpp run cancel-swallow    # aborts on purpose, exit 134
```

The facets are short, but `cancel` parks its worker for about 200 ms, which is
long enough to catch from another terminal — and seeing the two identities in a
tool other than our own program is worth the extra step:

```
[host]$ ./cpp/build/release/pthreads cancel & sleep 0.1
[host]$ ps -L -o pid,tid,comm,psr -p "$(pgrep -x pthreads)"
    PID     TID COMMAND         PSR
2676217 2676217 pthreads          2
2676217 2676219 pthreads         14
```

Two rows, one process. The first row's `TID` equals the `PID` — the same fact
the `identity` facet reported, now confirmed by a tool that has never heard of
this program. `PSR` is the CPU each thread is on, which the `affinity` facet is
what lets you choose. `COMMAND` is the procfs `comm` field, showing the process
name here because this particular facet does not rename its worker.

Then the harness:

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: a 16-character name is refused with ERANGE (comm is 16 bytes incl. NUL)
ok: cpp: the refused rename left the previous name in place -- a silent no-op if unchecked
ok: cpp: the thread created with a 256 KiB attr got at least 256 KiB (custom=262144)
ok: cpp: and materially less than the default (default_worker=8388608)
ok: cpp: a stack below PTHREAD_STACK_MIN is refused with EINVAL, not rounded up
ok: cpp: two threads pinned with pthread_setaffinity_np ran on two different CPUs
ok: cpp: pinning those threads left the caller's own affinity mask alone (main_cpus_allowed=16) -- unlike ch49's process-wide pin
ok: cpp: an unprivileged SCHED_FIFO request is refused with EPERM
ok: cpp: pthread_cancel unwinds the stack -- the C++ destructor really ran
ok: cpp: swallowing the forced unwind kills the process (exit=134)
ok: cpp: glibc names the reason -- a catch (...) that does not rethrow is fatal here
ok: cpp: std::thread::native_handle_type is pthread_t (static_assert, compile time)
ok: cpp: and procfs shows the name on the std::thread's own task
PASS 45 / FAIL 0
```

## What the standard still does not give you

Chapter 49 had two live C++26 features to hard-gate. This chapter has none, and
that absence is itself the finding rather than a gap to paper over: pthreads is
POSIX, and the C++ standard's position on all six controls above has not moved.

As of C++23, and on this toolchain with GCC 16 and clang 22, the standard
library offers no thread naming, no stack-size control, no affinity, and no
scheduling policy. `std::thread` gives you construction, `join`, `detach`,
`joinable`, an opaque `id`, and `hardware_concurrency()`. C++20 added
`std::jthread` and `std::stop_token` — which is a real addition, and this
chapter's cancellation section is the argument for why it was the right one —
but it does not widen the surface toward any of the other five.

That is a defensible position rather than an oversight. Thread naming has
different length limits on every platform, affinity does not exist in the same
shape on all of them, and a portable `set_stack_size` would have to say
something about platforms with no such concept. The committee's answer is
`native_handle()`: refuse to abstract what cannot be abstracted cleanly, and
guarantee an escape hatch instead.

The practical consequence for the rest of Part 14 is worth stating now. Every
higher-level model in this compendium — Boost.Thread, coroutines, fibers,
Asio — sits on top of this same layer. When one of them exposes a control the
standard does not, this is where it is coming from, and when one of them cannot
expose something, this is the layer you drop to.

## Cross-check: same digest, one level down

Every facet reported `digest=0x481984990deee5ff`, and clang agrees:

```
[host]$ cd cpp && cmake --preset release-clang && cmake --build --preset release-clang --target pthreads
[host]$ ./build/release-clang/pthreads identity
```

That constant now spans Chapter 46's C++ toolbox, Chapter 47's Go, Chapter 48's
Rust, Chapter 49's four execution models, and Chapter 50's six per-thread
controls under two compilers. Three languages, four execution models, six
knobs, one number.

The claim narrows appropriately each time. Chapter 49 said the execution model
does not change the answer. This chapter says the same about the per-thread
configuration: renaming a thread, shrinking its stack to 256 KiB, pinning it to
CPU 0, or cancelling it mid-flight changes where and how the work happens, and
not what it computes.

## What you learned

- **A thread has two identities.** `pthread_t` is an opaque glibc handle you
  may only compare; `gettid()` is the kernel's task id that `/proc`, `perf`,
  `ps -L`, and gdb all speak. They do not convert.
- **A process is its first thread.** The main thread's tid equals the pid,
  which is why `getpid()` inside a worker answers a different question than the
  one you meant to ask.
- **`/proc/self/task/<tid>/` is the per-thread mirror of `/proc/self/`.**
  Thread names written with `pthread_setname_np` show up there, and in every
  tool that reads it.
- **`comm` is 16 bytes including the NUL.** Fifteen characters maximum; glibc
  refuses a longer name with `ERANGE` and leaves the old one in place, so an
  unchecked naming call is a silent no-op.
- **Stack size is a real per-thread decision.** 8 MiB by default per created
  thread, tunable with `pthread_attr_setstacksize`, floored at
  `PTHREAD_STACK_MIN` with `EINVAL` rather than silent rounding.
- **Affinity and scheduling policy are per-thread.** Chapter 49's
  `sched_setaffinity(0, ...)` moved the whole process;
  `pthread_setaffinity_np` moves one thread and leaves the caller alone.
- **A refusal can be the observable.** An unprivileged `SCHED_FIFO` request
  fails with `EPERM`, and asserting that specific clean refusal is a better
  gate than a demo that would need root to show anything.
- **`pthread_cancel` does unwind — that is not the problem.** Destructors and
  cleanup handlers both run and `join` reports `PTHREAD_CANCELED`. The problem
  is that the mechanism is an exception, and an ordinary `catch (...)` without
  a rethrow aborts the whole process with `exception not rethrown`.
- **That is why `std::stop_token` exists.** Cooperative cancellation does
  strictly less, on purpose: a flag the target checks where it chooses, with
  nothing for a catch-all to swallow.
- **`native_handle()` is sanctioned, not a hack.** It is in the standard
  precisely so the six controls above stay reachable — and on this platform
  `native_handle_type` is `pthread_t`, asserted at compile time.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, <strong>glibc 2.43</strong>,
g++ 16.1.1 20260515, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0, Lua 5.4.8, 16
logical / 8 physical CPUs; local, unprivileged, no network, no VM):
<code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. &amp;&amp; pwd) lua verify.lua</code>
reported <code>PASS 45 / FAIL 0</code> with gate E (affinity) and gate I (clang
parity) both running for real rather than skipping, and <code>python3
scripts/test-all-examples.py --only 50-pthreads</code> reported <code>1 passed,
0 failed, 0 skipped</code>. Every transcript quoted above is a real run from
this session. Confirmed live: all seven non-aborting facets produced
<code>digest=0x481984990deee5ff</code>, byte-identical to Chapter 46's C++,
Chapter 47's Go, Chapter 48's Rust, and Chapter 49's <code>conc</code>;
<code>identity</code> reported <code>main pid=2675825 tid=2675825 same=yes</code>
with three distinct worker tids; <code>naming</code> reported the same
<code>'ch50-worker'</code> through both <code>pthread_getname_np</code> and
<code>/proc/self/task/&lt;tid&gt;/comm</code>, with a 16-character name refused
<code>rc=34 (ERANGE)</code> and the previous name intact; <code>stack</code>
reported <code>main=8376320 default_worker=8388608 custom=262144</code> and
<code>below_min_rc=22 (EINVAL)</code> against
<code>PTHREAD_STACK_MIN=16384</code>; <code>affinity</code> put two workers on
CPUs 0 and 1 with <code>main_cpus_allowed=16 unchanged=yes</code>;
<code>sched</code> reported <code>policy=0</code> (<code>SCHED_OTHER</code>) at
priority 0 with <code>SCHED_FIFO</code> refused <code>rc=1 (EPERM)</code> and
the policy unchanged; <code>cancel</code> ran <code>~Guard(raii)</code> and the
<code>pthread_cleanup_push</code> handler and reported
<code>joined_retval_is_canceled=yes</code>; <code>cancel-swallow</code> printed
<code>FATAL: exception not rethrown</code> and died of SIGABRT with exit 134;
and <code>bridge</code> named a <code>std::thread</code> through
<code>native_handle()</code> with <code>proc_comm='std-thread-x'</code>, the
<code>static_assert</code> on <code>native_handle_type</code> having already
passed at compile time. The <code>ps -L</code> transcript is a real capture of a
live <code>cancel</code> run. Not exercised: <span class="status
status--unverified">unverified</span> — a <em>successful</em> real-time policy
change is not gated anywhere in this example. It requires
<code>CAP_SYS_NICE</code> or an <code>RLIMIT_RTPRIO</code> allowance that this
unprivileged run does not have, so the chapter's claim is limited to the clean
<code>EPERM</code> refusal that was measured, and <code>verify.lua</code> prints
an informational line naming the stronger claim it is declining to make.</p>
