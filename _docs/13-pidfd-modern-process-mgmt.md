---
title: "pidfd: modern process management"
order: 13
part: "Processes, Signals, Privilege"
description: "The pid-reuse race constructed step by step, then closed for good: pidfd_open, clone3(CLONE_PIDFD), poll on a process, waitid(P_PIDFD), and pidfd_send_signal become pmon v2's default engine — with strace evidence of what the Go runtime and the Rust stdlib really do with pidfds today."
duration: "45 minutes"
---

`pmon` v1 ended the last chapter supervising a crashing child correctly: no
async handlers, SIGCHLD arriving as data through a signalfd, a `WNOHANG` reap
classifying every death. But it still carries a lie it inherited from 1970s
UNIX — it names its child by an *integer*, and integers get reused. Between
the moment a child dies and the moment you act on its pid, that number can
come to mean a completely different process, and `kill(2)` will cheerfully
signal the stranger. The one new idea in this chapter is that a process can
be a *file descriptor*: `pidfd_open(2)` hands you an fd that refers to the
process itself, not to its number — pollable in exactly the event-loop style
Chapter 9 built, reapable with `waitid(2)`, signalable with
`pidfd_send_signal(2)`, and immune to reuse by construction. `pmon` v2 makes
that its default engine and keeps the v1 sigchld engine beside it,
byte-identical in output, so you can strace the difference.

The code is in `examples/13-pidfd-modern-process-mgmt/`. `./demo.sh` there
builds all three implementations and runs a short supervision demo; its
`README.md` specifies the CLI, the output contract, and the exit codes all
three languages share.

{% include excalidraw.html
   file="13-pid-reuse-race-pidfd"
   alt="Two horizontal timeline lanes. Top lane, five numbered steps: spawn remembering pid 4242, child crashes leaving a zombie, a wait reaps it freeing the pid, the kernel recycles 4242 for an unrelated process, and kill 4242 delivers SIGTERM to the stranger, with an amber dashed arrow noting same integer different process. Bottom lane, four numbered steps: spawn plus pidfd_open pinning child 4242 to pidfd 4, the child exit turning the fd readable under poll, waitid P_PIDFD reaping exactly the process behind the fd, and pidfd_send_signal returning ESRCH after death instead of hitting a recycled pid."
   caption="Figure 13.1 — the pid-reuse race in five numbered steps, and the pidfd lane that closes it; the fdinfo and strace annotations are from this chapter's real runs" %}

> **Tools used** — `strace`, `pgrep`, `man`, `ls`/`cat` over `/proc`,
> `gcc`, `rustc` (host); `exitsnoop`/`killsnoop` bcc-tools (lab VM, exercised
> in Part 8). Everything here is checked by `scripts/check-host.sh`, ships
> with Fedora, or is preinstalled in the lab VMs.

## The pid-reuse race, in five steps

Build the failure concretely, because every step is ordinary. (1) You spawn
a worker and remember its pid — say 4242. A pid is an index into a kernel
table, nothing more; on this host the table wraps at
`/proc/sys/kernel/pid_max` = 4194304. (2) The worker crashes. It becomes a
zombie: the pid stays reserved *only* until someone collects the exit
status. (3) Someone does — and here is the trap: in any program bigger than
a toy, "someone" is not always you. A library thread that spawns helpers, a
language runtime that reaps everything (Chapter 11 showed Go's runtime
holding exactly this power), a signal handler someone else installed — any
`wait()` in the process can free the pid. (4) The kernel recycles 4242 for
an unrelated process. Under default settings that needs the counter to wrap,
but a busy build server wraps in hours, and a container with a small
`pid_max` wraps in minutes. (5) You finally act on your saved integer —
`kill(4242, SIGTERM)` — and the signal lands on the stranger. Steps 3→5 are
the window in Figure 13.1; nothing you hold distinguishes old 4242 from new
4242, because an integer carries no identity.

One subtlety makes supervisors *seem* safe and is worth stating precisely: a
zombie's pid cannot be recycled, so as long as *you* are the parent and
*you* have not reaped yet, your pid is still good. The race needs a reap you
don't control — which is exactly what runtimes, PAM modules, and "helpful"
libraries do. And any pid-based operation on a process you are *not* the
parent of (a `kill` from a shell script, a `/proc/<pid>` poke from a
monitoring agent) has the race with no mitigation at all.

## Four syscalls that make a process a file

