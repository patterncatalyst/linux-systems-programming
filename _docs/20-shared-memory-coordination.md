---
title: "Shared-memory coordination: futexes and seqlocks"
order: 20
part: "IPC"
description: "The coordination ladder from a 1 ms poll loop through POSIX message queues to the futex — the kernel's one real primitive, sleep/wake on a shared word — plus a seqlock for read-mostly snapshots, process-shared atomics in all three languages, and a measured IPC latency matrix, as shmkv v1 gains watchers that never poll."
duration: 60 minutes
---

Chapter 16 (mmap and shared mappings) left `shmkv` in an awkward state: two
processes share a page of key-value slots, but a reader who wants to *notice*
a change has no option except reading the page again, and again, and again.
This chapter fixes that, and the one new idea is smaller than you expect: for
coordination between processes, the kernel provides essentially **one
primitive** — `futex(2)`, "sleep until this 32-bit word changes; wake the
sleepers queued on it." Everything you have ever called a mutex, condition
variable, semaphore, or `WaitGroup` is userspace protocol wrapped around that
word. `shmkv` v1 builds the protocol in the open: a **seqlock** makes reads of
the shared page consistent without ever blocking the writer, and a shared
futex turns "something changed" from a question you keep asking into an event
you sleep through. Along the way we climb the whole coordination ladder —
polling, POSIX message queues, futex — and measure every rung.

The code is in `examples/20-shared-memory-coordination/`. `./demo.sh` builds
all three implementations and runs the serve/watch/bench demo; the `README.md`
there pins the SHKV2 page layout, the CLI, and the exit codes all three
languages share.

{% include excalidraw.html
   file="20-futex-wait-wake"
   alt="Three lanes: watcher process, kernel, writer process. The watcher's seqlock read finds nothing new and calls futex FUTEX_WAIT on word value 3; the kernel re-checks the word equals 3 and sleeps the task on a wait queue keyed by inode and offset 12. The writer publishes k4 under the seqlock bumping seq 6 to 7 to 8, stores futex word 4 with release ordering, and calls FUTEX_WAKE INT_MAX which returns 1; an amber arrow marks the watcher runnable again, and it back-fills ids from the 8-slot ring. A dashed note explains the EAGAIN lost-wakeup guard."
   caption="Figure 20.1 — one shmkv wake end to end: the watcher parks on the mapped word, the writer publishes and wakes, the kernel's wait queue keyed by (inode, offset) connects them; every syscall in this figure appears verbatim in the strace cross-check below" %}

> **Tools used** — `strace`, `xxd`, `perf`, `/usr/bin/time`, `awk` (over
> `/proc/<pid>/status`), `gcc` (for the comparison harness), `python3` (the
> example runner) — all host; `offcputime`/`futexctn` (bcc-tools, lab VM,
> exercised in Part 8). Everything host-side is checked by
> `scripts/check-host.sh` or ships with Fedora.

## The coordination ladder

Suppose process A has changed shared memory and process B needs to know. The
options form a ladder, and this chapter's `bench` subcommand times every rung.

**Rung 0: poll.** B re-reads the page in a loop, sleeping between looks so it
does not burn a CPU. Simple, portable — and the sleep *is* the latency. Our
poll channel deliberately sleeps 1 ms *before* checking, so by construction
every observation costs at least one full nap; the measured p50 of 1,056 µs is
the sleep plus scheduler overhead, and no tuning removes the trade-off: a
shorter nap trades latency for wasted wakeups, and every nap is a voluntary
context switch whether or not anything happened.

**Rung 1: a kernel object with a queue.** POSIX message queues
(`mq_open(2)`, `mq_timedsend(2)`, `mq_timedreceive(2)`) give you a named
kernel channel with priorities and blocking receive. They carry *data*, not
just "something happened" — our bench sends the 8-byte update id through the
queue. The cost is a syscall on every send and every receive, and a second
copy of information the shared page already holds.

