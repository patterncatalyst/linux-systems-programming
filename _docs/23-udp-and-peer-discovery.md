---
title: "UDP and peer discovery"
order: 23
part: "Networking"
description: "chatterd learns to find peers it was never told about: UDP multicast beacons on 239.23.7.1, the IP_ADD_MEMBERSHIP / IP_MULTICAST_TTL setsockopt dance, a small gossip protocol deduped by name, and the bridge from a discovered datagram to the canonical chatterd TCP chat frame (introduced ch21) — proven across two libvirt guests with tcpdump, ss -uap, and both transcripts."
duration: "55 minutes"
---

Chapters 21 and 22 built `chatterd` on TCP: the canonical chatterd chat
frame — magic `"CH"`, version, type, a big-endian length, and a payload —
carried between two endpoints you already knew — you typed the peer's
address on the command line. This chapter removes that assumption. `chatterd
discover` finds peers it was never told about, and the primitive that makes
that possible is the one TCP cannot offer: **UDP multicast**, a single
`sendto` that reaches every listener in a group without the sender knowing
who — or how many — they are. The shape of the chapter is a bridge: a
connectionless UDP beacon announces "I exist, dial me here," a receiver hears
strangers and dedupes them, and the first time it meets a new name it opens
an ordinary TCP connection and speaks the *same canonical chat frame ch21
introduced* — the header never changes; each version only adds message
*types*. Discovery is UDP; the conversation stays TCP. The star of the
run is real: two libvirt guests, `systems-target` and `systems-peer`, each
discovering the other across the `virbr0` bridge.

The code is in `examples/23-udp-and-peer-discovery/`. `./demo.sh` there builds
all three implementations; its `README.md` specifies the CLI, the byte-exact
beacon and frame formats, and the exit codes all three languages share.

{% include excalidraw.html
   file="23-two-vm-topology"
   alt="A libvirt virbr0 bridge band on 192.168.124.0/24 holds a central amber multicast-group box for 239.23.7.1 port 51824 TTL 1, with systems-target at 192.168.124.7 on the left and systems-peer at 192.168.124.95 on the right, each joined to the group on enp1s0 and holding udp fd 4 and tcp 9241; two dashed amber bidirectional beacon arrows (35 bytes from target, 34 bytes from peer) reach the group, and a solid bidirectional TCP arrow labelled one ch21 chat frame each way joins the two guests directly."
   caption="Figure 23.1 — the two-guest lab topology: both VMs join 239.23.7.1, beacon to the group, and bridge each discovery to a direct TCP :9241 chat frame; every address, port, and byte-length here is from the two-host run later in the chapter" %}

> **Tools used** — `ss`, `tcpdump`, `ip`, `strace` (host); `tcpdump`, `ss`,
> `ip maddr` (systems-target / systems-peer VMs, run live in this chapter);
> `tcplife`/`tcpconnect`/`offcputime` bcc-tools (lab VM, exercised in Part 8).
> Everything host-side is checked by `scripts/check-host.sh` or ships with
> Fedora; the VM tools ship in the lab cloud-init.

## Why UDP, and why the datagram stays small

TCP gives you a reliable, ordered byte *stream* over an established
connection; every one of those words is a cost. A connection is state on both
ends and a three-way handshake before the first byte; reliability means
retransmit timers and buffering; ordering means head-of-line blocking. For
discovery you want the opposite of all of it. You do not know who your peers
are, so there is nobody to connect *to*; you want one announcement to reach
many; and you do not care if a single beacon is lost, because another one
follows in 200 ms. That is UDP: connectionless, message-oriented, best-effort.
`sendto` hands the kernel one datagram, it goes on the wire as one IP packet
(or a fragmented set), and the receiver's `recv` returns it whole or not at
all — **message boundaries are preserved**, unlike a TCP stream where you must
re-frame yourself. Loss and reorder are possible and, for a beacon, harmless:
a dropped beacon just means "discovered one round later."

The datagram stays small on purpose. `chatterd`'s beacon is one ASCII line —
`CHATTERD1 <name> <tcp-port> <host-ip>` — 34 or 35 bytes on the wire in the
run below, far under the ~1472-byte non-fragmenting payload of a 1500-byte
MTU. Keeping a beacon inside one un-fragmented packet is a real discipline:
UDP has no reassembly guarantee beyond IP's, so a beacon that fragments turns
one lost fragment into a wholly lost beacon. Small, self-describing, and
idempotent is the whole design.

