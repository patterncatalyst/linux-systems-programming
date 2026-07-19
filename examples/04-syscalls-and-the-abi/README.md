# 04-syscalls-and-the-abi — sysprobe

A syscall specimen: the same small program three times — C++23, Go, and
Rust — performing a fixed, labeled sequence of system calls so you can watch
each language's runtime hit the kernel ABI under `strace(1)`:

1. **open** — `openat(2)` of a scratch file in `$TMPDIR` (default `/tmp`),
   preferring an anonymous `O_TMPFILE` inode, with a `mkstemp`-style
   create+unlink fallback on filesystems without `O_TMPFILE` support
2. **write** — `write(2)` of a 25-byte payload to that fd, then `close(2)`
   (RAII scope exit in C++, checked `unix.Close` in Go, `OwnedFd` drop in Rust)
3. **sleep** — `nanosleep(2)` for 10 ms, resuming on `EINTR`
4. **random** — `getrandom(2)` for 16 bytes, looping over partial reads

## CLI and output

The binary is named `app` and takes no arguments (`app <anything>` prints
`usage: app` to stderr and exits 2). On success it prints exactly:

```
step=open ok
step=write ok
step=sleep ok
step=random ok
sysprobe: 4 steps ok
```

and exits 0. On failure it prints `sysprobe: <step>: <details>` to stderr and
exits 1. All three implementations expose identical observable behavior, so
one `verify.lua` covers them all.

## Watching it under strace

The point of the program. For example:

```
strace -e trace=openat,write,close,nanosleep,getrandom ./cpp/build/release/app
```

shows the labeled sequence — `openat(... O_TMPFILE|O_RDWR|O_CLOEXEC)`, the
25-byte `write`, the `close`, `nanosleep({tv_sec=0, tv_nsec=10000000})`, and
`getrandom(..., 16, 0) = 16` — interleaved with each runtime's own startup
syscalls, which is exactly the contrast the chapter is about.

## The demo contract

Every language directory has a `demo.sh` with the same interface:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

The top-level `demo.sh` is the dispatcher: `./demo.sh cpp run` execs
`cpp/demo.sh run`; bare `./demo.sh` (or `all`) builds and runs all three.

## Verification

`verify.lua` runs `./demo.sh $LSP_LANG run` via `scripts/lib/checks.lua` and
asserts exit 0, all four `step=<name> ok` lines, the `sysprobe: 4 steps ok`
summary, and that the five lines appear in order as one contiguous block.
Exit 0 = pass, 1 = fail, 77 = skip.
