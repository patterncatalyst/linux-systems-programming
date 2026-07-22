---
title: "r09 batch 1 — stub completion for examples 37 and 40"
layout: plan
render_with_liquid: false
published: false
---

# r09 batch 1 — stub completion for examples 37 and 40

Working plan for completing the Go, Rust, and `verify.lua` stubs in
`37-use-method-60s-analysis` and `40-low-latency-fastpath`. The C++
implementation in each is the finished reference; Go and Rust conform to the
behavioral contract derived from it.

Recorded here so the work survives an interrupted session. Checkpoint commit
after every step.

## Approach

Golden-capture first, `verify.lua` second, ports last. Run the already-built
C++ binaries on `systems-target`, capture exact stdout/stderr as the oracle,
write `verify.lua` against that oracle and prove it passes with `LSP_LANG=cpp`
before a single line of Go or Rust exists. Then port one language at a time,
gating each on the same unchanged `verify.lua`. Dependencies are chosen to be
satisfiable from the existing `~/.cargo` and `~/go/pkg/mod` caches so no build
needs the network.

Rejected alternatives:

- **Write all three languages then verify** — with `mode: vm` and one shared
  guest, a failing assertion gives three suspects instead of zero. The
  C++-passing gate makes `verify.lua` itself trustworthy first.
- **A new driver script for 37** mirroring 40's `run-latency-bench.sh` — 37
  needs one thing that script provides (a background load generator running
  concurrently with `analyze`), and that fits in a single compound `ssh`
  command, the shape example 33 already uses.
- **Driving 37/40 through `demo.sh <lang> run` under `TARGET`** —
  `deploy-to-vm.sh` scp's to a fixed `/home/fedora/app` and uses `ssh -t`. Two
  concurrent invocations race on that path, and the pty injects CRLF that
  breaks Lua patterns. `verify.lua` stages to `/tmp/lsp37-<lang>/app` with
  plain `ssh` instead.
- **The `core_affinity` crate** already declared in 40's `Cargo.toml` — not in
  the local cargo cache, so it needs network. `nix 0.31.3` is cached and
  provides `sched::sched_setaffinity` and `CpuSet`.

## Behavioral contract — example 37 (`loadmix`)

Program name in usage strings is literally `app`. All normal output on stdout;
usage on stderr.

### CLI

| Invocation | Behavior |
|---|---|
| `app --resource cpu\|mem\|io\|net --seconds N` | saturate; exit 0 |
| `app analyze [--seconds N]` | checklist; exit 0; `N` default 60 |
| anything else, unknown flag, missing value, non-integer or non-positive `--seconds`, bad or missing `--resource` | usage on stderr, exit 2 |

Usage is exactly two lines on stderr:

```
usage: app --resource cpu|mem|io|net --seconds N
       app analyze [--seconds N]
```

`analyze` is recognized only as `args[0]`. In saturate mode any token that is
not `--resource` or `--seconds` followed by a value is a usage error.

### Saturate mode

`cpus = hardware_concurrency()` (0 becomes 1). Worker count:

- `mem` — `max(2, cpus)`
- `cpu` — `max(4, cpus * 3)`
- `io`, `net` — `max(4, cpus * 4)`

Two stdout lines, in order:

```
loadmix: start resource={res} seconds={n} workers={w}
loadmix: done resource={res} seconds={n} workers={w} ops={u64} bytes={u64}
```

Per-resource semantics, with the meaning of `ops` and `bytes`:

- **cpu** — `w` threads run xorshift64 until a `steady_clock` deadline,
  checking the clock every 2^20 iterations. `ops` is total iterations; `bytes`
  is the sum of each thread's final state word (a liveness checksum, not a byte
  count).
- **mem** — reads `MemTotal:` kB from `/proc/meminfo`;
  `target = MemTotal_kB * 1024 * 1.35` (fallback 2 GiB if unreadable);
  `chunk = max(target/w, 65536)`; each worker allocates a `chunk`-byte buffer
  and increments one byte every 4096 until the deadline. `ops` is total page
  touches; `bytes` is `target`.
- **io** — `mkdir /var/tmp/loadmix-io-<pid>` (0700); each worker opens
  `w<i>.dat` with `O_WRONLY|O_CREAT|O_TRUNC|O_DSYNC` (0600) and `pwrite`s a
  16384-byte xorshift64\*-filled buffer at offset 0 repeatedly (the file stays
  16 KiB; the random fill defeats btrfs zstd compression); then `close`,
  `unlink`, and `rmdir`. `ops` is write count; `bytes` is bytes written.
- **net** — one `AF_INET/SOCK_DGRAM` receiver bound to `127.0.0.1:0` with
  `SO_RCVTIMEO` 200 ms; `w` sender threads `connect` to that port and `send` a
  64-byte zero payload until the deadline; the receiver drains until stopped.
  `ops` is packets sent; `bytes` is bytes sent.

### Analyze mode

Exact line sequence on stdout:

```
analyze: start seconds={n} cpus={cpus}
analyze: tool uptime raw="{trimmed uptime output, or "unavailable"}"
analyze: tool dmesg status={ok|denied}
analyze: tool top cpu="{trimmed line starting %Cpu, else empty}" tasks="{trimmed line starting Tasks:, else empty}"
analyze: tool ss raw="{trimmed first line of `ss -s`}"
analyze: tool pidstat active_processes={count}
analyze: metric resource=cpu name=busy_pct value={:.2f} unit=pct
analyze: metric resource=cpu name=run_queue value={:.2f} unit=procs
analyze: metric resource=mem name=used_pct value={:.2f} unit=pct
analyze: metric resource=mem name=swap_io value={:.2f} unit=kbps
analyze: metric resource=io name=util_pct value={:.2f} unit=pct
analyze: metric resource=io name=iowait_pct value={:.2f} unit=pct
analyze: metric resource=io name=await_ms value={:.2f} unit=ms
analyze: metric resource=net name=pkts value={:.2f} unit=per_s
```

then, for each resource in the fixed order cpu, mem, io, net, three lines:

```
analyze: signal resource={r} type=Utilization fired={true|false} value={:.2f} threshold={:.2f}
analyze: signal resource={r} type=Saturation fired={true|false} value={:.2f} threshold={:.2f}
analyze: signal resource={r} type=Errors fired=false
```

then:

```
analyze: verdict resource={r} ratio={:.2f}
analyze: done
```

Timing and subprocess rules:

- Every subprocess runs under `env LC_ALL=C LANG=C`, wrapped so `2>&1`
  captures the whole pipeline: `( env LC_ALL=C LANG=C dmesg | tail -n 5 ) 2>&1`.
- `uptime`, `dmesg | tail -n 5`, `top -b -n1`, and `ss -s` are captured
  **before** the interval samplers.
- The five interval samplers run **concurrently**, each redirected to its own
  file under `/tmp/loadmix-analyze-<pid>/`, then all are waited on and the
  directory removed. Wall clock is about N seconds, not 5N. Commands:
  `vmstat 1 N`, `mpstat -P ALL 1 N`, `iostat -xz 1 N`, `sar -n DEV 1 N`,
  `pidstat 1 N`.
- `free -m` is captured **after** the samplers.
- `dmesg` status is `denied` if the capture failed or the output contains
  `Operation not permitted`.

Parsing rules — all by column *name*, read from each tool's own header row,
never a fixed index:

- **vmstat** — header is the token row containing `swpd`; data rows are those
  with `len(tokens)+2 >= len(header)`; **skip data row 0** (the since-boot
  average); `run_queue_max = max(r)`, `swap_io_max = max(si+so)`.
- **mpstat** — use the `Average:` / `CPU` header row and the `Average:` / `all`
  data row, both with the first 2 tokens dropped; `busy_pct = 100 - %idle`
  (`%idle` defaults to 100), `iowait_pct = %iowait` (defaults to 0).
- **free -m** — header row is the one containing `available`; the data row
  starts with `Mem:` with 1 token dropped (header drops 0);
  `used_pct = (total-available)/total*100`, or 0 if `total <= 0`.
- **iostat -xz** — block index increments on each `avg-cpu:` line; header is the
  `Device` row; **skip block 0**; skip devices whose name starts with `zram`,
  `sr`, `loop`, or `dm-`; drop 1 token from header and row;
  `util_max = max(%util)`, `await_max = max(w_await)`.
- **sar -n DEV** — `Average:`/`IFACE` header and `Average:`/`lo` data row, 2
  tokens dropped each; value is `rxpck/s + txpck/s`.
- **pidstat** — `active_processes` is the count of lines whose first token is
  `Average:` and whose second token is not `UID`.
- Any missing header or data row yields the documented default (0, or 100 for
  `%idle`); a non-numeric field is treated as absent and falls back to the
  default.

Thresholds and verdict, all fixed:

| resource | Utilization value / threshold | Saturation value / threshold |
|---|---|---|
| cpu | `busy_pct` / 60.0 | `run_queue_max` / `cpus + 2.0` |
| mem | `mem_used_pct` / 75.0 | `swap_io_max` / 300.0 |
| io | iostat `%util` max / 40.0 | mpstat `%iowait` / 8.0 |
| net | sar `lo` pkts/s / 2000.0 | same pkts/s / 8000.0 |

`fired = value >= threshold`. The verdict is the resource with the greatest
`saturation_value / saturation_threshold`, scanned cpu → mem → io → net with a
strict `>` so ties keep the earlier resource; `ratio` is that maximum. `Errors`
is always `fired=false` by design — this example induces load, not
kernel-logged faults.

> The C++ header comment on line 15 uses a word banned by `CLAUDE.md`. Do not
> carry it into the Go or Rust headers; write "accurate" or "a real checklist
> run" instead.

## Behavioral contract — example 40 (`chatterd-fastpath`)

Usage is three lines on stderr; exit 2 on any parse failure, unknown
subcommand, or empty argv:

```
usage: app fastpath --port P --pin CPU [--busy-poll]
       app naive --port P
       app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]
```

### Wire frame — fixed 64 bytes, nothing variable-length

| offset | size | field |
|---|---|---|
| 0 | 1 | magic0 `0x43` (`'C'`) |
| 1 | 1 | magic1 `0x46` (`'F'`) |
| 2 | 1 | version `0x01` |
| 3 | 1 | type `0x01` (echo) |
| 4 | 8 | `seq`, big-endian u64 |
| 12 | 8 | `send_ns`, big-endian u64 — the client always writes 0 |
| 20 | 44 | zero padding |

Servers never inspect `seq` or `send_ns`; they read exactly 64 bytes and write
the identical 64 bytes back.

### `fastpath --port P --pin CPU [--busy-poll]`

The order of operations is observable and matters:

1. **Pin first**, before binding. Range check:
   `cpu < 0 || cpu >= sysconf(_SC_NPROCESSORS_ONLN)` gives stderr
   `app: error: cpu {c} out of range (0..{nproc-1})`, exit 1.
   `sched_setaffinity` failure gives
   `app: error: sched_setaffinity({c}): {strerror}`, exit 1.
2. Listen: `SO_REUSEADDR`, bind `INADDR_ANY:P`, backlog 16. Failures give
   `app: error: socket: …`, `app: error: bind 0.0.0.0:{P}: …`, or
   `app: error: listen: …`, exit 1.
3. Install non-restarting SIGINT and SIGTERM handlers — deliberately not
   `SA_RESTART`.
4. stderr:
   `app: fastpath listening on 0.0.0.0:{P} pinned-cpu={C} busy-poll={on|off}`
5. Accept loop while not stopped: `poll(listener, POLLIN, 200ms)`; `EINTR`
   continues; a poll error gives `app: error: poll: {strerror}`, exit 1; a
   timeout or no `POLLIN` continues. On accept, set `TCP_NODELAY`; with
   `--busy-poll`, set the connection `O_NONBLOCK`. Accept `EINTR` or `EAGAIN`
   continues; any other accept error gives `app: error: accept: {strerror}`,
   exit 1.
6. Echo loop over **one** 64-byte stack buffer reused for the connection's whole
   life — zero heap allocation in the loop. Blocking mode reads until 64 bytes;
   any error, EOF, or EINTR breaks. Busy-poll mode spins on non-blocking `recv`
   over `EAGAIN`/`EWOULDBLOCK` with no `sched_yield` or `nanosleep`,
   re-checking the stop flag on each `EAGAIN`. Then write all 64 bytes back,
   retrying on `EINTR`; on any other write error, give up on the connection.
7. On stop: stderr `app: fastpath shutting down`, exit 0. One connection is
   served at a time, to completion.

### `naive --port P`

Identical to fastpath minus pinning and busy-poll, plus the deliberate
pessimization: **a fresh heap buffer allocated and freed per message**.
Announce line `app: naive listening on 0.0.0.0:{P}`; shutdown line
`app: naive shutting down`; exit 0. Its affinity mask is left untouched (all
online CPUs) — `run-latency-bench.sh` asserts this at the kernel level.

### `measure --target HOST:PORT --n N [--warmup W] [--tag TAG]`

- `HOST` is split at the **last** `:` and must be an IPv4 dotted quad;
  otherwise stderr `app: error: {host}: not an IPv4 address`, exit 1. A
  `connect` failure gives `app: error: connect {host}:{port}: {strerror}`,
  exit 1.
- `--warmup` defaults to 200. `--n` is required and must be greater than 0; the
  port must be 1..65535; all parsing is strict full-string unsigned, so
  `--n 5x` is a usage error with exit 2.
- `TCP_NODELAY` on the client socket.
- stdout, first line:
  `app: measure target={host}:{port} n={N} warmup={W}`
- `N + W` synchronous round trips, never more than one frame in flight. Per
  iteration: build the frame with `seq = i` and `send_ns = 0`; take
  `t_send` from `CLOCK_MONOTONIC`; write 64; blocking-read exactly 64; take
  `t_recv`. A read failure gives stderr
  `app: error: measure: connection closed at iteration {i}`, exit 1. A header
  mismatch or an echoed `seq != i` gives
  `app: error: measure: malformed/mismatched echo at iteration {i}`, exit 1.
  Samples are recorded only when `i >= W`.
- Percentile over the ascending-sorted samples: `idx = ceil(p/100 * n)`, clamped
  to `[1, n]`, returning `sorted[idx-1]` — 1-based nearest-rank, to be
  reproduced exactly.
- Output tail, exact:

```
app: percentiles_ns tag={TAG or -} p50={} p90={} p99={} p99.9={} min={} max={} mean={:.2f} n={}
app: table
  p50    {} ns
  p90    {} ns
  p99    {} ns
  p99.9  {} ns
  min    {} ns
  max    {} ns
  mean   {:.2f} ns
```

