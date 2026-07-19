# 23-udp-and-peer-discovery — chatterd v2

chatterd grows peer discovery. Where chapters 21–22 built the TCP side (the
canonical chatterd chat frame between two known endpoints), `chatterd
discover` adds the missing half: finding peers you were never told about. It
sends periodic **UDP multicast beacons**, listens for everyone else's, and the
first time it hears a new peer it opens a TCP connection to that peer's
advertised port and exchanges one chat frame each way — the same canonical
wire frame every chatterd version speaks, reused here for JOIN/DELIVER.

```
$ ./demo.sh go run discover --group 239.23.7.1 --port 51823 \
      --name alice --tcp-port 9231 --iface 127.0.0.1 &
$ ./demo.sh go run discover --group 239.23.7.1 --port 51823 \
      --name bob   --tcp-port 9232 --iface 127.0.0.1 &

# alice's stdout:
discovered peer bob at 127.0.0.1:9232
peer bob says: hello from bob

# bob's stdout:
discovered peer alice at 127.0.0.1:9231
peer alice says: hello from alice
```

Each process announces on stderr (`chatterd: announcing as <name> on
<group>:<port> (tcp <ip>:<tcp-port>)`), runs for `--rounds` beacons spaced
`--announce-ms` apart, then exits 0 after an 800 ms grace window so in-flight
exchanges finish.

## What the chapter is really about

UDP multicast is the one-to-many primitive. A beacon sent once to a group
address (`239.0.0.0/8`, the administratively-scoped block) reaches every
member on the interface without the sender knowing who or how many they are.
Discovery is: **join the group, announce yourself, and learn from what you
hear.** The three implementations exercise the same setsockopt dance —

- **`IP_ADD_MEMBERSHIP`** (`ip_mreq { imr_multiaddr = group, imr_interface =
  iface }`) — join the group on a specific interface. Without this the kernel
  drops group traffic before it reaches the socket.
- **`IP_MULTICAST_IF`** — pin *outgoing* multicast to the same interface, so a
  multi-homed host sends the beacon where its peers are listening.
- **`IP_MULTICAST_LOOP`** — deliver our own multicast to other sockets **on
  this host**. This is what makes the local two-process verify work over the
  loopback interface; across two machines it is irrelevant (the packet is on
  the wire either way).
- **`IP_MULTICAST_TTL = 1`** — keep beacons on the local link; do not let a
  router forward them.

The receive side dedupes by peer name, so "discovered peer …" prints exactly
once per peer no matter how many beacons arrive.

### Loopback multicast note

The loopback device `lo` is not flagged `MULTICAST`, but the IPv4 stack still
loops group traffic **when the multicast interface is set to `127.0.0.1`**.
That is why the local verify passes `--iface 127.0.0.1`: both processes join
and send on the loopback address and the kernel loops each beacon to the
other. On the VMs the interface is the guest's `enp1s0` address and the
beacons cross the `virbr0` bridge for real.

## Wire formats (byte-exact, identical across all three languages)

### 1. Discovery beacon — UDP datagram to `<group>:<port>`

An ASCII text datagram, no framing (UDP preserves message boundaries):

```
CHATTERD1 <name> <tcp-port> <host-ip>
```

- `CHATTERD1` — literal magic / version tag (9 bytes).
- single ASCII spaces separate the four fields; `<name>` contains no space.
- `<tcp-port>` — decimal, the TCP port this peer accepts chat frames on.
- `<host-ip>` — dotted-quad the peer wants to be dialed at (its `--iface`).

A receiver splits on whitespace, requires exactly 4 fields with `tok[0] ==
"CHATTERD1"`, and ignores a beacon whose `<name>` equals its own.

Captured on the peer guest's `enp1s0` during the two-host run (`tcpdump -X`),
a beacon from systems-target — 35 payload bytes:

```
IP 192.168.124.7.40516 > 239.23.7.1.51824: UDP, length 35
    0x001a:            4348 4154        ....CHAT
    0x0020:  5445 5244 3120 7461 7267 6574 2039 3234  TERD1.target.924
    0x0030:  3120 3139 322e 3136 382e 3132 342e 37    1.192.168.124.7
=  "CHATTERD1 target 9241 192.168.124.7"
```

### 2. Chat frame — TCP, the canonical chatterd frame (reused, not this chapter's invention)

This is **not** a ch23-specific format — it is the one wire frame every
chatterd version (ch21's thread-per-connection server, ch22's epoll engine,
ch24's heartbeat client, ch27's async transport) reads and writes. ch23 just
dials it peer-to-peer instead of client-to-server. The UDP beacon above is a
completely separate object; this frame only ever travels over the TCP
connection opened *after* discovery.

