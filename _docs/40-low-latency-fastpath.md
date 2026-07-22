---
title: "The low-latency fast path: pin a core, reuse one buffer, and stop asking the scheduler for permission"
order: 40
part: "Performance and Low Latency"
description: "chatterd-fastpath strips the ch21 echo hot path down to a pinned core, one reused 64-byte buffer, and an optional busy-poll spin, then proves each move at the kernel level — Cpus_allowed_list and voluntary_ctxt_switches read from /proc/<pid>/status, not the program's own claim. The three languages reach identical behavior by three different shutdown mechanisms, and Go's CPU count lies about its container through a taskset mask rather than a cgroup."
duration: "50 minutes"
---

Chapter 39 taught you to trust a latency number: warm the code, time each
iteration, correct for coordinated omission, and report the tail instead of the
mean. This chapter spends that discipline. If you now believe a `p99` you
measured, the next question is how to make it *small* — and the answer, on Linux,
is a short list of moves that each remove one source of the multi-microsecond
stalls Chapter 39's `co_p99` was busy accounting for. `chatterd-fastpath` is the
same echo hot path from Chapter 21's `chatterd`, stripped to exactly those moves:
pin the process to one CPU, reuse a single stack buffer for every message so the
loop never allocates, and — optionally — spin on a non-blocking `recv` instead of
blocking, trading a whole core for the elimination of the sleep/wake round trip
through the scheduler. A `naive` server in the same binary does the opposite of
each, so you can race them under identical placement and watch the gap.

The code is in `examples/40-low-latency-fastpath/`. It is a **VM example**: the
measurements only mean something on a quiet, pinned machine, so `verify.lua`
stages the binary and `run-latency-bench.sh` to the lab guest and drives them
there.

{% include excalidraw.html
   file="40-fastpath-datapath"
   alt="Two bands showing one echo round trip each. Top, the naive server: a client frame arrives at a server that may run on any of the online CPUs; per message it calls make_unique to allocate a fresh 64-byte heap buffer, then a blocking read(2) that puts the thread to sleep — an arrow labeled voluntary context switch loops out through a box labeled kernel scheduler and back, waking the thread when data arrives — then write(2) echoes and the heap buffer is freed. A counter reads voluntary_ctxt_switches roughly one per message. Bottom, the fastpath server: pinned by sched_setaffinity to a single CPU shown boxed and shaded, one reused 64-byte stack buffer for the whole connection, and a busy-poll loop that spins on non-blocking recv(2) over EAGAIN, never entering the scheduler — no context-switch arrow, the thread stays on its core. A counter reads voluntary_ctxt_switches roughly zero. A note reads same 64-byte frame, same round trip — the difference is who the thread has to ask for its CPU back."
   caption="Figure 40.1 — one echo round trip, naive versus fastpath: a per-message heap buffer and a blocking read that sleeps through the scheduler, against one reused buffer on a pinned core that never gives the CPU up" %}

> **Tools used** — `taskset` (systems-target VM), `ssh`/`scp` (host, the lab's
> own transport). The pinning and busy-poll proofs read `/proc/<pid>/status` on
> the guest directly — procfs, no extra tool. `taskset` ships in
> `util-linux-core` on the Fedora 44 guest; the lab transport is the same
> `scripts/lab/` tooling every VM example uses.

## Three moves that shrink the tail

Every technique in `fastpath` targets a specific line item in a latency budget,
and each is worthless — or actively harmful — applied blindly. Naming them up
front:

- **Pin to one CPU** (`sched_setaffinity(2)`). A thread the scheduler is free to
  migrate can be moved, between one message and the next, onto a core that first
  has to be brought back up from a low C-state and whose caches hold none of your
  working set. That migration *is* a tail-latency event. Pinning trades the
  ability to flee a noisy neighbor for the removal of migration as a source of
  jitter. It is the right trade for a dedicated latency-critical process and the
  wrong one for a general-purpose worker pool.
- **Reuse one buffer** (zero heap allocation in the loop). `fastpath` reads into a
  single 64-byte stack array for the entire life of a connection; `naive`
  `make_unique`s a fresh heap buffer for every message. At 64 bytes the allocator
  cost is small, but it is *variable* — most calls hit the thread-local cache and
  return in nanoseconds, and the occasional one that has to grab the arena lock or
  fault in a fresh page does not, and that variance lands in the tail.
