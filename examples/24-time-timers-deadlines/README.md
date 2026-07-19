# 24-time-timers-deadlines — chatterd v3, heartbeats + deadlines

Chapter 24 grows `chatterd` — the peer-to-peer chat daemon introduced in
chapter 21 — into a version that survives a peer going away. The lesson is
*time*: how to keep a connection alive with a periodic heartbeat, how to
declare a peer dead by measuring **silence on a monotonic clock**, and how to
come back with a **jittered exponential backoff** instead of a hot reconnect
loop. A standalone `clockprobe` subcommand measures the machine's actual timer
resolution so the numbers stop being abstract.

```
chatterd clockprobe                                  # measure clock/timer resolution
chatterd listen  --name N --addr HOST:PORT     [opts] # passive peer
chatterd connect --name N --peer NAME@HOST:PORT [opts] # active peer (dials + reconnects)

opts: --heartbeat-ms H  (default 1000)   PING interval
      --timeout-ms   T  (default 3000)   drop a silent peer after this
      --backoff-ms   B  (default 200)    base reconnect backoff
      --max-backoff-ms M (default 5000)  backoff ceiling
      --message MSG                      send one chat message after linking
      --seed S                           seed the jitter RNG (deterministic tests)
```

## Behavior (identical across C++, Go, and Rust)

- **`clockprobe`** prints exactly one line and exits 0:

  ```
  clockprobe: CLOCK_MONOTONIC res=<ns>ns nanosleep(1ms) actual=<us>us timerfd(1ms) actual=<us>us
  ```

  `res` is `clock_getres(CLOCK_MONOTONIC)`; the two `actual` fields are the
  measured wall-time of a 1 ms `nanosleep(2)` and a 1 ms one-shot `timerfd`,
  each timed on a **monotonic/steady clock** (never `CLOCK_REALTIME`). On a
  typical host both land near 1050–1100 µs — the overshoot is the scheduler's
  wake-up latency, which is exactly the point.

- **`listen`** binds, prints `chatterd: <N> listening on <ip>:<port>` (a
  `:0` port is reported as the actual ephemeral port), and accepts a peer.

- **`connect`** prints `chatterd: <N> connecting to <peer> at <addr>` and
  dials.

- On a completed JOIN handshake **both** sides print
  `chatterd: <N> linked with <peer>`; if `--message` was given, the message
  is sent and the receiver prints `chatterd: <N> message from <peer>: <text>`.

- While linked, each side sends a `PING` every `--heartbeat-ms`. **Any**
  received frame (JOIN/MSG/PING) is "traffic" and resets a
  `--timeout-ms` deadline. When the deadline fires with no traffic the peer
  is dropped: `chatterd: peer <peer> timed out`. A closed socket (peer
  killed) is handled the same way — reads stop, but it is the *deadline*, on
  the monotonic clock, that declares death.

- After a drop the **connector** reconnects with equal-jitter exponential
  backoff — `delay = B/2 + rand(0..B/2)`, with `B` doubling each failed
  attempt up to `--max-backoff-ms` and reset to the base on a fresh
  connection — printing `chatterd: reconnecting to <peer> in <ms>ms` before
  each wait. The **listener** simply returns to `accept()` to welcome a
  reconnecting peer.

- **SIGINT / SIGTERM** at any point (linked, mid-backoff, or waiting to
  accept) prints `chatterd: <N> shutting down` and exits 0.

- No/bad arguments print a usage block on stderr and exit 2.

## Wire format (identical across languages and every chatterd version)

This is **the** chatterd chat frame: introduced in chapter 21, and extended
in later chapters only by adding new `TYPE` values — the header itself never
changes, so every chatterd version (thread-per-connection in ch21, epoll in
ch22, the UDP-discovered peers of ch23, this chapter's heartbeats, the async
runtimes of ch27) reads and writes exactly these bytes. All multi-byte
integers are big-endian. The header is a fixed 6 bytes:

```
 offset  size  field     value / meaning
 ------  ----  --------  ---------------------------------------------------
   0      1    MAGIC0    0x43  ('C')
   1      1    MAGIC1    0x48  ('H')
   2      1    VERSION   0x01
   3      1    TYPE      0x01 JOIN     payload = peer name (UTF-8)
                                 0x02 MSG      payload = chat text (UTF-8)
                                 0x03 DELIVER  payload = name + 0x00 (NUL) + text
                                 0x04 WELCOME  payload = motd/roster text (UTF-8)
                                 0x05 PING     payload = empty        (v3 addition)
                                 0x06..0xFF    reserved for future versions
   4      2    LENGTH    uint16 payload length N (0..65535)
   6      N    PAYLOAD   N bytes
```

