# 37-use-method-60s-analysis: loadmix, Brendan Gregg's USE method as a runnable program

`loadmix` is one binary with two jobs: create a resource problem on demand,
then diagnose one. `analyze` mode runs the checklist an on-call engineer
would run by hand, `uptime`, `dmesg`, `top`, `ss`, `vmstat`, `mpstat`,
`iostat`, `sar`, `pidstat`, and `free`, reads each tool's plain-text table by
column name, and prints which resource is Utilized, Saturated, or Erroring.
`--resource cpu|mem|io|net` mode pegs one resource so there is something real
for `analyze` to find.

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

`dmesg status=denied` is the expected shape for an unprivileged account:
`analyze` treats a permission-denied tool as a reportable condition, not a
crash.

## Two modes, one binary

```
app --resource cpu|mem|io|net --seconds N   saturate that resource
app analyze [--seconds N]                    run the checklist and name
                                              the saturated resource
```

- `--resource cpu` spins a worker pool doing xorshift arithmetic only, no
  syscalls, no allocation in the hot loop.
- `--resource mem` touches 1.35x the guest's `MemTotal` across a worker pool,
  overshooting available RAM so pages land in swap, without OOM-killing the
  guest.
- `--resource io` opens one file per worker with `O_DSYNC` and `pwrite`s a
  16 KiB buffer of xorshift bytes to the same offset in a loop, forcing a
  synchronous write on every call.
- `--resource net` floods UDP datagrams over loopback from a pool of senders
  to one receiver.

Every saturator prints a `loadmix: start resource=... seconds=... workers=N`
line, runs for approximately the requested duration, and prints a
`loadmix: done resource=... ops=... bytes=...` line. `analyze` induces no
load of its own; it spends its `--seconds` window watching the system
through the nine tools listed above.

An unrecognized `--resource`, a missing `--seconds`, `--seconds 0` or
negative, or an unrecognized `analyze` flag all print the two-line usage
banner above to stderr and exit `2`. A clean run of either mode exits `0`.
There is no separate runtime-error exit code: `analyze`'s subprocess calls
degrade instead of failing the whole run, a denied `dmesg`, an empty `ss -s`,
or a sampler with no data rows all show up as a `0.00`/`denied`/empty field
rather than a non-zero exit.

## The USE threshold table

```
cpu   Utilization: busy_pct  >= 60%        Saturation: run_queue >= cpus+2
mem   Utilization: used_pct  >= 75%        Saturation: swap_io   >= 300 KB/s
io    Utilization: util_pct  >= 40%        Saturation: iowait_pct >= 8%
net   Utilization: pkts/s    >= 2000       Saturation: pkts/s     >= 8000
```

cpu's Saturation threshold scales with the guest it runs on, `cpus + 2`,
rather than a flat constant: a run queue longer than the core count plus a
little headroom means threads are waiting for a CPU that isn't there. Errors
is printed for every resource but always `fired=false`, since these
saturators induce load, not kernel-logged faults; the line keeps the
Utilization/Saturation/Errors triple complete. The final verdict picks the
resource with the highest Saturation-value-to-threshold *ratio*, not the
highest raw Saturation value, so net's 8000-scale threshold never automatically
loses to cpu's 4-scale one.

## Parsing by column name, not position

Every parser reads a tool's header row first and looks fields up by name
against it, never by a hardcoded index. `Row` (C++), `row` (Go), and `Row`
(Rust) all express the same idea: column names from a tool's own header line,
zipped with that row's values, so a `sysstat` or `procps-ng` version bump
that reorders columns doesn't break the parser.

`parse_mpstat` is the clearest case: `mpstat -P ALL`'s `Average:` rows begin
with a label token (`Average: CPU` / `Average: all`) that has to be skipped
before `%idle` lines up with the real idle percentage, and `busy_pct` is
computed as `100.0 - %idle` rather than summing `%usr + %sys + %nice + ...`,
since idle's complement is "everything else" regardless of what utilization
sub-categories a future sysstat release adds. `vmstat` has no `Average:` row
at all and its first sample is a since-boot average, so its parser discards
row 0 and takes the max run-queue value across the rest. `iostat -xz` has the
same since-boot-first-row quirk, plus `-z` hides idle devices, so its parser
tracks block boundaries by index rather than by content.

