---
title: "The USE method in 60 seconds: a checklist that runs the real tools, not a shortcut around them"
order: 37
part: "Observability"
description: "loadmix is Brendan Gregg's Utilization/Saturation/Errors checklist as a runnable program — a saturation generator that pegs one resource at a time, and an analyze mode that shells out to uptime, vmstat, mpstat, iostat, sar, and pidstat exactly as a human would, parses each tool's plain-text table by column name from its own header row, runs the five interval samplers concurrently so a 60-second checklist costs 60 seconds and not five minutes, and names the resource whose Saturation signal fired hardest."
duration: "50 minutes"
---

Chapter 36's `sysagent` built a USE-method collector from first principles —
reading `/proc/stat`, `/proc/net`, `/proc/diskstats`, and cgroup PSI directly,
no external process, no tool you'd have to trust. That is the right call for a
program that runs continuously in production. But it is not how most engineers
actually triage a system that's in trouble *right now*: they SSH in and reach
for `uptime`, `vmstat 1`, `mpstat -P ALL 1`, `iostat -xz 1`, `pidstat 1`, and
`free`, running the checklist Brendan Gregg popularized as "the USE method" —
for every resource, ask whether it's **Utilized**, **Saturated**, or
**Erroring** — because those tools already exist, are already trusted, and
already parse a live system correctly. `loadmix` is that checklist wired into
a program: a saturation generator that pegs one resource on command, and an
`analyze` mode that runs the real tools exactly as a human would, reads their
plain-text tables the way a human reads them — by column *name*, from each
tool's own header row — and tells you which resource is in trouble and why.

The code is in `examples/37-use-method-60s-analysis/`. The run script there
builds/sets up and runs it; its `README.md` covers what it does and how to
drive it.

{% include excalidraw.html
   file="37-use-method-checklist"
   alt="A pipeline diagram of loadmix analyze. Left column, four sequential pre-samples run once at start: uptime, dmesg tail, top -b -n1, and ss -s, each landing in a single printed 'tool' line. Center, a band labeled 'five interval samplers, spawned concurrently' containing five parallel boxes — vmstat 1 N, mpstat -P ALL 1 N, iostat -xz 1 N, sar -n DEV 1 N, and pidstat 1 N — each writing to its own temp file, all five joined by a single wait so the band's total wall time is N seconds, not 5N; a note under the band reads 'N-second checklist costs ~N seconds'. Right, each sampler's output feeds a column-name parser (Row::get against the tool's own header row) into a 4x3 signal matrix: rows cpu/mem/io/net, columns Utilization/Saturation/Errors, each cell a fired=true-or-false verdict against a fixed threshold — cpu's Saturation threshold shown as cpus+2 to make the guest-relative scaling visible. An arrow from the matrix's Saturation column into a final verdict box reads 'resource with the highest Saturation-value/threshold ratio wins'. A small inset box below the diagram shows the saturate side: app --resource cpu|mem|io|net --seconds N spinning a worker pool that the analyze pipeline, run concurrently against it, is able to see and attribute."
   caption="Figure 37.1 — the analyze pipeline: four one-shot pre-samples, five interval samplers run concurrently, column-name parsing into a 4×3 USE signal matrix, and the highest-Saturation-ratio verdict" %}

