# 16-mmap-and-shared-mappings — shmkv v0

A tiny key/value store whose entire database is one `mmap`'d file, implemented
three times — C++23, Go, and Rust — against **one on-disk format**. That format
is the whole point: the Rust binary can create a store, the Go binary can write
into it, and the C++ binary can read it back, because all three agree on every
byte. `verify.lua` proves this with `cmp(1)`, not by trusting exit codes.

Every process maps the file `MAP_SHARED`, so stores land in the shared page
cache and are visible to any other process mapping the same file; `msync(MS_SYNC)`
pushes dirty pages to disk before a write reports success.

## The on-disk format (byte-exact)

```
offset 0    6 bytes    magic "SHKV1\0"           53 48 4b 56 31 00
offset 6    4 bytes    slot_count, u32 little-endian
offset 10   slot_count slots, 256 bytes each:
            [  0.. 64)   key,   NUL-padded  (max 63 bytes; key[0] == 0 means empty)
            [ 64..256)   value, NUL-padded  (max 191 bytes)
```

File size is always exactly `10 + slot_count * 256` bytes — `create --slots 8`
produces a 2058-byte file, and any file whose size disagrees with its header is
rejected as `not a shmkv v0 store`. `create` truncates to zero and extends with
`ftruncate`, so every slot starts as guaranteed-zero bytes: "empty" falls out
of the file format for free.

## CLI

```
shmkv create FILE --slots N     # ftruncate + mmap + init header
                                #   -> "created FILE: N slots, SIZE bytes"
shmkv set FILE KEY VALUE        # linear probe: overwrite KEY's slot, else
                                #   first empty; msync -> "set KEY"
shmkv get FILE KEY              # value on stdout, or exit 4
shmkv dump FILE                 # "key=value" per pair, key-sorted (stdout);
                                #   "shmkv: n/N slots used" (stderr)
```

Exit codes, identical in all three languages:

| code | meaning                                                        |
|------|----------------------------------------------------------------|
| 0    | success                                                        |
| 1    | cannot open / not a shmkv v0 store / mmap-msync failure        |
| 2    | usage error, key > 63 bytes, value > 191 bytes, empty key      |
| 4    | `get`: key not found (`shmkv: key not found` on stderr)        |
| 5    | `set`: store full (`shmkv: store full (N slots)` on stderr)    |

## What each language shows

- **C++** (`cpp/src/main.cpp`) — RAII throughout: an `Fd` class owns the
  descriptor, a `Mapping` class owns the `mmap`/`munmap` pair and exposes
  `msync` as `sync()`; every fallible path returns `std::expected`, and output
  goes through `std::println`.
- **Go** (`go/main.go`) — `unix.Mmap` returns the mapping as a `[]byte`, and the
  discipline is never touching that slice after `unix.Munmap`; errors wrap
  causes with `%w` while a `cliError` type pins the exact stderr line and exit
  code; `errors.Is`/`errors.As` route them in `main`.
- **Rust** (`rust/src/main.rs`) — the `memmap2` crate, with the program's single
  `unsafe` block confined to one function (`map_shared`) behind an explicit
  boundary comment spelling out why the call is sound; the `File` owns the fd
  as an `OwnedFd`, and drop order (map before file) mirrors the C++ RAII types.

The dump ordering is bytewise key sort in all three, and `set` clears the whole
256-byte slot before writing, so a shorter value never leaves a longer one's
tail behind — both details matter for the byte-identical guarantee.

## Run it

```
./demo.sh build              # build all three
./demo.sh cpp run create /tmp/db.kv --slots 8
./demo.sh go  run set /tmp/db.kv answer 42
./demo.sh rust run dump /tmp/db.kv
```

Each language directory keeps the book-wide demo contract: `./demo.sh` builds
then runs, `./demo.sh build` builds only, `./demo.sh run [args]` runs the built
binary, and with `TARGET` set `run` deploys to that lab VM instead.

## Verification

`verify.lua` (one pass per `LSP_LANG`) asserts behavior, not exit-0:

- header bytes via `od` (`53 48 4b 56 31 00` + LE slot count), file size via
  `stat` (2058 bytes for 8 slots);
- CLI shapes: `set`/`get`/`dump` output, overwrite reusing its slot (2/8 used),
  exit 4 on missing key, exit 5 on a full store, exit 2 on oversized keys or
  values, exit 1 on a non-store file;
- **cross-language interop**: Rust creates and writes a store, Go writes into
  it, C++ and Go read it back; the three `dump` outputs are compared with
  `cmp`; then C++ creates a second store replaying the same operations and
  Rust reads it — and `cmp` shows the two store files are byte-identical.