**Multicast versus broadcast.** Broadcast (`255.255.255.255` or a subnet
directed broadcast) reaches *every* host on the segment whether it cares or
not, and routers rightly drop it. Multicast is opt-in: a datagram addressed
to a group in `239.0.0.0/8` (the administratively-scoped block) is delivered
only to sockets that have *joined* that group, via `IP_ADD_MEMBERSHIP`. Three
more socket options shape the send: `IP_MULTICAST_IF` pins which interface the
beacon leaves by (it matters on a multi-homed host), `IP_MULTICAST_TTL = 1`
keeps it on the local link so no router forwards it, and `IP_MULTICAST_LOOP`
controls whether *this host's* other sockets also receive it — the switch that
makes the local two-process demo work over loopback and is irrelevant across
two machines, where the packet is on the wire regardless.

{% include excalidraw.html
   file="23-discovery-sequence"
   alt="Two horizontal bands. The upper blue band, UDP multicast discovery, holds three boxes left to right: announce loop sendto to group emitting CHATTERD1 target 9241; receive loop recv one datagram splitting four fields and checking magic; and an amber parse-and-dedup box that skips its own name and calls seen.first before dialing. An amber arrow labelled new peer bridge to TCP drops into the lower grey band, TCP bridge, whose boxes are connect with retry, write_frame slash read_frame with a 6-byte header (magic, version, type, 2-byte big-endian length) plus payload, and a dark stdout box printing discovered peer and peer says. A note reads loss and reorder tolerated."
   caption="Figure 23.2 — one peer's path from a connectionless beacon to a connection-oriented frame: recv, dedup by name, then dial and speak the canonical chat frame ch21 introduced (JOIN out, DELIVER back)" %}

## How the code works

