---
title: "Pipes, FIFOs, and splice"
order: 18
part: "IPC"
description: "pmon v4 turns the supervisor into a log pipeline: the pipe as a ring of kernel pages with its capacity read live via F_GETPIPE_SZ, what PIPE_BUF atomicity does and does not promise, FIFO open semantics and the ENXIO trick, SIGPIPE/EPIPE as the writer's contract, and splice moving log bytes into a FIFO without a userspace copy — proven with strace, /proc, and a 5.3 GB/s dd run."
duration: "45 minutes"
---

This part of the book is about processes talking to each other, and it opens
with the primitive every shell has used since 1973. `pmon` ended the process
part supervising a crashing child with two byte-identical engines — pidfd and
sigchld — but it threw the child's output away, and a supervisor that
discards logs is half a supervisor. Version 4 captures the child's stdout and
stderr into a framed log file and then republishes that log through a named
pipe to any reader that wants it, surviving readers that come and go. The one
new idea is that a pipe is not a file with two ends: it is a bounded ring of
kernel pages, and every behavior this chapter meets — the 65536-byte
capacity, `PIPE_BUF` atomicity, EOF as "last write end closed", `SIGPIPE` on
a readerless write, even `splice(2)`'s zero-copy trick — falls straight out
of that ring.

The code is in `examples/18-pipes-fifos-splice/`. `./demo.sh` there builds
all three implementations and runs a short capture demo; its `README.md`
specifies the CLI, the log record format, and the exit codes all three
languages share.

{% include excalidraw.html
   file="18-pipe-internals"
   alt="Three panels: a writer process box holding fd 1 the write end, a kernel band labelled struct pipe_inode_info inode 16462630 containing a row of eight page slots from the sixteen-slot ring with head and tail pointers marked, and a reader process box holding fd 5 the read end with fdinfo flags 02000000; notes record the 65536-byte capacity read via F_GETPIPE_SZ, the F_SETPIPE_SZ rounding of 1 up to 4096, the 1048576 pipe-max-size ceiling, and the warning that PIPE_BUF atomicity is not message framing."
   caption="Figure 18.1 — one pipe from this chapter's live supervise run: the child's fd 1 and the supervisor's fd 5 share the ring behind pipe inode 16462630; every number is from /proc or a live fcntl" %}

> **Tools used** — `strace`, `mkfifo`, `dd`, `stat`, `getconf`, `ls`/`cat`
> over `/proc`, `python3` (host); `bpftrace`/bcc-tools (lab VM, exercised in
> Part 8). Everything here is checked by `scripts/check-host.sh`, ships with
> Fedora, or is preinstalled in the lab VMs.

## A pipe is a ring of pages

`pipe2(2)` allocates a `pipe_inode_info` in the kernel — an array of sixteen
`pipe_buffer` slots, each holding one page — and returns two fds onto it:
slot 0 reads from the ring's tail, slot 1 writes at its head. Nothing about
this is a file. There is no offset (`lseek` fails with `ESPIPE`), no name,
and no storage: a full ring blocks the writer, an empty ring blocks the
reader, and the kernel wakes each side as the other makes progress. That
back-pressure is the feature — a slow log consumer stalls the producer
instead of eating unbounded memory.

The capacity is not folklore; you can ask for it. `fcntl(F_GETPIPE_SZ)`
reads it live, and `F_SETPIPE_SZ` resizes in whole pages:

```bash
[host]$ python3 -c '
import fcntl, os
r, w = os.pipe2(os.O_CLOEXEC)
print("capacity:", fcntl.fcntl(w, 1032))          # F_GETPIPE_SZ
print("PIPE_BUF:", os.pathconf(w, "PC_PIPE_BUF"))
fcntl.fcntl(w, 1031, 1)                           # F_SETPIPE_SZ
print("asked for 1, got:", fcntl.fcntl(w, 1032))'
capacity: 65536
PIPE_BUF: 4096
asked for 1, got: 4096
[host]$ cat /proc/sys/fs/pipe-max-size
1048576
```