**Rung 2: the futex.** `futex(addr, FUTEX_WAIT, expected, timeout)` says:
*if* the 32-bit word at `addr` still holds `expected`, put me to sleep on a
kernel wait queue attached to that address; `futex(addr, FUTEX_WAKE, n)`
wakes up to `n` sleepers. The check-then-sleep is atomic against wakers —
that is the entire reason the primitive exists. If the word already moved,
`FUTEX_WAIT` returns `EAGAIN` immediately and the caller re-reads shared
state: the lost-wakeup race that plagues homemade "check a flag, then sleep"
schemes is closed *inside* the kernel. This is the C++ spelling, from
`examples/20-shared-memory-coordination/cpp/src/main.cpp`:

```cpp
void futex_wait(std::uint32_t* addr, std::uint32_t expected,
                std::chrono::milliseconds timeout) {
  timespec ts{.tv_sec = timeout.count() / 1000,
              .tv_nsec = (timeout.count() % 1000) * 1'000'000};
  // EAGAIN (word already moved on), ETIMEDOUT and EINTR are all normal here:
  // the caller re-checks shared state and decides whether to wait again.
  ::syscall(SYS_futex, addr, FUTEX_WAIT, expected, &ts, nullptr, 0);
}
```

Two details are load-bearing. There is no glibc wrapper — the raw
`syscall(SYS_futex, …)` *is* the interface, in all three languages. And the
call deliberately omits `FUTEX_PRIVATE_FLAG`: a private futex is keyed by
process-local virtual address, which is faster but invisible to other
processes; a **shared** futex is keyed by `(inode, offset)` of the mapped
file, so two unrelated processes that `mmap` the same `/tmp/kv.shm` sleep and
wake on the same kernel queue even though the page sits at different virtual
addresses in each. The fast path costs *zero* syscalls: an uncontended check
is just an atomic load, and only actual sleeping or waking enters the kernel.
That is why glibc builds every pthread mutex and condvar on this exact call.

## A seqlock for read-mostly data

The futex says *when* to look; the seqlock makes what you see *consistent*.
`shmkv`'s page has one writer and many readers, and the readers vastly
outnumber the writes — the classic seqlock shape (the kernel uses one for
`gettimeofday`'s clock data). The writer's side is four lines:

```cpp
template <typename Mutate>
void seqlock_write(const Shm& shm, Mutate&& mutate) {
  auto seq = shm.seq();
  seq.fetch_add(1, std::memory_order_relaxed);  // odd: writer active
  std::atomic_thread_fence(std::memory_order_release);
  mutate();
  seq.fetch_add(1, std::memory_order_release);  // even: consistent again
}
```

The writer bumps the sequence word **twice** per update, and both bumps are
essential. The first makes the word *odd* — a flag meaning "mutation in
progress; anything you copy now may be torn," so readers that see an odd
value do not even bother copying. The second makes it even again — but at a
*different* even value than before. That difference is the whole detection
scheme: a reader records the word before copying and compares it after; if a
writer ran at any point in between, the word has moved (odd mid-flight, or a
larger even value), the copies cannot be trusted, and the reader throws them
away and retries. Bump once instead of twice and a reader that started just
before the writer could see the same value on both sides of a torn copy. The
fences pair up across processes: the writer's release fence ensures the slot
bytes are visible before the closing bump, and the reader's acquire fence
ensures its copies happened before it re-reads the word. Writers never wait,
never take a lock, never care how many readers exist — readers pay with
occasional retries, which is exactly the right trade for read-mostly data.
The retry loop is bounded at 10⁶ attempts so a corrupt file (a stuck-odd word
from a crashed writer) produces `seqlock read livelocked` instead of a hang.

## Process-shared atomics, three ways

All of this only works if atomic operations on *mapped* memory are really
atomic, and each language has its own rules for saying so. In C++, a
`std::atomic<T>` must be constructed, but our words live in a file another
process formatted — so `Shm` binds `std::atomic_ref` onto the raw bytes:

```cpp
  std::atomic_ref<std::uint32_t> seq() const {
    return std::atomic_ref{*reinterpret_cast<std::uint32_t*>(base() + kOffSeq)};
  }
```

`atomic_ref` (C++20) is precisely "atomic operations on an object I do not
own": the contract is that the referenced `uint32_t` is properly aligned (our
header offsets 8, 12, 16 guarantee it on a page-aligned mapping) and that no
plain, non-atomic access touches it while any `atomic_ref` does. Rust's
equivalent is `AtomicU32::from_ptr`, and its safety argument is an aliasing
argument: `&AtomicU32` is a shared reference to an `UnsafeCell`-backed type,
which is the *only* kind of shared reference Rust permits to memory that
mutates underneath you. Casting `*mut u8` into `&u32` and racing it would be
undefined behavior; casting to `&AtomicU32` is the sanctioned spelling,
provided the pointer is aligned, live for the lifetime, and every access to
those four bytes goes through atomics. Go has no blessed story at all:
`sync/atomic` on a `*uint32` pointing into an `unix.Mmap` slice works —
since Go 1.19 the memory model documents atomics as sequentially consistent,
they compile to the same `lock`-prefixed instructions, the mapping is outside
the GC-managed heap so nothing moves it — but *you* carry the alignment
burden (64-bit atomics fault on unaligned addresses on some platforms; our
counter at offset 16 of a page is safe), and nothing stops ordinary code
from racing the same bytes non-atomically. The comment block at the top of
`examples/20-shared-memory-coordination/go/main.go` spells the caveats out.

