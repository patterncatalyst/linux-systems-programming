---
title: "/proc, /sys, and the agent: a USE-method snapshot read straight from the kernel, no tools required"
order: 36
part: "Observability"
description: "sysagent v0 is a USE-method metrics collector that reads /proc and /sys/fs/cgroup directly — no root, no shelled-out tools — computing CPU utilization from a two-read /proc/stat delta spanning --interval-ms, run-queue and load from /proc/loadavg, memory from /proc/meminfo, per-disk I/O from /proc/diskstats, network counters from /proc/net/dev, and cgroup PSI (falling back to system-wide /proc/pressure) gathered concurrently with the sampling sleep, exposed identically by a one-shot sample --json and a serve --port /metrics HTTP endpoint, with byte-for-byte identical field names across C++, Go, and Rust."
duration: "50 minutes"
---

Chapter 35's `overheadbench` measured syscall, memory, and I/O overhead across
a bare process, a container, and a VM — but every one of those numbers came
from watching `strace -c` and hand-timed loops wrapped around each run. That's
the right instrument for a one-off measurement campaign; it is not how a
program watches a system continuously, in production, for the months between
deploys. This chapter builds the tool that does: `sysagent`, a USE-method
metrics collector that reads `/proc` and `/sys/fs/cgroup` directly — no root,
no subprocess, no tool you'd have to trust is still installed on the box next
year. Chapter 37's `loadmix` builds the opposite kind of tool: an `analyze`
mode that shells out to `vmstat`, `mpstat`, `iostat`, `sar`, and `pidstat`
exactly the way an on-call engineer would, and parses their plain-text tables
by column name instead of reading kernel counters directly. The split between
the two chapters is deliberate — this one is the case for reading the
kernel's own accounting straight from the source, and the numbers it produces
are the ones a long-running agent can afford to compute every second without
forking a process to do it.

The code is in `examples/36-proc-sys-and-the-agent/`. The run script there
builds/sets up and runs it; its `README.md` covers what it does and how to
drive it.

{% include excalidraw.html
   file="36-proc-datasource-map"
   alt="A pipeline diagram of sysagent's take_snapshot. Left, a single /proc/stat read at t0 landing in a small box labeled 'cumulative counters since boot, not a percentage.' Center, a band labeled 'gathered concurrently with the --interval-ms sleep' containing five parallel boxes — /proc/loadavg, /proc/meminfo, /proc/diskstats, /proc/net/dev, and cgroup PSI (cpu.pressure/memory.pressure/io.pressure, falling back to /proc/pressure) — each writing into its own field of one shared Snapshot, all joined before the sleep's deadline is allowed to pass. A second /proc/stat read at t1 sits to the right of the band; arrows from both t0 and t1 feed a delta box computing cpu_util_pct/cpu_user_pct/cpu_system_pct as 100 times (busy1-busy0)/(total1-total0). A note reads 'two cumulative reads, --interval-ms apart, become one percentage.' Below, the completed Snapshot feeds two consumers drawn side by side: sample --json printing the object once to stdout, and serve --port P's GET /metrics handler calling take_snapshot fresh on every request. A small inset box shows the PSI fallback path: this process's own cgroup v2 delegate (resolved via the '0::' line in /proc/self/cgroup) tried first, the system-wide /proc/pressure/* files tried second, and psi_available:false — not an error — if neither is readable."
   caption="Figure 36.1 — sysagent's take_snapshot: a t0/t1 /proc/stat delta bracketing five concurrently-gathered /proc + cgroup PSI sources, all landing in one Snapshot that sample --json and serve /metrics both emit" %}

> **Tools used** — none, by design: `sysagent` reads `/proc/*` and
> `/sys/fs/cgroup/*` directly, with no subprocess, no bcc-tools/bpftrace, and
> no root. The only tool this chapter reaches for to observe it from the
> outside is `curl` (host), against the `serve --port P` `/metrics` endpoint
> — `curl` ships in Fedora's base install and isn't separately checked by
> `scripts/check-host.sh`, the same way Chapter 39 notes for `cpupower` and
> `objdump`.

