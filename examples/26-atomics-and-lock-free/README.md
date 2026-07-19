# 26-atomics-and-lock-free

Chapter 26's example: `spscring`, a single-producer/single-consumer lock-free
ring buffer implemented three times — C++23, Go, and Rust — each with that
language's atomics, but one shared observable contract so a single `verify.lua`
covers all three.

## The program

```
spscring --capacity K --items N [--pad on|off]
```

A **producer** thread pushes `N` `u64` values (`0, 1, ..., N-1`) through a
bounded ring of `K` slots; the **consumer** (the main thread) pops each value
and sums it. There is no mutex and no lock: the ring is coordinated by two
`u64` atomic counters, `head` (advanced by the consumer) and `tail` (advanced
by the producer), using acquire/release ordering — Lamport's classic SPSC
queue. When the producer has finished all `N` pushes and the consumer all `N`
pops, the program prints exactly one line:

```
spscring: items=1000000 sum=499999500000 throughput_mops=37.29 pad=off
```

- `items=<N>` — echoes the requested count.
- `sum=<s>` — the consumer's running total. Because the values are `0..N-1`,
  a correct run **always** yields `s = N·(N-1)/2`. Any dropped, duplicated, or
  torn hand-off changes this number, so the sum is a concurrency oracle.
- `throughput_mops=<m>` — millions of pushes per second, `N / wall_seconds / 1e6`,
  printed with two decimals. Its magnitude is machine- and run-dependent; the
  book uses it only to *narrate* the false-sharing effect, and `verify.lua`
  only asserts that it parses as a positive number.
- `pad=<on|off>` — echoes the `--pad` mode.

The three binaries produce the same line shape and the same `items`/`sum`/`pad`
fields for the same arguments.

**`--pad on|off`** toggles cache-line padding between `head` and `tail`
(default `off`):

- `off` — the two counters sit adjacent in one 64-byte cache line. The
  producer's `store(tail)` and the consumer's `store(head)` then invalidate
  each other's copy of the shared line on every hand-off — **false sharing** —
  even though neither thread touches the other's counter.
- `on` — each counter is `alignas(64)` / `#[repr(align(64))]` / padded onto its
  own cache line, so the two cores stop fighting over one line.

Both modes compute the same, correct sum; the throughput difference between
them is the chapter's whole point, not a hard assertion (on a lightly loaded
box a single 1M-item run is noisy — the book measures the gap with repeats).

## The ring protocol (identical across languages)

`head` and `tail` are **monotonic** `u64` counters that only ever increase;
the physical slot for logical position `p` is `buf[p % K]`.

```
push(v):                             pop() -> v:
  t = tail (private to producer)       h = head (private to consumer)
  while t - head.load(Acquire) == K      while h == tail.load(Acquire)
      spin      # ring full                  spin      # ring empty
  buf[t % K] = v                         v = buf[h % K]
  tail.store(t+1, Release)               head.store(h+1, Release)
```

- The producer owns `tail` and only *reads* `head`; the consumer owns `head`
  and only *reads* `tail`. That single-writer-per-counter rule is what makes
  the queue safe without a lock.
- `tail.store(Release)` **publishes** the slot write that precedes it;
  `tail.load(Acquire)` in the consumer **observes** it, establishing a
  happens-before edge so the consumer's read of `buf[h % K]` is not a data
  race. `head` carries the symmetric edge that lets the producer safely reuse
  a slot only after the consumer has drained it.
- Ring occupancy is `tail - head`; **full** is `tail - head == K`, **empty**
  is `head == tail`. Using monotonic counters (rather than wrapped indices)
  makes those two states unambiguous without a spare slot.

## Three atomics toolkits, one contract

| | index atomics | memory ordering | padding knob |
|---|---|---|---|
| **C++23** | `std::atomic<uint64_t>` head/tail in a `Control` struct; producer is a `std::jthread`, consumer is `main` | explicit `memory_order_acquire` on the loads, `memory_order_release` on the stores; the plain `std::vector<uint64_t>` slots ride the release/acquire edge | `Control<true>` gives head and tail each `alignas(64)`; `Control<false>` packs them adjacent |
| **Go** | `atomic.Uint64` head/tail; producer goroutine, consumer on `main`, joined over a `chan struct{}` | **no per-op order argument** — Go's `sync/atomic` is sequentially consistent, and the memory model's "an atomic write is synchronized-before the read that observes it" *is* the acquire/release edge, just not relaxable | `ctrlPadded` inserts `[56]byte` after each counter; `ctrlPacked` leaves them adjacent |
| **Rust** | `AtomicU64` head/tail shared across a `std::thread::scope`; slots are `Vec<AtomicU64>` too | explicit `Ordering::{Acquire, Release}` on head/tail, `Ordering::Relaxed` on the slots — so the data race is ruled out by the type system, no `unsafe` | `CtrlPadded` wraps each counter in `#[repr(align(64))]`; `CtrlPacked` leaves them adjacent |

**What Go gives you (and doesn't):** C++ and Rust let you name the exact
ordering per operation (`acquire`, `release`, `relaxed`) and could, in
principle, relax the slot accesses further. Go's `sync/atomic` exposes only
sequentially-consistent `Load`/`Store`/`Add` — you cannot ask for a weaker
order, but you also cannot get it wrong: every atomic access already carries
the happens-before edge this queue needs. The Go build runs under `-race`
(see `go/demo.sh`); the detector understands the atomic edge and stays silent
on the plain `buf[]` slot accesses, proving the lock-free hand-off is
race-free rather than merely lucky.

## Layout

```
26-atomics-and-lock-free/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI
├── cpp/         # CMake preset build (GCC or clang), demo.sh
├── go/          # go build -race (toolchain go1.26.5), demo.sh
└── rust/        # cargo build (rustup pin 1.97.1; std only), demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary
  (e.g. `./demo.sh run --capacity 1024 --items 1000000 --pad on`)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

This is a host-only example — pure threads and atomics, no sockets, no VM.

## Verification

`verify.lua` runs from this directory with `LSP_LANG` set to one language at a
time and asserts, per language:

1. `--capacity 1024 --items 1000000 --pad off` exits 0 and prints
   `items=1000000 sum=499999500000 throughput_mops=<float> pad=off` — the sum
   is the exact triangular number `N·(N-1)/2`, so every one of the million
   hand-offs delivered its value once;
2. the same with `--pad on` yields the identical sum with `pad=on`;
3. `--capacity 1 --items 1000` (a one-slot ring — maximum head/tail
   contention) still sums to exactly `499500`;
4. the `throughput_mops` field parses as a positive number;
5. no arguments prints `usage: spscring --capacity K --items N [--pad on|off]`
   on stderr and exits 2.

Verified on the Fedora 44 reference host (kernel 7.1.3-200.fc44): all three
languages PASS all 11 checks, Go under the race detector.
