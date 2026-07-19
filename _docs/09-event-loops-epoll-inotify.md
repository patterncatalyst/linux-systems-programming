---
title: "Event loops: epoll and inotify"
order: 9
part: "Files and I/O"
description: "The readiness model — nonblocking fds multiplexed by one epoll loop, level versus edge triggering, inotify's per-watch queues and overflow, and timerfd/signalfd as ordinary readable fds — as fwatch v1 grows a real-time watch subcommand and Go's select turns out to be the same kernel machinery wearing channels."
duration: 50 minutes
---

Chapter 8 closed on a question durability cannot answer: "which of ten
thousand fds is ready?" This chapter answers it with the **readiness model**
— the kernel will tell you *when an fd has something for you*, and a program
built on that promise never blocks anywhere except the one call that asks.
`fwatch` v1 keeps the polling-era `snapshot` and `diff` subcommands from
Chapters 7 and 8 and adds `watch`: a real-time watcher that reports
`created`/`modified`/`deleted` per file, debounced 100 ms per path. The C++
and Rust versions build it the explicit way — one `epoll` loop over three
kernel fds (inotify, timerfd, signalfd), a single thread, nothing hidden —
while the Go version inverts the picture: the runtime's netpoller owns the
epoll loop, and the same three event sources surface as channels in a
`select`. Same kernel machinery, two programming surfaces, and by the end
you will have watched both under `strace` and read epoll's own interest list
out of `/proc`.

The code is in `examples/09-event-loops-epoll-inotify/`. `./demo.sh` there
builds all three implementations and runs a 1-second self-demo; its
`README.md` specifies the CLI, the debounce/coalescing rules, and the exit
codes all three languages share.

{% include excalidraw.html
   file="09-event-loop-anatomy"
   alt="Two bands. In the user-space band, four loop steps run left to right — arm timerfd, epoll_wait, dispatch on the data token, drain and act — with a return arrow labeled loop. In the kernel band, an eventpoll interest-list box holds tfd 3, 4, 5, fed by three source boxes: the inotify fd with wd 1 and mask 0x3c6, the CLOCK_MONOTONIC timerfd, and the signalfd carrying blocked SIGINT and SIGTERM; an amber readiness arrow carries EPOLLIN and the token back up into epoll_wait."
   caption="Figure 9.1 — the anatomy of fwatch watch: one loop, three event sources; every fd number, token, and mask in this figure reappears verbatim in the /proc inspection later in the chapter" %}

> **Tools used** — `strace`, `mktemp`, `stat`, `ls`, `python3` (host);
> `bpftrace` (lab VM, exercised in Part 8). Everything here is checked by
> `scripts/check-host.sh`, ships with Fedora, or is preinstalled in the lab VMs.

## The readiness model

A blocking `read(2)` couples two things that have no business being coupled:
*waiting* for data and *moving* it. With one input source that is fine; with
three, a blocking read on the wrong fd deadlocks you — while you wait for a
file event, the timeout you promised never fires. The classic escape was one
thread per source; the scalable escape is to decouple: put every fd in
**nonblocking** mode (`IN_NONBLOCK`, `TFD_NONBLOCK`, `SFD_NONBLOCK` — all
`O_NONBLOCK` under different names), so `read` either returns data
immediately or fails with `EAGAIN`, and delegate all the *waiting* to one
syscall that can wait on everything at once.

That syscall is `epoll_wait(2)`, and its shape explains its scalability. You
create a kernel object with `epoll_create1(2)` — it is itself an fd, of type
`anon_inode:[eventpoll]` — then *register* interest once per fd with
`epoll_ctl(EPOLL_CTL_ADD, …)`. The registration carries two things: an event
mask (`EPOLLIN` — readable) and a caller-chosen 64-bit cookie
(`epoll_event.data`) the kernel returns verbatim when that fd fires. The
interest list lives **in the kernel**, which is the whole advantage over the
older `poll(2)`/`select(2)`: those pass the entire fd set on every call, so
each wait is O(fds); epoll pays the setup cost once and each wakeup returns
only the fds that are actually ready. `fwatch` has three fds so any of them
would work — the point of learning epoll on three is that the loop is
*identical* at thirty thousand, which is where Part 6's network servers take
it.

## Level, edge, and the ONESHOT caveat

Registration has one decision that changes program correctness, not just
performance: **when** does a ready fd count as ready again?

