---
title: "Shared state and the futex"
order: 25
part: "Concurrency in Depth"
description: "workq builds a bounded MPMC job queue three ways — std::mutex + condvars, a buffered channel, Mutex + Condvar — and we look under all of them at the one primitive doing the work: the futex, whose uncontended fast path is a userspace CAS with no syscall and whose slow path is FUTEX_WAIT/WAKE, proven by strace scaling from 5,186 to 488,818 calls, then broken on purpose under ThreadSanitizer and go -race while Rust refuses to compile the race at all."
duration: "50 minutes"
---

Every chapter since Part 3 has reached for a mutex without asking what one
*is*. `pmon` guarded its supervisor state with one; Chapter 20's `shmkv`
went a layer lower and called `futex(2)` by hand on a word in a mapped file.
This chapter closes the gap between those two: the `std::mutex`,
`sync.Mutex`, and `std::sync::Mutex` you have been using are **futexes
wearing a library**, and once you can see the futex underneath, the whole
behaviour of contended code — why an uncontended lock is nearly free, why a
contended one is expensive, why a lost wakeup hangs a program forever —
stops being folklore. The example is `workq`, a bounded multi-producer /
multi-consumer job queue with a worker pool, written three times with each
language's native shared-state kit but one observable contract, so a single
`verify.lua` covers all three. Then we break it: `--buggy` routes every
consumer into one unsynchronised counter, and we watch three tools react.

The code is in `examples/25-shared-state-and-the-futex/`. `./demo.sh` there
builds all three implementations; its `README.md` specifies the CLI, the
deterministic value contract, and the exit codes all three languages share.

{% include excalidraw.html
   file="25-mpmc-queue"
   alt="Left to right: three producer boxes (producer 0 owns i=0 mod P, producer 1 owns i=1, a dashed producer P-1) feed push arrows into a central amber bounded-queue box labeled cap K, mutex plus not_full and not_empty condvars, equals buffered chan in Go, produced is atomic long; pop arrows fan out to three consumer boxes each holding a local (count, sum), the dashed one being consumer C-1; an amber dashed XOR-combine-after-join arrow runs from the consumer stack down to a dark box reading XOR-fold locals after join, checksum equals 42b3746c6cee7465, independent of P, C, cap. A caption note says one mutex serializes every push and pop, and XOR is commutative so drain order cannot change the result."
   caption="Figure 25.1 — workq's shape: P producers, one bounded queue, C consumers folding into per-consumer accumulators combined by XOR — a program whose correct output is one fixed constant no matter how the threads interleave" %}

> **Tools used** — `strace`, `perf`, `/usr/bin/time`, `cat`
> (`/proc/<pid>/task/*/status`), `cmake`/`g++` with `-fsanitize=thread`,
> `go build -race`, `rustc` (host); bcc-tools `offcputime` (lab VM, exercised
> in Part 8). Everything host-side is checked by `scripts/check-host.sh` or
> ships with Fedora.

## The mutex is a futex

A **futex** — fast userspace mutex — is a 32-bit word in your address space
plus one syscall, `futex(2)`, that the kernel only ever touches when threads
actually collide. The design splits every lock operation into two paths. On
the **fast path**, the lock word is `0` (free); acquiring it is a single
atomic compare-and-swap from `0` to `1`, executed entirely in userspace by
one `cmpxchg` instruction. No syscall, no mode switch, no scheduler — the
kernel does not even know a lock was taken. On the **slow path**, the CAS
loses because the word is already `1`: only *now* does the thread call
`futex(FUTEX_WAIT, &word, 1)`, which asks the kernel to park it on a wait
queue keyed by the physical address of the word until someone else calls
`futex(FUTEX_WAKE, &word)` on unlock. This is the whole reason locks are
cheap: contention is what costs, not locking.

{% include excalidraw.html
   file="25-futex-fast-slow-path"
   alt="Two horizontal bands. The upper user-space band (the lock word lives here, glibc pthread / libstdc++ / std) shows on the left Thread A: lock() with word == 0, an arrow labeled cmpxchg into an amber CAS word 0 to 1 box (one atomic instruction, returns held), and a note Fast path: no syscall, ~98% of lock ops in the 1x1 run stay here. On the right Thread B: lock() finds word == 1 and its CAS loses, and Thread A: unlock() sets word 1 to 0. The lower kernel band (entered only when the CAS loses) holds an amber futex wait queue box keyed by (mm, and word) with Thread B parked; an amber futex(FUTEX_WAIT) arrow descends from Thread B into it, an amber futex(FUTEX_WAKE) arrow descends from Thread A's unlock, and a dashed runnable arrow returns Thread B to user space. A footer note reads voluntary context switches track the parking: 302 in the 1x1 run to 139,565 in the 8x8 run, from /usr/bin/time -v."
   caption="Figure 25.2 — one lock, two paths: the uncontended CAS never leaves userspace; only a lost CAS descends into FUTEX_WAIT, and the numbers are this chapter's measured strace and context-switch scaling" %}