## Six sources, one deterministic schema

`sysagent` has exactly two subcommands, and both produce the same shape of
data:

```
sysagent sample [--json] [--interval-ms N]   # one snapshot to stdout
sysagent serve  --port P [--interval-ms N]   # the same snapshot over HTTP
```

`sample` takes one reading and prints it — `key=value` lines by default,
a single-line JSON object with `--json`. `serve` exposes the identical
snapshot over `GET /metrics`, taking a fresh reading on every request; any
other path 404s. Both walk the same six sources: `/proc/stat` (as a delta,
covered next), `/proc/loadavg`, `/proc/meminfo`, `/proc/diskstats`,
`/proc/net/dev`, and cgroup PSI. What makes this example worth its own
chapter isn't any one of those parsers — each is a plain-text file, `open`,
split fields, done — it's that the field names landing in the JSON are a
**deterministic, cross-language schema**: `cpu_util_pct`, `mem_total_kb`,
`disks[].reads`, `psi_available`, and every other key are byte-identical
whether the binary is the C++, Go, or Rust build. Point one Grafana
dashboard, one `jq` filter, or one test suite at any of the three and it
works unmodified — the entire point of README.md's field table, which this
chapter's `verify.lua` checks key by key.

## The `/proc/stat` delta: two cumulative reads, one percentage

`/proc/stat`'s `cpu` line is not a gauge. It's eight cumulative tick counters
— `user`, `nice`, `system`, `idle`, `iowait`, `irq`, `softirq`, `steal` —
that only ever grow, counted in USER_HZ ticks since the machine booted. A
single read of that line tells you nothing about *current* utilization; it
tells you the total CPU-seconds this machine has ever spent in each state.
`cpu_util_pct` only exists as the ratio of two such readings, `--interval-ms`
apart: `busy` is `user + nice + system + irq + softirq + steal`, `total` is
`busy + idle + iowait`, and the percentage is `100 × Δbusy / Δtotal` over
that window. `take_snapshot` reads `/proc/stat` once at the start (`t0`),
gathers everything else while `--interval-ms` elapses, reads `/proc/stat`
again (`t1`), and only then computes the delta:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    auto t1 = read_cpu_ticks();
    if (!t1) return std::unexpected(t1.error());

    auto busy0 = t0->user + t0->nice + t0->system;
    auto busy1 = t1->user + t1->nice + t1->system;
    auto idle0 = t0->idle + t0->iowait;
    auto idle1 = t1->idle + t1->iowait;
    auto total0 = busy0 + idle0 + t0->irq + t0->softirq + t0->steal;
    auto total1 = busy1 + idle1 + t1->irq + t1->softirq + t1->steal;

    auto d_total = static_cast<double>(total1 - total0);
    auto d_busy = static_cast<double>((busy1 - busy0) + (t1->irq - t0->irq) +
                                       (t1->softirq - t0->softirq) + (t1->steal - t0->steal));
    auto d_user = static_cast<double>(t1->user - t0->user);
    auto d_system = static_cast<double>(t1->system - t0->system);

    if (d_total > 0.0) {
        snap.cpu_util_pct = 100.0 * d_busy / d_total;
        snap.cpu_user_pct = 100.0 * d_user / d_total;
        snap.cpu_system_pct = 100.0 * d_system / d_total;
    }
```

```go
	t1, err := readCPUTicks()
	if err != nil {
		return Snapshot{}, err
	}

	busy0 := t0.user + t0.nice + t0.system
	busy1 := t1.user + t1.nice + t1.system
	idle0 := t0.idle + t0.iowait
	idle1 := t1.idle + t1.iowait
	total0 := busy0 + idle0 + t0.irq + t0.softirq + t0.steal
	total1 := busy1 + idle1 + t1.irq + t1.softirq + t1.steal

	dTotal := float64(total1 - total0)
	dBusy := float64((busy1 - busy0) + (t1.irq - t0.irq) + (t1.softirq - t0.softirq) + (t1.steal - t0.steal))
	dUser := float64(t1.user - t0.user)
	dSystem := float64(t1.system - t0.system)

	if dTotal > 0 {
		snap.CPUUtilPct = 100 * dBusy / dTotal
		snap.CPUUserPct = 100 * dUser / dTotal
		snap.CPUSystemPct = 100 * dSystem / dTotal
	}
