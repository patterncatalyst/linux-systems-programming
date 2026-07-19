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

**UPDATE (re-capture against commit `f2b2dba`):** the example-37 saturator
bug described below (all four `sat_*` functions exiting in milliseconds
regardless of `--seconds`) is **fixed** as of commit `f2b2dba`
(`fix(demo-37): saturators now honor --seconds instead of exiting
instantly`), which adds `std::this_thread::sleep_until(deadline)` inside each
`sat_*` pool scope so `~jthread`'s `request_stop()`-before-`join()` ordering
no longer kills workers on their first iteration. **The entire "S1 —
example 37" section below has been re-captured from scratch against the
rebuilt, fixed binary** (verified staged via `md5sum` match between the
host build and the guest copy, and via a live timing check —
`--resource cpu --seconds 4` took 4.002s wall on the guest, not
milliseconds — before any capture began). **All example-37 numbers in this
version supersede the pre-fix numbers; the pre-fix numbers were invalid and
must not be used to write `verify.lua`.** Example-40 sections (S2 golden
capture and Calibration finding 3) are untouched by this update — they were
never affected by the example-37 bug.

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

## S1 — example 37 (`loadmix`) golden capture — **RE-CAPTURED post-`f2b2dba`**

**This entire section is a fresh capture against the fixed binary, replacing
the pre-fix capture in full.** Host build directory
`examples/37-use-method-60s-analysis/cpp/build/release/app` was confirmed to
already reflect commit `f2b2dba` (working tree clean, `ninja` reported no
outstanding work); staged to `/tmp/lsp37-cpp/app` on the guest via `scp`,
overwriting a stale pre-fix copy left over from the earlier capture session,
`chmod 755`. `md5sum` matched byte-for-byte between host and guest
(`48ec7371b960552a39efc2fdbf658d72`) before any capture began. A pre-capture
sanity check confirmed the fix is live on the guest: `time app --resource
cpu --seconds 4` → `real 0m4.002s` (was ~0.007s pre-fix).

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
case. Unchanged from the pre-fix capture, as expected — the fix only touches
the `sat_*` functions' internal duration handling, not argument parsing.

### 6. `--resource cpu --seconds 5` — exit 0, timed — **now ~5s**

```
loadmix: start resource=cpu seconds=5 workers=6
loadmix: done resource=cpu seconds=5 workers=6 ops=8801461550 bytes=11315899737308079569

real	0m5.002s
user	0m9.219s
sys	0m0.001s
```

`workers=6` matches the contract: `max(4, cpus*3)` with `cpus=2` →
`max(4,6)=6`. Wall time **5.002s** — matches `--seconds 5` almost exactly
(pre-fix: 0.007s regardless of `--seconds`). `ops` is now ~9 orders of
magnitude larger than the pre-fix capture (16896201 → 8801461550) since the
workers genuinely spin for the full duration instead of exiting on their
first loop check.

### 7. `--resource io --seconds 5` — exit 0, timed — **now ~5s**

```
loadmix: start resource=io seconds=5 workers=8
loadmix: done resource=io seconds=5 workers=8 ops=4131 bytes=67682304

real	0m5.012s
user	0m0.021s
sys	0m0.361s
```

`workers=8` matches `max(4, cpus*4)` = `max(4,8)=8`. `bytes = ops * 16384`
(4131 × 16384 = 67682304), consistent with the fixed 16 KiB file contract.
Wall time **5.012s** (pre-fix: 0.058s).

### 8. `--resource net --seconds 5` — exit 0, timed — **now ~5s**

```
loadmix: start resource=net seconds=5 workers=8
loadmix: done resource=net seconds=5 workers=8 ops=6587351 bytes=421590464

real	0m5.207s
user	0m0.915s
sys	0m8.315s
```

`workers=8` matches `max(4, cpus*4)=8`. `bytes = ops * 64`
(6587351 × 64 = 421590464). Wall time **5.207s** (pre-fix: 0.220s) — the
small overshoot past 5.0s (vs. cpu/io's tighter ~5.00–5.01s) is consistent
with `sat_net`'s two-pool structure (sender pool joins inside the `sleep_until`
scope, then a receiver thread is stopped and joined afterward, noticing
`stop` within ~200ms per the source comment).