Indentation is literal: two leading spaces, then the label padded so values
start at column 10 (`p50` + 4 spaces, `p90` + 4, `p99` + 4, `p99.9` + 2,
`min` + 4, `max` + 4, `mean` + 3). Exit 0.

### `run-latency-bench.sh`

Complete and language-agnostic — **do not rewrite it**. It takes
`/path/to/app [N=20000] [WARMUP=500]` and is staged onto the guest by
`verify.lua`, exactly as example 33 stages `run-sandbox-checks.sh`. It emits:

```
app: nproc={} pin_cpu={1|0} client_cpu={0}
app: naive-cpus-allowed {Cpus_allowed_list}      # phase A, expect the full set
app: fastpath-cpus-allowed {Cpus_allowed_list}   # phase A, expect exactly PIN_CPU
app: === naive measure trial k/3 ===             # phase B, k = 1..3, interleaved
app: === fastpath measure trial k/3 ===
```

Ports 19501/19502 in phase A, 19503/19504 in phase B. Phase B externally
`taskset`s both servers to `PIN_CPU` and the client to `CLIENT_CPU`, so the
only variable is in-server discipline. Exit 0 on success; if a server never
comes up it prints `app: error: phase A/B servers did not come up` and exits 1.
Trials are interleaved and tagged `naive-k` / `fastpath-k` precisely so
`verify.lua` can group by tag and compare medians of three, never a single
sample.

### Cross-language mechanism notes

Behavior identical, mechanism idiomatic.

- **Go 40** — raw fds via `golang.org/x/sys/unix` (already at v0.47.0 in
  `go.mod`/`go.sum`) for socket, bind, listen, accept4, read, recv, write, and
  setsockopt, so busy-poll bypasses the netpoller. Shutdown uses
  `signal.Notify` plus a goroutine that sets an atomic and calls `shutdown(2)`
  on the listener and the live connection fd — the ch21 pattern the C++ header
  itself cites — because Go's runtime installs `SA_RESTART` handlers and you
  cannot rely on EINTR. Pinning needs `runtime.LockOSThread()` in `main` **and**
  `unix.SchedSetaffinity(tid, &set)` for every tid in `/proc/self/task`, since
  new threads inherit the mask and phase A reads `/proc/<pid>/status`, which
  reports the thread-group leader.
- **Rust 40** — single-threaded; `nix 0.31` with features
  `["signal", "sched", "poll"]` (cached locally) for `sigaction` without
  `SA_RESTART`, `sched_setaffinity`/`CpuSet`, and `poll`. `std::net` for the
  sockets, `set_nodelay` and `set_nonblocking` from std.
  `ErrorKind::Interrupted` is the EINTR path. **Remove `core_affinity = "0.8"`
  from `Cargo.toml`** — not in the local cargo cache, so it would force a
  network fetch — and regenerate `Cargo.lock`, which is currently the stale
  template lock naming `template-hello` and `rustix`.
- **Go 37** — `os/exec` for captures, `runtime.NumCPU()`, `unix.Open` plus
  `unix.Pwrite` with `unix.O_DSYNC`, a `net.UDPConn` with `SetReadDeadline` for
  the receiver, and goroutines with `sync.WaitGroup` and atomic counters.
- **Rust 37** — `std::process::Command`,
  `std::thread::available_parallelism()`, `std::thread::scope` (the `jthread`
  analogue), `AtomicU64`, `std::net::UdpSocket::set_read_timeout`, and
  `OpenOptions::custom_flags(libc::O_DSYNC)` with `FileExt::write_at` for the
  pwrite path. Only new dependency is `libc = "0.2"` (cached). Rename the
  package from `template-hello` to `loadmix`; keep `[[bin]] name = "app"`.

## Steps

Concurrency legend: **[VM]** touches `systems-target` and is strictly serial
against every other [VM] step; **[host]** is local. Never two
`cargo build --release` at once, and never a cargo build concurrent with a
cmake build.

**S1 [VM] — golden capture, example 37.** Stage the already-built
`examples/37-use-method-60s-analysis/cpp/build/release/app` to
`/tmp/lsp37-cpp/app` via `scp`. Record verbatim: no-args, `analyze --seconds`,
`--resource bogus`, `--seconds 0` (all exit-2 shapes); `--resource cpu
--seconds 5`; `--resource io --seconds 5`; `--resource net --seconds 5`;
`analyze --seconds 10` idle; and a compound
`nohup app --resource cpu --seconds 25 & sleep 2; app analyze --seconds 12`.
Record the guest's `nproc`. **Determine empirically whether `--resource mem`
survives** on this guest — it deliberately targets 1.35 × MemTotal and may be
OOM-killed; the answer decides whether `verify.lua` asserts `mem` exit 0 or only
its argument acceptance. No dependencies; read-only.