The **condition variable** is the same primitive with a subtlety that has
sunk more programs than any other in this book: the **lost wakeup**. A
condvar's `wait` must (1) release the mutex, (2) block, and (3) reacquire —
and if a `notify` lands in the gap between a thread deciding to wait and
actually parking, a naive implementation sleeps through it forever. Two
mechanisms defend against it. First, `futex(FUTEX_WAIT, &word, expected)`
takes the value the caller *expects*; the kernel re-checks it under the wait
queue lock and returns `EAGAIN` if the word already changed, refusing to
park on stale state — in the 1×1 run below, 1,796 of 4,583 futex calls were
exactly this `EAGAIN` re-check. Second, and this is the discipline *your*
code owes, a condvar wait must always sit inside a **predicate loop**, never
a bare `if` — re-test the condition on every wake, because wakeups can be
spurious and the world can change between wake and reacquire. Every queue in
this chapter obeys that rule, and it is the single most important line in
each.

Higher-level wrappers are the same futex again. C++20 added
`std::atomic<T>::wait`/`notify_one`, a futex on a bare atomic with no mutex;
Go's `sync.Mutex` spins briefly, then parks on the runtime's futex
equivalent, and since Go 1.9 flips into a **starvation mode** that hands the
lock directly to the longest waiter once anyone has waited over 1 ms, trading
throughput for fairness; Rust's `parking_lot::Mutex` is one word and often
beats `std` under contention, though `std::sync::Mutex` on Linux has itself
been a thin futex since 1.62. Different vocabularies, one kernel object.

## How the code works

The queue is the heart of it, and it is where the three languages diverge
most, so it gets the tabs. Each is the real blocking-queue primitive,
verbatim from `examples/25-shared-state-and-the-futex/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
  void Push(std::uint64_t v) {
    std::unique_lock lock(mu_);
    not_full_.wait(lock, [&] { return q_.size() < cap_; });
    q_.push_back(v);
    not_empty_.notify_one();
  }

  // Pop one item; returns false once the queue is closed and drained.
  bool Pop(std::uint64_t& out) {
    std::unique_lock lock(mu_);
    not_empty_.wait(lock, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;  // closed_ is true here
    out = q_.front();
    q_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void Close() {
    {
      std::unique_lock lock(mu_);
      closed_ = true;
    }
    not_empty_.notify_all();
  }
```

```go
	// Closer: end the consumers' range once every item has been pushed.
	go func() {
		producers.Wait()
		close(queue)
	}()

	var consumers sync.WaitGroup
	for idx := 0; idx < c.consumers; idx++ {
		consumers.Add(1)
		go func(idx int) {
			defer consumers.Done()
			for v := range queue {
				if c.buggy {
					sharedConsumed++    // DATA RACE: unsynchronized shared counter
					sharedChecksum ^= v // DATA RACE: unsynchronized shared checksum
				} else {
					localCounts[idx]++
					localSums[idx] ^= v
				}
			}
		}(idx)
	}
	consumers.Wait()
```

```rust
    fn push(&self, v: u64) {
        let mut g = self.inner.lock().unwrap();
        while g.q.len() >= self.cap {
            g = self.not_full.wait(g).unwrap();
        }
        g.q.push_back(v);
        drop(g);
        self.not_empty.notify_one();
    }

    /// Pop one item; returns None once the queue is closed and drained.
    fn pop(&self) -> Option<u64> {
        let mut g = self.inner.lock().unwrap();
        loop {
            if let Some(v) = g.q.pop_front() {
                drop(g);
                self.not_full.notify_one();
                return Some(v);
            }
            if g.closed {
                return None;
            }
            g = self.not_empty.wait(g).unwrap();
        }
    }

    fn close(&self) {
        let mut g = self.inner.lock().unwrap();
        g.closed = true;
        drop(g);
        self.not_empty.notify_all();
    }
```