- **Level-triggered** (the default): `epoll_wait` reports the fd as long as
  data *remains* — a fact about the fd's state. Forget to read everything and
  the next wait simply reports it again.
- **Edge-triggered** (`EPOLLET`): the fd is reported only on the
  *transition* to readable — a fact about a state change. Miss data and no
  further wakeup comes until *new* data arrives; the only correct consumer
  is one that drains the fd to `EAGAIN` on every wakeup, without exception.

This book uses level triggering everywhere it writes an epoll loop, because
level converts a whole class of lost-wakeup bugs into mere extra wakeups.
The discipline it demands instead is smaller but real: **consume what made
the fd readable**. That is why the loop below reads the timerfd's 8-byte
expiration counter even though it only wants "the timer fired" — leave those
8 bytes and the timerfd stays readable, `epoll_wait` returns instantly
forever, and your loop spins at 100% CPU. Note that `fwatch` drains its
inotify fd to `EAGAIN` anyway: with level triggering that is an optimization
(fewer wakeups per burst, and each burst is timestamped coherently); under
edge it would be the difference between working and silently stalling.

> **EPOLLONESHOT caveat** — there is a third mode you should know exists
> before you need it. When *multiple threads* call `epoll_wait` on one epoll
> fd, a level-triggered fd that still has data can be reported to two
> threads at once, and even an edge can race a second event while thread one
> is mid-read. `EPOLLONESHOT` disarms the registration after each report;
> the owning thread must re-arm with `EPOLL_CTL_MOD` when done. It is the
> standard fix for multi-threaded epoll — and the reason this book's
> single-threaded loops do not need it. One loop, one thread, no re-arm
> protocol: that simplicity is a feature, not a limitation.

Interestingly, the readiness style is observable per implementation: the
strace later in this chapter shows Go's netpoller registering our inotify fd
with `EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET` — the runtime is an
edge-triggered consumer (it always drains to `EAGAIN` and parks goroutines
on the transition), while our C++ and Rust loops register plain level
`EPOLLIN`.

## inotify: per-watch queues, and where they overflow

`inotify_init1(2)` returns an fd; `inotify_add_watch(2)` attaches a **watch**
to an inode — directory or file — with a mask of event types, and returns a
small integer *watch descriptor* (`wd`) that names the watch in every event.
`fwatch` watches one directory with six mask bits: `IN_CREATE`, `IN_MODIFY`,
`IN_ATTRIB`, `IN_DELETE`, `IN_MOVED_FROM`, `IN_MOVED_TO` — together `0x3c6`,
a number you will meet again in `/proc`. Reading the fd yields packed
variable-length records — `wd`, `mask`, `cookie`, `len`, then `len` bytes of
NUL-padded name — so all three implementations walk a byte buffer, stepping
by `sizeof(header) + len`. A record with `len == 0` is an event on the
watched object itself (we skip those), and a rename arrives as a
`IN_MOVED_FROM`/`IN_MOVED_TO` pair joined by `cookie`.

Semantics worth being precise about: events queue **per inotify instance**,
in order, and the kernel already coalesces *identical adjacent* events. The
queue is finite — on this host `/proc/sys/fs/inotify/max_queued_events` is
16384 (with `max_user_watches` 524288 and `max_user_instances` 512) — and
when it fills, the kernel does not block the writer and does not drop
silently: it appends one `IN_Q_OVERFLOW` event and discards until you catch
up. A correct watcher treats `IN_Q_OVERFLOW` as "my view is stale,
re-scan" — which is exactly why `fwatch` kept its `snapshot`/`diff`
subcommands: the polling machinery from Chapters 7 and 8 is the recovery
path for the event machinery of Chapter 9. Also note what a directory watch does
*not* give you: recursion. Subdirectories need their own watches — the cost
model behind editors and sync clients burning thousands of `wd`s.

## Everything is an fd: timerfd and signalfd

The elegance of the readiness model is that anything that can *become ready*
can join the loop, and Linux has spent two decades converting non-fd events
into fds. `fwatch` uses two of them. `timerfd_create(2)` gives a timer whose
expiration makes an fd readable; reading it returns an 8-byte count of
expirations since the last read. The C++ and Rust loops run exactly **one**
timer for two jobs — each iteration re-arms it (one-shot) to
`min(overall deadline, earliest debounce due)`, so whichever deadline is
nearest is the one wakeup that gets scheduled. One subtlety hides in
`arm_timer`: an `it_value` of all zeros means *disarm*, so a deadline that
is already past is clamped to one nanosecond — "fire immediately" and "never
fire" are one bit pattern apart.