- **Busy-poll** (spin on non-blocking `recv`). A blocking `read(2)` on an idle
  socket puts the thread to sleep; when the next frame arrives, the kernel has to
  wake it, which is a scheduler round trip — a **voluntary context switch** — on
  every single message. Busy-poll never sleeps: it spins on a non-blocking `recv`
  that returns `EAGAIN` until data is there, burning 100% of the pinned core to
  keep the thread on-CPU and ready. This is the most expensive move (a whole core,
  gone) and the one with the largest single effect on median latency, as the
  numbers below show.

The rest of the program is deliberately ordinary: a fixed 64-byte wire frame
(magic `CF`, a version and type byte, a big-endian `seq`, and zero padding), a
`measure` subcommand that runs `N` synchronous ping-pong round trips against
either server and prints the same percentile table Chapter 39 built, and a
`run-latency-bench.sh` driver that races the two under identical `taskset`
placement so the *only* variable is the in-server discipline.

## Pinning, and the count that lies about its container

`pin_to_cpu` is four lines of real work — validate the CPU index, build a
one-CPU affinity mask, install it — wrapped around one decision that turns out to
diverge across the three languages more sharply than anything else in the
program:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
Result<void> pin_to_cpu(int cpu) {
    long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu < 0 || (nproc > 0 && cpu >= nproc)) {
        return std::unexpected(
            std::format("cpu {} out of range (0..{})", cpu, nproc > 0 ? nproc - 1 : 0));
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        return std::unexpected(std::format("sched_setaffinity({}): {}", cpu, errno_text()));
    }
    return {};
}
```

```go
func pinToCPU(cpu int) error {
	nproc := onlineCPUs()
	if cpu < 0 || (nproc > 0 && cpu >= nproc) {
		last := 0
		if nproc > 0 {
			last = nproc - 1
		}
		return fmt.Errorf("cpu %d out of range (0..%d)", cpu, last)
	}

	// One P, one hot thread: without this the runtime is free to schedule the
	// loop's goroutine onto another M running on another CPU.
	runtime.GOMAXPROCS(1)
	runtime.LockOSThread()

	var set unix.CPUSet
	set.Zero()
	set.Set(cpu)
	if err := unix.SchedSetaffinity(0, &set); err != nil {
		return fmt.Errorf("sched_setaffinity(%d): %v", cpu, err)
	}
	if err := unix.SchedSetaffinity(os.Getpid(), &set); err != nil {
		return fmt.Errorf("sched_setaffinity(%d): %v", cpu, err)
	}
	return nil
}
```

```rust
fn pin_to_cpu(cpu: i32) -> StepResult<()> {
    // sysconf(_SC_NPROCESSORS_ONLN) reports CPUs online SYSTEM-WIDE and ignores
    // this process's affinity mask — the same call the C++ port uses. That
    // matters under the bench driver's `taskset -c 1`: a mask-aware count
    // (like Go's runtime.NumCPU) would report 1 and wrongly reject a pin to
    // CPU 1. sysconf sidesteps that.
    let nproc = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) };
    if cpu < 0 || (nproc > 0 && i64::from(cpu) >= nproc) {
        let last = if nproc > 0 { nproc - 1 } else { 0 };
        return Err(format!("cpu {cpu} out of range (0..{last})"));
    }
    let mut set = CpuSet::new();
    set.set(cpu as usize)
        .map_err(|e| format!("sched_setaffinity({cpu}): {e}"))?;
    // Pid 0 == the calling thread, which is the main (and only) thread, whose
    // tid == pid — so this is also what /proc/<pid>/status reports.
    sched_setaffinity(Pid::from_raw(0), &set)
        .map_err(|e| format!("sched_setaffinity({cpu}): {e}"))?;
    Ok(())
}
```

The range check needs a CPU count, and the correct count is the number of CPUs
online **system-wide**, because the argument is an absolute CPU index. C++ and
Rust both ask `sysconf(_SC_NPROCESSORS_ONLN)`, which returns exactly that (2 on
the lab guest) and is blind to the caller's affinity mask. The obvious Go
equivalent, `runtime.NumCPU()`, is *not* the same thing: it returns the size of
the process's **CPU affinity mask**. Those two numbers agree on an idle machine
and diverge the instant something narrows the mask — which is precisely what the
benchmark driver does. Phase B of `run-latency-bench.sh` launches each server
under `taskset -c 1`, so the fastpath process starts life with a one-CPU mask,
`runtime.NumCPU()` returns **1**, and the Go server's own range check rejects the
`--pin 1` it was told to use:

```
app: error: cpu 1 out of range (0..0)
```

This is the same failure mode as `hardware_concurrency()` reporting a container's
cgroup quota instead of the host's core count — the running theme of this part of
the book — reached through an unexpected door. Here the count lies not because a
cgroup capped it but because a `taskset` affinity mask narrowed it, and the fix
is the same in spirit: ask the kernel the question you actually mean.
`onlineCPUs()` in the Go port reads `/sys/devices/system/cpu/online` and counts
the ranges there, matching the `sysconf` semantics the other two languages get
for free:

```go
func onlineCPUs() int {
	data, err := os.ReadFile("/sys/devices/system/cpu/online")
	if err != nil {
		return runtime.NumCPU()
	}
	// ... parse the "0-1" / "0,2-3" range list, falling back to NumCPU on any
	// parse failure ...
}
```

The Go port also carries two lines the others don't need. `sched_setaffinity(2)`
acts on a **thread**, and a goroutine is not a thread — the runtime multiplexes
goroutines over OS threads (Ms) and will migrate the one running your loop at
will. `runtime.LockOSThread()` nails the hot loop's goroutine to one M and
`GOMAXPROCS(1)` stops the runtime from parking worker threads on other cores; only
then does pinning that thread mean anything. And the affinity call is made
*twice* — once with `0` (the calling thread, which is what actually restricts the
loop) and once with `os.Getpid()` (the thread-group leader, which is the mask
`/proc/<pid>/status` reports and the benchmark reads back). C++ and Rust are
single-threaded here, so one `sched_setaffinity(0, …)` both pins the loop and
shows up in procfs; there is no leader-versus-worker gap to close.

## Busy-poll: trading a whole core for the scheduler round trip

The second codetab is the fastpath's defining loop. In busy-poll mode the
connection socket is set `O_NONBLOCK`, and this is the read:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
bool read_full_busy(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t r = ::recv(fd, p + off, n - off, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (g_stop_for_busy_read && g_stop_for_busy_read->load(std::memory_order_relaxed)) {
                    return false;
                }
                continue;  // spin
            }
            return false;
        }
        if (r == 0) {
            return false;  // peer closed
        }
        off += static_cast<std::size_t>(r);
    }
    return true;
}
```

