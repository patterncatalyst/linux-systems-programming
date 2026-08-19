---
title: "Boost.Thread: what the standard took, what it left, and the third way to cancel a thread"
order: 52
part: "Compendium: C++ Concurrency"
description: "boostthread asks what survived C++11's near-wholesale standardization of Boost.Thread, and answers with four capabilities std:: still lacks in C++23 on this toolchain: future::then() and when_all composing without blocking, where a concept evaluated against std::future<int> and boost::future<int> on every run reports no and yes; upgrade_lock promoting a reader to a writer in place, where std::upgrade_lock is not merely absent but undeclared; thread::interrupt() throwing an ordinary boost::thread_interrupted at defined interruption points; and thread::attributes::set_stack_size yielding exactly 262144, byte-identical to ch50's pthread_attr_setstacksize. The cancellation arc closes here -- catch (...) without a rethrow aborts the process under ch50's pthread_cancel (exit 134) and leaves it alive under Boost (exit 0), while a busy loop with no interruption point ignores interrupt() entirely. Verified against system Boost 1.90.0 on the Fedora 44 host: verify.lua PASS 37/FAIL 0, no Conan, no network."
duration: "55 minutes"
---

C++11 took Boost.Thread almost wholesale. `thread`, `mutex`, `lock_guard`,
`unique_lock`, `condition_variable`, `future`, `promise`, `packaged_task`,
`async` — the names, the semantics, and in several cases the wording moved into
the standard with barely a change. Boost.Thread is one of the most successful
standardization proposals in the language's history, and its reward for that
success is that most C++ programmers now have no reason to open it.

Which makes the interesting question a narrow one. Not "how do I use
Boost.Thread" — you already know, because you know `std::thread`. The question
is **what the standard did not take**, and whether any of it still matters
fifteen years later.

The answer is specific, it is four things, and every one of them is still
missing from C++23 on this toolchain.

{% include excalidraw.html
   file="52-what-the-standard-took"
   alt="Boost.Thread split into two bands by what C++11 standardized. The upper band, labelled standardized because C++11 took Boost.Thread almost wholesale, holds three boxes: thread, mutex, lock_guard, unique_lock and condition_variable which became std:: verbatim; future, promise, packaged_task, async and shared_future which became std:: minus .then(); and shared_mutex and barrier which arrived in C++17 and C++20 minus the upgrade lock. A side note summarizes the pattern: the TYPES were taken, the COMPOSITION and the third lock state were not. The lower amber band, labelled left behind and still Boost-only in C++23 on this toolchain, holds four boxes: future::then() and when_all which compose without blocking, showing async(21).then(*2) equals 42 against std::future having no .then(); upgrade_lock as a third lock state that is shared with respect to readers and exclusive with respect to upgraders and promotes without releasing; thread::interrupt() throwing an ordinary C++ exception called boost::thread_interrupted at defined points; and thread::attributes::set_stack_size at 256 KiB observing 262144, the same byte-for-byte as ch50's POSIX call. A note across the bottom states that std::upgrade_lock does not exist at all, being undeclared rather than merely absent. Amber arrows drop from the standardized futures and shared_mutex boxes down into the corresponding left-behind capabilities."
   caption="Figure 52.1 — C++11 took Boost.Thread's types nearly unchanged; what it left behind is composition, the third lock state, and per-thread attributes" %}

> **Tools used** — `g++` and `cmake` (host; gated by `scripts/check-host.sh` as
> `g++ >= 14` and `cmake >= 3.25`), `ninja` (host; gated), `clang++` (host;
> gated, parity gate G), `lua` (host; gated, runs `verify.lua`), and `python3`
> (host; gated, runs `scripts/test-all-examples.py`). **System Boost 1.90.0**
> from Fedora's `boost-devel` — the first Part 14 example with a third-party
> dependency, and still **no Conan and no network**. No VM, no root:
> `examples/52-boost-thread` is `mode: local` in `examples/manifest.yaml`.

C++-only, like the three chapters before it.

## Not claimed — compiled

Before any of the four pillars, one methodological point that shapes the whole
chapter.

"`std::future` has no `.then()`" is the kind of statement a book asserts and a
reader takes on trust, and it is the kind of statement that quietly rots when a
standard moves. It does not have to be. Whether a type has a member is
decidable at compile time:

```cpp
template <class F>
concept HasThen = requires(F f) { f.then([](F) {}); };
```

```
[host]$ ./demo.sh cpp run versions
versions: Boost 1.90.0 BOOST_THREAD_VERSION=5
versions: std::future_has_then=no boost::future_has_then=yes
```

That second line is the compiler's answer, produced on this run, on this
toolchain, and asserted by the harness. If a future GCC ships continuations on
`std::future`, this example starts reporting `yes` and the gate fails — which
is exactly the behavior you want from a claim about what a standard library
lacks.

Every "std:: cannot do this" in this chapter is either checked this way or
measured directly. None of it is quoted from documentation.

## Pillar 1: composition

`std::future` can be waited on. That is the entire interface for getting a
value out of it: block, or poll and then block. There is no way to say "when
this finishes, do that" without dedicating a thread to finding out.

```cpp
    boost::future<int> start = boost::async(boost::launch::async, [] { return 21; });

    // .then() takes the READY FUTURE, not the value -- so the continuation can
    // inspect whether the antecedent held a value or an exception. That is
    // why the lambda's parameter is boost::future<int> and not int.
    boost::future<int> doubled = start.then([](boost::future<int> prev) { return prev.get() * 2; });
```

```
[host]$ ./demo.sh cpp run continuations
continuations: async(21).then(*2) = 42 chained=yes
```

The detail in that comment is the design decision worth carrying away. `.then()`
hands the continuation the *future*, not the value. It looks like a papercut
until you consider the alternative: if the continuation received an `int`, there
would be nowhere to deliver an exception thrown by the antecedent. Taking the
future means `prev.get()` can rethrow inside the continuation, where it can be
caught, and error propagation composes exactly as value propagation does.

`when_all` is the same idea across futures rather than along one:

```cpp
    // when_all returns a future holding a tuple of the ORIGINAL futures, so
    // each one's value or exception survives the join individually.
    auto joined = boost::when_all(std::move(a), std::move(b));
```

```
[host]$ ./demo.sh cpp run when-all
when-all: joined two futures -> 1 + 2 = 3
```

Again the futures survive the join rather than being flattened into values, and
again the reason is that one of them might hold an exception.

This gap has a history worth knowing. Continuations were specified in the
Concurrency TS and were widely expected to land in C++17. They did not, and the
reason is that futures turned out to be the wrong foundation to build on — a
`future` implies shared state, an allocation, and a synchronization edge whether
or not the operation needs any of the three. C++26's `std::execution`
is the eventual answer, built on senders and receivers rather than futures, and
Chapter 49 measured that it does not exist on this toolchain yet
(`__cpp_lib_senders` undefined). Chapter 56 compares it for real if it arrives.

So this pillar is a genuine fifteen-year gap: the standard removed something on
purpose, and its replacement is still not here.

## Pillar 2: the third lock state

The read-mostly pattern: take a shared lock, inspect the data, occasionally
discover you need to modify it. With `std::shared_mutex` there is no path from
one to the other. You drop the shared lock, take a unique lock, and in the gap
between them another writer can change what you just read — so everything you
learned under the shared lock has to be re-checked.

`boost::upgrade_lock` is a third lock state, and the "third" is the point. It is
shared with respect to ordinary readers and *exclusive with respect to other
upgraders*, so at most one thread can be holding the intention to upgrade:

```cpp
    boost::upgrade_lock<boost::shared_mutex> up(sm);

    // A concurrent reader can still take a shared lock while we hold the
    // UPGRADE lock -- that is the point: readers are not blocked yet.
    boost::thread reader([&] {
        boost::shared_lock<boost::shared_mutex> rd(sm);
        reader_saw_old.store(shared_value == 1, std::memory_order_release);
        reader_done.store(true, std::memory_order_release);
    });
```

```
[host]$ ./demo.sh cpp run upgrade
upgrade: reader_saw_old_value=yes value_after_promotion=2
upgrade: promoted shared->unique without releasing
```

`reader_saw_old_value=yes` is the first half: readers were not blocked while the
upgrade lock was held. Then:

```cpp
    // Now promote. This does NOT release the lock in between, so no other
    // writer can interleave here.
    {
        boost::upgrade_to_unique_lock<boost::shared_mutex> unique(up);
        shared_value = 2;
    }
```

The promotion waits for existing readers to drain and then converts in place.
There is no window, so nothing you read under the upgrade lock can have changed
by the time you write.

The `std::` side of this comparison is unusual and worth stating precisely:
**`std::upgrade_lock` is not absent, it is undeclared.** While writing the
compile-time contrast for pillar 1 I tried the same trick here — a concept
testing for `std::upgrade_lock<std::shared_mutex>` — and it does not compile at
all:

```
error: 'upgrade_lock' in namespace 'std' does not name a template type
```

A concept can ask whether a type has a member. It cannot ask whether a name
exists, because a name that does not exist is a hard error rather than a
substitution failure. That is why gate D has no `std::`-side half: there is
nothing to point a test at.

## Pillar 3: cancellation, the third way

Two chapters have now ended on cancellation, and this one closes the arc.

{% include excalidraw.html
   file="52-cancellation-three-ways"
   alt="One mistake, catch parenthesis dot dot dot without a rethrow, run against three cancellation models across an amber band. Left: ch50's pthread_cancel, a glibc forced unwind where destructors do run but the unwind must propagate, asynchronous at any cancellation point, leading to a dashed ghost box reading FATAL colon exception not rethrown, SIGABRT exit 134, the PROCESS dies not the thread. Centre: ch52's thread::interrupt(), an ordinary C++ exception called boost::thread_interrupted thrown at defined points only such as sleep_for, condition variable wait and join, leading to an amber box reading process survives exit 0, a swallowed exception is a bug not a process kill. Right: ch51's stop_token, a flag where nothing is thrown, request_stop sets it and the target polls when it likes, cooperative and poll-only, leading to a box reading nothing to swallow because there is no exception at all and the risk is never stopping. A closing note states Boost sits between the two, catchable like an exception because that is exactly what it is."
   caption="Figure 52.2 — the same mistake against three cancellation models: power runs left to right, safety runs right to left, and Boost is the only one that is both catchable and non-fatal when mishandled" %}

Chapter 50 showed that `pthread_cancel` unwinds — destructors run, cleanup
handlers run — but that the unwind is a special exception glibc *requires* to
propagate. A `catch (...)` with no rethrow, which exists in every large C++
codebase and is usually written to stop one thread's failure from killing the
program, does precisely the opposite: `FATAL: exception not rethrown`, SIGABRT,
exit 134.

Chapter 51 showed C++20's answer: `std::stop_token` is a flag. Nothing is
thrown, so nothing can be swallowed, and a thread stops because it agreed to.

Boost.Thread is the third point, and it sits between them:

```cpp
    } catch (const boost::thread_interrupted&) {
        caught->store(true, std::memory_order_release);
        std::printf("interrupt: worker caught boost::thread_interrupted\n");
        std::fflush(stdout);
        // Rethrowing is optional here. Returning is fine -- and that is the
        // whole difference from ch50.
    }
```

`boost::thread_interrupted` is an ordinary C++ exception. Not a compiler
intrinsic, not a glibc forced unwind — a class you can catch by reference, log,
translate into your own error type, and decline to rethrow.

The decisive test is the ch50 mistake, run again:

```cpp
    } catch (...) {
        // The EXACT mistake that killed the process in ch50: catch everything,
        // rethrow nothing. Against pthread_cancel's forced unwind this aborts
        // with "FATAL: exception not rethrown". Against a Boost interruption
        // it is merely a swallowed exception, because that is all it is.
```

```
[host]$ ./demo.sh cpp run interrupt-swallow; echo "exit=$?"
interrupt-swallow: caught the interruption and did NOT rethrow
interrupt-swallow: process survived -- boost interruption is an ordinary exception, not glibc's forced unwind
exit=0
```

Exit 0. The identical code shape, the identical mistake, and the process lives.

The difference is not how careful the programmer was — it is what the mechanism
*is*. A forced unwind carries a requirement to propagate because glibc has no
other way to guarantee the thread actually dies. An ordinary exception carries
no such requirement, so swallowing it is an ordinary bug: the thread keeps
running when you meant it to stop, and you find out from a log line rather than
a core dump.

