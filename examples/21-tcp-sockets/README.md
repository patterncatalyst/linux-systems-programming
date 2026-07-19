# 21-tcp-sockets — chatterd v0

`chatterd` is the book's peer-to-peer chat program. This first cut is a
thread-per-connection TCP **chat room**: one binary is both the daemon and the
client.

```
# terminal 1 — the room
$ ./demo.sh cpp run serve --port 9000
chatterd: listening on 127.0.0.1:9000
chatterd: bob joined
chatterd: alice joined

# terminal 2 — bob
$ ./demo.sh cpp run chatctl --port 9000 --name bob
server: bob joined
server: alice joined
alice: hello bob

# terminal 3 — alice
$ ./demo.sh cpp run chatctl --port 9000 --name alice
server: alice joined
hello bob            # typed on stdin; sent to everyone else
```

`serve` accepts connections and runs each client on its own thread (C++
`std::jthread`, a Go goroutine, a Rust `std::thread`). Every message a client
sends is broadcast to all **other** clients; a join notice is broadcast to
**all** members, the newcomer included. `chatctl` connects, announces its name,
then sends one frame per line of stdin while a reader thread prints everything
that arrives as `<name>: <text>`.

## The wire protocol (fixed for the whole book)

This chapter introduces **the** chatterd chat frame: chapters 22, 24, and 27
reuse this exact header and only ever *add* frame types, never change it. A
frame that a `chatctl` from this chapter reads or writes is byte-identical to
the one a v1 (ch22), v3 (ch24), or v4 (ch27) build would produce.

```
 byte:  0    1    2      3      4    5      6                6+L-1
       +----+----+------+------+----+----+---------------------------+
       | 'C'| 'H'| ver  | type | length  |   L bytes of UTF-8        |
       |    |    | 0x01 |      | (u16BE) |   payload                 |
       +----+----+------+------+----+----+---------------------------+
```

- **magic** — the two bytes `0x43 0x48` (ASCII `"CH"`). Any other value means
  "not a chatterd frame"; the reader closes the connection.
- **version** — one byte, `0x01`. This chapter is version 0 of the *program*
  but the wire's version byte has always been `1`; only the frame *header*
  would ever bump it, and it never has.
- **type** — one byte selecting the payload's shape. This chapter uses three
  of the five book-wide types:

  | Type | Value | Payload | Who sends it | Meaning |
  |---|---|---|---|---|
  | **JOIN** | `1` | `name` | client, once, right after connecting | "I am `name`" |
  | **MSG** | `2` | `text` | client, per stdin line | "I say `text`" |
  | **DELIVER** | `3` | `name` `00` `text` | server, to every client | a broadcast |

  `WELCOME` (`4`) and `PING` (`5`) are reserved for chapters 22 and 24; this
  program never sends either, and a `chatctl` reader here skips any frame type
  it does not recognize instead of erroring out — which is exactly what lets a
  ch21 client stay connected to a newer server that does send them.
- **length** — a 2-byte unsigned payload length in **big-endian** order,
  `0..65535`. It counts the payload only, never the 6 header bytes.
- **payload** — `length` bytes of UTF-8, shaped per the `type` above. A
  `DELIVER` payload's `name` and `text` are separated by a single `0x00` NUL
  byte; neither field may itself contain a NUL.

The server itself never invents new frame *types*. A client's `JOIN` sets its
name and a `MSG` carries one line of chat; the server relays every `MSG` as a
`DELIVER` and synthesises **notices** as `DELIVER` frames whose sender name is
the reserved string `server`:

- on a join → `DELIVER` `server` `00` `<name> joined`, sent to every member.
- on a disconnect → `DELIVER` `server` `00` `<name> left`, sent to the rest.

So a client that reads a `DELIVER` payload `server\0alice joined` prints
`server: alice joined`, and one that reads `alice\0hello bob` prints
`alice: hello bob`. That is the whole protocol for this chapter; chapters
22–24 and 27 add `WELCOME`/`PING` and new engines around it, never a new
header.

### Worked bytes

`alice`'s JOIN frame (payload `alice`, 5 bytes):

```
43 48 01 01  00 05  61 6c 69 63 65
\___/ ^^ ^^  \___/  \____________/
magic ver type len=5    "alice"
      "CH"  JOIN
```