**S2 [VM] — golden capture, example 40.** Stage
`examples/40-low-latency-fastpath/cpp/build/release/app` and
`run-latency-bench.sh` to `/tmp/lsp40-cpp/`. Record: usage and exit-2 shapes;
`measure --target 127.0.0.1:1 --n 10` (connect failure, exit 1);
`measure --target notanip:9999 --n 10`; `fastpath --port P --pin 99`
(out of range, exit 1); a full
`bash run-latency-bench.sh /tmp/lsp40-cpp/app 20000 500` run; and a SIGINT and
SIGTERM shutdown of both servers checking exit 0 and the shutdown lines. Record
`naive-cpus-allowed`, `fastpath-cpus-allowed`, and the three-trial p50 medians
for both variants — the observed margin sets the verify threshold. Depends on
S1 (single guest).

**S3 [host+VM] — write `examples/37-use-method-60s-analysis/verify.lua`.**
Shape it on `examples/33-seccomp-and-landlock/verify.lua` (skip unless
`LSP_LANG` is valid; skip unless `TARGET` is set; resolve the IP via
`scripts/lab/vm-ip.sh`; skip if unresolvable) plus the `expect_true` helper
from `examples/39-benchmarking-without-lies/verify.lua`. Assertions:

1. `./demo.sh <lang> build` exits 0.
2. Stage the binary to `/tmp/lsp37-<lang>/app` — named `app` so `/proc` comm is
   `app`, the book-wide rule — and assert staging exits 0.
3. Error shapes: no args, `--resource bogus --seconds 5`,
   `--resource cpu --seconds 0`, `analyze --bogus` all exit 2 with
   `usage: app %-%-resource`.
4. Saturate `cpu`, `io`, and `net` with `--seconds 4`: exit 0,
   `loadmix: start resource=%s seconds=4 workers=%d+`,
   `loadmix: done … ops=%d+ bytes=%d+`, the worker count equals the documented
   formula against the guest's `nproc`, and `ops > 0`.
5. Idle `analyze --seconds 8`: exit 0; all 8 metric lines present with a
   `%d+%.%d%d` value; all 12 signal lines present in cpu/mem/io/net × U/S/E
   order; `analyze: verdict resource=%a+ ratio=%d+%.%d%d`; `analyze: done` last;
   `analyze: tool pidstat active_processes=%d+`;
   `analyze: tool dmesg status=%a+`; `analyze: start seconds=8 cpus=%d+`.
6. **Load attribution** — one compound `ssh` with a background saturator and a
   foreground analyze: under `--resource cpu`, both `cpu busy_pct` and
   `cpu run_queue` rise materially above the idle values,
   `signal resource=cpu type=Utilization fired=true`, and
   `verdict resource=cpu`. Retry up to 3 attempts like example 36's cpu-busy
   check, gating on `busy_pct >= 60` **or** `run_queue >= nproc+2` so a noisy
   guest does not produce a false red.
7. Wall-clock sanity: `analyze --seconds 8` completes well under 5×8 s, proving
   the samplers really run concurrently.
8. Cleanup `rm -rf /tmp/lsp37-<lang>` on the guest; `checks.finish()`.

Then run `LSP_LANG=cpp TARGET=systems-target REPO_ROOT=… lua verify.lua` and
require a clean pass. Depends on S1.

**S4 [host+VM] — write `examples/40-low-latency-fastpath/verify.lua`.** Same
skeleton. Stage `app` **and** `run-latency-bench.sh` to `/tmp/lsp40-<lang>/`,
`chmod 755` both. Assertions:

1. Build and stage exit 0.
2. Error shapes: no args, `frobnicate`, `fastpath --port 9999` (no `--pin`),
   `naive` (no port), `measure --target 127.0.0.1:9999` (no `--n`) all exit 2
   with `usage: app fastpath`.
3. `fastpath --port P --pin <nproc+50>` exits 1 with
   `app: error: cpu %d+ out of range`.
4. `measure --target 127%.0%.0%.1:1 --n 10` exits 1 with `app: error: connect`;
   `measure --target nothost:9999 --n 5` exits 1 with `not an IPv4 address`.
5. `bash run-latency-bench.sh /tmp/lsp40-<lang>/app 20000 500` exits 0, and from
   its output `fastpath-cpus-allowed` is exactly the pinned CPU (a single value,
   no range or comma) while `naive-cpus-allowed` is the full set — the
   kernel-level pinning proof, not the program's own claim.
6. All six `app: percentiles_ns tag=(naive|fastpath)-%d …` lines parse; each has
   `n` equal to the requested 20000; `min <= p50 <= p90 <= p99 <= p99.9 <= max`;
   `mean` parses as a float; every value is greater than 0.
7. Median-of-three per variant: `median(fastpath p50) < median(naive p50)` with
   the margin calibrated from S2, printing both medians unconditionally so a
   marginal run is diagnosable from the log.
8. Separately: start `naive --port P` backgrounded over ssh, wait for the port,
   send SIGINT, assert `app: naive shutting down` and exit 0; repeat for
   `fastpath --port P --pin 0 --busy-poll` asserting
   `app: fastpath listening on 0%.0%.0%.0:%d+ pinned%-cpu=0 busy%-poll=on`,
   `app: fastpath shutting down`, and exit 0. This is the assertion Go is most
   likely to fail, so it must be explicit.