```
+-------+-------+------+-----------------+------------------------+
| magic | ver   | type | length          | payload                |
| 2B    | 1B    | 1B   | 2B, big-endian  | <length> bytes, UTF-8   |
+-------+-------+------+-----------------+------------------------+
 0x43 0x48  0x01  JOIN=1  0..65535          type-specific:
  "CH"              MSG=2                    JOIN    = name
                     DELIVER=3                MSG     = text
                     WELCOME=4                DELIVER = name + 0x00 (NUL) + text
                     PING=5                   WELCOME = text
                                              PING    = empty
```

The header (magic/version/type/length) is byte-identical across every
chatterd version; a version only ever *adds* a type, never changes the
header. ch23's post-discovery exchange uses two of the five types, one frame
each way:

1. the discoverer dials `<host-ip>:<tcp-port>` and sends a **JOIN** frame
   whose payload is its own name (`<self>`) — the same "announce on connect"
   JOIN chapters 21/22/24/27 use;
2. the listener reads that frame, replies with a **DELIVER** frame whose
   payload is `<self>\0hello from <self>` (name, then a NUL byte, then the
   greeting text), and closes;
3. the discoverer reads the DELIVER frame, splits the payload on the NUL
   byte, and prints `peer <name> says: <text>`.

Both peers discover each other independently, so each dials the other once and
each prints one `peer … says:` line — the message crossed the network. Because
the header is shared, a ch23 peer's JOIN/DELIVER exchange is wire-compatible
with a ch21/ch22/ch24/ch27 chatterd speaking the same frame; the newer-only
types (WELCOME, PING) simply never appear in this chapter's exchange.

## The three implementations

Identical CLI, stdout/stderr shapes, and exit codes (**0** ok / **1** runtime
error / **2** usage). Malformed `--group`/`--iface` or an unknown flag is a
usage error (2); a syntactically valid but unbindable `--iface` fails at bind
time (1).

| | multicast socket | concurrency |
|---|---|---|
| **C++23** | raw `setsockopt` `IP_ADD_MEMBERSHIP` / `IP_MULTICAST_IF` / `IP_MULTICAST_LOOP` / `IP_MULTICAST_TTL`; every fd in an RAII `Fd`, no naked `close`; `std::expected` on each fallible step; output via `std::println` | accept + receive loops on `std::jthread`s, stopped through their `stop_token`; dedup set under a `std::mutex` |
| **Go 1.26** | `net.ListenMulticastUDP` for the joined receiver; the beacon sender is a `golang.org/x/sys/unix` socket with `IP_MULTICAST_IF`/`_LOOP`/`_TTL` set via `Setsockopt*` | accept + receive goroutines coordinated by a `context` + `sync.WaitGroup`; errors wrapped with `%w` |
| **Rust (2024)** | `socket2::Socket` (`join_multicast_v4` / `set_multicast_if_v4` / `set_multicast_loop_v4` / `set_multicast_ttl_v4`), converted `into()` a std `UdpSocket` (an `OwnedFd`) shared via `Arc` | accept + receive `std::thread`s stopped through an `AtomicBool`; `Result`/`?` throughout, no `tokio` (this chapter is sockets, not async) |

## Running it

```
./demo.sh build            # build all three
./demo.sh go run discover --group 239.23.7.1 --port 51823 \
    --name alice --tcp-port 9231 --iface 127.0.0.1
```

Flags: `--group <ip>` and `--port <n>` and `--name <s>` are required;
`--tcp-port` (default 9101), `--iface` (default 127.0.0.1), `--announce-ms`
(default 200), `--rounds` (default 10) are optional.

## Verification

`verify.lua` (mode `vm-peer`) runs the **local** two-process assertion on
loopback multicast — it clears `TARGET` so `demo.sh run` executes on the host,
starts `alice` and `bob` with distinct names and TCP ports, and asserts each
discovers the other exactly once, exchanges the chat frame, and exits 0, plus
the usage/exit-code shapes. Distinct ports per language keep back-to-back
cpp/go/rust runs off each other's `TIME_WAIT` sockets.

### Two-host demo (systems-target + systems-peer)

The real observable is two daemons on two machines. Deploy the binary to both
guests and run `discover` on each with its own `--iface`; the beacons cross
`virbr0` and each guest discovers and greets the other:

```
# on systems-target (192.168.124.7):
discovered peer peer at 192.168.124.95:9231
peer peer says: hello from peer

# on systems-peer (192.168.124.95):
discovered peer target at 192.168.124.7:9231
peer target says: hello from target
```
