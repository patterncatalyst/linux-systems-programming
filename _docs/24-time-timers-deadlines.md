---
title: "Time, timers, and deadlines"
order: 24
part: "Networking"
description: "chatterd v3 learns to survive a peer that vanishes: the POSIX clock taxonomy, clock_gettime's vDSO fast path (no syscall — proven by its absence under strace), timerfd as a pollable heartbeat, the per-language deadline idiom, why liveness must be measured on a monotonic clock, and jittered exponential backoff."
duration: "45 minutes"
---

Chapters 21–23 built `chatterd` into a peer-to-peer chat daemon that links two
hosts over TCP and passes framed messages. It has one fatal naïveté: it assumes
the peer stays. Pull the cable, kill the process, or let a switch drop the
flow, and a `chatterd` peer sits forever in a `read` that will never return —
TCP alone will not tell you the far end is gone in any useful window. Version 3
fixes this with *time*: a periodic **heartbeat** proves the link is alive, a
**deadline** measured on silence declares it dead, and a **jittered
exponential backoff** brings it back without hammering. The one new idea under
all three is that not all clocks are the same clock — and picking the wrong one
turns a heartbeat into a liability the first time NTP corrects your wall clock.

The code is in `examples/24-time-timers-deadlines/`. `./demo.sh` there builds
all three implementations; its `README.md` specifies the CLI, the wire format
(the same canonical frame chapters 21–23 used, now carrying a `PING` TYPE this
chapter adds), and the exit codes all three languages share.

{% include excalidraw.html
   file="24-clock-taxonomy"
   alt="Top band: the four clocks clock_gettime can read as boxes — CLOCK_REALTIME (dashed, wall time, settable, can step backward), CLOCK_MONOTONIC (amber, since a boot epoch, never steps, steady_clock/Instant), CLOCK_BOOTTIME (monotonic plus suspend time), and PROCESS_CPUTIME_ID (this process's on-CPU time). Bottom band: while a peer is silent and ntpd corrects the wall clock, a dashed REALTIME box shows a wall-clock deadline rewinding so a dead peer looks alive, beside an amber MONOTONIC box where elapsed is always non-negative and the timeout fires on real silence; an amber arrow from CLOCK_MONOTONIC points to it labeled chatterd reads this."
   caption="Figure 24.1 — the clock taxonomy and the monotonic-vs-wall bug: a wall-clock deadline slips when the clock is stepped; a CLOCK_MONOTONIC deadline does not" %}

> **Tools used** — `strace`, `python3` (host); `cyclictest` (systems-target
> VM, exercised in Part 8). Everything host-side is checked by
> `scripts/check-host.sh` or ships with Fedora.

## The clock taxonomy

`clock_gettime(2)` takes a *clock id*, and the choice is the whole chapter.
Four ids matter here. **`CLOCK_REALTIME`** is the wall clock — seconds since the
Unix epoch, the thing `date` prints. It is *settable* (`settimeofday`,
`adjtimex`) and *disciplined by NTP*, which means it can jump forward, and — the
part that bites — jump *backward*. **`CLOCK_MONOTONIC`** counts from an
unspecified epoch (near boot) and has one guarantee the wall clock cannot give:
it never steps and never goes backward. NTP may gently *slew* its rate, but two
successive reads always satisfy `t1 >= t0`. **`CLOCK_BOOTTIME`** is monotonic
*and* keeps counting across system suspend, which `CLOCK_MONOTONIC` does not —
the right clock for "has 30 s of wall time passed even if the laptop slept."
**`CLOCK_PROCESS_CPUTIME_ID`** measures only the CPU time *this process* burned,
useful for profiling, useless for wall deadlines.

The rule that follows is absolute: **any duration or deadline must be measured
on a monotonic clock.** A heartbeat timeout is a duration — "no traffic for
3 seconds" — so it reads `CLOCK_MONOTONIC`, never `CLOCK_REALTIME`. Figure 24.1
is the bug you avoid by obeying this. Suppose you computed a deadline as
`wall_now() + 3s` and stored it. Halfway through, `ntpd` decides the clock is
0.5 s fast and steps it *back*. Now `wall_now()` reads earlier than when you
started; your deadline recedes into the future; a peer that died stays "alive"
for an extra half second — or, on a backward step large enough, your `select`
timeout goes negative and a healthy peer is declared dead instantly. Every
language in this chapter routes liveness through a monotonic reading:
`std::chrono::steady_clock` in C++, `std::time::Instant` in Rust, and Go's
`time.Now`, whose returned value carries a hidden monotonic component precisely
so `time.Since` cannot be fooled by a wall-clock step.