### What Boost gives up for that

Interruption points are a **defined list** — `this_thread::sleep_for`,
`condition_variable::wait`, `thread::join`, `this_thread::interruption_point()`,
and a handful of others. A thread that never reaches one cannot be interrupted,
and rather than assert that, the example demonstrates it:

```cpp
void busy_worker() {
    try {
        for (unsigned long i = 0;; ++i) {
            g_sink = g_sink + i;  // no sleep, no wait, no interruption point
        }
    } catch (const boost::thread_interrupted&) {
        g_busy_stopped.store(true, std::memory_order_release);
    }
}
```

```
[host]$ ./demo.sh cpp run interrupt-busy
interrupt-busy: interrupt() delivered, worker_stopped=no
interrupt-busy: a loop with no interruption point never checks, so it never stops
```

`interrupt()` was called and returned successfully. The thread simply never
looked. `pthread_cancel` *would* have stopped this loop — asynchronous
cancellation can interrupt a thread that is not cooperating, which is exactly
the power that makes it dangerous.

So the three models line up as a trade rather than a ranking:

| model | reach | mishandled |
| --- | --- | --- |
| `pthread_cancel` | any cancellation point, asynchronous | **kills the process** |
| `thread::interrupt()` | defined interruption points | thread keeps running |
| `stop_token` | wherever the target polls | thread keeps running |

Power runs down the table, safety runs up it. Boost.Thread is the only one of
the three that is both catchable and non-fatal when mishandled — which is why it
is worth knowing even though the standard chose differently.

That subcommand leaves via `_exit(0)` with the worker still spinning, because
there is no way to join a thread that will not stop. Everything it touches is
`static`, since a detached thread outliving `main()` must not be reading
`main`'s stack — a lesson learned the hard way while building it, when the first
version dumped core doing exactly that.

## Pillar 4: attributes, and a genuinely nasty gotcha

Chapter 50 set a thread's stack size with `pthread_attr_setstacksize` and noted
that `std::thread` offers nothing equivalent. Boost.Thread does, portably:

```
[host]$ ./demo.sh cpp run attributes
attributes: requested=262144 observed=262144 at_least_requested=yes
attributes: this is ch50's pthread_attr_setstacksize, reached portably
```

262144 — byte-identical to Chapter 50's measurement, because on Linux this
*is* that call. The gate asserts the exact number for that reason: it is not a
coincidence worth hedging, it is the same POSIX API reached through a portable
wrapper.

Getting there requires knowing one thing that the compiler will not tell you:

```cpp
    const boost::thread::attributes& cattrs = attrs;
    boost::thread worker(cattrs, &stack_probe);
```

That `const` is **required**, and it is not a style preference. With
`BOOST_THREAD_PROVIDES_VARIADIC_THREAD` — the default at version 5 — a
*non-const* `attributes` lvalue is captured by the variadic
`thread(F&& f, Args&&... args)` constructor, which deduces
`F = thread_attributes` and then tries to **call** the attributes object. What
you get is a wall of template errors out of `boost/thread/detail/invoke.hpp`:

```
error: no matching function for call to 'invoke(std::remove_reference<boost::thread_attributes&>::type, ...)'
error: 'f' cannot be used as a member pointer, since it is of type 'boost::thread_attributes'
```

Neither line mentions the constructor you meant to call. A lambda fails this
way, a function pointer fails this way, and adding a dummy argument to
disambiguate does not help — the variadic overload just swallows that too.
Binding a `const` reference makes `thread(attributes const&, F&&)` the only
viable candidate, and it compiles immediately.

This is the kind of thing that costs an afternoon, and it is in the chapter
because it cost one.

## How the code works

The Boost configuration macros live in `CMakeLists.txt` rather than the source
file, and that placement is load-bearing:

```cmake
target_compile_definitions(boostthread PRIVATE
  BOOST_THREAD_VERSION=5
  BOOST_THREAD_PROVIDES_FUTURE
  BOOST_THREAD_PROVIDES_FUTURE_CONTINUATION
  BOOST_THREAD_PROVIDES_FUTURE_WHEN_ALL_WHEN_ANY)
```