`alice` sending `hello bob` as a MSG frame (payload `hello bob`, 9 bytes):

```
43 48 01 02  00 09  68 65 6c 6c 6f 20 62 6f 62
\___/ ^^ ^^  \___/  \______________________/
magic ver type len=9        "hello bob"
      "CH"  MSG
```

The server's broadcast of that message as a DELIVER frame (payload
`alice\0hello bob`, 15 bytes):

```
43 48 01 03  00 0f  61 6c 69 63 65 00 68 65 6c 6c 6f 20 62 6f 62
\___/ ^^ ^^  \___/  \____________/ ^^ \______________________/
magic ver type len=15   "alice"     NUL      "hello bob"
      "CH" DELIVER
```

## The CLI

```
chatterd serve   --port PORT [--host HOST]              # default host 127.0.0.1
chatterd chatctl --port PORT --name NAME [--host HOST]
```

Exit codes are the same across all three languages: `0` clean, `1` a runtime
error (bind refused, connect refused, …), `2` a usage error. `serve` runs until
**SIGINT**, then closes the listener, lets the connection threads wind down, and
exits `0`. `chatctl` runs until stdin reaches EOF. Empty stdin lines are
dropped so a client never emits an empty-text frame (that would read as a
second JOIN).

## The three implementations

Identical CLI, identical stdout/stderr shapes, identical exit codes — one
`verify.lua` covers all three.

| | connection model | listener shutdown | fd ownership |
|---|---|---|---|
| **C++23** | one `std::jthread` per client; a `std::stop_callback` shuts the socket down on stop so the blocking read unwinds | `std::atomic<bool>` set by the SIGINT handler, polled by a `poll(2)` that SIGINT interrupts | RAII `Socket` owns every fd; no naked `close` |
| **Go 1.26** | one goroutine per client + a per-client writer goroutine draining a channel; a `select` over send/`done`/`ctx` never wedges a broadcaster | a `context` cancelled on SIGINT; `ln.Close()` unblocks `Accept` | `net.Conn` / `net.Listener` |
| **Rust 2024** | one `std::thread` per client; broadcasts write to `try_clone`d `TcpStream`s under a `Mutex` | `AtomicBool` set by the SIGINT handler, polled by a non-blocking `accept` loop | listener built via `nix` and taken as an `OwnedFd`; `TcpStream` owns each connection |

No async runtime here on purpose — this chapter is about blocking sockets and
threads. Chapter 27 is where `chatterd` meets tokio.

## Gotchas worth remembering

- **Length prefix, not newline.** TCP is a byte stream with no message
  boundaries; a single `read` may return half a frame or two frames. Every
  reader loops until it has all 6 header bytes, then exactly `length` payload
  bytes. `\0` inside a `DELIVER` payload is why a newline delimiter would not
  do.
- **Broadcast excludes the sender; join notices do not.** A speaker already
  sees what they typed, so their own message is not echoed back. A join,
  though, goes to every member — which is why both clients in a two-party room
  each see a join line.
- **SIGINT and a blocking accept.** A flag set in the handler is invisible to a
  thread parked in `accept`. C++ interrupts `poll` with the signal; Go closes
  the listener out from under `Accept`; Rust makes `accept` non-blocking and
  polls the flag. All three reach the same observable end: the listener closes
  and the process exits `0`.
- **`SO_REUSEADDR`.** Set on the listener so a fresh `serve` can rebind the port
  while a previous run's connections linger in `TIME_WAIT`.

## Files

```
demo.sh        # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
verify.lua     # behavioral checks (delivered lines, joins, clean SIGINT exit)
cpp/ go/ rust/ # one chatterd each; binary always ./…/app; demo.sh contract identical
```

## Verification

`python3 scripts/test-all-examples.py --only 21-tcp-sockets` builds all three
and runs `verify.lua` per language. The checks are behavioral: two real
`chatctl` clients are driven over loopback through scripted stdin fifos, and the
test asserts the **actually delivered lines** — bob receives `alice: hello`,
alice sees no echo of her own message, both clients see a join notice from the
`server` sender, the daemon logs each join, and SIGINT leaves the listener
closed with exit `0`.
