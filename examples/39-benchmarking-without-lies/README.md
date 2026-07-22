# 39-benchmarking-without-lies

Chapter 39's example: `benchlab`, a microbenchmark harness for the chatterd
frame codec (`encode_frame`/`decode_frame`, introduced ch21) — implemented
three times, C++23, Go, and Rust, with one shared observable contract. The
point of this chapter is not the codec; it's the two different ways to
measure it, side by side in the same binary.

## The program

```
benchlab --op encode|decode|roundtrip --iters N [--warmup W]   # the real thing
benchlab --lie [--op encode|decode|roundtrip]                  # the anti-pattern
```

`--op` selects what's timed against a fixed workload — one chatterd `DELIVER`
frame (`alice\0the quick brown fox jumps over the lazy dog, three times, for
benchlab`):

- `encode` — build the frame from the payload bytes.
- `decode` — parse a pre-built frame back into `(type, payload)`.
- `roundtrip` — both, per iteration.

### The real thing

`--iters N --warmup W` (default `W=1000`) runs `W` untimed warmup iterations
— letting the allocator, branch predictor, and (on Go) the runtime reach
something like steady state — then times each of the next `N` iterations
individually with a monotonic clock, and reports:

```
$ ./demo.sh cpp run --op roundtrip --iters 100000 --warmup 5000
benchlab: op=roundtrip iters=100000 warmup=5000
benchlab: n=100000 min_ns=57 median_ns=87 p99_ns=124 max_ns=1012817
benchlab: co_p99_ns=914420 expected_interval_ns=87 co_n=113003
benchlab: checksum=000000000079c3e0
```

- `min_ns`/`median_ns`/`p99_ns`/`max_ns` — the raw latency distribution in
  nanoseconds, nearest-rank percentiles over the sorted sample.
- `co_p99_ns` — the **coordinated-omission-corrected** p99 (see below);
  always `>= p99_ns`.
- `expected_interval_ns` — the interval the correction assumes, derived from
  the run's own raw median (documented, not a hidden knob).
- `co_n` — how many samples the corrected distribution actually holds
  (`>= n`, since correction only ever adds synthetic samples).
- `checksum` — a running sum of a value derived from every call's result
  (the encoded frame's last byte, or the decoded payload's length). Two
  purposes: it stops the compiler from proving the loop body's result is
  unused and deleting it, and — because the workload is fixed — it is
  **bit-for-bit identical every run and across all three languages**
  (`encode` above: `00000000000fb388` in C++, Go, *and* Rust, for the exact
  same input). If the codec is behaving identically everywhere, the
  checksums must match; a diverging checksum is a bug, not noise.

The `min`/`median`/`p99`/`max` numbers above are one machine's numbers — like
every other latency figure in this book, their magnitude is run- and
hardware-dependent. What's invariant, and what `verify.lua` actually checks,
is the *shape*: `min <= median <= p99 <= max`, `co_p99 >= p99`, `co_n >= n`,
and a stable checksum across repeated runs of the same workload.

### Coordinated omission, and why a tight loop needs correcting for it