9. Cleanup; `checks.finish()`. Run with `LSP_LANG=cpp` and require a clean pass.

Depends on S2, and S3's VM runs must be finished (serial guest).

**S5 [host, then VM] — Go port of 37.** Rewrite
`examples/37-use-method-60s-analysis/go/main.go` against the contract above.
`go.mod` already pins `go 1.26` and `toolchain go1.26.5` and requires
`golang.org/x/sys v0.47.0` with a matching `go.sum` — do not change them. The
build is host-local and cheap; the verify run is [VM] and serial. Gate:
`LSP_LANG=go` passes the unchanged S3 `verify.lua`. Depends on S3.

**S6 [host, then VM] — Rust port of 37.** Rewrite
`examples/37-use-method-60s-analysis/rust/src/main.rs`; edit `Cargo.toml`
(`name = "loadmix"`, add `libc = "0.2"`, keep `edition = "2024"` and
`[[bin]] name = "app"`); regenerate `Cargo.lock`. Leave `rust-toolchain.toml` at
1.97.1. **This cargo build runs alone** — no concurrent cmake or cargo. Gate:
`LSP_LANG=rust` passes S3's `verify.lua`. Depends on S5.

**S7 [host, then VM] — Go port of 40.** Rewrite
`examples/40-low-latency-fastpath/go/main.go` per the raw-fd, `shutdown(2)`,
pin-all-tids notes. `go.mod` and `go.sum` unchanged. Gate: `LSP_LANG=go` passes
S4's `verify.lua`, including the phase-A single-CPU assertion and the SIGINT
assertions. Depends on S4 and S6.

**S8 [host, then VM] — Rust port of 40.** Rewrite
`examples/40-low-latency-fastpath/rust/src/main.rs`; fix `Cargo.toml` (drop
`core_affinity`, set
`nix = { version = "0.31", features = ["signal","sched","poll"] }`, keep
`anyhow`); regenerate the stale `Cargo.lock`. **Alone on the machine.** Gate:
`LSP_LANG=rust` passes S4's `verify.lua`. Depends on S7.

**S9 [VM] — whole-runner gate.**
`python3 scripts/test-all-examples.py --mode vm --only 37-use-method-60s-analysis --only 40-low-latency-fastpath --jobs 1`.
VM examples are already strictly serial in the runner. Record the result here
per the verification discipline. Depends on S8.

Safe to run concurrently: editing and reading files at any time; a Go build
alongside prose edits. Must be sequenced: every VM step against every other VM
step (one guest, and 40's bench pins vCPUs and would corrupt 37's USE
measurements); S6 and S8 against each other and against any cmake build.

Out of scope but flagged: both `README.md` files are still verbatim `_template`
hello-syscall text.

## Acceptance criteria

1. `python3 scripts/test-all-examples.py --mode vm --only 37-use-method-60s-analysis --only 40-low-latency-fastpath`
   reports PASS for all six example × language pairs, with the lab up.
2. Neither `verify.lua` is under 60 lines, and each has at least 15
   `checks.expect_*` assertions; neither can pass against a binary that merely
   exits 0, provable by pointing `demo.sh` at `/bin/true` and observing failure.
3. For 37, for each of `cpu`, `io`, `net` at `--seconds 4`, all three languages
   print `loadmix: start` and `loadmix: done` with an identical `workers=` value
   for the same guest and `ops > 0`.
4. For 37 `analyze`, all three languages emit the identical 25-line skeleton in
   the identical order — 1 start, 5 tool, 8 metric, 12 signal, verdict, done —
   with every metric value formatted to exactly 2 decimal places.
5. For 37 under concurrent `--resource cpu` load, all three languages report
   `signal resource=cpu type=Utilization fired=true` and `verdict resource=cpu`
   within 3 attempts.
6. For 40, `run-latency-bench.sh` exits 0 for all three languages, phase A
   reports a single CPU in `fastpath-cpus-allowed` and the full online set in
   `naive-cpus-allowed`, and all six `percentiles_ns` lines have `n=20000` with
   monotonic `min <= p50 <= p90 <= p99 <= p99.9 <= max`.
7. For 40, `median(fastpath p50) < median(naive p50)` over three interleaved
   trials, for each language.
8. For 40, SIGINT to a running `naive` and to a running `fastpath --busy-poll`
   produces the documented shutdown line and exit status 0, in all three
   languages.
9. For both examples, all exit-2 usage paths and all exit-1 error paths produce
   byte-identical message text across the three languages, modulo `strerror`
   wording, which is identical on the same guest.
10. Every build succeeds with no network access: `go build` resolves from
    `~/go/pkg/mod` and `cargo build --release` from `~/.cargo/registry`,
    verifiable with `--offline`; `go.mod` still says `go 1.26` and
    `toolchain go1.26.5`; both `rust-toolchain.toml` still say
    `channel = "1.97.1"`; both `Cargo.toml` still say `edition = "2024"` with
    `[[bin]] name = "app"`.