## clock_gettime and the vDSO — no syscall at all

Reading the clock happens constantly — on every loop iteration `chatterd`
recomputes how long until the deadline — so it had better be cheap. It is, and
the reason is chapter 4's vDSO. `CLOCK_MONOTONIC` and `CLOCK_REALTIME` reads do
not trap into the kernel: the kernel maps a shared page (the vDSO) into every
process, updates a timekeeping structure there on each tick, and the libc
`clock_gettime` wrapper reads it in userspace with no `syscall` instruction.
The proof is negative and we run it later: `strace` a program that reads the
monotonic clock four times and you will not see a single `clock_gettime` line,
because there was no syscall to trace. Sleeping is the opposite — you cannot
sleep in userspace, so `nanosleep`/`clock_nanosleep` and `timerfd_settime` do
trap, and those *do* show up.

## How the code works

`clockprobe` is a standalone subcommand that makes the abstractions concrete by
measuring them. It reads the monotonic clock's resolution with `clock_getres`,
times a 1 ms `nanosleep`, and times a 1 ms one-shot `timerfd` — each timed on a
steady clock — then prints one line. Here it is in all three languages,
verbatim from `examples/24-time-timers-deadlines/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
int run_clockprobe() {
    timespec res{};
    ::clock_getres(CLOCK_MONOTONIC, &res);
    const long res_ns = res.tv_sec * 1'000'000'000L + res.tv_nsec;

    const auto t0 = steady_clock::now();
    timespec req{0, 1'000'000};  // 1 ms
    timespec rem{};
    while (::nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
    const auto sleep_us = duration_cast<microseconds>(steady_clock::now() - t0).count();

    UniqueFd tfd(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC));
    if (!tfd.valid()) {
        die("timerfd_create", errno_ec());
    }
    itimerspec its{};
    its.it_value.tv_nsec = 1'000'000;  // one-shot 1 ms
    if (::timerfd_settime(tfd.get(), 0, &its, nullptr) < 0) {
        die("timerfd_settime", errno_ec());
    }
    const auto t1 = steady_clock::now();
    std::uint64_t ticks = 0;
    [[maybe_unused]] auto rd = ::read(tfd.get(), &ticks, sizeof ticks);
    const auto tfd_us = duration_cast<microseconds>(steady_clock::now() - t1).count();

    std::println("clockprobe: CLOCK_MONOTONIC res={}ns nanosleep(1ms) actual={}us "
                 "timerfd(1ms) actual={}us",
                 res_ns, sleep_us, tfd_us);
    return 0;
}
```

```go
func runClockprobe() int {
	var res unix.Timespec
	if err := unix.ClockGetres(unix.CLOCK_MONOTONIC, &res); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: clock_getres: %v\n", err)
		return 1
	}
	resNs := res.Sec*1_000_000_000 + int64(res.Nsec)

	t0 := time.Now()
	req := unix.NsecToTimespec(1_000_000) // 1 ms
	for unix.Nanosleep(&req, &req) == unix.EINTR {
	}
	sleepUs := time.Since(t0).Microseconds()

	tfd, err := unix.TimerfdCreate(unix.CLOCK_MONOTONIC, unix.TFD_CLOEXEC)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: timerfd_create: %v\n", err)
		return 1
	}
	defer unix.Close(tfd)
	its := unix.ItimerSpec{Value: unix.Timespec{Nsec: 1_000_000}} // one-shot 1 ms
	if err := unix.TimerfdSettime(tfd, 0, &its, nil); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: timerfd_settime: %v\n", err)
		return 1
	}
	t1 := time.Now()
	var ticks uint64
	buf := (*[8]byte)(unsafe.Pointer(&ticks))[:]
	_, _ = unix.Read(tfd, buf)
	tfdUs := time.Since(t1).Microseconds()

	fmt.Printf("clockprobe: CLOCK_MONOTONIC res=%dns nanosleep(1ms) actual=%dus "+
		"timerfd(1ms) actual=%dus\n", resNs, sleepUs, tfdUs)
	return 0
}
```

