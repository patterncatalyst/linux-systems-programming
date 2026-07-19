---
title: "TCP sockets from first principles"
order: 21
part: "Networking"
description: "chatterd v0 opens Part 6 with a thread-per-connection TCP chat room: socket/bind/listen/accept4 and sockaddr_in by hand, SO_REUSEADDR and why a fresh serve needs it, TCP as a boundaryless byte stream that forces the length-prefix frame reused for the rest of the book, and the RAII Socket that owns every fd — cross-checked with ss, strace, and the wire bytes themselves."
duration: "50 minutes"
---

Part 5 spent nine chapters moving bytes between processes that already knew
each other — pipes an ancestor set up, UNIX sockets a stranger found by a
path on the local machine. `chatterd` opens Part 6 by dropping the last
assumption: the peer is on another machine, reachable only by an address and
a port, over a transport that can lose, reorder, and re-split everything you
send. This first cut is deliberately small — a thread-per-connection TCP
**chat room**, one binary that is both the daemon (`serve`) and the client
(`chatctl`) — so that every primitive underneath a network server is visible
with nothing hidden: `socket`, `bind`, `listen`, `accept4`, a `sockaddr_in`
filled in by hand, and the one idea that TCP forces on every protocol built
on it — that a byte stream has no message boundaries, so *you* must add them.

The code is in `examples/21-tcp-sockets/`. `./demo.sh` there builds all three
implementations; its `README.md` fixes the CLI, the exit codes, and the wire
frame that chapters 22–24 and 27 grow but never replace.

{% include excalidraw.html
   file="21-server-lifecycle"
   alt="A top band labeled serve — one listener thread holds five boxes left to right: socket() AF_INET SOCK_STREAM|CLOEXEC, setsockopt SO_REUSEADDR = 1 rebind past TIME_WAIT, bind() sockaddr_in 127.0.0.1:9000, listen(128) passive socket fd 3, and an amber accept4() loop SOCK_CLOEXEC producing new fd 4, 5. Two amber spawn-thread arrows fall from the accept loop into a lower band, one std::jthread or goroutine or std::thread per accepted connection, holding serve_client fd 4 (bob) and serve_client fd 5 (alice). An amber note reads SIGINT arrow g_stop = 1; poll(2) wakes arrow stop accepting; listener fd closes; threads join; exit 0."
   caption="Figure 21.1 — chatterd serve's lifecycle: the four setup calls, the accept4 loop, and one thread per accepted fd; every fd number here reappears in the ss and strace captures later" %}

> **Tools used** — `ss`, `strace`, `python3`, `mktemp`, `grep` (host);
> `tcpdump`/`perf` (host, where privileged), and bcc-tools
> (`tcplife`/`tcpconnect`/`offcputime`) on the lab VM, exercised in Part 8.
> Everything host-side is checked by `scripts/check-host.sh` or ships with
> Fedora.

## Four calls to a listening socket

A server socket is built by four syscalls in a fixed order, and `listen_tcp`
is the canonical spelling of all four, verbatim from
`examples/21-tcp-sockets/cpp/src/main.cpp`:

```cpp
Result<Socket> listen_tcp(const std::string& host, std::uint16_t port) {
    Socket sock{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!sock.valid()) {
        return std::unexpected(std::format("socket: {}", errno_text()));
    }
    int one = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(std::format("{}: not an IPv4 address", host));
    }
    if (::bind(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::unexpected(std::format("bind {}:{}: {}", host, port, errno_text()));
    }
    if (::listen(sock.get(), 128) < 0) {
        return std::unexpected(std::format("listen: {}", errno_text()));
    }
    return sock;
}
```

`socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)` asks for an IPv4
(`AF_INET`) reliable byte stream (`SOCK_STREAM` — TCP), and folds close-on-exec
into the creating call the same way chapter 7 insisted every fd source must:
an `accept4` a moment later has the same discipline, because a listener that
leaks into a forked child is a port you can never rebind. The returned fd is
not yet an address — `bind` gives it one. `sockaddr_in` is the IPv4 address
struct, and its two fields carry a trap worth stating once: `sin_port` and
`sin_addr` are **network byte order** (big-endian), which is why the port
goes through `htons` (host-to-network-short) and the dotted-quad string goes
through `inet_pton` (presentation-to-network) rather than being assigned
raw. Skip `htons` on a little-endian x86 box and you bind port 9000 as
0x2823 — 10275 — and spend an afternoon wondering why nobody can connect.
`inet_pton` here also does double duty as validation: it returns 1 only for a
real IPv4 literal, so a hostname or garbage gets the `not an IPv4 address`
error instead of a mysterious `bind` failure. Turning a *name* like
`chat.example.com` into a `sockaddr` is a different job — `getaddrinfo(3)`,
which also picks IPv4-vs-IPv6 for you; this example stays numeric on purpose
and leaves resolution to chapter 23.

`bind` attaches the address; `listen(fd, 128)` flips the socket from active
to **passive** — it will never `connect` out, only accept in — and sizes the
*accept backlog*: the queue of connections the kernel completes the handshake
for while your code is between `accept` calls. 128 is the number of clients
that can be mid-connect before the kernel starts refusing; for a chat room it
is comfortably more than enough.

### SO_REUSEADDR, and the TIME_WAIT you will hit in ten minutes

The one line that looks optional and is not: `setsockopt(..., SO_REUSEADDR,
...)`. When a TCP connection closes, the end that closed *actively* parks the
socket in `TIME_WAIT` for a couple of minutes, holding the `(local addr,
local port, remote addr, remote port)` tuple so a late straggler packet from
the old connection cannot be mistaken for the new one. For a server that
means: kill `serve`, restart it within that window, and `bind` fails with
`EADDRINUSE` — "Address already in use" — because a lingering client
connection still pins the port. `SO_REUSEADDR` tells the kernel that a
`bind` to a port in `TIME_WAIT` is fine, and it is the reason you can Ctrl-C
this server and relaunch it immediately. Go sets it for you on every TCP
listener (which is why the Go `cmdServe` is a bare `net.Listen`); the Rust
build sets it explicitly through `nix` (`setsockopt(&fd, ReuseAddr, &true)`)
before `bind`, exactly as the C++ does.

## TCP is a byte stream, so we frame it ourselves

Here is the idea the rest of the book is built on. A UNIX pipe and a TCP
connection are both `SOCK_STREAM`: a reliable, ordered sequence of bytes with
**no message boundaries**. If a client sends `build_join("alice")` and then
`build_msg("hi")` back to back, the server's next `read` may return all of
the first frame and half the second, or two-and-a-half frames, or one byte.
TCP guarantees the *bytes* arrive in order and intact; it guarantees nothing
about how they are grouped into `read` calls. A protocol that assumed "one
`read` == one message" would work on loopback in testing and shatter under a
real network the first time a segment split.

The fix is **length-prefix framing**, wrapped in a small fixed header, and it
is fixed for the whole book: chapters 22, 24, and 27 add message *types* to
this exact 6-byte header, never a new one. Every message, both directions,
opens with a 2-byte magic `0x43 0x48` (ASCII `"CH"`), a 1-byte version
(`0x01`), a 1-byte `type`, then a 2-byte **big-endian** payload length
(`0..65535`), and finally exactly that many payload bytes.