`signalfd(2)` does the same for signals — with a prerequisite that trips
everyone once: you must **block** the signals first (`sigprocmask(2)`),
otherwise they are still delivered asynchronously to a handler and the fd
reads nothing. Blocked-then-read, SIGINT stops being a control-flow
interruption and becomes data: a `signalfd_siginfo` record you handle at a
place *you* chose, after the current batch, with no reentrancy rules and no
`volatile sig_atomic_t` flag. The loop still guards `EINTR` anyway
(any signal outside the blocked set can still interrupt `epoll_wait`), but
the shutdown path itself is just another `case` in the dispatch.

## How the code works

Two data structures carry the whole design. `pending` — `std::map<std::string,
Pending>` in C++, `BTreeMap<String, Pending>` in Rust, `map[string]*pendingEvent`
in Go — maps each path to its coalesced event kind and its debounce deadline
(`due = now + 100ms`); ordered maps are chosen deliberately so a batch flush
prints in name order and `verify.lua` can assert output deterministically
(Go sorts the keys at flush time instead). The second structure is the token:
C++ registers each fd with `data.u32` set to `tok_inotify`/`tok_timer`/
`tok_signal` (Rust: `TOK_INOTIFY = 0` … `TOK_SIGNAL = 2`), so dispatch is a
`switch` on a number the kernel echoes back — no fd comparisons, no lookup
table. Coalescing is one shared `merge` rule: within a window, delete wins,
a fresh create stays `created` through later writes, and delete-then-recreate
reads as `modified` — the same visible truth `diff` would have reported.

Setup is four fallible constructions — `inotify_init1` + `inotify_add_watch`,
`timerfd_create`, `sigprocmask` + `signalfd`, `epoll_create1` + three
`epoll_ctl` ADDs — every fd owned by Chapter 7's RAII wrapper (`Fd` in C++,
`OwnedFd`-backed nix types in Rust, `os.File` in Go). Then the loop; here it
is in all three languages, verbatim from
`examples/09-event-loops-epoll-inotify/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    for (;;) {
        // One timer covers both jobs: it is always armed to whichever comes
        // first — the overall timeout or the earliest per-path debounce.
        auto next = deadline;
        for (const auto& [_, p] : pending) {
            next = std::min(next, p.due);
        }
        if (auto armed = arm_timer(*timer, next - steady_clock::now()); !armed) {
            std::println(stderr, "fwatch: watch: timerfd_settime: {}", armed.error().message());
            return 1;
        }

        std::array<epoll_event, 8> events{};
        const int n = ::epoll_wait(epoll->get(), events.data(), events.size(), -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::println(stderr, "fwatch: watch: epoll_wait: {}", last_error().message());
            return 1;
        }

        for (int i = 0; i < n; ++i) {
            switch (events[i].data.u32) {
            case tok_inotify: {
                alignas(inotify_event) char buf[4096];
                for (;;) {
                    const ssize_t got = ::read(inotify->get(), buf, sizeof buf);
                    if (got <= 0) {
                        break; // EAGAIN: queue drained
                    }
                    const auto now = steady_clock::now();
                    for (const char* p = buf; p < buf + got;) {
                        const auto* ev = reinterpret_cast<const inotify_event*>(p);
                        p += sizeof(inotify_event) + ev->len;
                        if (ev->len == 0) {
                            continue; // event on the directory itself
                        }
                        const auto kind = classify(ev->mask);
                        if (!kind) {
                            continue;
                        }
                        const std::string name{ev->name};
                        const auto it = pending.find(name);
                        const auto old_kind = it == pending.end()
                                                  ? std::nullopt
                                                  : std::optional{it->second.kind};
                        pending.insert_or_assign(
                            name, Pending{merge(old_kind, *kind), now + kDebounce});
                    }
                }
                break;
            }
            case tok_timer: {
                std::uint64_t expirations = 0;
                std::ignore = ::read(timer->get(), &expirations, sizeof expirations);
                const auto now = steady_clock::now();
                if (now >= deadline) {
                    flush(pending, now, /*all=*/true);
                    std::println("fwatch: exiting (timeout)");
                    return 0;
                }
                flush(pending, now, /*all=*/false);
                break;
            }
            case tok_signal: {
                signalfd_siginfo info{};
                std::ignore = ::read(sigfd->get(), &info, sizeof info);
                flush(pending, steady_clock::now(), /*all=*/true);
                std::println("fwatch: exiting (signal)");
                return 0;
            }
            default:
                break;
            }
        }
    }
```

