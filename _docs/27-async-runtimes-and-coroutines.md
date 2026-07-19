---
title: "Async runtimes and coroutines"
order: 27
part: "Concurrency in Depth"
description: "chatterd v4 tells the same broadcast three async ways over the same OS threads: hand-rolled C++20 coroutines suspending on an epoll reactor, Go's G-M-P scheduler and netpoller that needed no rewrite, and tokio's tasks-and-Wakers — with /proc/<pid>/task thread counts (1 vs 17 vs dynamic) and a single-epoll-loop strace showing how each spends threads, plus the framework for when plain threads still win."
duration: "60 minutes"
---

Chapters 25 and 26 took concurrency down to the metal — the futex under a
mutex, atomics and a lock-free ring. This chapter goes the other direction:
up to the abstractions that let one program juggle tens of thousands of
connections without one thread each. `chatterd` — the book's peer-to-peer
chat daemon — gets a v4 **async rewrite** whose observable behaviour is
byte-for-byte unchanged: a message from one client is broadcast to every
other. What changes is the connection engine, selected by `serve --engine
thread|async`. The `thread` engine (one OS thread per client, the chapter 21
baseline) stays, so you can put the two side by side. The `async` engine is
a different story in each language: C++ grows a hand-rolled C++20 coroutine
reactor over `epoll`; Rust switches to **tokio**; and Go's async engine is
its *thread* engine, because goroutines were already the answer. The one new
idea is that all three suspend a *unit of work* — a coroutine frame, a
goroutine, a task — instead of parking an OS thread, and multiplex many of
them onto few threads. The concurrency lens is not a section here. It is the
whole chapter.

