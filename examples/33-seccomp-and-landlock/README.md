# 33 — seccomp + Landlock (`fwatch` v3)

`fwatch` gets sandboxed. `watch --sandbox DIR` applies two independent
kernel access-control layers before the epoll/inotify watch loop from
[chapter 09](../09-event-loops-epoll-inotify) ever runs:

| Layer | What it restricts | Mechanism |
|---|---|---|
| **Landlock** | filesystem **reads**, to `DIR` only | an unprivileged, self-imposed ruleset the kernel enforces against this process and every descendant — it cannot be lifted short of exec-ing something that never asked for it |
| **seccomp** | which **syscalls** may run at all | a syscall allowlist covering only what the watch loop needs; anything else returns `EPERM` |

Two `probe` subcommands are negative controls that apply ONE layer in
isolation and prove it denies exactly what it claims to:

```
fwatch: landlock ABI=<n> enforced
fwatch: seccomp filter installed (<k> syscalls allowed)
```

| Language | Landlock | seccomp |
|---|---|---|
| **C++23** | raw `syscall(2)` (`<linux/landlock.h>`, no libc wrapper exists) | `libseccomp` |
| **Go 1.26** | raw `syscall(2)` via `golang.org/x/sys/unix`'s `SYS_LANDLOCK_*` numbers | a hand-assembled classic-BPF program installed via the raw `seccomp(2)` syscall |
| **Rust (edition 2024)** | the `landlock` crate | the `seccompiler` crate (Firecracker's BPF compiler) |

**Go runtime caveat.** Both mechanisms are per-*thread* kernel state by
default. The Go runtime schedules goroutines across OS threads it creates and
destroys on its own schedule (GC workers, sysmon, threads parked in blocking
syscalls); restricting only the calling thread would leave the others free,
and a goroutine later migrated onto one of them would silently run outside
the sandbox. The Go implementation avoids this with `runtime.LockOSThread()`
(pins the sandbox-installing goroutine to a stable OS thread) plus the
`LANDLOCK_RESTRICT_SELF_TSYNC` and `SECCOMP_FILTER_FLAG_TSYNC` flags, which
apply the new state to **every** thread already running in the process, not
just the caller's — confirmed locally: a goroutine deliberately scheduled
after the sandbox is installed still gets EACCES/EPERM.

The Landlock ABI has no libc/x/sys wrapper in any of the three languages;
every version probes it at runtime with
`landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` rather
than assuming a version — the printed `ABI=<n>` is a live kernel query, not a
constant.

## CLI

```
usage: fwatch <command>
  snapshot DIR                              one line per regular file
  diff OLD NEW                              compare two snapshots
  watch DIR [--timeout-ms T]                unsandboxed watch (chapter 09 behavior, unchanged)
  watch --sandbox DIR [--timeout-ms T]      Landlock+seccomp sandboxed watch
  probe --sandbox DIR --outside PATH        negative control: open PATH (outside DIR) under Landlock
  probe --forbidden-syscall                 negative control: socket(2) under a seccomp allowlist that omits it
```

- `snapshot` / `diff` / unsandboxed `watch` are unchanged from chapter 09.
- `watch --sandbox DIR` — applies Landlock (read+watch access restricted to
  `DIR`) then the seccomp allowlist, prints both status lines to stderr, then
  runs the identical watch loop. Events, debounce, and the
  `(timeout)`/`(signal)` exit lines are exactly as in chapter 09 — the
  sandbox is invisible to the loop's own logic.
- `probe --sandbox DIR --outside PATH` — restricts Landlock to `DIR`, then
  attempts to open `PATH` (expected to be outside `DIR`) for reading.
- `probe --forbidden-syscall` — installs the same seccomp allowlist (which
  never includes `socket(2)`), then calls `socket(AF_INET, SOCK_STREAM, 0)`.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | success (snapshot/diff/watch, including a sandboxed watch's clean timeout/signal exit) |
| `1` | runtime error, or a probe hit something other than the expected denial |
| `2` | usage error |
| `20` | **probe confirmed denial** — the sandbox blocked what it should (the passing case for a negative control) |
| `21` | **probe was NOT denied** — the sandbox failed to block what it should (a real sandbox bug) |

## Try it

```sh
./demo.sh cpp build
mkdir -p /tmp/fw-sandboxed
./cpp/build/release/app watch --sandbox /tmp/fw-sandboxed --timeout-ms 5000 &
sleep 0.3; echo hi > /tmp/fw-sandboxed/x.txt; rm /tmp/fw-sandboxed/x.txt
wait   # landlock/seccomp status lines, then the same event stream as ch09

# negative controls
./cpp/build/release/app probe --forbidden-syscall; echo "exit=$?"      # EPERM, exit 20
./cpp/build/release/app probe --sandbox /tmp/fw-sandboxed --outside /etc/hostname; echo "exit=$?"  # EACCES, exit 20
```

Landlock and seccomp are both **unprivileged** self-sandboxing mechanisms —
no root is needed to run any of the above on a Linux 5.13+ kernel with
Landlock enabled.

## Verification

This is a **vm**-mode example (`examples/manifest.yaml`): `verify.lua`
deploys the built binary and `run-sandbox-checks.sh` to `systems-target` and
runs the driver as root (the book's convention for every
namespace/cgroup/seccomp/Landlock demo, even though these two mechanisms
don't themselves require it). The driver exercises, in one ssh round trip:

1. `probe --forbidden-syscall` — asserts `EPERM` and exit `20`.
2. `probe --sandbox ... --outside ...` — asserts `EACCES` and exit `20`.
3. `watch --sandbox ...` with a file created/modified/deleted inside the
   tree while it runs — asserts an `event: ... inside.txt` line, the
   `(timeout)` exit line, and exit `0` (the positive control: sandboxing
   doesn't break the watcher).

`SKIP`s cleanly (exit 77) if `TARGET` isn't set or the lab VM is unreachable.

## Layout

```
33-seccomp-and-landlock/
├── demo.sh                   # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── run-sandbox-checks.sh     # staged + run as root by verify.lua on the guest
├── verify.lua                # automated check driven by the runner
├── cpp/                      # CMake preset build (links libseccomp), demo.sh
├── go/                       # go build, demo.sh
└── rust/                     # cargo build (landlock + seccompiler crates), demo.sh
```