```go
	for {
		select {
		case ev, ok := <-events:
			if !ok {
				return errors.New("inotify reader stopped unexpectedly")
			}
			oldKind := ""
			if p, exists := pending[ev.name]; exists {
				oldKind = p.kind
			}
			pending[ev.name] = &pendingEvent{
				kind: mergeKinds(oldKind, ev.kind),
				due:  time.Now().Add(debounce),
			}
			rearm()
		case <-flushTimer.C:
			flush(false)
			rearm()
		case <-timeout:
			flush(true)
			fmt.Println("fwatch: exiting (timeout)")
			return nil
		case <-sigs:
			flush(true)
			fmt.Println("fwatch: exiting (signal)")
			return nil
		}
	}
```

```rust
    loop {
        // One timer covers both jobs: it is always armed to whichever comes
        // first — the overall timeout or the earliest per-path debounce.
        let next = pending.values().map(|p| p.due).min().map_or(deadline, |d| d.min(deadline));
        arm_timer(&timer, next.saturating_duration_since(Instant::now()))?;

        let mut events = [EpollEvent::empty(); 8];
        let n = match epoll.wait(&mut events, EpollTimeout::NONE) {
            Ok(n) => n,
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e).context("epoll_wait"),
        };

        for event in &events[..n] {
            match event.data() {
                TOK_INOTIFY => loop {
                    let batch = match inotify.read_events() {
                        Ok(batch) => batch,
                        Err(Errno::EAGAIN) => break, // queue drained
                        Err(e) => return Err(e).context("inotify read"),
                    };
                    let now = Instant::now();
                    for ev in batch {
                        let Some(name) = ev.name else {
                            continue; // event on the directory itself
                        };
                        let Some(kind) = classify(ev.mask) else {
                            continue;
                        };
                        let name = name.to_string_lossy().into_owned();
                        let old = pending.get(&name).map(|p| p.kind);
                        pending
                            .insert(name, Pending { kind: merge(old, kind), due: now + DEBOUNCE });
                    }
                },
                TOK_TIMER => {
                    let mut buf = [0u8; 8];
                    let _ = nix::unistd::read(&timer, &mut buf);
                    let now = Instant::now();
                    if now >= deadline {
                        flush(&mut pending, now, true);
                        println!("fwatch: exiting (timeout)");
                        return Ok(());
                    }
                    flush(&mut pending, now, false);
                }
                TOK_SIGNAL => {
                    let _ = sigfd.read_signal();
                    flush(&mut pending, Instant::now(), true);
                    println!("fwatch: exiting (signal)");
                    return Ok(());
                }
                _ => {}
            }
        }
    }
```

C++ and Rust are the same program in two spellings — arm, wait forever
(`-1` / `EpollTimeout::NONE`; the timerfd *is* the timeout), dispatch by
token, drain or consume, `EINTR` retries the wait. The one visible library
difference: nix's `inotify.read_events()` does the record-walking that C++
does by hand over `buf`, returning parsed events with `Option<OsString>`
names — same bytes, one abstraction up.

Go's excerpt has no epoll in it, and that is the entire point. Its `watch`
is built from a goroutine and four channel operations: `readEvents` (in
`examples/09-event-loops-epoll-inotify/go/main.go`) wraps the `O_NONBLOCK`
inotify fd in an `os.File` — the act that registers it with the runtime's
**netpoller** — and calls `f.Read` in a loop. That read *looks* blocking,
but because the fd is nonblocking and registered, the runtime parks the
*goroutine* (not an OS thread) until its epoll loop reports readiness, then
resumes it to walk the same packed records (`unix.SizeofInotifyEvent`,
`raw.Len`, TrimRight the NUL padding) and send on a channel. The other
sources never touch an fd in user code: `time.After` is the timeout,
a `time.Timer` re-armed to the earliest `due` is the debounce (`rearm`
mirrors the C++ `arm_timer` computation exactly), and `signal.Notify`
delivers SIGINT/SIGTERM on a channel. `select` is `epoll_wait` one level up
— it blocks until any source is ready and dispatches — except the "fds" are
channels and the kernel machinery is rented from the runtime.

