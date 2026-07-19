---
title: "Signals and safe handling"
order: 12
part: "Processes, Signals, Privilege"
description: "Dispositions, per-thread masks, and the pending set — then three race-free ways to consume signals as pmon v1 becomes a restarting supervisor: signalfd in C++, runtime channels in Go, the self-pipe trick in Rust, with strace catching Go's 114-call rt_sigaction storm and SIGURG preemption in the act."
duration: 50 minutes
---

Chapter 11 taught `pmon` to run one child and report how it died. Version 1
makes it a real supervisor: restart a crashing child with a doubling backoff,
shut down cleanly on SIGTERM or SIGINT, and replace the child on SIGHUP. That
sounds like three `signal()` calls — and that is exactly the trap. The one new
idea of this chapter is that a signal handler is close to lawless territory
where almost no code is safe to run, so well-built programs run *no* meaningful
code there at all: they convert signals into ordinary data — a file descriptor
read, a channel receive, a byte in a pipe — and consume that data in a plain
loop. Each language implements a different one of the three classic
conversions, on purpose, because together they are the lesson.

The code is in `examples/12-signals-and-safe-handling/`. `./demo.sh` builds
all three implementations and drives a crash-restart-give-up cycle; the
`README.md` there specifies the CLI, the output lines, and the exit codes all
three languages share.

{% include excalidraw.html
   file="12-signal-delivery-paths"
   alt="Two bands. Kernel band: kill and child-exit sources feed a per-process disposition table and a per-thread mask, converging on an amber pending set that coalesces one bit per signal, then a dashed delivery box gated on unblocked. User band: three columns for C++ signalfd read, Go runtime handler into a signal.Notify channel, and Rust self-pipe handler setting an AtomicBool and writing a doorbell byte, each ending in the main loop reading plain data."
   caption="Figure 12.1 — every signal passes the same three kernel words — disposition, mask, pending — and pmon hears it three ways, none of them in signal context" %}

> **Tools used** — `strace`, `kill`, `sh`, `python3`, `/proc` (host);
> `bpftrace` (lab VM, exercised in Part 8). Everything here is checked by
> `scripts/check-host.sh`, ships with Fedora, or is preinstalled in the lab
> VMs.

## Disposition, mask, pending

The kernel's whole signal model fits in three words, and every bug in this
area comes from mixing them up.

The **disposition** is per-process: for each signal, one of `SIG_DFL` (the
default — terminate, ignore, stop, or dump core, depending on the signal),
`SIG_IGN`, or a handler function installed with `sigaction(2)`. Dispositions
survive `fork`; across `execve` a handler resets to `SIG_DFL` (the function
pointer would be meaningless in the new image) while `SIG_IGN` survives.

The **mask** is per-*thread*: the set of signals whose delivery is postponed.
A blocked signal is not lost — it waits. The mask survives both `fork` *and*
`exec`, which is a supervisor-shaped landmine: `pmon supervise` blocks
SIGTERM, and a forked child inherits that mask straight through `execvp`, so
the supervised program would start life unable to be terminated. This is why
`spawn` in all three implementations resets the mask in the child, between
`fork` and `exec` — the C++ spelling, from
`examples/12-signals-and-safe-handling/cpp/src/main.cpp`:

```cpp
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::execvp(argv[0], argv.data());
        constexpr char msg[] = "pmon: error: exec failed\n";
        [[maybe_unused]] auto n = ::write(STDERR_FILENO, msg, sizeof msg - 1);
        ::_exit(127);
    }
```

The **pending set** is where a generated-but-undelivered signal sits, and for
standard signals it is literally one bit each: a second SIGCHLD arriving
while one is already pending *coalesces* into the same bit. This is the core
of SIGCHLD semantics: the signal means "one **or more** children changed
state", never "exactly one". The robust consumer treats SIGCHLD as a doorbell
and asks `waitpid(…, WNOHANG)` what actually happened; `pmon` has exactly one
child at a time, so a targeted `waitpid(child, …, WNOHANG)` is the whole
answer — the general reap-until-empty loop arrives with pmon's later growth.
Delivery itself happens only when a signal is pending *and* unblocked in some
thread, at an arbitrary instant between two instructions — which is the
problem the next section is about.

