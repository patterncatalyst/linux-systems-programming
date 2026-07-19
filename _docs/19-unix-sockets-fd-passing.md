---
title: "UNIX sockets and fd passing"
order: 19
part: "IPC"
description: "pmon grows a control plane on a UNIX stream socket: filesystem vs abstract addresses under ss -x, SO_PEERCRED as kernel-vouched local auth, and SCM_RIGHTS sending an open log fd across process boundaries — proven shared at the open-file-description layer with /proc fdinfo on both sides."
duration: "50 minutes"
---

Chapter 18 gave `pmon` a log pipeline built from pipes and FIFOs — fast,
kernel-buffered, and strictly byte-shaped: a pipe moves data, and only
between processes that inherited its ends. Version 5 needs something pipes
cannot do: a stranger — a `pmctl` process started minutes later, from
another terminal — must find the running supervisor, prove who it is, ask
questions, and get answers. That is a *control plane*, and the Linux
primitive for it is the UNIX domain socket: a socket with the full
`connect`/`accept` shape of TCP but living entirely inside the kernel, with
two abilities no network socket has. It can tell the server exactly which
uid and pid is calling (`SO_PEERCRED`), and it can carry *file descriptors*
in its messages (`SCM_RIGHTS`). The second one is the headline: `pmctl
logfd` never opens the log file — the supervisor hands it an already-open
descriptor, and the chapter closes the loop on chapter 7's three-layer model
by proving, with `/proc` on both processes at once, exactly which layer
crossed the socket.

The code is in `examples/19-unix-sockets-fd-passing/`. `./demo.sh` there
builds all three implementations; its `README.md` specifies the CLI, the
one-line wire protocol, and the exit codes all three languages share.

{% include excalidraw.html
   file="19-uds-control-plane"
   alt="Two process bands over a kernel band: the pmon supervise process holds listener fd 4 bound to /tmp/…/ctl.sock, an accepted fd 5 annotated with the SO_PEERCRED result uid=1000 pid=2624432, and log fd 3 opened O_RDWR|O_APPEND feeding a dashed child box for sleep 300 pid 2622905; the pmctl process holds client fd 3 which connects down to socket inode 16422777 in the kernel band, paired by a bidirectional amber arrow with socket inode 16411996 owned by the supervisor, exactly as ss -xp reported."
   caption="Figure 19.1 — pmon v5's control plane during one status call: every pid, fd number, and socket inode in this figure comes from the live run traced later in the chapter" %}

> **Tools used** — `ss`, `strace`, `stat`, `tail`, `ls`, `python3` (host);
> `bpftrace`/bcc-tools (lab VM, exercised in Part 8). Everything host-side is
> checked by `scripts/check-host.sh` or ships with Fedora.

## One family, three conversations

`AF_UNIX` sockets come in the same three types as `AF_INET`, and the choice
is about message shape, not speed. `SOCK_STREAM` is a connected, reliable
byte stream — no boundaries, which is why pmon's protocol is line-framed
("one `\n`-terminated command in, one reply out"). `SOCK_DGRAM` is
connectionless datagrams — unlike UDP they are reliable and ordered
kernel-local, but there is no connection, and without a connection there is
no accepted socket to ask `SO_PEERCRED` about, which rules it out here.
`SOCK_SEQPACKET` is the hybrid TCP never had: connection-oriented *and*
boundary-preserving, a good fit for record protocols. pmon uses a stream
because the framing is one trivial line and because the connection itself is
load-bearing: it scopes authentication (credentials are checked per accepted
connection) and delivery (the reply belongs to whoever wrote the command).

`socketpair(2)` is the degenerate case worth knowing: it returns two
already-connected `AF_UNIX` ends with no address at all, the right tool when
the two parties are parent and child and nobody needs to *find* anybody —
it is how container runtimes hand fds to the processes they spawn. pmon
needs the opposite: a rendezvous point a stranger can name. That means an
address, and UNIX sockets have two address namespaces.

## Two namespaces: a path, or nothing but a name