This harness is a **closed loop**: iteration *N+1* never starts until
iteration *N* returns. That's also the classic setup for **coordinated
omission** (Gil Tene's term): if iteration *N* stalls — a page fault, a
context switch, a GC-adjacent pause, a neighbor process getting the core —
the *one* sample recorded for it is that stall's full latency, but a
fixed-rate caller (a real client sending one request every `E` nanoseconds)
would have had several requests queue up behind that stall, each observing a
delay close to the stall's length. Recording only one slow sample instead of
several **erases exactly the tail** a benchmark is supposed to report
accurately.

The correction (HdrHistogram's `recordValueWithExpectedInterval`): for every
raw sample `v` above the expected interval `E`, backfill synthetic samples
`v - E, v - 2E, ...` down to (but not below) `E`, representing the virtual
requests that would have been queued behind the stall. `E` here is set to the
run's own raw median — i.e., "assume this loop could sustain its typical
per-op rate" — so no extra flag is needed and the two numbers come from one
run. A safety cap (`co_n <= 5,000,000`) stops a pathological single giant
stall from producing an unbounded backfill.

`co_p99_ns` in the example above (`914420`) vs. the raw `p99_ns` (`124`) is
the whole lesson: one paused iteration during a 100k-iteration run is invisible
to the raw p99 (only 1000 of 100000 samples count toward it) but dominates the
corrected one, because that single stall represented what would have been
thousands of queued requests in a real fixed-rate system.

### `--lie` — the benchmark this chapter argues against

```
$ ./demo.sh cpp run --lie --op roundtrip
benchlab: lie op=roundtrip (no warmup, single wall-clock sample, ignores variance)
benchlab: lie elapsed_ns=3069 sink=76
```

No warmup, one call, one wall-clock delta, no percentiles, no variance — and
therefore no way to tell a JIT/allocator-cold first call, a scheduler
preemption, or a page fault apart from steady-state performance. This is not
a strawman: it is the shape of `time single_call()` benchmarks that get
pasted into commit messages. `--lie`'s output never contains `p99_ns=` or
`median_ns=` — there is nothing here to compute a percentile *from* — and
`verify.lua` asserts that absence directly.

## Wire format (the codec under test, unchanged since ch21)

```
 byte:  0     1     2       3      4     5     6 ............. 6+LEN-1
      +-----+-----+-------+------+-----+-----+---------------------------+
      | 'C' | 'H' | 0x01  | TYPE |   LENGTH   |         PAYLOAD          |
      +-----+-----+-------+------+-----+-----+---------------------------+
        magic     version         u16 be (LEN)    LEN bytes
```

`benchlab` re-implements only `encode_frame`/`decode_frame` against this
format (a `DELIVER`, type `0x03`) — not the daemon; see ch21/22/27 for the
networked chatterd that actually speaks this frame over a socket. `decode`
and `roundtrip` use `std::expected<DecodedFrame, std::string>` (C++),
`Result<(u8, Vec<u8>), String>` with `?` (Rust), and a wrapped `error` via
`%w` (Go) to report a malformed frame — on this fixed, always-valid workload
that path is never taken, but it exists because a hand-rolled codec harness
should look exactly like calling code would, not a stripped-down happy path.

## Layout

```
39-benchmarking-without-lies/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI
├── cpp/         # CMake preset build, hand-rolled harness, demo.sh
├── go/          # go build, hand-rolled harness (testing.B-style loop), demo.sh
└── rust/        # cargo build, hand-rolled harness, demo.sh
```

Every language uses a small hand-rolled harness rather than a benchmarking
framework (Google Benchmark / `go test -bench` / criterion): `benchlab` is a
CLI that prints one shared, cross-language table shape, which a framework's
own report format would not give us for free, and there is no third-party
dependency to pin or fetch for any of the three languages — `rust/Cargo.lock`
carries no external crates.

## The demo contract

Every language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary
  (e.g. `./demo.sh run --op decode --iters 100000 --warmup 5000`)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

This is a host-only example — pure in-process computation, no sockets, no VM.
Exit codes: `0` a run completed and printed its table (or its lie), `1` the
codec broke on its own fixed workload (a bug, never expected here), `2` a
usage error (missing/unknown flags, or `--warmup >= --iters`).

## Verification

`verify.lua` runs from this directory with `LSP_LANG` set to one language at a
time and asserts, per language:

1. for `encode`, `decode`, and `roundtrip` at `--iters 20000 --warmup 1000`:
   the header line echoes `op`/`iters`/`warmup`; `n` equals the requested
   `iters`; `min_ns <= median_ns <= p99_ns <= max_ns`; `co_p99_ns >= p99_ns`;
   `co_n >= n`; `expected_interval_ns` matches the documented derivation
   (the run's own raw median, floored at 1); and a 64-bit hex checksum is
   printed;
2. running the identical fixed workload twice yields the identical checksum
   both times — the codec's output is deterministic even though the ns
   timings are not;
3. `--lie` exits 0, identifies itself as the lie, reports exactly one
   `elapsed_ns` sample, and contains **no** `p99_ns=`/`median_ns=` anywhere
   in its output;
4. no flags, an unknown `--op`, and `--warmup >= --iters` each exit 2 with a
   usage line.

Verified on the Fedora 44 reference host (kernel 7.1.3-200.fc44, GCC 16.1.1,
go1.26.5, rustc 1.97.1): all three languages PASS all 42 checks, repeatably
across multiple runs.