{% include excalidraw.html
   file="21-frame-format"
   alt="A row of 21 byte cells for the server's DELIVER broadcast of alice's message to bob. The first six cells, amber, hold 43 48 01 03 00 0f labeled magic CH, version 0x01, type 0x03 DELIVER, length (u16BE) = 15. The next five cells hold 61 6c 69 63 65 labeled name = alice. One dark cell holds 00 labeled NUL. Nine cells hold 68 65 6c 6c 6f 20 62 6f 62 labeled text = hello bob. Below: 00 0f = 0x000f = 15 = len(alice) + 1 (NUL) + len(hello bob). Then the frames that got here: alice's own MSG frame (type 0x02) carried just hello bob as its payload, no name; JOIN (type 0x01) payload is name alone, sent once at connect. A footnote notes length is u16BE, 0..65535, counts payload bytes only, and the 6 header bytes are never counted; WELCOME (0x04) and PING (0x05) are declared and reserved but neither sent nor expected in this chapter."
   caption="Figure 21.2 — the wire frame byte for byte: the DELIVER broadcast of alice's message to bob; 43 48 01 03 00 0f is the exact magic+version+type+length header captured off the socket in the cross-check below" %}

The `type` byte is what makes the NUL separator mostly disappear: a **JOIN**
frame's payload is just the sender's `name`; a **MSG** frame's payload is the
chat `text` alone, nothing else; the server relays every MSG as a **DELIVER**
frame whose payload is `name` `0x00` `text` — the *only* frame that still
needs a NUL, because it is the only one carrying two fields to a reader who
never sent them itself. `WELCOME` and `PING` are declared and reserved for
chapters 22 and 24 but never sent or read here. The length prefix means the
reader never scans for a delimiter — it reads exactly the 6 header bytes,
learns the type and size from them, reads exactly that many payload bytes,
and is done. There is no separate "reject an oversized frame" check the way
an unbounded length would need: `length` is 16 bits, so 65535 bytes is the
largest payload the wire can even express, and that ceiling is a property of
the field, not a runtime guard the reader has to remember to add.

## How the code works

The reader is the heart of the program, and where the length-prefix
discipline lives. Here it is in all three languages, each verbatim from
`examples/21-tcp-sockets/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
std::optional<RawFrame> read_frame(int fd) {
    std::uint8_t hdr[kHeaderSize];
    if (!read_full(fd, hdr, kHeaderSize)) {
        return std::nullopt;
    }
    if (hdr[0] != kMagic0 || hdr[1] != kMagic1 || hdr[2] != kWireVersion) {
        return std::nullopt;  // wrong magic/version: not a chatterd frame
    }
    std::uint8_t type = hdr[3];
    std::uint16_t len = (static_cast<std::uint16_t>(hdr[4]) << 8) | hdr[5];
    std::string payload(len, '\0');
    if (len > 0 && !read_full(fd, payload.data(), len)) {
        return std::nullopt;
    }
    return RawFrame{type, std::move(payload)};
}
```

```go
// readFrame returns the frame's type and payload, or an error; io.EOF marks a
// clean close.
func readFrame(r *bufio.Reader) (byte, []byte, error) {
	var hdr [headerSize]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, nil, err
	}
	if hdr[0] != magic0 || hdr[1] != magic1 || hdr[2] != version {
		return 0, nil, fmt.Errorf("not a chatterd frame (bad magic/version)")
	}
	typ := hdr[3]
	n := binary.BigEndian.Uint16(hdr[4:6])
	payload := make([]byte, n)
	if n > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return 0, nil, err
		}
	}
	return typ, payload, nil
}
```

```rust
/// Read one frame; `Ok(None)` marks a clean EOF, `Err` a protocol fault.
fn read_frame<R: Read>(r: &mut R) -> Result<Option<(u8, Vec<u8>)>> {
    let mut hdr = [0u8; HEADER_SIZE];
    match r.read_exact(&mut hdr) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e.into()),
    }
    if hdr[0..2] != MAGIC || hdr[2] != WIRE_VERSION {
        bail!("not a chatterd frame (bad magic/version)");
    }
    let frame_type = hdr[3];
    let len = u16::from_be_bytes([hdr[4], hdr[5]]);
    let mut payload = vec![0u8; len as usize];
    if len > 0 {
        match r.read_exact(&mut payload) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
            Err(e) => return Err(e.into()),
        }
    }
    Ok(Some((frame_type, payload)))
}
```

