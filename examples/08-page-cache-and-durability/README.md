# 08-page-cache-and-durability — iobench

Chapter 8's example: the same small write benchmark implemented three times —
C++23, Go, and Rust — to show what the page cache gives you (throughput) and
what it defers (durability). One binary, three sync strategies:

```
iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE
```

It writes M MiB (default 64) to FILE in 64 KiB blocks:

- `buffered` — plain `write(2)`s land in the page cache. The report line times
  the writes alone; a second line `fsync_ms=<t>` times the closing
  `fdatasync(2)` that actually makes the data durable. The gap between the two
  is the chapter's point.
- `fsync-every` — `fdatasync(2)` every N blocks (default 8, plus a trailing
  sync if the block count is not a multiple of N), all inside the timed
  region. This is what "durable as you go" costs.
- `direct` — `O_DIRECT` with 4096-aligned buffers, bypassing the page cache.
  If the filesystem rejects `O_DIRECT` at `open(2)` with `EINVAL`, the program
  prints `direct: unsupported on this filesystem` and exits 4.

## Output and exit codes

```
mode=<m> bytes=<b> ms=<t> MiB/s=<r>     # always, on stdout
fsync_ms=<t2>                           # buffered mode only
```

`bytes` is exact (M × 1048576), `ms` is integer milliseconds, `MiB/s` has one
decimal. Exit codes: `0` success, `2` usage error (usage line on stderr), `4`
`O_DIRECT` unsupported, `1` any other I/O failure.

Representative run on the reference host (Fedora 44, btrfs, NVMe):

```
$ ./demo.sh go run --mode buffered   bench.bin
mode=buffered bytes=67108864 ms=21 MiB/s=2975.5
fsync_ms=17
$ ./demo.sh go run --mode fsync-every bench.bin
mode=fsync-every bytes=67108864 ms=143 MiB/s=445.7
$ ./demo.sh go run --mode direct      bench.bin
mode=direct bytes=67108864 ms=81 MiB/s=786.2
```

Buffered "throughput" is mostly the memcpy into the cache; the durable-as-you-go
and cache-bypass modes pay the real device cost inside the timed region. On
tmpfs (`/tmp`), `fdatasync` is nearly free and the three modes converge —
worth showing in class, and why `verify.lua` asserts shapes, not timings.

## Implementations

All three expose identical observable behavior (CLI, output, exit codes):

- `cpp/` — C++23: RAII `Fd` wrapper around the descriptor,
  `std::expected<T, std::error_code>` for every fallible op, `std::println`,
  `std::aligned_alloc` under a `unique_ptr` for the O_DIRECT buffer.
- `go/` — Go 1.26: raw syscalls via `golang.org/x/sys/unix`
  (`Open`/`Write`/`Fdatasync`), wrapped errors with `%w`,
  `errors.Is(err, unix.EINVAL)` for the O_DIRECT probe, manual 4096 alignment
  of the block.
- `rust/` — Rust 2024: `rustix` (`open`/`write`/`fdatasync` over `OwnedFd`),
  `anyhow` with context for error paths, `Errno::INVAL` match for the
  O_DIRECT probe, aligned block carved from a stable `Vec`.

Every write loop retries short writes and `EINTR` — a 64 KiB `write(2)` is not
guaranteed to complete in one call.

## The demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

The top-level `demo.sh` is the dispatcher: `./demo.sh cpp run --mode buffered
out.bin` execs `cpp/demo.sh run --mode buffered out.bin`.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts behavior, not just
exit status:

- no args → usage line, exit 2; unknown mode → exit 2 and no file created
- buffered 8 MiB → report line with `bytes=8388608`, `fsync_ms` line directly
  after it, and the file is exactly 8388608 bytes on disk (`stat`)
- fsync-every 4 MiB (`--every 4`) → single report line with `bytes=4194304`,
  no `fsync_ms` line, file exactly 4194304 bytes
- direct 4 MiB → either the report line and correct size, or the documented
  `EINVAL` message with exit 4 — both are valid outcomes per filesystem

Timing fields are asserted for shape only (`ms=%d+`, `MiB/s=%d+%.%d`);
asserting magnitudes would flake. An `strace -c` cross-check of
`--mode fsync-every --every 4 --size-mb 4` shows exactly 64 data writes and
16 `fdatasync` calls — the syscall pattern the chapter describes.
