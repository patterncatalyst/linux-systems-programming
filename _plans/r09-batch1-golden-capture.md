---
title: "r09 batch 1 — golden capture from the C++ reference"
layout: plan
render_with_liquid: false
---

# r09 batch 1 — golden capture from the C++ reference

Captured on `systems-target` (the KVM lab guest) against the already-built
C++ reference binaries for `37-use-method-60s-analysis` and
`40-low-latency-fastpath`, per `_plans/r09-batch1-stub-completion.md` steps
S1 and S2. Staging used plain `scp`/`ssh` to `/tmp/lsp37-cpp/app` and
`/tmp/lsp40-cpp/` — never `demo.sh <lang> run` under `TARGET`, per the plan's
warning about `deploy-to-vm.sh`'s `ssh -t` CRLF and fixed `/home/fedora/app`
path. (That stray `/home/fedora/app` from a prior session was in fact found
on the guest during this capture — see the pitfall note below.)

**Read this file's "Calibration findings" section first** — it contains a
critical divergence from the plan's behavioral contract that affects how
`verify.lua` must be written for example 37.

## Guest facts

```
nproc: 2
uname -r: 6.19.10-300.fc44.x86_64
uname -a: Linux systems-target.lab 6.19.10-300.fc44.x86_64 #1 SMP PREEMPT_DYNAMIC Wed Mar 25 18:23:49 UTC 2026 x86_64 GNU/Linux
os-release: Fedora Linux 44 (Cloud Edition)
MemTotal: 3997020 kB (~3.9 GiB)
swap: ~3902 MiB
```

Guest was healthy before, during, and after all captures (`free -m`/`uptime`
checked repeatedly; no OOM-killer messages in `dmesg` at any point).

---

## S1 — example 37 (`loadmix`) golden capture

Binary staged to `/tmp/lsp37-cpp/app` via `scp`, `chmod 755`.

### 1. No args — exit 2

```
$ /tmp/lsp37-cpp/app
EXIT=2
--STDOUT-- (empty)
--STDERR--
usage: app --resource cpu|mem|io|net --seconds N
       app analyze [--seconds N]
```

### 2. `analyze --seconds` (missing value) — exit 2

Identical usage text and exit code to #1.

### 3. `--resource bogus --seconds 5` — exit 2

Identical usage text and exit code to #1.

### 4. `--seconds 0` — exit 2

Identical usage text and exit code to #1.

### 5. `analyze --bogus` — exit 2

Identical usage text and exit code to #1.

All five exit-2 shapes are byte-identical on stderr; stdout is empty in every
case.

### 6. `--resource cpu --seconds 5` — exit 0

```
loadmix: start resource=cpu seconds=5 workers=6
loadmix: done resource=cpu seconds=5 workers=6 ops=16896201 bytes=2347147294612324476
```

`workers=6` matches the contract: `max(4, cpus*3)` with `cpus=2` → `max(4,6)=6`.

### 7. `--resource io --seconds 5` — exit 0

```
loadmix: start resource=io seconds=5 workers=8
loadmix: done resource=io seconds=5 workers=8 ops=24 bytes=393216
```

`workers=8` matches `max(4, cpus*4)` = `max(4,8)=8`. `bytes = ops * 16384`
(24 × 16384 = 393216), consistent with the fixed 16 KiB file contract.

### 8. `--resource net --seconds 5` — exit 0

```
loadmix: start resource=net seconds=5 workers=8
loadmix: done resource=net seconds=5 workers=8 ops=7415 bytes=474560
```

`workers=8` matches `max(4, cpus*4)=8`. `bytes = ops * 64` (7415×64=474560).

### 9. `analyze --seconds 10` (idle), wall-clock timed — exit 0

Wall time: **10.370s** (`time` builtin, `%R`) for `--seconds 10` — confirms
the five interval samplers (`vmstat`, `mpstat`, `iostat`, `sar`, `pidstat`,
each `1 10`) run **concurrently**, not serially (serial would be ~50s).