## Why handlers are a minefield

A handler interrupts your program wherever it happens to be — possibly
halfway through `malloc`, holding the allocator's lock. If the handler then
calls `malloc` (or `printf`, or anything that might), the same thread waits
on a lock it already holds: deadlock, rarely, under load, in production.
That is why POSIX defines **async-signal-safety**: the short list of
functions guaranteed re-entrant against this kind of interruption —
`write`, `_exit`, `sigprocmask`, `kill`, `waitpid`, and a few dozen friends
(`man 7 signal-safety`). Not `malloc`. Not `printf`. Not most of any
language's runtime: locking a mutex, growing a Go stack, or formatting a
Rust string are all off the list. The compiler will not warn you; the
program will just deadlock or corrupt state on the unlucky interleaving.

The chapter's three implementations all draw the same conclusion: get out of
signal context immediately, running either nothing there (C++), or only
handler code someone already proved safe (Go's runtime, Rust's
`signal-hook`). The only place `pmon` itself must obey the list is the child
branch of `spawn` above — after `fork` in a program that may have threads
(Go always does), the child runs with only async-signal-safe calls until
`execvp`: `sigprocmask`, `execvp`, `write`, `_exit`. All four are on the
list, and nothing else happens there.

## The self-pipe trick, and signalfd as its resolution

The classic escape is the **self-pipe trick**: at startup, create a pipe;
the handler does exactly one safe thing — `write` one byte to it — and the
main loop, already blocked in `poll`, wakes up, drains the pipe, and handles
the event as ordinary code. The pipe is the bridge from signal context to
loop context, and `write(2)` is async-signal-safe. The Rust implementation
is deliberately the historical exhibit, with the unsafe parts delegated to
`signal-hook` — each signal is registered twice, a flag and a doorbell, from
`examples/12-signals-and-safe-handling/rust/src/main.rs`:

```rust
/// One registered signal: the flag half of the flag + doorbell pair. The
/// handler owns its dup of the pipe's write end as an OwnedFd; signal-hook
/// makes it non-blocking so the handler can never wedge on a full pipe.
fn flag_for(signal: i32, doorbell: &PipeWriter) -> Result<Arc<AtomicBool>> {
    let flag = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal, Arc::clone(&flag)).context("flag::register")?;
    let owned: OwnedFd = doorbell.as_fd().try_clone_to_owned().context("dup")?;
    signal_hook::low_level::pipe::register_raw(signal, owned)
        .context("pipe::register_raw")?;
    Ok(flag)
}
```

Linux then folded the trick into the kernel: `signalfd(2)` gives you a file
descriptor that *is* the pipe, with structured `signalfd_siginfo` records
instead of anonymous doorbell bytes. You block the signals with
`sigprocmask` — so normal delivery never happens — and read them like any
other fd. The C++ implementation opens with exactly that pair:

```cpp
    // Block the signals we care about, then receive them as fd reads.
    sigset_t mask;
    sigemptyset(&mask);
    for (const int signo : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) {
        sigaddset(&mask, signo);
    }
    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        die("sigprocmask", errno_ec());
    }
    UniqueFd sfd(::signalfd(-1, &mask, SFD_CLOEXEC));
    if (!sfd.valid()) {
        die("signalfd", errno_ec());
    }
```

No handler is ever installed; nothing asynchronous ever runs. Go is the
third design point: the runtime owns real handlers for almost every signal
(you will count them in the cross-check), and those handlers — written once,
by people who proved them safe — forward into the scheduler, where
`signal.Notify` turns delivery into a channel send:

```go
	sigCh := make(chan os.Signal, 8)
	signal.Notify(sigCh, unix.SIGTERM, unix.SIGINT, unix.SIGHUP)

	// One in-flight wait goroutine per live child; the buffered channel lets
	// it finish even when the supervisor consumes the exit via stopChild.
	waitCh := make(chan childExit, 1)
```

