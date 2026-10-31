# 40-low-latency-fastpath: chatterd-fastpath, an echo server with nothing left to remove

`chatterd-fastpath` takes the echo hot path from `chatterd` (the book's chat
server) and strips it to three moves: pin the process to one CPU, reuse a
single stack buffer for the life of a connection, and, optionally, spin on a
non-blocking `recv` instead of sleeping in a blocking `read`. A `naive` server
in the same binary does the opposite of each move, so the two can be raced
under identical placement and the gap between them measured.

```
[host]$ cd examples/40-low-latency-fastpath && ./demo.sh build
```

The interesting run happens on the lab guest, where `run-latency-bench.sh`
brings up both servers and reads their pinning back from the kernel before
timing them:

```
[vm]$ bash run-latency-bench.sh ./app 20000 500
app: naive-cpus-allowed 0-1
app: fastpath-cpus-allowed 1
app: percentiles_ns tag=naive-1 p50=11957 p90=15325 p99=28509 p99.9=80747 min=10075 max=442827 mean=12975.38 n=20000
app: percentiles_ns tag=fastpath-1 p50=9282 p90=11225 p99=16626 p99.9=47039 min=6900 max=87199 mean=9607.65 n=20000
app: percentiles_ns tag=naive-2 p50=11135 ... n=20000
app: percentiles_ns tag=fastpath-2 p50=7936 ... n=20000
app: percentiles_ns tag=naive-3 p50=11320 ... n=20000
app: percentiles_ns tag=fastpath-3 p50=8184 ... n=20000
```

`naive-cpus-allowed` comes back `0-1`, the full online set, because the naive
server never touches its own affinity mask. `fastpath-cpus-allowed` comes back
`1`, a single CPU, because `sched_setaffinity(2)` narrowed it. Both lines are
read from `/proc/<pid>/status` by the driver script, not printed by the
program itself.

## The frame and the three subcommands

One binary, three subcommands, one fixed 64-byte frame:

```
app fastpath --port P --pin CPU [--busy-poll]
app naive --port P
app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]
```

`fastpath` pins itself to `CPU` with `sched_setaffinity(2)`, reuses one
stack-allocated buffer for every message, and with `--busy-poll` spins a
non-blocking `recv(2)` instead of blocking on `read(2)`. `naive` is the
baseline chatterd hot loop would run: default scheduling, a fresh heap buffer
per message, a blocking `read(2)`. `measure` connects to either server and
runs `N` synchronous ping-pong round trips, never more than one frame in
flight, timing each against `CLOCK_MONOTONIC` and printing a
coordinated-omission-corrected `p50`/`p90`/`p99`/`p99.9` table in nanoseconds.
`W` warmup round trips (500 by default under the bench driver) are discarded
before recording starts.

The wire frame is this example's own format, a smaller, fixed-size cousin of
the chatterd chat frame from the TCP chapter:

```
+-------+---------+------+-----------+---------------+---------+
| magic | version | type | seq (u64) | send_ns (u64) | pad     |
| 2B    | 1B      | 1B   | 8B BE     | 8B BE         | 44B     |
+-------+---------+------+-----------+---------------+---------+
```

Magic is `CF` (chatterd-fastpath). The server never inspects `seq` or
`send_ns`: it reads 64 bytes and writes the identical 64 bytes back. All
timing happens on the client's own clock; `seq` is only echoed back so
`measure` can confirm it got its own frame back rather than another
connection's or garbage, a correctness check, not a timing mechanism.

## Three moves that shrink the tail

Each move removes one specific, nameable source of tail latency, and each has
a real cost:

- **Pin to one CPU** (`sched_setaffinity(2)`). An unpinned thread can migrate
  between messages onto a core that needs to come back from a low C-state and
  holds none of the working set in cache, and that migration is itself a
  tail-latency event. Pinning trades the freedom to flee a noisy neighbor for
  the removal of migration as a source of jitter.