### Errors, three ways

Setup is where `watch` can fail, and the missing-directory case exercises
each language's Chapter 5 policy end to end. C++ threads four
`std::expected` constructions together, chaining the three `epoll_ctl` adds
with `.and_then(…)` so the first failure short-circuits with its
`std::error_code`; the observed result is
`fwatch: watch: /no/such/dir: No such file or directory`, exit 1. Go gets
`unix.ENOENT` back from `inotify_add_watch`, matches it with
`errors.Is(err, unix.ENOENT)` to emit the friendlier
`no such directory`, and — because the raw fd is not yet wrapped in an
`os.File` at that point — must `unix.Close(ifd)` manually on that one path:
the seam between raw-fd code and RAII-style ownership is exactly one line
wide, and it is the line you forget. Rust's `anyhow::Context` builds the
same chain declaratively (`.with_context(|| dir.to_string())`), printed with
`{err:#}` to get `context: cause` in one line. Runtime errors inside the
loop split by kind: `EINTR` is not an error (retry the wait — explicitly in
C++/Rust, invisibly in Go), `EAGAIN` on the inotify fd is the *success* case
"queue drained", and anything else aborts with context. Usage errors exit 2,
runtime failures exit 1, clean shutdown — timeout or signal — exits 0; all
three verified below.

### Concurrency lens

Count the threads doing *your* work: one in C++, one in Rust — and one
goroutine plus `main` in Go, multiplexed by the runtime onto its thread
pool. The single-threaded epoll design is not an accident, it is the
concurrency *strategy*: since only one thread touches `pending`, there are
no locks, no atomics, and no `EPOLLONESHOT` re-arm protocol; the event loop
serializes everything by construction. Go reaches the same freedom-from-locks
differently — `pending` is confined to the `select` goroutine, and other
concerns communicate by channel — which is Chapter 6's "share memory by
communicating" applied to I/O. The costs surface in `strace`: idle for 400 ms,
the C++ watcher makes exactly **1** `epoll_wait` call (armed once, sleeps,
fires at the deadline), while the Go binary makes **10** `epoll_pwait` calls
across its threads — the netpoller is consulted by the scheduler, not just
by your `select`, and no `timerfd_create` or `signalfd4` ever appears
because Go implements timers in the runtime and signals via handlers feeding
channels. Neither number is wrong; one design spends syscalls to buy an
abstraction, the other spends explicitness to buy syscall silence.

## Build, run, observe

```bash
[host]$ cd examples/09-event-loops-epoll-inotify && ./demo.sh
```

Each language builds and runs a 1-second self-watch. For a run you control,
background a watcher and touch files under it:

```bash
[host]$ WD=$(mktemp -d)
[host]$ ./cpp/build/release/app watch "$WD" --timeout-ms 1500 &
[host]$ echo hi > "$WD/a.txt"; echo more >> "$WD/a.txt"; sleep 0.4; rm "$WD/a.txt"; wait
```

Observed output, all three languages identical:

```
event: created a.txt
event: deleted a.txt
fwatch: exiting (timeout)
```

Two lines, not four: the create and the immediate append landed in one 100 ms
debounce window and `merge` kept `created`; the `rm` 400 ms later was its own
window. Send SIGINT instead of waiting and the last line becomes
`fwatch: exiting (signal)` (observed, exit 0). The book's harness agrees on
the full behavior matrix — `python3 scripts/test-all-examples.py --only
09-event-loops-epoll-inotify` reports `PASS` for cpp, go, and rust
(3 passed, 0 failed, 0 skipped).

## Cross-check: the loop under strace, and epoll's own bookkeeping

The chapter's claims are checkable from outside. First, the C++ watcher
under this book's strace build (`scratchpad/usr/bin/strace`, v7.1), during
exactly the create/append/delete session above; trimmed, long paths elided:

```bash
[host]$ strace -e trace=epoll_wait,inotify_add_watch,read ./cpp/build/release/app watch "$WD" --timeout-ms 1500
inotify_add_watch(3, "/tmp/…/fw-demo", IN_MODIFY|IN_ATTRIB|IN_MOVED_FROM|IN_MOVED_TO|IN_CREATE|IN_DELETE) = 1
epoll_wait(6, [{events=EPOLLIN, data=0}], 8, -1) = 1
read(3, "\1\0\0\0\0\1\0\0\0\0\0\0\20\0\0\0a.txt\0\0\0\0\0\0\0\0\0\0\0"..., 4096) = 64
read(3, 0x7ffcfe3fba00, 4096)           = -1 EAGAIN (Resource temporarily unavailable)
epoll_wait(6, [{events=EPOLLIN, data=0x1}], 8, -1) = 1
read(4, "\1\0\0\0\0\0\0\0", 8)          = 8
epoll_wait(6, [{events=EPOLLIN, data=0}], 8, -1) = 1
read(3, "\1\0\0\0\0\2\0\0\0\0\0\0\20\0\0\0a.txt\0\0\0\0\0\0\0\0\0\0\0", 4096) = 32
read(3, 0x7ffcfe3fba00, 4096)           = -1 EAGAIN (Resource temporarily unavailable)
epoll_wait(6, [{events=EPOLLIN, data=0x1}], 8, -1) = 1
read(4, "\1\0\0\0\0\0\0\0", 8)          = 8
epoll_wait(6, [{events=EPOLLIN, data=0x1}], 8, -1) = 1
read(4, "\1\0\0\0\0\0\0\0", 8)          = 8
+++ exited with 0 +++
```

Every mechanism from this chapter is on one screen. The `data=0` wakeups are
the inotify token, `data=0x1` the timer token. The first inotify drain
returns **64 bytes — two packed records in one read** (the create, mask
`\0\1\0\0` = `IN_CREATE`, and the append, coalesced by our merge into one
output line), followed by the mandatory `EAGAIN`. The delete arrives later as
one 32-byte record with mask `\0\2\0\0` = `IN_DELETE`. Three timer wakeups,
three 8-byte expiration reads: two debounce flushes and the final deadline.
Total `epoll_wait` calls for the whole session: five.

Second — and this is the part few people ever look at — epoll's interest
list is inspectable. With a watcher running (PID via `$!`):

```bash
[host]$ ls -l /proc/$PID/fd | tail -4
lr-x------ 1 rsedor rsedor 64 Jul 18 23:06 3 -> anon_inode:inotify
lrwx------ 1 rsedor rsedor 64 Jul 18 23:06 4 -> anon_inode:[timerfd]
lrwx------ 1 rsedor rsedor 64 Jul 18 23:06 5 -> anon_inode:[signalfd]
lrwx------ 1 rsedor rsedor 64 Jul 18 23:06 6 -> anon_inode:[eventpoll]
[host]$ cat /proc/$PID/fdinfo/6
tfd:        3 events:       19 data:                0  pos:0 ino:426 sdev:11
tfd:        4 events:       19 data:                1  pos:0 ino:426 sdev:11
tfd:        5 events:       19 data:                2  pos:0 ino:426 sdev:11
[host]$ cat /proc/$PID/fdinfo/3
inotify wd:1 ino:142a5 sdev:35 mask:3c6 ignored_mask:0 fhandle-bytes:c fhandle-type:1 f_handle:6945b24ea542010000000000
[host]$ cat /proc/$PID/fdinfo/4
clockid: 1
ticks: 0
it_value: (2, 694650159)
```

The eventpoll fdinfo *is* Figure 9.1: three `tfd` (target fd) rows, our
tokens 0/1/2 in the `data` column, `events: 19` = `EPOLLIN|EPOLLERR|EPOLLHUP`
(the error bits are always reported; you never register them). The inotify
fdinfo shows `wd:1`, our exact mask `3c6`, and the watched inode `142a5` —
hex for 82597, which `stat -c '%i'` on the watched directory confirms. The
timerfd fdinfo shows `clockid: 1` (`CLOCK_MONOTONIC`) and, caught mid-run,
`it_value: (2, 694650159)` — about 2.69 s remaining on a 3000 ms run, i.e.
armed to the overall deadline because no debounce was pending.

Third, the Go binary on the same session, tracing what *our code never
wrote*:

```bash
[host]$ strace -f -e trace=epoll_create1,epoll_ctl,epoll_pwait ./go/bin/app watch "$WD" --timeout-ms 800
epoll_create1(EPOLL_CLOEXEC)    = 5
epoll_ctl(5, EPOLL_CTL_ADD, 6, {events=EPOLLIN, data=0x5e0998}) = 0
epoll_ctl(5, EPOLL_CTL_ADD, 4, {events=EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET, data=0x3fb6883027000001}) = 0
epoll_pwait(5, [], 128, 0, NULL, 0) = 0
epoll_pwait(5, [], 128, 799, NULL, 0) = 0
```