The multicast socket is the heart of the chapter, and all three languages run
the same four-option dance around it — join the group, pin the outgoing
interface, enable loopback, set TTL 1 — differing only in how thick the
library wrapper is. Here it is verbatim from
`examples/23-udp-and-peer-discovery/`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
std::expected<Fd, std::string> make_multicast_socket(in_addr group, in_addr iface,
                                                     std::uint16_t port) {
  Fd fd(::socket(AF_INET, SOCK_DGRAM, 0));
  if (!fd) return std::unexpected(errno_msg("socket(udp)"));
  int one = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
    return std::unexpected(errno_msg("SO_REUSEADDR"));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(port);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0)
    return std::unexpected(errno_msg("bind(udp)"));

  ip_mreq mreq{};
  mreq.imr_multiaddr = group;
  mreq.imr_interface = iface;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    return std::unexpected(errno_msg("IP_ADD_MEMBERSHIP"));
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) < 0)
    return std::unexpected(errno_msg("IP_MULTICAST_IF"));
  unsigned char loop = 1;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
    return std::unexpected(errno_msg("IP_MULTICAST_LOOP"));
  unsigned char ttl = 1;
  if (::setsockopt(fd.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
    return std::unexpected(errno_msg("IP_MULTICAST_TTL"));
  set_rcv_timeout(fd.get(), 200);
  return fd;
}
```

```go
func makeSender(ip4 [4]byte) (int, error) {
	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_DGRAM, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	if err := unix.SetsockoptInet4Addr(fd, unix.IPPROTO_IP, unix.IP_MULTICAST_IF, ip4); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("IP_MULTICAST_IF: %w", err)
	}
	if err := unix.SetsockoptByte(fd, unix.IPPROTO_IP, unix.IP_MULTICAST_LOOP, 1); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("IP_MULTICAST_LOOP: %w", err)
	}
	if err := unix.SetsockoptByte(fd, unix.IPPROTO_IP, unix.IP_MULTICAST_TTL, 1); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("IP_MULTICAST_TTL: %w", err)
	}
	return fd, nil
}
```

```rust
fn make_multicast(cfg: &Config) -> Fallible<UdpSocket> {
    let sock = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
    sock.set_reuse_address(true)?;
    let bind = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, cfg.port);
    sock.bind(&SockAddr::from(bind))?;
    sock.join_multicast_v4(&cfg.group, &cfg.iface)?;
    sock.set_multicast_if_v4(&cfg.iface)?;
    sock.set_multicast_loop_v4(true)?;
    sock.set_multicast_ttl_v4(1)?;
    sock.set_read_timeout(Some(Duration::from_millis(200)))?;
    Ok(sock.into())
}
```

Three spellings, one syscall sequence. The C++ version is the raw form and
shows every step: bind to `INADDR_ANY` on the group port (you bind the
*port*, not the group address, so every group joined on that port arrives),
then the four `setsockopt` calls. `SO_REUSEADDR` matters because two
`chatterd` processes on one host must both bind the same UDP port — without it
the second `bind` fails `EADDRINUSE`. The `ip_mreq` carries two addresses:
`imr_multiaddr` is the group to join, `imr_interface` is the local interface
address to join it *on* — get that wrong on a multi-homed box and the kernel
drops group traffic before your socket ever sees it. Rust's `socket2` collapses
the same calls into named methods (`join_multicast_v4`, `set_multicast_ttl_v4`)
and then `into()`s the socket to a plain `std::net::UdpSocket` — an `OwnedFd`
it can share across threads. Go splits the roles: the *receiver* is built by
`net.ListenMulticastUDP` (which does the join for you), while the beacon
*sender* is this separate raw `unix.Socket` with the three send-side options
set explicitly — the standard library has no one-call multicast sender, so it
drops to `golang.org/x/sys/unix`. Note the `SO_RCVTIMEO` / `set_read_timeout`
of 200 ms on every receive socket: it is not about beacons, it is the escape
hatch that lets each blocking `recv` wake periodically to re-check the stop
flag at shutdown.

The rest is small. The **announce loop** (in `main`/`discover`) formats the
beacon once — `CHATTERD1 <name> <tcp-port> <iface>` — and `sendto`s it to the
group `--rounds` times, `--announce-ms` apart. The **receive loop** reads a
datagram, splits on whitespace, requires exactly four fields with `tok[0] ==
"CHATTERD1"`, ignores any beacon whose name equals its own, parses the peer's
port and IP, and consults a dedup set so a peer is announced *once* no matter
how many of its beacons arrive. On first sight it prints `discovered peer
<name> at <ip>:<port>`, then bridges to TCP: `dial` opens a connection to the
advertised `host-ip:tcp-port` — with a bounded retry, because the beacon can
arrive a hair before the peer's listener is up — `write_frame` sends a
canonical **JOIN** frame whose payload is its own name, `read_frame` reads
the peer's **DELIVER** reply (payload `<name>\0<text>` — name, a NUL byte,
then the greeting text), splits it on the NUL, and prints `peer <name> says:
<text>`. The **accept loop** on the TCP listener is the mirror image: read
the dialer's JOIN frame, reply with a DELIVER frame carrying its own name +
NUL + greeting text, close. Both peers discover each other independently, so
each dials once and each prints exactly one `peer … says:` line. The
`<host-ip>` field is the one fragile
convention worth flagging: a peer advertises the address it wants to be dialed
at (its `--iface`), so behind NAT or on a different subnet it would advertise
an unreachable address — a limitation a real gossip protocol solves with
observed source addresses, out of scope here.

## Errors, three ways

The contract matches every `chatterd` version: exit 0 on success, 1 on a
runtime failure with one diagnostic line, 2 on a usage error — and
`verify.lua` asserts all three per language. The split is about *when* a bad
input is caught. A malformed `--group`/`--iface`, an unknown flag, or a
missing subcommand is a pure usage error: parsing rejects it before any socket
exists, prints `usage: chatterd discover …`, and exits 2 (C++ returns
`std::unexpected(2)`, Go returns `ok=false` with code 2, Rust returns
`Err(2)`). But a *syntactically valid* `--iface` that no interface owns is
only discoverable at `bind` time, so it fails later as a runtime error, exit
1 — `chatterd: error: bind(tcp): Cannot assign requested address` and its Go/
Rust equivalents. The dial path is deliberately forgiving: it retries up to 40
times at 50 ms before giving up with `connect … giving up` (exit-neutral — it
logs to stderr and the loop continues), because a beacon legitimately races
its sender's own listener. And `send`/`recv` guard `EINTR` with a retry in the
C++ frame I/O, which the Go and Rust runtimes handle beneath their standard
libraries.

## Concurrency lens

Three flows of control run per process: the announce loop (on `main`), the TCP
accept loop, and the UDP receive loop. The receive loop is the only writer of
the dedup set, and each language reflects that differently. C++ wraps the set
in a `Seen` struct with a `std::mutex` — defensive, since a future second
reader would need it — while Go confines its `map[string]bool` to the
`recvLoop` goroutine and Rust confines its `HashSet` to the `recv_loop`
thread, so neither needs a lock at all: the state is owned by one flow, which
is Chapter 6's "share memory by communicating" reflex applied to sockets.
Shutdown is the interesting coordination. There is no natural end to a
`recv` loop, so each waits on a stop signal *and* a 200 ms socket read
timeout: the timeout wakes the blocked `recv` often enough to observe the
signal. C++ uses `std::jthread` and each loop's `stop_token`; Go cancels a
`context` and `Wait`s a `sync.WaitGroup`; Rust flips an `AtomicBool` and
`join`s the threads. The 800 ms grace window after the last beacon is the
subtle part: it lets an exchange that a *final* beacon just triggered finish
before the listen threads are told to stop, so a discovery on the last round
still completes its frame.

## Build, run, observe

```bash
[host]$ cd examples/23-udp-and-peer-discovery && ./demo.sh build
```

The runner (`python3 scripts/test-all-examples.py --only
23-udp-and-peer-discovery --mode vm`) reports `PASS PASS PASS` for cpp, go,
rust (3 passed, 0 failed, 0 skipped). Its `verify.lua` clears `TARGET` so the
assertion runs *locally* on loopback multicast: two instances discover each
other and exchange a frame. Driving that by hand the way this chapter's
loopback evidence was produced (distinct names, distinct TCP ports):

```console
[host]$ ./cpp/build/release/app discover --group 239.23.7.1 --port 51823 --name alice --tcp-port 9231 --iface 127.0.0.1 &
[host]$ ./cpp/build/release/app discover --group 239.23.7.1 --port 51823 --name bob --tcp-port 9232 --iface 127.0.0.1
chatterd: announcing as bob on 239.23.7.1:51823 (tcp 127.0.0.1:9232)
discovered peer alice at 127.0.0.1:9231
peer alice says: hello from alice
```

and `alice`'s stdout is the mirror: `discovered peer bob at 127.0.0.1:9232` /
`peer bob says: hello from bob`. The socket is a joined, unconnected UDP
endpoint — `ss -uap` on the host during the run shows exactly that, one
`UNCONN` socket bound to all addresses on the group port, owned by our binary:

```console
[host]$ ss -uap | grep 51833
UNCONN 0  0  0.0.0.0:51833  0.0.0.0:*  users:(("app",pid=2818975,fd=4))
[host]$ ip maddr show lo | grep 239.23.7.1
	inet  239.23.7.1
