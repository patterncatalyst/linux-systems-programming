# 05-errors-three-ways — copyx

A file copier, written three times — C++23, Go, and Rust — to establish the
book's error taxonomy: every failure is classified by *which side of the copy
broke*, the reason is printed in one fixed shape, and the exit code carries
the classification for scripts and callers.

```
copyx SRC DST
```

## Behavior contract (identical in all three languages)

| Outcome | stdout / stderr | Exit |
| --- | --- | --- |
| Success | stdout: `copied <N> bytes` | 0 |
| Bad usage (argc != 3) | stderr: `usage: copyx SRC DST` | 2 |
| Source-side failure (open/read SRC) | stderr: `copyx: <reason>` | 2 |
| Destination-side failure (open/write/close DST) | stderr: `copyx: <reason>` | 3 |

`<reason>` names the path (and the syscall when it is not `open(2)`), then the
kernel's explanation in `strerror(3)` form:

```
copyx: nope.bin: No such file or directory          # exit 2
copyx: read src.fifo: Input/output error            # exit 2
copyx: write /dev/full: No space left on device     # exit 3
copyx: close /dev/full: No space left on device     # exit 3
```

Two policies are deliberately visible in each implementation:

- **EINTR is retried.** `read(2)`/`write(2)` interrupted by a signal
  transferred nothing; each language has an explicit retry loop
  (`read_some`/`readSome`) rather than treating EINTR as an error.
- **Short writes are resumed.** `write_all`/`writeAll` advances past the
  bytes the kernel accepted and reissues the rest until the buffer is gone.
  The write side also *observes* `close(2)`, because that is where deferred
  IO errors surface — the read side's close result carries nothing actionable
  and is released by RAII/defer.

The copy loop uses a 64 KiB buffer; `DST` is created `0644` and truncated.

### How each language spells the same taxonomy

- **C++23** (`cpp/`): an RAII `Fd` owner (move-only, closes in the
  destructor, explicit `close()` returning `std::expected<void,
  std::error_code>` for the write side); every fallible helper returns
  `std::expected<T, std::error_code>`; `copy_file` maps those into a
  `Failure{message, exit_code}`.
- **Go** (`go/`): raw `unix.Open/Read/Write/Close` from
  `golang.org/x/sys/unix`; a custom `*opError` carrying phase, path, errno,
  and exit code, with `Unwrap()` so `errors.Is(err, unix.ENOENT)` still works;
  EINTR detected with `errors.Is(err, unix.EINTR)`. Go's own errno strings
  are lowercase, so the first letter is capitalized to match `strerror(3)`.
- **Rust 2024** (`rust/`): `rustix::fs::open` returning `OwnedFd`,
  `rustix::io::{read, write}` over `AsFd`, a `thiserror` enum with one variant
  per phase and an `exit_code()` mapping; the checked close hands the fd out
  of the `OwnedFd` with `into_raw_fd()` into `rustix::io::try_close` (the
  `try_close` crate feature) so the close result is observed exactly once.

## The demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary with `args`
  (with env `TARGET` set, deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally)
- `./demo.sh` — build, then run a local walkthrough of the taxonomy:
  a real 200 000-byte copy verified with `cmp`, a missing source (exit 2),
  and a write to `/dev/full` (exit 3)

The top-level `demo.sh` dispatches: `./demo.sh cpp run a b`, `./demo.sh all`,
`./demo.sh build`.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts observable
behavior, not exit-0:

- a 200 000-byte copy (size chosen so the final read is short) exits 0,
  prints `copied 200000 bytes`, and `cmp` proves the destination is
  byte-identical to the source;
- an empty source copies to an existing, empty destination with
  `copied 0 bytes`;
- a missing source exits 2 with `copyx: … No such file or directory`;
- writing to `/dev/full` exits 3 with `copyx: write /dev/full: …` and a
  reason mentioning space.
