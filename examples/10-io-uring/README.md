# 10-io-uring — fwatch v2, the batched sync engine

Chapter 10 grows `fwatch` into a small directory sync tool with two copy
engines, and uses the difference between them to teach io_uring: submission
and completion rings shared with the kernel, batches of SQEs per syscall, and
linked operations.

```
fwatch scan DIR                                 # v0: polling snapshot
fwatch watch DIR --timeout MS                   # v1: inotify events
fwatch sync SRCDIR DSTDIR [--engine rw|uring]   # v2: batched copy (default rw)
```

## Behavior (identical across C++, Go, and Rust)

- `scan DIR` walks the tree and prints one line:
  `scanned <N> files <B> bytes` (regular files only; symlinks skipped).
- `watch DIR --timeout MS` watches `DIR` (non-recursive) via inotify for
  `MS` milliseconds, printing `event CREATE|MODIFY|DELETE <name>` per event
  and a final `watched <N> events`.
- `sync SRCDIR DSTDIR` recreates the directory tree (including empty
  directories) and copies every regular file, then prints
  `synced <N> files <B> bytes engine=<e> ms=<t>`.
- No/bad arguments print a usage block on stderr and exit 2; runtime errors
  print `fwatch: ...` on stderr and exit 1.

## The two engines

- `--engine rw` (all three languages): a plain `read(2)`/`write(2)` loop,
  128 KiB at a time — the baseline from earlier chapters.
- `--engine uring`:
  - **C++**: a self-contained educational io_uring layer built on raw
    `syscall(2)` wrappers — `io_uring_setup(2)`, `io_uring_enter(2)`, and the
    `mmap(2)`'d SQ ring, CQ ring, and SQE array (no liburing; the host lacks
    `liburing-devel`, and seeing the rings by hand is the point). Each file is
    copied as batches of linked READ→WRITE SQE pairs (`IOSQE_IO_LINK`), one
    pair per 128 KiB chunk, up to 8 pairs per `io_uring_enter`. Under
    `strace -c`, all data movement collapses into a handful of
    `io_uring_enter` calls.
  - **Rust**: the same batched linked-pair design on the `io-uring` crate
    (0.7), which owns the ring setup/mmap details the C++ version spells out.
  - **Go**: prints `engine=uring: unsupported in Go (see chapter 10)` and
    exits **64**. That is deliberate, not a gap: the Go runtime already
    multiplexes goroutines over epoll in the netpoller, and io_uring support
    (golang/go#31908) remains on hold. The chapter uses this asymmetry to
    discuss where each language's I/O story comes from.

## Demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (builds first if needed)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

The top-level `demo.sh` dispatches: `./demo.sh cpp run sync SRC DST` execs
`cpp/demo.sh run sync SRC DST`. The binary is always named `app`.

Try it:

```sh
./demo.sh build
mkdir -p /tmp/fw/src && echo hi > /tmp/fw/src/a.txt
./demo.sh cpp  run sync /tmp/fw/src /tmp/fw/dst --engine uring
./demo.sh rust run sync /tmp/fw/src /tmp/fw/dst --engine uring
./demo.sh go   run sync /tmp/fw/src /tmp/fw/dst --engine uring   # exit 64
```

## Verification

`verify.lua` (run per language with `LSP_LANG` set) builds a fixture tree —
five files totalling 1,348,634 bytes, sized to straddle the 128 KiB chunk and
the 8-pair batch limit — then asserts:

- exact `scanned`/`synced` counter lines,
- `diff -r` byte-equality of the rw copy (all languages) and the uring copy
  (C++ and Rust),
- the Go uring refusal message with exit 64,
- live `event CREATE ...` lines from `watch` while a file is created, and
- usage + exit 2 for a bare invocation.