```

```rust
    let t1 = read_cpu_ticks()?;

    let busy0 = t0.user + t0.nice + t0.system;
    let busy1 = t1.user + t1.nice + t1.system;
    let idle0 = t0.idle + t0.iowait;
    let idle1 = t1.idle + t1.iowait;
    let total0 = busy0 + idle0 + t0.irq + t0.softirq + t0.steal;
    let total1 = busy1 + idle1 + t1.irq + t1.softirq + t1.steal;

    let d_total = (total1 - total0) as f64;
    let d_busy =
        ((busy1 - busy0) + (t1.irq - t0.irq) + (t1.softirq - t0.softirq) + (t1.steal - t0.steal))
            as f64;
    let d_user = (t1.user - t0.user) as f64;
    let d_system = (t1.system - t0.system) as f64;

    if d_total > 0.0 {
        snap.cpu_util_pct = 100.0 * d_busy / d_total;
        snap.cpu_user_pct = 100.0 * d_user / d_total;
        snap.cpu_system_pct = 100.0 * d_system / d_total;
    }
```

`irq`, `softirq`, and `steal` fold into `d_busy` alongside `user`/`nice`/
`system` because all three represent CPU time genuinely spent — interrupt
handling, softirq processing, and (on a VM) time stolen by the hypervisor for
another guest — none of it is idle time, even though none of it is `user` or
`system` either. The `d_total > 0.0`/`dTotal > 0` guard exists because a
sufficiently short `--interval-ms` on an otherwise-idle single-CPU host could
in principle produce a zero-tick window (the kernel's tick granularity is
coarser than a few milliseconds); when that happens, all three percentages
stay at their zero-initialized default rather than dividing by zero.

## Graceful degradation: PSI is optional, not an error

Pressure Stall Information — the `some avg10=`/`avg60=`/`avg300=` lines in
`cpu.pressure`, `memory.pressure`, and `io.pressure` — tells you what
fraction of the last 10/60/300 seconds *some* task was stalled waiting on
that resource, which is a genuinely different signal from utilization: a CPU
can be 100% utilized with zero pressure (one thread pinning one core, nothing
else waiting) or 40% utilized with real pressure (many threads taking turns,
each one stalled part of the time). `read_psi` looks in two places, in
order: this process's own cgroup v2 delegate first — resolved by reading the
single `0::<path>` line out of `/proc/self/cgroup` and joining it onto
`/sys/fs/cgroup<path>/{cpu,memory,io}.pressure` — falling back to the
system-wide `/proc/pressure/{cpu,memory,io}` files if the cgroup-scoped ones
aren't there. Neither location is guaranteed: PSI can be compiled out of the
kernel, or a container can be handed a cgroup namespace with no delegated
`.pressure` files at all. When that happens, `sysagent` doesn't fail the
whole snapshot — `psi_available` comes back `false`, the three `avg10`
fields default to `0.0`, and every other field in the schema is unaffected.
A missing PSI source is data, not an error; the schema has a bit for exactly
that case so a dashboard built against it never has to special-case "did
this host even expose PSI."

## How the code works

`main` dispatches on `argv[1]` — `sample` or `serve`, anything else prints
the usage line and exits — and both subcommands share one hand-rolled
`Flags` scan (`--json`, `--interval-ms N`, `--port P`) kept in the same
shape across all three languages precisely so the argument-parsing code
never becomes the interesting part of the diff. `cmd_sample` calls
`take_snapshot(interval_ms)` once and prints either `to_text` or `to_json`;
`cmd_serve` hands `port` and `interval_ms` straight to `serve`, which owns
the HTTP loop and calls `take_snapshot` again on every `GET /metrics`.

`to_json`/`ToJSON`/`to_json` is the piece that actually delivers the
cross-language guarantee the schema promises, and it earns that guarantee by
being deliberately unglamorous: none of the three languages hands the
`Snapshot` to a generic serializer. C++ builds the object with an
`ostringstream` and literal `"key":value,` fragments in a fixed order; Go
does the same with `strings.Builder` and `fmt.Fprintf`; Rust with `String`
and `push_str`. A generic serializer — `nlohmann::json`, `encoding/json`,
`serde_json` — would have been less code, but none of the three guarantees
the *same* key order or the same float formatting as the others out of the
box, and a schema that's supposed to be byte-identical across languages
can't leave that up to three different libraries' defaults. Hand-writing the
same fixed field order three times, with the same `%.2f`-style two-decimal
formatting (`fmt2`/`f2`/`{:.2}`) for every float, is what actually makes
`verify.lua`'s key-by-key checks — and a `diff` between any two languages'
output — meaningful.

The HTTP layer is the one place the three implementations genuinely diverge
in mechanism rather than just syntax, and it follows the book's established
chatterd pattern: Go leans on `net/http`'s stdlib server, registering
`/metrics` on a `ServeMux` and letting `http.Server.Shutdown` handle a clean
`SIGINT`/`SIGTERM` stop. C++ and Rust hand-roll the same tiny surface
instead — a raw listening socket, `poll(2)` waiting on both the listener and
a `signalfd`/`SignalFd` doorbell, and one inline HTTP/1.1 response writer —
because pulling in a framework for one endpoint would hide the exact
mechanism this book has been building toward since Chapter 21's `chatterd`
and Chapter 24's signal-driven shutdown, not simplify it.

## Errors, three ways

`sysagent` has one usage-error surface and one runtime-error surface, and
they map to exit codes **2** and **1** respectively across all three
languages. A **usage error** — no subcommand, an unrecognized subcommand,
`serve` with no `--port`, or a non-positive `--interval-ms`/`--port` —
prints the single usage line to stderr and exits 2; every language funnels
every parse failure through the same `usage()` call before returning. A
**runtime error** — `/proc/stat`, `/proc/loadavg`, `/proc/meminfo`,
`/proc/diskstats`, or `/proc/net/dev` failing to open or parse — surfaces as
`sysagent: error: <message>` on stderr and exit 1; `/proc/stat` itself is
checked first, before any concurrent gathering even begins, since the whole
snapshot brackets around its two readings. A successful `sample` or a clean
signal-driven `serve` shutdown exits **0**.

PSI sits outside that error surface entirely, and Rust's type signatures
make the guarantee visible at compile time rather than just by convention:
`read_loadavg`, `read_meminfo`, `read_diskstats`, and `read_netdev` all
return `Result<T, String>` — they *can* fail, and `take_snapshot` propagates
that failure. `read_psi` returns `Option<Psi>` — there is no `Err` variant
for it to construct, so a PSI-read failure literally cannot become a runtime
error; the type system rules it out before `take_snapshot` is ever called.
C++ mirrors the same asymmetry by discipline rather than by type: every
other worker thread calls `note_error(...)` on failure, but `psi_thread`
never does, its `if` guards only the success path. Go hardcodes the same
asymmetry directly into the fan-in loop, which explicitly `continue`s past
`r.source == "psi"` before counting it toward the required sources or a
possible `firstErr`. Three different enforcement mechanisms — a type the
compiler checks, a code path that's simply never written, a loop that
explicitly skips one name — for the same one-sentence rule: a missing PSI
source degrades the snapshot, it never fails it.

## Concurrency lens

Five of `sysagent`'s six sources are independent of each other and of the
`/proc/stat` delta they're bracketed by, so `take_snapshot` overlaps all five
reads with the `--interval-ms` sleep instead of paying for each sequentially
— the wall-clock cost of one `sample` is `max(interval_ms, gather_time)`, not
`interval_ms + gather_time`. Every language spawns the same five workers
(`loadavg`, `meminfo`, `diskstats`, `netdev`, `psi`), sleeps, and joins before
`t1` is read, guaranteeing every field is filled in before the delta is
computed — but *how* each language gets a result out of a worker back into
the shared `Snapshot` is where the three genuinely part ways:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
std::expected<Snapshot, std::string> take_snapshot(int interval_ms) {
    auto t0 = read_cpu_ticks();
    if (!t0) return std::unexpected(t0.error());

    // Gather the non-CPU sources concurrently with the sampling sleep, using
    // jthreads (auto-joining, cooperatively cancellable) rather than raw
    // pthreads; ok_count is an atomic so every worker can report success
    // without a mutex.
    Snapshot snap{};
    std::atomic<int> ok_count{0};
    std::string first_error;
    std::mutex err_mu;
    auto note_error = [&](std::string msg) {
        std::lock_guard lock(err_mu);
        if (first_error.empty()) first_error = std::move(msg);
    };

    std::jthread load_thread([&] {
        if (auto la = read_loadavg()) {
            snap.load1 = la->load1;
            snap.load5 = la->load5;
            snap.load15 = la->load15;
            snap.runnable = la->runnable;
            snap.total_threads = la->total_threads;
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(la.error());
        }
    });
    std::jthread mem_thread([&] {
        if (auto mi = read_meminfo()) {
            snap.mem_total_kb = mi->total_kb;
            snap.mem_available_kb = mi->available_kb;
            snap.mem_used_kb = mi->total_kb - mi->available_kb;
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(mi.error());
        }
    });
    std::jthread disk_thread([&] {
        if (auto ds = read_diskstats()) {
            snap.disks = std::move(*ds);
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(ds.error());
        }
    });
    std::jthread net_thread([&] {
        if (auto ns = read_netdev()) {
            snap.net = std::move(*ns);
            ok_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            note_error(ns.error());
        }
    });
    std::jthread psi_thread([&] {
        if (auto psi = read_psi()) {
            snap.psi_available = true;
            snap.psi_cpu_some_avg10 = psi->cpu_some_avg10;
            snap.psi_mem_some_avg10 = psi->mem_some_avg10;
            snap.psi_io_some_avg10 = psi->io_some_avg10;
        }
        ok_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));

    // jthread destructors join as each goes out of scope at the end of this
    // block; explicitly joining here just makes the ordering-before-t1 clear.
    load_thread.join();
    mem_thread.join();
    disk_thread.join();
    net_thread.join();
    psi_thread.join();
```