Framing rules, byte-exact and version-stable:

- A reader accumulates bytes and consumes a frame only once all `6 + N` bytes
  are present; partial reads are buffered.
- `MAGIC`/`VERSION` mismatch is a protocol error and drops the connection.
- `PING` is the frame type added in **v3** (this chapter) for heartbeating.
  `DELIVER` and `WELCOME` belong to the server-broadcast chatterd versions
  (ch22, ch27) — this chapter's peer-to-peer `listen`/`connect` model never
  emits them, but since the header is shared, a peer that ever receives one
  (or any `0x06..0xFF` value) treats it as generic liveness traffic and skips
  its `LENGTH` bytes rather than erroring. `JOIN` (the peer-name handshake)
  and `MSG` (chat text) are unchanged since ch21, so a v3 peer here still
  links with older or newer chatterd peers for everything except
  heartbeating — dropping a silent peer is the only thing PING adds.

Example: a JOIN announcing the name `A` is the 7 bytes
`43 48 01 01 00 01 41`; a PING is the 6 bytes `43 48 01 05 00 00`.

## One subject, three timer stacks

The protocol and the observable lines are identical; how each language builds
the heartbeat/deadline loop is the lesson.

- **C++ — `timerfd_create(CLOCK_MONOTONIC)` + `poll(2)`.** A periodic timerfd
  is the heartbeat; a `std::chrono::steady_clock` deadline is peer liveness
  (the poll timeout is the time left until it). The peer socket, the heartbeat
  timerfd, and a `signalfd(2)` shutdown doorbell share one `poll` loop. Every
  fd is owned by a RAII `UniqueFd`; sends use `MSG_NOSIGNAL`, and errors flow
  through `std::expected`.
- **Go — `time.Ticker` + `time.Timer` + `context`.** A `Ticker` fires the
  heartbeat, a `Timer` is the deadline (`Reset` on every frame), and the
  context from `signal.NotifyContext` is shutdown. A per-connection reader
  goroutine turns the socket into a channel of frames, so the session is one
  `select` over ticker / deadline / frames / `ctx.Done()`. `time.Now` carries
  a monotonic reading, so `time.Since` never sees a wall-clock step.
- **Rust — `nix` timerfd + `std::time::Instant` + `poll`.** A periodic
  `nix::sys::timerfd::TimerFd` is the heartbeat, an `Instant` deadline is peer
  liveness, and a `nix` `SignalFd` is shutdown — all three in one
  `nix::poll::poll` loop over `std` `TcpStream`/`TcpListener` fds (`OwnedFd`
  ownership throughout). Errors flow through `Result`/`?`. No tokio here —
  async is chapter 27.

## Layout

```
24-time-timers-deadlines/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated behavioral check (clockprobe + two-peer scenario)
├── cpp/  go/  rust/   # one implementation each, same demo.sh contract
```

## The demo contract

Each language directory has a `demo.sh` with the identical interface used
book-wide: `./demo.sh build`, `./demo.sh run [args]`, and a bare `./demo.sh`
that builds then runs. With `TARGET` set, `run` deploys to that lab VM — but
this chapter runs purely on the host (loopback), so `TARGET` is unused here.

Try it by hand:

```
# terminal 1 — passive peer on an ephemeral port
./demo.sh cpp run listen  --name A --addr 127.0.0.1:9000 --heartbeat-ms 500 --timeout-ms 1500 --message "hi from A"
# terminal 2 — active peer; ^C peer A and watch B time out then back off
./demo.sh cpp run connect --name B --peer A@127.0.0.1:9000 --heartbeat-ms 500 --timeout-ms 1500 --message "hi from B"
```

## Verification

`verify.lua` (run by `scripts/test-all-examples.py` with `LSP_LANG` set)
asserts observable behavior, not exit 0: that `clockprobe` prints all three
resolution fields with parseable ns/µs on a `CLOCK_MONOTONIC` line (never
`CLOCK_REALTIME`), that two loopback peers link and deliver a message each
way, that killing the listener makes the connector print `peer A timed out`
followed by two-or-more growing, jittered `reconnecting to A in <ms>ms` lines,
and that a `SIGTERM` shuts the connector down cleanly (exit 0). Exit 0 = pass,
1 = fail, 77 = skip.