The fix arrived in a burst across Linux 5.1–5.4, and this host's manpages
(kernel `7.1.3-200.fc44`) document each piece. `pidfd_open(2)` — HISTORY:
"Linux 5.3" — "creates a file descriptor that refers to the process whose
PID is specified in pid"; the close-on-exec flag is set automatically, so
the Chapter 7 discipline costs nothing here. The fd's superpower is stated
under NOTES: it "can be monitored using poll(2), select(2), and epoll(7).
When the process that it refers to terminates, these interfaces indicate the
file descriptor as readable" — a process exit becomes an ordinary readiness
event, composable with every other fd your Chapter 9 event loop owns. There
is nothing to actually read: `read(2)` on a pidfd fails with `EINVAL`. The
manpage also warns that an fd obtained by opening `/proc/<pid>` — the older
trick — "is not pollable and can't be waited on with waitid(2)".

For your *own* children there is a spelling with no window at all:
`clone(2)`/`clone3(2)` with `CLONE_PIDFD` (since Linux 5.2) delivers the
pidfd atomically at spawn time, via `parent_tid` for `clone` or
`cl_args.pidfd` for `clone3` — the same "set the property at creation, not
after" move as `O_CLOEXEC` in Chapter 7. Acting through the fd:
`pidfd_send_signal(2)` (Linux 5.1) "sends the signal sig to the target
process referred to by pidfd", and `waitid(2)` grew an ID type
`P_PIDFD` (Linux 5.4) to "wait for the child referred to by the PID file
descriptor". Once the process behind the fd has been reaped, signalling it
yields `ESRCH` — a clean error instead of a delivered signal to a recycled
pid, which is the whole point of the bottom lane of Figure 13.1.

One documentation trap, caught live: this host's `pidfd_open` manpage still
says "glibc provides no wrapper for pidfd_open(), necessitating the use of
syscall(2)" — but a one-line probe built with `gcc` against the installed
glibc 2.43 (`#include <sys/pidfd.h>` … `pidfd_open(1,0)`) compiles *and
links*.
Manpages lag libraries. The C++ example keeps the raw
`syscall(SYS_pidfd_open, …)` anyway so it builds against older glibc too,
and because seeing the syscall spelled out is the point of the chapter.

## What the runtimes do today

Claims about runtime internals rot fast, so here is only what `strace -f`
showed on *this* host, running *these* binaries. Go 1.26.5 is a pidfd
enthusiast: before its first spawn, the runtime probes the whole feature
set — `pidfd_open` on its own pid, a `waitid(P_PIDFD, …)` that must fail
`ECHILD`, a `pidfd_send_signal(fd, 0, NULL, 0)` no-op, and then a throwaway
no-op child cloned with `CLONE_PIDFD` and reaped through its fd. Every
subsequent `exec.Cmd` child is spawned as
`clone(CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD, parent_tid=[6])` — the
runtime keeps its *own* pidfd for every child it starts, acquired atomically
at clone time, and `os.Process` documents using it for `Wait` and `Signal`
when available. Rust 1.97.1's stdlib, by contrast, spawns with a plain
`fork`-flavored `clone(CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD)` and
issues no pidfd calls of its own; the integration exists
(`Command::create_pidfd`, `std::os::linux::process::PidFd`) but compiling a
probe against it on this stable toolchain fails with `E0658: use of unstable
library feature 'linux_pidfd'`, pointing at tracking issue #82971. That is
the verified line: Go's runtime rides pidfds invisibly; stable Rust makes
you call `pidfd_open` yourself (our example uses `rustix`); where either
lands next is a research note, not something this chapter will claim.

## How the code works

The structure is shared across the three implementations. A `ChildStatus`
(`{exited, value}`) is the reaped truth about one child; `report_exit`
formats it identically everywhere and computes the mirrored exit code
(`status` as-is, `128+signo` for a signal death). An `Engine` selects one of
two wait strategies behind one restart policy; an `Outcome` says which of
the two things a wait produced — a reaped status, or a stop request
(`signal` or `timeout`). C++ spells that as
`std::variant<ChildStatus, Stop>`, Rust as an `enum Outcome`, and Go — with
`select` as its native "whichever happens first" — doesn't need the type at
all. The C++ `Fd` owner is the same class Chapter 7 built; Rust gets
`OwnedFd` straight from `rustix::process::pidfd_open`, and Go holds the raw
integer from `unix.PidfdOpen` with an explicit `unix.Close`.