## Concurrency, two ways

`analyze`'s five interval samplers (`vmstat`, `mpstat -P ALL`, `iostat -xz`,
`sar -n DEV`, `pidstat`, each run as `<tool> 1 N`) are real subprocesses
spawned concurrently and joined once, not run one after another; that's the
whole reason a `--seconds 10` checklist finishes in about 10 seconds instead
of the roughly 50 seconds five serial invocations would cost.

Each saturator's worker pool is a second, unrelated concurrency story:
compute a deadline, spawn workers that loop until stopped or the deadline
passes, then join. C++'s pool is `std::jthread`, whose RAII destructor runs
`request_stop()` before `join()`, so without `std::this_thread::sleep_until(deadline)`
holding the parent scope open, every worker would be told to stop the
instant the spawn loop ends, in single-digit milliseconds, regardless of
`--seconds`. Go and Rust don't need that pattern: each worker checks its own
deadline directly inside its loop condition, so the parent's `wg.Wait()` or
`thread::scope` return is a rendezvous, not part of the termination signal.

## Layout

```
37-use-method-60s-analysis/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check, driven against the systems-target VM
├── cpp/         # CMake preset build (src/main.cpp), demo.sh
├── go/          # go build (main.go), demo.sh
└── rust/        # cargo build (src/main.rs), demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the same interface, identical
across the whole book:

- `./demo.sh`: build then run
- `./demo.sh build`: build only
- `./demo.sh run [args]`: run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

The top-level `demo.sh` here is only a dispatcher: `./demo.sh cpp run` execs
`cpp/demo.sh run`; bare `./demo.sh` (or `all`) builds and runs all three.

This is a VM example: the saturation signals only mean something measured
against a real, otherwise-quiet kernel. `verify.lua` does not drive it
through `demo.sh <lang> run TARGET=...`, since `deploy-to-vm.sh` wraps
commands in `ssh -t`, which injects CRLF that breaks the tool's plain-text
output. Instead it stages the built binary directly to
`/tmp/lsp37-<lang>/app` on `systems-target` with plain `scp`/`ssh` and drives
it there.

## Verification

`verify.lua` runs with `LSP_LANG` set to one language at a time and `TARGET`
pointing at the lab VM. It builds, stages the binary, then asserts:

- all four usage-error shapes (no args, a bad `--resource`, `--seconds 0`, a
  bad `analyze` flag) exit `2` with the two-line usage banner on stderr
- `--resource cpu|io|net --seconds 4` each exit `0`, print the documented
  worker-count formula for the guest's real `nproc`, report `ops > 0`, and
  finish within a tight window around the requested `--seconds` (this is
  exactly what would have caught a saturator that exited in milliseconds
  regardless of the requested duration)
- `--resource mem` exits `0` without OOM-ing the guest, matches its own
  worker-count formula, and stays under a generous ceiling (`ops > 0` is not
  required for mem: a short duration can spend the whole budget on the
  initial allocation before any touch pass completes)
- an idle `analyze --seconds 8` prints the exact line skeleton, 1 start line,
  5 tool lines, 8 metric lines, 12 signal lines in cpu/mem/io/net x
  Utilization/Saturation/Errors order, 1 verdict line, `analyze: done` as the
  last line of the app's own output, and finishes well under `5 x --seconds`,
  proving the five samplers ran concurrently rather than serially
- under a concurrent background `--resource cpu` saturator, a second
  `analyze` run attributes the load: `busy_pct >= 60` or
  `run_queue >= nproc + 2`, Utilization `fired=true`, and `verdict=cpu`,
  retried up to 3 attempts against a margin calibrated from real captures

`python3 scripts/test-all-examples.py --only 37-use-method-60s-analysis
--mode vm` builds all three and runs `verify.lua` per language against
`systems-target`.

## Mapping to a chapter

This example backs `_docs/37-use-method-60s-analysis.md`, "The USE method in
60 seconds." The chapter walks through the column-name parsing discipline,
the USE threshold table, and the `std::jthread` RAII ordering issue in the
saturator pool in more depth than this README does.