```go
func TakeSnapshot(intervalMs int) (Snapshot, error) {
	t0, err := readCPUTicks()
	if err != nil {
		return Snapshot{}, err
	}

	var snap Snapshot
	results := make(chan namedErr, 5)

	go func() {
		la, err := readLoadAvg()
		if err == nil {
			snap.Load1, snap.Load5, snap.Load15 = la.load1, la.load5, la.load15
			snap.Runnable, snap.TotalThreads = la.runnable, la.totalThreads
		}
		results <- namedErr{"loadavg", err}
	}()
	go func() {
		mi, err := readMemInfo()
		if err == nil {
			snap.MemTotalKB, snap.MemAvailableKB = mi.totalKB, mi.availableKB
			snap.MemUsedKB = mi.totalKB - mi.availableKB
		}
		results <- namedErr{"meminfo", err}
	}()
	go func() {
		disks, err := readDiskStats()
		if err == nil {
			snap.Disks = disks
		}
		results <- namedErr{"diskstats", err}
	}()
	go func() {
		net, err := readNetDev()
		if err == nil {
			snap.Net = net
		}
		results <- namedErr{"netdev", err}
	}()
	go func() {
		if p, ok := readPSI(); ok {
			snap.PSIAvailable = true
			snap.PSICPUSomeAvg10, snap.PSIMemSomeAvg10, snap.PSIIOSomeAvg10 =
				p.cpuSomeAvg10, p.memSomeAvg10, p.ioSomeAvg10
		}
		results <- namedErr{"psi", nil}
	}()

	time.Sleep(time.Duration(intervalMs) * time.Millisecond)

	var firstErr error
	required := 0
	for i := 0; i < 5; i++ {
		r := <-results
		if r.source == "psi" {
			continue
		}
		required++
		if r.err != nil && firstErr == nil {
			firstErr = fmt.Errorf("%s: %w", r.source, r.err)
		}
	}
	if firstErr != nil {
		return Snapshot{}, firstErr
	}
	_ = required
```

