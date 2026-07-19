# 13 — pidfd: modern process management

`pmon` v2, the chapter's process supervisor, implemented three times — C++23,
Go, and Rust — with identical observable behavior (CLI, output lines, exit
codes). This iteration adds a second supervision engine built on **pidfd**,
the modern child becomes-a-file-descriptor API, and makes it the default:

```
usage: pmon <command>
  run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit
  supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]
            -- CMD [ARGS...]               restart CMD on abnormal exit
                                           (defaults: pidfd, N=3, T=10000)
```

## What each version added

- **v0 — `run`**: spawn a command, wait for it, report and mirror its exit
  (`status` → same exit code, fatal signal → `128+signo`).
- **v1 — `supervise --engine sigchld`**: restart the child on abnormal exit,
  driven the classic way: SIGCHLD arrives (via a signalfd in C++/Rust,
  `signal.Notify` in Go), then a `WNOHANG` wait reaps and classifies it.
- **v2 — `supervise --engine pidfd`** (default): no SIGCHLD anywhere.
  `pidfd_open(2)` turns the child pid into a file descriptor; `poll(2)` on it
  reports the exit as ordinary readability; `waitid(P_PIDFD, ...)` reaps
  exactly the process behind the fd (a recycled pid can never be reaped or
  signalled by mistake); the stop path delivers SIGTERM through the fd with
  `pidfd_send_signal(2)`.

## Output contract (identical in all three languages)

| Stream | Line | When |
|---|---|---|
| stderr | `pmon: engine=pidfd child=<pid> pidfd=<fd>` | each spawn, pidfd engine |
| stderr | `pmon: engine=sigchld child=<pid>` | each spawn, sigchld engine |
| stderr | `pmon: run child=<pid>` | `run` spawn |
| stdout | `pmon: child=<pid> exited status=<n>` | child exited normally |
| stdout | `pmon: child=<pid> killed signal=<n>` | child died by signal |
| stdout | `pmon: restart <k>/<N>` | before each respawn |
| stdout | `pmon: giving up after <N> restarts` | budget exhausted (exit 1) |
| stdout | `pmon: exiting (signal)` / `pmon: exiting (timeout)` | stop paths (exit 0) |

A clean child exit (`status=0`) ends supervision with exit 0 and no restart.
SIGINT/SIGTERM to `pmon` and the `--timeout-ms` window both TERM the child,
reap it, and leave with exit 0. Usage errors exit 2; a command that cannot be
spawned exits 1 with `pmon: spawn: <cmd>: ...`.

## Per-language mechanics

- **C++23** (`cpp/src/main.cpp`) — raw `syscall(SYS_pidfd_open)` and
  `syscall(SYS_pidfd_send_signal)` on purpose (the libc wrappers are recent),
  `poll(2)` over `{pidfd, signalfd}`, `waitid(P_PIDFD)` to reap. RAII `Fd` and
  `SpawnAttrs` owners, `std::expected` error paths, `std::println`,
  `posix_spawnp` with `POSIX_SPAWN_SETSIGMASK` so the child does not inherit
  the blocked signalfd mask.
- **Go 1.26** (`go/main.go`) — `unix.PidfdOpen` + a goroutine parked in
  `unix.Poll`, exit delivered over a channel into one `select` alongside
  `signal.Notify` and the deadline; a raw `waitid(P_PIDFD)` via
  `unix.Syscall6` with an explicit `siginfo_t` layout (x/sys keeps the union
  opaque); stop path via `unix.PidfdSendSignal`. Errors are wrapped with `%w`.
- **Rust edition 2024** (`rust/src/main.rs`) — `rustix::process::pidfd_open`
  returning an `OwnedFd`, `rustix::event::poll` over the pidfd + a nix
  `SignalFd`, `rustix::process::waitid(WaitId::PidFd)`, stop path via
  `rustix::process::pidfd_send_signal`. `Result` + `?` throughout.
  Note: `std::process::Command` does **not** reset the child's signal mask, so
  `pre_exec` clears it — otherwise a supervised `sleep` inherits a blocked
  SIGTERM and can never be stopped.

## Try it

```bash
./demo.sh cpp  run supervise --max-restarts 2 -- sh -c 'exit 1'
./demo.sh go   run supervise --engine sigchld -- sh -c 'exit 0'
./demo.sh rust run supervise --timeout-ms 500 -- sleep 30
./demo.sh cpp  run run -- sh -c 'kill -9 $$'   # mirrors as exit 137
```

## Verification (behavioral)

`verify.lua` drives every path and asserts effects, not exit-0:

- `run` mirrors exit 0/3 and SIGKILL (exit 137, `killed signal=9`).
- The **pidfd engine supervises a crashing child**: exactly 3 spawn lines
  each carrying a real `pidfd=<fd>` number, 3 observed crashes,
  `restart 1/2` then `restart 2/2` in order, the giving-up line, exit 1.
- The **sigchld engine still works**: same run shape, no `pidfd=` field
  anywhere in its output, and its restart lines are byte-identical to the
  pidfd engine's.
- Both stop paths terminate the supervised child (`killed signal=15`), print
  the `exiting (...)` line, exit 0 — and a `pgrep` process-tree check proves
  no supervised `sleep` outlives its supervisor.
- Usage errors exit 2; an unspawnable command exits 1 and names the command.