```go
func readFullBusy(fd int, buf []byte, s *stopper) bool {
	off := 0
	for off < len(buf) {
		n, err := unix.Read(fd, buf[off:])
		if err != nil {
			if errors.Is(err, unix.EAGAIN) || errors.Is(err, unix.EWOULDBLOCK) {
				if s.stop.Load() {
					return false
				}
				continue // spin
			}
			if errors.Is(err, unix.EINTR) {
				continue
			}
			return false
		}
		if n == 0 {
			return false // peer closed
		}
		off += n
	}
	return true
}
```

```rust
fn read_full_busy(fd: RawFd, buf: &mut [u8]) -> bool {
    let mut off = 0;
    while off < buf.len() {
        match recv(fd, &mut buf[off..], MsgFlags::empty()) {
            Ok(0) => return false, // peer closed
            Ok(n) => off += n,
            Err(Errno::EAGAIN) => {
                if stop_requested() {
                    return false;
                }
                // spin
            }
            Err(_) => return false,
        }
    }
    true
}
```

There is nothing in the `EAGAIN` branch but a stop-flag check and `continue`. No
`sched_yield`, no `nanosleep`, no `poll` — those would all, in different ways,
hand the CPU back to the kernel, which is the exact cost busy-poll exists to
avoid. The thread stays runnable and on its core, re-issuing `recv` as fast as the
CPU will let it, so the instant a frame lands the very next `recv` returns it with
no wakeup latency at all. The blocking path (`read_full_blocking`, not shown) is
the ordinary alternative: a plain blocking `read` that the kernel parks on until
data arrives.

The difference is not subtle, and it does not require a profiler to see — it is
counted for you in `/proc/<pid>/status`. Every time a blocking `read` sleeps and
is woken, the kernel records a **voluntary context switch** against the process.
Running a 20,000-message `measure` against each server, both `taskset`-pinned to
CPU 1, and reading `voluntary_ctxt_switches` before and after:

```console
[vm]$ # naive (blocking read)
before: voluntary_ctxt_switches: 2
after:  voluntary_ctxt_switches: 20500
[vm]$ # fastpath (busy-poll)
before: voluntary_ctxt_switches: 2
after:  voluntary_ctxt_switches: 3
```

The blocking server took **one voluntary context switch per message** — it slept
and was woken 20,498 times to serve 20,000 requests plus warmup. The busy-poll
server slept **once** for the whole run. That single number is the mechanism the
median latency reflects: on this run the naive `p50` was 11,426 ns and the
fastpath `p50` was 8,148 ns, and the ~3,300 ns the fastpath saved is, to a first
approximation, the sleep/wake round trip through the scheduler that it declined to
take 20,000 times.

## How the code works

Both servers share one accept skeleton: a `poll(2)` on the listener with a 200 ms
timeout so the stop flag is checked a few times a second even when idle, then
`accept4`, `TCP_NODELAY`, and a per-connection echo loop that serves one client to
completion before returning to `accept`. The whole program runs on **one thread**
by design — the entire point is that a latency-critical hot loop should not be
handing work between threads, so there is no thread pool and no per-connection
goroutine or task. The fastpath loop reads into the one 64-byte buffer and writes
it straight back; the naive loop `make_unique`s (C++), `make([]byte, …)`s (Go), or
`vec![0u8; …]`s (Rust) a fresh buffer every iteration and lets it fall out of
scope, the deliberate pessimization the benchmark is built to expose.

`measure` is the client half, and its percentile math is exactly Chapter 39's:
sort the samples, then nearest-rank index with `idx = ceil(p/100 · n)` clamped to
`[1, n]`. It builds each frame with `seq = i`, times the round trip between two
`CLOCK_MONOTONIC` reads, and — a correctness check, not a timing one — verifies
the echo came back with the same `seq` before recording the sample, so a
cross-wired or corrupted connection fails loudly rather than reporting fast
garbage. Warmup round trips (500 by default in the benchmark) are discarded before
recording starts, for the reasons Chapter 39 spent a section on.

## Errors, three ways

`fastpath` has two exit codes for two kinds of failure, and they mean different
things. A **usage error** — an unknown subcommand, a missing `--pin`, a `measure`
with no `--n`, a port outside 1–65535 — prints the three-line usage banner to
stderr and exits **2**. Argument parsing is strict full-string unsigned, so
`--n 5x` is a usage error, not a silent truncation to 5.

A **runtime error** is exit **1**, and it carries an `app: error: …` message
naming what went wrong: `cpu 99 out of range (0..1)` from `pin_to_cpu`,
`connect 127.0.0.1:1: Connection refused` when `measure` can't reach a server,
`notanip: not an IPv4 address` when the target host isn't a dotted quad (the
servers use `inet_pton`, not a DNS lookup, so a hostname is an error by design),
and `measure: connection closed at iteration N` if a server dies mid-run. The
three languages carry these three ways — C++ returns `std::expected<T,
std::string>` and prints the `error()` string; Go returns `error` values with the
`strerror` text unwrapped so the message reads like C's rather than Go's layered
`dial tcp4 …: connect: …`; Rust returns `Result<T, String>` with the message
pre-formatted at the failure site — but the bytes on stderr are identical across
all three on the same guest, because `verify.lua` asserts exactly that.

## Concurrency lens