```
analyze: start seconds=10 cpus=2
analyze: tool uptime raw="04:23:04 up  2:10,  1 user,  load average: 0.00, 0.00, 0.00"
analyze: tool dmesg status=denied
analyze: tool top cpu="%Cpu(s):  0.0 us,  0.0 sy,  0.0 ni,100.0 id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st" tasks="Tasks: 151 total, 1 running, 150 sleep, 0 d-sleep, 0 stopped, 0 zombie"
analyze: tool ss raw="Total: 167"
analyze: tool pidstat active_processes=1
analyze: metric resource=cpu name=busy_pct value=0.40 unit=pct
analyze: metric resource=cpu name=run_queue value=3.00 unit=procs
analyze: metric resource=mem name=used_pct value=13.02 unit=pct
analyze: metric resource=mem name=swap_io value=0.00 unit=kbps
analyze: metric resource=io name=util_pct value=0.19 unit=pct
analyze: metric resource=io name=iowait_pct value=0.00 unit=pct
analyze: metric resource=io name=await_ms value=0.00 unit=ms
analyze: metric resource=net name=pkts value=0.00 unit=per_s
analyze: signal resource=cpu type=Utilization fired=false value=0.40 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=false value=3.00 threshold=4.00
analyze: signal resource=cpu type=Errors fired=false
analyze: signal resource=mem type=Utilization fired=false value=13.02 threshold=75.00
analyze: signal resource=mem type=Saturation fired=false value=0.00 threshold=300.00
analyze: signal resource=mem type=Errors fired=false
analyze: signal resource=io type=Utilization fired=false value=0.19 threshold=40.00
analyze: signal resource=io type=Saturation fired=false value=0.00 threshold=8.00
analyze: signal resource=io type=Errors fired=false
analyze: signal resource=net type=Utilization fired=false value=0.00 threshold=2000.00
analyze: signal resource=net type=Saturation fired=false value=0.00 threshold=8000.00
analyze: signal resource=net type=Errors fired=false
analyze: verdict resource=cpu ratio=0.75
analyze: done
```

Note `dmesg status=denied` — the `fedora` user is unprivileged and `dmesg`
returns `Operation not permitted` on this guest; that is the expected
`denied` path, not a capture error.

### 10. Compound load-attribution run — **see Calibration findings below**

`nohup /tmp/lsp37-cpp/app --resource cpu --seconds 25 >/tmp/bg.out 2>/tmp/bg.err & sleep 2; /tmp/lsp37-cpp/app analyze --seconds 12`

```
EXIT=0 (analyze)
analyze: start seconds=12 cpus=2
...
analyze: metric resource=cpu name=busy_pct value=0.46 unit=pct
analyze: metric resource=cpu name=run_queue value=3.00 unit=procs
...
analyze: signal resource=cpu type=Utilization fired=false value=0.46 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=false value=3.00 threshold=4.00
...
analyze: verdict resource=cpu ratio=0.75
analyze: done

--BG-STDOUT--
loadmix: start resource=cpu seconds=25 workers=6
loadmix: done resource=cpu seconds=25 workers=6 ops=6129699 bytes=9716437789073071330
```

**This does NOT show load attribution** — `busy_pct` stayed at idle levels
(0.46, versus 0.40 idle baseline) despite a concurrent `--resource cpu
--seconds 25` background job. Root cause identified below: the background
job actually finishes in milliseconds, not 25 seconds, so by the time
`analyze`'s samplers start polling it is long gone.

### `--resource mem` — special investigation (run last)

Guest health immediately before:

```
Mem: total=3903MiB used=508MiB free=3147MiB available=3395MiB
Swap: total=3902MiB used=10MiB
load average: 0.31
```

**Test A** — `timeout 60 /tmp/lsp37-cpp/app --resource mem --seconds 2`:

```
elapsed=2.342s (wall clock, via `time`)
EXIT=0
loadmix: start resource=mem seconds=2 workers=2
loadmix: done resource=mem seconds=2 workers=2 ops=0 bytes=5525480448
```