Boost reads these while its own headers are being preprocessed. A `#define` that
lands after the first Boost include — because some other header pulled it in
first — is silently ignored, and the symptom is not a warning but a missing
`.then()`, i.e. a compile error in *your* code pointing nowhere near the cause.
Putting them on the command line makes that impossible.

CMake 4 needed one more thing:

```cmake
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
```

CMake 4 removed its own `FindBoost` module in favour of the
`BoostConfig.cmake` that Boost now installs. Without the policy set, the build
still succeeds but warns that it found Boost by a path CMake has announced it is
deleting. The `if(POLICY ...)` guard keeps it working on older CMake too.

## Errors, three ways

**An error the type system prevents.** The `HasThen` concept turns "does this
API exist" into a compile-time value, so the chapter's central factual claim
cannot silently rot.

**An error the type system makes worse.** The `attributes` gotcha is the
counter-example, and an instructive one: C++'s overload resolution picked a
technically-viable candidate and produced a diagnostic that describes a
consequence three layers down rather than the mistake. Perfect forwarding plus
an unconstrained variadic constructor is a known-sharp combination, and this is
what it looks like from the wrong end.

**An error you can decline to handle.** `boost::thread_interrupted` can be
caught and not rethrown, and the process survives. Whether that is an error at
all depends on intent — which is the difference between a mechanism that
enforces a policy and one that offers you a choice.

## Concurrency lens

The `upgrade` pillar contains the one piece of genuine synchronization
reasoning in this example, and it is worth being precise about why the
handshake is written the way it is:

```cpp
    while (!reader_done.load(std::memory_order_acquire)) {
        boost::this_thread::yield();
    }
    reader.join();
```

The acquire load pairs with the reader's release store, so `reader_saw_old` is
guaranteed visible before it is read. The `join()` immediately afterwards would
have supplied that edge on its own — but the spin has to come first, because the
promotion must not happen until the reader has actually observed the old value.
Joining without the spin would be correct and would prove nothing: the reader
might have run entirely after the promotion.

`yield()` rather than a busy spin, for the reason Chapter 51 gave: a spin burns
a CPU that a single-CPU host may not have to spare, and a sleep would put a
duration into a chapter that gates on none.

The `interrupt-busy` pillar has the opposite shape — deliberately no
synchronization at all, because the whole point is a thread that never reaches
a point where it could notice anything.

## Build, run, observe

```
[host]$ cd examples/52-boost-thread
[host]$ ./demo.sh cpp build
[host]$ for p in versions continuations when-all upgrade interrupt interrupt-busy attributes; do
          ./demo.sh cpp run "$p"; done
```

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

```
ok: cpp: std::future has NO .then() -- a concept the compiler evaluated, not a claim
ok: cpp: boost::future::then() chained a continuation onto a running async
ok: cpp: boost::when_all joined two independent futures into one
ok: cpp: a concurrent reader could still take a shared lock while the upgrade lock was held
ok: cpp: upgrade_to_unique_lock promotes in place -- no window for another writer, which is exactly what std::shared_mutex cannot offer
ok: cpp: swallowing a Boost interruption leaves the process ALIVE (exit 0) -- the identical mistake against ch50's pthread_cancel aborted with exit 134
ok: cpp: a busy loop with no interruption point ignores interrupt() entirely -- Boost interrupts at defined points, it does not preempt
ok: cpp: and the number is 262144 -- byte-identical to what ch50 measured through pthread_attr_setstacksize directly, because on Linux this is that call
PASS 37 / FAIL 0
```

## Cross-check: one digest, six chapters

Every pillar reported `digest=0x481984990deee5ff`, and clang agrees. That
constant now spans Chapter 46's C++ toolbox, Chapter 47's Go, Chapter 48's Rust,
Chapter 49's four execution models, Chapter 50's six per-thread controls,
Chapter 51's eleven synchronization cases, and Chapter 52's eight pillars.

The stack-size number is the sharper cross-check here. Chapter 50 called
`pthread_attr_setstacksize(256 KiB)` and measured 262144. This chapter called
`boost::thread::attributes::set_stack_size(256 KiB)` and measured 262144. Two
APIs, two chapters, one syscall underneath — the same relationship Chapter 51
established between `std::mutex` and the futex, one library higher.

## What you learned