The three are the same state machine in three vocabularies. The load-bearing
call is still *read-exactly-n*: C++ hand-rolls it in `read_full`, a loop that
keeps calling `read` until `n` bytes have accumulated, retrying `EINTR` and
returning false on EOF; Go and Rust get it from the standard library
(`io.ReadFull`, `read_exact`). This loop is the whole point — a single
`read` returning fewer bytes than asked is not an error, it is Tuesday, and
the reader that treats a short read as a complete message is the bug the
length prefix exists to prevent. The 6-byte header is read whole, then
checked before anything in it is trusted: `hdr[0]`/`hdr[1]` must be `'C'`/`'H'`
and `hdr[2]` must be the version this build speaks (`0x01`), or the frame is
rejected outright — a stray non-chatterd byte stream, or a future version this
build doesn't understand, is refused before its length field is ever read as
a byte count. Only then are `type` (`hdr[3]`) and the big-endian 16-bit
`length` (`hdr[4..6]`, decoded with explicit shifts in C++,
`binary.BigEndian` in Go, `u16::from_be_bytes` in Rust) trusted, and the
payload allocated to exactly `length` bytes and read in full — capped at
65535 simply because the field is 16 bits, with no separate size check to
remember. None of the three distinguishes a bad magic/version from a short
read with a diagnostic in this chapter: a truncated header, a wrong magic,
and a truncated payload all end the connection the same quiet way a clean EOF
does — `read_frame` returns `std::nullopt`/an `error`/(mostly) `Ok(None)`, and
the caller simply stops reading that connection. Rust's return type is the
one place the distinction survives structurally (`Ok(None)` is EOF,
`Err(...)` is a bad magic/version via `bail!`), but `serve_client`'s
`_ => break` arm folds both back into the same outcome anyway.

Around the reader sits the accept loop of Figure 21.1. `cmd_serve` installs a
SIGINT handler, then loops: `poll` the listener with a 200 ms timeout (so a
SIGINT that flips `g_stop` is noticed even between connections),
`accept4(..., SOCK_CLOEXEC)` a ready connection into a fresh `Socket`, bump an
id, and hand the connection to a `std::jthread` running `serve_client`. Each
connection thread reads frames forever; a JOIN logs `chatterd: <name> joined`
and broadcasts a `server\0<name> joined` DELIVER notice to **all** members,
while a MSG is relayed as a DELIVER carrying `<name>\0<text>` to every client
**except** the sender — you already see what you typed. A `Hub` (a
`std::vector<Client>` under a `std::mutex`, a `map[int]*client` in Go, a
`Mutex<HashMap>` in Rust) is the shared roster every thread reads and writes.

The RAII `Socket` is the quiet hero. It is a move-only handle that `close`s
its fd in the destructor and nowhere else — there is not a single naked
`close` in the C++ program. When `serve_client` returns, its `Socket`
parameter dies and the connection closes; when `cmd_serve` returns, the
listener `Socket` dies last and the port frees. Ownership *is* lifetime, so
"who closes this fd, and when" is answered by scope rather than by vigilance
— the same lesson chapter 7 drew for files, now load-bearing for a server
that opens and closes a socket per client.

## Errors, three ways

The contract is the book's usual triple: exit 0 clean, exit 1 for a runtime
failure with one diagnostic line, exit 2 for a usage error — and one
`verify.lua` asserts all three across the languages (19 behavioral checks
each). The instructive case is a client against a dead port. All three report
it and exit 1, and the errno underneath is identical (`ECONNREFUSED`) even as
each language's error type dresses it differently:

```console
[host]$ ./cpp/build/release/app  chatctl --port 9099 --name x < /dev/null
chatctl: error: connect 127.0.0.1:9099: Connection refused
[host]$ ./go/bin/app            chatctl --port 9099 --name x < /dev/null
chatctl: error: connect 127.0.0.1:9099: dial tcp 127.0.0.1:9099: connect: connection refused
[host]$ ./rust/target/release/app chatctl --port 9099 --name x < /dev/null
chatctl: error: connect 127.0.0.1:9099: Connection refused (os error 111)
```

C++ carries the failure in `std::expected<Socket, std::string>` and formats
the `std::error_code`; Go wraps with `%w` so the `dial tcp` context chains in;
Rust attaches `anyhow` context and prints the bare `os error 111`. A server-side
`bind` that loses (a privileged port, say) is the same shape from the other
side: `chatterd: error: bind 127.0.0.1:80: Permission denied`, exit 1. Usage
slips — no subcommand, `serve` without `--port`, `chatctl` without `--name` —
all short-circuit to the two-line usage text and exit 2 before any socket is
opened. Inside the connection threads, errors are handled by *ending that one
connection*, never the server: a peer that vanishes mid-write makes `write_all`
return quietly (there is nobody left to tell), and the accept loop takes the
next caller.

## Concurrency lens

`chatterd` v0 is **thread-per-connection**: one OS thread (or goroutine) blocked
in `read_frame` per client, plus the listener thread in `accept`. It is the
easiest server model to reason about — each connection is a straight-line
blocking program, and the only shared state is the `Hub` behind a mutex — and
it is exactly the model that stops scaling, which is why chapter 22 exists.
Two cracks are already visible. First, **cost per connection**: a thread is a
megabyte or two of stack plus a scheduler entry, so ten thousand idle chat
clients is gigabytes of RAM and a scheduler run queue that thrashes — the
classic C10K wall. Second, and more subtle, **head-of-line blocking in the
broadcast**: the C++ and Rust `broadcast` hold the `Hub` mutex while writing
to every client in turn, so one client whose kernel send buffer is full (a
slow reader) stalls `write` and freezes *every other* broadcast behind the
lock. Go sidesteps it structurally — each client owns a 64-deep buffered
channel and a writer goroutine, and `broadcast` does a non-blocking
`select { case c.out <- frame: case <-c.done: }`, so a wedged client is
skipped rather than allowed to block the room. That contrast — a mutex held
across I/O versus a bounded queue per consumer — is the seam chapter 22 opens
up when it moves the server onto one epoll loop and a thread pool. For now, the
three shutdown strategies all reach the same observable end: C++ interrupts
`poll` with SIGINT and lets each `std::jthread`'s `stop_callback` shut its
socket down; Go closes the listener out from under `Accept` via a cancelled
`context`; Rust flips an `AtomicBool` a non-blocking `accept` loop polls. All
three close the listener and exit 0.

## Build, run, observe

```bash
[host]$ cd examples/21-tcp-sockets && ./demo.sh build
```

`python3 scripts/test-all-examples.py --only 21-tcp-sockets` builds all three
and reports `PASS PASS PASS` (3 passed, 0 failed, 0 skipped; `verify.lua` 19/19
per language). To drive it by hand the way this chapter's evidence was
produced, start a room and connect two clients — bob first, so he witnesses
alice arrive:

```bash
[host]$ ./cpp/build/release/app serve --port 9001 --host 127.0.0.1 &
chatterd: listening on 127.0.0.1:9001
[host]$ ./cpp/build/release/app chatctl --port 9001 --name bob    # terminal 2
[host]$ ./cpp/build/release/app chatctl --port 9001 --name alice  # terminal 3
```

With both connected, alice typing `hello bob` on stdin lands only in bob's
terminal, never echoed back to her. The observed transcripts and server log
from the scripted run were exactly:

```
# alice's terminal            # bob's terminal              # server stderr
server: alice joined          server: bob joined            chatterd: listening on 127.0.0.1:9001
                              server: alice joined          chatterd: bob joined
                              alice: hello bob              chatterd: alice joined
                              server: alice left            chatterd: alice left
                                                            chatterd: bob left
                                                            chatterd: shutting down
```