11. `python3 scripts/validate.py` passes — it `bash -n`s every `.sh` under
    `examples/`.
12. No new files are created. The only files changed are the four language
    sources, four Rust manifest and lock files, and the two `verify.lua`.
13. No Go or Rust source contains any word banned by `CLAUDE.md`.

## Risks

- **37 `--resource mem` OOM-kills the guest.** It deliberately targets 1.35 ×
  MemTotal and touches every page. Detected in S1 by a non-zero or signal exit
  from the C++ reference. If it does, `verify.lua` must not assert exit 0 for
  `mem` — assert only the shared start/done line shape at a very short
  duration, or omit `mem` from the saturate loop and document why in the
  `verify.lua` header. The silent-failure mode is the guest dying mid-suite and
  later examples failing for unrelated reasons; mitigate by running `mem` last,
  if at all.
- **Go's `/proc/<pid>/status` affinity mask is the thread-group leader's**, not
  the hot loop's. If the Go port sets affinity only on the `LockOSThread`'d
  goroutine, phase A can still print the full CPU set — or worse, print the
  right answer by luck on one run and the wrong one on another. Detected by
  criterion 6; mitigated by setting affinity for every tid in
  `/proc/self/task`.
- **Go's blocking reads do not return EINTR** because the runtime installs
  `SA_RESTART`. A Go server copying the C++ signal design will hang forever on
  SIGINT with an idle connection open, and the verify assertion will time out
  rather than fail cleanly. Mitigated by the explicit SIGINT assertion in S4
  step 8 and the `shutdown(2)` design; the failure mode is a runner timeout, so
  read a `VERIFY_TIMEOUT` as this bug first.
- **The fastpath-beats-naive margin is not guaranteed** on a 2-vCPU nested-KVM
  guest; `run-latency-bench.sh`'s own comments note a lone trial can hit a
  multi-hundred-microsecond stall. Calibrate the threshold from S2's real
  numbers, compare medians of three, and always print both medians. If Go's GC
  or netpoller makes the margin vanish, that is a genuine finding about the
  port, not a flake — distinguish by running the bench three times manually.
- **Cargo re-resolution needs the network.** Both `Cargo.lock` files are stale
  template locks, and changing `[dependencies]` forces re-resolution.
  `nix 0.31.3`, `libc`, `anyhow`, `bitflags`, `cfg-if`, and `memoffset` are
  cached; `core_affinity` is not. Detect early with `cargo fetch --offline`
  before writing any Rust; the symptom otherwise is a build failure deep in S6
  or S8 after the expensive work is done.
- **`deploy-to-vm.sh` uses `ssh -t` and a fixed `/home/fedora/app` path.** If
  `verify.lua` takes the `demo.sh run` path instead of staging directly, CRLF
  from the pty silently breaks Lua patterns that look correct, and concurrent
  invocations race on the binary. Mitigated by the staging design; detected as
  "pattern did not match" on output that looks right in the log.
- **Locale and sysstat column drift.** The C++ forces `LC_ALL=C LANG=C` around
  every capture and parses by column name. A port that hardcodes indices or
  omits the locale forcing will pass on this guest and break on any other. Not
  detectable by `verify.lua` on a single guest — enforce by code review against
  the parsing rules above.
- **Concurrency and OOM on the host.** A previous wide fan-out was already
  OOM-killed and two lab VMs are running. Two `cargo build --release` at once,
  or a cargo build alongside a cmake build, can repeat that. Sequence S6 and S8
  strictly; the symptom is a build killed by signal 9 with no compiler
  diagnostic.
- **Rounding divergence in `{:.2f}`.** C++, Go, and Rust do not agree on
  half-way cases. Verify must match `%d+%.%d%d` shapes and numeric ranges, never
  exact decimal strings compared across languages.

## Port findings — example 40 (S7/S8)

Recorded from the Go and Rust ports of `chatterd-fastpath`. Two of the three
were pre-empted by the Risks section above; the third was not, and is the one
worth carrying into chapter 40's prose.

### 1. Go shutdown — the `SA_RESTART`/netpoller divergence (predicted, Risk 3)

Confirmed exactly as Risk 3 warned. The C++ reference unblocks an in-flight
`read(2)`/`poll(2)` by installing a **non-`SA_RESTART`** handler so the syscall
returns `EINTR` and the loop notices the stop flag. That mechanism is
unavailable to Go: the Go runtime installs its own `SA_RESTART` handlers, and a
"blocking" read on a `net.Conn` never sits in a syscall to begin with — the
netpoller parks the goroutine on an epoll registration. No signal makes that
read return `EINTR`.