The interesting concurrency in this chapter is not in the hot loop — that is
single-threaded on purpose — it is in **shutdown**, and it is the one place where
identical observable behavior demanded three genuinely different mechanisms.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;  // deliberately NOT SA_RESTART
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}
```

```go
func (s *stopper) trigger() {
	s.stop.Store(true)
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ln != nil {
		_ = s.ln.Close()
	}
	if s.conn != nil {
		_ = s.conn.Close()
	}
	if s.rawFD >= 0 {
		// SHUT_RDWR makes an in-flight recv(2) return 0 immediately; the fd
		// itself is closed by the loop that owns it.
		_ = unix.Shutdown(s.rawFD, unix.SHUT_RDWR)
	}
}
```

```rust
fn install_signal_handlers() {
    let sa = SigAction::new(
        SigHandler::Handler(on_signal),
        SaFlags::empty(), // deliberately NOT SA_RESTART
        SigSet::empty(),
    );
    // SAFETY: on_signal only performs a relaxed atomic store, which is
    // async-signal-safe.
    unsafe {
        let _ = sigaction(Signal::SIGINT, &sa);
        let _ = sigaction(Signal::SIGTERM, &sa);
    }
}
```

C++ and Rust are showing the *same* function and the same idea: install a signal
handler that does nothing but set an atomic stop flag, with `sa_flags` explicitly
**not** `SA_RESTART`. That flag is the whole trick. Without `SA_RESTART`, a
blocking `read(2)` or `poll(2)` already in progress when `SIGINT` arrives does not
resume — it returns `EINTR`, the loop unwinds, sees the stop flag, and exits. The
signal handler and the blocked syscall cooperate through the kernel's default
`EINTR` behavior, and no second thread is involved. Rust can copy the C++ design
line for line because Rust's standard library makes real blocking syscalls; the
one wrinkle is that std's higher-level wrappers (`TcpListener::accept`,
`Read::read_exact`) retry `EINTR` internally, so the servers are built on raw file
descriptors via `nix` where every `EINTR` is handled explicitly.

Go cannot use this technique at all, which is why its tab shows a completely
different function. The Go runtime installs its own signal handlers with
`SA_RESTART`, and — more fundamentally — a "blocking" read on a `net.Conn` is not
sitting in a blocking syscall to begin with. The runtime's network poller parks
the goroutine on an `epoll` registration and lets the OS thread go do other work;
no signal will make that parked read return `EINTR`, because there is no syscall
in flight to interrupt. So Go reaches identical behavior — `SIGINT`, then a clean
`app: … shutting down` and exit 0 — by a different route entirely: the
`signal.Notify` goroutine sets the stop flag and then **closes the listener and
the live connection**. Closing an fd is what wakes a poller-parked read; it is the
`shutdown(2)`-driven wakeup Chapter 21's `chatterd` used, and it is the idiomatic
Go answer to "interrupt a blocked I/O wait." The busy-poll path needs one extra
touch — its socket is a dup'd raw fd outside the poller — so `trigger` also calls
`shutdown(SHUT_RDWR)` on it to break the spin. Three mechanisms — non-`SA_RESTART`
`EINTR`, non-`SA_RESTART` `EINTR` again, and close-the-fd — for one observable
outcome, which `verify.lua` pins down with a bounded 10-second wait so a server
that *fails* to shut down surfaces as a clear failure rather than a hung run.

## Build, run, observe

```bash
[host]$ cd examples/40-low-latency-fastpath && ./demo.sh build
```

The `run-latency-bench.sh` driver does the real work. Phase A brings up a naive
and a fastpath server with no external pinning and reads each one's
`Cpus_allowed_list` from `/proc/<pid>/status` — this is the pinning proof, taken
from the kernel rather than from anything the program prints about itself. Phase B
then races the two under identical `taskset` placement (both servers pinned to the
same CPU, the client to the other), running three interleaved trials of each so a
lone scheduling stall on this shared nested-KVM guest can't decide the outcome:

```console
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

`naive-cpus-allowed` is `0-1`, the full online set — the naive server never
touches its affinity mask. `fastpath-cpus-allowed` is `1`, a single CPU, because
`sched_setaffinity` narrowed it. Median of the three naive `p50`s is 11,320 ns;
median of the three fastpath `p50`s is 8,184 ns — the fast path is about 1.38×
faster on this run, and the margin held across every language and every capture
this session (1.38×–1.62×), never inverted, never marginal. The runner drives all
three languages against the same `verify.lua`:

```console
[host]$ python3 scripts/test-all-examples.py --only 40-low-latency-fastpath --mode vm
verifying...
  verify 40-low-latency-fastpath [cpp]: PASS
  verify 40-low-latency-fastpath [go]: PASS
  verify 40-low-latency-fastpath [rust]: PASS

example                  cpp   go    rust
40-low-latency-fastpath  PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

## Cross-check: two kernel-level signals, neither the program's own word

The chapter's two central claims are checked by two independent numbers, both read
from `/proc/<pid>/status` on the guest — not from anything the server prints about
itself, which is the discipline that makes them proofs rather than assertions:

| claim | procfs field | naive | fastpath |
|---|---|---|---|
| the fast path is pinned | `Cpus_allowed_list` | `0-1` (full set) | `1` (one CPU) |
| busy-poll never sleeps | `voluntary_ctxt_switches`, 20k-message run | 2 → 20500 | 2 → 3 |

The first row proves the pinning move happened at the kernel level: if
`sched_setaffinity` had silently failed, `fastpath-cpus-allowed` would read `0-1`
like the naive server, and the benchmark would be measuring two unpinned processes
fighting over both cores. The second row proves the busy-poll move: a server that
claimed to busy-poll but was secretly blocking would show ~20,000 voluntary
context switches like the naive one. Both numbers come from the kernel's own
accounting, and both are exactly what the mechanism predicts — which is why
`verify.lua` asserts the `Cpus_allowed_list` shape directly and why the
context-switch contrast, though not gated, is the single clearest picture of what
busy-poll buys and what it costs.

## What you learned

- **Pinning, zero allocation, and busy-poll each remove one specific source of
  tail latency** — migration jitter, allocator variance, and the scheduler
  sleep/wake round trip respectively — and each has a real cost (a core you can't
  reclaim, most of all for busy-poll), so they are tools for a dedicated
  latency-critical path, not defaults.
- **Busy-poll's whole effect is visible as a context-switch count**: a blocking
  server takes one voluntary context switch per message (20,500 over this run), a
  busy-poll server takes essentially none (3), and the median-latency gap is that
  scheduler round trip, declined 20,000 times.
- **Prove a low-latency claim from the kernel, not the program.**
  `Cpus_allowed_list` and `voluntary_ctxt_switches` in `/proc/<pid>/status` are
  the ground truth for "is it pinned" and "does it sleep"; a server's own printed
  line is a claim, and the two are allowed to disagree when something is broken.
- **A CPU count lies about its container through more than one door.** Go's
  `runtime.NumCPU()` reports the affinity-mask size, so under `taskset -c 1` it
  returns 1 where `sysconf(_SC_NPROCESSORS_ONLN)` returns the host's 2 — the same
  class of bug as `hardware_concurrency()` reading a cgroup quota, reached via an
  affinity mask instead. For an absolute-CPU-index range check, the system-wide
  online count is the one you mean.
- **Identical observable behavior can require different mechanisms per runtime.**
  C++ and Rust unblock a shutdown with a non-`SA_RESTART` handler and `EINTR`; Go's
  network poller means no syscall is in flight to interrupt, so it closes the fd
  instead — same `SIGINT`, same exit 0, three different routes there.

Chapter 41 takes everything from this part and the last — the USE checklist, the
LGTM pipeline, benchmarks you can trust, and this chapter's pinned, allocation-free
hot path — and assembles it into the capstone: a supervised, sandboxed, fully
observed two-VM fleet.

---

<p><span class="status status--verified">verified</span> — on the
<code>systems-target</code> lab guest (Fedora 44, kernel
<code>6.19.10-300.fc44</code>, 2 vCPU nested KVM) this session:
<code>python3 scripts/test-all-examples.py --only 40-low-latency-fastpath --mode
vm</code> printed <code>PASS PASS PASS</code> (3 passed, 0 failed;
<code>verify.lua</code> reported <code>PASS 46 / FAIL 0</code> for each of
cpp/go/rust). The pinning proof read <code>naive-cpus-allowed 0-1</code> and
<code>fastpath-cpus-allowed 1</code> from <code>/proc/&lt;pid&gt;/status</code>.
A 20,000-message <code>measure</code> against each server, both
<code>taskset</code>-pinned to CPU 1, moved <code>voluntary_ctxt_switches</code>
from 2 to 20500 for the blocking naive server and from 2 to 3 for the busy-poll
fastpath server, with <code>p50</code> 11426 ns (naive) versus 8148 ns
(fastpath) on that run. A full <code>run-latency-bench.sh ./app 20000 500</code>
gave median <code>p50</code> 11320 ns (naive) versus 8184 ns (fastpath), a 1.38×
margin; per-language runner medians this session were 1.47× (cpp), 1.62× (go),
and 1.51× (rust), all clearing the <code>verify.lua</code>
<code>median(fastpath p50) &lt; median(naive p50) · 0.90</code> gate. Go's
<code>runtime.NumCPU()</code>-under-<code>taskset</code> range-check failure
(<code>cpu 1 out of range (0..0)</code>) was reproduced directly before the
<code>onlineCPUs()</code> fix and is gone after it. Not exercised: any run
outside the lab guest — this is a VM example whose numbers are only meaningful on
a quiet, pinned machine.</p>