Spawning carries one landmine the sigchld engine planted: the supervisor
blocks SIGINT/SIGTERM (and SIGCHLD, in the sigchld engine) process-wide so
its signalfd can deliver them as data — and a blocked mask is *inherited*
across fork and exec. A supervised `sleep` that inherits a blocked SIGTERM
can never be stopped. Each language defuses it differently: C++
`posix_spawnp` with `POSIX_SPAWN_SETSIGMASK` resets the child's mask
declaratively; Rust clears it in a `pre_exec` closure (`sigprocmask` is
async-signal-safe, so the closure is legal); Go's runtime resets the mask
for `exec.Cmd` children on its own.

The pidfd engine's wait is where the chapter's idea becomes code:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
{% raw %}// waitid(P_PIDFD) reaps the process the fd refers to — never a recycled pid.
[[nodiscard]] std::expected<ChildStatus, std::error_code> reap_pidfd(const Fd& pidfd) {
    siginfo_t si{};
    while (::waitid(P_PIDFD, static_cast<id_t>(pidfd.get()), &si, WEXITED) != 0) {
        if (errno != EINTR) {
            return std::unexpected(last_error());
        }
    }
    return status_from(si);
}

// pidfd engine: the child's exit is just readability on a file descriptor.
[[nodiscard]] std::expected<Outcome, std::error_code>
wait_pidfd(const Fd& pidfd, const Fd& sigfd, steady_clock::time_point deadline) {
    for (;;) {
        std::array<pollfd, 2> fds{{
            {.fd = pidfd.get(), .events = POLLIN, .revents = 0},
            {.fd = sigfd.get(), .events = POLLIN, .revents = 0},
        }};
        const int n = ::poll(fds.data(), fds.size(), remaining_ms(deadline));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(last_error());
        }
        if (n == 0) {
            return Stop::timeout;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            signalfd_siginfo info{};
            std::ignore = ::read(sigfd.get(), &info, sizeof info);
            return Stop::signal;
        }
        if (fds[0].revents != 0) {
            return reap_pidfd(pidfd).transform([](ChildStatus st) { return Outcome{st}; });
        }
    }
}{% endraw %}
```

```go
{% raw %}		if engine == "pidfd" {
			pidfd, err := unix.PidfdOpen(pid, 0)
			if err != nil {
				fmt.Fprintf(os.Stderr, "pmon: pidfd_open: %v\n", err)
				return 1
			}
			fmt.Fprintf(os.Stderr, "pmon: engine=pidfd child=%d pidfd=%d\n", pid, pidfd)
			exited := make(chan struct{})
			go func() { // the pidfd becomes readable exactly when the child exits
				fds := []unix.PollFd{{Fd: int32(pidfd), Events: unix.POLLIN}}
				for {
					if _, err := unix.Poll(fds, -1); err == unix.EINTR {
						continue
					}
					close(exited)
					return
				}
			}()
			select {
			case <-exited:
			case <-stop:
				stopped = "signal"
			case <-deadline:
				stopped = "timeout"
			}
			if stopped != "" {
				_ = unix.PidfdSendSignal(pidfd, unix.SIGTERM, nil, 0)
				<-exited
			}
			st, err = reapPidfd(pidfd)
			unix.Close(pidfd)
			if err != nil {
				fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
				return 1
			}
		}{% endraw %}
```

```rust
/// waitid(P_PIDFD) reaps the process the fd refers to — never a recycled pid.
fn reap_pidfd(pidfd: &impl AsFd) -> Result<ChildStatus> {
    let status = waitid(WaitId::PidFd(pidfd.as_fd()), WaitIdOptions::EXITED)
        .context("waitid(P_PIDFD)")?
        .context("waitid(P_PIDFD): no status")?;
    Ok(status_from_waitid(status))
}