### 9. `analyze --seconds 10` (idle), wall-clock timed — exit 0

Wall time: **10.417s** (`time` builtin, `%R`) for `--seconds 10` — confirms
the five interval samplers (`vmstat`, `mpstat`, `iostat`, `sar`, `pidstat`,
each `1 10`) still run **concurrently**, not serially (serial would be
~50s). This capture ran immediately after captures 6–8 (back-to-back
saturator runs), so the baseline is slightly elevated versus a cold-guest
idle reading — noted here because capture 10 below is compared against
this baseline, not a cold one.

```
analyze: start seconds=10 cpus=2
analyze: tool uptime raw="05:51:20 up  3:39,  1 user,  load average: 1.78, 0.45, 0.15"
analyze: tool dmesg status=denied
analyze: tool top cpu="%Cpu(s):  0.0 us,  0.0 sy,  0.0 ni,100.0 id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st" tasks="Tasks: 149 total, 2 running, 147 sleep, 0 d-sleep, 0 stopped, 0 zombie"
analyze: tool ss raw="Total: 167"
analyze: tool pidstat active_processes=4
analyze: metric resource=cpu name=busy_pct value=1.34 unit=pct
analyze: metric resource=cpu name=run_queue value=3.00 unit=procs
analyze: metric resource=mem name=used_pct value=12.30 unit=pct
analyze: metric resource=mem name=swap_io value=0.00 unit=kbps
analyze: metric resource=io name=util_pct value=8.20 unit=pct
analyze: metric resource=io name=iowait_pct value=0.59 unit=pct
analyze: metric resource=io name=await_ms value=5.33 unit=ms
analyze: metric resource=net name=pkts value=0.00 unit=per_s
analyze: signal resource=cpu type=Utilization fired=false value=1.34 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=false value=3.00 threshold=4.00
analyze: signal resource=cpu type=Errors fired=false
analyze: signal resource=mem type=Utilization fired=false value=12.30 threshold=75.00
analyze: signal resource=mem type=Saturation fired=false value=0.00 threshold=300.00
analyze: signal resource=mem type=Errors fired=false
analyze: signal resource=io type=Utilization fired=false value=8.20 threshold=40.00
analyze: signal resource=io type=Saturation fired=false value=0.59 threshold=8.00
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

### 10. Compound load-attribution run — **now WORKS, run 3× for stability**

`nohup /tmp/lsp37-cpp/app --resource cpu --seconds 25 >/tmp/bgN.out 2>/tmp/bgN.err & sleep 2; /tmp/lsp37-cpp/app analyze --seconds 12`

This is the run S3's load-attribution assertion depends on, and it was
previously impossible to satisfy (see the superseded pre-fix Calibration
finding). Run 3 times back-to-back; all three attempts show strong,
unambiguous attribution:

| attempt | busy_pct | run_queue | Utilization fired | Saturation fired | verdict ratio |
|---|---|---|---|---|---|
| 1 | 100.00 | 8.00 | true | true | 2.00 |
| 2 | 100.00 | 8.00 | true | true | 2.00 |
| 3 | 100.00 | 7.00 | true | true | 1.75 |

Full output, attempt 1:

```
analyze: start seconds=12 cpus=2
analyze: tool uptime raw="05:51:41 up  3:39,  1 user,  load average: 1.82, 0.56, 0.20"
analyze: tool dmesg status=denied
analyze: tool top cpu="%Cpu(s): 97.8 us,  2.2 sy,  0.0 ni,  0.0 id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st" tasks="Tasks: 150 total, 1 running, 149 sleep, 0 d-sleep, 0 stopped, 0 zombie"
analyze: tool ss raw="Total: 167"
analyze: tool pidstat active_processes=3
analyze: metric resource=cpu name=busy_pct value=100.00 unit=pct
analyze: metric resource=cpu name=run_queue value=8.00 unit=procs
analyze: metric resource=mem name=used_pct value=12.35 unit=pct
analyze: metric resource=mem name=swap_io value=0.00 unit=kbps
analyze: metric resource=io name=util_pct value=0.00 unit=pct
analyze: metric resource=io name=iowait_pct value=0.00 unit=pct
analyze: metric resource=io name=await_ms value=0.00 unit=ms
analyze: metric resource=net name=pkts value=0.00 unit=per_s
analyze: signal resource=cpu type=Utilization fired=true value=100.00 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=true value=8.00 threshold=4.00
analyze: signal resource=cpu type=Errors fired=false
analyze: signal resource=mem type=Utilization fired=false value=12.35 threshold=75.00
analyze: signal resource=mem type=Saturation fired=false value=0.00 threshold=300.00
analyze: signal resource=mem type=Errors fired=false
analyze: signal resource=io type=Utilization fired=false value=0.00 threshold=40.00
analyze: signal resource=io type=Saturation fired=false value=0.00 threshold=8.00
analyze: signal resource=io type=Errors fired=false
analyze: signal resource=net type=Utilization fired=false value=0.00 threshold=2000.00
analyze: signal resource=net type=Saturation fired=false value=0.00 threshold=8000.00
analyze: signal resource=net type=Errors fired=false
analyze: verdict resource=cpu ratio=2.00
analyze: done
ANALYZE_EXIT=0
BG_EXIT=0