```

Loopback multicast is the one quirk to know: `lo` is not flagged `MULTICAST`,
but the IPv4 stack still loops group traffic when the multicast interface is
set to `127.0.0.1`, which is why the local demo passes `--iface 127.0.0.1`.
Output equality alone could be faked by a program that quietly skipped the
network, so the real proof is on two machines watching the wire.

## Cross-check: two guests, the beacons on the wire, and both transcripts

The star run is `systems-target` (192.168.124.7) and `systems-peer`
(192.168.124.95), each launched with its own `--iface`. First, the beacons
themselves — `tcpdump` on the peer guest sees both guests' multicast, decoded
byte for byte:

```console
[peer]$ sudo tcpdump -i enp1s0 -n -X udp port 51824 -c 8
15:09:31.986449 IP 192.168.124.95.51824 > 239.23.7.1.51824: UDP, length 34
	0x0000:  4500 003e e3df 4000 0111 62af c0a8 7c5f  E..>..@...b...|_
	0x0020:  5445 5244 3120 7065 6572 2039 3234 3120  TERD1.peer.9241.
	0x0030:  3139 322e 3136 382e 3132 342e 3935       192.168.124.95
15:09:32.531220 IP 192.168.124.7.51824 > 239.23.7.1.51824: UDP, length 35
	0x0000:  4500 003f 89b9 4000 0111 bd2c c0a8 7c07  E..?..@....,..|.
	0x0020:  5445 5244 3120 7461 7267 6574 2039 3234  TERD1.target.924
	0x0030:  3120 3139 322e 3136 382e 3132 342e 37    1.192.168.124.7
```

Everything in the chapter is on the wire. The destination is the group
`239.23.7.1.51824`; the ASCII column reads `CHATTERD1 peer 9241
192.168.124.95` (34 bytes) and `CHATTERD1 target 9241 192.168.124.7` (35
bytes) — the length difference is exactly the shorter name and IP. In the IP
header, the byte after `4000` is `01`: TTL 1, our `IP_MULTICAST_TTL`, the
router-stop that keeps beacons link-local. Second, the joined socket and group
membership on the peer guest, while `chatterd` runs:

```console
[peer]$ sudo ss -uanp | grep 51824
UNCONN 0  0  0.0.0.0:51824  0.0.0.0:*  users:(("chatterd",pid=2105,fd=4))
[peer]$ ip maddr show enp1s0
	inet  239.23.7.1