Three mechanisms, one shape: signal context shrinks to zero (C++), or to a
`write` and an atomic store (Rust), or to code you never wrote (Go), and
everything meaningful happens in a loop that reads plain values.

## Forwarding, and who gets Ctrl-C

`pmon` forwards shutdown explicitly: `kill(child_pid, SIGTERM)`, one target.
But when you press Ctrl-C in a terminal, the kernel sends SIGINT to the
foreground **process group** — pmon *and* its child both receive it, because
`spawn` leaves the child in pmon's group. That is a deliberate v1 choice,
and it composes: the child dies of its default SIGINT disposition, pmon
consumes its copy through its mechanism, reaps, prints the shutdown line —
the same path `kill -INT <pmon-pid>` exercises alone. The alternative
design, `setpgid` the child into its own group and signal `-pgid` to reach a
whole tree, is what shells do to implement job control; pmon does not need
it yet, and neither spelling can catch a child's *grandchildren* that
daemonize out of the group — the real fix for that is cgroups, in the
containers part.

{% include excalidraw.html
   file="12-supervisor-state-machine"
   alt="State machine: an amber child-running state with infinite poll timeout, a backoff-wait state with deadline poll timeout, an amber transition between them labeled with the doubling backoff, a return transition when the backoff elapses, exits to exit 0 on clean exit or SIGTERM/SIGINT and to exit 1 when restarts reach the maximum, and a dashed SIGHUP reload box that stops the child, resets the budget, and spawns a replacement."
   caption="Figure 12.2 — pmon supervise as a two-state machine; every transition is triggered by an ordinary value read in the main loop" %}

## How the code works

The supervisor state fits in four variables, identical in all three
languages: the current child's pid (or handle), a `running` flag (Go spells
it "is there a timer"), a restart counter checked against `--max-restarts`,
and the current backoff, which starts at `--backoff-ms` and doubles after
every crash. A crash moves the machine from *running* to *backoff wait*; the
backoff elapsing moves it back by spawning a fresh child; a clean exit,
SIGTERM, or SIGINT leaves through exit 0; exhausting the budget leaves
through exit 1; SIGHUP stays in *running* but swaps the child and resets
both counter and backoff — a reload should not inherit a crashing child's
penalties. Here is the consumption loop, three ways:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    for (;;) {
        int timeout = -1;
        if (!running) {
            const auto left = deadline - steady_clock::now();
            timeout =
                left <= 0ns
                    ? 0
                    : static_cast<int>(
                          duration_cast<milliseconds>(left).count()) +
                          1;
        }
        pollfd pfd{.fd = sfd.get(), .events = POLLIN, .revents = 0};
        const int nready = ::poll(&pfd, 1, timeout);
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll", errno_ec());
        }
        if (nready == 0) {
            // Backoff elapsed: bring the child back.
            child = start();
            running = true;
            continue;
        }

        signalfd_siginfo si{};
        const ssize_t nread = ::read(sfd.get(), &si, sizeof si);
        if (nread != static_cast<ssize_t>(sizeof si)) {
            die("read signalfd", errno_ec());
        }

        switch (si.ssi_signo) {
        case SIGTERM:
        case SIGINT: {
            if (running) {
                stop_child(child);
            }
            std::println("pmon: shutting down ({})",
                         si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT");
            return 0;
        }
        case SIGHUP: {
            std::println("pmon: reload requested");
            if (running) {
                stop_child(child);
            }
            restarts = 0;
            backoff = opts.backoff_ms;
            child = start();
            running = true;
            break;
        }
        case SIGCHLD: {
            int wstatus = 0;
            const pid_t reaped = ::waitpid(child, &wstatus, WNOHANG);
            if (!running || reaped != child) {
                break;  // stale notification (e.g. already reaped on reload)
            }
            const ChildExit ce = decode(wstatus);
            report(child, ce);
            if (!ce.signaled && ce.status == 0) {
                return 0;
            }
            if (restarts >= opts.max_restarts) {
                std::println("pmon: giving up after {} restarts",
                             opts.max_restarts);
                return 1;
            }
            ++restarts;
            std::println("pmon: restart #{} (backoff {}ms)", restarts, backoff);
            deadline = steady_clock::now() + milliseconds(backoff);
            backoff *= 2;
            running = false;
            break;
        }
        default:
            break;
        }
    }