A **filesystem** socket is what `bind` creates when `sun_path` starts with a
normal character: a socket-type inode at that path. It obeys directory
permissions — parking `ctl.sock` in a `0700` directory *is* the access
policy — and it outlives its creator, which is why `listen_ctl` starts with
`unlink()`: a crashed supervisor leaves a stale inode that would make the
next `bind` fail with `EADDRINUSE`. An **abstract** socket, a Linux
extension, is what you get when `sun_path[0]` is a NUL byte: no inode, no
permissions, automatically gone when the last fd closes, and scoped to the
network namespace. Both kinds show up in `ss -x`, and the rendering
distinguishes them — on this host, with pmon's socket listening and a
one-line `python3` peer bound to the abstract name `\0pmon-demo`:

```console
[host]$ ss -xl | grep -E 'pmon|ctl.sock'
u_str LISTEN 0  8  /tmp/p19.t9j0/ctl.sock 16411867  * 0
u_str LISTEN 0  1              @pmon-demo 16406037  * 0
```

The `@` prefix is `ss`'s spelling of that leading NUL. pmon deliberately
uses the filesystem kind: the path *is* the discovery mechanism and the
permission check, and `stop` unlinking it is a clean "no longer accepting"
signal. The classic trap is capacity, not permissions: `sun_path` is 108
bytes, so a control socket under a deep CI workspace path fails to bind at
all — every implementation checks this (the C++ one returns
`socket path too long`), and `verify.lua` runs from `mktemp -d` for exactly
this reason.

## Who is calling? Ask the kernel, not the packet

The first thing the supervisor does with an accepted connection — before
reading a single command byte — is:

```cpp
ucred cred{};
socklen_t cred_len = sizeof(cred);
if (::getsockopt(conn.get(), SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) == 0) {
    std::println(stderr, "pmon: ctl connect uid={} pid={}", cred.uid, cred.pid);
}
```

`SO_PEERCRED` returns the pid, uid, and gid of the peer *as recorded by the
kernel at `connect` time*. The client cannot forge it, because the client
never supplies it — this is the difference between authentication and a
claim. On the live run every control connection produced a line like:

```console
pmon: ctl connect uid=1000 pid=2623337
```