> **Tools used** — `uptime`, `dmesg`, `top`, `ss`, `vmstat`, `free`
> (procps-ng/util-linux/iproute2, base Fedora on the `systems-target` guest)
> plus `mpstat`, `iostat`, `sar`, `pidstat` (the `sysstat` package, installed
> by the guest's own cloud-init) — all on the `systems-target` VM, which
> `loadmix analyze` invokes directly as subprocesses. No tool here is
> `loadmix`-specific; this chapter's whole premise is that the checklist is
> the same nine commands you'd type by hand.

## Two modes, one binary

`loadmix` has exactly two things it does, and the split is deliberate: one
half creates a problem, the other half diagnoses it, so the same binary can
demonstrate both the checklist and its payoff.

```
app --resource cpu|mem|io|net --seconds N   saturate that resource
app analyze [--seconds N]                    run the checklist and name
                                              the saturated resource
```

`--resource cpu` spins a pool of worker threads doing nothing but xorshift
arithmetic — no syscalls, no allocation in the hot loop, pure CPU. `--resource
mem` touches 1.35× the guest's `MemTotal` across a worker pool, deliberately
overshooting available RAM to push real pages into swap. `--resource io`
opens one file per worker with `O_DSYNC` and `pwrite`s a 16 KiB buffer of
xorshift-generated (non-compressible) bytes to the same offset in a loop,
forcing a synchronous write to the block device on every call. `--resource
net` floods UDP datagrams over loopback from a pool of senders to one
receiver. Each saturator prints a `loadmix: start ...` line with the
resource, requested duration, and worker count, runs for (approximately)
that duration, and prints a `loadmix: done ... ops= bytes=` line. `analyze`
does none of that — it induces no load of its own, and instead spends its
`--seconds` window watching the system through the same nine tools listed
above.

## Parsing by column name, not position

The single design decision that makes `analyze` durable — the reason it
survives a `sysstat` or `procps-ng` version bump that reorders columns — is
that every parser reads a tool's header row first and looks fields up by
*name* against that row, never by a hardcoded index. `Row` (C++), `row` (Go),
and `Row` (Rust) all express the same idea: a table row is a name list zipped
with a value list, and `get`/`getOr` walks the name list linearly to find the
column a caller actually asked for.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// A parsed table row: column names (from a tool's own header line) zipped
// with this row's values, so every parser below reads fields *by name*
// rather than by a hardcoded position — robust to sysstat/procps column
// reordering across versions.
struct Row {
    std::vector<std::string> names;
    std::vector<std::string> values;

    [[nodiscard]] std::optional<double> get(const std::string& name) const {
        for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
            if (names[i] == name) {
                try {
                    return std::stod(values[i]);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
        return std::nullopt;
    }
};

Row make_row(const std::vector<std::string>& header_tokens, size_t header_skip,
             const std::vector<std::string>& data_tokens, size_t data_skip) {
    Row r;
    for (size_t i = header_skip; i < header_tokens.size(); ++i) r.names.push_back(header_tokens[i]);
    for (size_t i = data_skip; i < data_tokens.size(); ++i) r.values.push_back(data_tokens[i]);
    return r;
}
```

```go
// row is a parsed table row: column names (from a tool's own header line)
// zipped with this row's values, so every parser below reads fields *by
// name* rather than by a hardcoded position — robust to sysstat/procps
// column reordering across versions.
type row struct {
	names  []string
	values []string
}

func (r row) getOr(name string, def float64) float64 {
	n := len(r.names)
	if len(r.values) < n {
		n = len(r.values)
	}
	for i := 0; i < n; i++ {
		if r.names[i] == name {
			v, err := strconv.ParseFloat(r.values[i], 64)
			if err != nil {
				return def
			}
			return v
		}
	}
	return def
}

func makeRow(headerTokens []string, headerSkip int, dataTokens []string, dataSkip int) row {
	var r row
	if headerSkip < len(headerTokens) {
		r.names = append(r.names, headerTokens[headerSkip:]...)
	}
	if dataSkip < len(dataTokens) {
		r.values = append(r.values, dataTokens[dataSkip:]...)
	}
	return r
}
```

```rust
// A parsed table row: column names (from a tool's own header line) zipped
// with this row's values, so every parser below reads fields *by name*
// rather than by a hardcoded position -- robust to sysstat/procps column
// reordering across versions.
struct Row {
    names: Vec<String>,
    values: Vec<String>,
}

impl Row {
    fn get(&self, name: &str) -> Option<f64> {
        let n = self.names.len().min(self.values.len());
        for i in 0..n {
            if self.names[i] == name {
                return self.values[i].parse::<f64>().ok();
            }
        }
        None
    }
}

fn make_row(
    header_tokens: &[String],
    header_skip: usize,
    data_tokens: &[String],
    data_skip: usize,
) -> Row {
    let names = header_tokens.get(header_skip..).unwrap_or(&[]).to_vec();
    let values = data_tokens.get(data_skip..).unwrap_or(&[]).to_vec();
    Row { names, values }
}
```

`header_skip`/`data_skip` exist because a header row and its data rows don't
always start aligned — `mpstat -P ALL`'s `Average:` rows both begin with a
label token (`Average: CPU` / `Average: all`) that has to be skipped before
`%idle` in the header lines up with the actual idle percentage in the data.
`parse_mpstat` puts this together end to end: it scans every line looking for
exactly those two `Average:` rows (sysstat's own aggregate over the whole
sampling window, which conveniently excludes the since-boot first report for
you), builds a `Row` from them, and reads `%idle` and `%iowait` by name:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
struct MpstatResult {
    double busy_pct = 0.0;
    double iowait_pct = 0.0;
};

// mpstat -P ALL's own "Average:" row over the whole window (excludes the
// since-boot first report automatically — sysstat's job, not ours).
MpstatResult parse_mpstat(const std::string& text) {
    MpstatResult res;
    std::vector<std::string> header, data;
    for (auto& line : split_lines(text)) {
        auto toks = split_ws(line);
        if (toks.size() < 2) continue;
        if (toks[0] == "Average:" && toks[1] == "CPU") header = toks;
        else if (toks[0] == "Average:" && toks[1] == "all") data = toks;
    }
    if (header.empty() || data.empty()) return res;
    Row r = make_row(header, 2, data, 2);
    double idle = r.get("%idle").value_or(100.0);
    res.iowait_pct = r.get("%iowait").value_or(0.0);
    res.busy_pct = 100.0 - idle;
    return res;
}
```

```go
type mpstatResult struct {
	busyPct   float64
	iowaitPct float64
}

// mpstat -P ALL's own "Average:" row over the whole window (excludes the
// since-boot first report automatically — sysstat's job, not ours).
func parseMpstat(text string) mpstatResult {
	res := mpstatResult{}
	var header, data []string
	for _, line := range splitLines(text) {
		toks := splitWs(line)
		if len(toks) < 2 {
			continue
		}
		if toks[0] == "Average:" && toks[1] == "CPU" {
			header = toks
		} else if toks[0] == "Average:" && toks[1] == "all" {
			data = toks
		}
	}
	if header == nil || data == nil {
		return res
	}
	r := makeRow(header, 2, data, 2)
	idle := r.getOr("%idle", 100.0)
	res.iowaitPct = r.getOr("%iowait", 0.0)
	res.busyPct = 100.0 - idle
	return res
}
```

```rust
#[derive(Default)]
struct MpstatResult {
    busy_pct: f64,
    iowait_pct: f64,
}

// mpstat -P ALL's own "Average:" row over the whole window (excludes the
// since-boot first report automatically -- sysstat's job, not ours).
fn parse_mpstat(text: &str) -> MpstatResult {
    let mut res = MpstatResult::default();
    let mut header: Vec<String> = Vec::new();
    let mut data: Vec<String> = Vec::new();
    for line in split_lines(text) {
        let toks = split_ws(&line);
        if toks.len() < 2 {
            continue;
        }
        if toks[0] == "Average:" && toks[1] == "CPU" {
            header = toks;
        } else if toks[0] == "Average:" && toks[1] == "all" {
            data = toks;
        }
    }
    if header.is_empty() || data.is_empty() {
        return res;
    }
    let r = make_row(&header, 2, &data, 2);
    let idle = r.get("%idle").unwrap_or(100.0);
    res.iowait_pct = r.get("%iowait").unwrap_or(0.0);
    res.busy_pct = 100.0 - idle;
    res
}
```

`busy_pct` is computed as `100.0 - %idle` rather than summing `%usr` + `%sys`
+ `%nice` + ... by name, because `%idle`'s complement is definitionally
"everything else," immune to a future sysstat release adding a new
utilization sub-category this parser has never heard of. The other three
parsers (`parse_vmstat`, `parse_iostat`, `parse_sar_pkts`) each carry one more
quirk worth knowing before you read the source: `vmstat` has no `Average:`
line at all, and its first sample is a since-boot average rather than a live
interval, so `parse_vmstat` scans every data row, discards row 0, and takes
the *max* of `r` (the run queue) across the rest — the steady-state
saturation signal, not one diluted by averaging across the whole window.
`iostat -xz` has the identical since-boot-first-row quirk, plus `-z` hides
idle devices entirely, so `parse_iostat` tracks block boundaries with its own
`avg-cpu:` counter and skips block 0 by index rather than by content.

## How the code works

`cmd_analyze` runs in three phases. First, four one-shot pre-samples —
`uptime`, `dmesg | tail -n 5`, `top -b -n1`, `ss -s` — each captured through
`run_capture`, the one function in the file with a real failure mode (the
shell itself failing to start), which is why it's the one that returns
`std::expected`/`error`/`Result` per this book's convention. `dmesg` on an
unprivileged account returns `Operation not permitted`; `cmd_analyze` checks
for that string and reports `status=denied` rather than treating it as a
capture failure, because an unprivileged `analyze` run is the common case,
not an edge case.

Second, the five interval samplers — `vmstat`, `mpstat -P ALL`, `iostat -xz`,
`sar -n DEV`, `pidstat`, each invoked as `<tool> 1 N` — are spawned
**concurrently**, each redirected to its own temp file, and only then
joined. This matters more than it looks: each of those five commands blocks
for `N` seconds sampling at a 1-second interval, so running them one after
another would make a 60-second checklist take five minutes. Running them
together makes it take 60 seconds, matching the chapter's own title. Third,
`free -m`'s single-shot output and the five sampler files are parsed (all by
column name, as above), assembled into eight `analyze: metric ...` lines, and
checked against the fixed threshold table that produces the USE signal
matrix and the verdict.

That threshold table is worth reading directly, because it *is* the USE
method encoded as data:

```
cpu   Utilization: busy_pct  >= 60%        Saturation: run_queue >= cpus+2
mem   Utilization: used_pct  >= 75%        Saturation: swap_io   >= 300 KB/s
io    Utilization: util_pct  >= 40%        Saturation: iowait_pct >= 8%
net   Utilization: pkts/s    >= 2000       Saturation: pkts/s     >= 8000
```

cpu's Saturation threshold is the one that scales with the machine it's
running on rather than being a flat number: `cpus + 2`, so a run queue longer
than the guest's own core count plus a little headroom means threads are
genuinely waiting for a CPU that isn't there — the textbook definition of
saturation, not an arbitrary constant that would be wrong on a differently
sized host. Errors is printed for every resource but always `fired=false`,
because this chapter's saturators induce load, not kernel-logged faults; the
line exists so the U/S/E triple is complete and so a future chapter that
*does* wire in `dmesg`-sourced error counts has somewhere to plug them in.
The verdict is whichever resource has the highest `Saturation value /
Saturation threshold` ratio — not the highest raw Saturation value, which
would let net's `8000`-scale threshold always lose to cpu's `4`-scale one
regardless of which resource is actually worse off.

## Errors, three ways

`loadmix` has one failure surface and one exit code for it: a **usage
error** — no `--resource`/`--seconds` given at all, an unrecognized
`--resource` value, `--seconds 0` or negative, or an unrecognized flag to
`analyze` — prints the two-line usage banner to stderr and exits **2**. C++
funnels every parse path through `usage()` and returns `2` from `main`; Go
calls `usage()` then `os.Exit(2)`; Rust calls `usage()` then
`std::process::exit(2)`. There is no separate "runtime error" exit code here,
unlike Chapter 39's codec-bug path or Chapter 40's connection-refused path —
`analyze`'s subprocess calls are designed to degrade gracefully instead of
failing the whole run: a denied `dmesg`, an empty `ss -s`, or a sampler that
produces no data rows all show up as a `0.00`/`denied`/empty field in the
output rather than a non-zero exit, because the entire point of a diagnostic
checklist is that it still reports *something* under the same restricted
privileges a real on-call engineer would have. A successful run of either
mode — saturate or analyze — exits **0**.

## Concurrency lens

Every saturator uses the same three-part shape: compute a `deadline`, spawn a
worker pool that loops until `stop_requested() || now >= deadline`, then join.
C++'s pool is a `std::vector<std::jthread>`; Go's is a `sync.WaitGroup` over
goroutines with `sync/atomic` counters; Rust's is `std::thread::scope`, the
`jthread` analogue where worker threads are RAII-joined when the scope block
ends. All three converge on identical worker-count formulas — `max(4, cpus*3)`
for cpu, `max(4, cpus*4)` for io and net, `max(2, cpus)` for mem — and
identical atomic counters accumulated across the pool.

Where the three languages genuinely diverge is in how the parent thread stays
alive long enough for that deadline to matter. C++'s `std::jthread` runs
`request_stop()` *before* `join()` in its destructor, which means the instant
the enclosing scope's spawn loop finishes, every worker is told to stop —
before the deadline the workers are checking has had any chance to arrive.
Left alone, this was a real bug in the first cut of this example: every
saturator exited in single-digit milliseconds regardless of `--seconds`,
because `request_stop()` fired essentially at thread-creation time. The fix
is `std::this_thread::sleep_until(deadline)` in the parent, holding the
enclosing scope open for the full requested duration before it's allowed to
close and trigger the RAII-driven stop-then-join:

```cpp
    {
        std::vector<std::jthread> pool;
        pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) {
            pool.emplace_back([&total_iters, &checksum, deadline, w](std::stop_token st) {
                uint64_t x = 0x9E3779B97F4A7C15ULL ^ (w + 1);
                uint64_t iters = 0;
                while (!st.stop_requested()) {
                    x ^= x << 7;
                    x ^= x >> 9;
                    ++iters;
                    if ((iters & 0xFFFFF) == 0 && std::chrono::steady_clock::now() >= deadline) break;
                }
                total_iters.fetch_add(iters, std::memory_order_relaxed);
                checksum.fetch_add(x, std::memory_order_relaxed); // keeps x provably live (no DCE)
            });
        }
        // Hold the scope open for the full duration. ~jthread runs
        // request_stop() *before* join(), so without this wait the workers are
        // told to stop the instant the spawn loop ends and the deadline check
        // below never fires. The sleep makes that stop request the intended
        // post-deadline signal rather than an immediate kill.
        std::this_thread::sleep_until(deadline);
    } // jthreads join here (RAII)
```

Go and Rust never needed this hack, because in both ports each worker checks
its *own* deadline directly inside its loop condition (`time.Now().Before(deadline)`,
`Instant::now() < deadline`) rather than waiting on an external stop signal
raised by the parent — the parent simply calls `wg.Wait()` / lets
`thread::scope` return, which blocks until every worker's own loop condition
has already become false on its own. There is no analogue of `jthread`'s
stop-before-join ordering to fight, because nothing external tells these
workers to stop early; they stop themselves, on schedule, and the parent's
join is purely a rendezvous rather than part of the termination signal.

The second, unrelated concurrency story in this chapter is `analyze`'s five
interval samplers, spawned as five real OS processes — `fork`+`execvp` in
C++, `exec.Command` on five goroutines in Go, `Command::spawn` in Rust — with
a single joint wait afterward. This isn't thread concurrency inside the
program at all; it's five independent subprocesses racing the wall clock
together so that, as the source comment puts it, "a 12s analyze window costs
~12s wall-clock, not 5x that from running them one at a time."

## Build, run, observe

```bash
[host]$ cd examples/37-use-method-60s-analysis && ./demo.sh build
```

This is a **VM example**: the saturation signals only mean something measured
against a real, otherwise-quiet kernel, so `verify.lua` stages the built
binary directly to `/tmp/lsp37-<lang>/app` on `systems-target` via plain
`scp`/`ssh` and drives it there — not through `demo.sh <lang> run
TARGET=...`, whose `deploy-to-vm.sh` wraps commands in `ssh -t` and injects
CRLF that breaks the tool's plain-text output.

An idle checklist first, so the baseline is visible:

```console
[vm]$ ./app analyze --seconds 10
analyze: start seconds=10 cpus=2
analyze: tool uptime raw="05:51:20 up  3:39,  1 user,  load average: 1.78, 0.45, 0.15"
analyze: tool dmesg status=denied
analyze: tool top cpu="%Cpu(s):  0.0 us,  0.0 sy,  0.0 ni,100.0 id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st" tasks="Tasks: 149 total, 2 running, 147 sleep, 0 d-sleep, 0 stopped, 0 zombie"
analyze: tool ss raw="Total: 167"
analyze: tool pidstat active_processes=4
analyze: metric resource=cpu name=busy_pct value=1.34 unit=pct
analyze: metric resource=cpu name=run_queue value=3.00 unit=procs
...
analyze: signal resource=cpu type=Utilization fired=false value=1.34 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=false value=3.00 threshold=4.00
...
analyze: verdict resource=cpu ratio=0.75
analyze: done
```

`dmesg status=denied` is expected — the guest's unprivileged `fedora` user
gets `Operation not permitted` from `dmesg`, and `analyze` reports that
plainly rather than failing. This run finished in **10.417s** wall-clock for
a `--seconds 10` window, confirming the five samplers really do run
concurrently — five serial `<tool> 1 10` invocations would have taken close
to 50 seconds.

Now the payoff: a background saturator, then a concurrent checklist run
against it.

```console
[vm]$ nohup ./app --resource cpu --seconds 25 >/tmp/bg.out 2>&1 &
[vm]$ sleep 2 && ./app analyze --seconds 12
analyze: tool top cpu="%Cpu(s): 97.8 us,  2.2 sy,  0.0 ni,  0.0 id, ..."
analyze: metric resource=cpu name=busy_pct value=100.00 unit=pct
analyze: metric resource=cpu name=run_queue value=8.00 unit=procs
analyze: signal resource=cpu type=Utilization fired=true value=100.00 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=true value=8.00 threshold=4.00
analyze: verdict resource=cpu ratio=2.00
analyze: done
```

`busy_pct` rose from the idle baseline of **1.34** to **100.00**; `run_queue`
rose from **3.00** to **7.00–8.00** across three independent captures on this
2-vCPU guest — comfortably clearing both the `busy_pct >= 60` and
`run_queue >= cpus+2 = 4` thresholds by a wide margin, every time.
Utilization fired true, and the verdict landed on `cpu`: `analyze`, run
concurrently against a background `loadmix --resource cpu` saturator it knows
nothing about, correctly identified the resource in trouble by reading
exactly the same five tools a person would have run by hand.

```console
[vm]$ ./app --resource cpu --seconds 5
loadmix: start resource=cpu seconds=5 workers=6
loadmix: done resource=cpu seconds=5 workers=6 ops=8801461550 bytes=11315899737308079569
```

`workers=6` matches the documented formula, `max(4, cpus*3)` with `cpus=2` →
`max(4,6)=6`; this run's wall time was **5.002s** for a requested
`--seconds 5`, tracking the request within about 0.2 seconds.

The runner drives all three languages against `systems-target`:

```console
[host]$ python3 scripts/test-all-examples.py --only 37-use-method-60s-analysis --mode vm
verifying...
  verify 37-use-method-60s-analysis [cpp]: PASS
  verify 37-use-method-60s-analysis [go]: PASS
  verify 37-use-method-60s-analysis [rust]: PASS

example                        cpp   go    rust
37-use-method-60s-analysis     PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

## Cross-check: the same load, seen two ways

The load-attribution run above is itself a cross-check — `analyze`'s verdict
is only trustworthy if it agrees with what you already know is true, since
you launched the saturator yourself. But it's worth pairing `analyze`'s own
report against `top`'s independent view, captured in the very same run: `top
-b -n1`'s `%Cpu(s)` line read `97.8 us, 2.2 sy, 0.0 id` while `mpstat`'s
`busy_pct` (100.0 − `%idle`) read `100.00`. The two numbers come from
different tools sampling at different instants over different windows —
`top` a single one-shot snapshot, `mpstat` an average over the whole
`analyze --seconds` window — so they are not expected to match to the decimal
place, and they don't; what they agree on is the story: this guest's CPUs are
saturated, not idle, not lightly loaded. A verdict that `mpstat` alone
disagreed with `top` about — one reporting near-idle while the other reported
saturated — would be the sign of a parsing bug, not measurement noise, since
both tools are reading the same underlying `/proc/stat` counters through
different sampling windows.

## What you learned

- **The USE method is a checklist, not a tool** — for every resource, ask
  whether it's Utilized, Saturated, or Erroring, against a threshold you
  choose up front. `loadmix analyze` is that checklist wired into a program
  that runs the same nine commands (`uptime`, `dmesg`, `top`, `ss`, `vmstat`,
  `mpstat`, `iostat`, `sar`, `pidstat`, `free`) a person would run by hand.
- **Parse plain-text tool output by column name, from the tool's own header
  row, never by hardcoded position** — `Row::get`/`row.getOr` walk a name
  list to find the field a caller asked for, which is what keeps every
  parser correct across a `sysstat` or `procps-ng` version that reorders
  columns.
- **Run independent interval samplers concurrently, not serially** — five
  `<tool> 1 N` processes spawned together and joined once made a
  `--seconds 10` checklist finish in 10.417s instead of the ~50s five serial
  invocations would have cost.
- **A saturation threshold should scale with the machine it runs on where
  that's the accurate signal** — cpu's `run_queue >= cpus + 2` uses the
  guest's own core count, unlike net's flat `8000 pkts/s`, because "threads
  waiting for a CPU" only means something relative to how many CPUs exist.
- **`std::jthread`'s RAII order (`request_stop()` before `join()`) can silence
  a worker before its own deadline ever fires** — the fix is holding the
  parent scope open with `sleep_until(deadline)`; Go and Rust never needed
  the workaround because each worker checks its own deadline directly rather
  than waiting on an externally raised stop signal.
- **`analyze`, run concurrently against a background `--resource cpu`
  saturator it knows nothing about, correctly attributed the load**:
  `busy_pct` 1.34 → 100.00, `run_queue` 3.00 → 7.00–8.00, Utilization fired
  true, verdict `cpu` — stable across three independent attempts on the lab
  guest.

Chapter 38 takes the numbers this checklist prints to a terminal and sends
them somewhere durable: OpenTelemetry into the LGTM stack Chapter 3 stood up,
so the next time a resource saturates you don't have to be watching when it
happens.

---

<p><span class="status status--verified">verified</span> — on the
<code>systems-target</code> lab guest (Fedora 44, kernel
<code>6.19.10-300.fc44</code>, 2 vCPU nested KVM) this session:
<code>python3 scripts/test-all-examples.py --only
37-use-method-60s-analysis --mode vm</code> printed <code>PASS PASS
PASS</code> (3 passed, 0 failed; <code>verify.lua</code> reported
<code>PASS 50 / FAIL 0</code> for each of cpp/go/rust). cpu/io/net
saturators tracked their requested <code>--seconds</code> within ~0.2s (a
<code>--resource cpu --seconds 5</code> run printed <code>workers=6</code>,
matching <code>max(4, cpus*3)</code> for this guest's 2 cores, and completed
in 5.002s wall). <code>analyze --seconds 10</code> against an idle guest
completed in 10.417s, not the ~50s five serial sampler invocations would
cost, confirming concurrent execution. The compound load-attribution run (a
background <code>--resource cpu</code> saturator, a concurrent
<code>analyze --seconds 12</code>) moved <code>busy_pct</code> from an idle
1.34 to 100.00 and <code>run_queue</code> from 3.00 to 7.00–8.00, with
Utilization <code>fired=true</code> and <code>verdict resource=cpu</code>,
stable across 3 independent attempts. <code>--resource mem</code> was
confirmed non-OOM on the guest at <code>workers=2</code>,
<code>bytes=5525480448</code> (1.35×this guest's <code>MemTotal</code>), with
<code>free -m</code>/<code>dmesg</code>/<code>journalctl -k</code> checks
showing no OOM-kill and full memory/swap recovery after exit. Not exercised:
a privileged <code>dmesg</code> capture (the guest's unprivileged
<code>fedora</code> account reports <code>status=denied</code>, which
<code>analyze</code> treats as an expected, non-failing path) and any run
outside the lab guest — this is a VM example whose saturation signals are
only meaningful against a real, otherwise-quiet kernel.</p>