> **Sidebar: where is eventfd?** `eventfd(2)` is a lighter kernel counter
> that chapter 9's event loops already waited on, and it measures well (2 µs
> p50 on this host, same harness). But an eventfd is named by *file
> descriptor*, not by anything in the shared page — to coordinate two
> unrelated processes with one you must inherit it across `fork` or pass it
> through a UNIX socket with `SCM_RIGHTS`, exactly the machinery chapter 19
> (UNIX sockets and fd passing) built for pmon's `logfd`. The futex needs
> none of that: its rendezvous *is* the mapped file. That is the deep
> difference — fd-based channels need an fd-passing bootstrap; a shared
> mapping is its own bootstrap.

## How the code works

One page carries everything. The SHKV2 layout — magic at offset 0, the
seqlock `u32` at 8, the futex `u32` at 12, a `u64` update counter at 16, and
eight 64-byte slots (24-byte key, 40-byte value, NUL-padded) from offset 64 —
is deliberately two counters, not one. The `u64` is the truth: total updates
ever published, compared and back-filled from. The `u32` at offset 12 exists
because `FUTEX_WAIT` can only watch 32 bits; it holds the low half of the
counter and is *only* used as the thing to sleep on. The slots form a ring
keyed by `(id-1) % 8`, which buys slack: a watcher that sleeps through a
burst can reconstruct up to eight missed updates from the ring instead of
only ever seeing the latest. `Shm` owns the fd and the mapping (RAII `Fd` +
`Mapping` in C++, `Drop` impls around `OwnedFd` and a munmap guard in Rust,
`defer`-driven `close` in Go) and hands out the three header words through
the per-language atomic views described above.

The publish path in `cmd_serve` is the writer's half of Figure 20.1: under
`seqlock_write`, store the slot bytes and the `u64` counter; after the
closing bump, store the `u32` futex word with release ordering; then
`FUTEX_WAKE` with `INT_MAX` — wake *every* watcher, since any number of
processes may be parked. The order matters: data first (under the seqlock),
then the word, then the wake, so a woken watcher is guaranteed to find the
word already changed and the data already consistent.

The watch loop is the reader's half, and its shape is the condition-variable
pattern with the pieces visible. Take a seqlock snapshot; if the counter
advanced, print every missed id from the ring and loop. Only when there is
nothing new does it arm the sleep:

```cpp
    std::uint32_t w = shm->futex_word().load(std::memory_order_acquire);
    if (w != static_cast<std::uint32_t>(last)) continue;  // moved on: re-read now
    futex_wait(shm->futex_ptr(), w, 2000ms);
```

Read the guard carefully: it re-checks the word *after* deciding the snapshot
was stale, and passes the exact observed value as `expected`. If a publish
lands between those two lines, the kernel's own compare fails, `FUTEX_WAIT`
returns `EAGAIN`, and the loop re-reads — a wake can be early, late, or
spurious, and the loop is correct anyway, because sleeping is only ever an
optimization over re-checking. The 2-second timeout is the same humility
applied to crashes: a dead writer costs a watcher at most 2 s per lap until
its own 30 s deadline fails the run.