```rust
fn run_clockprobe() -> i32 {
    let res = clock_getres(ClockId::CLOCK_MONOTONIC).expect("clock_getres");
    let res_ns = res.tv_sec() as i64 * 1_000_000_000 + res.tv_nsec() as i64;

    let t0 = Instant::now();
    let req = TimeSpec::nanoseconds(1_000_000); // 1 ms
    while clock_nanosleep(ClockId::CLOCK_MONOTONIC, ClockNanosleepFlags::empty(), &req)
        == Err(Errno::EINTR)
    {}
    let sleep_us = t0.elapsed().as_micros();

    let tfd = TimerFd::new(TfdClockId::CLOCK_MONOTONIC, TimerFlags::empty()).expect("timerfd");
    tfd.set(Expiration::OneShot(TimeSpec::nanoseconds(1_000_000)), TimerSetTimeFlags::empty())
        .expect("timerfd_settime");
    let t1 = Instant::now();
    tfd.wait().expect("timerfd wait");
    let tfd_us = t1.elapsed().as_micros();

    println!(
        "clockprobe: CLOCK_MONOTONIC res={res_ns}ns nanosleep(1ms) actual={sleep_us}us \
         timerfd(1ms) actual={tfd_us}us"
    );
    0
}
```

The three are the same program in three dialects. `clock_getres` on this host
returns `1ns` — nanosecond nominal resolution — but resolution is not accuracy:
the two 1 ms sleeps *overshoot*, and that gap is the lesson. A `timerfd` is a
timer wrapped in a file descriptor: `timerfd_create(CLOCK_MONOTONIC, …)` makes
one, `timerfd_settime` arms it (an `it_value` with no `it_interval` is a
one-shot; both set makes it periodic), and it becomes *readable* when it
expires — a `read` returns a `uint64` count of expirations. That last property
is why it belongs in this chapter: a timer that is an fd drops straight into the
`poll`/`epoll` loop chapter 9 built, no signal handler required.

The session loop is where the deadline lives. In the C++ `run_session`, every
iteration computes how long to block from the monotonic deadline, and hands
that to `poll` as its timeout:

```cpp
    for (;;) {
        int poll_ms = 0;
        {
            const auto left = deadline - steady_clock::now();
            poll_ms = left <= 0ns
                          ? 0
                          : static_cast<int>(duration_cast<milliseconds>(left).count()) + 1;
        }
```

`deadline` is a `steady_clock::time_point` set to `now() + timeout` at session
start and *reset to a fresh `now() + timeout` on every received frame* — JOIN,
MSG, or PING, all count as traffic. The `poll` array holds three fds: the
`signalfd` shutdown doorbell, the periodic heartbeat `timerfd`, and the peer
socket. When `poll` returns 0, the timeout elapsed with nothing to read — the
peer went silent for `timeout_ms` — and the session returns `TimedOut`. When
the heartbeat fd fires, the loop reads its 8-byte tick and sends a `PING`. This
is the same one-loop-three-sources shape as chapter 9's `fwatch`, now serving
liveness instead of file events. The three languages express the deadline
differently — C++ subtracts two `time_point`s, Rust does the identical
arithmetic on `Instant`, and Go resets a `time.Timer` and `select`s on its
channel — but the invariant is one sentence: *the clock is monotonic and the
deadline is data.*

Reconnection is the connector's job, and it is where jitter earns its keep.
After a drop, `run_connect` waits out a backoff before redialing:

```cpp
        // Jittered exponential backoff (equal jitter: half fixed, half random).
        const long half = backoff / 2;
        std::uniform_int_distribution<long> jit(0, half > 0 ? half : 0);
        const long delay = half + jit(rng);
        std::println("chatterd: reconnecting to {} in {}ms", opts.peer_name, delay);
        if (sleep_or_shutdown(delay, shutdown_fd.get())) {
            std::println("chatterd: {} shutting down", opts.name);
            return 0;
        }
        backoff = std::min(backoff * 2, opts.max_backoff_ms);
```

Two ideas are stacked here. **Exponential** — `backoff` doubles per failed
attempt (200 → 400 → 800 …) up to `--max-backoff-ms`, so a peer that is down
for a while is polled ever more gently instead of in a hot loop. **Jitter** —
the actual `delay` is `half + rand(0..half)`, the "equal jitter" strategy: half
the interval fixed, half random. Jitter matters because without it, a hundred
clients that all lost the same server would redial in lockstep on every doubling
boundary, a synchronized thundering herd that knocks the server back down the
instant it recovers. Randomizing each client's wait smears the retries across
the window. A fresh connection resets `backoff` to the base, so a brief blip is
never punished with a long wait. The wait itself is `sleep_or_shutdown` — a
`poll` on the signalfd with the backoff as its timeout — so a `SIGTERM`
mid-backoff wakes instantly instead of stranding the process for seconds.

