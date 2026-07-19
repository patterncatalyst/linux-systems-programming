---
title: "Scaling the server"
order: 22
part: "Networking"
description: "chatterd v1 holds many connections from one thread: nonblocking accept and per-connection read/write state machines, an outbound queue with EPOLLOUT backpressure reusing chapter 9's level-triggered epoll — then the chapter's real comparison, Go's goroutine-per-connection model with the runtime netpoller running epoll_pwait underneath, proven with per-process Threads counts, ss -tp, and strace."
duration: "55 minutes"
---

Chapter 21 gave `chatterd` a length-prefixed frame protocol and the obvious
server: one OS thread per connection, each blocked in `recv`. That design is
correct and it is easy to reason about — right up to the point a decade of
network programmers named the **C10K problem**: ten thousand idle clients means
ten thousand kernel threads, ten thousand stacks, and a scheduler run queue that
spends more time context-switching than working. This chapter keeps the threaded
engine (it still passes every test) and adds a second one that holds the same N
connections from a *single* thread. The one new idea is the **readiness model
from chapter 9, aimed at sockets**: put every fd in nonblocking mode, let one
`epoll_wait` tell you which are ready, and make each connection a little state
machine with an inbound buffer and an outbound queue so a partial read or a
blocked write is just *state*, never a stalled thread. Then the payoff — Go
writes the naive goroutine-per-connection code and gets the epoll engine for
free, because its runtime hides exactly this machinery underneath.

The code is in `examples/22-scaling-the-server/`. `./demo.sh` there builds all
three implementations and runs an end-to-end demo; its `README.md` specifies the
CLI, the wire format (unchanged since chapter 21), and the exit codes all three
languages share.

{% include excalidraw.html
   file="22-connection-state-machine"
   alt="Two bands. The top READ PATH band runs left to right: a recv() loop draining nonblocking until EAGAIN feeds an inbuf that reassembles partial frames across reads, into take_frame which asks whether one whole 6+LEN frame is ready, into MSG to broadcast which appends the frame to every connection's outbuf. The bottom WRITE PATH and BACKPRESSURE band: broadcast drops for each conn into queue() plus flush() which send()s what fits now; on send() EAGAIN the remainder stays in outbuf (the backpressure queue) because the kernel socket buffer is full; an amber arrow labelled full: arm EPOLLOUT runs down-left to an epoll_wait node that later reports EPOLLOUT for this conn, and an amber arrow labelled EPOLLOUT wakeup runs back up-right to flush(), closing the loop. A note reads: level-triggered, EPOLLIN always armed, EPOLLOUT armed only while outbuf is non-empty."
   caption="Figure 22.1 — chatterd's epoll engine as one per-connection read/write state machine; the EPOLLOUT backpressure loop is the whole reason a slow client cannot stall the server" %}

> **Tools used** — `strace`, `ss`, `python3` (host); `perf`, bcc-tools
> `offcputime`/`tcplife`/`tcpconnect` (lab VM, exercised in Part 8). Everything
> host-side is checked by `scripts/check-host.sh` or ships with Fedora.

## Nonblocking sockets, EAGAIN, and the state machine

A blocking server couples one thread to one connection because `recv` *waits*.
Break that coupling and a single thread can service everyone, but the price is
that every operation now returns "not yet" instead of blocking — and "not yet"
has a name: `EAGAIN` (equivalently `EWOULDBLOCK`). The listener is created
nonblocking (`SOCK_NONBLOCK` in the `socket` type), so `accept4` returns
`EAGAIN` when the backlog is drained; each client socket is nonblocking, so
`recv` returns `EAGAIN` when the kernel receive buffer is empty and `send`
returns `EAGAIN` when its *send* buffer is full. None of those are errors — they
are the readiness model telling you to go wait in `epoll_wait` and come back.

Because a nonblocking `recv` can hand you *any* number of bytes — half a frame,
three frames, one frame plus a header — a connection cannot be a straight-line
function anymore. It has to be **state that survives across `epoll_wait`
iterations**. That is what `EConn` is:

```cpp
struct EConn {
    Fd fd;
    std::string nick = "?";
    std::string inbuf;
    std::string outbuf; // pending outbound bytes (the backpressure queue)
    bool want_write = false;
    bool dead = false;
};
```

`inbuf` accumulates whatever `recv` returns until `take_frame` — the same
6-byte-header parser from chapter 21 — can lift a whole frame off the front;
leftover bytes stay for next time. `outbuf` is the mirror image and the heart of
the chapter: bytes we *want* to send but the kernel would not take. `want_write`
records whether we currently have `EPOLLOUT` armed for this connection (so we
only call `epoll_ctl` on a real transition), and `dead` marks a connection to
reap after the current event batch rather than deleting it mid-iteration. The
connections live in `std::unordered_map<std::uint64_t, EConn>` keyed by an epoll
**token**, `next_tok_` starting at 2 because tokens 0 and 1 are reserved for the
listener and the `signalfd` — the same "let the kernel echo back a small integer
so dispatch is a `switch`, not an fd lookup" trick chapter 9 used.

## How the code works

Setup is four fallible steps, each guarded by `std::expected`:
`make_listener(port, nonblock=true)`, `make_signalfd` (block `SIGINT`/`SIGTERM`
process-wide, then read them as data), `epoll_create1(EPOLL_CLOEXEC)`, and two
`epoll_ctl` ADDs registering the listener and the signal fd with `EPOLLIN`. Then
`run()` is the entire server: a `switch` on the token the kernel returned.

```cpp
            for (int i = 0; i < n; ++i) {
                const auto tok = events[i].data.u64;
                if (tok == kListen) {
                    accept_ready();
                } else if (tok == kSignal) {
                    reap_closed();
                    std::println(stderr,
                                 "chatterd: stopped engine=epoll messages={} peak_conns={}",
                                 messages_, peak_);
                    return 0;
                } else {
                    if (!conns_.contains(tok)) {
                        continue; // reaped earlier in this batch
                    }
                    if ((events[i].events & (EPOLLHUP | EPOLLERR)) != 0) {
                        conns_.at(tok).dead = true;
                    }
                    if ((events[i].events & EPOLLIN) != 0) {
                        on_readable(tok);
                    }
                    if (conns_.contains(tok) && (events[i].events & EPOLLOUT) != 0) {
                        flush(tok);
                    }
                }
            }
            reap_closed();
```

Three details earn their place. The `conns_.contains(tok)` re-check before
`flush` matters because `on_readable` can mark a connection dead (or a broadcast
can), and acting on a fd we just decided to drop is a use-after-free waiting to
happen; deferring the actual erase to `reap_closed` at the end of the batch is
how a single-threaded loop safely mutates the container it is iterating events
from. `accept_ready` loops `accept4(…, SOCK_CLOEXEC | SOCK_NONBLOCK)` until
`EAGAIN` — one `EPOLLIN` on the listener can mean *many* pending connections, and
draining to `EAGAIN` is the level-triggered discipline from chapter 9 applied to
the backlog. Each new connection is registered `EPOLLIN`-only and immediately
handed a `WELCOME` frame via `queue`, which is the client's proof it is in the
fan-out set. `on_readable` drains `recv` into `inbuf` to `EAGAIN`, then pulls
every complete frame: a `JOIN` sets the nick, a `MSG` bumps the counter and
`broadcast`s a `DELIVER` frame — which is just `queue` called for every
connection.

`queue` and `flush` are where backpressure lives, and where the three languages
are worth reading side by side. `queue` appends to `outbuf` and calls `flush`;
`flush` sends as much as the kernel will take and, crucially, *decides whether to
arm `EPOLLOUT`* based on what is left over:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    void flush(std::uint64_t tok) {
        auto& conn = conns_.at(tok);
        while (!conn.outbuf.empty()) {
            const ssize_t n = ::send(conn.fd.get(), conn.outbuf.data(), conn.outbuf.size(),
                                     MSG_NOSIGNAL);
            if (n > 0) {
                conn.outbuf.erase(0, static_cast<std::size_t>(n));
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break; // socket buffer full: keep the rest, wait for EPOLLOUT
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                conn.dead = true;
                return;
            }
        }
        // Register/clear EPOLLOUT interest to match the backlog state.
        const bool need = !conn.outbuf.empty();
        if (need != conn.want_write) {
            conn.want_write = need;
            epoll_event e{.events = EPOLLIN | (need ? EPOLLOUT : 0u), .data = {.u64 = tok}};
            ::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, conn.fd.get(), &e);
        }
    }