```rust
pub fn take_snapshot(interval_ms: u64) -> Result<Snapshot, String> {
    let t0 = read_cpu_ticks()?;

    let mut snap = Snapshot::default();
    let mut first_err: Option<String> = None;

    thread::scope(|s| {
        let load_h = s.spawn(read_loadavg);
        let mem_h = s.spawn(read_meminfo);
        let disk_h = s.spawn(read_diskstats);
        let net_h = s.spawn(read_netdev);
        let psi_h = s.spawn(read_psi);

        thread::sleep(Duration::from_millis(interval_ms));

        match load_h.join().unwrap() {
            Ok(la) => {
                snap.load1 = la.load1;
                snap.load5 = la.load5;
                snap.load15 = la.load15;
                snap.runnable = la.runnable;
                snap.total_threads = la.total_threads;
            }
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        match mem_h.join().unwrap() {
            Ok(mi) => {
                snap.mem_total_kb = mi.total_kb;
                snap.mem_available_kb = mi.available_kb;
                snap.mem_used_kb = mi.total_kb - mi.available_kb;
            }
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        match disk_h.join().unwrap() {
            Ok(disks) => snap.disks = disks,
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        match net_h.join().unwrap() {
            Ok(net) => snap.net = net,
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        if let Some(psi) = psi_h.join().unwrap() {
            snap.psi_available = true;
            snap.psi_cpu_some_avg10 = psi.cpu_some_avg10;
            snap.psi_mem_some_avg10 = psi.mem_some_avg10;
            snap.psi_io_some_avg10 = psi.io_some_avg10;
        }
    });
```