```

`ip maddr` proves the kernel actually joined the group on `enp1s0` — the
observable effect of `IP_ADD_MEMBERSHIP` — and `ss -uap` shows the same
`UNCONN` fd-4 UDP socket as on the host. Third, the payoff: both guests'
stdout, quoted verbatim from the two-host run:

```console
[vm]$ /home/fedora/chatterd discover --group 239.23.7.1 --port 51824 --name target --tcp-port 9241 --iface 192.168.124.7
discovered peer peer at 192.168.124.95:9241
peer peer says: hello from peer
```

```console
[peer]$ /home/fedora/chatterd discover --group 239.23.7.1 --port 51824 --name peer --tcp-port 9241 --iface 192.168.124.95
discovered peer target at 192.168.124.7:9241
peer target says: hello from target
```

Each guest discovered the other exactly once and exchanged one TCP frame — the
message genuinely crossed the network. No libvirt workaround was needed: both
guests sit on the same `virbr0` L2 segment, so a TTL-1 multicast beacon is
delivered by the bridge as ordinary link-local traffic; libvirt's NAT only
governs traffic *leaving* the host toward the internet, which discovery never
does.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the kernel-side view of the UDP→TCP bridge — watching the dial land with
> bcc-tools `tcpconnect`, the short-lived chat connection appear and close in
> `tcplife`, and the receive loops' off-CPU parking in `offcputime` — needs
> root and the bcc stack, which are not run on this host; the Debugging part
> (Part 8) exercises exactly that on `systems-target`.

## What you learned

- UDP is connectionless, message-oriented, best-effort: `sendto` emits one
  datagram whose boundaries `recv` preserves, and loss/reorder are tolerable
  for a beacon that repeats — which is why discovery is UDP and the
  conversation is TCP.
- Multicast is opt-in delivery to a joined group: `IP_ADD_MEMBERSHIP` joins,
  `IP_MULTICAST_IF` pins the send interface, `IP_MULTICAST_TTL = 1` keeps it
  link-local, and `IP_MULTICAST_LOOP` controls local delivery — verified live
  by `ip maddr show` and the TTL-1 byte in `tcpdump`.
- Beacons stay small and self-describing (34–35 bytes here) to fit one
  un-fragmented packet, and a receiver dedupes by name so one peer is
  announced once regardless of beacon count.
- The bridge from discovery to conversation is a plain `connect` to the
  advertised endpoint, reusing the canonical chat frame ch21 introduced (a
  JOIN out, a DELIVER back) — proven across two guests, each printing one
  `discovered peer` and one `peer … says:` line.

Next, **time, timers, and deadlines**: the clocks a networked program must
choose between, and how `timerfd` and deadline discipline keep a beacon loop
accurate about *when* it fires.

---

<p><span class="status status--verified">verified</span> — evidence produced
this session on the Fedora 44 reference host (kernel 7.1.3-200.fc44) and the
two lab guests <code>systems-target</code> (192.168.124.7) /
<code>systems-peer</code> (192.168.124.95): the runner printed
<code>23-udp-and-peer-discovery&nbsp;&nbsp;PASS&nbsp;&nbsp;PASS&nbsp;&nbsp;PASS</code>
(3 passed, 0 failed, 0 skipped) under <code>--mode vm</code>; the host
loopback run produced the exact <code>alice</code>/<code>bob</code>
<code>discovered peer</code> and <code>peer … says:</code> lines, with
<code>ss -uap</code> showing the <code>UNCONN 0.0.0.0:51833</code> fd-4 socket
and <code>ip maddr show lo</code> the joined group. On the two guests,
<code>tcpdump -X</code> on <code>systems-peer</code> captured both beacons to
<code>239.23.7.1.51824</code> (34 B <code>CHATTERD1 peer 9241
192.168.124.95</code> and 35 B <code>CHATTERD1 target 9241 192.168.124.7</code>,
IP-header TTL byte <code>01</code>); <code>ss -uanp</code> showed the joined
<code>chatterd</code> fd-4 UDP socket and <code>ip maddr show enp1s0</code> the
239.23.7.1 membership; and both guests' stdout printed one
<code>discovered peer</code> and one <code>peer … says:</code> line as quoted.
No libvirt NAT workaround was required — the guests share one <code>virbr0</code>
L2 segment. The bcc-tools <code>tcpconnect</code>/<code>tcplife</code>/<code>offcputime</code>
callout is unverified as marked and is exercised in Part 8.</p>
