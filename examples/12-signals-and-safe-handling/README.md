# 12-signals-and-safe-handling — pmon v1, the supervising loop

Chapter 12 grows `pmon` from "run one child and report" into a real
supervisor — and uses the restart loop as a vehicle for the chapter's actual
subject: how to consume signals *safely*. Each language deliberately uses a
different mechanism, because the three mechanisms together are the lesson.

```
pmon run -- CMD [ARGS...]                                  # v0: run once, report
pmon supervise [--max-restarts N] [--backoff-ms B] -- CMD  # v1: restart on failure
```

## Behavior (identical across C++, Go, and Rust)

- `run -- CMD` starts CMD with inherited stdio, prints
  `pmon: started pid <P>`, waits, prints
  `pmon: child <P> exited status=<S>` (or `killed signal=<N>`), and exits
  with the child's status (`128+N` for a signal death).
- `supervise` (defaults: `--max-restarts 5`, `--backoff-ms 100`):
  - a child that exits **0** ends supervision: exit 0.
  - a child that exits nonzero or dies to a signal is restarted after a
    backoff that **doubles** each time (B, 2B, 4B, ...):
    `pmon: restart #<k> (backoff <ms>ms)`, then a fresh
    `pmon: started pid <P'>`. After N failed restarts:
    `pmon: giving up after <N> restarts`, exit 1.
  - **SIGTERM / SIGINT**: forward SIGTERM to the child, reap it, print
    `pmon: shutting down (SIGTERM|SIGINT)`, exit 0.
  - **SIGHUP**: print `pmon: reload requested`, SIGTERM + reap the current
    child (its exit is not reported and does not count as a crash), start a
    replacement immediately, and reset the backoff and restart budget.
- No/bad arguments print a usage block on stderr and exit 2; runtime errors
  print `pmon: error: ...` on stderr and exit 1.

## Three signal mechanisms, one contract

The restart loop is identical; how each language *hears* a signal is not.
None of them runs meaningful code in signal context — that is the point.

- **C++ — signalfd(2) + poll(2), zero handlers.** SIGTERM/SIGINT/SIGHUP/
  SIGCHLD are blocked with `sigprocmask(2)` and read as `signalfd_siginfo`
  records from a file descriptor inside a `poll` loop (the backoff is just
  the poll timeout). Nothing ever executes asynchronously, so the
  async-signal-safe function list stops mattering — except between `fork()`
  and `execvp()`, where the child restores an empty mask (an inherited
  blocked SIGTERM survives exec and would make the child unkillable) using
  only async-signal-safe calls.
- **Go — signal.Notify + select.** The runtime owns the actual handlers and
  turns delivery into channel sends. Supervision is one `select` over three
  channels: signals, child exits from a `cmd.Wait()` goroutine, and the
  backoff `time.Timer`. Child liveness and signal forwarding go through
  `golang.org/x/sys/unix`.
- **Rust — the self-pipe trick via signal-hook.** Each signal is registered
  twice: `signal_hook::flag::register` sets a per-signal `AtomicBool`, and
  `low_level::pipe::register_raw` (which takes its dup of the pipe's write
  end as an `OwnedFd` and makes it non-blocking) writes a doorbell byte —
  `write(2)` is async-signal-safe. The main loop polls the doorbell's read
  end with `nix::poll`, drains it, checks the flags, and asks
  `waitpid(WNOHANG)` what actually happened. SIGCHLD needs no flag: the
  wait status is the source of truth.

## Demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (builds first if needed)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

The top-level `demo.sh` dispatches: `./demo.sh cpp run supervise -- sleep 5`
execs `cpp/demo.sh run supervise -- sleep 5`. The binary is always named
`app`.

Try it:

```sh
./demo.sh build
./demo.sh cpp run supervise --max-restarts 2 --backoff-ms 200 -- sh -c 'exit 1'
./demo.sh go  run supervise -- sleep 300 &   # then: kill -HUP %1 ; kill -TERM %1
./demo.sh rust run run -- sh -c 'exit 3' ; echo "propagated: $?"
```

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts behavior, not
exit codes alone:

- `run` propagates exit 3 and maps a SIGKILL death to exit 137 with the
  matching report lines;
- the crash loop against `sh -c 'exit 1'` with `--max-restarts 2
  --backoff-ms 120` prints `restart #1 (backoff 120ms)` and `restart #2
  (backoff 240ms)`, starts three distinct child pids, gives up, exits 1,
  and measurably sleeps (elapsed >= 340 ms by `date +%s%N`);
- SIGTERM and SIGINT to a live supervisor produce the shutdown line, exit
  0, and a reaped child (`kill -0` on the child pid fails afterwards);
- SIGHUP produces the reload line and a replacement child whose pid
  differs, with the old pid dead and the replacement alive — and a
  follow-up SIGTERM still shuts everything down cleanly.

Run it standalone from this directory:

```sh
LSP_LANG=rust REPO_ROOT=../.. lua verify.lua
```

or through the orchestrator:

```sh
python3 ../../scripts/test-all-examples.py --only 12-signals-and-safe-handling
```