```

```go
	for {
		var timerCh <-chan time.Time
		if timer != nil {
			timerCh = timer.C
		}
		select {
		case s := <-sigCh:
			switch s {
			case unix.SIGTERM, unix.SIGINT:
				if timer == nil {
					stopChild(cmd)
				}
				fmt.Printf("pmon: shutting down (%s)\n", signame(s))
				return 0
			case unix.SIGHUP:
				fmt.Println("pmon: reload requested")
				if timer == nil {
					stopChild(cmd)
				} else {
					timer.Stop()
					timer = nil
				}
				restarts = 0
				backoff = opts.backoffMs
				cmd = start()
			}
		case ce := <-waitCh:
			// The child exited on its own.
			report(cmd.Process.Pid, ce)
			if !ce.signaled && ce.status == 0 {
				return 0
			}
			if restarts >= opts.maxRestarts {
				fmt.Printf("pmon: giving up after %d restarts\n", opts.maxRestarts)
				return 1
			}
			restarts++
			fmt.Printf("pmon: restart #%d (backoff %dms)\n", restarts, backoff)
			timer = time.NewTimer(time.Duration(backoff) * time.Millisecond)
			backoff *= 2
		case <-timerCh:
			// Backoff elapsed: bring the child back.
			timer = nil
			cmd = start()
		}
	}
```

```rust
    loop {
        let timeout = if running {
            PollTimeout::NONE
        } else {
            let left = deadline.saturating_duration_since(Instant::now());
            let ms = left.as_millis().saturating_add(1).min(u16::MAX as u128);
            if left.is_zero() { PollTimeout::ZERO } else { PollTimeout::from(ms as u16) }
        };
        let nready = {
            let mut fds = [PollFd::new(reader.as_fd(), PollFlags::POLLIN)];
            match poll(&mut fds, timeout) {
                Ok(n) => n,
                Err(Errno::EINTR) => continue,
                Err(e) => return Err(e).context("poll"),
            }
        };
        if nready == 0 {
            if Instant::now() < deadline {
                continue; // clamped timeout; keep waiting
            }
            // Backoff elapsed: bring the child back.
            child = spawn(&opts.argv)?;
            running = true;
            continue;
        }

        // Drain the doorbell (poll said readable; coalesced bytes are fine).
        let mut buf = [0u8; 64];
        let _ = reader.read(&mut buf).context("read doorbell")?;

        let term = got_term.swap(false, Ordering::SeqCst);
        let intr = got_int.swap(false, Ordering::SeqCst);
        if term || intr {
            if running {
                stop_child(child)?;
            }
            println!(
                "pmon: shutting down ({})",
                if term { "SIGTERM" } else { "SIGINT" }
            );
            return Ok(0);
        }

        if got_hup.swap(false, Ordering::SeqCst) {
            println!("pmon: reload requested");
            if running {
                stop_child(child)?;
            }
            restarts = 0;
            backoff = opts.backoff_ms;
            child = spawn(&opts.argv)?;
            running = true;
            continue;
        }

        // SIGCHLD (or a stale doorbell byte): see whether our child exited.
        if !running {
            continue;
        }
        match waitpid(Pid::from_raw(child), Some(WaitPidFlag::WNOHANG)) {
            Ok(ws) => {
                let Some(ce) = decode_wait(ws) else { continue };
                report(child, ce);
                if ce.clean() {
                    return Ok(0);
                }
                if restarts >= opts.max_restarts {
                    println!("pmon: giving up after {} restarts", opts.max_restarts);
                    return Ok(1);
                }
                restarts += 1;
                println!("pmon: restart #{restarts} (backoff {backoff}ms)");
                deadline = Instant::now() + Duration::from_millis(backoff);
                backoff *= 2;
                running = false;
            }
            Err(Errno::EINTR) | Err(Errno::ECHILD) => continue,
            Err(e) => return Err(e).context("waitpid"),
        }
    }
