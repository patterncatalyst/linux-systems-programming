# 34 — our programs in containers

`app`, a tiny PID-1 container entrypoint, implemented three times — C++23,
Go, and Rust — each built into its own multi-stage **Containerfile** (UBI10
builder → UBI10-micro runtime) and run under **rootless podman**. It
demonstrates three things a program running as a container's entrypoint has
to get right that a normal program never has to think about:

```
usage: app serve|naive     # C++, Go, Rust
usage: app serve|naive|worker|job   # Go, Rust (self-reexec children; see below)
```

1. **pid-1 signal duties** — `serve` reaps every child (so short-lived jobs
   never pile up as zombies) and forwards `SIGTERM` to the worker it
   supervises before exiting. `naive` installs no signal handling at all, the
   mistake this example exists to make visible.
2. **container-aware resource detection** — `serve`/`naive` read their own
   cgroup v2 `cpu.max`/`memory.max` and print
   `container: cpu.max=<q> effective_parallelism=<n> mem.max=<b>`. Go's
   `GOMAXPROCS` and Rust's `available_parallelism()` are cgroup-aware; C++'s
   `hardware_concurrency()` is not — it reports the **host's** cpu count
   however small `--cpus` is set. That mismatch is the trap.
3. **debugging across namespaces** — `podman exec` and a rootless
   `podman unshare nsenter` from the host both reach the same PID 1 inside
   the running container.

## Layout

```
34-our-programs-in-containers/
├── demo.sh              # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua           # podman build/run/stop, behavioral checks
├── cpp/                 # CMake preset build, demo.sh, Containerfile
├── go/                  # go build, demo.sh, Containerfile
└── rust/                # cargo build, demo.sh, Containerfile
```

Each language directory's `Containerfile` builds a small (a few MB to a few
tens of MB) runtime image:

| Stage | Base | Does |
|---|---|---|
| builder | `registry.access.redhat.com/ubi10/ubi:10.2` | installs the toolchain, compiles `app` |
| runtime | `registry.access.redhat.com/ubi10-micro:10.2` | copies just the binary, sets `ENTRYPOINT`/`CMD` |

C++'s builder links `libstdc++`/`libgcc` statically (`libstdc++-static`), so
the runtime needs nothing beyond glibc. Go builds with `CGO_ENABLED=0` for a
fully static binary. Rust's builder installs the exact `rustc 1.97.1` pinned
by `rust-toolchain.toml` via `rustup` (UBI10's `dnf` package is 1.92.0); the
resulting binary needs only `libc`/`libgcc_s`, both already in
`ubi10-micro`.

## Output contract (identical shape across all three languages)

| Stream | Line | When |
|---|---|---|
| stdout | `container: cpu.max=<q> effective_parallelism=<n> mem.max=<b>` | first line, `serve` and `naive` |
| stdout | `app: pid=<pid> ppid=<ppid>` | second line, `serve` and `naive` |
| stdout | `app: worker started pid=<pid>` | `serve`, once |
| stdout | `app: worker pid=<pid> tick=<n>` | `serve`, every 2s from the worker |
| stdout | `app: job pid=<pid> seq=<n> done` | `serve`, every ~1s, from a short-lived job |
| stdout | `app: reaped pid=<pid> status=<n>` | `serve`, immediately after each job/worker exit |
| stdout | `app: naive heartbeat tick=<n>` | `naive`, every 1s, forever |
| stdout | `app: shutting down (SIGTERM\|SIGINT)` | `serve` only, right before it exits 0 |

`naive` never prints a shutdown line — that absence is itself the evidence
the example verifies.

`cpu.max` is printed as `max` (unconstrained) or `<quota>/<period>` (derived
from the cgroup file's raw `"<quota> <period>"` content); `mem.max` is
printed exactly as the cgroup reports it (`max` or a byte count).

## The trap, reproduced for real

Run with `podman run --cpus=2 --memory=128m`, all three correctly report the
container's `cpu.max=200000/100000` and `mem.max=134217728` (not the host's
`max`/`max`) — but `effective_parallelism` tells a different story per
language, captured from a real run on a 16-cpu host:

```
cpp:  container: cpu.max=200000/100000 effective_parallelism=16 mem.max=134217728
go:   container: cpu.max=200000/100000 effective_parallelism=2  mem.max=134217728
rust: container: cpu.max=200000/100000 effective_parallelism=2  mem.max=134217728
```

C++'s `std::thread::hardware_concurrency()` is `sysconf(_SC_NPROCESSORS_ONLN)`
under the hood: it has no idea a cgroup exists. Go's `GOMAXPROCS` (default
container-aware as of the `go 1.26` language version) and Rust's
`available_parallelism()` both divide the cgroup's cpu quota by its period
and report `2` — the number that is actually true.

## Per-language mechanics

- **C++23** (`cpp/src/main.cpp`) — raw `fork(2)` for both the worker and each
  job (no exec: they are just different functions in the same binary). This
  is the one place a subtle, very real bug lived during development: the
  forked worker **inherits the parent's blocked signal mask**
  (`sigprocmask(SIG_BLOCK, ...)` for `SIGTERM`/`SIGINT`/`SIGCHLD`, set up so
  `serve` can receive them via `signalfd(2)`). Left alone, a forwarded
  `SIGTERM` to the worker would just sit *pending* forever instead of
  running its default (terminate) action — the worker would be unkillable.
  The fix is one `sigprocmask(SIG_SETMASK, &empty, ...)` in the child right
  after `fork()`, the same principle as pmon's (ch. 12) reset-before-`execvp`,
  just with no `exec` to do it for you. `reap_all()` drains
  `waitpid(-1, WNOHANG)` from the ordinary `poll(2)` loop (never from signal
  context) whenever the `signalfd` reports `SIGCHLD`. A `std::jthread` (with
  a `stop_token`, RAII-joined) drives the periodic job spawns.