The code is in `examples/27-async-runtimes-and-coroutines/`. `./demo.sh`
builds all three; its `README.md` specifies the CLI, the canonical chatterd
chat frame (magic `"CH"` + version + type + big-endian length, unchanged
since ch21 — this chapter's engines use only its JOIN/MSG/DELIVER types),
and the exit codes all three languages share.

{% include excalidraw.html
   file="27-coroutine-suspension"
   alt="Three horizontal bands. In the user band, a connection() coroutine runs four boxes left to right: step 1 co_await fill() with recv returning EAGAIN, step 2 the accent-highlighted co_await ReadAwaitable calling await_suspend(h), step 3 a dashed frame-suspended box where control returns down, and step 6 h.resume() where recv returns bytes and decodes. In the reactor band (the single OS thread, Reactor::run) sit wait_readable(fd,h) which stores waiters_[fd]=h and arms the fd, and epoll_wait(-1) which resumes the parked handle. In the kernel band an EPOLLIN | EPOLLONESHOT box marks the fd ready via events[i].data.fd. An amber suspend arrow runs down from step 2 to wait_readable, an epoll_ctl arrow arms the kernel fd, an amber ready arrow returns to epoll_wait, and an amber resume arrow returns up to step 6."
   caption="Figure 27.1 — chatterd v4's C++20 coroutine engine: co_await ReadAwaitable suspends the connection frame and parks its handle in the reactor; epoll_wait resumes the exact handle on readiness. Suspension is a return, not a blocked thread." %}

> **Tools used** — `strace`, `ls`/`/proc/<pid>/task`, `nproc`, `python3`
> (host); `offcputime`, `tcplife`, `tcpconnect` from bcc-tools (lab VM,
> exercised in Part 8). Everything host-side is checked by
> `scripts/check-host.sh` or ships with Fedora.

## One machine, three stories over the same threads

Every async runtime solves one problem: a blocking `recv` wastes a whole OS
thread while it waits, and threads are expensive — a megabyte of stack, a
scheduler slot, a context-switch each time the kernel changes your mind. The
readiness model from chapter 9 already fixed the *waiting* (nonblocking fds
plus one `epoll_wait`), but it left the *code* inside out: a hand-written
state machine per connection, dispatching on a token. Coroutines, goroutines,
and tasks all buy back the straight-line shape — you write
`read; broadcast; read` and the runtime slices it at each suspension point,
storing "where was I" somewhere cheaper than a thread. The three differ only
in *where that somewhere lives* and *who drives the resume*.

## How the code works

**C++: a coroutine is a struct the compiler writes for you.** When a function
contains `co_await`, `co_return`, or `co_yield`, the compiler transforms it
into a state machine whose locals live in a heap-allocated *coroutine frame*,
and it hands control to a `promise_type` you supply. chatterd defines two.
`Detached` is fire-and-forget — `initial_suspend` and `final_suspend` both
return `std::suspend_never`, so it starts eagerly and destroys its own frame
when it finishes; the acceptor and every per-connection coroutine are
`Detached`. `Task<T>` is the lazy, value-returning kind, and its
`final_suspend` is where the interesting trick lives:

```cpp
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                auto c = h.promise().cont_;
                return c ? c : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
```

Returning a handle from `await_suspend` is **symmetric transfer**: instead of
returning to the caller and unwinding the stack, the finished task resumes its
awaiter directly, in the same stack frame — no recursion, no stack growth even
across thousands of chained resumptions. The `co_await` operator on `Task`
does the mirror image: `await_suspend` stashes the caller's handle in
`cont_` and returns the task's own handle to begin it. The awaitable that
connects all this to the kernel is tiny — it is the whole point of the
figure. `ReadAwaitable::await_suspend` hands the *current* coroutine handle to
the reactor, which stores it against the fd and arms `epoll`; when the fd
fires, `Reactor::run` calls `h.resume()` and the `co_await` expression
completes as if `recv` had simply blocked:

```cpp
    void wait_readable(int fd, std::coroutine_handle<> h) {
        waiters_[fd] = h;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLONESHOT;
        ev.data.fd = fd;
        if (armed_.contains(fd)) {
            ::epoll_ctl(ep_.get(), EPOLL_CTL_MOD, fd, &ev);
        } else {
            ::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev);
            armed_.insert(fd);
        }
    }
```

`EPOLLONESHOT` is chapter 9's multi-consumer safeguard reused for a different
reason: each `co_await` re-arms exactly once, so a readable fd fires for
exactly the one coroutine parked on it and nothing double-resumes. `data.fd`
carries the fd back, `waiters_[fd]` maps it to the handle, and `h.resume()`
runs the frame from its suspension point. That is the entire runtime — no
external async library is linked.

Below is the suspension point itself, beside the two runtimes that hide the
same mechanism. Each excerpt is the *wait-for-the-next-event-and-dispatch*
heart of its engine, verbatim from
`examples/27-async-runtimes-and-coroutines/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Pull at least one more byte into `buf`. false => EOF / error.
Task<bool> fill(Reactor& reactor, int fd, std::string& buf) {
    for (;;) {
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            co_return true;
        }
        if (n == 0) {
            co_return false; // peer closed
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            co_await ReadAwaitable{reactor, fd};
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        co_return false;
    }
}
```

```go
func (h *hub) run(ctx context.Context) {
	clients := make(map[*client]struct{})
	for {
		select {
		case c := <-h.register:
			clients[c] = struct{}{}
		case c := <-h.unregister:
			if _, ok := clients[c]; ok {
				delete(clients, c)
				close(c.out)
			}
		case b := <-h.broadcast:
			for c := range clients {
				if c != b.from {
					c.out <- b.frame // buffered; delivery is guaranteed
				}
			}
		case <-ctx.Done():
			for c := range clients {
				c.conn.Close() // unblock the reader goroutine
				close(c.out)
			}
			return
		}
	}
}
```

```rust
        loop {
            tokio::select! {
                // Every later frame is MSG: its payload is the message text.
                r = read_frame(&mut rd) => match r {
                    Ok((_typ, body)) => {
                        let frame = encode_frame(TYPE_DELIVER, &format!("{nick}\0{body}"));
                        let map = clients.lock().unwrap();
                        for (cid, tx) in map.iter() {
                            if *cid != id {
                                let _ = tx.send(frame.clone());
                            }
                        }
                    }
                    Err(_) => break,
                },
                _ = shutdown.recv() => break,
            }
        }
```

**Go: nothing to rewrite.** The Go excerpt has no `epoll`, no coroutine, no
`.await` — and that is the lesson. `serve --engine async` runs the *same*
goroutine-per-connection hub as `--engine thread`; the flag exists only for
parity. Each `handleConn` is its own goroutine calling a plain blocking
`readFrame`; when that read would block, the runtime parks the *goroutine* on
its **netpoller** (an `epoll` instance the runtime owns) and runs another on
the same OS thread. A single `hub` goroutine owns the client set and fans out
broadcasts over buffered channels, so no lock guards the map — chapter 6's
"share memory by communicating." Go absorbed the async model in 2012; the
`select` above *is* an event loop, one level up, with channels where the
kernel has fds.

**Rust: tokio, `.await` as a compiler transform.** An `async fn` compiles to
a state machine implementing `Future`; `.await` desugars to a loop that calls
`poll(cx)` and, on `Poll::Pending`, returns — after squirrelling away the
`Waker` from `cx`. tokio's `serve` builds a multi-threaded runtime and
`tokio::spawn`s one `handle_conn` task per connection; `read_frame(&mut rd)`
inside the `select!` is where the task yields. When the socket has nothing,
tokio's reactor (built on `mio`, itself `epoll`) holds the task's Waker; on
readiness it calls `wake()`, which re-enqueues the task for a worker thread to
`poll` again. The fan-out uses a per-client `mpsc` channel drained by a small
writer task — the same decoupling as Go's `c.out`, spelled in tokio's types.

A forward glance for C++: `std::execution` (senders/receivers, P2300) was
merged into the C++26 working draft and points at a *standard* vocabulary for
exactly this — a portable way to describe async work and the executor that
runs it, so you would not hand-roll `Task` and `Reactor` at all. It is
genuinely forward-looking; this chapter builds the machinery by hand precisely
to show what a runtime does underneath such an abstraction.

## Errors, three ways

The CLI contract is the book's usual: clean shutdown exits 0, runtime failure
exits 1 with one diagnostic, usage errors exit 2 — and `verify.lua` asserts
all of it (42 checks: 14 per language across both engines plus the CLI cases).
The async-specific error is *cancellation*. On `SIGTERM` each engine must
unwind in-flight work without a leak. C++ flips an `std::atomic<bool>` in an
async-signal-safe handler and writes to an `eventfd` registered in the
reactor, so the blocked `epoll_wait` returns, the loop guard sees the flag,
and every self-owning `Detached` frame is dropped as its fd closes. Go uses
`signal.NotifyContext`, so `<-ctx.Done()` becomes another `select` arm that
closes each `c.out` and the listener. tokio makes the signal a stream —
`sigint.recv()`/`sigterm.recv()` are arms of the accept `select!`, and a
`broadcast` channel tells every connection task to `break`. Three spellings of
the same discipline: turn the signal into just another event the loop already
knows how to wait on. All three then print `chatterd: shutdown` and exit 0.

## Concurrency lens

This is the whole chapter, so quantify it. The observable difference between
the three engines is *how many OS threads they spend to hold the same
connections* — and `/proc/<pid>/task` counts them directly. Serving 50 held
connections on this 16-core host (`nproc` = 16, `GOMAXPROCS` = 16):

| engine | idle threads | with 50 clients | model |
|---|---|---|---|
| C++ `async` (coroutines) | 1 | **1** | one reactor thread, 50 frames |
| Rust `async` (tokio) | 17 | **17** | 1 main + 16 `tokio-rt-worker` (= cores) |
| Go `async` (goroutines) | 7 | **11** | M threads grown on demand, ≤ P |
| C++ `thread` | 1 | **51** | one OS thread per client |
| Rust `thread` | 1 | **51** | one OS thread per client |

The three async engines pin three philosophies. The C++ coroutine reactor is
resolutely **single-threaded** — 50 connections, one thread — so, exactly like
chapter 9's epoll loop, it needs no locks on the reactor's own state and no
`EPOLLONESHOT` re-arm race beyond the one it already runs. tokio commits to
**worker-threads-equal-cores**: a fixed 17 (one `app` main thread plus 16
`tokio-rt-worker` threads, confirmed by their `comm` names), independent of
connection count — tasks migrate between workers by work-stealing. Go sits
between: goroutines are scheduled onto at most `GOMAXPROCS` logical processors
(P), but the runtime only *materialises* OS threads (M) as work demands, so
the count drifts (7 idle, 11 under load) rather than tracking a constant. The
two thread engines are the control: 1 + 50 = 51, the cost the async engines
exist to avoid.

Two runtime mechanisms deserve names. **Go's preemption**: before Go 1.14 a
goroutine in a tight loop with no function calls could starve its P forever;
now the runtime's monitor sends **`SIGURG`** (chapter 12's async-preemption
signal) to an M whose goroutine has run past ~10 ms, and the handler forces a
yield — cooperative scheduling with an involuntary backstop. **tokio's
Waker**: a task that returns `Poll::Pending` is *inert* until something calls
`wake()` on the Waker it registered; the reactor is that something, calling it
from the `epoll_wait` that noticed the fd. Miss a `wake()` and the task hangs
forever — the async analogue of a lost wakeup. When a tokio program *does*
stall, `tokio-console` is the tool: it shows per-task poll counts and the tasks
that never yield, the way `top` shows busy threads.