Sixteen 4096-byte pages, 65536 bytes; ask for a 1-byte pipe and the kernel
rounds up to one page; unprivileged growth stops at
`/proc/sys/fs/pipe-max-size` (1048576 here). `pmon` uses `pipe2` rather than
`pipe` for the same reason chapter 7 put `O_CLOEXEC` in every open: the flag
lands atomically, with no window for a concurrently spawned child to inherit
the fd. `strace -f -e trace=pipe2` on the C++ supervisor shows exactly two
calls per attempt — `pipe2([5, 6], O_CLOEXEC) = 0` and
`pipe2([7, 8], O_CLOEXEC) = 0` — one pipe for the child's stdout, one for
its stderr.

## `PIPE_BUF`, and what it does not promise

POSIX guarantees that a single `write(2)` of at most `PIPE_BUF` bytes — 4096
on Linux, read out above with `pathconf` — is atomic: its bytes land in the
ring contiguously, never interleaved with another writer's bytes. That
guarantee is narrower than it sounds, and the gap is why v4's capture
architecture exists. Atomicity is *not framing*: the reader can still `read`
half a message, because reads have no atomicity at all. It is *not a promise
about large writes*: one `write` of 8000 bytes may be split anywhere, with
another writer's data landing in the middle. And it is *not a promise about
your language's buffering*: a child that "prints a line" may reach the
kernel as several small writes or one giant flush, at the whim of its stdio
library — a supervised process is exactly the kind of writer you cannot
retrain.

So `pmon` never relies on write atomicity to keep `[out]` and `[err]`
records apart. The tempting shortcut — `dup2` both child streams onto *one*
pipe and let `PIPE_BUF` sort it out — loses the out/err distinction forever
and still interleaves mid-line whenever a write exceeds 4096 bytes or a line
crosses two writes. Instead each stream gets its own pipe, and a userspace
relay loop reassembles byte chunks into complete lines before stamping the
prefix. Interleaving still happens — but only at line granularity, chosen by
the relay, never mid-record. That relay loop is the real price
of framed capture: the kernel moves bytes; meaning is your job.

## FIFOs: a pipe with a name

`supervise` and its child share ancestry, so inheriting pipe ends works. A
log *consumer* started an hour later from another terminal shares nothing —
it needs a rendezvous point with a name. `mkfifo(3)` creates one: an inode
of type `S_IFIFO` whose only content is a pipe ring materialized in the
kernel when both ends open it. `stat` reports our created FIFO as `fifo 644
live2.fifo` — a filesystem name, zero bytes of storage.

Opening a FIFO has its own choreography, because a pipe with one end is
useless. Who blocks when:

| `open(2)` mode | counterpart absent | counterpart present |
|---|---|---|
| `O_RDONLY` | **blocks** until a writer opens | returns at once |
| `O_WRONLY` | **blocks** until a reader opens | returns at once |
| `O_RDONLY\|O_NONBLOCK` | returns at once (reads give `EAGAIN`/EOF later) | returns at once |
| `O_WRONLY\|O_NONBLOCK` | **fails with `ENXIO`** | returns at once |
| `O_RDWR` | returns at once (Linux behavior; POSIX leaves it undefined) | returns at once |

The asymmetry in row four is deliberate kernel policy — a nonblocking
writer with no reader would otherwise instantly fill the ring and stall —
and `pmon tail` turns it into a feature: it opens the FIFO
`O_WRONLY|O_NONBLOCK|O_CLOEXEC`, treats `ENXIO` as "no reader yet, sleep
50 ms, retry", and on success clears `O_NONBLOCK` with `fcntl(F_SETFL)` so
the relay gets blocking back-pressure. The cleared bit is visible in
`/proc/<pid>/fdinfo/4`: `flags: 02100001` decodes as `O_WRONLY` +
`O_LARGEFILE` + `O_CLOEXEC` — no `O_NONBLOCK` remaining.

## `SIGPIPE`/`EPIPE`: the writer's contract