The C++ and Rust queues are the same design in two spellings: one
`std::mutex`/`Mutex` guards a `std::deque`/`VecDeque`, two condition
variables split the two blocking reasons apart — producers park on
`not_full`, consumers on `not_empty` — and `Close` sets `closed_` under the
lock then `notify_all`s so every drained consumer wakes, sees the queue empty
*and* closed, and returns cleanly. The predicate is the correctness: C++'s
`not_empty_.wait(lock, [&]{ return !q_.empty() || closed_; })` re-tests on
every wake, and Rust spells the identical loop by hand with
`while ... { g = self.not_empty.wait(g).unwrap(); }`. Notice both `notify`
*after* releasing (Rust's explicit `drop(g)`; C++'s scope) so the woken
thread does not immediately re-block on a lock the notifier still holds — the
classic "wake-then-stall" pessimisation. Go throws the whole apparatus away:
a **buffered channel** `make(chan uint64, c.cap)` *is* the bounded blocking
queue — send blocks when full, receive blocks when empty, both on the
runtime's own futex parks — and closing the channel is the drain signal that
ends every consumer's `range`. A separate closer goroutine waits on the
producers' `WaitGroup` and calls `close(queue)`; without that indirection,
whoever closed early would panic a producer still sending.

The accumulator design is the quiet star. On the correct path each consumer
folds into *its own* `localSums[idx]` slot — no shared write on the hot path
at all — and the per-consumer sums are XOR-combined only after every thread
has joined, an edge the join itself establishes. That is deliberate: because
each item's `payload(seed, i)` is a pure function of its index and XOR is
commutative and associative, the final checksum is one fixed constant
(`42b3746c6cee7465` for the default seed at N=100000) no matter how items
split across producers or which consumer drains which. The only shared write
in flight is the `produced` counter, and it is a `std::atomic<long>` /
`atomic.AddInt64` / `AtomicI64` incremented with **relaxed** ordering,
because it needs atomicity but no happens-before edge — the mutex and channel
already supply all the ordering the data needs.

## Errors, three ways

The argument contract is the same shape as every example in the book, and
all three languages produce byte-identical diagnostics. A missing required
flag prints the usage line alone and exits 2; `--foo` prints
`workq: unknown flag: --foo` then usage; `--items x` prints
`workq: not an integer: x`; `--producers 0` prints
`workq: --producers and --consumers must be >= 1`; and a value-less
`--producers` at end of line prints `workq: --producers needs a value`.
`verify.lua` asserts all of these, plus the degenerate `--items 0` draining
to `produced=0 consumed=0 checksum=0000000000000000`, per language — 25
checks each. The split of responsibility mirrors the languages' Chapter 5
policies: C++ threads `std::expected<Config, ParseError>` through the parser,
Go returns `(config, msg, ok)`, Rust returns `Result<Config, Option<String>>`
where `Err(None)` means "usage only" — three encodings of the same state
machine, one observable behaviour.

## Concurrency lens

The interesting failures here are not error codes, they are **data races**,
and `--buggy` manufactures a real one: every consumer abandons its private
slot and mutates one shared `shared_consumed` / `shared_checksum` with no
lock. Two threads running `counter += 1` interleave load/add/store and lose
updates, so `consumed` comes out *below* N and the checksum is garbage — the
canonical race. What each language does about it is the lesson. C++ and Go let
you *write* the race and rely on a sanitizer to catch it at runtime; Rust
will not let the racing version compile at all, because a shared `&mut` across
threads violates the borrow checker — the buggy path has to smuggle a raw
`*mut` through a hand-written `unsafe impl Send`/`Sync` wrapper (`Racy<T>`)
just to express what the other two write by accident. The type system is not
being pedantic; it is refusing the exact bug the sanitizers exist to find. We
prove all three below.

## Build, run, observe

```bash
[host]$ cd examples/25-shared-state-and-the-futex && ./demo.sh build
```

The runner agrees on the correct path: `python3 scripts/test-all-examples.py
--only 25-shared-state-and-the-futex` reports `PASS PASS PASS` (3 passed, 0
failed, 0 skipped), 25 `verify.lua` checks per language, Go built under
`-race`. A hand run confirms the shared constant regardless of shape:

```console
[host]$ ./cpp/build/release/app --producers 8 --consumers 8 --items 100000 --cap 8
workq: produced=100000 consumed=100000 checksum=42b3746c6cee7465 ms=127
```

Now break it. **C++ under ThreadSanitizer** (`cmake --preset tsan && cmake
--build --preset tsan`) names the two racing lines precisely:

```console
[host]$ ./cpp/build/tsan/app --producers 4 --consumers 4 --items 100000 --buggy
WARNING: ThreadSanitizer: data race (pid=2802859)
  Read of size 8 at 0x7ffe96951458 by thread T6:
    #0 operator() .../cpp/src/main.cpp:209 (app+0x400da1)
  Previous write of size 8 at 0x7ffe96951458 by thread T5:
    #0 operator() .../cpp/src/main.cpp:209 (app+0x400db0)
SUMMARY: ThreadSanitizer: data race .../cpp/src/main.cpp:209 in operator()
...
workq: produced=100000 consumed=80629 checksum=a27c6c5d34a3e44a ms=4177
```

Lines 209 and 210 are exactly `shared_consumed += 1;` and
`shared_checksum ^= v;`, and `consumed=80629` shows ~19,000 lost updates.
**Go under `-race`** finds the same thing at `main.go:160`, prints
`WARNING: DATA RACE` for each pair of colliding goroutines, and exits nonzero:

```console
[host]$ ./go/bin/app --producers 4 --consumers 4 --items 100000 --buggy
WARNING: DATA RACE
Read at 0x00c000018198 by goroutine 13:
  main.run.func3() .../go/main.go:160 +0x23d
Previous write at 0x00c000018198 by goroutine 16:
  main.run.func3() .../go/main.go:160 +0x252
...
workq: produced=100000 consumed=95400 checksum=46376190c7e5d451 ms=3122
Found 4 data race(s)
```

**Rust** needs no sanitizer — safe Rust rejects the race outright. Reduce the
buggy accumulator to its essence and `rustc` refuses it:

```console
[host]$ rustc --edition 2024 racetest.rs
error[E0499]: cannot borrow `consumed` as mutable more than once at a time
 6 |             s.spawn(|| {
   |             -       ^^ `consumed` was mutably borrowed here in the previous iteration of the loop
 7 |                 consumed += 1;
   |                 -------- borrows occur due to use of `consumed` in closure
```

The example's `--buggy` only compiles because it opts out through `unsafe`,
and when it runs the corruption wanders every time — `consumed=97094`,
`97633`, `97301`, `95632` across four runs, never the correct `100000` — the
empirical shadow of the guarantee the compiler was offering.

## Cross-check: the futex under the microscope

The claim that an uncontended lock avoids the kernel entirely is checkable
with `strace -c`, counting `futex(2)` calls as we turn contention up. Two
runs of the same 100,000-item workload, first with one producer and one
consumer over a roomy 256-slot queue, then with eight of each over an
8-slot queue:

```console
[host]$ strace -f -c -e trace=futex ./cpp/build/release/app --producers 1 --consumers 1 --items 100000 --cap 256
% time     seconds  usecs/call     calls    errors syscall
100.00    0.021349           4      4583      1796 futex
[host]$ strace -f -c -e trace=futex ./cpp/build/release/app --producers 8 --consumers 8 --items 100000 --cap 8
% time     seconds  usecs/call     calls    errors syscall
100.00   33.008779          67    491514    124803 futex
```

The 1×1 run performs roughly 200,000 lock acquisitions (a `Push` and a `Pop`
per item, each taking the mutex) yet enters the kernel only **4,583** times —
about 98% of all lock operations resolved as a pure userspace CAS, exactly
the fast path. Crank contention to 8×8 with a tight buffer and the count
explodes to **491,514**: every collision that loses its CAS now pays a
`FUTEX_WAIT`, and every unlock with a waiter pays a `FUTEX_WAKE` (the exact
counts drift a few percent per run — a repeat 1×1 gave 4,623, an 8×8 gave
488,818 — but the two-orders-of-magnitude gap is invariant). Watching
the raw calls confirms both operations, `2` being the mutex's contended state
and `2147483647` a condvar `notify_all`:

```console
[pid ...] futex(0x7ffe96160f70, FUTEX_WAIT_PRIVATE, 2, NULL) = 0
[pid ...] futex(0x7ffe96160f70, FUTEX_WAKE_PRIVATE, 1)       = 1
          futex(0x7f9c51aaf6bc, FUTEX_WAKE_PRIVATE, 2147483647) = 0
```

An independent surface tells the same story. Every `FUTEX_WAIT` that actually
parks is a **voluntary context switch** — the thread yielding the CPU because
it chose to block — and `/usr/bin/time -v` counts them from `getrusage`:

```console
[host]$ /usr/bin/time -v ./cpp/build/release/app --producers 1 --consumers 1 --items 100000 --cap 256
	Voluntary context switches: 302
	Involuntary context switches: 0
[host]$ /usr/bin/time -v ./cpp/build/release/app --producers 8 --consumers 8 --items 100000 --cap 8
	Voluntary context switches: 139565
	Involuntary context switches: 645
```

302 versus 139,565 — the futex-call scaling and the context-switch scaling
are two views of one fact: contention turns lock operations into kernel
sleeps. The kernel exposes the very same counters live per thread; summing
`voluntary_ctxt_switches` across `/proc/<pid>/task/*/status` mid-run on a
larger job gave 5,551 for the 1×1 shape (3 tasks) against 491,926 for the 8×8
shape (17 tasks). (`perf stat` reads `task-clock:u` freely on this host, but
the software `context-switches` event is a kernel counter gated by
`perf_event_paranoid=2`, so the `getrusage`/`/proc` route is the
unprivileged one — the same numbers without lowering the paranoia knob.)

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet view of this is bcc-tools `offcputime`, which attributes every
> thread's *off-CPU* time to the stack that blocked it, so a contended
> `workq` shows its wall-clock time piling up under `futex_wait` →
> `pthread_cond_wait` with no `strace` slowdown. That needs `CAP_BPFMON` and
> a disposable kernel, so it is not run here; the Debugging part (Part 8)
> exercises `offcputime` on the `systems-target` VM.

## What you learned

- A mutex is a **futex**: an uncontended lock is a userspace CAS with no
  syscall (4,583 futex calls for ~200,000 lock ops in the 1×1 run), and only
  a lost CAS descends into `FUTEX_WAIT`/`FUTEX_WAKE` — which is why
  contention, not locking, is what costs.
- A condition variable must wait inside a **predicate loop**, never a bare
  `if`; the kernel's `EAGAIN` value re-check and your re-test on every wake
  are the two halves of the lost-wakeup defence.
- The correct queue keeps shared writes off the hot path (per-consumer slots,
  XOR-combined after join; the only shared word is a relaxed atomic counter),
  so a mutex or channel supplies every happens-before edge the data needs.
- A data race is undefined behaviour, and the three languages meet it
  differently: ThreadSanitizer and `go -race` catch it at runtime (`consumed`
  falling to 80,629 / 95,400), while Rust's borrow checker refuses to compile
  the racing version at all — the bug the sanitizers hunt is the bug the type
  system forbids.

Next, **atomics and lock-free**: `spscring` drops the lock entirely and
coordinates a ring buffer with two atomic counters, where acquire/release
ordering — not a futex — is the whole game.

---

<p><span class="status status--verified">verified</span> — every number and
excerpt above was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, GCC 16.1.1, go1.26.5, rustc 1.97.1, strace 7.1) this session.
The runner printed <code>25-shared-state-and-the-futex  PASS  PASS  PASS</code>
(3 passed, 0 failed, 0 skipped; verify.lua 25/25 per language, Go under
<code>-race</code>); the hand run printed the
<code>checksum=42b3746c6cee7465</code> line; ThreadSanitizer reported the data
race at <code>main.cpp:209</code> with <code>consumed=80629</code>;
<code>go -race</code> reported <code>DATA RACE</code> at <code>main.go:160</code>,
<code>Found 4 data race(s)</code>, <code>consumed=95400</code>; <code>rustc
--edition 2024</code> rejected the reduced racing program with
<code>error[E0499]</code>, and the <code>--buggy</code> Rust binary printed
<code>consumed=97094/97633/97301/95632</code> across four runs; <code>strace
-c</code> counted <code>4583</code> futex calls (1796 EAGAIN) for the 1×1
cap-256 run and <code>491514</code> (124803 EAGAIN) for the 8×8 cap-8 run
(counts drift a few percent per run), with real
<code>FUTEX_WAIT_PRIVATE</code>/<code>FUTEX_WAKE_PRIVATE</code> lines
observed; and <code>/usr/bin/time -v</code> reported <code>302</code> versus
<code>139565</code> voluntary context switches, matched by
<code>voluntary_ctxt_switches</code> summed across
<code>/proc/&lt;pid&gt;/task/*/status</code> (5,551 vs 491,926 mid-run). The
<code>offcputime</code> bcc-tools callout is unverified as marked and is
exercised in Part 8.</p>