{% include excalidraw.html
   file="27-gmp-scheduler"
   alt="Three bands. The top band holds goroutines G — conn 1, conn 2, conn 3, and G times N — plus a dashed G blocked in recv, parked on the netpoller not on an M. The middle band is the scheduler: P0 and P1 each a logical processor bound to an OS thread M with a local run queue, then ellipsis P15 marked equals 16 cores, and an accent netpoller box, one epoll instance, running epoll_wait for the whole runtime. The bottom kernel band shows an async-preemption box where the runtime sends SIGURG to an M so a G running over 10 ms yields, and an epoll interest list where a socket readable puts a G back on a run queue. Amber arrows show the blocked G parking on the netpoller and readiness returning it to runnable; grey arrows show goroutines running on Ps."
   caption="Figure 27.2 — Go's G-M-P scheduler and netpoller: goroutines suspend on I/O against one runtime-owned epoll instance, SIGURG preempts CPU hogs, and no async rewrite was ever needed." %}

{% include excalidraw.html
   file="27-tokio-task-lifecycle"
   alt="Three bands. The top band is tokio tasks — an async fn per connection compiled to a state machine: tokio::spawn(handle_conn), an accent .await arrow to poll(cx) returning Poll::Pending, a dashed task-parked box with the Waker held by the reactor, and poll(cx) again returning Poll::Ready and sending the frame. The middle band is 16 tokio-rt-worker threads equal to cores, work-stealing schedulers, with a worker thread that drives poll and a run queue where Waker::wake re-enqueues the task. The bottom band is the mio reactor, one epoll instance, whose epoll_wait on a readable fd calls the registered Waker. Amber arrows trace poll, wake, and re-poll around the cycle."
   caption="Figure 27.3 — tokio's task lifecycle: 20k tasks share 16 worker threads; .await parks a task and registers a Waker, epoll readiness fires that Waker, and the task is re-polled to completion." %}