The reader side of a pipe has a gentle end-of-conversation signal: when the
last write end closes, `read` returns 0, EOF. The writer side gets a
harsher one. Writing to a pipe whose last read end has closed raises
`SIGPIPE`, whose default action *terminates the process* — the design that
makes `head -1 < bigfile-producer` shut the producer down for free in a
shell pipeline. A long-lived relay must not die because one `cat` was
killed, so `cmd_tail` ignores `SIGPIPE` in all three languages, converting
the event into an `EPIPE` error return from `write`/`splice` at the exact
call that noticed. That is the writer's contract: handle `EPIPE` at every
write site, or inherit a default that kills you. `pmon` treats it as a
state transition — print `pmon: tail reader detached`, close the writer fd,
and go back to waiting for the next reader.

{% include excalidraw.html
   file="18-log-topology"
   alt="Three horizontal bands. Top, pmon supervise pid 2633754: the sh child pid 2633756 writes fd 1 and fd 2 into two kernel pipe nodes with inodes 16462630 and 16462631, which feed a poll loop node listing signalfd 4, pidfd 6, and pipe read ends 5 and 7, appending framed out and err lines into a live.log node opened append-only as fd 3. Middle, pmon tail pid 2634759: a log fd 3 node with fdinfo pos 10 connects by an amber splice arrow labelled with the real splice call returning 22 to a FIFO writer fd 4 node whose flags 02100001 show O_NONBLOCK cleared. Bottom, kernel FIFO plus reader: the live.fifo inode of type S_IFIFO flows into a cat reader holding fd 3, with a note that a detached reader causes EPIPE and the relay re-splices undelivered bytes to the next reader."
   caption="Figure 18.2 — pmon v4's log pipeline end to end; every pid, fd number, pipe inode, flag value, and splice return comes from the live runs traced in this chapter" %}

## How the code works

Two structures carry the capture. `Capture` owns one child's two pipe read
ends plus per-stream open/closed state; `LineRelay` owns the framing — a
byte buffer per stream that accumulates chunks, emits every completed line
to the log as `[out] line\n` or `[err] line\n` immediately (line-buffered,
so the log is useful while the child lives), bounds the partial-line buffer
at 64 KiB, and on EOF flushes any trailing partial line with a supplied
newline. Two relays, not one, because the *pipes* are two: bytes from
stdout can never contaminate an `[err]` record even if the child sprays
both streams at once.

The wiring is where the invariants live. The C++ supervisor, per attempt:

```cpp
        Capture cap{*log};
        std::array<int, 2> outp{}, errp{};
        if (::pipe2(outp.data(), O_CLOEXEC) != 0 || ::pipe2(errp.data(), O_CLOEXEC) != 0) {
            std::println(stderr, "pmon: pipe2: {}", last_error().message());
            return 1;
        }
        cap.out_r = Fd{outp[0]};
        Fd out_w{outp[1]};
        cap.err_r = Fd{errp[0]};
        Fd err_w{errp[1]};

        const auto pid = spawn_child(cmd, out_w, err_w);
```

`spawn_child` wires the write ends to the child with
`posix_spawn_file_actions_adddup2(fa, out.get(), STDOUT_FILENO)` — and
`dup2` semantics do the O_CLOEXEC bookkeeping for us: the *duplicate*
created in the child has the close-on-exec bit clear, so the child's fds 1
and 2 survive `exec` while every original supervisor-side fd vanishes from
the child. Two lines later comes the sentence this chapter exists to teach:

```cpp
        out_w.reset();  // parent's copies of the write ends must close,
        err_w.reset();  // or EOF never arrives
```

EOF on a pipe means "*every* write-end fd is closed". The supervisor holds
copies of the write ends between `pipe2` and `posix_spawnp`; if it kept
them, the child could die and the read ends would stay open forever — the
supervisor would be holding its own pipe open, waiting on itself. Closing
the parent's copies immediately after spawn makes child-exit imply
pipe-EOF, which is what lets `drain_to_eof` terminate.