```

```go
		case frame := <-h.broadcast:
			h.messages.Add(1)
			// Blocking send is the backpressure: a slow client slows the hub
			// rather than dropping a frame. writePump keeps draining even on
			// write errors, so this never deadlocks on a dead peer.
			for c := range h.clients {
				c.send <- frame
			}
```

```rust
fn flush(reg: &mio::Registry, conns: &mut HashMap<usize, EConn>, tok: usize) {
    let c = match conns.get_mut(&tok) {
        Some(c) => c,
        None => return,
    };
    while !c.outbuf.is_empty() {
        match c.stream.write(&c.outbuf) {
            Ok(0) => {
                c.dead = true;
                return;
            }
            Ok(n) => {
                c.outbuf.drain(0..n);
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => break, // buffer full: wait for WRITABLE
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(_) => {
                c.dead = true;
                return;
            }
        }
    }
    let need = !c.outbuf.is_empty();
    if need != c.want_write {
        c.want_write = need;
        let interest = if need {
            Interest::READABLE | Interest::WRITABLE
        } else {
            Interest::READABLE
        };
        let _ = reg.reregister(&mut c.stream, Token(tok), interest);
    }
}
```

The C++ and Rust `flush` are the same state machine: drain until the socket says
`EAGAIN`/`WouldBlock`, then toggle write-interest to match. When a client stops
reading, its kernel send buffer fills, `send` returns `EAGAIN`, the remaining
bytes sit in `outbuf`, and `epoll_ctl`/`reregister` arms `EPOLLOUT` so the next
`epoll_wait` wakes us to try again — the amber loop in Figure 22.1. That is
backpressure with no thread blocked and no frame dropped: a slow reader grows its
own `outbuf` and nobody else's delivery is affected. The only asymmetry is the
library layer — mio's `reregister` takes an `Interest` set where raw epoll takes
a bitmask — but `EPOLLIN|EPOLLOUT` *is* `Interest::READABLE | Interest::WRITABLE`
underneath. Go's excerpt looks nothing alike, and that is the whole point of the
next section: its backpressure is a *blocking channel send*. `c.send` is a
buffered channel (capacity 64); when a client's `writePump` can't keep up the
channel fills, the send blocks, and the hub slows down — the same "slow reader
slows the writer, never drops" invariant, expressed as goroutines instead of
`EPOLLOUT`.

## Go: goroutine-per-connection, epoll underneath

{% include excalidraw.html
   file="22-netpoller-vs-epoll"
   alt="Two side-by-side bands. Left, titled C++/Rust — you write the epoll loop: one OS thread (Threads: 1) calling epoll_wait (4 calls for a 50-conn flood) connected by a bidirectional amber arrow labelled EPOLLIN/EPOLLOUT + token to a kernel eventpoll interest list holding tok 0 listener, tok 1 signalfd, tok 2..N every client socket, one process; a note reads backpressure = outbuf + EPOLLOUT (explicit). Right, titled Go — you write goroutines; the runtime hides the epoll: a goroutine per conn (read + write) plus hub with channels fanning out over ~7 OS threads total, a bidirectional amber arrow park/resume goroutine down to a dark runtime netpoller (hidden) box running epoll_pwait (165 calls, edge-triggered EPOLLET), then down to the same kernel sockets, one process, N conns (ss -tp); a note reads backpressure = blocking channel send (implicit); threaded engine = same code, 51 threads."
   caption="Figure 22.2 — the chapter's central comparison: the epoll loop you write by hand in C++/Rust is the epoll loop Go's runtime writes for you, driven by epoll_pwait one layer down" %}

Go's server has no `epoll_create1`, no `epoll_ctl`, no `epoll_wait` — and it
still multiplexes thousands of sockets on a handful of threads. Each accepted
connection gets two goroutines (a `readPump` and a `writePump`) plus a shared
`hub` goroutine that owns the client set and fans `broadcast` out over channels.
The `readPump` calls `fr.next()`, which calls `conn.Read`, which *looks*
blocking; but `net.Conn` fds are registered with the runtime's **network
poller**, so a would-be-blocking `Read` parks the *goroutine* — not the OS
thread — until the netpoller's `epoll_pwait` reports the socket readable, then
resumes it. The `--engine threaded|epoll` flag on the Go binary changes only the
label it prints; both values run this exact code, because for Go there is no
second design to write. This is the payoff Figure 22.2 draws: the hand-rolled
epoll interest list on the left and the runtime netpoller on the right are the
same kernel object, and the `strace` cross-check below shows Go issuing the
`epoll_pwait` calls our C++ loop makes explicit — plus the edge-triggered
`EPOLLET` registration, because the runtime, unlike our level-triggered loop,
always drains to `EAGAIN` and parks on the transition.

## Errors, three ways

The contract is chapter 21's: exit 0 on success, 1 on a runtime failure with one
diagnostic line, 2 on a usage error — and `verify.lua` asserts all three per
language (24 checks each, 72 total). The interesting split is which side owns
which error. A client against a dead port fails `connect` and exits 1 with
`chatctl: cannot connect to 127.0.0.1:1: Connection refused` — `std::error_code`
text in C++, a `%w` chain in Go, `anyhow` context in Rust, the same errno
underneath. The *server* must survive its clients: a peer that hangs up mid-write
turns `send` into an error, which just marks that `EConn` dead and reaps it —
`MSG_NOSIGNAL` on every `send` is what stops a dead peer from killing the whole
process with `SIGPIPE`. A foreign speaker whose bytes don't start with the `CH`
magic desyncs one connection; `take_frame` clears its buffer and the stream is
dropped, never the server. And the two "not yet" returns are *successes*, not
failures: `EAGAIN` from `recv` means "drained, go wait", `EAGAIN` from `send`
means "buffered, arm `EPOLLOUT`". Confusing either for an error is how naive
nonblocking servers spin at 100% CPU or drop frames.

## Concurrency lens

Count the threads doing your work, and the three engines separate cleanly. The
epoll engine is **one** thread: `conns_` is touched by nobody else, so there are
no locks, no atomics, and the event loop serializes every mutation by
construction — the single-threaded design *is* the concurrency strategy. The
threaded engine is **1 + N**: a `Registry` behind a mutex, a per-socket write
mutex so two broadcasters can't interleave bytes on one wire, and `std::atomic`
counters; simple per connection, but every message wakes N reader threads. Go is
a bounded handful of runtime threads regardless of connection count, with
`pending`-style state confined to the hub goroutine and everything else
communicating by channel — chapter 6's "share memory by communicating" applied to
I/O. Measured on this host with 50 connections held open: the C++ epoll daemon
reports `Threads: 1`, the C++ threaded daemon `Threads: 51`, and the Go daemon
`Threads: 7` (near, but not pinned to, this box's 16 CPUs — the runtime grows
threads on demand up to `GOMAXPROCS` runnable). One process, one design decision,
three very different rows in `/proc/<pid>/status`.

## Build, run, observe

```bash
[host]$ cd examples/22-scaling-the-server && ./demo.sh cpp
```

The runner (`python3 scripts/test-all-examples.py --only 22-scaling-the-server`)
reports `PASS PASS PASS` for cpp, go, rust (3 passed, 0 failed, 0 skipped). To
drive it by hand the way this chapter's evidence was produced, start an epoll
daemon on an ephemeral port, listen as `bob`, and send as `alice`:

```bash
[host]$ ./cpp/build/release/app serve --engine epoll --port 0 &
chatterd: serving engine=epoll on 127.0.0.1:45829
[host]$ ./cpp/build/release/app listen --port 45829 bob --count 1 &
[host]$ ./cpp/build/release/app send --port 45829 alice "hello, chatterd"
alice: hello, chatterd
```

`bob`'s terminal prints the same `alice: hello, chatterd` line, because every
`MSG` is delivered to every connection including the sender — seeing your own
echo is how `send` knows it was accepted. On `SIGTERM` the daemon prints
`chatterd: stopped engine=epoll messages=1 peak_conns=2`. The load case is
`flood`, which opens N concurrent connections, waits for all N `WELCOME`s, has
one of them broadcast, and counts deliveries:

```bash
[host]$ ./cpp/build/release/app flood --port 45829 50
flood: connected 50
flood: delivered 50
```

Fifty connections, one broadcast, fifty deliveries, no dropped frames — and the
daemon's summary reads `messages=1 peak_conns=50`. The threaded engine and all
three languages produce byte-identical output; that identity is what lets one
`verify.lua` cover them.

## Cross-check: one thread, one process, one epoll_wait

Output equality could hide a server that quietly spun up a thread per client, so
the cross-checks watch the mechanism. **First, the shape of the process.** With
50 `listen` clients held open against each engine, `/proc/<pid>/status` and
`ss -tnp` agree:

```console
[host]$ grep Threads /proc/<pid>/status ; ss -tnp state established | grep -c '"app",pid=<pid>,'
Threads:	1          # epoll engine  (Rust: also 1)
50
Threads:	51         # threaded engine — one thread per connection
50
Threads:	7          # Go — netpoller, a bounded thread pool
50
```

All fifty established sockets belong to the *one* daemon pid in every case
(`users:(("app",pid=…,fd=12))` … `fd=49`); the engines differ only in how many
threads sit behind that one process. **Second, what drives the epoll engine.**
Tracing a two-party exchange, one thread does everything:

```console
[host]$ strace -f -e trace=epoll_wait,accept4,epoll_ctl -p <pid>
2825978 epoll_wait(5, [{events=EPOLLIN, data=0}], 64, -1) = 1
2825978 accept4(3, NULL, NULL, SOCK_CLOEXEC|SOCK_NONBLOCK) = 6
2825978 epoll_ctl(5, EPOLL_CTL_ADD, 6, {events=EPOLLIN, data=0x2}) = 0
2825978 accept4(3, NULL, NULL, SOCK_CLOEXEC|SOCK_NONBLOCK) = -1 EAGAIN (Resource temporarily unavailable)
2825978 epoll_wait(5, [{events=EPOLLIN, data=0x2}], 64, -1) = 1
2825978 epoll_ctl(5, EPOLL_CTL_DEL, 6, NULL) = 0
```

`data=0` is the listener token, `data=0x2`/`0x3` the two client tokens — exactly
the integers `accept_ready` assigned — and `accept4` loops to `EAGAIN` before the
next `epoll_wait`. Under a 50-connection flood, `strace -c` counts just **4**
`epoll_wait` calls covering 51 `accept4`, 100 `sendto`, and 100 `epoll_ctl` for
457 syscalls total: one wait multiplexes an entire batch. **Third, Go's hidden
epoll.** Tracing the Go binary shows the syscalls our code never wrote:

```console
[host]$ strace -f -e trace=epoll_ctl,epoll_pwait -p <go-pid>
2826032 epoll_pwait(6, [{events=EPOLLIN, data=0x3ffafa0b2e000002}], 128, -1, NULL, 0) = 1
2826032 epoll_ctl(6, EPOLL_CTL_ADD, 5, {events=EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET, data=0x3ffafa0b3d000002}) = 0
2826067 epoll_pwait(6 <unfinished ...>
```

The runtime registers sockets **edge-triggered** (`EPOLLET`) with an opaque
64-bit cookie where we used token integers, and issues the calls from *several*
threads (2826032, 2826067, 2826064): 165 `epoll_pwait` calls for the same flood
our C++ loop served in 4 `epoll_wait`s, the rest of its syscalls dominated by
`futex` and `nanosleep` from the scheduler. **Fourth, the C10K cost made
concrete.** Ten floods of 50 (500 connections) under `strace -c`:

```console
engine=epoll     : clone3   0   epoll_wait  40   accept4 510   total 4570   (0.017s in syscalls)
engine=threaded  : clone3 500   accept4    500                 total 8546   (1.85s in syscalls)
```

The threaded engine pays **one `clone3` per connection** — 500 thread creations —
and two orders of magnitude more time in the kernel; the epoll engine creates
zero threads and multiplexes the lot through 40 `epoll_wait` calls. That single
counter, `clone3: 500` versus `clone3: 0`, *is* the C10K problem in one line.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the kernel-side cost view — `perf stat` context-switches under load (this
> host's `perf_event_paranoid=2` restricts the counter to userspace, reading 0
> without privilege), `offcputime` flame graphs showing threaded workers parked
> off-CPU, and `tcplife`/`tcpconnect` tracing every connection's lifetime across
> the fleet — needs root and a disposable kernel, so it runs on the
> `systems-target` VM in Part 8, not here.

## What you learned

- The C10K escape is nonblocking sockets plus one `epoll_wait`: `EAGAIN` is not an
  error but "go wait", and a connection becomes *state* (`inbuf`, `outbuf`) that
  survives across wakeups rather than a thread blocked in `recv`.
- Backpressure is the outbound queue plus `EPOLLOUT`: a slow reader fills its own
  `outbuf`, we arm write-interest, and the next `epoll_wait` drains it — no thread
  blocks, no frame drops, no other client is affected. Level-triggered epoll from
  chapter 9 makes this safe with only "consume what woke you" discipline.
- Go's goroutine-per-connection code is the same epoll engine with the loop
  hidden: the netpoller runs `epoll_pwait` (edge-triggered) underneath and parks
  goroutines, not threads — 1 vs 51 vs 7 threads in `/proc`, all serving 50
  connections from one process.
- The mechanism is externally visible: `ss -tp` shows every connection on one
  pid, `strace` shows `epoll_wait` (or the runtime's `epoll_pwait`) driving
  everything, and `clone3: 0` versus `clone3: 500` measures the cost the whole
  design exists to avoid.

Next, **two hosts**: `chatterd` v2 leaves loopback for a real network — UDP
multicast discovery to find peers, then a TCP bridge carrying the same frames
between two VMs on one L2 segment.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, GCC 16.1.1, strace 7.1, 16 CPUs) this session: the runner printed
<code>22-scaling-the-server  PASS  PASS  PASS</code> (3 passed, 0 failed, 0
skipped; verify.lua 24 checks per language, 72 total); the hand-driven epoll
session produced the exact <code>alice: hello, chatterd</code> echo to both
sender and listener and the <code>chatterd: stopped engine=epoll messages=1
peak_conns=2</code> summary; <code>flood 50</code> printed <code>flood: connected
50</code> / <code>flood: delivered 50</code> with <code>peak_conns=50</code>; with
50 connections held open, <code>/proc/&lt;pid&gt;/status</code> read
<code>Threads: 1</code> (C++ and Rust epoll), <code>51</code> (C++ threaded), and
<code>7</code> (Go), while <code>ss -tnp</code> showed all 50 established sockets
owned by the one daemon pid; the C++ epoll <code>strace</code> excerpt is real
trimmed output (single thread, tokens 0/0x2/0x3, <code>accept4</code> to
<code>EAGAIN</code>, 4 <code>epoll_wait</code> calls for a 50-conn flood); the Go
trace showed <code>epoll_ctl … EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET</code> and 165
<code>epoll_pwait</code> calls across several threads; and the 500-connection
<code>strace -c</code> comparison recorded <code>clone3: 0</code> (epoll) versus
<code>clone3: 500</code> (threaded). The <code>perf</code>/bcc-tools kernel-side
view is unverified as marked (host <code>perf_event_paranoid=2</code>) and is
exercised on <code>systems-target</code> in Part 8.</p>