```

Structure first: all three block on *one* wait point — `poll` on the
signalfd, `select` over three channels, `poll` on the doorbell — and the
backoff is not a `sleep` but that wait's *timeout* (Go uses a timer channel
for the same effect). That single decision keeps the supervisor responsive:
a SIGTERM arriving mid-backoff interrupts the wait instead of queuing behind
a sleep. In C++ and Rust the timeout is recomputed each iteration from an
absolute deadline — poll can return early for unrelated reasons, and
re-deriving "time left" from a deadline is immune to that; the `+ 1`
guards the truncation of a fractional millisecond so the loop cannot spin at
zero-timeout just before the deadline.

Three per-language details deserve a closer look. The C++ SIGCHLD arm
checks `reaped != child` and drops the record: on a reload, `stop_child`
already reaped the old child synchronously, but the SIGCHLD it generated is
still queued in the signalfd — a *stale notification*. The state machine
must be driven by `waitpid`'s answer, never by the signal's arrival; the
signal is a doorbell, remember, not a fact. Go's version of the same care
is `waitCh` having buffer size 1 with one wait-goroutine per child:
`stopChild` consumes the exit synchronously from the channel, so a reload
cannot leave a goroutine's send racing the next child's. Rust's is the
drain-then-check ordering — read up to 64 doorbell bytes (several signals
may have coalesced into one wakeup, exactly like the pending set), *then*
swap each `AtomicBool` to false, then let `waitpid(WNOHANG)` say whether
SIGCHLD meant anything; a doorbell byte with nothing behind it (`StillAlive`)
just loops.

Fragile bits, stated plainly: the doubling backoff is uncapped — with
`--backoff-ms 1` and `--max-restarts 40` the final wait would be 2³⁹ ms,
about seventeen years — so `--max-restarts` is the only brake, and a
production supervisor would clamp the backoff itself; Rust's `PollTimeout`
is a `u16`, so waits past 65.5 s are clamped and re-entered (the
`Instant::now() < deadline` check); and `run` (unchanged from v0) reaps with
the default mask and dispositions, so Ctrl-C during `pmon run` kills both
processes — supervision semantics belong to `supervise` only.

## Errors, three ways

The contract is unchanged from chapter 11 — `pmon: error: …` on stderr and
exit 1 for runtime failures, usage on exit 2 — but signals sharpen the
`EINTR` policy from chapter 4 into three distinct spellings. C++ retries
`poll` on `EINTR` explicitly, and `reap_blocking` loops `waitpid` the same
way; under `supervise` the mask makes those interruptions nearly impossible,
but `run` reaps with the default mask, so the loop is load-bearing there.
Rust matches `Errno::EINTR` as a value in both `poll` and `waitpid` — its
handlers really do interrupt syscalls, as the cross-check will show — and
additionally tolerates `Errno::ECHILD` ("no child to reap") as a stale
doorbell rather than an error. Go never sees `EINTR` at all: the runtime
installs every handler with `SA_RESTART` and retries interrupted waits
inside `cmd.Wait`, so user code only ever receives the final verdict.
One deliberate acceptance shows up in all three `stop_child`s: `ESRCH` from
`kill` means the child is already gone mid-shutdown — a race you expect,
handled at the call site, while everything else carries context up to one
printer, exactly the chapter-5 policy.

## Concurrency lens

Go's runtime is the loudest signal user in this book, and now we can catch
it working. Its handlers are installed with `sa_mask=~[]` (all signals
blocked while handling) and `SA_ONSTACK` (an alternate stack, so a handler
survives even a blown goroutine stack), and the runtime *sends itself*
signals: since Go 1.14, a goroutine stuck in a tight loop is preempted by a
`tgkill(…, SIGURG)` from another runtime thread. An idle supervisor never
needs that, but GC pressure forces it — `GOGC=1` makes collections constant,
and the trace shows runtime threads interrupting each other:

```bash
[host]$ GOGC=1 strace -f -o urg.trace -e trace=tgkill ./go/bin/app supervise \
        --max-restarts 8 --backoff-ms 1 -- sh -c 'exit 1' >/dev/null