bob sees his own join notice (joins go to everyone), then alice's join, then
her message, then her leave when she Ctrl-Ds out; alice never sees `alice:
hello bob` because the sender is excluded from chat broadcasts. A SIGINT to
`serve` printed `chatterd: shutting down` and the process exited 0.

## Cross-check: the listener, the connections, and the bytes on the wire

Three independent tools confirm the mechanism rather than just the output.
**First, `ss -tlnp` on the running listener** shows the passive socket, its
backlog, and its owner:

```console
[host]$ ss -tlnp | grep :9000
LISTEN 0  128  127.0.0.1:9000  0.0.0.0:*  users:(("app",pid=2816579,fd=3))
```

`LISTEN`, the `128` backlog we passed to `listen`, the bound `127.0.0.1:9000`,
and `fd=3` in process `app` — Figure 21.1's listener box, live. **Second,
`ss -tnp` during a two-client session** shows four established sockets — two
in the server, one in each client — and the peer columns pair them by the
ephemeral ports the kernel assigned the clients:

```console
[host]$ ss -tnp | grep :9001
ESTAB 0 0  127.0.0.1:9001   127.0.0.1:47504  users:(("app",pid=2818525,fd=4))
ESTAB 0 0  127.0.0.1:9001   127.0.0.1:47512  users:(("app",pid=2818525,fd=5))
ESTAB 0 0  127.0.0.1:47504  127.0.0.1:9001   users:(("app",pid=2818649,fd=3))
ESTAB 0 0  127.0.0.1:47512  127.0.0.1:9001   users:(("app",pid=2818655,fd=3))
```

Server pid 2818525 holds `fd 4` and `fd 5` — the two `accept4` results from
Figure 21.1 — each paired with a client's ephemeral port (47504 for bob,
47512 for alice), while each client holds its `fd 3` connecting side. **Third,
the frame bytes themselves.** `tcpdump -i lo` reads them off the wire, but the
capture socket needs `CAP_NET_RAW`; on this unprivileged host the identical
bytes are visible one layer down, at the `read`/`write` syscalls, under
`strace` — the exact sequence the socket handed to (and took from) TCP during
alice's `hello bob`:

```console
[host]$ strace -f -tt -e trace=accept4,read,write -s 80 ./cpp/build/release/app serve --port 9002 ...
accept4(3, NULL, NULL, SOCK_CLOEXEC) = 4
accept4(3, NULL, NULL, SOCK_CLOEXEC) = 5
read(5, "CH\1\1\0\5", 6)                     = 6
read(5, "alice", 5)                         = 5
read(5, "CH\1\2\0\t", 6)                     = 6
read(5, "hello bob", 9)                      = 9
write(4, "CH\1\3\0\17alice\0hello bob", 21)  = 21
```