C++ and Go both mutate the *same* shared `snap` directly from inside each
worker — `load_thread` writes `snap.load1`/`load5`/`load15`, `mem_thread`
writes the three `mem_*` fields, and so on. This is race-free, but only
because every worker touches a *disjoint* set of fields: distinct struct
fields are distinct objects for data-race purposes in both languages'
memory models, so five threads writing to five non-overlapping field groups
of one struct never race — Go additionally needs the channel send/receive
in `results <- namedErr{...}` / `r := <-results` to establish the
happens-before edge that makes each goroutine's writes visible to the
receiver, which the buffered channel provides for free. Rust's `thread::scope`
takes a structurally different shape for a reason that isn't stylistic: the
five spawned closures (`s.spawn(read_loadavg)`, etc.) don't capture `snap` at
all, because two closures each holding `&mut snap` to write disjoint fields
would still require *two simultaneous mutable borrows of the same value* —
something the borrow checker refuses regardless of whether the underlying
fields actually overlap, since it reasons about the whole object, not the
fields a closure happens to touch. So Rust's workers return owned values
through their `JoinHandle`s instead, and every assignment into `snap` happens
in the *scope* thread itself, sequentially, immediately after each
`.join().unwrap()` — safe by construction, where C++ and Go are safe by the
programmer having correctly kept every worker's field set disjoint.

## Build, run, observe

```bash
[host]$ cd examples/36-proc-sys-and-the-agent && ./demo.sh build
```

A real snapshot, gathered on the reference host while the JSON output was
being captured for this chapter:

```console
[host]$ ./demo.sh cpp run sample --json
{"cpu_util_pct":6.27,"cpu_user_pct":5.33,"cpu_system_pct":0.63,"load1":0.80,
 "load5":0.88,"load15":1.04,"runnable":7,"total_threads":3961,
 "mem_total_kb":65545860,"mem_available_kb":33268760,"mem_used_kb":32277100,
 "disks":[{"name":"nvme0n1","reads":1092534,"writes":28109458,...}, ...],
 "net":[{"iface":"lo","rx_bytes":1456453981,"tx_bytes":1456453981}, ...],
 "psi_available":true,"psi_cpu_some_avg10":0.00,"psi_mem_some_avg10":0.00,
 "psi_io_some_avg10":0.00}
```

`mem_total_kb=65545860` is ~62.5 GiB, this host's actual installed RAM;
`psi_available:true` because this box's cgroup v2 delegate exposes PSI, so
the process's own `cpu.pressure`/`memory.pressure`/`io.pressure` files were
readable — no fallback to `/proc/pressure` was needed on this run. With no
arguments at all, all three builds print the usage line to stderr and exit 2:

```console
[host]$ ./demo.sh go run
usage: sysagent sample [--json] [--interval-ms N] | serve --port P [--interval-ms N]
```

`serve` exposes the same schema over HTTP, one fresh sample per request:

```console
[host]$ ./demo.sh rust run serve --port 9100 &
sysagent: listening on 0.0.0.0:9100
[host]$ curl -s http://127.0.0.1:9100/metrics | head -c 40
{"cpu_util_pct":7.21,"cpu_user_pct":6.02,
[host]$ curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:9100/nope
404
[host]$ kill -INT %1
sysagent: shutting down
```

The second `cpu_util_pct` (7.21) differs from the first (6.27) because
`/metrics` takes a genuinely new `--interval-ms`-apart sample on every
request rather than caching one — the same live measurement `sample`
prints, just reachable over the network. The unknown path 404s, and `SIGINT`
prints the shutdown line and exits 0.

The runner drives all three languages on the local host:

```console
[host]$ python3 scripts/test-all-examples.py --only 36-proc-sys-and-the-agent --mode local
verifying...
  verify 36-proc-sys-and-the-agent [cpp]: PASS
  verify 36-proc-sys-and-the-agent [go]: PASS
  verify 36-proc-sys-and-the-agent [rust]: PASS

example                     cpp   go    rust
36-proc-sys-and-the-agent   PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

## Cross-check: a static fact that has to match exactly, and a load that has to move

Two different cross-checks back two different kinds of claim. The first is a
**static fact**: `mem_total_kb` is `MemTotal` straight out of
`/proc/meminfo`, unaffected by system load or timing, so `verify.lua` reads
it two ways in the same run — once through `serve`'s `/metrics` response,
once through a direct `awk '/^MemTotal:/{print $2}' /proc/meminfo` — and
requires the two numbers to agree **exactly**, not approximately. On this
host that's `65545860` from both sources, every time; any mismatch would
mean the parser is reading the wrong field or the wrong file, not measurement
noise, since nothing about this number is supposed to move between two
reads a few milliseconds apart.

The second cross-check backs a **dynamic** claim instead: that `cpu_util_pct`
actually tracks real CPU load rather than returning a constant or a
miscomputed delta. `verify.lua` takes an idle `sample --json` baseline, spins
up `nproc` tight busy-loop children for slightly longer than a
`--interval-ms 500` window, samples again, and checks that `cpu_util_pct`
rose by a real margin — retried up to three times, with a 12-percentage-point
floor (or an outright ≥55% reading, for the case where the host is already
loaded before the check even starts). The margin and the retries exist
because this runs on a real, possibly-noisy development box, not an isolated
benchmark rig — the check has to distinguish "this host happened to be busy
for an unrelated reason" from "the parser is broken," and a single
non-retried comparison couldn't.

## What you learned

- **`/proc/stat` is a cumulative counter, not a gauge.** A percentage only
  exists as the delta between two readings `--interval-ms` apart —
  `take_snapshot` reads it once before the concurrent gather, once after,
  and computes `cpu_util_pct` only from the difference.
- **Gather independent `/proc` sources concurrently with the sampling
  sleep, not sequentially after it.** Five sources, one shared
  `--interval-ms` window: the wall-clock cost of a sample is
  `max(interval_ms, gather_time)`, not the sum.
- **A missing signal should degrade the data, not fail the call.** PSI
  absent — kernel support missing, or a container with no cgroup v2
  delegate — sets `psi_available:false` rather than returning an error;
  Rust enforces this at the type level (`Option<Psi>`, no `Err` variant to
  construct), C++ and Go enforce the same rule by never writing the failure
  path for that one source.
- **A cross-language schema is only real if you hand-write the
  serializer.** All three `to_json` implementations build the JSON object
  field-by-field in a fixed order with the same float formatting, rather
  than delegating to a generic library whose key order and number
  formatting would drift between languages.
- **Rust's borrow checker forces a different concurrency shape, not just
  different syntax.** C++ and Go both mutate one shared struct directly
  from five worker threads — safe because the field sets are disjoint, but
  only by the programmer's discipline. Rust can't express that pattern at
  all without extra ceremony, so `thread::scope`'s workers return owned
  values and every write into `snap` happens sequentially in the parent,
  after each `.join()` — safe by construction instead.
- **`mem_total_kb` served over HTTP matched a direct `/proc/meminfo` read
  exactly**, and a busy-loop child measurably raised `cpu_util_pct` over an
  idle baseline — two cross-checks confirming the schema reports what the
  kernel actually has, not what the code merely claims.

Chapter 37 keeps the same Utilization/Saturation/Errors lens but inverts the
source of truth entirely: instead of reading `/proc` directly the way this
chapter does, `loadmix analyze` shells out to `uptime`, `vmstat`, `mpstat`,
`iostat`, `sar`, and `pidstat` exactly as an on-call engineer would, and
parses each tool's plain-text table by column name rather than reading
kernel counters itself. Read the two chapters back to back — same checklist,
same USE method, two structurally opposite ways of answering it.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host (kernel 7.1.3-200.fc44, 16 CPUs, ~62 GiB RAM) this session:
<code>python3 scripts/test-all-examples.py --only 36-proc-sys-and-the-agent
--mode local</code> printed <code>PASS PASS PASS</code> (3 passed, 0 failed;
<code>verify.lua</code> reported <code>PASS 32 / FAIL 0</code> for each of
cpp/go/rust). A real <code>sample --json</code> run printed
<code>cpu_util_pct:6.27</code>, <code>mem_total_kb:65545860</code>,
<code>psi_available:true</code>, and populated <code>disks</code>/<code>net</code>
arrays, exactly matching the schema in <code>README.md</code>. No arguments
printed the usage line on stderr and exited 2. <code>serve --port P</code>
printed <code>sysagent: listening on 0.0.0.0:P</code>; a second real sample
over <code>GET /metrics</code> read <code>cpu_util_pct:7.21</code>, confirming
each request takes a fresh reading; an unknown path returned HTTP 404;
<code>SIGINT</code> printed <code>sysagent: shutting down</code> and exited 0.
<code>verify.lua</code>'s cross-check confirmed <code>mem_total_kb</code>
served over <code>/metrics</code> agrees exactly with a direct
<code>awk '/^MemTotal:/{print $2}' /proc/meminfo</code> read
(<code>65545860</code> both ways), and its retried, margin-based check
confirmed a busy-loop child measurably raised <code>cpu_util_pct</code> above
an idle baseline. Not exercised: the <code>psi_available:false</code>
degraded path (this host's cgroup v2 delegate exposes PSI, so every sample
this session read <code>psi_available:true</code>) and any run against the
lab VM — this is a local-mode example (<code>mode: local</code> in the
manifest) that reads the current host's own <code>/proc</code>, not a
VM's.</p>