## Errors, three ways

The contract is chapter-wide: clean exit 0, runtime failures exit 1 with one
diagnostic, usage errors exit 2. The interesting failures here are temporal.
The most important one is a *non-error*: when the peer socket returns EOF (0
bytes) because the far end closed, `chatterd` does **not** immediately declare
the peer gone. It sets `sock_dead`, stops polling the socket, and lets the
*deadline* fire — so death is always declared by the same monotonic timeout
whether the peer crashed silently or closed politely, and the observable line
is identical. Sending a `PING` into a half-closed socket can raise `EPIPE`; C++
passes `MSG_NOSIGNAL` so it never becomes a fatal `SIGPIPE`, and the failed send
is deliberately ignored — "a broken pipe is left to the deadline," as the
comment says, because the deadline is the single source of truth about
liveness. `EINTR` on `poll` is retried in C++ and Rust and handled beneath the
runtime in Go. Bad numeric flags (`--timeout-ms abc`) and missing required
flags exit 2 through each language's parser — `std::expected` chains in C++,
error returns in Go, `Result` in Rust — all printing the same `usage:` block.

## Concurrency lens

The C++ and Rust sessions are single-threaded: one `poll` loop multiplexes the
socket, the heartbeat timerfd, and the shutdown signalfd, so there is no shared
state to lock — the timer and the deadline are just two more fds and one
`time_point`. Go inverts it, exactly as chapter 9 did: a per-connection reader
goroutine turns the socket into a `chan frame`, and the session is a `select`
over four cases — `ticker.C` (heartbeat), `deadline.C` (liveness), the frame
channel, and `ctx.Done()` (shutdown from `signal.NotifyContext`). The subtlety
Go forces you to get right is `time.Timer` reset: before `deadline.Reset`, the
code drains a possibly-already-fired timer channel, because a `Timer` that
expired between `select` iterations leaves a value buffered in `C` that would
otherwise make the *next* `select` see a phantom timeout. C++ and Rust sidestep
this entirely by recomputing the `poll` timeout from the deadline each
iteration — there is no timer object to get out of sync, just arithmetic on a
monotonic reading. Both designs reach the same behavior; the single-threaded
one has less to synchronize, the Go one reads more like the protocol.

## Build, run, observe

```bash
[host]$ cd examples/24-time-timers-deadlines && ./demo.sh build
```

The runner (`python3 scripts/test-all-examples.py --only
24-time-timers-deadlines`) reports `PASS PASS PASS` for cpp, go, rust —
`verify.lua` asserts observable behavior, not exit 0. First, `clockprobe` on
this host:

```console
[host]$ ./cpp/build/release/app clockprobe
clockprobe: CLOCK_MONOTONIC res=1ns nanosleep(1ms) actual=1056us timerfd(1ms) actual=1013us
```

All three languages agree to within noise — Go printed `actual=1066us` /
`1005us`, Rust `1055us` / `1002us`. The number to sit with is the overshoot: a
1 ms sleep took ~1056 µs, ~56 µs long. That gap is not the clock's resolution
(1 ns) — it is the *scheduler's wake-up latency*, the time between the timer
expiring and your thread actually getting a CPU. On a general-purpose kernel
this is tens of microseconds and variable; the timerfd path is a touch tighter
(~1013 µs) because it wakes straight out of `read` rather than through
`nanosleep`'s return. Now the heartbeat scenario, driven by hand — a listener
`A` and a connector `B` on loopback, then `A` killed:

```console
[host]$ ./cpp/build/release/app listen --name A --addr 127.0.0.1:0 \
        --heartbeat-ms 300 --timeout-ms 1000 --message "hi from A" &
chatterd: A listening on 127.0.0.1:35145
[host]$ ./cpp/build/release/app connect --name B --peer A@127.0.0.1:35145 \
        --heartbeat-ms 300 --timeout-ms 1000 --backoff-ms 200 --message "hi from B" --seed 7
chatterd: B connecting to A at 127.0.0.1:35145
chatterd: B linked with A
chatterd: B message from A: hi from A
[host]$ kill -9 <A's pid>
chatterd: peer A timed out
chatterd: reconnecting to A in 107ms
chatterd: reconnecting to A in 245ms
chatterd: reconnecting to A in 712ms
chatterd: reconnecting to A in 1055ms
chatterd: B shutting down          # after SIGTERM
```