Every claim in Figure 21.2 is on those lines (bob connected first onto `fd 4`;
his own JOIN read and the join-notice broadcasts are elided above for space —
they carry the same shape, just a different type and length). `accept4`
returns fds 4 and 5, each `SOCK_CLOEXEC` as the source asked. The reader
takes the fixed 6-byte header first — `read(5, "CH\1\1\0\5", 6)` is `43 48`
(`"CH"`), `01` (version), `01` (type = JOIN), `00 05` (length = 5,
big-endian) — then exactly that many payload bytes (`"alice"`), a strict
two-phase read that is the length-prefix discipline in the raw. alice's chat
line repeats the shape with a different type: `"CH\1\2\0\t"` is
`43 48 01 02 00 09` — type `02` = MSG, length 9 — followed by the 9-byte
payload `"hello bob"`, with no name in it at all; the connection already told
the server who she is. The server's relayed DELIVER, `write(4,
"CH\1\3\0\17alice\0hello bob", 21)`, is the whole broadcast in one write:
header `43 48 01 03 00 0f` (type `03` = DELIVER, length 15 = `len("alice")`
`+ 1 (NUL) + len("hello bob")`), then the 15-byte payload `alice\0hello bob`
— the NUL still separating name from text, because DELIVER is the only type
that carries both. That length prefix, octal `\17`, is `0f` in hex — 15 —
the same 6-byte magic/version/type/length header that would show up in a
`tcpdump` capture off `lo`, byte for byte the amber header of Figure 21.2.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet view of these connections is bcc-tools, not `strace`:
> `tcpconnect`/`tcplife` trace every TCP session on the box with its bytes and
> lifetime, and `offcputime` shows the connection threads parked in
> `read`/`accept` (which is where a thread-per-connection server spends nearly
> all of its time). Those tools need root and a disposable kernel; the
> Debugging part (Part 8) runs them against `chatterd` on the
> `systems-target` VM.

## What you learned

- A server socket is four ordered calls — `socket` → `bind` → `listen` →
  `accept4` — with `sockaddr_in` in network byte order (`htons`, `inet_pton`)
  and `SO_REUSEADDR` set before `bind` so a restart can rebind a port still
  held in `TIME_WAIT`; `ss -tlnp` showed the passive socket with backlog 128
  on `fd 3`, and `ss -tnp` the four established halves of a two-client
  session with their paired ephemeral ports.
- TCP is a boundaryless byte stream: a `read` can return part of a frame or
  several, so every message opens with a fixed 6-byte header — magic `CH`,
  version, type, then a big-endian `u16` length — and a reader that loops
  until it has exactly the header, then exactly the payload; `strace` caught
  that two-phase read and the `43 48 01 03 00 0f` (DELIVER, length 15) header
  on the wire.
- The RAII `Socket` owns every fd by scope, so there is no naked `close`; and
  thread-per-connection is the simple model that chapter 22 must leave behind
  — a thread and a stack per client, plus a broadcast that holds the roster
  lock across I/O and stalls behind one slow reader.

Next, **scaling the server**: chapter 22 takes the same `chatterd` frame off
threads and onto a single epoll loop with nonblocking sockets, so ten
thousand idle clients cost fds instead of stacks.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, strace 7.1) this session. The runner printed
<code>21-tcp-sockets  PASS  PASS  PASS</code> (3 passed, 0 failed, 0 skipped;
<code>verify.lua</code> 19/19 per language). The hand-driven two-client
session produced the exact transcripts shown — bob received
<code>alice: hello bob</code>, alice saw no echo of her own line, both clients
saw a <code>server</code>-sender join notice, the server logged each join and
<code>chatterd: shutting down</code>, and SIGINT exited 0. <code>ss -tlnp</code>
showed <code>LISTEN 0 128 127.0.0.1:9000 … fd=3</code>; <code>ss -tnp</code>
showed the four established sockets with server pid 2818525 holding fd 4/5 and
clients on ephemeral ports 47504/47512. The connect-refused, bind-denied, and
usage error strings and exit codes (1/1/2) are real output. The
<code>strace</code> excerpt is real trimmed output re-captured against the
canonical CH frame: <code>accept4 … = 4</code> and <code>= 5</code>, the
two-phase header-then-payload reads for alice's JOIN (<code>CH\1\1\0\5</code>
then <code>alice</code>) and MSG (<code>CH\1\2\0\t</code> then
<code>hello bob</code>), and the server's relayed DELIVER,
<code>write(4, "CH\1\3\0\17alice\0hello bob", 21)</code> — the
<code>\17</code> (<code>0f</code> in hex, 15) big-endian length half of the
6-byte magic/version/type/length header. <code>tcpdump</code> on
<code>lo</code> was not run (needs <code>CAP_NET_RAW</code>, unavailable in
this unprivileged session); the identical wire bytes were captured at the
syscall boundary instead. The bcc-tools "On the lab VM" callout is unverified
as marked and is exercised in Part 8.</p>
