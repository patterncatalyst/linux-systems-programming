# 06-concurrency-three-ways

Chapter 6's example: `parhash`, a parallel checksummer implemented three times
— C++23, Go, and Rust — with each language's native concurrency toolkit, but
one shared observable contract so a single `verify.lua` covers all three.

## The program

```
parhash DIR
```

Walks every regular file under `DIR` (symlinks are never followed), hashes
each with FNV-1a 64 (implemented inline in all three sources — no hash
libraries), using a bounded pool of **4 workers**, then prints one line per
file, **sorted by relative path**, followed by a summary:

```
bbd23ea491ed9813  a.txt
4d038ef62a7d9c0b  b.txt
f839d20567c0a911  sub/c.txt
51d88627df287325  sub/deep/d.bin
cbf29ce484222325  zz-empty.dat
parhash: 5 files, 4 workers
```

Given the same tree, the three binaries produce byte-identical stdout.

**Errors:** missing/extra argument → `usage: parhash DIR` on stderr, exit 2;
`DIR` not a directory → `parhash: cannot walk DIR` on stderr, exit 1;
an unreadable file mid-walk → `parhash: skipping <relpath>` on stderr, and the
walk continues.

**SIGINT:** the program stops accepting work (the walker quits enumerating,
workers refuse queued-but-unstarted paths), drains in-flight hashes (the file
each worker is on always finishes), prints the sorted lines for everything
that completed, prints `parhash: interrupted` on stderr, and exits **130**.

## Three concurrency shapes, one contract

| | work distribution | cancellation |
|---|---|---|
| **C++23** | 4 `std::jthread` workers popping a mutex-guarded `std::deque` via a `condition_variable_any` whose wait is `stop_token`-aware | SIGINT is blocked in every thread; a watcher `jthread` `sigtimedwait()`s for it, flips an `std::atomic<bool>`, and `request_stop()`s a shared `std::stop_source` |
| **Go** | 4 worker goroutines receiving from an unbuffered `chan string` fed by a walker goroutine; results return on a second channel | `signal.NotifyContext` cancels a `context.Context`; workers `select` `ctx.Done()` ahead of the next path |
| **Rust** | 4 scoped threads (`std::thread::scope`) sharing one `mpsc::Receiver` behind a `Mutex`; results return on a second `mpsc` channel | `signal-hook` sets a shared `AtomicBool` that walker and workers poll between files |

The Go build runs with `-race` (see `go/demo.sh`) — this is the concurrency
chapter, so the race detector referees every verification run.

## Layout

```
06-concurrency-three-ways/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI
├── cpp/         # CMake preset build (GCC or clang), demo.sh
├── go/          # go build -race (toolchain go1.26.5), demo.sh
└── rust/        # cargo build (rustup pin 1.97.1; anyhow + signal-hook), demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (e.g. `./demo.sh run /etc`)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

All three `run` paths `exec` the binary, so a signal sent to the `demo.sh`
PID lands on `parhash` itself — which is exactly how `verify.lua` delivers
its `SIGINT`.

## Verification

`verify.lua` runs from this directory with `LSP_LANG` set to one language at
a time and asserts, per language:

1. a 5-file fixture (nested dirs, a binary file, an empty file) produces
   stdout **byte-identical** (`diff -u`) to the precomputed expected output —
   the same expected bytes for all three languages;
2. a missing argument prints the usage line and exits 2;
3. on a big sparse tree (~6 GiB logical, ~0 on disk, seconds of hashing),
   `kill -INT` after 100 ms yields only well-formed partial hash lines on
   stdout, no summary line, `parhash: interrupted` on stderr, and exit 130.

Verified on the Fedora 44 reference host (kernel 7.1.3-200.fc44): all three
languages PASS all 8 checks, Go under the race detector.