/// pidfd engine: the child's exit is just readability on a file descriptor.
fn wait_pidfd(pidfd: &impl AsFd, sigfd: &SignalFd, deadline: Instant) -> Result<Outcome> {
    loop {
        let mut fds = [
            PollFd::new(pidfd, PollFlags::IN),
            PollFd::new(sigfd, PollFlags::IN),
        ];
        let n = match poll(&mut fds, Some(&remaining(deadline))) {
            Err(rustix::io::Errno::INTR) => continue,
            other => other.context("poll")?,
        };
        if n == 0 {
            return Ok(Outcome::Stopped(Stop::Timeout));
        }
        if fds[1].revents().contains(PollFlags::IN) {
            let _ = sigfd.read_signal();
            return Ok(Outcome::Stopped(Stop::Signal));
        }
        if !fds[0].revents().is_empty() {
            return Ok(Outcome::Reaped(reap_pidfd(pidfd)?));
        }
    }
}
```

Walk the C++ and Rust versions first, because they are the same function.
`poll` watches two fds — the pidfd and the signalfd — with a timeout derived
from the supervision deadline. `remaining_ms`/`remaining` recompute the
budget on every iteration precisely because `poll` can return early
(`EINTR`, a signalfd wakeup that turns out to be SIGCHLD-free); a fixed
timeout would stretch the deadline every retry. A return of 0 is the
deadline itself. The signalfd is checked *before* the pidfd deliberately: if
the operator's Ctrl-C and the child's exit arrive together, we prefer to
report a stop — the reap still happens on the stop path, so nothing is lost.
Only then does readability on `fds[0]` mean "the child terminated", and
`reap_pidfd` collects the status with `waitid(P_PIDFD, fd, …, WEXITED)` —
naming the child *by the fd*, so even in a process where some library reaps
promiscuously, this call can never collect a stranger. `status_from`
classifies `si_code == CLD_EXITED` versus killed/dumped, the same siginfo
plumbing as Chapter 11.

Go cannot put a channel in a `poll` set, so it inverts the bridge: one
goroutine parks in `unix.Poll` on the pidfd alone, infinite timeout, and
`close(exited)` converts readability into channel-readability. The `select`
then merges three event sources — exit, `signal.Notify` delivery, deadline —
exactly as the C++/Rust `poll` merged two fds and a timeout. On a stop, the
`<-exited` after `PidfdSendSignal` is load-bearing twice over: it waits for
the child to actually die (the pidfd goes readable), and it guarantees the
goroutine has finished touching `pidfd` before `unix.Close(pidfd)` runs —
close an fd a goroutine is still polling and the slot number could be
recycled mid-poll, which is Chapter 7's double-close lesson wearing a
concurrency hat.

The stop path is the syscall the diagram promised:
`pidfd_send_signal(pidfd, SIGTERM, …)` in all three languages, result
deliberately ignored. If the child died between the deadline and the signal,
the target is a zombie (signal discarded, returns 0) or already reaped
(`ESRCH`) — both harmless, and `reap_pidfd` right after collects the truth
either way. The restart policy above all this is unchanged from v1: a clean
`status=0` ends supervision, anything else burns one of `--max-restarts`,
and the counter lines (`restart 1/2` … `giving up after 2 restarts`) are
byte-identical across engines and languages — `verify.lua` literally diffs
the sequences against each other.

One fragile bit, documented rather than hidden: `pidfd_open` after spawn has
a theoretical window that `CLONE_PIDFD` at clone time would close. It is
safe *here* because `pmon` is the parent and reaps only through the fd, so
the child's pid is pinned (zombie at worst) until after the pidfd exists —
but the moment you take pidfds of processes you did not spawn, insist on
verifying identity (the cross-check below shows `fdinfo`'s `Pid:` line) or
get the fd at clone time the way Go's runtime does.

## Errors, three ways

The retry policy from Chapter 5 lands on three calls here: `poll` and
`waitid` retry `EINTR` in a loop in C++ (`errno != EINTR` breaks), Go
(`errno == unix.EINTR { continue }` around `Syscall6`, and the poller
goroutine's `err == unix.EINTR`), and Rust
(`Err(rustix::io::Errno::INTR) => continue`). Expected failures are policy
at the call site: `pidfd_send_signal` on the stop path ignores its result
(zombie or `ESRCH`, both fine), and the sigchld engine treats a `WNOHANG`
reap that returns nobody as "coalesced SIGCHLD, keep waiting" — Chapter 12's
lesson encoded in all three languages. Everything unexpected carries context
up to one printer with the raw error preserved: C++ wraps `errno` in
`std::error_code` inside `std::expected`, Go wraps with `%w` so
`errors.Is`/`errors.As` still see the errno underneath, Rust chains
`.context("waitid(P_PIDFD)")` onto the `rustix` errno with `anyhow`. And one
error is a *feature test*: Go's runtime probe expects `waitid(P_PIDFD)` on
itself to fail `ECHILD` — using a precise errno as a yes/no answer about
kernel support, the same trick your own code can use on kernels older than
5.4.

## Concurrency lens

C++ and Rust run this whole chapter on one thread: one `poll`, two fds, no
shared state, no reentrancy — the safest concurrency is none. Go runs the
same logic as a small concurrent system: the runtime's own reaper machinery,
a poller goroutine per spawn, and a `select` merging channels. That shape
has two sharp edges the code steps around. First, fd lifetime across
goroutines — covered above; the `<-exited` handshake is what makes
`unix.Close` safe. Second, process-wide signal state: the blocked mask that
powers the C++/Rust signalfd is shared by *all* threads, which is why the
child-side mask reset (`POSIX_SPAWN_SETSIGMASK`, `pre_exec`) is
non-negotiable — any thread may be the one that spawns. Note also what the
strace shows about fd-table behavior under concurrency: every spawn prints
`pidfd=4` because the previous pidfd was closed before the next
`pidfd_open`, and Chapter 7's lowest-free-slot rule hands the number back —
while in Go the runtime's *own* pidfd for the same child sits one slot over.
Two fds, one process, no interference: identity multiplied is still
identity.

## Build, run, observe

```bash
[host]$ cd examples/13-pidfd-modern-process-mgmt && ./demo.sh
```

Each language builds and supervises a short-lived child. The chapter's two
canonical runs, from this host — a crash loop that exhausts its restart
budget:

```bash
[host]$ ./demo.sh cpp run supervise --max-restarts 2 -- sh -c 'exit 1'
pmon: engine=pidfd child=2621527 pidfd=4
pmon: child=2621527 exited status=1
pmon: restart 1/2
pmon: engine=pidfd child=2621528 pidfd=4
pmon: child=2621528 exited status=1
pmon: restart 2/2
pmon: engine=pidfd child=2621529 pidfd=4
pmon: child=2621529 exited status=1
pmon: giving up after 2 restarts
```

— exit code 1, three spawns, `pidfd=4` every time. And a timeout stop that
must not leak the child:

```bash
[host]$ ./demo.sh go run supervise --timeout-ms 400 -- sleep 9.99
pmon: engine=pidfd child=2621961 pidfd=4
pmon: child=2621961 killed signal=15
pmon: exiting (timeout)
[host]$ pgrep -af 'sleep 9[.]99'; echo "pgrep exit=$?"
pgrep exit=1
```

The `pgrep` exit 1 is the observable that matters: no supervised `sleep`
outlived its supervisor. (The `[.]` keeps `pgrep -f` from matching its own
command line — a pid-namespace-sized footgun in miniature.) The Rust binary
produces the same crash-loop shape (children 2622026–2622028 on this host),
and the runner — `python3 scripts/test-all-examples.py --only
13-pidfd-modern-process-mgmt` — reports `PASS PASS PASS` with `verify.lua`'s
behavioral checks at `PASS 37 / FAIL 0` per language.

## Cross-check, three ways

**The syscalls, under `strace`.** Filtering each engine to the calls this
chapter claims it makes. C++ (glibc's `posix_spawnp` arrives as `clone3`):

```bash
[host]$ strace -f -e trace=pidfd_open,clone3,waitid,poll \
        ./cpp/build/release/app supervise --max-restarts 0 -- sh -c 'exit 1'