--BG-STDOUT--
loadmix: start resource=cpu seconds=25 workers=6
loadmix: done resource=cpu seconds=25 workers=6 ops=44164896060 bytes=5514586990755454969
```

Attempts 2 and 3 (metric/signal/verdict lines only — the rest of the output
shape was identical to attempt 1 in both):

```
# attempt 2
analyze: metric resource=cpu name=busy_pct value=100.00 unit=pct
analyze: metric resource=cpu name=run_queue value=8.00 unit=procs
analyze: signal resource=cpu type=Utilization fired=true value=100.00 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=true value=8.00 threshold=4.00
analyze: verdict resource=cpu ratio=2.00
ANALYZE_EXIT=0
BG_EXIT=0
loadmix: start resource=cpu seconds=25 workers=6
loadmix: done resource=cpu seconds=25 workers=6 ops=44886125692 bytes=17660584183443754701

# attempt 3
analyze: metric resource=cpu name=busy_pct value=100.00 unit=pct
analyze: metric resource=cpu name=run_queue value=7.00 unit=procs
analyze: signal resource=cpu type=Utilization fired=true value=100.00 threshold=60.00
analyze: signal resource=cpu type=Saturation fired=true value=7.00 threshold=4.00
analyze: verdict resource=cpu ratio=1.75
ANALYZE_EXIT=0
BG_EXIT=0
loadmix: start resource=cpu seconds=25 workers=6
loadmix: done resource=cpu seconds=25 workers=6 ops=44575950456 bytes=8335093140041756969
```

`busy_pct` rose from the idle baseline (1.34, capture 9) to **100.00** in
all three attempts — nowhere near the 60.00 threshold's margin. `run_queue`
rose from the idle baseline (3.00) to **7.00–8.00**, comfortably clearing
both proposed gate conditions from the plan (`busy_pct >= 60` and
`run_queue >= nproc+2 = 4`). **`Utilization fired=true` and `verdict
resource=cpu` fired identically in all three attempts — STABLE, no
flakiness observed.** This directly reverses the pre-fix finding (which
showed the background job invisible to `analyze` because it had already
exited in milliseconds); the background `loadmix` process is now still
running and genuinely saturating both vCPUs for the entire 12-second
`analyze` window (and beyond — it was launched for `--seconds 25`).

### 11. `--resource mem` — re-tested under real sustained load (run last)

**This is the highest-risk item in this re-capture.** Pre-fix, `mem` only
ran for ~2.3–2.8s regardless of `--seconds` (the same missing-wait bug, see
superseded finding below) and so never actually sustained 1.35×MemTotal of
touched memory for long — it was never a real OOM test. Post-fix, `mem` now
holds `workers=2` threads each touching a `target_bytes/2 ≈ 2635 MiB` chunk
for the genuine requested duration, so this needed to be re-verified as an
actual sustained-memory-pressure test, not just an argument-parsing check.

Guest health immediately before (after a fresh `free -m`/`uptime` check,
following the cpu/io/net/analyze captures above):

```
Mem: total=3903MiB used=485MiB free=3399MiB available=3418MiB
Swap: total=3902MiB used=15MiB
load average: 3.96, 1.63, 0.61   (elevated from the immediately preceding cpu saturation runs)
```

**Test A** — `time /tmp/lsp37-cpp/app --resource mem --seconds 2`, with a
0.5s-interval `free -m` monitor running concurrently:

```
loadmix: start resource=mem seconds=2 workers=2
loadmix: done resource=mem seconds=2 workers=2 ops=0 bytes=5525480448

