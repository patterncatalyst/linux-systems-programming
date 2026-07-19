---
title: "Atomics and lock-free: memory ordering, the SPSC ring, and false sharing"
order: 26
part: "Concurrency in Depth"
description: "spscring drops the lock entirely: a single-producer/single-consumer ring coordinated by two atomic counters, with acquire/release explained on the real head/tail, why a non-atomic ring tears, the ABA problem SPSC sidesteps, and false sharing measured with perf — a padded line beats an unpadded one by ~34% on this host."
duration: "55 minutes"
---

Every coordination primitive so far in this book has ultimately been a *lock*:
`parhash` handed work across a mutex-guarded queue, and `shmkv` reached all
the way down to the futex the mutex is built from. A lock is a promise that
only one thread runs the critical section at a time — correct, general, and
never free. This chapter removes it. `spscring` is a bounded ring buffer with
exactly one producer thread and one consumer thread, and it moves ten million
values between them with no mutex, no futex, and no `unsafe` data race — only
two atomic counters and a careful choice of *memory ordering*. That last
phrase is the one new idea: on a modern multicore CPU the order in which one
thread's writes become visible to another is not "the order you wrote them,"
and the whole discipline of lock-free code is naming exactly how much ordering
you need and paying for no more.

The code is in `examples/26-atomics-and-lock-free/`. `./demo.sh` builds all
three implementations; its `README.md` specifies the CLI, the one-line output,
and the exit codes all three languages share.

{% include excalidraw.html
   file="26-spsc-ring"
   alt="Two thread boxes over a shared-ring band. The producer on core 0 owns tail and reads head, does buf[t%K]=v then tail.store(t+1, Release); the consumer on core 2 owns head and reads tail, does v=buf[h%K] then head.store(h+1, Release). Two accent boxes in the middle are the tail atomic (producer-only writer) and head atomic (consumer-only writer). Amber arrows show each thread's Release store to its own counter and Acquire load of the other's; a dashed amber arrow from tail down into buf[0] is labelled tail Release publishes buf[t%K]. A ring of six slots buf[0..5] shows buf[0]=3 and buf[1]=4 filled, the rest free; a note reads occupancy = tail - head = 2, full when tail - head == K, empty when head == tail."
   caption="Figure 26.1 — the Lamport SPSC ring: each counter has exactly one writer, and the Release store on that counter publishes the slot the other thread is about to read" %}

> **Tools used** — `perf stat`, `perf c2c`, `objdump`, `go tool objdump`,
> `taskset`, `python3` (host). `perf`/`objdump`/`taskset` are in
> `scripts/check-host.sh`; `perf c2c` needs root and is deferred to the lab VM
> (Part 8).

## Memory ordering, on the ring's own counters

The ring is two monotonic `u64` counters. `tail` counts values the producer
has pushed; `head` counts values the consumer has popped. The physical slot
for logical position `p` is `buf[p % K]`, occupancy is `tail - head`, the ring
is **full** at `tail - head == K` and **empty** at `head == tail`. The single
rule that makes this safe without a lock is *one writer per counter*: the
producer writes `tail` and only reads `head`; the consumer writes `head` and
only reads `tail`. No location is ever written by two threads, so there is no
write-write race to arbitrate — which is exactly what a lock would have
arbitrated.

What is left is a *publication* problem, and it is subtler than it looks.
When the producer runs `buf[t % cap] = i` and then `tail.store(t + 1)`, it has
performed two writes on core 0. The consumer, spinning on core 2 until
`tail` moves, will read the slot the instant it sees the new `tail`. For that
read to be valid, the slot write must be visible *before* the counter write —
and nothing about program order guarantees that across cores. Both the
compiler and the CPU are free to reorder independent stores; core 2 may
observe the new `tail` while the slot still holds stale bytes. That is a
*torn* hand-off, and a naive ring built on plain `uint64_t` counters exhibits
it: the sum comes out wrong, sporadically, on exactly the runs you did not
test. Atomics fix it not merely by being indivisible but by carrying an
**ordering**:

- **Release** on a store says "every write I did before this one is visible to
  any thread that acquires this value." `tail.store(t + 1, release)` publishes
  the slot write that precedes it.
- **Acquire** on a load says "every write the releasing thread did before its
  release is now visible to me." `tail.load(acquire)` in the consumer observes
  the slot. Together they form a *happens-before* edge across the two cores.
- **Relaxed** promises atomicity and nothing about ordering. Rust marks the
  slot accesses `Ordering::Relaxed` precisely because they need no edge of
  their own — the `tail` release/acquire pair already covers them.