[host]$ grep -m1 -A1 'SIGURG' urg.trace
2622691 tgkill(2622690, 2622690, SIGURG) = 0
2622690 --- SIGURG {si_signo=SIGURG, si_code=SI_TKILL, si_pid=2622690, si_uid=1000} ---
[host]$ grep -c -- '--- SIGURG' urg.trace
4
```

Four SIGURG deliveries in a supervisor that never asked for any signal —
your Go programs live in this weather all the time, which is why `os/exec`
retries `EINTR` for you and why you must never install a C signal handler
into a Go process. The same lens explains a signalfd caveat: the mask is
per-thread, and signalfd only sees signals *blocked* in every thread that
might otherwise take delivery. The C++ code blocks the mask before any
thread could exist, so the invariant holds trivially; in a threaded program
you must block-then-spawn so children inherit the mask. Rust's mechanism is
the only one where user-adjacent code truly runs in signal context, and its
concurrency budget is exactly one atomic store plus one non-blocking `write`
— a full doorbell drops the byte harmlessly, because bytes, like pending
bits, are allowed to coalesce.

## Build, run, observe

```bash
[host]$ cd examples/12-signals-and-safe-handling && ./demo.sh
```

Each language builds and runs the crash loop. By hand, from this session —
`run` still propagates verdicts exactly as v0 did:

```bash
[host]$ ./cpp/build/release/app run -- sh -c 'exit 3'; echo "exit=$?"
pmon: started pid 2616819
pmon: child 2616819 exited status=3
exit=3
[host]$ ./cpp/build/release/app run -- sh -c 'kill -9 $$'; echo "exit=$?"
pmon: started pid 2616821
pmon: child 2616821 killed signal=9
exit=137
[host]$ ./cpp/build/release/app supervise --max-restarts 2 --backoff-ms 60 -- sh -c 'exit 1'
pmon: started pid 2616933
pmon: child 2616933 exited status=1
pmon: restart #1 (backoff 60ms)
pmon: started pid 2616934
pmon: child 2616934 exited status=1
pmon: restart #2 (backoff 120ms)
pmon: started pid 2616935
pmon: child 2616935 exited status=1
pmon: giving up after 2 restarts
```

Three distinct pids, the backoff printed doubling, exit 1. Driving the Go
build with signals from another shell produces the reload-then-shutdown
story — note the replacement pid:

```bash
[host]$ ./go/bin/app supervise -- sleep 300 &
pmon: started pid 2625064
[host]$ kill -HUP <pmon-pid>
pmon: reload requested
pmon: started pid 2625068
[host]$ kill -TERM <pmon-pid>
pmon: shutting down (SIGTERM)
```

`verify.lua` asserts all of this behaviorally — 31 checks per language,
including measured backoff elapsed time and post-reload shutdown — and this
session's runner (`python3 scripts/test-all-examples.py --only
12-signals-and-safe-handling`) reports `PASS PASS PASS` for cpp, go, and
rust, with a standalone `LSP_LANG=cpp` run printing `PASS 31 / FAIL 0`.

## Cross-check, three ways

**The syscall shape, under `strace`.** If the three designs are real, the
traces must be unmistakably different. The C++ supervisor's *entire* signal
footprint for a one-restart run is ten lines — zero `rt_sigaction`, one
`signalfd4`:

```bash
[host]$ strace -o cpp.trace -e trace=rt_sigaction,rt_sigprocmask,signalfd4,poll,ppoll \
        ./cpp/build/release/app supervise --max-restarts 1 --backoff-ms 30 -- sh -c 'exit 1' >/dev/null