From there the pipes simply join the multiplexing each engine already had.
C++ polls four fds — signalfd, both pipe read ends, and (pidfd engine) the
pidfd — marking a drained pipe's slot as `fd = -1`, which `poll(2)`
ignores. Rust builds the same four-slot `rustix::event::poll` set beside
its nix `SignalFd`, parking closed pipes with an empty event mask. Go
inverts idiomatically: `capture` wraps each pipe in a `bufio.Scanner`
goroutine feeding one channel of prefixed `logLine`s into the same `select`
that already multiplexes the pidfd poller, `signal.Notify`, and the
deadline; a `sync.WaitGroup` closes the channel when both scanners hit EOF,
so channel-closed is the Go spelling of drained-to-EOF. In every language
the reap path calls the drain *before* printing the exit observation — the
log is complete before `pmon: child=… exited status=…` appears, an
ordering `verify.lua` asserts.

`tail` is the second half: create the FIFO (`EEXIST` is tolerated only if
the existing inode really is a FIFO — anything else is error, exit 1), open
the log read-only at offset 0 so a late reader gets the backlog, then relay
forever. The C++ and Rust relays move bytes with `splice(2)`; Go uses
`io.Copy` per reader session. The heart of each:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// splice(2) fast path: move log bytes kernel-side into the pipe. The file
// offset only advances on success, so a detached reader (EPIPE) loses
// nothing — the same bytes are respliced for the next reader.
[[nodiscard]] std::expected<Relay, std::error_code>
relay_splice(const Fd& log, const Fd& w) {
    const ssize_t n = ::splice(log.get(), nullptr, w.get(), nullptr, kChunk, 0);
    if (n > 0) {
        return Relay::moved;
    }
    if (n == 0) {
        return Relay::idle;
    }
    switch (errno) {
        case EPIPE:
            return Relay::detached;
        case EINTR:
        case EAGAIN:
            return Relay::idle;
        case EINVAL:
        case ENOSYS:
            return Relay::no_splice;  // fs without splice support: fall back
        default:
            return std::unexpected(last_error());
    }
}
```

```go
			// One copy session: on EPIPE (reader detached) io.Copy reports
			// how many bytes actually reached the pipe, so the log offset is
			// rewound to the first unwritten byte — the next reader loses
			// nothing.
			start, serr := logf.Seek(0, io.SeekCurrent)
			if serr != nil {
				w.Close()
				fmt.Fprintf(os.Stderr, "pmon: seek %s: %v\n", logPath, serr)
				return 1
			}
			n, cerr := io.Copy(w, logf)
			if cerr == nil { // caught up: poll for appended lines
				time.Sleep(pollTick)
				continue
			}
			if errors.Is(cerr, syscall.EPIPE) {
				fmt.Println("pmon: tail reader detached")
				if _, serr := logf.Seek(start+n, io.SeekStart); serr != nil {
					w.Close()
					fmt.Fprintf(os.Stderr, "pmon: seek %s: %v\n", logPath, serr)
					return 1
				}
				w.Close()
				continue reader
			}
			w.Close()
			fmt.Fprintf(os.Stderr, "pmon: relay: %v\n", cerr)
			return 1
