# 18 — Pipes, FIFOs, splice (`pmon` v4)

`pmon` is the process supervisor that grows across the book: v0 `run`,
v1 the sigchld supervise engine, v2 the pidfd engine (chapter 13), v3 the
privilege-drop chapter. v4 keeps both supervise engines exactly as they were
and turns the supervisor into a log pipeline built from kernel pipe
machinery:

- **`supervise` captures the child** — the child's stdout and stderr each
  flow through their own `pipe(2)` back into the supervisor, which relays
  them line-buffered into a log file as `[out] ...` / `[err] ...` lines.
  The two pipe read ends simply join the fd set each engine already
  multiplexes; nothing about the v1/v2 restart/timeout/stop behavior
  changes.
- **`tail` republishes the log** — it creates a FIFO (`mkfifo(3)`), follows
  the log file, and relays appended bytes into whatever reader opens the
  FIFO. C++ and Rust move the bytes kernel-side with `splice(2)`
  (file → pipe, no userspace copy; automatic `read`/`write` fallback on
  filesystems without splice support). A reader that disappears is not
  fatal: SIGPIPE is ignored, the resulting `EPIPE` prints
  `pmon: tail reader detached`, and the relay waits for the next reader —
  losing nothing, because the log offset only advances for bytes that
  actually reached the pipe.

| Language | Architecture |
|---|---|
| **C++23** | one `poll(2)` loop per child over four fds — the two capture pipes, the signalfd, and (pidfd engine) the pidfd from raw `SYS_pidfd_open`; `waitid(P_PIDFD)` reaps race-free. RAII `Fd`/`SpawnAttrs`/`FileActions` owners, `std::expected` error paths, `posix_spawnp` wiring the pipes via `adddup2` and resetting the child's signal mask. `tail` uses the `splice(2)` fast path with an offset-preserving `read`/`write` fallback. |
| **Rust (edition 2024)** | the same poll set via `rustix::event::poll` + a nix `SignalFd`, `pidfd_open` returning an `OwnedFd`, `waitid(WaitId::PidFd)`; `rustix::pipe::splice` for the tail fast path; `anyhow` + `?` throughout, every fd `OwnedFd`-backed. |
| **Go 1.26** | the idiomatic inversion: one goroutine per capture pipe feeds a channel of prefixed lines into the same `select` that already multiplexes the pidfd poller (or `signal.Notify(SIGCHLD)`), the stop signals, and the deadline. `tail` relays with `io.Copy` per reader session, rewinding the log to `start+written` on `EPIPE`; `mkfifo`/nonblocking FIFO open via `golang.org/x/sys/unix`. |

All three expose identical observable behavior, so one `verify.lua` covers
them.

## CLI

```
usage: pmon <command>
  run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit
  supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]
            [--log FILE] -- CMD [ARGS...]  restart CMD on abnormal exit;
                                           capture stdout/stderr into FILE
                                           (defaults: pidfd, N=3, T=10000,
                                           FILE=pmon.log)
  tail --log FILE --fifo PATH              relay appended log lines into a
                                           FIFO created at PATH
```

The v2 output contract is unchanged: spawn lines
(`pmon: engine=pidfd child=<pid> pidfd=<fd>` / `pmon: engine=sigchld
child=<pid>` / `pmon: run child=<pid>`) on stderr; observations
(`pmon: child=<pid> exited status=<n>`, `pmon: child=<pid> killed
signal=<n>`, `pmon: restart <k>/<N>`, `pmon: giving up after <N> restarts`,
`pmon: exiting (signal|timeout)`) on stdout. `run` mirrors the child's exit
(fatal signal → `128+signo`); clean `status=0` ends supervision with exit 0;
an exhausted restart budget exits 1; both stop paths exit 0.

New in v4:

- `supervise` appends each complete child line to the log immediately as
  `[out] line` / `[err] line`; a trailing partial line is flushed with a
  newline when the stream closes, and the log is fully drained before the
  exit observation is printed.
- `tail --log FILE --fifo PATH` — creates PATH as a FIFO (an existing FIFO
  is reused; anything else is an error), prints
  `pmon: tail ready (fifo PATH)` to stderr, then relays FILE from the
  beginning into each successive FIFO reader, polling for appended bytes.
  Reader detach (`EPIPE`) prints `pmon: tail reader detached` (stdout) and
  the relay keeps running; undelivered bytes are re-sent to the next
  reader. SIGINT/SIGTERM → `pmon: exiting (signal)`, exit 0.
- Exit codes everywhere: `0` success, `1` runtime error, `2` usage error.

## Demo contract

Each language directory has the standard `demo.sh`:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (`app`); the local path
  `exec`s the binary so a backgrounded `run tail ...` can be signalled
  directly
- `./demo.sh` — build, then a one-shot self-demo (`run -- sh -c ...`)
- With env `TARGET` set, `run` deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh`

The top-level `./demo.sh [cpp|go|rust|all|build]` dispatches per language.

## Try it

```sh
./demo.sh cpp build
cd "$(mktemp -d)"
APP=/path/to/examples/18-pipes-fifos-splice/cpp/build/release/app

# two-pipe capture with a restart (pidfd engine, unchanged from v2)
$APP supervise --max-restarts 2 --log pmon.log -- sh -c 'echo hello; echo oops >&2; exit 1'
cat pmon.log        # [out] hello / [err] oops, repeated per attempt

# republish the log through a FIFO
$APP tail --log pmon.log --fifo pmon.fifo &
cat pmon.fifo &     # receives the backlog
echo '[out] more' >> pmon.log       # ...and anything appended
kill %2             # kill the cat: tail prints "pmon: tail reader detached"
cat pmon.fifo       # reattach: nothing was lost
```

Watch the fast path: `strace -e trace=splice` on the C++ or Rust tail shows
`splice(3, NULL, 4, NULL, 65536, 0) = <n>` moving log bytes straight into
the pipe, and `= -1 EPIPE` at the moment the reader dies.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts observable
behavior: usage errors exit 2; `run` still mirrors exit statuses (3 stays
3, SIGKILL becomes 137 with `killed signal=9`); the pidfd engine supervises
a fail-once-then-succeed child with exactly two
`engine=pidfd child=... pidfd=...` spawn lines, `restart 1/3`, exit 0, and
a log holding two `[out] run-out` and two `[err] run-err` lines; the
sigchld engine drives the same capture with byte-identical restart lines
and no `pidfd=` field anywhere, exits 1 on an exhausted budget; the v2
timeout stop path still TERMs the child (`killed signal=15`,
`exiting (timeout)`, exit 0) with the pipes attached. The tail scenario
drives the full pipeline with real processes: the FIFO appears on disk, a
`cat` reader receives the backlog and then lines written by a second live
`supervise` run (supervise → log → tail → FIFO → cat), killing the cat
yields exactly one `pmon: tail reader detached` while `kill -0` proves the
relay survived, a reattached `cat` receives both the line appended during
the gap and later appends (no loss), and SIGTERM ends tail with
`pmon: exiting (signal)`, exit 0. Every phase transition is polled with a
bounded `grep` loop, never a blind sleep.