The reading half of the seqlock is the protocol walked in every language:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
std::expected<Snapshot, std::string> seqlock_read(const Shm& shm) {
  auto seq = shm.seq();
  for (int attempt = 0; attempt < 1'000'000; ++attempt) {
    std::uint32_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1) {  // writer mid-update: retry
      std::this_thread::yield();
      continue;
    }
    Snapshot snap;
    snap.counter = shm.counter().load(std::memory_order_relaxed);
    std::memcpy(snap.slots.data(), shm.base() + kOffSlots, kSlotsBytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) == s1) return snap;
  }
  return std::unexpected(std::string("seqlock read livelocked"));
}
```

```go
func seqlockRead(s *shm) (snapshot, error) {
	for attempt := 0; attempt < 1_000_000; attempt++ {
		s1 := atomic.LoadUint32(s.seq())
		if s1&1 != 0 { // writer mid-update: retry
			runtime.Gosched()
			continue
		}
		var snap snapshot
		snap.counter = atomic.LoadUint64(s.counter())
		copy(snap.slots[:], s.m[offSlots:offSlots+slotsBytes])
		if atomic.LoadUint32(s.seq()) == s1 {
			return snap, nil
		}
	}
	return snapshot{}, fmt.Errorf("seqlock read livelocked")
}
```

```rust
fn seqlock_read(shm: &Shm) -> Result<Snapshot> {
    for _ in 0..1_000_000 {
        let s1 = shm.seq().load(Ordering::Acquire);
        if s1 & 1 != 0 {
            std::thread::yield_now(); // writer mid-update: retry
            continue;
        }
        let mut snap = Snapshot { counter: shm.counter().load(Ordering::Relaxed), slots: [0; SLOTS_BYTES] };
        unsafe {
            ptr::copy_nonoverlapping(shm.slots(), snap.slots.as_mut_ptr(), SLOTS_BYTES);
        }
        fence(Ordering::Acquire);
        if shm.seq().load(Ordering::Relaxed) == s1 {
            return Ok(snap);
        }
    }
    bail!("seqlock read livelocked")
}
```

Same protocol, three idioms: acquire-load the word, refuse odd, copy the
counter and all 512 slot bytes with a *plain* copy that is allowed to race,
fence, re-check. Go leans on its sequentially-consistent atomics instead of
explicit fences and yields to the goroutine scheduler rather than the OS.
The copy being torn sometimes is not a bug — it is the design; the re-check
is what makes it safe to have raced.

`cmd_bench` is the measurement harness for the ladder. A consumer thread
(`std::jthread`, a goroutine, a `thread::scope` spawn) blocks on one of the
three channels; the producer publishes and — this is the part that keeps the
numbers meaningful — **spins until the consumer acknowledges round `i-1`
before sending round `i`**, through an `ack` atomic. Without that
serialization, a fast producer queues ahead and you measure queue depth, not
wakeup latency. The mq channel carries the id through a real POSIX queue
(created `O_EXCL`, unlinked by RAII/defer on every path — `verify.lua`
checks `/dev/mqueue` is clean afterwards); Go builds `mq_open`,
`mq_timedsend`, `mq_timedreceive`, and `mq_unlink` from raw `Syscall6`
because glibc's wrappers live in librt — and the kernel, it turns out, wants
the queue name *without* the leading slash glibc requires, which the strace
below confirms.

Fragile bits, stated plainly: one writer per file is an unchecked assumption
(two servers would interleave seqlock bumps and corrupt each other — a
`flock` would fix it and chapter 25's futex-built locks are the deeper
answer); the futex word is 32 bits, so a watcher blind for exactly 2³²
updates would miss a wake until the 2 s timeout catches it; the ring
back-fills at most 8 ids, and a watcher asleep for more sees only the latest
8; and the bench's `p50_us=0` for futex is truncation, not magic — the
serialized handshake means the consumer often observes the word before it
ever reaches the syscall, so the fast path is a sub-microsecond atomic load.

## Errors, three ways

The shared contract is exit 2 for usage, exit 1 with a `shmkv:`-prefixed
line on stderr for runtime failures — and unlike fwatch's byte-identical
diagnostics, this contract deliberately pins only the prefix and the codes,
so each language's errno formatting shows through: C++ formats
`strerror(errno)` into `std::expected`'s error string (`shmkv: open
/tmp/nope.shm: No such file or directory`), Go's wrapped `%w` chain
lower-cases it (`…: no such file or directory`), and Rust's
`anyhow::Context` appends its own tail (`…: No such file or directory (os
error 2)`). All three were exercised this session and all exited 1. Domain
errors the kernel cannot detect get one shared spelling: a file that exists
but lacks the magic prints `bad magic (want SHKV2)` identically everywhere,
and the futex-specific errnos — `EAGAIN`, `ETIMEDOUT`, `EINTR` — are not
errors at all here; every implementation swallows them and re-reads shared
state, because the loop, not the syscall, owns correctness.

## Concurrency lens

Everything in this chapter is the concurrency lens, but the Go caveat
deserves its own light. `FUTEX_WAIT` is a blocking syscall: it parks the
calling **OS thread** in the kernel, where the goroutine scheduler cannot
reach it. The runtime's sysmon notices the stall and hands the thread's P to
another thread, so the program keeps running — but every concurrent futex
waiter pins one kernel thread for its whole sleep. One watcher per process is
fine; a thousand goroutines each parked in `FUTEX_WAIT` would be a thousand
OS threads, which is precisely the degenerate case channels and the
netpoller exist to avoid. The comment at the top of `go/main.go` documents
this trade so the pattern is not copied blindly. The second thing worth
naming: the memory-ordering vocabulary of chapter 6 — acquire, release,
fences — crossed a process boundary in this chapter without changing at all.
Cache coherence does not know what a process is; orderings pair between any
two threads that touch the same physical page, whichever address spaces they
live in. That is why `seqlock_write`'s release fence in a Go server orders
writes that a Rust watcher's acquire fence observes.

## Build, run, observe

```bash
[host]$ cd examples/20-shared-memory-coordination && ./demo.sh
```

Cross-language is the point, so drive it by hand with a Go writer and a Rust
watcher on one file:

```bash
[host]$ ./go/bin/app serve /tmp/kv.shm --updates 6 --interval-ms 100 &
[host]$ ./rust/target/release/app watch /tmp/kv.shm --events 6
watch: file=/tmp/kv.shm events=6
observed update 1: k1=value-1
observed update 2: k2=value-2
observed update 3: k3=value-3
observed update 4: k4=value-4
observed update 5: k5=value-5
observed update 6: k6=value-6
watch: complete events=6
```

The contract is the page plus the futex, not the runtime — the Go process's
`FUTEX_WAKE` lands in the Rust process's `FUTEX_WAIT`. Afterwards the file
itself reads out the whole protocol:

```bash
[host]$ xxd -l 96 /tmp/kv.shm
00000000: 5348 4b56 3200 0000 0c00 0000 0600 0000  SHKV2...........
00000010: 0600 0000 0000 0000 0000 0000 0000 0000  ................
00000020: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000030: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000040: 6b31 0000 0000 0000 0000 0000 0000 0000  k1..............
00000050: 0000 0000 0000 0000 7661 6c75 652d 3100  ........value-1.
```

Little-endian, offset 8: the seqlock word is `0x0c` = 12 — exactly two bumps
for each of six publishes, resting even. Offset 12: futex word 6. Offset 16:
`u64` counter 6. Offset 64: slot 0 still holds `k1`/`value-1`, id 1 having
landed at `(1-1) % 8`. Then the ladder, measured — 2,000 serialized rounds
per channel, C++ bench:

```bash
[host]$ for ch in futex mq poll; do ./cpp/build/release/app bench /tmp/b.shm --rounds 2000 --channel $ch; done
bench: channel=futex rounds=2000 p50_us=0 p99_us=4
bench: channel=mq rounds=2000 p50_us=2 p99_us=14
bench: channel=poll rounds=2000 p50_us=1056 p99_us=1083
```

The Go and Rust benches bracket the same values (Go: 0/4, 3/6, 1003/1021;
Rust: 0/0, 2/5, 1061/1153 µs). Folding in the fd-based channels from
chapters 18 and 19 — pipe, UNIX stream socketpair, and the eventfd from the
sidebar, re-measured this session with the identical serialized-round
harness rebuilt around `read(2)` — gives the matrix:

| Channel | Kernel object | p50 (µs) | p99 (µs) | Waiter blocks in | Chapter |
|---|---|---|---|---|---|
| futex wake | wait queue on (inode, offset) | <1 | 4 | `futex(FUTEX_WAIT)` | this one |
| pipe | pipe buffer | 2 | 5 | `read(2)` | 18 |
| eventfd | 64-bit counter | 2 | 6 | `read(2)` | 9, 19 |
| POSIX mq | message queue | 2 | 14 | `mq_timedreceive(2)` | this one |
| UNIX socket | socket buffer | 3 | 12 | `read(2)` | 19 |
| 1 ms poll loop | none | 1,056 | 1,083 | `nanosleep(2)` | rung 0 |

{% include excalidraw.html
   file="20-ipc-matrix"
   alt="A ladder of six boxes ordered left to right by latency: futex wake p50 under 1 microsecond p99 4, eventfd 2 and 6, pipe 2 and 5, POSIX mq 2 and 14, UNIX socket 3 and 12, then a dark box for the 1 millisecond poll loop at p50 1056 and p99 1083 microseconds with an amber arrow labeled roughly 350x. Below, two summary boxes: the scheduler evidence of 8 versus 3774 voluntary context switches in a 4 second window, and rusage counts of 120 versus 2002 voluntary switches over 2000 rounds."
   caption="Figure 20.2 — the coordination ladder as measured on this host: every blocking channel lands within one order of magnitude, and the poll loop pays its nap" %}

Two takeaways, plainly bounded by this host (an idle 8-core/16-thread Fedora
44 laptop; treat the rankings as portable and the absolutes as local). The
fast one: *every* blocking channel — futex, pipe, eventfd, mq, UDS — lands
in single-digit microseconds at p50; within that band, choose by shape
(named vs fd-based, data-carrying vs pure signal), not speed, though the
futex is alone in costing zero syscalls when nothing is contended. The slow
one: the poll loop is ~350× worse at p50 than the slowest blocking channel
and over 1,000× worse than the futex, and it cannot be fixed, because its
latency is its sleep — the only knob trades latency against wasted wakeups.

## Cross-check, three ways

**The syscalls, under `strace`.** The watcher, filtered to `futex` (glibc's
own private-futex noise trimmed), during a three-update serve:

```bash
[host]$ strace -f -e trace=futex -o watch.trace ./cpp/build/release/app watch /tmp/kv.shm --events 3
[host]$ grep -v PRIVATE watch.trace
2622774 futex(0x7f7e9662600c, FUTEX_WAIT, 0, {tv_sec=2, tv_nsec=0}) = 0
2622774 futex(0x7f7e9662600c, FUTEX_WAIT, 1, {tv_sec=2, tv_nsec=0}) = 0
2622774 futex(0x7f7e9662600c, FUTEX_WAIT, 2, {tv_sec=2, tv_nsec=0}) = 0
2622774 +++ exited with 0 +++
```

The address ends in `0x00c` — offset 12 in the mapped page, exactly where
the layout puts the futex word — the op is plain `FUTEX_WAIT` (shared, no
`_PRIVATE`), and `expected` climbs 0, 1, 2 as each observed update re-arms
the next sleep. The serve side, traced in a separate run, is the mirror
image: `futex(0x7f79491d300c, FUTEX_WAKE, 2147483647) = 1` three times — wake
`INT_MAX`, one waiter actually woken. And the mq bench under
`-e trace=mq_open,mq_timedsend,mq_timedreceive,mq_unlink`:

```bash
2623348 mq_open("shmkv-bench-2623348", O_RDWR|O_CREAT|O_EXCL, 0600, {mq_flags=0, mq_maxmsg=8, mq_msgsize=8, mq_curmsgs=0}) = 4
2623348 mq_timedsend(4, "\1\0\0\0\0\0\0\0", 8, 0, {tv_sec=1784433492, tv_nsec=616289183}) = 0
2623349 mq_timedreceive(4, "\1\0\0\0\0\0\0\0", 8, NULL, {tv_sec=1784433492, tv_nsec=616316355}) = 8
...
2623348 mq_unlink("shmkv-bench-2623348") = 0
```

The kernel-level name really has no leading slash — the Go implementation's
raw-syscall comment, confirmed against the C++ binary going through glibc.
Producer tid 2623348 sends, consumer tid 2623349 receives the same 8 bytes,
and the unlink runs on exit. (That traced 5-round run reported
`p50_us=19` — tracing overhead is real; never quote numbers from under
`strace`.)

**The scheduler's ledger, in `/proc`.** `voluntary_ctxt_switches` counts how
often a task *chose* to sleep. A watcher parked on the futex through a slow
serve (10 updates, 500 ms apart), sampled 4 s apart:

```bash
[host]$ grep voluntary_ctxt_switches /proc/<watch-pid>/status   # then again after 4 s
voluntary_ctxt_switches:	2
voluntary_ctxt_switches:	10
```

Eight sleeps in four seconds — one per update observed, nothing else. The
poll-channel bench over the same window (summing all threads via
`/proc/<pid>/task/*/status`): 285 → 4,059, i.e. **3,774** voluntary switches
in 4 s, ~943/s — the 1 ms nap plus overhead, matching the measured 1,056 µs
p50 almost exactly. Whole-process rusage agrees: `/usr/bin/time -v` on 2,000
rounds reports 120 voluntary switches for the futex channel (the serialized
handshake usually publishes before the consumer reaches the syscall — the
p50 = 0 fast path, counted) against 2,002 for poll: one nap per round, plus
startup, by construction. `perf stat -e context-switches` was exercised on
both runs but on this host counted zero: stock Fedora ships
`kernel.perf_event_paranoid = 2`, which restricts unprivileged perf to
user-space counting, and context switches happen in the kernel — the
elapsed-time line still tells the story (0.004 s futex vs 2.125 s poll for
the same 2,000 rounds), and the chapter 28 debugging toolbelt revisits perf
with `CAP_PERFMON`.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the eBPF view of this chapter is `offcputime` proving the watcher's stack
> is parked in `futex_wait_queue` (not spinning) and `futexctn` summarizing
> contention fleet-wide; bcc-tools need privileges this host's lane does not
> use, and chapter 30 (eBPF observation tools) exercises exactly that on the
> `systems-target` VM.

## What you learned

- The kernel's one coordination primitive is the futex — atomically
  check-then-sleep on a 32-bit word, wake its queue — and a *shared* futex
  keyed by `(inode, offset)` lets unrelated processes rendezvous through a
  mapped file with zero syscalls on the uncontended path.
- A seqlock suits read-mostly shared data: the writer bumps the sequence word
  twice (odd = in progress, and the closing bump makes any overlap
  detectable), readers copy optimistically and retry on mismatch — torn
  copies are designed-for, discarded, and bounded.
- Process-shared atomics have per-language rules: `std::atomic_ref` and
  `AtomicU32::from_ptr` bind atomic operations (and, in Rust, the aliasing
  contract) onto mapped words you do not own; Go's `sync/atomic` works on
  mmapped memory but leaves alignment to you, and `FUTEX_WAIT` parks a whole
  OS thread per waiting goroutine.
- Measured on this host, every blocking IPC channel notifies in single-digit
  microseconds at p50 while a 1 ms poll loop pays three orders of magnitude
  more — and `/proc`'s `voluntary_ctxt_switches` (8 vs 3,774 in the same
  4 s) is the scheduler's own testimony as to why.

Next, **TCP servers**: the socket leaves the machine, and listen/accept,
backlogs, and `SO_REUSEPORT` decide who gets the connection.

---

<p><span class="status status--verified">verified</span> — every number and
excerpt above was produced on the Fedora 44 reference host this session: the
runner printed <code>20-shared-memory-coordination  PASS  PASS  PASS</code>
(3 passed, 0 failed, 0 skipped); the Go-serve/Rust-watch transcript, the
<code>xxd</code> dump (seq 12, futex word 6, counter 6), the bench rows for
all three languages, and the pipe/UDS/eventfd comparison rows all come from
runs performed now; the strace excerpts are real trimmed output (shared
<code>FUTEX_WAIT</code> at offset <code>0xc</code> with expected 0/1/2,
<code>FUTEX_WAKE</code> INT_MAX returning 1, slashless kernel mq names,
<code>mq_unlink</code> on exit); the context-switch evidence (watch 2→10,
poll 285→4,059 over 4 s; rusage 120 vs 2,002) was sampled live; and the
<code>perf_event_paranoid = 2</code> limitation was observed, not assumed.
The bcc-tools callout is unverified as marked, and all latency absolutes are
plainly bounded by this host.</p>