clone3({flags=CLONE_VM|CLONE_VFORK|CLONE_CLEAR_SIGHAND, exit_signal=SIGCHLD, ...}, 88) = 2622912
pidfd_open(2622912, 0)          = 4
poll([{fd=4, events=POLLIN}, {fd=3, events=POLLIN}], 2, 9999) = 1 ([{fd=4, revents=POLLIN}])
waitid(P_PIDFD, 4, {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=2622912, si_uid=1000, si_status=1, ...}, WEXITED, NULL) = 0
```

The stop path adds the last syscall of the set —
`poll(..., 299) = 0 (Timeout)`, then `pidfd_send_signal(4, SIGTERM, NULL, 0)
= 0`, then the same `waitid(P_PIDFD, 4, …)` now reporting `CLD_KILLED` with
`si_status=SIGTERM`. Go shows the runtime's pidfd life underneath ours
(trimmed to the interesting lines, thread pids left in):

```
2623684 pidfd_open(2623681, 0)          = 4          ← probe: pidfd of itself
2623684 waitid(P_PIDFD, 4, ...) = -1 ECHILD          ← probe: waitid support
2623684 pidfd_send_signal(4, 0, NULL, 0) = 0         ← probe: signal 0
2623684 clone(child_stack=NULL, flags=CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD, parent_tid=[6]) = 2623689
2623684 pidfd_open(2623689, 0)          = 4          ← ours, via unix.PidfdOpen
2623684 waitid(P_PIDFD, 4, {..., si_pid=2623689, si_status=1, ...}, WEXITED, NULL) = 0
```

`parent_tid=[6]` is the runtime's own pidfd for the child, delivered
atomically by `CLONE_PIDFD`; fd 4 is ours. Rust spawns with a plain
`clone(CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD)` — no stdlib pidfd —
followed by our `pidfd_open(2623708, 0) = 4` and the identical
`waitid(P_PIDFD, …)`. Three runtimes, three spawn spellings, one kernel API.

**The fd itself, in `/proc`.** Pause a live supervision
(`supervise --timeout-ms 8000 -- sleep 6.66`, supervisor pid 2624388) and
read the fd table the way Chapter 7 taught:

```bash
[host]$ ls -l /proc/2624388/fd | tail -2
lrwx------ 1 rsedor rsedor 64 Jul 18 23:59 3 -> anon_inode:[signalfd]
lrwx------ 1 rsedor rsedor 64 Jul 18 23:59 4 -> anon_inode:[pidfd]
[host]$ cat /proc/2624388/fdinfo/4
pos:	0
flags:	02000002
mnt_id:	5
ino:	2596278
Pid:	2624390
NSpid:	2624390
```

The `Pid: 2624390` line is the payoff quoted directly from the kernel: the
open file description *itself* records which process it refers to, and it
matches the `child=2624390` that `pmon` announced. `flags: 02000002` is
`O_RDWR|O_CLOEXEC` — the close-on-exec bit the manpage promised, set with no
code on our side. (`NSpid` previews Part 10: inside a pid namespace the same
process has a different number, but the same pidfd.)

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet-wide view of this chapter is bcc-tools: `exitsnoop` timestamping
> every process exit and `killsnoop` logging who signalled whom, which would
> show `pmon`'s SIGTERM (and would *not* show a pid-reuse mishit, because
> there is none). bcc-tools need root and kernel headers, so they are not run
> on this host; Chapter 30 in the Debugging part exercises them on the
> `systems-target` VM.

## What you learned

- A pid is an index that gets recycled; the reuse race is five ordinary
  steps, and the dangerous reap in step 3 is often performed by a runtime or
  library you don't control.
- `pidfd_open` (or `CLONE_PIDFD` at clone time) pins process identity to an
  fd: exit becomes `poll` readability — Chapter 9's event-loop model now
  covers processes — `waitid(P_PIDFD)` reaps exactly that process, and
  `pidfd_send_signal` yields `ESRCH` rather than signalling a stranger;
  `/proc/<pid>/fdinfo/<fd>`'s `Pid:` line names the pinned process.
- The runtimes diverge today, verified by strace on this host: Go 1.26
  probes and adopts `CLONE_PIDFD` for every child it spawns, while stable
  Rust 1.97 spawns with fork-style `clone` and keeps its pidfd API behind
  the unstable `linux_pidfd` feature — so our Rust and C++ engines call the
  syscalls themselves.
- Signal masks are inherited across exec: a supervisor that blocks signals
  for a signalfd must reset the child's mask (`POSIX_SPAWN_SETSIGMASK`,
  `pre_exec`) or its children become unstoppable.

Next, **identity and privilege**: who a process *is* — real, effective, and
saved uids, capability sets, and how `pmon` drops root without dropping the
ability to do its job.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host
(kernel 7.1.3-200.fc44) this session: the runner printed
<code>13-pidfd-modern-process-mgmt  PASS  PASS  PASS</code> (3 passed, 0
failed, 0 skipped) and a direct <code>verify.lua</code> run showed
<code>PASS 37 / FAIL 0</code>; the crash-loop and timeout transcripts are
real output (children 2621527–2621529, 2622026–2622028, 2621961), with the
no-orphan <code>pgrep</code> exit 1 reproduced; the strace excerpts show the
real <code>clone3</code>/<code>clone(CLONE_PIDFD)</code>/fork-style spawn
paths, Go's four-step runtime probe, <code>pidfd_open</code> returning 4,
<code>poll</code> readiness, <code>waitid(P_PIDFD)</code> siginfo, and
<code>pidfd_send_signal</code> on the stop path; the live
<code>fdinfo</code> read out <code>Pid: 2624390</code> matching the
announced child; the glibc-wrapper probe compiled and linked against glibc
2.43, and the <code>create_pidfd</code> probe failed with E0658 on rustc
1.97.1. Manpage quotes are from this host's pages. The bcc-tools callout is
unverified as marked and deferred to Part 8.</p>