The runtime built its own epoll instance, registered its wakeup fd, then
added our inotify fd (4) **edge-triggered** with an opaque runtime cookie
where we used token integers — 9 `epoll_pwait` calls in this run, issued
from several threads, none from `main.go`. The idle comparison quantifies
the two architectures: over a 400 ms idle watch, `strace -c` shows the C++
binary making exactly 1 `epoll_wait`, 1 `epoll_create1`, 1 `timerfd_create`,
1 `timerfd_settime`, 1 `signalfd4`, and 1 `inotify_add_watch`; the Rust
binary is syscall-for-syscall identical (the same six calls, once each); the
Go binary makes 10 `epoll_pwait` and zero timerfd/signalfd calls. Two tools
— the trace and `/proc` — one consistent story: readiness is a kernel fact,
and both surfaces are views of it.

<p><span class="status status--unverified">unverified</span> — <strong>On the
lab VM:</strong> the kernel-side view of this loop — watching wakeup sources
fire with bpftrace on <code>tracepoint:syscalls:sys_enter_epoll_wait</code>
and kprobes on the eventpoll internals, and forcing a real
<code>IN_Q_OVERFLOW</code> by shrinking <code>max_queued_events</code> —
needs root and a disposable kernel, so it is not run here; chapter 30
(Debugging part) exercises exactly this on <code>systems-target</code>.</p>

## What you learned

- Readiness decouples waiting from reading: nonblocking fds, one
  `epoll_wait`, and a kernel-side interest list you can literally read back
  from `/proc/<pid>/fdinfo/<epfd>` — tokens, masks, and all.
- Level triggering plus "consume what woke you" is the book's default;
  edge demands drain-to-`EAGAIN` always (Go's netpoller does exactly that,
  registering with `EPOLLET`), and `EPOLLONESHOT` exists for the
  multi-threaded case this design avoids.
- inotify queues events per instance with per-watch `wd`s and a finite
  queue (16384 here) that overflows into `IN_Q_OVERFLOW` — the signal to
  fall back to `snapshot`/`diff`; watches do not recurse.
- timerfd and signalfd turn timeouts and SIGINT into readable fds — one
  re-armed one-shot timer served both the debounce and the deadline, and a
  blocked-then-read signal became an ordinary dispatch case.
- Go's goroutine + channels + `select` is the same machinery inverted: 6
  explicit setup syscalls and 1 wait for C++/Rust idle, versus 10 runtime
  `epoll_pwait`s and no timerfd/signalfd for Go.

Next, **io_uring**: the completion model — instead of the kernel telling you
an fd is *ready* so you can do the work, you submit the work and the kernel
tells you it is *done*.

---

<p><span class="status status--verified">verified</span> — all evidence in
this chapter was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, strace 7.1) this session: the runner reported
<code>PASS</code> for cpp, go, and rust (3 passed, 0 failed, 0 skipped); the
create/append/delete session printed exactly <code>event: created
a.txt</code>, <code>event: deleted a.txt</code>, <code>fwatch: exiting
(timeout)</code>, and a SIGINT run printed <code>fwatch: exiting
(signal)</code> with exit 0 (missing dir exited 1, bad usage 2); the C++
strace excerpt is real trimmed output (5 <code>epoll_wait</code>s, the
64-byte two-record inotify read, the <code>EAGAIN</code>s, three 8-byte
timerfd reads); the <code>/proc</code> quotes are real (<code>tfd</code>
rows 3/4/5 with data 0/1/2, <code>mask:3c6</code>, <code>ino:142a5</code>
matching <code>stat -c '%i'</code> = 82597, <code>clockid: 1</code>,
<code>it_value: (2, 694650159)</code>); the Go trace showed
<code>epoll_ctl … EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET</code> on the inotify
fd and 9 <code>epoll_pwait</code>s (10 in the idle <code>-c</code> run, with
0 timerfd/signalfd syscalls), while C++ and Rust each made exactly one of
each setup call and one wait when idle; and
<code>/proc/sys/fs/inotify</code> read 16384 / 524288 / 512. Not run here:
the bpftrace/overflow experiment (lab-VM callout above).</p>
