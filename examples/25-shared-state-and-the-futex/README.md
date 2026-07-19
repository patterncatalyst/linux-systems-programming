# 25-shared-state-and-the-futex

Chapter 25's example: `workq`, a bounded **MPMC job queue + worker pool**
implemented three times — C++23, Go, and Rust — with each language's native
shared-state toolkit, but one observable contract so a single `verify.lua`
covers all three. The queue's blocking is exactly the mutex/condvar dance the
kernel implements with **futexes**; `strace`-ing any of these binaries shows
`futex(2)` calls under load.

## The program

```
workq --producers P --consumers C --items N [--buggy] [--seed S] [--cap K]
```

`P` producer threads push `N` items **total** into a bounded blocking queue of
capacity `K` (default 256); `C` consumer threads pop items and fold each item's
payload into a running checksum. When every item has been produced and drained,
the program prints one line:

```
workq: produced=<N> consumed=<N> checksum=<hex> ms=<t>
```

- `produced` / `consumed` are exact counts (both equal `N` on the correct path).
- `checksum` is a 16-digit zero-padded lowercase hex `u64`.
- `ms` is wall-clock milliseconds (timing — the only non-deterministic field).

Defaults: `--seed 0x0123456789ABCDEF`, `--cap 256`. `--producers`,
`--consumers`, and `--items` are required.

**Errors** (all to stderr, exit 2, always followed by the usage line):
missing a required flag → usage only; `--foo` → `workq: unknown flag: --foo`;
a flag with no value → `workq: --producers needs a value`; a non-integer value
→ `workq: not an integer: x`; `P<1`, `C<1`, `N<0`, or `K<1` → a bounds message.

## The value contract (identical across languages and runs)

Each item index `i ∈ [0, N)` has a payload that is a **pure function** of
`(seed, i)` — a splitmix64 finalizer over `seed + (i+1)·GOLDEN`, all in wrapping
`u64` arithmetic:

```
GOLDEN = 0x9E3779B97F4A7C15   MIX1 = 0xBF58476D1CE4E5B9   MIX2 = 0x94D049BB133111EB

payload(seed, i):
    x = seed + (i+1)*GOLDEN        # wrapping u64
    x = (x ^ (x >> 30)) * MIX1     # wrapping
    x = (x ^ (x >> 27)) * MIX2     # wrapping
    x =  x ^ (x >> 31)
    return x
```

Producer `p` owns the indices `{ i : i mod P == p }`, so the **set** of items is
`{0..N-1}` for any `P`. The checksum folds payloads with **XOR**, which is
commutative and associative, so the result is independent of how items are
split among producers or the order consumers drain them. For a fixed
`(seed, N)` the checksum is therefore one fixed constant — byte-identical in
C++, Go, and Rust:

| seed | N | checksum |
|---|---|---|
| `0x0123456789ABCDEF` (default) | 8 | `697cd06b54533772` |
| `0x0123456789ABCDEF` (default) | 100000 | `42b3746c6cee7465` |
| `42` | 50000 | `fceb5608a4b73dc0` |
| any | 0 | `0000000000000000` |

Real output (default seed, all three binaries print the same line):

```
$ ./demo.sh cpp  run --producers 4 --consumers 4 --items 100000
workq: produced=100000 consumed=100000 checksum=42b3746c6cee7465 ms=49
$ ./demo.sh go   run --producers 1 --consumers 8 --items 100000 --cap 4
workq: produced=100000 consumed=100000 checksum=42b3746c6cee7465 ms=102
$ ./demo.sh rust run --producers 8 --consumers 3 --items 100000 --cap 1024
workq: produced=100000 consumed=100000 checksum=42b3746c6cee7465 ms=62
```

## Three shared-state shapes, one contract

| | the bounded queue | the checksum accumulator |
|---|---|---|
| **C++23** | `std::mutex` + two `std::condition_variable`s (`not_full` / `not_empty`) guarding a `std::deque`; `std::jthread` producers/consumers, `std::atomic<long>` produced counter | per-consumer local fold, XOR-combined after join |
| **Go** | a buffered `chan uint64` (capacity `K`) as the blocking queue; a closer goroutine `sync.WaitGroup.Wait()`s the producers then `close()`s the channel, ending every consumer's `range` | per-consumer local fold, XOR-combined after the consumer `WaitGroup` |
| **Rust** | `Mutex<VecDeque<u64>>` + two `Condvar`s inside `std::thread::scope`; no `Arc` — the scope lets the threads borrow the queue directly | per-consumer `(count, sum)` returned from `ScopedJoinHandle::join`, XOR-combined |

None of these needs an atomic on the hot path except the produced counter — the
mutex/condvar (or channel) already establishes the happens-before edges.

## The `--buggy` demo (a real data race — chapter material, not verified)

`--buggy` keeps the same correct, synchronized queue but routes **every**
consumer into a single **unsynchronized** shared counter + checksum. That is a
genuine data race; `verify.lua` never runs it, but the chapter does:

- **Go** builds with `-race` (see `go/demo.sh`), so `--buggy` prints
  `WARNING: DATA RACE` and ends with `Found N data race(s)` and a nonzero exit.
  The corrupted `consumed` comes out below `N` (lost updates), e.g. `94730`.
- **C++** has a ThreadSanitizer preset: `cmake --preset tsan && cmake --build
  --preset tsan`, then `./build/tsan/app ... --buggy` reports
  `WARNING: ThreadSanitizer: data race` at the two shared-write lines.
- **Rust** won't let you write this in safe code at all — the buggy path needs
  an `unsafe` `*mut` shared across threads. Run it and the borrow checker's
  point is made empirically: `consumed` and `checksum` come out different every
  time (`consumed=97282`, `95773`, …), never the correct `100000` /
  `42b3746c6cee7465`.

## Layout

```
25-shared-state-and-the-futex/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check (correct path only)
├── cpp/         # CMake presets (release / tsan / asan); std::mutex+condvar, jthread
├── go/          # go build -race (toolchain go1.26.5); buffered channel queue
└── rust/        # cargo build (rustup pin 1.97.1); Mutex+Condvar, thread::scope
```

## The demo contract

Every language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

The top-level `demo.sh` dispatches: `./demo.sh cpp run …` execs `cpp/demo.sh
run …`; bare `./demo.sh` (or `all`) builds and runs all three.

## Verification

`verify.lua` runs from this directory with `LSP_LANG` set to one language at a
time and asserts, per language (25 checks each):

1. the error contract — no args / unknown flag / missing required flag / bad
   integer / value-less flag all print the usage line and exit 2;
2. a fixed run (`P=4 C=4 N=100000`, default seed) reports
   `produced=100000 consumed=100000` and the shared checksum constant
   `42b3746c6cee7465`;
3. determinism — three more shapes with different `P`, `C`, and `--cap` over the
   same `(N, seed)` all yield that same constant with `produced==consumed==N`;
4. seed `42` (`N=50000`) yields its own shared constant `fceb5608a4b73dc0`, and
   `N=0` drains to `produced=0 consumed=0 checksum=0000000000000000`.

The `--buggy` race is a sanitizer/`-race` chapter demo and is deliberately
outside the passing verify.

Verified on the Fedora 44 reference host (kernel 7.1.3-200.fc44, GCC 16.1.1,
go1.26.5, rustc 1.97.1): all three languages PASS 25/25, Go under `-race`.
