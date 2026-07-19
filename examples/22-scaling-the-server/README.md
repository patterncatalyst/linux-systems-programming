# 22-scaling-the-server — chatterd v1

chatterd is a peer-to-peer chat daemon we grow across chapters 21–24 and 27.
ch21 introduced the length-prefixed frame protocol and a thread-per-connection
server. ch22 is about **how a server holds many connections at once**: it keeps
the threaded engine and adds a single-threaded **epoll** engine — one thread,
nonblocking accept, and a per-connection read/write state machine with an
outbound queue and real backpressure.

The daemon and its client are the same binary (`app`): `serve` is the daemon,
`send` / `listen` / `flood` are the client (`chatctl`) modes.

```
$ ./demo.sh cpp run serve --engine epoll --port 0 &
chatterd: serving engine=epoll on 127.0.0.1:41453

$ ./demo.sh cpp run listen --port 41453 bob --count 1 &
$ ./demo.sh cpp run send --port 41453 alice "hello, chatterd"
alice: hello, chatterd
# bob's terminal prints the same line:
alice: hello, chatterd

$ ./demo.sh cpp run flood --port 41453 50
flood: connected 50
flood: delivered 50

# SIGINT/SIGTERM the daemon:
chatterd: stopped engine=epoll messages=2 peak_conns=50
```

Every MSG is delivered to **every** connected client, including the sender, so
a client confirms its message was accepted by seeing its own line echoed.

## What the chapter is really about

Two ways to service N connections from one process:

- **`--engine threaded`** — one OS thread per connection blocks in `recv()`; a
  shared registry (mutex + per-socket write mutex) fans each message out. Simple
  to reason about, one kernel thread per client. The accept loop `poll(2)`s the
  listening socket alongside a `signalfd`, so a signal breaks it out cleanly.
- **`--engine epoll`** — one thread owns an `epoll` set over the listener, a
  `signalfd`, and every client socket. Accept is nonblocking (`accept4` in a
  loop until `EAGAIN`); each connection is a state machine with an **inbound
  buffer** (frames are reassembled across reads) and an **outbound queue**.
  Broadcasting appends the encoded frame to every connection's queue and tries
  to drain it; if the socket buffer is full the remainder stays queued and the
  connection re-arms `EPOLLOUT`, which drives the rest of the flush later. That
  is **backpressure**: a slow reader can't corrupt or drop another client's
  frames, and the writer never blocks the whole server.

`verify.lua` proves the epoll engine handles **50 concurrent connections** and
delivers one broadcast to **all 50** (`flood: delivered 50`, no dropped frames),
that the threaded engine does the same, and that both shut down cleanly with a
`messages`/`peak_conns` summary that matches what was driven through them.

### A note on Go

Go exposes the same `--engine threaded|epoll` flag, but **both** engines are
the same idiomatic code: one goroutine per connection reading, one writing, and
a hub goroutine fanning out over channels. There is no hand-rolled epoll — the
Go runtime's **network poller** already multiplexes every socket through
`epoll_wait(2)` and parks a *goroutine* (not an OS thread) on each blocked
Read/Write. The epoll the other two languages write by hand, Go's netpoller runs
underneath; the engine flag only changes the label. Observable behavior is
identical across all three.

## Wire format

One frame protocol, introduced in ch21 and reused unchanged by every later
chatterd version and every language. All multi-byte fields are **big-endian**.

```
 byte:  0     1     2       3      4     5     6 ............. 6+LEN-1
      +-----+-----+-------+------+-----+-----+---------------------------+
      | 'C' | 'H' | 0x01  | TYPE |   LENGTH   |         PAYLOAD          |
      +-----+-----+-------+------+-----+-----+---------------------------+
        magic     version         u16 (LEN)      LEN bytes

magic    = 0x43 0x48 ('C','H')
version  = 0x01
TYPE     = 1 byte (see below)
LENGTH   = uint16 big-endian, number of PAYLOAD bytes (0..65535)
PAYLOAD  = LENGTH bytes
```

Header is always 6 bytes; total frame length is `6 + LENGTH`.