[host]$ cat cpp.trace
rt_sigprocmask(SIG_BLOCK, [HUP INT TERM CHLD], NULL, 8) = 0
signalfd4(-1, [HUP INT TERM CHLD], 8, SFD_CLOEXEC) = 3
rt_sigprocmask(SIG_BLOCK, ~[], [HUP INT TERM CHLD], 8) = 0
rt_sigprocmask(SIG_SETMASK, [HUP INT TERM CHLD], NULL, 8) = 0
poll([{fd=3, events=POLLIN}], 1, -1)    = 1 ([{fd=3, revents=POLLIN}])
poll([{fd=3, events=POLLIN}], 1, 30)    = 0 (Timeout)
rt_sigprocmask(SIG_BLOCK, ~[], [HUP INT TERM CHLD], 8) = 0
rt_sigprocmask(SIG_SETMASK, [HUP INT TERM CHLD], NULL, 8) = 0
poll([{fd=3, events=POLLIN}], 1, -1)    = 1 ([{fd=3, revents=POLLIN}])
+++ exited with 1 +++
```

(The block-all/restore pairs are glibc protecting its own `fork` internals —
not our code.) No `--- SIGCHLD ---` stops ever appear: blocked signals are
never delivered, they are *read*, and the 30 ms poll timeout is the backoff.
The same command with `-f` on the Go binary tells the opposite story: this
session's supervisor pid 2617196 issued exactly **114 `rt_sigaction`
calls** — 58 probes reading old dispositions plus 56 installs of the runtime
handler (`sa_handler=0x484d40`, `sa_mask=~[]`,
`SA_ONSTACK|SA_RESTART|SA_SIGINFO`) covering SIGHUP through `SIGRT_32` — the
identical 114-call storm chapter 4 counted for a program that never touched
`os/signal`, because it is runtime initialization, not `signal.Notify`. Zero
`signalfd4`, zero `poll`/`ppoll` (the runtime waits in `futex` and its
netpoller). Rust sits between: 13 `rt_sigaction` — probe+install pairs for
our four signals via `signal-hook`, plus `SIGPIPE` set to ignore and a
`SIGSEGV`/`SIGBUS` pair that std installs for stack-overflow reporting — a
`pipe2([3, 4], O_CLOEXEC)` for the doorbell, and then real deliveries
interrupting the wait, self-pipe mechanics live on the wire:

```
poll([{fd=3, events=POLLIN}], 1, -1)    = ? ERESTART_RESTARTBLOCK (Interrupted by signal)
--- SIGCHLD {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=2624617, si_uid=1000, si_status=1, ...} ---
poll([{fd=3, events=POLLIN}], 1, -1)    = 1 ([{fd=3, revents=POLLIN}])
```

**The kernel's bookkeeping, in `/proc`.** `/proc/<pid>/status` prints the
three words of this chapter as hex bitmasks — bit *n* is signal *n + 1*. A
live C++ supervisor from this session:

```bash
[host]$ ./cpp/build/release/app supervise -- sleep 30 &
pmon: started pid 2622949
[host]$ grep -E '^Sig(Pnd|Blk|Ign|Cgt)' /proc/<pmon-pid>/status
SigPnd:	0000000000000000
SigBlk:	0000000000014003
SigIgn:	0000000000000006
SigCgt:	0000000000000000
```

Decode `SigBlk` by hand: `0x14003` = bits 0, 1, 14, 16 = signals 1, 2, 15,
17 — SIGHUP, SIGINT, SIGTERM, SIGCHLD, precisely the `sigprocmask` set. And
`SigCgt: 0`: this process catches *nothing* — the whole design in one zero.
(`SigIgn: 6` — SIGINT and SIGQUIT — was inherited from the non-interactive
shell that launched this background job: dispositions cross `fork` and
`exec`, as promised.) The other two, decoded with three lines of Python
against the same live technique:

```bash
[host]$ python3 -c '
import signal
for name, hexmask in [("go SigCgt", "fffffffd7fc1feff"), ("rust SigCgt", "14443")]:
    m = int(hexmask, 16)
    sigs = [signal.Signals(b + 1).name for b in range(31) if m >> b & 1]
    print(name, "->", bin(m).count("1"), "caught;", " ".join(sigs[:6]), "...")'