The Go port therefore reaches identical observable behavior by a different
mechanism: a `signal.Notify` goroutine sets the stop flag and then **closes the
listener and the live connection**. Closing is what wakes a parked netpoller
read — the `shutdown(2)`-driven wakeup the C++ header itself cites from ch21.
The accept loop additionally carries a 200ms deadline mirroring the C++ `poll`
timeout, so shutdown stays bounded even if a close races an in-flight accept.
The busy-poll path takes a dup'd fd out of the netpoller (`TCPConn.File()` +
`O_NONBLOCK`) and is woken by `shutdown(SHUT_RDWR)` on that raw fd. Verified: the
SIGINT assertion (the one Risk 3 flagged Go was most likely to fail) passes,
`PASS 28 / FAIL 0`.

### 2. Go pinning proof — the `/proc` thread-group leader (predicted, Risk 2)

As Risk 2 warned, `sched_setaffinity` on a goroutine is meaningless (goroutines
migrate between Ms). The port pins with `runtime.LockOSThread` + `GOMAXPROCS(1)`
to nail the hot loop to one M, then calls `SchedSetaffinity` **twice** — once
with pid 0 (the calling thread, which is what actually restricts the loop) and
once with the process pid (the thread-group leader, which is what
`/proc/<pid>/status` reports and phase A reads). This is a lighter touch than
the plan's "every tid in `/proc/self/task`" note and was sufficient: phase A
prints `fastpath-cpus-allowed 1` (single CPU) and `naive-cpus-allowed 0-1` (full
set), the kernel-level proof, on every run.

### 3. Go range check — `runtime.NumCPU()` masked by `taskset` (NOT predicted)

**This is the novel finding — nothing in the Risks section anticipated it.** The
bench driver's phase B launches the server under `taskset -c 1`. The C++ range
check uses `sysconf(_SC_NPROCESSORS_ONLN)`, which reports CPUs online
**system-wide** (2 on the guest) and is blind to the affinity mask, so a pin to
CPU 1 is in range. Go's `runtime.NumCPU()` returns the size of the process's
**affinity mask** — under `taskset -c 1` that is 1 — so the port's own range
check rejected the pin to CPU 1 with `app: error: cpu 1 out of range (0..0)`,
and the whole phase-B timing comparison failed to start.

This is the same class of bug as `hardware_concurrency()` lying about its
container (the running theme of this part), but reached through an unexpected
door: a `taskset` affinity mask rather than a cgroup quota. The count that
matters for a *range check on absolute CPU indices* is the system-wide online
count, not the caller's affinity-masked view. The fix is `onlineCPUs()`, which
parses `/sys/devices/system/cpu/online` to match the C++ `sysconf` semantics.
C++ and Rust never hit this because both use `sysconf(_SC_NPROCESSORS_ONLN)`
directly.

### 4. Rust — the port that follows C++ verbatim

Unlike Go, Rust makes real blocking syscalls (no M:N runtime parking work off
the syscall), so the C++ non-`SA_RESTART` → `EINTR` → notice-the-stop-flag
technique works **verbatim**. The catch is that std's higher-level wrappers hide
`EINTR` — `TcpListener::accept` retries it internally (`cvt_r`) and
`Read::read_exact` treats `ErrorKind::Interrupted` as "try again" — so the
servers are built on raw fds via `nix` (`OwnedFd` as the RAII `Socket`), where
every `EINTR` is handled explicitly. Pinning is a single `sched_setaffinity(0,
…)`: the process is single-threaded (tid == pid), so one call both pins the hot
loop and shows up in `/proc/<pid>/status` — no `LockOSThread`/`GOMAXPROCS` dance.
`Cargo.toml` dropped `core_affinity`/`anyhow`/`rustix`; `nix` now carries
`socket`/`net`/`poll`/`sched`/`signal`/`fs`. Verified `PASS`, median(naive
p50)=12063 ns vs median(fastpath p50)=7747 ns.

### Whole-example gate

`test-all-examples.py --only 40-low-latency-fastpath --mode vm` — all three
languages PASS in one runner pass; per-run margins cpp 1.47x, go 1.62x, rust
1.51x, all clearing the `verify.lua` 0.90-factor gate.

## Progress

| Step | Status |
|---|---|
| S1 golden capture 37 | done — see `r09-batch1-golden-capture.md` |
| S2 golden capture 40 | done — see `r09-batch1-golden-capture.md` |
| S3 verify.lua 37 | done — 50 assertions, clean pass with `LSP_LANG=cpp` (`PASS 50 / FAIL 0`); `/bin/true` negative control confirmed FAIL (`PASS 9 / FAIL 41`) |
| S4 verify.lua 40 | done — clean pass with `LSP_LANG=cpp` (`PASS 46 / FAIL 0`), commit `4ccb916` |
| S5 Go port 37 | pending |
| S6 Rust port 37 | pending |
| S7 Go port 40 | done — `PASS 28 / FAIL 0`, commit `ba1387a` (see Port findings 1–3) |
| S8 Rust port 40 | done — `PASS`, commit `127c049` (see Port finding 4) |
| S9 runner gate | 40 done (all three languages PASS in one `--mode vm` pass); 37 pending |
