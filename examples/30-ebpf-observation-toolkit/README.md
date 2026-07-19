# 30 — eBPF observation toolkit

**VM example.** This chapter is not about writing eBPF — it's about pointing
the standard eBPF *observation* tools (bcc-tools, bpftrace) at an ordinary
userspace program and reading what they say. `app work --seconds N` runs a
small, labeled workload; a driver script (`observe.sh`, running as root on the
lab VM) starts it in the background and then runs five separate tools against
it in turn, each proving it really saw an event our program produced:

| Tool | What it watches | Bait in `app work` |
|---|---|---|
| `opensnoop` | `open(2)` calls | a fixed file, opened/written/closed every iteration |
| `execsnoop` | `exec(2)` calls | a short-lived `true` child, forked/exec'd every 4th iteration |
| `funccount` | per-symbol call counts via uprobe | the named hot function `busy_hash()`, called every iteration |
| `offcputime` | off-CPU time by stack | a ~230ms sleep between iterations |
| `bpftrace` | a one-line uprobe program | the same `busy_hash()`, counted with `@calls` |

`busy_hash()` is deliberately kept unmangled and unstripped, and marked
noinline, in all three builds — cpp and rust emit the plain symbol
`busy_hash`, Go emits `main.busy_hash` — so `observe.sh` can target every
language with the *same* pattern: `<binary>:*busy_hash*`.

All three languages produce byte-for-byte identical output, including the
final accumulated hash value (same seed, same algorithm, same iteration
count), so one `verify.lua` covers them.

## The workload: `app work --seconds N`

```
work: start seconds=60 pid=13414 bait=/tmp/lsp-ebpf-work-bait.txt
work: exec i=0 status=0
work: exec i=4 status=0
...
work: done seconds=60 iters=260 opens=260 execs=65 busy_calls=260 busy_hash=12869313528293982360
```

Each iteration (~230ms): open/write/close the bait file; every 4th iteration,
fork+exec `true` and wait for it; call `busy_hash()` once; sleep ~230ms. None
of this needs root — only the *observation* side does.

| Language | How the loop runs |
|---|---|
| **C++23** | The loop body runs inside a `std::jthread` (takes a `std::stop_token`, ready for cooperative cancellation); `main()` joins it explicitly so the summary line always sees final counters. An RAII `Fd` class owns the bait file's descriptor. Every fallible syscall returns `std::expected<T, std::error_code>`. Output goes through `std::println`. |
| **Go 1.26** | The loop runs in a goroutine; `main` blocks on a buffered result channel — the goroutine's "join". Every fallible call is wrapped with `fmt.Errorf("...: %w", err)`. |
| **Rust (edition 2024)** | Straight-line `Result`/`?` through the whole loop; the bait file's descriptor is a `rustix::fs` `OwnedFd`, closed by `Drop` — no explicit `close(2)`. |

### A real optimizer gotcha found while building this

The first version of this workload printed `busy_calls` but never the
accumulated hash value itself. At `-O2`, GCC's interprocedural analysis proved
`busy_hash()` had no observable side effects and that its final result was
never used anywhere — and deleted the *entire* call chain, `noinline` or not
(`noinline` only forbids inlining; it says nothing about dead-code
elimination of a provably-unused, provably-pure call). `funccount` and the
`bpftrace` uprobe both reported zero calls even though the binary clearly
still exported the `busy_hash` symbol and a plain `nm`/`objdump` looked
completely normal. The fix: print the final hash in the `done` summary line,
in all three languages. The moment the result is observable, the whole chain
is provably necessary again, and the uprobe fires. `busy_hash` is part of the
printed summary for that reason — it isn't an incidental extra field.

## `observe.sh` — the actual demo

`observe.sh <path-to-app-binary> [seconds]` runs **on the guest, as root**
(bcc-tools and bpftrace need `CAP_BPF`/`CAP_PERFMON`, which on this lab image
means root). It:

1. starts `<binary> work --seconds N` in the background,
2. runs `opensnoop -p PID`, `execsnoop -P PID`, `funccount -p PID -d N
   '<binary>:*busy_hash*'`, `offcputime -p PID N`, and a `bpftrace` uprobe
   one-liner against it in turn, each wrapped in its own `timeout`,
3. prints a `=== summary ===` block deriving a hit count per tool,
4. lets the workload finish (or stops it) before exiting.

It deliberately does **not** use `set -e` — a bcc tool killed by its own
`timeout` wrapper (several of these tools don't reliably self-terminate on
their internal `-d`/duration flag; see below) is expected, not a driver
failure. Each step's output is checked on its own, at the end.