**Test B** — `timeout 60 /tmp/lsp37-cpp/app --resource mem --seconds 8`:

```
elapsed=2.775s (wall clock — NOT ~8s, see Calibration findings)
EXIT=0
loadmix: start resource=mem seconds=8 workers=2
loadmix: done resource=mem seconds=8 workers=2 ops=674498 bytes=5525480448
```

Guest health immediately after test B:

```
Mem: total=3903MiB used=561MiB free=3352MiB available=3341MiB
Swap: total=3902MiB used=17MiB
load average: 0.35
sudo dmesg | tail -20: no OOM-killer activity; only SELinux/clocksource lines
```

**Verdict: `mem` is safe on this guest.** `workers=2` = `max(2, cpus)`.
`bytes=5525480448` = `target = MemTotal_kB(3997020) * 1024 * 1.35` exactly.
It exits 0 both times, does not crash or destabilize the guest, and swap
usage barely moved (10→17 MiB). **`verify.lua` may assert exit 0 for `mem`.**
However — see Calibration findings — the "seconds" argument is not honored
here either (2.3–2.8s elapsed regardless of `--seconds 2` vs `--seconds 8`),
so `verify.lua` must not assert wall-clock duration for `mem`, only exit 0,
the `workers=2` value, and `bytes` equal to the computed target.

One incidental discovery while investigating clock behavior on this guest:
`dmesg` (captured via `sudo`, since the app's own unprivileged capture is
`denied`) shows the kernel marking the guest's TSC unstable and falling back
to `kvm-clock`:

```
[  222.788514] clocksource: timekeeping watchdog on CPU0: Marking clocksource 'tsc' as unstable...
[  222.793981] tsc: Marking TSC unstable due to clocksource watchdog
```

