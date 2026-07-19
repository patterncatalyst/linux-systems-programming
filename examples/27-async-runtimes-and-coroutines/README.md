# 27 — async runtimes and coroutines (chatterd v4)

`chatterd` is the book's recurring peer-to-peer chat daemon. This chapter is the
**async rewrite**: the *same* server, with its connection handling rebuilt on
each language's async model. The observable behaviour is unchanged — a message
from one client is broadcast to every other connected client — so a single
`verify.lua` covers all three languages.

| language | `--engine async` is…                                             |
|----------|------------------------------------------------------------------|
| **C++**  | hand-rolled **C++20 coroutines** over an epoll reactor: each connection is a coroutine that `co_await`s socket readability. The `task`/awaitable machinery lives in `cpp/src/main.cpp`; no external async library is linked. |
| **Rust** | **tokio**: an `async fn` per connection on a `tokio::net::TcpListener`, spawned as tasks, fanning out over per-client `mpsc` channels. |
| **Go**   | **unchanged from v1** — goroutines already *are* the async model. `--engine async` selects the same goroutine-per-connection hub. That is the lesson: Go has no separate coroutine layer to add. |

Every version also keeps the prior **`--engine thread`** engine working
(one OS thread per client), so you can compare the two side by side and confirm
they are byte-for-byte identical on the wire.

## The wire format (canonical chatterd chat frame)

This is the **same frame every chatterd chapter uses** — introduced in ch21
and extended only additively (by adding new `type` values) ever since; the
header never changes shape. Every message on a connection, in both
directions, is a single frame:

```
 byte:   0    1    2    3    4    5    6                      6+L-1
       +----+----+----+----+----+----+=========================+
       |magic(CH)|ver |type|len (u16)|   payload (L bytes)     |
       |0x43 0x48|0x01|    |big-end. |   UTF-8, type-specific   |
       +----+----+----+----+----+----+=========================+
```

* `magic` — 2 bytes, always `0x43 0x48` (ASCII `"CH"`).
* `version` — 1 byte, `0x01`.
* `type` — 1 byte: `JOIN=1`, `MSG=2`, `DELIVER=3`, `WELCOME=4`, `PING=5`.
* `length` (`L`) — a 16-bit **big-endian** unsigned integer: the number of
  bytes in `payload` (0‥65535).
* `payload` — `L` bytes, UTF-8, type-specific (see below).

This chapter's engines (thread and async, in all three languages) use only
three of the five types — the same three ch21 introduced:

* **JOIN** — the **first** frame a client sends after connecting; payload is
  the client's nick (e.g. `alice`).
* **MSG** — every **later** frame the client sends; payload is message text.
* **DELIVER** — for a MSG with payload `T` from the client whose nick is `N`,
  the server sends every **other** connected client a DELIVER frame whose
  payload is exactly `N + 0x00 (NUL) + T`. The sender never receives its own
  message.

`WELCOME` and `PING` are other chapters' additions to this same frame (ch22's
welcome/roster message, ch24's heartbeat) — this chapter never sends them, and
because the header is unchanged, a ch21-era client that doesn't know those
types still exchanges JOIN/MSG/DELIVER correctly with this chapter's server.

### Byte-exact examples

```
JOIN    "alice"              43 48 01 01 00 05 61 6c 69 63 65
MSG     "hi"                 43 48 01 02 00 02 68 69
DELIVER "alice\0hi"          43 48 01 03 00 08 61 6c 69 63 65 00 68 69
```

That last line is a real capture: with `alice` and `bob` both connected,
`alice` sending `hi` puts these fourteen bytes on `bob`'s socket — the 6-byte
header (`43 48` magic, `01` version, `03` type DELIVER, `00 08` length) then
the 8-byte payload `alice\0hi` (nick, NUL, text). The framing and the
`N NUL T` payload are identical across C++, Go, and Rust, and across the
`thread` and `async` engines.

## CLI

```
app serve    [--engine thread|async] [--host H] [--port P]   # run the daemon
app loadtest [--host H] [--port P] [--clients N]             # drive a broadcast
```

Defaults: `--host 127.0.0.1`, `--port 47100`, `--engine async`, `--clients 20`.

* `serve` prints `chatterd: listening on H:P engine=E` on stderr once bound,
  then serves until it receives `SIGINT` or `SIGTERM`, at which point it logs
  `chatterd: shutdown` and exits `0`.
* An unknown engine prints `chatterd: unknown engine '…' (want thread|async)`
  and exits `2`; a missing/unknown subcommand prints usage and exits `2`.
* `loadtest` opens `N` receiver connections plus one sender, has the sender
  broadcast `hello world`, and checks every receiver got `sender: hello world`.
  It prints `loadtest: delivered K/N` and exits `0` iff `K == N`.

## Demo contract

Standard across the book:

```
./demo.sh              # build + run all three
./demo.sh cpp run serve --engine async --port 47100
./demo.sh go  run loadtest --port 47100 --clients 20
./demo.sh build        # build only
```

With `TARGET` set, `run` deploys the binary to that lab VM. This example runs
entirely on loopback; it never touches a VM.

Try it by hand:

```
./demo.sh cpp run serve --engine async --port 47100 &   # start the daemon
./demo.sh cpp run loadtest --port 47100 --clients 20    # -> loadtest: delivered 20/20
kill -TERM %1                                            # -> chatterd: shutdown
```

## Verification

`verify.lua` (driven by `scripts/lib/checks.lua`) starts the daemon in the
background, waits for the port, runs the 20-client `loadtest`, and asserts the
observable behaviour for **both** engines:

* `chatterd: listening on 127.0.0.1:PORT engine=<engine>` announced,
* `loadtest: delivered 20/20` — the broadcast reached all 20 clients,
* the loadtest exits `0`,
* `SIGTERM` produces `chatterd: shutdown` and a `0` exit — a clean shutdown,

plus the CLI contract (missing subcommand → exit 2, unknown engine → exit 2).
The same assertions pass identically for C++, Go, and Rust.

## Layout

```
27-async-runtimes-and-coroutines/
├── demo.sh            # dispatcher
├── verify.lua         # background-daemon broadcast + shutdown checks
├── cpp/src/main.cpp   # thread engine + hand-rolled coroutine (async) engine
├── go/main.go         # goroutine hub (async == thread here)
└── rust/src/main.rs   # std thread engine + tokio (async) engine
```

## Notes on the C++ coroutine engine

`cpp/src/main.cpp` implements the minimum coroutine runtime the async engine
needs, and nothing more:

* **`Detached`** — a fire-and-forget coroutine (eager start, self-destroying at
  `final_suspend`). The acceptor and each per-connection handler are `Detached`.
* **`Task<T>`** — a lazy, awaitable coroutine that returns a value and uses
  symmetric transfer to resume its awaiter on completion. `fill()` (one
  non-blocking `recv`, suspending on `EAGAIN`) is a `Task<bool>`.
* **`Reactor`** — an epoll loop mapping a waited-on fd to the
  `std::coroutine_handle` parked on its readability (`EPOLLONESHOT`, re-armed on
  each `co_await`). A `co_await ReadAwaitable{reactor, fd}` suspends the current
  coroutine until `fd` is readable.

Every socket is owned by an RAII `Fd`; a `SIGTERM`/`SIGINT` handler flips an
`std::atomic<bool>` and kicks an `eventfd` so the reactor (or the `thread`
engine's `poll`) wakes and unwinds cleanly. The `thread` engine uses
`std::jthread` workers joined on shutdown.