and `verify.lua` asserts that the uid printed equals `id -u` in all three
languages. pmon only logs the identity; the one-line change to *enforce*
(compare `cred.uid` against `getuid()` and hang up) is the pattern every
socket-activated daemon on your machine uses. Two sharp edges: the pid is
the connector's pid at connect time — the peer may have exited or the pid
been reused by the time you act on it (chapter 13's pidfd argument, again),
and stronger per-message guarantees exist via `SCM_CREDENTIALS` if you need
them.

## What actually crosses the socket

`SCM_RIGHTS` is routinely described as "sending a file descriptor", and that
phrase hides the mechanism. The number does not travel — fd 3 in the sender
arrived as fd 4 in the receiver on the traced run below. What the kernel
does is precise in chapter 7's terms: it takes the sender's fd-table slot,
follows it to the **open file description**, allocates a *new slot in the
receiver's fd table* (lowest free, as always), and points it at that same
description. The result is exactly what `dup(2)` produces — shared offset,
shared status flags, independent close — except the two references live in
different processes. The inode layer never moves either; it was always
system-wide.

{% include excalidraw.html
   file="19-scm-rights-transfer"
   alt="Two fd-table bands over a shared kernel band: sender slot 3 with close-on-exec set at open, and receiver slot 4 labeled new lowest free with close-on-exec set by MSG_CMSG_CLOEXEC, both pointing at one dark open-file-description box holding pos 47 and flags 02102002, which points down to inode 95934 for pmon.log; sendmsg with cmsg_data 3 names the sender slot, an amber dashed arrow shows the kernel installing the new receiver slot, and recvmsg reports cmsg_data 4."
   caption="Figure 19.2 — SCM_RIGHTS in chapter 7's three layers: two per-process slots, one open file description, one inode; pos, flags, inode, and both fd numbers are from the held-open run below" %}

That layer choice has practical consequences pmon is built around. The log
is opened `O_RDWR | O_APPEND`, not `O_WRONLY` — flags live on the
description, so a write-only description would arrive write-only and
`pmctl`'s reads would fail `EBADF`. And `pmctl` reads with `pread` at
explicit offsets instead of `read`, because the *offset* is shared too: a
`lseek`+`read` would move the supervisor's append position out from under
it. (`O_APPEND` writes ignore the offset, so the writer is safe regardless —
but the discipline costs nothing and the point generalizes.)

## How the code works

The supervision half is chapter 11–13 machinery matured: `spawn_child`
forks/execs the command with `setpgid(0,0)` so `stop` can `SIGTERM` the
whole group, `dup2`s the log fd over stdout and stderr, and a reaper —
`std::jthread`, goroutine, or `std::thread` — `wait`s, backs off 300 ms, and
respawns, guarding a small `State{child, started, restarts, stopping}` with
a mutex (C++/Rust condvar, Go channels). What is new is the socket plumbing.
`listen_ctl` is the canonical four-step: `unlink` the stale path,
`socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)`, `bind` a `sockaddr_un`
(after the 108-byte check), `listen`. The accept loop takes each connection
with `accept4(…, SOCK_CLOEXEC)` — chapter 7's rule that the cloexec bit must
be set atomically applies to *every* fd source, and `accept` is one —
logs `SO_PEERCRED`, reads one line, and dispatches: `status` formats the
shared state, `stop` tears down, `logfd` calls the interesting function.
Sending, in the C++ spelling, verbatim from
`examples/19-unix-sockets-fd-passing/cpp/src/main.cpp`:

```cpp
void send_fd(int sock, int fd) {
    char payload[] = "ok\n";
    iovec iov{payload, 3};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    if (::sendmsg(sock, &msg, 0) < 0) {
        std::println(stderr, "pmon: sendmsg: {}", errno_text());
    }
}
```

Every line is load-bearing. Control messages ride in an *ancillary buffer*
alongside the normal payload, and the `CMSG_*` macros exist because that
buffer has alignment rules you should never hand-compute: `CMSG_SPACE(n)`
sizes the buffer (header + padded data — 24 bytes here), `CMSG_LEN(n)` is
the length recorded *inside* the header (20: header + 4 unpadded), and
`CMSG_DATA` locates the aligned payload slot where the fd is `memcpy`ed.
Getting `msg_controllen` and `cmsg_len` confused is the classic way to send
a message the receiver silently drops. The 3-byte `"ok\n"` payload is not
decoration either: ancillary data is delivered *with* a byte of real data,
and sending at least one byte is the portable way to guarantee the cmsg is
picked up by the receiver's next `recvmsg`.

The receive side is where the three languages diverge most, so it gets the
tabs — each function verbatim:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
Result<Fd> recv_fd(int sock) {
    char payload[16];
    iovec iov{payload, sizeof(payload)};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    ssize_t n = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return std::unexpected(std::format("recvmsg: {}", errno_text()));
    }
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(cm), sizeof(int));
            return Fd{fd};
        }
    }
    return std::unexpected("no SCM_RIGHTS control message in reply");
}
```

```go
func recvLogFd(conn *net.UnixConn) (int, error) {
	buf := make([]byte, 16)
	oob := make([]byte, unix.CmsgSpace(4))
	_, oobn, _, _, err := conn.ReadMsgUnix(buf, oob)
	if err != nil {
		return -1, fmt.Errorf("recvmsg: %w", err)
	}
	msgs, err := unix.ParseSocketControlMessage(oob[:oobn])
	if err != nil {
		return -1, fmt.Errorf("parse cmsg: %w", err)
	}
	for _, m := range msgs {
		fds, ferr := unix.ParseUnixRights(&m)
		if ferr == nil && len(fds) > 0 {
			return fds[0], nil
		}
	}
	return -1, errors.New("no SCM_RIGHTS control message in reply")
}
```

```rust
fn recv_log_fd(conn: &UnixStream) -> Result<OwnedFd> {
    let mut payload = [0u8; 16];
    let mut iov = [IoSliceMut::new(&mut payload)];
    let mut cmsg_buf = nix::cmsg_space!(RawFd);
    let msg = recvmsg::<()>(
        conn.as_raw_fd(),
        &mut iov,
        Some(&mut cmsg_buf),
        MsgFlags::MSG_CMSG_CLOEXEC,
    )
    .context("recvmsg")?;
    for cmsg in msg.cmsgs().context("parse cmsg")? {
        if let ControlMessageOwned::ScmRights(fds) = cmsg
            && let Some(&fd) = fds.first()
        {
            // SAFETY: the kernel installed this fd for us via SCM_RIGHTS;
            // we are its only owner.
            return Ok(unsafe { OwnedFd::from_raw_fd(fd) });
        }
    }
    bail!("no SCM_RIGHTS control message in reply")
}
```

Three details deserve emphasis. First, `MSG_CMSG_CLOEXEC`: the received fd
is installed by the kernel, not by any `open` you control, so this flag is
the only atomic way to give it the cloexec bit — the recvmsg-then-`fcntl`
window is chapter 7's fork/exec race all over again. Note the layering in
Figure 19.2: both slots end up cloexec, but each got the bit *its own way*,
because close-on-exec is per-slot, never shared. Second, ownership: the
C++ code moves the raw int into the RAII `Fd` immediately, and Rust wraps
it in `OwnedFd::from_raw_fd` behind the crate's only `unsafe` block — the
comment is the ownership proof that makes it sound; Go documents intent
with `defer unix.Close(fd)` at the call site. Third, all three iterate the
control messages rather than assuming the first one is the rights message —
and all three return the byte-identical diagnostic
`no SCM_RIGHTS control message in reply` when a plain reply arrives, which
is what a `status` response would look like to `recvLogFd`. From there
`log_tail_via_fd` is deliberately mundane: `pread` from offset 0 in 4 KiB
chunks (1 MiB cap — a fragile bound, stated in the source), split lines,
print the last three prefixed `via-fd: `. Mundane is the point: the fd
works like any other, because it *is* any other fd.

## Errors, three ways

The contract is the same as every pmon version: exit 0, runtime failures
exit 1 with one diagnostic line, usage errors exit 2 — and `verify.lua`
asserts all three per language, 27 behavioral checks each. The
interesting split is *which* errors each side owns. `pmctl` treats a failed
`connect` as fatal (`pmctl: error: connect /tmp/…/absent.sock: No such file
or directory` — `std::error_code` text in C++, wrapped `%w` chain in Go,
`anyhow` context in Rust; same errno underneath). The supervisor, by
contrast, must survive *its* clients: an unknown command gets an
`err unknown command` reply line, `pmctl` unwraps the `err ` prefix and
exits 1, and the supervisor keeps serving. A peer that hangs up mid-reply
just ends that connection — `write_all` swallows the error deliberately,
because there is nobody left to report it to, and the loop `accept`s the
next caller. `EINTR` is retried at every blocking call in the C++
(`read_command`, `write_all`, `waitpid`), which the runtimes of Go and Rust
handle beneath their standard libraries. And the address-length failure is
caught before the syscall in all three, because a 108-byte truncation by
`strncpy` — the traditional C bug — would *bind to the wrong path* rather
than fail.

## Concurrency lens

Two threads of control share `State` in every implementation: the accept
loop and the reaper. The subtle race is `stop` versus respawn: `stop` sets
`stopping` and SIGTERMs the current group, but the reaper may already be
past its backoff and inside `spawn_child`. All three implementations
re-check `stopping` *after* the spawn returns and, if the flag flipped,
undo — SIGTERM and reap the child they just created — before signaling
`child_done`. Without that re-check, `stop` leaks exactly one freshly
respawned process, a bug that only bites when the child dies within 300 ms
of a stop. The languages phrase the wait differently — C++ and Rust block
on a condvar predicate, Go closes a `stopCh` the reaper `select`s against
the backoff timer — but the state machine is identical, which is what makes
the single `verify.lua` possible. Two quieter points: connections are
handled serially by design (one accepted fd at a time, so state locking
stays trivial and `SO_PEERCRED` lines interleave with nothing), and the log
file is shared by three writers — supervisor and each child generation via
`dup2` — safely, because every line is a single `write` on an `O_APPEND`
description, which the kernel makes an atomic append. One Go footnote:
`listener.SetUnlinkOnClose(false)` keeps Go's helpfulness from unlinking
the socket before the stop path does it explicitly, and
`int(logFile.Fd())` in the `logfd` case is again a loan against the
`*os.File` staying live, exactly as dissected in chapter 7.

## Build, run, observe

```bash
[host]$ cd examples/19-unix-sockets-fd-passing && ./demo.sh build
```

The runner (`python3 scripts/test-all-examples.py --only
19-unix-sockets-fd-passing`) reports `PASS PASS PASS` for cpp, go, rust —
27 behavioral checks per language. Driving the C++ build by hand the way
this chapter's evidence was produced (`$WD` is a short `mktemp -d`
directory — remember `sun_path`):

```bash
[host]$ ./cpp/build/release/app supervise --ctl $WD/ctl.sock --log $WD/pmon.log -- \
        /bin/sh -c 'echo alpha; echo beta; echo gamma; exec sleep 300' &
