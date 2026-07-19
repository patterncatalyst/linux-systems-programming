# 07 — fds, RAII, and the VFS: `fwatch` v0

The file-watcher artifact makes its first appearance. `fwatch` v0 has no
events and no timing — it is a purely deterministic snapshot/diff tool built
on directory file descriptors, implemented three times (C++23, Go, Rust) with
identical observable behavior.

The point of the chapter: a directory is just another fd. Every implementation
walks the tree with *dirfd-relative* syscalls — `openat(2)` to descend,
`fstatat(2)` with `AT_SYMLINK_NOFOLLOW` to classify — and every fd lives
inside an owning wrapper so it is closed exactly once on every path,
including errors:

- **C++**: a move-only `Fd` class (RAII around `close(2)`) and a `DirStream`
  class around `DIR*` — note that `fdopendir(3)` *takes ownership* of the fd,
  so `DirStream::adopt(Fd)` releases the fd into the stream and `closedir(3)`
  is the single cleanup. Fallible operations return
  `std::expected<T, std::error_code>` (or a small `Failure` carrying the
  formatted path context).
- **Go**: `unix.Openat` / `unix.Fstatat` from `golang.org/x/sys/unix`, with
  `os.NewFile` adopting each directory fd so one deferred `Close` owns it.
  Errors are wrapped with `%w`; `errors.Is(err, unix.ENOENT)` skips entries
  that vanish mid-walk and `errors.As` digs the raw errno back out for the
  common message format.
- **Rust**: `rustix::fs::{open, openat, statat}` returning `OwnedFd` — drop is
  the close. `rustix::fs::Dir::read_from` dups the fd for its own cursor, so
  the `OwnedFd` stays usable as the anchor for the `*at()` calls. `anyhow`
  carries the error context; `?` does the plumbing.

## CLI

The binary is named `app` (as everywhere in the book) but speaks as `fwatch`:

```
usage: fwatch snapshot DIR
       fwatch diff DIR SNAPSHOT_FILE
```

**`fwatch snapshot DIR`** walks `DIR` recursively and prints one line per
regular file to stdout, sorted bytewise by path:

```
<relpath> <size-bytes> <mtime-unix-seconds>
```

Directories are descended into but not listed; symlinks, sockets, pipes, and
devices are ignored in v0. Entries that disappear between `readdir` and
`fstatat` are silently skipped.

**`fwatch diff DIR SNAPSHOT_FILE`** re-scans `DIR`, compares against the
saved snapshot, and prints — in sorted path order —

```
+ <path>        new file
- <path>        file is gone
~ <path>        size or mtime changed
fwatch: <A> added, <R> removed, <M> modified
```

then exits 0 (a non-empty diff is not an error). The summary line is always
printed, even when all counts are zero.

Snapshot lines are parsed from the right (the last two space-separated fields
are size and mtime), so paths containing spaces round-trip.

**Exit codes**: 0 success, 1 runtime error (`fwatch: error: ... (errno N)` on
stderr, identical text in all three languages), 2 usage error (usage text on
stderr).

## Layout

```
07-fds-raii-and-the-vfs/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI
├── cpp/         # CMake preset build, demo.sh
├── go/          # go build, demo.sh
└── rust/        # cargo build, demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh` — build, then run a self-contained fixture demo: create a small
  tree in a temp dir, `snapshot` it, mutate it (append / remove / create),
  and `diff` it against the saved snapshot.
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary with `args`
  (e.g. `./demo.sh run snapshot /etc`)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

The top-level `demo.sh` is only a dispatcher: `./demo.sh cpp run snapshot .`
execs `cpp/demo.sh run snapshot .`.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) builds a fixture tree
whose mtimes are pinned with `touch -d @EPOCH`, then asserts observable
behavior end to end:

1. `snapshot` output is byte-for-byte the four expected sorted entry lines.
2. `diff` against the unchanged tree prints only
   `fwatch: 0 added, 0 removed, 0 modified`.
3. After adding `new.txt`, deleting `gone.txt`, and appending to + retouching
   `a.txt`, `diff` prints exactly `~ a.txt`, `- gone.txt`, `+ new.txt`, and
   the `1 added, 1 removed, 1 modified` summary — nothing else.
4. A fresh `snapshot` reflects the new size and mtime of `a.txt`.
5. Error paths: missing directory → exit 1 with `(errno 2)`; missing snapshot
   file → exit 1; missing arguments → exit 2 with the usage text.