### A real bcc-tools quirk this driver works around

`opensnoop`'s own `-d SECONDS` flag did not reliably make it exit on its own
in testing — it consistently ran until an external `timeout` killed it,
regardless of the requested duration. Every invocation in `observe.sh` is
therefore wrapped in an outer `timeout`, and a `124` exit from that wrapper is
treated as expected, not a failure — only the captured *content* is asserted
on, never the tool's own exit code.

## CLI

```
usage: app work --seconds N
```

Missing/invalid `--seconds`, or any subcommand other than `work`, prints the
usage line to stderr and exits `2`.

## Demo contract

Each language directory has the standard `demo.sh`:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (`app`) locally
- With env `TARGET` set, `run` deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh` and runs there instead (no root needed for
  the workload itself)

The top-level `./demo.sh [cpp|go|rust|all|build]` dispatches per language.
`observe.sh` is deliberately separate from this dispatcher — it's a custom
root-run driver script (starts the workload *and* orchestrates five different
tools against it), not a single binary+args invocation, so it doesn't fit the
`deploy-to-vm.sh` shape and is staged/run directly instead.

## Try it (on the lab VM)

```sh
export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target
cd examples/30-ebpf-observation-toolkit

# build and stage one language's binary, e.g. cpp
./cpp/demo.sh build
IP=$(../../scripts/lab/vm-ip.sh systems-target)
ssh "fedora@$IP" 'mkdir -p /tmp/lsp30-cpp'
scp cpp/build/release/app "fedora@$IP:/tmp/lsp30-cpp/app"
scp observe.sh "fedora@$IP:/tmp/lsp30-observe.sh"
ssh "fedora@$IP" 'chmod 755 /tmp/lsp30-cpp/app /tmp/lsp30-observe.sh'

# run the observation, as root
ssh "fedora@$IP" 'sudo bash /tmp/lsp30-observe.sh /tmp/lsp30-cpp/app 60'
```

A real run's tail looks like this:

```
=== funccount busy_hash (uprobe/funccount bait) ===
Tracing 1 functions for "b'/tmp/lsp30-cpp/app:*busy_hash*'"... Hit Ctrl-C to end.

FUNC                                    COUNT
busy_hash                                  21
Detaching...

=== offcputime (sleep off-CPU bait) ===
(33 lines captured; frames naming our binary/function:)
    -                app (13416)
    __clock_nanosleep_2
    do_nanosleep
    hrtimer_nanosleep
    __x64_sys_clock_nanosleep

=== bpftrace uprobe:busy_hash ===
Attached 2 probes
@calls: 22

=== summary ===
opensnoop_opens=20
execsnoop_execs=6
funccount_calls=21
offcputime_hits=2
bpftrace_calls=22
app_summary: work: done seconds=60 iters=260 opens=260 execs=65 busy_calls=260 busy_hash=12869313528293982360
```

## Verification

`verify.lua` (run per language under the runner's **vm mode**,
`TARGET=systems-target`) stages the built binary under `/tmp/lsp30-<lang>/app`
— so its `/proc` comm is exactly `app` regardless of which language built
it — plus `observe.sh`, then runs the driver as root and asserts, from its
captured output, that each tool really observed our program:

1. **opensnoop** shows comm `app` opening the bait file's exact path.
2. **execsnoop** shows a `true` child whose **PPID equals the workload's own
   pid** (parsed from `observe.sh`'s own `workload pid=` line) — not just
   that *some* exec happened somewhere on the box.
3. **funccount** reports a **nonzero** call count for `busy_hash`.
4. **offcputime** shows a stack naming comm `app` (the ~230ms sleep between
   iterations put it off-CPU during the trace window).
5. **bpftrace**'s uprobe one-liner reports `@calls` **nonzero**.

Each is a positive count/match on a *specific* signal our program produced,
not merely "the tool ran and exited 0" — a tool that loads and exits cleanly
while counting nothing would fail every one of these.

Run it:

```sh
LIBVIRT_DEFAULT_URI=qemu:///system \
  python3 scripts/test-all-examples.py --only 30-ebpf-observation-toolkit --mode vm
```

**Why this is real observation, not an inference:** every assertion is tied
to something our own program did — the exact bait path, the workload's own
pid as the exec's parent, a call count on the exact function we wrote. This
is the standard eBPF-based observability toolkit doing what it's designed to
do: watch an ordinary, unmodified userspace binary from the outside. We write
no kernel-side eBPF program here — bcc-tools and bpftrace already ship
theirs; this chapter is about aiming them correctly and reading the answer.