- **Reuse one buffer.** `fastpath` reads into a single 64-byte stack array for
  the whole life of a connection; `naive` allocates a fresh heap buffer
  (`make_unique` in C++, `make([]byte, …)` in Go, `vec![0u8; …]` in Rust) on
  every message and lets it fall out of scope. At 64 bytes the allocator cost
  is small but variable: most calls hit a thread-local cache, the occasional
  one grabs the arena lock or faults in a page, and that variance lands in the
  tail.
- **Busy-poll** (`--busy-poll`). A blocking `read(2)` on an idle socket puts
  the thread to sleep; waking it for the next frame costs a scheduler round
  trip, a voluntary context switch, on every message. Busy-poll never sleeps:
  it spins on a non-blocking `recv` that returns `EAGAIN` until data arrives,
  burning the whole pinned core to keep the thread on-CPU and ready. It is the
  most expensive move, a dedicated core, and the one with the largest effect
  on median latency.

## Proving pinning and busy-poll from the kernel, not the program

Both claims are checked against `/proc/<pid>/status`, never against anything
the server prints about itself.

`Cpus_allowed_list` proves the pin: if `sched_setaffinity` had silently
failed, `fastpath-cpus-allowed` would read `0-1` like the naive server instead
of a single CPU.

`voluntary_ctxt_switches` proves busy-poll never sleeps. Running a
20,000-message `measure` against each server, both pinned to CPU 1 with
`taskset`, and reading the counter before and after:

```
[vm]$ # naive (blocking read)
before: voluntary_ctxt_switches: 2
after:  voluntary_ctxt_switches: 20500

[vm]$ # fastpath (busy-poll)
before: voluntary_ctxt_switches: 2
after:  voluntary_ctxt_switches: 3
```

The blocking server takes one voluntary context switch per message, 20,498 of
them to serve 20,000 requests plus warmup. The busy-poll server sleeps once
for the whole run. On this run the naive `p50` was 11,426 ns and the fastpath
`p50` was 8,148 ns; to a first approximation, the ~3,300 ns gap is the
sleep/wake round trip through the scheduler that busy-poll declined to take
20,000 times.

## The CPU count that lies about a `taskset` mask

`pin_to_cpu`'s range check needs a CPU count, and the correct count is the
number of CPUs online system-wide, because the pin argument is an absolute CPU
index. C++ and Rust both call `sysconf(_SC_NPROCESSORS_ONLN)`, which returns
that number (2 on the lab guest) and is blind to the caller's own affinity
mask. Go's obvious equivalent, `runtime.NumCPU()`, is not the same thing: it
returns the size of the process's CPU affinity mask instead. The two numbers
agree on an idle machine and diverge the moment something narrows the mask,
which is exactly what `run-latency-bench.sh` does by launching each server
under `taskset -c 1`. Under that mask, `runtime.NumCPU()` returns `1`, and a
naive port's own range check would reject the `--pin 1` it was told to use:

```
app: error: cpu 1 out of range (0..0)
```

The fix reads the kernel's own account of the online set instead:
`onlineCPUs()` in the Go port parses `/sys/devices/system/cpu/online` and
falls back to `runtime.NumCPU()` only if that read fails, matching the
`sysconf` semantics the other two languages get automatically. The Go port also
carries two lines neither C++ nor Rust needs: `sched_setaffinity(2)` acts on a
thread, and a goroutine is not a thread, so `runtime.LockOSThread()` pins the
hot loop's goroutine to one OS thread and `GOMAXPROCS(1)` keeps the runtime
from parking other work on other cores before the affinity call means
anything. The affinity call itself runs twice, once against the calling
thread (which restricts the loop) and once against the process (the mask
`/proc/<pid>/status` reports back).

## Shutdown: three mechanisms, one observable outcome

The hot loop is single-threaded by design in all three languages; the
interesting concurrency is in shutdown. C++ and Rust install a `SIGINT`
handler with `sa_flags` explicitly **not** `SA_RESTART`, so a blocking
`read(2)` or `poll(2)` already in flight returns `EINTR` instead of resuming,
the loop sees a stop flag, and it exits. Rust reaches for this directly
through `nix`-owned raw file descriptors, because `std`'s higher-level
wrappers (`TcpListener::accept`, `Read::read_exact`) retry `EINTR`
internally.