| TYPE | name    | dir | payload |
|------|---------|-----|---------|
| 0x01 | JOIN    | client → server | nickname, UTF-8 |
| 0x02 | MSG     | client → server | message text, UTF-8 |
| 0x03 | DELIVER | server → client | `nick + 0x00 (NUL) + text` |
| 0x04 | WELCOME | server → client | empty (sent once, on accept) |
| 0x05 | PING    | — | reserved (introduced in ch24; unused here) |

This is the same frame every chatterd chapter and language speaks: a v0
(ch21) client can chat through a v1 (this chapter's) or v3/v4 server and back
because JOIN/MSG/DELIVER never change shape — later chapters only add types
(WELCOME here, PING in ch24), never touch the 6-byte header or repurpose an
existing type.

`DELIVER` carries the originating nickname, then a single NUL (`0x00`) byte,
then the message text — split the payload on its first NUL to recover the two
fields (a nickname is never allowed to contain a NUL). WELCOME is sent the
instant the server accepts a connection — receiving it is proof the connection
is registered in the server's fan-out set, which is how `flood` knows all N
peers are present before it triggers the broadcast.

Example — `alice` sends `hi` (`msg`), and the server delivers it:

```
client -> server   43 48 01 01 00 05  61 6c 69 63 65               JOIN  "alice"
client -> server   43 48 01 02 00 02  68 69                        MSG   "hi"
server -> client   43 48 01 03 00 08  61 6c 69 63 65 00 68 69      DELIVER nick="alice" text="hi"
```

## CLI

Identical across the three languages; exit codes 0 ok / 1 runtime / 2 usage.

```
serve  --engine threaded|epoll --port P    run the daemon (P=0 picks an ephemeral port,
                                           printed as "on 127.0.0.1:<port>")
send   --port P NICK TEXT                  join as NICK, broadcast TEXT, print each
                                           delivered line as "nick: text", stop after
                                           seeing your own echo
listen --port P NICK --count N             join as NICK, print N delivered lines, exit
flood  --port P N [--text TEXT]            open N concurrent connections, wait for all to
                                           be welcomed, have one broadcast TEXT, and report
                                           "flood: connected <n>" / "flood: delivered <n>"
```

The daemon logs two lines to stderr: `chatterd: serving engine=<e> on
127.0.0.1:<port>` at startup and `chatterd: stopped engine=<e> messages=<m>
peak_conns=<p>` on SIGINT/SIGTERM, where `messages` is the number of MSG frames
broadcast and `peak_conns` the high-water mark of simultaneous connections.

## The three implementations

Same CLI, stdout/stderr shapes, exit codes, and wire protocol, so one
`verify.lua` covers all three.

| | epoll engine | threaded engine | shutdown |
|---|---|---|---|
| **C++23** | hand-rolled `epoll_create1`/`epoll_ctl`/`epoll_wait`; per-conn `EConn` with `inbuf`/`outbuf`, `EPOLLOUT` toggled on backpressure; RAII `Fd` owns every socket, `std::expected` for setup | `std::jthread` per connection, shared `Registry` (mutex + per-socket write mutex), `std::atomic` counters | `signalfd` in the loop / poll set; `SHUT_RDWR` wakes reader threads, jthreads join |
| **Go 1.26** | netpoller (see note) — goroutine per conn + hub goroutine, channel fan-out with blocking sends for backpressure | *same code* as epoll; the flag only changes the printed label | `signal.Notify`, close listener, `quit`/`done` channels, atomics read after the hub drains |
| **Rust 2024** | `mio` `Poll`/`Token`/`Interest`; per-conn `EConn` with `inbuf`/`outbuf`, reregister `READABLE\|WRITABLE` on backpressure; `signalfd` registered via `SourceFd`; `OwnedFd`-backed `mio::net` sockets | `std::thread` per connection, `Arc<ConnRegistry>` (mutex map + per-socket `Mutex<TcpStream>` write clone), atomics | `nix::poll` over listener + `signalfd`; `TcpStream::shutdown` wakes readers, threads join |

Build/run: `./demo.sh [cpp|go|rust] build` and `./demo.sh <lang> run <args>`.
`./demo.sh <lang>` with no args runs a short end-to-end demo (epoll daemon, a
two-party exchange, a 50-client flood, clean shutdown). Everything runs on
loopback; no VM is involved.