- **Sequential consistency** is the strongest: one global total order over all
  such operations. It is the *only* ordering Go's `sync/atomic` exposes, and it
  is strictly more than this ring needs.

{% include excalidraw.html
   file="26-memory-ordering"
   alt="Three stacked bands, each a memory ordering paired with its x86 code generation. Top, sequential consistency (Go's only setting): tail.Store(t) compiles to XCHGQ, implicitly locked, a full fence — the store pays for global order. Middle, acquire/release (C++ and Rust head/tail): a Release store to Acquire load edge compiles to a plain MOV, no lock and no fence, because x86 TSO already orders store-to-store and load-to-load. Bottom, relaxed (Rust ring slots): no cross-thread ordering promise, buf[t%K].store(v, Relaxed), compiles to the same plain MOV as release, safe only because tail's edge covers it. A footnote says on x86 the ordering is nearly free — the difference is the promise, not the instruction; on ARM or POWER the movs would need real barriers."
   caption="Figure 26.2 — the same hand-off under three orderings and what each becomes on x86-64; the instructions are from the objdump in the cross-check" %}

Here is the caution the brief demands: **do not read this ring's `Relaxed`
slots as a portability claim.** They are correct here because the acquire/release
edge on `tail` synchronises them, and because the single-writer rule means no
slot is ever concurrently written. Change the shape — more producers, a slot
read-modified in place — and `Relaxed` is a bug. Relaxed orderings are a
scalpel for a proof you have already written, not a default.

## Why SPSC, and the ABA problem it sidesteps

The reason this ring can be so simple is that it is *single*-producer,
*single*-consumer. The famous hazard of lock-free stacks and queues is **ABA**:
a thread reads a pointer value `A`, is preempted, and by the time it runs its
compare-and-swap the location has gone `A → B → A` — the CAS succeeds on a
value that is numerically identical but semantically stale, and the structure
corrupts. ABA is a property of designs that *compare-and-swap a reused value*.
`spscring` never does. Its counters are **monotonic** — they only ever
increase, for the life of the run — so no `tail` or `head` value ever recurs,
and there is no CAS at all: each counter has one writer that simply `store`s
the next number. Using unbounded monotonic counters rather than wrapped
indices is also what makes full-versus-empty unambiguous without wasting a
slot. SPSC is the foundation the rest of lock-free programming builds on
precisely because it makes ABA structurally impossible; the moment you add a
second producer you inherit CAS, ABA, and the tag-counter or hazard-pointer
machinery that fights it.

## How the code works

The heart of all three programs is the same two loops — a producer that spins
while the ring is full and a consumer that spins while it is empty — and the
ordering annotations are the whole story. The excerpts below are verbatim; read
the C++ and Rust ordering arguments against Go's conspicuous absence of them.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    // Producer: push 0..items-1. jthread joins on scope exit.
    std::jthread producer([&] {
        std::uint64_t t = 0;
        for (std::uint64_t i = 0; i < items; ++i) {
            // Wait until the ring has a free slot (consumer advanced head).
            while (t - head.load(std::memory_order_acquire) == cap) {
                // spin
            }
            buf[t % cap] = i;
            ++t;
            tail.store(t, std::memory_order_release);
        }
    });

    // Consumer runs on this thread.
    std::uint64_t sum = 0;
    std::uint64_t h = 0;
    for (std::uint64_t i = 0; i < items; ++i) {
        while (h == tail.load(std::memory_order_acquire)) {
            // spin: ring empty
        }
        sum += buf[h % cap];
        ++h;
        head.store(h, std::memory_order_release);
    }
```

```go
	done := make(chan struct{})
	go func() { // producer
		var t uint64
		for i := uint64(0); i < items; i++ {
			for t-head.Load() == capacity { // ring full: spin
			}
			buf[t%capacity] = i
			t++
			tail.Store(t)
		}
		close(done)
	}()

	// consumer (main goroutine)
	var h uint64
	for i := uint64(0); i < items; i++ {
		for h == tail.Load() { // ring empty: spin
		}
		sum += buf[h%capacity]
		h++
		head.Store(h)
	}

	<-done
```

```rust
    let sum = thread::scope(|s| {
        // Producer: push 0..items-1.
        s.spawn(move || {
            let mut t: u64 = 0;
            for i in 0..items {
                while t - head.load(Ordering::Acquire) == cap {
                    std::hint::spin_loop();
                }
                buf[(t % cap) as usize].store(i, Ordering::Relaxed);
                t += 1;
                tail.store(t, Ordering::Release);
            }
        });

        // Consumer on this thread.
        let mut h: u64 = 0;
        let mut sum: u64 = 0;
        for _ in 0..items {
            while h == tail.load(Ordering::Acquire) {
                std::hint::spin_loop();
            }
            sum = sum.wrapping_add(buf[(h % cap) as usize].load(Ordering::Relaxed));
            h += 1;
            head.store(h, Ordering::Release);
        }
        sum
    });