- **C++11 took Boost.Thread's types, not its composition.** `thread`, `mutex`,
  `future`, and friends moved into the standard nearly unchanged; `.then()`,
  `when_all`, and the upgrade lock did not.
- **Check API claims with the compiler.** A `concept` evaluated against
  `std::future<int>` and `boost::future<int>` reports `no` and `yes` on every
  run, so "the standard lacks this" is verified rather than trusted.
- **`.then()` hands the continuation the future, not the value** — which is what
  lets an exception from the antecedent rethrow inside the continuation, so
  errors compose the same way values do.
- **The continuations gap is deliberate and unresolved.** The Concurrency TS
  specified them, C++17 declined on the grounds that futures were the wrong
  foundation, and `std::execution` — the replacement — still does not exist on
  this toolchain (Chapter 49 measured it).
- **`upgrade_lock` is a third lock state**, exclusive against other upgraders,
  which is what lets it promote to unique with no window for another writer.
  `std::upgrade_lock` is undeclared, not merely absent — a concept cannot even
  test for it.
- **`boost::thread_interrupted` is an ordinary exception.** Catching it without
  rethrowing leaves the process alive at exit 0, where the identical mistake
  against `pthread_cancel` aborts at exit 134. The mechanism, not the caller's
  care, is what differs.
- **Boost trades reach for predictability.** Interruption happens only at
  defined points, so a busy loop ignores `interrupt()` entirely —
  `pthread_cancel` would have stopped it, and that is precisely its danger.
- **Put Boost's configuration macros on the compile line.** Defined after the
  first Boost include they are silently ignored, and the symptom is a missing
  member function rather than a warning.
- **Pass `boost::thread::attributes` as a `const` lvalue.** A non-const one is
  captured by the variadic constructor, which tries to *call* it and produces
  errors that never name the real problem.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, glibc 2.43,
<strong>Boost 1.90.0 from <code>boost-devel-1.90.0-7.fc44</code></strong> with
<code>BOOST_THREAD_VERSION=5</code>, g++ 16.1.1 20260515, clang 22.1.8, CMake
4.3.0, Ninja 1.13.0, Lua 5.4.8; local, unprivileged, <strong>no Conan and no
network</strong>, no VM): <code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. &amp;&amp; pwd)
lua verify.lua</code> reported <code>PASS 37 / FAIL 0</code> with the clang
parity gate running for real rather than skipping, and <code>python3
scripts/test-all-examples.py --only 52-boost-thread</code> reported <code>1
passed, 0 failed, 0 skipped</code>. Every transcript quoted above is a real run
from this session. Confirmed live: all eight pillars produced
<code>digest=0x481984990deee5ff</code>, byte-identical to Chapters 46 through
51; the <code>HasThen</code> concept reported
<code>std::future_has_then=no boost::future_has_then=yes</code>;
<code>.then()</code> chained <code>async(21)</code> into <code>42</code> and
<code>when_all</code> joined two futures into <code>1 + 2 = 3</code>; the
upgrade pillar reported <code>reader_saw_old_value=yes</code> then
<code>value_after_promotion=2</code>; <code>interrupt</code> caught
<code>boost::thread_interrupted</code> and joined normally;
<code>interrupt-swallow</code> printed <code>process survived</code> and exited
<code>0</code>, against ch50's measured <code>exit 134</code> for the same code
shape; <code>interrupt-busy</code> reported <code>worker_stopped=no</code> after
a successful <code>interrupt()</code>; and <code>attributes</code> reported
<code>requested=262144 observed=262144</code>, byte-identical to Chapter 50's
<code>pthread_attr_setstacksize</code> measurement. The
<code>std::upgrade_lock</code> claim was verified by compiling a concept against
it and observing the hard error <code>'upgrade_lock' in namespace 'std' does not
name a template type</code>, which is why it is described as undeclared rather
than absent. Not exercised: <span class="status
status--unverified">unverified</span> — Boost's experimental executors
(<code>boost::executors</code>) are not built or gated anywhere in this example.
They are the part of Boost.Thread that C++26's <code>std::execution</code> is
positioned to replace, and Chapter 49 measured that senders do not exist on this
toolchain (<code>__cpp_lib_senders</code> undefined); the live comparison is
deferred to Chapter 56.</p>