### The threads-vs-async decision

The table is also the decision framework. Reach for **plain threads** when
connections are few or the work per connection is CPU-bound and long — a
thread *is* a coroutine the kernel schedules, and you get preemption and
parallelism for free with none of the coloured-function friction. Reach for
**async** when you have many mostly-idle connections (a chat fan-out, a proxy,
a poller): 10k threads cost 10 GB of stacks and murder the scheduler, while
10k coroutine frames cost a few hundred bytes each. Between them: if you are in
Go, the choice is made — goroutines are cheap enough to thread-per-connection
*and* the runtime multiplexes them, so you write blocking code and get async
behaviour. In C++ or Rust the choice is real, and the practical tie-breaker is
that async adds a runtime you must understand to debug — which is why the rest
of this book's servers stayed single-threaded-epoll or thread-per-client until
a chapter, this one, whose subject *is* the runtime.

## Build, run, observe

```bash
[host]$ cd examples/27-async-runtimes-and-coroutines && ./demo.sh build
```

The runner (`python3 scripts/test-all-examples.py --only
27-async-runtimes-and-coroutines`) reports `PASS PASS PASS` for cpp, go, rust.
Driving one engine by hand the way the evidence above was produced:

```bash
[host]$ ./cpp/build/release/app serve --engine async --port 47100 &
chatterd: listening on 127.0.0.1:47100 engine=async
[host]$ ./cpp/build/release/app loadtest --port 47100 --clients 20
loadtest: delivered 20/20
[host]$ kill -TERM %1
chatterd: shutdown
```

`loadtest` opens 20 receivers plus a sender, broadcasts `hello world`, and
checks every receiver got `sender: hello world`; it exits 0 iff all 20 land.
Swap `--engine thread` and the transcript is identical, byte for byte on the
wire — the async rewrite changed the *how*, never the *what*.

## Cross-check: count the threads, trace the loop

**Thread counts, from `/proc`.** The table above is a live measurement: start
an engine, hold 50 connections open with a small Python client, and count
`/proc/<pid>/task`:

```console
[host]$ ls /proc/$SRV/task | wc -l          # cpp async, 50 clients held
1
[host]$ ls /proc/$SRV/task | wc -l          # tokio, 50 clients held
17
[host]$ for t in /proc/$SRV/task/*; do cat $t/comm; done | sort | uniq -c
      1 app
     16 tokio-rt-worker
```

One thread for the coroutine engine regardless of load; seventeen for tokio,
sixteen of them named `tokio-rt-worker` — worker-threads-equal-cores made
visible.

**One epoll loop, under `strace`.** Tracing the C++ coroutine engine while it
serves the 20-client loadtest, the syscall summary proves the reactor is a
single epoll loop with no threads spawned at all:

```console
[host]$ strace -f -c ./cpp/build/release/app serve --engine async --port 48403
   calls    errors syscall          (time/usecs columns trimmed)
--------- --------- ----------------
       65        22 recvfrom
       46           epoll_ctl
       22         1 accept4
       20           sendto
        6         1 epoll_wait
        1           epoll_create1
        1           eventfd2
        1           futex
```

Read it against the code: **one** `epoll_create1` (one interest list), **zero**
`clone`/`clone3` (no thread ever created — a single `futex` call, uncontended,
confirms it), 22 `accept4` for the 21 connections plus the trailing `EAGAIN`,
65 `recvfrom` with 22 `EAGAIN`s (each `EAGAIN` is a `co_await ReadAwaitable`
suspension), 46 `epoll_ctl` (the `EPOLLONESHOT` re-arm on every resumption),
and **20** `sendto` — the broadcast reaching all 20 receivers. Every mechanism
in Figure 27.1 is one line of that table. The tokio and Go engines, by
contrast, show `clone3` calls at startup (their worker/monitor threads) and
their epoll traffic issued from threads your code never wrote — the same
inversion chapter 9 saw with Go's netpoller, now with a tokio runtime beside
it.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the kernel-side view of *where the threads actually go* needs bcc-tools and
> root, so it is deferred to Part 8 on `systems-target`: `offcputime` folds an
> off-CPU flamegraph showing the coroutine reactor blocked in one `epoll_wait`
> while tokio's workers park in `futex`, and `tcplife`/`tcpconnect` tally the
> connection lifetimes each engine handles. Those are not run on this host.

## What you learned

- Async runtimes all suspend a *unit of work* — a coroutine frame, a
  goroutine, a task — instead of an OS thread, and multiplex many onto few.
  C++20 `co_await`/`co_return` compile to a heap frame plus a `promise_type`;
  `ReadAwaitable::await_suspend` parks the `std::coroutine_handle` in an epoll
  reactor that resumes it on readiness, with `EPOLLONESHOT` making the re-arm
  race-free.
- Go needed no rewrite: goroutines already suspend on I/O against a
  runtime-owned netpoller (epoll), scheduled onto `GOMAXPROCS` processors with
  `SIGURG` async preemption as the backstop — `select` is the event loop one
  level up.
- tokio desugars `.await` to `poll(cx)` returning `Poll::Pending` and
  registering a `Waker`; the mio reactor calls that Waker on readiness to
  re-enqueue the task onto one of its worker-threads-equal-cores;
  `tokio-console` is the debugger for stalled tasks.
- The measured cost tells you which to pick: for 50 held connections, C++
  coroutines spent **1** thread, tokio **17**, Go **7–11**, and the
  thread-per-client engines **51** — async wins on many idle connections,
  plain threads win on few or CPU-bound ones.

Next, the Debugging part opens with **gdb** — from a core dump on your desk to
a remote `gdbserver` in the lab VM, learning to read the state these runtimes
build at rest.

---

<p><span class="status status--verified">verified</span> — all evidence here
was produced on the Fedora 44 reference host (kernel 7.1.3-200.fc44, GCC
16.1.1, Go 1.26.5, rustc 1.97.1, tokio 1.53.0, strace 7.1, 16 cores,
GOMAXPROCS 16) this session: the runner printed
<code>27-async-runtimes-and-coroutines  PASS  PASS  PASS</code> (3 passed, 0
failed, 0 skipped; verify.lua 14 checks per language over both engines plus the
CLI cases); the hand-driven session produced the exact
<code>listening … engine=async</code>, <code>loadtest: delivered 20/20</code>,
and <code>chatterd: shutdown</code> lines; the thread-count table is live
<code>/proc/&lt;pid&gt;/task</code> counts with 50 held connections (cpp async
1, tokio 17 with 16 <code>tokio-rt-worker</code> threads by <code>comm</code>,
go 7 idle / 11 under load, both thread engines 51); and the C++ async
<code>strace -c</code> summary is real trimmed output (1 <code>epoll_create1</code>,
6 <code>epoll_wait</code>, 46 <code>epoll_ctl</code>, 22 <code>accept4</code>,
65 <code>recvfrom</code> with 22 <code>EAGAIN</code>, 20 <code>sendto</code>, 1
<code>futex</code>, no <code>clone</code>). The forward-looking
<code>std::execution</code>/P2300 note is marked as such. The "On the lab VM"
bcc-tools callout (offcputime/tcplife/tcpconnect) is unverified as marked and
is exercised in Part 8.</p>