```

Three things distinguish the spellings. C++ names the ordering on every atomic
operation (`memory_order_acquire` on the loads, `memory_order_release` on the
stores) and leaves the slot a plain `std::vector<uint64_t>` — the release/acquire
edge is what makes the plain slot write race-free. Rust makes the slot itself
an `AtomicU64` accessed `Ordering::Relaxed`, so the absence of a data race is
proven *by the type system* rather than asserted; the `spin_loop()` hint emits
the CPU's `PAUSE` instruction so a spinning core does not starve its
hyperthread sibling. Go carries **no ordering argument at all**: its
`sync/atomic` `Load`/`Store` are sequentially consistent, full stop. You cannot
relax them, but you also cannot get them wrong — the Go memory model guarantees
that an atomic write is *synchronized-before* the read that observes it, which
is exactly the acquire/release edge this ring needs, wearing a stronger
guarantee than it strictly requires. The Go binary is built and shipped under
the race detector (`go/demo.sh` passes `-race`); the detector understands the
atomic edge and stays silent on the plain `buf[]` slot writes, which is direct
evidence that the lock-free hand-off is race-free rather than merely lucky.

The `sum` field is a correctness oracle. The pushed values are `0 .. N-1`, so a
run that delivers every value exactly once must total `N·(N-1)/2`. For ten
million items that is `49999995000000`; any dropped, duplicated, or torn
hand-off would move it. It is checked on every run below.

## Errors, three ways

`spscring` has a deliberately tiny failure surface — it is compute, not I/O —
and it is all in argument parsing. A missing `--capacity` or `--items`, a
non-numeric value, a zero capacity, or an unknown `--pad` word all resolve to
one behaviour, identical across the three languages: print
`usage: spscring --capacity K --items N [--pad on|off]` to stderr and exit **2**.
C++ threads the fallible parse through `std::optional<Args>` and returns the
usage line on `nullopt`; Go returns `(args, false)` and `os.Exit(2)`; Rust
returns `Result<Args, ()>` and `ExitCode::from(2)`. A successful run prints its
one line and exits **0**. There is no exit-1 runtime-error path, because once
the arguments are valid the program cannot fail — there is no file, socket, or
allocation that can refuse it beyond the initial `Vec`/`vector` reservation.
`verify.lua` asserts all of this: 15 checks per language, including that the
one-slot ring (`--capacity 1`, maximum head/tail contention) still sums
correctly, and that the no-argument invocation produces the exact usage string
and exit 2.

## Concurrency lens

This is the most concurrent program in the book and the one with the least
synchronisation machinery: two threads, zero locks, two atomics. The safety
argument is entirely the single-writer rule plus the release/acquire edge, and
it is worth stating what would break each way. Drop the atomicity and the
counter loads can tear on a 32-bit split (not on this 64-bit host, but the type
is doing real work on others). Drop the *ordering* — use relaxed on `tail` —
and the slot publication edge vanishes: the consumer can read a slot the
producer has not finished writing, and the sum corrupts. Add a second producer
and the single-writer rule collapses, taking the whole lock-free proof with it.
The spin loops are the price of going lock-free: a blocked consumer *burns* its
core rather than sleeping, which is the right trade only when the queue is
rarely empty and latency matters more than a spare core. A production ring pairs
this fast path with a futex or `condvar` slow path for when it idles — which is
exactly the chapter-20 futex, reappearing as the fallback beneath the atomics
rather than the mechanism on top of them.

## Cache lines and false sharing

The `--pad` flag exposes a hardware effect that no amount of correct ordering
addresses. Caches move memory in 64-byte **lines**, and coherency is tracked
per line, not per variable. When `head` and `tail` sit in the *same* line — the
default, `--pad off` — the consumer's `head.store` invalidates the producer's
cached copy of the whole line, `tail` included, even though the producer never
touched `head`. On the next iteration the producer must re-fetch a line it
already had, and the line ping-pongs between the two cores on every hand-off.
This is **false sharing**: two threads contending for one cache line while
touching logically disjoint data. `--pad on` places each counter on its own
line and the contention disappears.

{% include excalidraw.html
   file="26-false-sharing"
   alt="Two panels. Left, pad=off: cores 0 and 2 both point with bidirectional amber invalidate arrows at one cache line #1 holding head and tail adjacent; annotated L1 load-misses about 128 million, median 4.64 Mops at capacity 1 with 10 million items. Right, pad=on: core 0 points locally down to line #1 holding head plus 56 bytes of padding, core 2 points locally to line #2 holding tail plus 56 bytes; annotated L1 load-misses about 108 million, median 6.20 Mops, about 34 percent faster."
   caption="Figure 26.3 — false sharing measured: one shared line ping-pongs between cores (left), two padded lines each stay core-local (right); the miss counts and throughputs are the perf numbers from the cross-check" %}

The C++ control block makes the choice a type:

```cpp
template <>
struct Control<true> {
    alignas(64) std::atomic<std::uint64_t> head{0};
    alignas(64) std::atomic<std::uint64_t> tail{0};
};