```

```rust
/// splice(2) fast path: move log bytes kernel-side into the pipe. The file
/// offset only advances on success, so a detached reader (EPIPE) loses
/// nothing — the same bytes are respliced for the next reader.
fn relay_splice(log: &File, w: &OwnedFd) -> Result<Relay> {
    match splice(log, None, w, None, CHUNK, SpliceFlags::empty()) {
        Ok(0) => Ok(Relay::Idle),
        Ok(_) => Ok(Relay::Moved),
        Err(rustix::io::Errno::PIPE) => Ok(Relay::Detached),
        Err(rustix::io::Errno::INTR) | Err(rustix::io::Errno::AGAIN) => Ok(Relay::Idle),
        Err(rustix::io::Errno::INVAL) | Err(rustix::io::Errno::NOSYS) => Ok(Relay::NoSplice),
        Err(e) => Err(e).context("splice"),
    }
}
```

`splice` exists because a pipe *is* a ring of page references: when one fd
of the pair is a pipe, the kernel can move data by wiring the file's
page-cache pages into `pipe_buffer` slots — no copy into userspace, no
copy back. Passing `NULL`/`None` for the file's offset pointer means "use
the fd's own offset and advance it", which is precisely the no-loss
mechanism: the offset moves only for bytes the pipe accepted, so `EPIPE`
mid-relay leaves it parked at the first undelivered byte, and the next
reader's first `splice` resends exactly from there. Go has no `splice` in
its portable API, so it makes the same promise arithmetically: remember
`start`, let `io.Copy` report `n` bytes delivered before the `EPIPE`, seek
back to `start+n`. (`io.Copy` between an `*os.File` pair actually tries
`copy_file_range`/`sendfile` internally — but to a FIFO it settles on
plain read/write, as the strace below confirms only for C++ and Rust.) The
C++/Rust fallback `relay_rw` keeps the identical rewind contract for
filesystems where `splice` returns `EINVAL`; a `tee(2)` aside completes
the family — it duplicates pipe contents to a second pipe without
consuming them, which is how you would split this log stream to two
FIFOs.

The fragile bits, stated plainly: the 50 ms `sleep`-poll for both "log has
no new bytes" and "no reader yet" is deliberate simplicity — chapter 9's
inotify machinery could make both event-driven, and a real system would use
it; the FIFO relay serves one reader at a time (a second simultaneous
opener would interleave records with the first — FIFOs have one ring, not
one per reader); and log rotation is unhandled: `tail` follows an fd, not
a name, so a renamed-away log keeps feeding the old inode. Chapter 19
replaces "one anonymous byte stream" with a real control plane.

## Errors, three ways

The chapter's errno vocabulary is small and every entry is *expected*, so
the interesting part is which errors are policy at the call site versus
fatal. `ENXIO` from a nonblocking FIFO open means "no reader yet": C++
checks `fd.error().value() != ENXIO`, Go `errors.Is(err, unix.ENXIO)`,
Rust matches `Err(rustix::io::Errno::NXIO)` — all three sleep and retry.
`EPIPE` means "reader left": mapped to the `detached` state, never
propagated. `EEXIST` from `mkfifo` is acceptance-with-verification: stat
the path, require `S_ISFIFO` (C++), `fs.ModeNamedPipe` (Go),
`file_type().is_fifo()` (Rust), and only a non-FIFO squatter is fatal.
`EINTR` everywhere returns to the loop — `tail`'s stop handlers install
with no `SA_RESTART` precisely so a signal can interrupt a blocking call
and let the loop observe the stop flag. Everything unexpected carries its
context up to one printer and exit 1, same as every pmon before it: three
error mechanisms (`std::expected`, wrapped `error` values, `anyhow`
results), one policy.

## Concurrency lens

The capture is a small concurrency study. C++ and Rust stay
single-threaded by *adding fds to an existing poll set* — the same event
loop that watches for child exit watches for child output, so there is no
locking anywhere; the log write happens on the only thread. Go runs three
goroutines per attempt (two scanners, one WaitGroup-closer), and the
channel is what serializes: only the `select` loop's goroutine touches the
log file, so `[out]`/`[err]` records cannot tear even though the scanners
run concurrently. The deadlock the design must dodge is the classic
pipe-capture one: a child that fills both pipes while the supervisor waits
only for exit would stall forever — which is why output fds live in the
same poll set/select as the exit source, never behind it. Signal-wise,
`SIGPIPE` disposition is process-wide state: C++/Rust set `SIG_IGN`
explicitly; Go's `signal.Ignore(syscall.SIGPIPE)` does the same job with a
runtime subtlety — Go would otherwise re-raise a fatal SIGPIPE only for
writes to fds 1 and 2, and the FIFO writer is fd 3, but the explicit
ignore documents intent and covers the stdout prints too. And the
supervisor still resets the child's signal mask at spawn (chapter 12's
lesson): a captured child is still a child.

## Build, run, observe

```bash
[host]$ cd examples/18-pipes-fifos-splice && ./demo.sh
```

Each language builds and runs a capture self-demo. Driving it by hand the
way this chapter's evidence was produced — first the capture:

```bash
[host]$ APP=examples/18-pipes-fifos-splice/cpp/build/release/app
[host]$ $APP supervise --max-restarts 2 --log pmon.log -- sh -c 'echo hello; echo oops >&2; exit 1'
pmon: engine=pidfd child=2632228 pidfd=6
pmon: child=2632228 exited status=1
pmon: restart 1/2
pmon: child=2632229 exited status=1
pmon: restart 2/2
pmon: child=2632230 exited status=1
pmon: giving up after 2 restarts
[host]$ cat pmon.log
[out] hello
[err] oops
[out] hello
[err] oops
[out] hello
[err] oops
```

(Spawn lines for attempts two and three trimmed.) Three attempts, six
framed records, stdout and stderr never crossed. Then the FIFO leg,
exercising detach and reattach:

```bash
[host]$ : > live2.log && $APP tail --log live2.log --fifo live2.fifo &
pmon: tail ready (fifo live2.fifo)
[host]$ cat live2.fifo & echo '[out] one' >> live2.log
[out] one
[host]$ kill %2                          # kill the cat
pmon: tail reader detached
[host]$ echo '[out] appended during gap' >> live2.log
[host]$ cat live2.fifo                   # reattach: nothing was lost
[out] appended during gap
[host]$ kill -TERM %1
pmon: exiting (signal)
```

Every line above is from the captured run: the first reader got `[out]
one`, the line appended while no reader existed arrived intact at the
second reader, and SIGTERM ended the relay with exit 0. The runner
(`python3 scripts/test-all-examples.py --only 18-pipes-fifos-splice`)
asserts this whole choreography — plus the v2 engine contract unchanged
under capture — and printed `PASS PASS PASS` for cpp, go, and rust on this
host.

## Cross-check, four ways

**The fd table of the running trio.** With `supervise` driving a ticking
child, `tail` relaying, and a `cat` attached, `/proc/<pid>/fd` maps every
descriptor in Figure 18.2 (paths shortened):

```bash
[host]$ ls -l /proc/2633754/fd   # supervisor
0 -> /dev/null    1 -> sup.out    2 -> sup.err    3 -> live.log
4 -> anon_inode:[signalfd]    5 -> pipe:[16462630]
6 -> anon_inode:[pidfd]       7 -> pipe:[16462631]
[host]$ ls -l /proc/2633756/fd   # child sh
0 -> /dev/null    1 -> pipe:[16462630]    2 -> pipe:[16462631]
[host]$ ls -l /proc/2634759/fd   # tail relay
3 -> live2.log    4 -> live2.fifo
[host]$ ls -l /proc/2634762/fd   # cat reader
1 -> c1.out       3 -> live2.fifo
```

The child's fd 1 and the supervisor's fd 5 name the *same* pipe inode
16462630 — one ring, two processes — and fd 2/fd 7 pair up on 16462631;
the relay and the reader meet at the FIFO. `fdinfo` reads out the flags:
the supervisor's pipe read end shows `flags: 02000000` (`O_CLOEXEC`, from
`pipe2`), and the relay's FIFO writer shows `02100001` with `pos: 10` on
its log fd — ten bytes, exactly `[out] one\n`, delivered.

**The capacity, from the fd itself.** The `F_GETPIPE_SZ` probe above
printed `capacity: 65536` and `PIPE_BUF: 4096` from a live pipe, agreeing
with `getconf PIPE_BUF /` (4096) and with sixteen pages of ring.

**The zero-copy path, under strace.** Filtering the C++ `tail` to
`splice` while a reader attaches, receives one tick line, and is killed:

```bash
[host]$ strace -f -e trace=splice $APP tail --log live.log --fifo live.fifo
splice(3, NULL, 4, NULL, 65536, 0)      = 22
splice(3, NULL, 4, NULL, 65536, 0)      = 0
splice(3, NULL, 4, NULL, 65536, 0)      = -1 EPIPE (Broken pipe)
```

fd 3 (the log) to fd 4 (the FIFO), 65536-byte requests: 22 bytes moved —
one `[out] tick 1784434137\n` record — then 0 for "caught up", then
`EPIPE` at the instant the `cat` died, answered on stdout by `pmon: tail
reader detached`. The Rust binary's trace is the same shape:
`splice(3, NULL, 4, NULL, 65536, 0) = 16` for its 16-byte record, then
`= 0`, then `= -1 EPIPE (Broken pipe)` — 22 splice calls total in that
session and not one `read` of log data in between. Go, by design, shows
plain `read`/`write` instead.

**Throughput, order-of-magnitude.** A FIFO is a kernel ring, so moving
bytes through one should run at memory speed, not disk speed:

```bash
[host]$ mkfifo dd.fifo && cat dd.fifo > /dev/null & dd if=/dev/zero of=dd.fifo bs=64k count=16384
1073741824 bytes (1.1 GB, 1.0 GiB) copied, 0.202097 s, 5.3 GB/s
```

One GiB through the FIFO in a fifth of a second on this host — 5.3 GB/s
through two processes and one ring, with `bs=64k` matching both the ring
capacity and pmon's `kChunk`.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet-wide view of this chapter is tracing pipe traffic and splice
> calls system-wide with `bpftrace` kprobes and bcc-tools instead of
> per-process `strace`; that tooling is not runnable on this host without
> privileges, and Part 8 (Debugging) exercises exactly that on the
> `systems-target` VM.

## What you learned

- A pipe is a bounded ring of sixteen kernel pages — capacity readable and
  resizable through `fcntl(F_GETPIPE_SZ/F_SETPIPE_SZ)` in whole-page units
  — and its blocking behavior in both directions is back-pressure, not a
  defect.
- `PIPE_BUF` (4096) makes small single writes atomic but promises nothing
  about framing, large writes, or reads — which is why framed `[out]`/`[err]`
  capture needs two pipes and a userspace line relay, and why the parent
  must close its write-end copies or EOF never comes.
- A FIFO is a pipe with an inode: `open` blocks until both ends exist,
  `O_WRONLY|O_NONBLOCK` turns "no reader" into a pollable `ENXIO`, and
  `SIGPIPE`-ignored plus `EPIPE`-handled is the contract every long-lived
  writer must sign.
- `splice(2)` moves file bytes into a pipe kernel-side — `strace` showed
  the relay running on nothing but `splice` — and advancing the file
  offset only for delivered bytes is what makes reader churn lossless in
  all three languages.

Next, **UNIX sockets and fd passing**: pmon grows a real control plane — a
socket a stranger can find, credentials the kernel vouches for, and open
file descriptors sent across process boundaries.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was reproduced on the Fedora 44 reference host (kernel
7.1.3-200.fc44) this session: the runner printed
<code>18-pipes-fifos-splice&nbsp;&nbsp;PASS&nbsp;&nbsp;PASS&nbsp;&nbsp;PASS</code>
(3 passed, 0 failed, 0 skipped); the F_GETPIPE_SZ probe printed capacity
65536, PIPE_BUF 4096, and the 1→4096 rounding live; the supervise run
produced the exact restart transcript and six-record log shown; the trio's
<code>/proc/&lt;pid&gt;/fd</code> listings (pipe inodes 16462630/16462631
shared child↔supervisor, fdinfo flags 02000000 and 02100001, log pos 10),
the detach/reattach transcript (<code>[out] one</code>, gap append delivered
to the second reader, <code>tail reader detached</code>,
<code>exiting (signal)</code>, exit 0), the C++ and Rust
<code>strace</code> splice excerpts (22- and 16-byte moves, 0 when idle,
<code>-1 EPIPE</code> on reader death), the
<code>pipe2([5, 6], O_CLOEXEC)</code> trace, and the dd-through-FIFO figure
(1073741824 bytes, 0.202097 s, 5.3 GB/s) are all real captured output. The
bcc-tools/bpftrace callout is unverified as marked and deferred to Part 8;
pipe capacity and throughput figures are host-specific.</p>