Go cannot use that technique: its runtime installs its own signal handlers
with `SA_RESTART`, and a "blocking" read on a `net.Conn` parks the goroutine
on an `epoll` registration rather than sitting in a syscall a signal could
interrupt. So the Go port reaches the same outcome, a clean
`app: … shutting down` message and exit 0, by a different route: its signal
handler sets a stop flag and closes the listener and the live connection.
Closing an fd is what wakes a poller-parked read. The busy-poll path needs one
extra step, since its socket is a `dup`'d raw fd outside the poller:
`trigger()` also calls `shutdown(SHUT_RDWR)` on it to break the spin.

## Errors, three ways, same bytes

A usage error (no subcommand, an unknown one, `fastpath` without `--pin`,
`naive` without `--port`, `measure` without `--n`) prints a three-line banner
to stderr and exits `2`:

```
usage: app fastpath --port P --pin CPU [--busy-poll]
       app naive --port P
       app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]
```

A runtime error exits `1` with an `app: error: …` message: `cpu 99 out of
range (0..1)` from the pin range check, `connect 127.0.0.1:1: Connection
refused` when `measure` can't reach a server, `notanip: not an IPv4 address`
when the target host isn't a dotted quad (the servers call `inet_pton`, not a
DNS lookup, so a hostname is a deliberate error), and
`measure: connection closed at iteration N` if a server dies mid-run. C++
returns `std::expected<T, std::string>`, Go returns `error` with the
`strerror` text unwrapped so the message reads like C's, and Rust returns
`Result<T, String>` pre-formatted at the failure site; the bytes on stderr are
identical across all three, which `verify.lua` asserts directly.

## Layout

```
40-low-latency-fastpath/
├── demo.sh               # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── run-latency-bench.sh  # staged to the lab guest and run there; the real benchmark
├── verify.lua            # behavioral checks, driven against the lab guest
├── cpp/                  # CMake preset build, src/main.cpp, demo.sh
├── go/                   # go build, main.go, demo.sh
└── rust/                 # cargo build, src/main.rs, demo.sh
```

## The demo contract

`./demo.sh [cpp|go|rust] build` builds one language; `./demo.sh build` builds
all three. `./demo.sh [cpp|go|rust] run [args]` runs the built binary
directly; bare `./demo.sh` builds and runs all three with no arguments, which
only exercises the usage banner, since none of the three subcommands has a
sensible zero-argument default. The real measurement lives in
`run-latency-bench.sh`, which this example is a **VM example** for: the
timing numbers only mean something on a quiet, pinned lab guest, so
`verify.lua` builds locally, stages the binary and the driver script to
`systems-target` over `scp`, and runs everything there.

## Verification

`verify.lua` builds each language, stages the binary and
`run-latency-bench.sh` to `/tmp/lsp40-<lang>/` on the lab guest, and asserts,
per language: the five usage-error shapes exit `2` with the banner; an
out-of-range `--pin` exits `1` with `cpu N out of range`; a refused connect
and a non-IPv4 `measure` target each exit `1` with the matching `app: error:`
text; the real `run-latency-bench.sh` run exits `0`; `Cpus_allowed_list` from
`/proc/<pid>/status` shows the naive server with the full online CPU set and
the fastpath server with exactly one; all six `percentiles_ns` lines parse
with `n=20000` and monotonic percentiles; the median of three fastpath `p50`s
beats the median of three naive `p50`s by at least 10%; and a bounded
10-second wait after `SIGINT` confirms both servers exit `0` with their
shutdown line, the check most likely to catch a Go port that forgot to close
its listener on signal.

```
[host]$ python3 scripts/test-all-examples.py --only 40-low-latency-fastpath --mode vm
verifying...
  verify 40-low-latency-fastpath [cpp]: PASS
  verify 40-low-latency-fastpath [go]: PASS
  verify 40-low-latency-fastpath [rust]: PASS
```

## Mapping to a chapter

This is the example for "The low-latency fast path: pin a core, reuse one
buffer, and stop asking the scheduler for permission"
(`_docs/40-low-latency-fastpath.md`), in the Performance and Low Latency part.