- **Go 1.26** (`go/main.go`) — no `fork(2)`-without-exec (Go's runtime is not
  fork-safe once other goroutines/threads exist), so the worker and each job
  are separate processes started by **re-executing the same binary**
  (`os.Executable()` + `exec.Command(self, "worker"|"job", ...)`). No
  `SIGCHLD`/`signalfd` anywhere: `Cmd.Wait()` calls `wait4(2)` for you, so one
  goroutine parked in `Wait()` per spawned child *is* the reap loop. A real,
  verified surprise from building this: a Go binary that never calls
  `signal.Notify` still **dies immediately** on `SIGTERM`, even as PID 1 —
  Go's runtime installs its own handler for fatal signals like `SIGTERM` at
  startup regardless of user code (see `runtime`'s internal signal table),
  so it never has an unclaimed `SIG_DFL` disposition for the kernel's PID-1
  special case to mask out. `naive` in Go is therefore not "unresponsive" the
  way C++'s/Rust's are — it is abrupt instead: no reaping, no forwarding, no
  shutdown message, just an instant, uncontrolled death.
- **Rust edition 2024** (`rust/src/main.rs`) — same self-reexec approach as
  Go, via `std::env::current_exe()` + `std::process::Command`, for the same
  reason (no fork-without-exec once threads exist). `signal_hook::iterator::
  Signals::new([SIGTERM, SIGINT])` turns both into a blocking iterator on the
  main thread — no polling, no manual mask bookkeeping, and (unlike the
  `sigprocmask`-based approach) nothing to reset before `exec`, since
  `signal_hook` only installs handlers and never blocks the signals it
  watches. Reaping is `std::thread::spawn` + `Child::wait()` per job, exactly
  like Go's goroutine-per-child.

## Try it

```bash
# Build and run locally (unconstrained: cpu.max=max mem.max=max)
./demo.sh cpp run serve
./demo.sh go run naive

# Build and run the actual container image, constrained
podman build -t lsp34-cpp -f cpp/Containerfile cpp
podman run --rm --cpus=2 --memory=128m lsp34-cpp serve

# Compare the trap side by side (2 vCPUs, three languages)
for lang in cpp go rust; do
  podman build -t lsp34-$lang -f $lang/Containerfile $lang >/dev/null
  podman run --rm --cpus=2 lsp34-$lang serve --help >/dev/null 2>&1 || true
  timeout 2 podman run --rm --cpus=2 lsp34-$lang serve | head -1
done

# Debugging across namespaces: podman exec, then a rootless nsenter from the host
podman run -d --name demo34 --cpus=2 --memory=128m lsp34-go serve
podman exec demo34 cat /proc/1/status | head -3
HOSTPID=$(podman inspect --format '{{.State.Pid}}' demo34)
podman unshare nsenter --target "$HOSTPID" --pid --mount -- readlink /proc/1/exe
podman stop -t 3 demo34   # near-instant: serve forwards SIGTERM and exits cleanly
podman rm -f demo34; podman rmi -f lsp34-go
```

`nsenter` directly (without `podman unshare`) fails rootless with
`Operation not permitted` — the container's namespaces live inside podman's
own unprivileged user namespace, and `podman unshare` is what puts you inside
it first.

## Verification (behavioral)

`verify.lua` requires podman on the host (`skip()`, exit 77, if it is
missing) and, for every language, actually builds the image and drives real
containers rather than trusting exit codes:

- **Resource detection under real constraints** — `podman run --cpus=2
  --memory=128m`, then the container's own logs must show the *constrained*
  `cpu.max=200000/100000` and `mem.max=134217728` — never the host's `max`.
  C++'s `effective_parallelism` must equal the **host's** live `nproc` (the
  trap, reproduced); Go's and Rust's must equal `2` (the cgroup quota). The
  host must report `nproc >= 3` or the test skips, since otherwise C++'s
  "wrong" number would coincidentally equal the "right" one.
- **PID-1 identity and reaping** — `app: pid=1 ppid=0` in the logs; several
  `app: reaped pid=... status=0` lines with no pile-up; a `podman exec`
  zombie scan of `/proc/[0-9]*/stat` reports `zombies=0` while jobs are being
  spawned and reaped.
- **Cross-namespace debugging** — `podman exec ... cat /proc/1/status` names
  the process `app`; a rootless `podman unshare nsenter --target <hostpid>
  --pid --mount -- readlink /proc/1/exe` from the host resolves to the same
  `/usr/local/bin/app`.
- **`serve` vs `naive`, timed** — `podman stop -t 3` on `serve` completes in
  well under a second (real runs: ~90-220ms) with a `shutting down (SIGTERM)`
  line in the logs. `podman stop -t 2` on `naive`: for C++/Rust this takes
  the **full grace period** (~2.1s) and podman's own log shows it fell back
  to `SIGKILL`; for Go it stops fast anyway (~90ms) for the runtime reason
  above, but in every language the shutdown line **never appears** in
  `naive`'s logs, proving no graceful path ran regardless of how the process
  actually died.
- Every container and the built image are removed at the end, whether the
  checks passed or not.

All three languages pass all 16 checks on the reference host (Fedora 44,
kernel 7.1.3-200.fc44, podman 5.8.4 rootless).
