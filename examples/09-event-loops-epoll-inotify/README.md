# 09 — Event loops: epoll + inotify (`fwatch` v1)

`fwatch` is the file watcher that grows across Part II. Chapter 07/08 gave it
polling-era subcommands (`snapshot`, `diff`); this chapter adds `watch`, a
real-time watcher — and the three implementations deliberately showcase the
two ways Linux programs consume readiness:

| Language | Architecture |
|---|---|
| **C++23** | one explicit `epoll` loop over three fds — the inotify fd, a `timerfd` (drives both the 100 ms per-path debounce and the overall timeout), and a `signalfd` (SIGINT/SIGTERM as readable data). Single thread; every fd owned by an RAII wrapper; fallible setup returns `std::expected`. |
| **Rust (edition 2024)** | the same single-threaded epoll architecture via the `nix` crate (`Epoll`, `Inotify`, `TimerFd`, `SignalFd` — all `OwnedFd`-backed, closed on drop), with `anyhow` for error context. |
| **Go 1.26** | the idiomatic inversion: the runtime owns the poller. A goroutine reads the `O_NONBLOCK` inotify fd through `os.File` (parking on the netpoller, which is itself an epoll loop), and feeds a channel; `signal.Notify` and `time.After` are channels too; one `select` loop multiplexes all of it. |

All three expose identical observable behavior, so one `verify.lua` covers
them.

## CLI

```
usage: fwatch <command>
  snapshot DIR                  one line per regular file: name<TAB>size<TAB>mtime_ns
  diff OLD NEW                  compare two snapshots: created|modified|deleted <name>
  watch DIR [--timeout-ms T]    watch DIR (default 2000 ms) until timeout or SIGINT/SIGTERM
```

- `snapshot DIR` — top-level regular files only, sorted by name, printed to
  stdout as `name\tsize\tmtime_ns`.
- `diff OLD NEW` — reads two snapshot files and prints one line per changed
  name, in name order: `created <name>`, `deleted <name>`, `modified <name>`
  (size or mtime changed). Exit 0.
- `watch DIR` — prints `fwatch: watching DIR` to stderr, then one line per
  filesystem event to stdout:

  ```
  event: created|modified|deleted <name>
  ```

  Events are debounced 100 ms per path (a create followed immediately by
  writes collapses to one `created`; delete wins within a window; a fast
  delete+recreate reads as `modified`). On timeout the last line is
  `fwatch: exiting (timeout)`; on SIGINT/SIGTERM it is
  `fwatch: exiting (signal)`. Both exit 0.
- Exit codes: `0` success, `1` runtime error (e.g. missing directory), `2`
  usage error.

## Demo contract

Each language directory has the standard `demo.sh`:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (`app`); the local path
  `exec`s the binary so a backgrounded `run watch ...` can be signalled
  directly
- `./demo.sh` — build, then run a 1-second self-demo (`watch .`)
- With env `TARGET` set, `run` deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh`

The top-level `./demo.sh [cpp|go|rust|all|build]` dispatches per language.

## Try it

```sh
./demo.sh cpp build
mkdir -p /tmp/fw && ./cpp/build/release/app watch /tmp/fw --timeout-ms 5000 &
sleep 0.3; echo hi > /tmp/fw/x.txt; echo more >> /tmp/fw/x.txt; rm /tmp/fw/x.txt
wait   # one debounced "event: ..." stream, then "fwatch: exiting (timeout)"
```

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts observable
behavior: the v0 snapshot format and diff regression
(`modified a.txt` / `deleted b.txt` / `created c.txt` in name order), a
backgrounded `watch` that sees created/modified/deleted events for files
touched after startup, the debounce collapsing create+write into exactly one
event line, the clean `(timeout)` and `(signal)` exit lines with exit 0, and
the exit-code contract (2 for usage, 1 for a missing directory).