pmon: listening on /tmp/p19.t9j0/ctl.sock
pmon: started child pid=2622905
[host]$ ./cpp/build/release/app pmctl --ctl $WD/ctl.sock status
child=2622905 uptime=20s restarts=0
[host]$ ./cpp/build/release/app pmctl --ctl $WD/ctl.sock logfd
via-fd: alpha
via-fd: beta
via-fd: gamma
[host]$ kill 2622905 && sleep 1 && ./cpp/build/release/app pmctl --ctl $WD/ctl.sock status
child=2624872 uptime=0s restarts=1
[host]$ ./cpp/build/release/app pmctl --ctl $WD/ctl.sock stop
stopping
```

Each `pmctl` call also produced a `pmon: ctl connect uid=1000 pid=…` line on
the supervisor's stderr; the kill produced `pmon: child pid=2622905 exited
status=143` and `pmon: restart 1 child pid=2624872`; and after `stop` the
socket file was gone (`test -S` fails) and the child group with it. The
`via-fd:` lines matched `tail -3 $WD/pmon.log` byte for byte — but output
equality could still be a program quietly opening the path, so the
cross-checks watch the mechanism itself.

## Cross-check, three ways

**The socket pair and its owners, with `ss -xp`.** During a control call
held open (below), both endpoints of the stream show up, each naming its
owning process and cross-referencing the other's inode:

```console
[host]$ ss -xp | grep -E 'ctl.sock|"app"'
u_str ESTAB 0 0  /tmp/p19.t9j0/ctl.sock 16411996  * 16422777 users:(("app",pid=2622904,fd=5))
u_str ESTAB 0 0                       * 16422777  * 16411996 users:(("app",pid=2624432,fd=3))
```

Supervisor pid 2622904 holds fd 5 (the accepted socket — Figure 19.1),
client pid 2624432 holds fd 3, and the peer columns pair the two inodes.
This is the same accounting the supervisor's `SO_PEERCRED` line reported
from the other direction: `pmon: ctl connect uid=1000 pid=2624432`.

**The cmsg itself, under `strace`.** Tracing `sendmsg` on the supervisor
and `recvmsg` on `pmctl` during a `logfd` call, `strace` decodes the
control message on both sides:

```console
2622904 sendmsg(5, {…, msg_iov=[{iov_base="ok\n", iov_len=3}],
        msg_control=[{cmsg_len=20, cmsg_level=SOL_SOCKET, cmsg_type=SCM_RIGHTS,
        cmsg_data=[3]}], msg_controllen=24, …}, 0) = 3