real	0m2.415s
user	0m0.546s
sys	0m2.268s
```

Memory trace during the run (used / total, swap):

```
t+0.0s: 3058MiB used / 3903MiB total, avail=844MiB,  swap=15MiB
t+0.6s: 3764MiB used / 3903MiB total, avail=138MiB,  swap=490MiB
t+1.1s: 3765MiB used / 3903MiB total, avail=137MiB,  swap=1236MiB
t+1.7s: 3764MiB used / 3903MiB total, avail=138MiB,  swap=1933MiB
t+2.2s (post-exit): 463MiB used / 3903MiB total, avail=3439MiB, swap=19MiB
```

Elapsed **2.415s** — now honors `--seconds 2` closely (pre-fix: 2.342s, but
that was coincidental — see below). Memory pressure genuinely climbed
(swap 15→1933 MiB, available RAM dropped to 137 MiB) then fully released
within ~0.2s of process exit, back to 19 MiB swap. Guest survived cleanly;
`ops=0` (no full touch pass completed in 2s once the process's own
allocation overhead is accounted for) — this remains a legitimate value per
the pre-fix note, unchanged.

**Test B** — `time /tmp/lsp37-cpp/app --resource mem --seconds 8`, same
monitoring:

```
loadmix: start resource=mem seconds=8 workers=2
loadmix: done resource=mem seconds=8 workers=2 ops=2697992 bytes=5525480448

real	0m12.624s
user	0m1.243s
sys	0m11.252s
```

Memory trace (20 samples over the run, condensed): used climbed to
~3700–3790 MiB within the first second and *stayed there* for the entire
run (unlike Test A's brief spike, this is now a **sustained** plateau, as
expected for a genuinely duration-honoring saturator); swap stabilized
around 1920–2010 MiB for the last ~9 seconds of the run, available RAM
stayed pinned around 110–190 MiB the whole time. Post-exit: memory and swap
returned to baseline (`used=434MiB`, `swap=27MiB`) within ~1s.

**New finding — duration overrun under swap pressure:** elapsed was
**12.624s for a requested `--seconds 8`**, a ~4.6s overrun, not the ~5.0–5.2s
tight match seen for cpu/io/net. Root cause (inferred from source): the
outer `sleep_until(deadline)` in the parent thread still fires at the
correct wall-clock time and `request_stop()` propagates immediately after,
but each worker's inner `for (size_t off = 0; off < buf.size(); off +=
4096)` touch loop has **no stop-check inside it** — only the outer `while
(!stop_requested() && now < deadline)` is checked between passes. Once
pages have been pushed to swap, a single inner pass over a ~2635 MiB chunk
at 4 KiB stride means hundreds of thousands of potential page-fault/swap-in
round trips, so the *in-flight* pass at the moment `request_stop()` fires
can take several extra seconds to finish before `join()` returns and the
process can exit. This is a distinct, more precise finding than the pre-fix
report (which saw no duration-honoring behavior at all for `mem`); post-fix,
`mem` **does** honor `--seconds` as a floor but can overrun it by several
seconds under real swap pressure, scaling with how deep into swap the run
gets.

Guest health immediately after Test B — checked thoroughly, including an
explicit OOM search across `dmesg` and `journalctl -k`:

```
Mem: total=3903MiB used=434MiB free=3513MiB available=3469MiB
Swap: total=3902MiB used=27MiB
load average: 2.55, 1.57, 0.64  (settling; a follow-up check ~90s later showed 0.86)