The backoff line is the whole design on one screen. Base is 200 ms, so the
first `delay` is `100 + rand(0..100)` = 107 ms; then the ceiling doubles and the
window grows — 245 (from base 400), 712 (from 800), 1055 (from 1600) — each a
jittered draw inside its window, never a bare power of two. `SIGTERM`
mid-backoff prints `B shutting down` and exits 0, waking out of the `poll`
immediately. Figure 24.2 is this sequence.

{% include excalidraw.html
   file="24-heartbeat-timeout-seq"
   alt="Two lifelines: connector B (amber) on the left, listener A (grey, ending early) on the right. Top: JOIN 'B' from B to A and JOIN 'A' back, then a note that both print linked and exchange messages. Middle: amber PING arrows every 300 ms in both directions, each resetting B's deadline, with a note that any frame sets deadline = mono_now() + timeout. Then A killed, socket EOF, and a dashed PING from B that is dropped on the broken pipe. Bottom on B's side: an amber box 'deadline fires on silence — chatterd: peer A timed out', then three stacked reconnect boxes 107 ms, 245 ms, 712 ms chained by amber arrows, beside a note explaining equal-jitter backoff delay = B/2 + rand(0..B/2) with B doubling per attempt and capped at max-backoff."
   caption="Figure 24.2 — chatterd v3 from link to loss to reconnect: the monotonic deadline fires on silence, then jittered exponential backoff (real 107/245/712 ms from this run) paces the redials" %}

## Cross-check: the vDSO's absence, and the heartbeat live under strace

Two independent traces confirm the mechanism. First, the vDSO claim. Ask
`strace` for *only* `clock_gettime` while running `clockprobe`, which reads the
monotonic clock four times (`t0`, after the sleep, `t1`, after the timerfd) plus
`clock_getres`:

```console
[host]$ strace -e trace=clock_gettime ./cpp/build/release/app clockprobe 2>&1 | grep -c clock_gettime
0
```

Zero lines. Not because the program never reads the clock, but because those
reads never became syscalls — they were served from the vDSO page in userspace.
What *does* trap is visible when you trace the sleeping calls:

```console
[host]$ strace -c ./cpp/build/release/app clockprobe
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
  1.20    0.000008           8         1           clock_nanosleep
  0.75    0.000005           5         1           timerfd_create
  0.30    0.000002           2         1           timerfd_settime
```

`clock_nanosleep` (glibc implements `nanosleep` on top of it), `timerfd_create`,
`timerfd_settime` — each once, exactly as the source calls them — and no
`clock_gettime` row at all. The Rust binary shows
`clock_nanosleep(CLOCK_MONOTONIC, …)` because it calls that clock explicitly;
the C++ one shows `CLOCK_REALTIME` because that is glibc's `nanosleep` default —
a fine reminder that the *sleep* clock and the *deadline* clock are chosen
independently.

Second, the heartbeat itself. Tracing the connector's `poll`, `read`, `sendto`,
and `timerfd_settime` during a live session lays the whole loop bare:

```console
[host]$ strace -e trace=poll,read,timerfd_settime,sendto ./cpp/.../app connect --name B ...
timerfd_settime(5, 0, {it_interval={tv_sec=0, tv_nsec=300000000}, ...}, NULL) = 0
sendto(4, "CH\1\1\0\1B", 7, MSG_NOSIGNAL, NULL, 0) = 7
poll([{fd=3,...}, {fd=5,...}, {fd=4,...}], 3, 1200) = 1 ([{fd=4, revents=POLLIN}])
poll([{fd=3,...}, {fd=5,...}, {fd=4,...}], 3, 1200) = 1 ([{fd=5, revents=POLLIN}])
read(5, "\1\0\0\0\0\0\0\0", 8)          = 8
sendto(4, "CH\1\5\0\0", 6, MSG_NOSIGNAL, NULL, 0) = 6
poll([{fd=3,...}, {fd=5,...}, {fd=4,...}], 3, 900) = 1 ([{fd=4, revents=POLLIN}])
```