template <>
struct Control<false> {
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
};
```

Go inserts `[56]byte` after each counter; Rust wraps each in
`#[repr(align(64))]`. Same bytes, same effect, three spellings.

## Build, run, observe

```bash
[host]$ cd examples/26-atomics-and-lock-free && ./demo.sh build
```

The runner (`python3 scripts/test-all-examples.py --only
26-atomics-and-lock-free`) reports `PASS PASS PASS` for cpp, go, rust — 15
behavioural checks per language, Go under `-race`. Driven by hand on this host,
pinned to two separate physical cores with `taskset` so the producer and
consumer land on cores 0 and 2:

```console
[host]$ taskset -c 0,2 ./cpp/build/release/app --capacity 1 --items 10000000 --pad off
spscring: items=10000000 sum=49999995000000 throughput_mops=5.51 pad=off
[host]$ taskset -c 0,2 ./cpp/build/release/app --capacity 1 --items 10000000 --pad on
spscring: items=10000000 sum=49999995000000 throughput_mops=6.42 pad=on
```

Both modes produce the identical, correct `sum=49999995000000`; only the
throughput differs. A one-slot ring forces a lockstep hand-off — every push
waits for the previous pop — which is the worst case for false sharing and so
the clearest place to measure it. Across eleven runs each, the median was
**4.64 Mops** padded off versus **6.20 Mops** padded on: padding buys roughly
**34%** on this Tiger Lake host. (At `--capacity 1024` the hand-off decouples,
throughput jumps into the tens of Mops, and the powersave governor's frequency
swings swamp the effect — the small ring is the reliable measurement surface, so
that is where the chapter takes its numbers.)

## Cross-check: perf counts the false sharing, objdump shows the ordering

Two independent tools confirm the two claims. First, `perf stat` counts the
cache traffic directly. Same binary, same pinning, `--capacity 1 --items
10000000`:

```console
[host]$ taskset -c 0,2 perf stat -e cache-misses,L1-dcache-load-misses ./cpp/build/release/app --capacity 1 --items 10000000 --pad off
spscring: items=10000000 sum=49999995000000 throughput_mops=5.51 pad=off
            12,493      cache-misses:u
       128,086,713      L1-dcache-load-misses:u
       1.817560572 seconds time elapsed
[host]$ taskset -c 0,2 perf stat -e cache-misses,L1-dcache-load-misses ./cpp/build/release/app --capacity 1 --items 10000000 --pad on
spscring: items=10000000 sum=49999995000000 throughput_mops=6.42 pad=on
            10,456      cache-misses:u
       107,745,083      L1-dcache-load-misses:u
       1.558742655 seconds time elapsed
```

The signature is in `L1-dcache-load-misses`: **~128 million** unpadded versus
**~108 million** padded, a ~20-million-miss gap that is the ring re-fetching a
line the other core just invalidated. Each spinning load on the shared line
misses in L1 because the line keeps bouncing away; separating the counters lets
each core keep its own line warm. The medians over eleven runs held the same
direction (~127 M vs ~108 M). That extra miss stream is the entire mechanism
behind the throughput gap the previous section measured.

Second, `objdump` settles what the three orderings actually *cost* on x86. The
C++ producer's release store and its acquire load of `head` compile to plain
`mov` (trimmed to the loop body):

```console
[host]$ objdump -d --no-show-raw-insn cpp/build/release/app | grep -A30 'run_benchILb1EE.*_M_runEv>:'
  4017c0:	mov    0x10(%rdi),%rax
  4017c4:	mov    (%rax),%rdx
  ...
  4017e8:	mov    %rsi,(%rax,%rdx,8)
  4017f0:	add    $0x1,%rsi
  4017f4:	mov    %rsi,(%rax)
```