dmesg -T | grep -iE "oom-kill|out of memory|killed process"  → NONE FOUND
journalctl -k --since "-5 min" | grep -iE "oom|killed process" → NONE FOUND
```

No leftover `app` processes; no leftover `/var/tmp/loadmix-io-*` directories
(the `io` saturator's own cleanup ran correctly in captures 7/6). SSH
remained responsive throughout every test.

**Verdict: `mem` remains SAFE on this guest, now confirmed under real,
sustained 1.35×MemTotal memory pressure for the full requested duration —
not just the brief allocation spike the pre-fix binary produced.** No
OOM-kill at any point across two tests. `workers=2` = `max(2, cpus)`.
`bytes=5525480448` = `target = MemTotal_kB(3997020) * 1024 * 1.35` exactly,
unchanged from pre-fix (this value was never affected by the bug). `ops` is
now genuinely non-zero at `--seconds 8` (2697992, an order of magnitude
above the pre-fix capture's 674498, consistent with real sustained
touching rather than a bug-truncated partial pass) and legitimately zero at
`--seconds 2` (allocation alone consumes most of the budget). **`verify.lua`
may assert exit 0, the `workers=2` value, and the `bytes=` target for `mem`
as before, but must still not assert a tight wall-clock bound** — the
overrun under swap pressure is real and can vary with guest load; a
generous upper bound (e.g. requested seconds × 3, or a fixed ceiling like
30s) would be safer than assuming near-exact duration matching for `mem`
specifically, unlike cpu/io/net which now track `--seconds` tightly (within
~0.2s).

One incidental discovery while investigating clock behavior on this guest,
carried forward from the pre-fix capture (still relevant, unrelated to
either bug): `dmesg` (captured via `sudo`, since the app's own unprivileged
capture is `denied`) shows the kernel marking the guest's TSC unstable and
falling back to `kvm-clock`:

```
[  222.788514] clocksource: timekeeping watchdog on CPU0: Marking clocksource 'tsc' as unstable...
[  222.793981] tsc: Marking TSC unstable due to clocksource watchdog
```

This is a nested-KVM artifact and does not affect `steady_clock`-based
deadline computation — confirmed again by this recapture's timings all
tracking their requested `--seconds` correctly (cpu/io/net within ~0.2s;
`analyze --seconds 10` at 10.417s).

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

### 1. RESOLVED (was CRITICAL) — example 37 saturate mode now honors `--seconds`

**Status as of the `f2b2dba` re-capture: FIXED.** cpu/io/net saturators now
run for very close to the requested `--seconds` (within ~0.2s, see S1
captures 6–8 above), and the load-attribution compound run (S1 capture 10)
now succeeds — `busy_pct` rises from an idle baseline of ~1.3% to 100.00%,
`run_queue` from ~3 to 7–8, `Utilization fired=true`, and `verdict
resource=cpu` all fire, **stably across 3 independent attempts**. `mem` now
genuinely sustains 1.35×MemTotal memory pressure for the full duration
rather than exiting during allocation, though it can *overrun* its
requested duration by several seconds under real swap pressure (see finding
2, updated below) — a new, narrower nuance replacing the old "duration not
honored at all" finding.

**Practical effect on S3:** the plan's load-attribution assertion (S3 step
6 / acceptance criterion 5) **can now be written against the actual
`--resource cpu` saturator as originally intended** — no need for the
`yes`-style external-load workaround or for skipping/weakening the
assertion for `cpu`. The cross-language-parity concern raised below (Go/Rust
honoring the contract while C++ didn't) no longer applies: all three
languages can now be held to the same "saturate for N seconds, and
`analyze` sees it" standard, since the C++ reference itself finally does.

**Recommended gate, per the plan's own hedge:** retry up to 3 attempts,
gating on `busy_pct >= 60` **or** `run_queue >= nproc+2` (`>= 4` on this
guest) — both conditions were cleared by an enormous margin in every
attempt (100.00 vs. 60 threshold; 7–8 vs. 4 threshold), so this gate has
comfortable headroom against a noisier CI run, not a tautology.

The original (superseded) root-cause writeup is preserved below for
context — it explains *why* the bug existed and is exactly what commit
`f2b2dba` fixed (adding `std::this_thread::sleep_until(deadline)` inside
each `sat_*` pool scope, so `~jthread`'s `request_stop()`-before-`join()`
ordering becomes the intended post-deadline signal instead of an immediate
kill).

---

**SUPERSEDED — pre-fix analysis, kept for context:**

**This was a real bug in the pre-fix C++ reference binary, reproducible
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

### 2. `--resource mem` survivability — **RE-TESTED under real sustained load, still SAFE**

**Updated as of the `f2b2dba` re-capture.** The pre-fix testing below never
actually exercised sustained memory pressure (the process exited during
allocation, in ~2.3–2.8s regardless of `--seconds`), so it was not a real
OOM test. Post-fix, `mem` genuinely holds ~2635 MiB/worker × 2 workers
(≈5271 MiB total, exceeding this guest's 3903 MiB RAM) touched for the
requested duration, pushing 1.9–2.0 GiB into swap and driving available RAM
down to ~110–190 MiB for the full run. **Re-tested at `--seconds 2` and
`--seconds 8` with continuous `free -m` monitoring and an explicit
post-run `dmesg`/`journalctl` OOM search: no OOM-kill at either duration,
guest remained fully SSH-responsive throughout, and memory/swap fully
recovered to baseline within ~1s of process exit.** `workers=2` and
`bytes=5525480448` still match the formula exactly (unaffected by the bug
or the fix). `ops` is now legitimately non-zero even at `--seconds 8`
(2697992, up from a bug-truncated 674498 pre-fix) and remains legitimately
0 at `--seconds 2` (allocation alone consumes the short budget).

**New nuance the pre-fix capture could not see: `mem` can *overrun* its
requested `--seconds` by several seconds under real swap pressure** — Test B
requested 8s and took 12.624s wall, because the per-worker touch loop has no
stop-check *inside* its inner `for` loop, only between passes, and a single
pass over a swapped-out ~2635 MiB chunk at 4 KiB stride can take many
seconds once `request_stop()` fires mid-pass. **`verify.lua` should assert
exit 0, the `workers=2` value, and the `bytes=` target for `mem` as before,
but should use a generous upper bound on wall-clock time (e.g. requested
seconds × 3, or a fixed ceiling) rather than assuming near-exact duration
tracking** — unlike cpu/io/net, which now track `--seconds` tightly (within
~0.2s) and can reasonably use a tighter bound. Per the plan's own risk
mitigation, `mem` was again run last in S1, after everything else, and the
guest was confirmed stable both immediately after and on a delayed
follow-up check.

**Superseded pre-fix testing (kept for context — these numbers predate the
fix and must not be used for `verify.lua` bounds):** exit 0 both times
tested, guest remained healthy (no OOM, swap barely touched: 10→17 MiB),
`workers=2` and `bytes=5525480448` matched the formula exactly. The
duration was not honored at all pre-fix (2.3–2.8s regardless of `--seconds
2` vs. `8`) for the different reason described in finding 1 above (the
missing wait resolved during/immediately after the initial buffer
allocation, before any real touch loop ran).

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

### 4. Sampler concurrency (example 37 `analyze`) — confirmed correct, re-confirmed post-fix

`analyze --seconds 10` completed in **10.417s** wall-clock in the `f2b2dba`
re-capture (was 10.370s pre-fix — both consistent with the five interval
samplers, `vmstat`/`mpstat`/`iostat`/`sar`/`pidstat`, each run as `<tool>
... 1 10`, running concurrently rather than serially; serial would be
~50s). `analyze` itself was never touched by the `sat_*` bug or its fix —
this timing is unaffected and re-confirmed rather than changed, since the
re-capture re-ran capture 9 fresh against the same guest for completeness.
No divergence here in either capture.

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