go SigCgt -> 56 caught; SIGHUP SIGINT SIGQUIT SIGILL SIGTRAP SIGABRT ...
rust SigCgt -> 6 caught; SIGHUP SIGINT SIGBUS SIGSEGV SIGTERM SIGCHLD ...
```

Go catches 56 of 64 signals — everything except SIGKILL and SIGSTOP (which
no one may catch), the job-control quartet SIGCONT/SIGTSTP/SIGTTIN/SIGTTOU,
and two reserved realtime slots — while its `SigBlk` line
(`fffffffc3bba3a00` on this run) belongs to the main thread only; the mask
is per-thread, and the runtime retunes it constantly, which is the
`rt_sigprocmask` chatter in its trace. Rust's `0x14443` is our four signals
(`0x14003`, the same bits as the C++ *blocked* set — caught in one design,
blocked in the other) plus bits 6 and 10: SIGBUS and SIGSEGV, the std
stack-overflow guard the strace already showed. Three processes, one hex
format, and each design legible straight from the kernel's ledger.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the kernel-side view of this chapter is `bpftrace` on
> `tracepoint:signal:signal_generate` and `signal_deliver`, watching the
> pending-set handoff fleet-wide instead of per-process: which pid raised
> each SIGCHLD, where each SIGURG landed. That needs root and a disposable
> kernel, so it is not run here; chapter 30 (Debugging part) exercises
> exactly this on `systems-target`.

## What you learned

- A signal's life is three kernel words: a per-process **disposition**, a
  per-thread **mask** (which survives fork *and* exec — reset it in the
  child), and a coalescing **pending set** — which is why SIGCHLD means "one
  or more" and `waitpid(WNOHANG)` is the only source of truth.
- Handlers interrupt arbitrary code, so only the async-signal-safe list may
  run there; the professional designs shrink signal context to nothing:
  signalfd reads (`SigCgt: 0`), runtime-owned handlers feeding channels, or
  a self-pipe doorbell plus atomic flags.
- One blocking wait with the backoff as its *timeout* keeps a supervisor
  responsive to shutdown mid-backoff, and stale SIGCHLD notifications must
  be discarded by checking `waitpid`'s answer, not the signal's arrival.
- The three designs are visible from outside: 0 vs 114 vs 13 `rt_sigaction`
  calls, `signalfd4` vs a runtime `tgkill(…, SIGURG)` storm vs `--- SIGCHLD
  ---` interrupting `poll` — and `/proc/<pid>/status` decodes each one in
  hex.

Next, **pidfds**: the pid-reuse race this supervisor still carries — a
recycled pid can send our forwarded SIGTERM to a stranger — and the file
descriptor that pins a process's identity for good.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host this
session: the runner printed <code>12-signals-and-safe-handling  PASS  PASS
PASS</code> (3 passed, 0 failed, 0 skipped) and a standalone
<code>LSP_LANG=cpp</code> run printed <code>PASS 31 / FAIL 0</code>; the
<code>run</code>/<code>supervise</code> transcripts (exit 3 → 3, SIGKILL →
137, restarts at 60/120 ms, reload pid 2625064 → 2625068) are unedited
session output; the ten-line C++ strace is the complete filtered trace
(<code>signalfd4</code> = fd 3, zero <code>rt_sigaction</code>); the Go
supervisor pid made exactly 114 <code>rt_sigaction</code> calls (58 probes +
56 installs of <code>0x484d40</code>) and, under <code>GOGC=1</code>, 4
<code>tgkill(…, SIGURG)</code> sends with 4 matching deliveries; the Rust
trace showed 13 <code>rt_sigaction</code>, <code>pipe2(O_CLOEXEC)</code>,
and <code>ERESTART_RESTARTBLOCK</code> on <code>poll</code>; and the
<code>/proc/&lt;pid&gt;/status</code> masks (<code>SigBlk 14003</code> /
<code>SigCgt 0</code>, <code>SigCgt fffffffd7fc1feff</code>,
<code>SigCgt 14443</code>) were read from live supervisors and decoded with
the Python shown. The "On the lab VM" bpftrace callout is unverified as
marked and deferred to chapter 30.</p>