Every claim in the chapter is on those lines. `timerfd_settime` arms the
heartbeat with a 300 ms interval. The `sendto(4, "CH\1\1\0\1B", 7, …)` is the
7-byte JOIN frame — `43 48 01 01 00 01 42`, the canonical header (`CH`,
version 1, type 1 = JOIN) plus the name `B` — matching the wire format byte
for byte, the same header chapters 21–23 used; only the TYPE byte and the
engine around it differ by chatterd version. When fd 5 (the timerfd) fires,
the loop reads its 8-byte tick (`\1\0\0\0\0\0\0\0` — one expiration) and
sends the 6-byte PING `CH\1\5\0\0` (`43 48 01 05 00 00`). And
the `poll` timeout *is* the deadline countdown: `1200` right after receiving a
frame (the deadline was just reset), then `900` after a heartbeat-only wakeup —
the monotonic deadline shrinking exactly as `deadline - steady_clock::now()`
says it should. The trace and the printed output tell one story: the timer
drives the PINGs, the monotonic deadline drives the timeout.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the scheduler wake-up latency that `clockprobe` measures as ~56 µs of
> overshoot has a proper tool, `cyclictest`, which stresses the timer path and
> reports latency percentiles; and comparing a `PREEMPT` versus `PREEMPT_RT`
> kernel needs a disposable box. Both need root and a kernel we can perturb, so
> they run on `systems-target` in the Debugging part (Part 8), not on this host.

## What you learned

- The clock id is the decision: `CLOCK_REALTIME` is the settable, NTP-steppable
  wall clock; `CLOCK_MONOTONIC` never steps and is the only correct clock for a
  duration or deadline; `CLOCK_BOOTTIME` adds suspend time;
  `CLOCK_PROCESS_CPUTIME_ID` measures CPU, not wall time. `chatterd` measures
  silence on the monotonic clock so an NTP correction can never fake a timeout.
- `clock_gettime` on the common clocks is a vDSO read, not a syscall — `strace`
  showed **zero** `clock_gettime` lines for a program that read the clock four
  times, while `clock_nanosleep` and `timerfd_settime` (which must trap) each
  appeared once.
- A `timerfd(CLOCK_MONOTONIC)` is a timer you can `poll` — it drops the
  heartbeat into the same event loop as the socket and the shutdown signalfd,
  and `clockprobe` measured a 1 ms timer landing at ~1013 µs, the ~13 µs
  overshoot being scheduler wake-up latency, not clock resolution.
- Jittered exponential backoff (`delay = B/2 + rand(0..B/2)`, `B` doubling to a
  cap, reset on a fresh link) paces reconnects and de-synchronizes a fleet of
  clients — the live run drew 107, 245, 712, 1055 ms, growing but never a bare
  power of two.

Next, **shared state and the futex**: `chatterd`'s peers coordinated by passing
bytes; the next chapter coordinates threads that share one address space, where
a lock is a userspace atomic that only touches the kernel when it must block.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, strace 7.1) this session: the runner printed
<code>24-time-timers-deadlines  PASS  PASS  PASS</code> (3 passed, 0 failed, 0
skipped), and the orchestrator was re-run to confirm the timing scenario is not
flaky. <code>clockprobe</code> printed <code>res=1ns</code> with
<code>nanosleep(1ms) actual=1056us timerfd(1ms) actual=1013us</code> for C++
(Go 1066/1005, Rust 1055/1002). The hand-driven loopback session produced the
exact <code>linked</code>, <code>message from A</code>, <code>peer A timed
out</code>, and jittered backoff lines <code>107/245/712/1055ms</code> (base 200,
seed 7), with <code>SIGTERM</code> printing <code>B shutting down</code> and
exit 0. <code>strace -e trace=clock_gettime</code> on <code>clockprobe</code>
matched <strong>0</strong> lines (vDSO), while <code>strace -c</code> showed
<code>clock_nanosleep</code>, <code>timerfd_create</code>, and
<code>timerfd_settime</code> once each and no <code>clock_gettime</code> row;
the live connector trace showed the 300 ms <code>timerfd_settime</code>, the
7-byte JOIN <code>CH\1\1\0\1B</code>, the 8-byte timerfd tick, the 6-byte PING
<code>CH\1\5\0\0</code>, and the <code>poll</code> timeout counting 1200→900 as
the monotonic deadline. The <code>cyclictest</code> / <code>PREEMPT_RT</code>
callout is unverified as marked and is exercised in Part 8.</p>