This is a nested-KVM artifact, not the cause of the saturate-mode bug below
(that bug reproduces at millisecond scale, far too fast to be a TSC skew
issue, and `steady_clock` reads via `kvm-clock` remain monotonic and
consistent with wall time — confirmed by the `analyze --seconds 10` timing
in capture #9 above, which matched 10s almost exactly).

---

## S2 — example 40 (`chatterd-fastpath`) golden capture

`app` and `run-latency-bench.sh` staged to `/tmp/lsp40-cpp/`, both
`chmod 755`.

### 1. Usage / exit-2 shapes — all identical, all exit 2

```
usage: app fastpath --port P --pin CPU [--busy-poll]
       app naive --port P
       app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]
```

Confirmed byte-identical for: no args; `frobnicate`; `fastpath --port 9999`
(no `--pin`); `naive` (no port); `measure --target 127.0.0.1:9999` (no
`--n`). Stdout empty in every case.

### 2. `measure --target 127.0.0.1:1 --n 10` — exit 1

```
app: error: connect 127.0.0.1:1: Connection refused
```

### 3. `measure --target notanip:9999 --n 10` — exit 1

```
app: error: notanip: not an IPv4 address
```

### 4. `fastpath --port 19510 --pin 99` — exit 1

```
app: error: cpu 99 out of range (0..1)
```

(`0..1` because `nproc=2` on this guest → `sysconf(_SC_NPROCESSORS_ONLN)-1=1`.)

### 5. Full `bash run-latency-bench.sh /tmp/lsp40-cpp/app 20000 500` — 3 runs, all exit 0

**Run 1:**

```
app: nproc=2 pin_cpu=1 client_cpu=0
app: naive-cpus-allowed 0-1
app: fastpath-cpus-allowed 1
app: === naive measure trial 1/3 ===
app: percentiles_ns tag=naive-1 p50=13319 p90=19388 p99=30516 p99.9=80617 min=9714 max=117205 mean=14639.83 n=20000
app: === fastpath measure trial 1/3 ===
app: percentiles_ns tag=fastpath-1 p50=7495 p90=9464 p99=17882 p99.9=56822 min=6190 max=187663 mean=8218.93 n=20000
app: === naive measure trial 2/3 ===
app: percentiles_ns tag=naive-2 p50=11005 p90=14279 p99=27274 p99.9=68718 min=9450 max=358138 mean=12009.51 n=20000
app: === fastpath measure trial 2/3 ===
app: percentiles_ns tag=fastpath-2 p50=7745 p90=10172 p99=19281 p99.9=59792 min=5654 max=317138 mean=8450.89 n=20000
app: === naive measure trial 3/3 ===
app: percentiles_ns tag=naive-3 p50=11373 p90=15496 p99=30172 p99.9=75192 min=9405 max=270060 mean=12534.06 n=20000
app: === fastpath measure trial 3/3 ===
app: percentiles_ns tag=fastpath-3 p50=7922 p90=10403 p99=20461 p99.9=55899 min=6496 max=85982 mean=8606.73 n=20000
EXIT=0
```

median(naive p50) = 11373, median(fastpath p50) = 7745, ratio = **1.468**

**Run 2:**

```
app: nproc=2 pin_cpu=1 client_cpu=0
app: naive-cpus-allowed 0-1
app: fastpath-cpus-allowed 1
naive p50s: 11236, 10684, 11375  →  median 11236
fastpath p50s: 7553, 8172, 7240  →  median 7553
EXIT=0
```

ratio = **1.488**

**Run 3:**

```
app: nproc=2 pin_cpu=1 client_cpu=0
app: naive-cpus-allowed 0-1
app: fastpath-cpus-allowed 1
naive p50s: 10666, 10631, 10655  →  median 10655
fastpath p50s: 7694, 7475, 7390  →  median 7475
EXIT=0
```

ratio = **1.425**

In every run: `naive-cpus-allowed` = `0-1` (the full online set — matches
contract, naive's affinity mask is left untouched); `fastpath-cpus-allowed`
= `1` (exactly `PIN_CPU`, a single value — matches contract). Every
`percentiles_ns` line has `n=20000` and satisfies
`min <= p50 <= p90 <= p99 <= p99.9 <= max`.

### 6. SIGINT shutdown of `naive --port 19601` — exit 0

```
app: naive listening on 0.0.0.0:19601
app: naive shutting down
```

### 7. SIGINT shutdown of `fastpath --port 19602 --pin 0 --busy-poll` — exit 0

```
app: fastpath listening on 0.0.0.0:19602 pinned-cpu=0 busy-poll=on
app: fastpath shutting down
```

### Bonus (not explicitly requested, captured for completeness — the full plan file also mentions SIGTERM)

SIGTERM shutdown of `naive --port 19603` — exit 0, same shutdown line.
SIGTERM shutdown of `fastpath --port 19604 --pin 1` (no `--busy-poll`) —
exit 0:

```
app: fastpath listening on 0.0.0.0:19604 pinned-cpu=1 busy-poll=off
app: fastpath shutting down
```

---

## Calibration findings

### 1. CRITICAL — example 37 saturate mode does not honor `--seconds` (all four resources)

**This is a real bug in the already-built C++ reference binary, reproducible
independent of this guest's environment, confirmed by reading
`examples/37-use-method-60s-analysis/cpp/src/main.cpp`.** It is not a clock
or nested-KVM artifact.

Empirical evidence — foreground, wall-clock timed:

| invocation | requested seconds | actual elapsed |
|---|---|---|
| `--resource cpu --seconds 1` | 1 | 0.007s |
| `--resource cpu --seconds 3` | 3 | 0.008s |
| `--resource cpu --seconds 6` | 6 | 0.008s |
| `--resource cpu --seconds 20` | 20 | 0.006s |
| `--resource cpu --seconds 60` | 60 | 0.006s |
| `--resource io --seconds 6` | 6 | 0.058s |
| `--resource net --seconds 6` | 6 | 0.220s |
| `--resource mem --seconds 2` | 2 | 2.342s (dominated by allocation, see below) |
| `--resource mem --seconds 8` | 8 | 2.775s |

`cpu`, `io`, and `net` all exit in well under a second regardless of the
requested duration — never anywhere near N seconds.

**Root cause** (read from `sat_cpu`/`sat_mem`/`sat_io`/`sat_net`, lines
~297–457 of `main.cpp`): each function computes a `deadline`, spawns
`workers` `std::jthread`s into a local `std::vector<std::jthread> pool`, and
the enclosing `{ }` block ends **immediately after the spawn loop** — there
is no `std::this_thread::sleep_until(deadline)` (or any wait) in the parent
thread before the block closes. When the block ends, `pool`'s destructor
runs, and `std::jthread`'s destructor calls `request_stop()` then `join()`
for each element **in sequence**: element 0 is stopped and joined to
completion before element 1 even receives its `request_stop()`. Since
`request_stop()` for the first worker fires within microseconds of thread
creation, and each worker's own loop condition is `!stop_requested() && now
< deadline` (stop-requested checked first, short-circuiting), the workers
exit almost immediately — bounded only by how long the *first* full loop
body takes (a handful of `2^20`-iteration batches for `cpu`, one syscall for
`io`/`net`), never by the actual `--seconds` value. The `deadline` variable
is effectively dead: it can only matter if the deadline is *shorter* than
the natural stop-propagation delay, which never happens with real durations.

`mem`'s longer, roughly duration-independent ~2.3–2.8s wall time is a
*different* symptom of the exact same missing-wait bug: each worker's
`std::vector<uint8_t> buf(chunk, 0)` (a ~2.6 GiB per-worker zero-fill, given
`workers=2` here) dominates the elapsed time, and the deadline/stop-request
race resolves *during or immediately after* that allocation rather than
after any real "touch until deadline" loop. At `--seconds=2` the whole
budget is consumed by allocation and `ops=0` (zero touches); at
`--seconds=8` one worker gets a partial pass in before being stopped
(`ops=674498` ≈ one full pass over a 2.6 GiB buffer at 4096-byte strides),
still nowhere near 8 seconds of sustained touching.

**Isolation proof that this is a saturate-mode-only bug, not a parsing bug:**
a genuine sustained load (`yes >/dev/null &` ×4, which really does peg both
vCPUs) produces exactly the behavior the contract describes:

```
analyze: metric resource=cpu name=busy_pct value=100.00 unit=pct
analyze: metric resource=cpu name=run_queue value=6.00 unit=procs
analyze: signal resource=cpu type=Utilization fired=true value=100.00 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=true value=6.00 threshold=4.00
analyze: verdict resource=cpu ratio=1.50
```

So `analyze`'s subprocess orchestration, column-name parsing, and
threshold/verdict logic are all correct as specified. The bug is entirely
contained in `sat_cpu`/`sat_mem`/`sat_io`/`sat_net`'s missing wait.

**Why this matters for S3 and beyond:**

- The plan's own load-attribution assertion (S3 step 6 / acceptance
  criterion 5: "under concurrent `--resource cpu` load, `busy_pct` and
  `run_queue` rise materially above idle, `Utilization fired=true`,
  `verdict resource=cpu`") **will fail against the current C++ binary** if
  driven exactly as the plan describes (`nohup app --resource cpu --seconds
  N &`, then `analyze`), because the background load is gone in
  milliseconds — this capture's own compound run (#10 above) demonstrates
  the failure directly.
- If Go and Rust are written to literally honor the documented contract
  ("saturate that resource" for the given duration — the plausible reading
  of the prose in the plan), they will *actually* run for N seconds. That
  would make the load-attribution test **pass for Go/Rust but fail for
  C++**, inverting the cross-language-parity goal the whole golden-capture
  approach is built on, and breaking "S3 must pass with `LSP_LANG=cpp`
  before a single line of Go or Rust exists."
- Recommendation: **this is very likely a bug that should be fixed in the
  C++ source** (add `std::this_thread::sleep_until(deadline);` in the
  parent thread of each `sat_*` function, before the `pool` block closes)
  before S3 is written, so that the golden-capture oracle reflects intended,
  correct, sustained-duration behavior. Absent a fix, `verify.lua`'s
  load-attribution check cannot be written against this binary's actual
  behavior without either (a) skipping/weakening it for `cpu` specifically,
  contradicting acceptance criterion 5, or (b) using the `yes`-style
  external-load workaround demonstrated above instead of the app's own
  `--resource cpu` saturator — which changes the test's meaning from "prove
  our own load generator is visible to our own analyzer" to "prove the
  analyzer sees load, full stop," a materially weaker claim than the plan
  intends.
- This does **not** affect the `worker=` count or `ops>0` assertions
  (acceptance criterion 3): those still pass, since `ops` is always > 0 (a
  partial batch still counts), and `workers` is computed and printed before
  any of this executes.
- Wall-clock concurrency of the five `analyze` samplers (criterion 7 / plan
  step 7) is unaffected and confirmed correct (10.37s for `--seconds 10`).

### 2. `--resource mem` survivability — SAFE, but does not honor duration either

Exit 0 both times tested, guest remained healthy (no OOM, swap barely
touched), `workers=2` and `bytes=5525480448` match the formula exactly.
`verify.lua` can assert exit 0, `workers=`, and the `bytes=` value for `mem`,
but must not assert anything about wall-clock duration or `ops` (the latter
can legitimately be 0 at short `--seconds` values, per the mechanism above).
Per the plan's own risk mitigation, `mem` was run last in S1, after
everything else, and the guest was confirmed stable afterward.

### 3. Example 40 fastpath-vs-naive margin — stable, not marginal

| run | naive median p50 (ns) | fastpath median p50 (ns) | ratio |
|---|---|---|---|
| 1 | 11373 | 7745 | 1.468 |
| 2 | 11236 | 7553 | 1.488 |
| 3 | 10655 | 7475 | 1.425 |

Fastpath is consistently **~30–33% faster** (never inverted, never close)
across three independent full bench runs on this 2-vCPU nested-KVM guest.
This is a comfortable margin, not one requiring hedging. Recommend
calibrating the `verify.lua` threshold conservatively below the observed
floor — e.g. assert `median(fastpath p50) < median(naive p50) * 0.90`
(a 10% requirement against an observed ~30%+ margin) — to tolerate a noisier
CI run without becoming a tautology. Both `naive-cpus-allowed` (`0-1`, the
full online set) and `fastpath-cpus-allowed` (`1`, the single pinned CPU)
were exact and identical across all three runs.

### 4. Sampler concurrency (example 37 `analyze`) — confirmed correct

`analyze --seconds 10` completed in 10.370s wall-clock, consistent with the
five interval samplers (`vmstat`, `mpstat`, `iostat`, `sar`, `pidstat`, each
run as `<tool> ... 1 10`) running concurrently rather than serially (serial
would be ~50s). No divergence here.

### 5. Pitfall encountered and worked around (documented for S3/S4 authors)

A stale `/home/fedora/app` binary (an old build of example 40, from a prior
`deploy-to-vm.sh`-driven session) was found already present on the guest
during this capture. It was never touched or removed — captures in this
file all used explicit `/tmp/lsp37-cpp/app` / `/tmp/lsp40-cpp/app` absolute
paths — but its presence is exactly the hazard the plan's staging design
(scp to `/tmp/lsp<NN>-<lang>/app` with plain `ssh`, never `demo.sh <lang> run`
under `TARGET`) is meant to avoid: a compound remote command that
constructs a `cd dir && cmd1 & sleep N; cmd2` (background one command with a
`cd` prefix, then a second command without repeating that `cd` or an
absolute path) will silently execute the second command from the SSH
session's default directory (`/home/fedora`) and can pick up this stale
binary instead of the one under test. `verify.lua` must always use full
absolute paths for every invocation in a compound remote command, never rely
on a leading `cd` to apply to later `;`-separated commands in the same
string.