recvmsg(3, {…, msg_control=[{cmsg_len=20, cmsg_level=SOL_SOCKET,
        cmsg_type=SCM_RIGHTS, cmsg_data=[4]}], msg_controllen=24,
        msg_flags=MSG_CMSG_CLOEXEC}, MSG_CMSG_CLOEXEC) = 3
pread64(4, "pmon: start child pid=2622905\nal"..., 4096, 0) = 47
pread64(4, "", 4096, 47) = 0
```

(Fields trimmed; the full lines are in the trace files.) The sender named
fd **3**; the receiver got fd **4** — the number is per-process, so it did
not travel. `cmsg_len=20` and `msg_controllen=24` are `CMSG_LEN(4)` and
`CMSG_SPACE(4)` from `send_fd`, live. And the very next syscalls show
`pmctl` reading all 47 bytes of log *through fd 4* at explicit offsets —
no `openat` anywhere in the trace.

**The shared description, in `/proc` — both processes at once.** `pmctl`
normally exits in milliseconds, so to catch both sides holding the fd, the
run above injected a 15-second delay after `recvmsg`
(`strace -e inject=recvmsg:delay_exit=15000000`). During the window, with
supervisor pid 2622904 and `pmctl` pid 2623753:

```console
[host]$ ls -l /proc/2622904/fd | grep pmon.log; ls -l /proc/2623753/fd | grep pmon.log
lrwx------ 1 rsedor rsedor 64 Jul 18 23:57 3 -> /tmp/p19.t9j0/pmon.log
lrwx------ 1 rsedor rsedor 64 Jul 18 23:58 4 -> /tmp/p19.t9j0/pmon.log
[host]$ cat /proc/2622904/fdinfo/3 /proc/2623753/fdinfo/4
pos:	47
flags:	02102002
mnt_id:	59
ino:	95934
pos:	47
flags:	02102002
mnt_id:	59
ino:	95934
[host]$ stat --format='%i %s %n' /tmp/p19.t9j0/pmon.log
95934 47 /tmp/p19.t9j0/pmon.log
```

Field for field equal, and each field pins a layer of the chapter 7 model.
`ino: 95934` on both fds matches `stat` on the path — one inode. `pos: 47`
on both — one *offset*, which only makes sense if there is one open file
description; 47 is the file's size because every `O_APPEND` write advanced
the shared position, and `pread` never moved it. `flags: 02102002` on both —
one set of status flags, decoding as `O_RDWR` (02) + `O_APPEND` (02000) +
`O_LARGEFILE` (0100000) + `O_CLOEXEC` (02000000): the description's flags
crossed intact, while the cloexec bit — per-slot, remember — reads set on
both sides only because each side set its own, one at `open` time, one via
`MSG_CMSG_CLOEXEC`. Two fd tables, one description, one inode: each claim
in Figure 19.2 observed on a live pair of processes.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet-wide view of this chapter is tracing fd passing across *all*
> processes with bcc-tools (`sofdsnoop`) and bpftrace probes on
> `unix_stream_sendmsg`, instead of per-process `strace`; bcc-tools are not
> runnable on this host without privileges, and the Debugging part (Part 8)
> exercises exactly that on the `systems-target` VM.

## What you learned

- `AF_UNIX` gives you three conversation shapes (stream, datagram,
  seqpacket) and two address namespaces — a filesystem inode governed by
  directory permissions and unlink, or an abstract NUL-prefixed name that
  `ss -x` renders with `@` and the kernel garbage-collects; `socketpair`
  skips addressing entirely for related processes.
- `SO_PEERCRED` is local auth with the kernel as witness: the supervisor
  logged `pmon: ctl connect uid=1000 pid=…` for every connection without
  the client claiming anything.
- `SCM_RIGHTS` transfers a *reference to the open file description*, not a
  number: sender fd 3 arrived as receiver fd 4, and `/proc` fdinfo on both
  processes simultaneously showed one `pos`, one `flags`, one `ino` —
  which is why the log must be `O_RDWR`, why `pmctl` uses `pread`, and why
  `MSG_CMSG_CLOEXEC` exists.
- The cmsg API is exact bookkeeping: `CMSG_SPACE` sizes the buffer,
  `CMSG_LEN` fills the header, at least one real payload byte rides along —
  and `strace` will show you all of it, decoded, when you get it wrong.

Next, **shared memory coordination**: `shmkv` stops copying bytes through
the kernel entirely — one mapped file, a seqlock for consistent reads, and
a futex so readers learn about writes without polling.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host this
session: the runner printed <code>19-unix-sockets-fd-passing  PASS  PASS
PASS</code> (3 passed, 0 failed, 0 skipped; verify.lua 27/27 per language);
the hand-driven session produced the exact <code>status</code>,
<code>via-fd:</code>, restart, and <code>stopping</code> lines shown, with
the socket file confirmed absent after stop; <code>ss -xl</code> showed the
filesystem and abstract (<code>@pmon-demo</code>) sockets as printed;
<code>ss -xp</code> showed the established pair with owning pids
2622904/2624432 and cross-referenced inodes; the strace excerpts are real
trimmed output with <code>cmsg_data=[3]</code> sent and
<code>cmsg_data=[4]</code> received; and the delay-injection window yielded
the simultaneous <code>/proc/&lt;pid&gt;/fd</code> and <code>fdinfo</code>
captures (pos 47, flags 02102002, ino 95934 on both sides, matching
<code>stat</code>). The "On the lab VM" bcc-tools callout is unverified as
marked and is exercised in Part 8.</p>