`4017c4` is `head.load(Acquire)`, `4017e8` is `buf[t%cap] = i`, and `4017f4` is
`tail.store(t, Release)` — three plain `mov`s, no `lock` prefix and no `mfence`
anywhere in the function (a `grep -c` for those over the whole producer symbol
returns 0). On x86-64's Total Store Order model, every load already has acquire
semantics and every store already has release semantics, so acquire/release is
*free* — the ordering lives in the promise, not the instruction. Rust's
`tail.load(Acquire)` in the consumer is likewise a bare `mov 0x0(%rbp),%rax`
right after the `pause`. Go's sequentially-consistent store is the contrast
that proves the point. Rebuilt without `-race` so the atomic inlines rather than
calling the detector's shim, the same `tail.Store(t)` is not a `mov`:

```console
[host]$ go build -o /tmp/app-norace ./... && go tool objdump -s 'main\.bench' /tmp/app-norace | grep -iE 'XCHG|main.go:135'
  main.go:135		XCHGQ R11, 0(R8)
```

`XCHGQ` is implicitly `lock`ed and carries a full barrier — the price of
sequential consistency's global order, paid on every push. That single
instruction is why "you cannot get Go's atomics wrong" and "Go's atomics are
the strongest" are the same sentence: seq-cst asks for more ordering than this
ring needs, and on x86 the store, not the load, is where you feel it.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> `perf c2c`, which samples HITM (hit-modified) events to name the exact cache
> line and the two instructions trading it, needs kernel profiling this host's
> `perf_event_paranoid=2` refuses without root; Part 8 runs it on
> `systems-target` to pin the false-sharing line to the `Control<false>` struct
> address directly, alongside `offcputime` confirming the spin loops are on-CPU
> (never sleeping, so nothing to see) — the flip side of the lock this chapter
> removed.

## What you learned

- A lock-free SPSC ring is two monotonic atomic counters with one writer each;
  the safety comes from that single-writer rule plus a Release-store /
  Acquire-load edge on each counter that *publishes* the slot write before the
  other thread reads it. A non-atomic ring tears because plain stores may become
  visible out of order across cores.
- Memory ordering is a spectrum: sequential consistency (Go's only setting) >
  acquire/release (C++ and Rust head/tail) > relaxed (Rust slots). On x86 TSO,
  acquire/release and relaxed are both plain `mov` while seq-cst's store is a
  locked `XCHGQ` — but that instruction equality is *not* portability: on ARM
  the same relaxed slot would need a real barrier, so relaxed is a scalpel for a
  proof you have written, never a default.
- SPSC sidesteps ABA structurally: monotonic counters never recur and there is
  no compare-and-swap, so the reused-value hazard that plagues multi-producer
  lock-free queues cannot arise here.
- False sharing is a cache-line effect, not a correctness bug: `head` and `tail`
  in one 64-byte line ping-pong between cores, and `perf stat` shows it as
  ~128 M vs ~108 M `L1-dcache-load-misses` — worth a real ~34% here, fixed by
  `alignas(64)` / `#[repr(align(64))]` / `[56]byte` padding.

Next, **async runtimes**: the same epoll and futex machinery re-composed as
C++ coroutines, the Go scheduler, and tokio — and when plain threads still win.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, 11th-gen Intel i7-11800H, 16 CPUs, powersave governor) this
session: the runner printed <code>26-atomics-and-lock-free  PASS  PASS
PASS</code> (3 passed, 0 failed, 0 skipped; verify.lua 15 checks per language,
Go under <code>-race</code>); the hand-driven <code>taskset -c 0,2</code> runs
printed the exact <code>sum=49999995000000</code> for both <code>pad=off</code>
(throughput 5.51) and <code>pad=on</code> (6.42), and eleven-run batches gave
medians 4.64 vs 6.20 Mops; <code>perf stat</code> reported
<code>128,086,713</code> vs <code>107,745,083</code> <code>L1-dcache-load-misses:u</code>
(medians ~127 M vs ~108 M); <code>objdump</code> showed the C++ release store
and acquire load as plain <code>mov</code> (<code>4017f4: mov %rsi,(%rax)</code>)
with no <code>lock</code>/<code>mfence</code>, Rust's acquire load as a plain
<code>mov</code> after <code>pause</code>, and <code>go tool objdump</code> on a
non-<code>-race</code> build showed <code>tail.Store</code> as
<code>XCHGQ R11, 0(R8)</code>. The <code>perf c2c</code> HITM analysis and
<code>offcputime</code> are unverified as marked — <code>perf c2c</code> failed
with <code>perf_event_paranoid=2</code> here and is exercised on the lab VM in
Part 8.</p>
