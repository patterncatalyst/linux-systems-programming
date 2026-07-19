# 35-virtualization-and-kvm — overheadbench

Chapter 35's example: `overheadbench`, three tiny host-local microbenchmarks
implemented identically in C++23, Go, and Rust. The example itself only
measures **wherever it is run** — it makes no VM/container comparison on its
own. The chapter's point is made by running the *same binary* in three
places (bare host, a lab KVM guest, and a rootless podman container) and
tabulating the numbers side by side.

## The program

```
overheadbench [--bench syscall|mem|io|all] [--iters N]
```

Runs one or more of three benches and prints one line per bench:

```
bench=syscall metric=91.85 unit=ns/call
bench=mem metric=14.03 unit=GB/s
bench=io metric=1480.21 unit=ops/s
```

- **`syscall`** — a tight `getpid(2)` loop (a raw syscall each iteration, not
  a libc-cached value). Reports **ns/call**: mean wall time per syscall.
  This is the number the chapter contrasts against a VM's vmexit-trap cost
  and a container's near-zero added cost (containers share the host kernel;
  a syscall inside one *is* a host syscall).
- **`mem`** — allocates a 128 MiB buffer of `u64` words and reads it
  sequentially, summing every word, for a configurable number of passes.
  Reports **GB/s**: total bytes read across all passes divided by elapsed
  time. The sum is consumed after the clock stops so the compiler can't
  prove the scan is dead code and delete it.
- **`io`** — repeats a create → write(4096 bytes) → `fsync(2)` → unlink
  cycle. Reports **ops/s**: complete cycles per second. The scratch
  directory is created **relative to the current working directory**
  (where `demo.sh` runs the binary from), not the system temp directory —
  on many dev hosts `/tmp` is `tmpfs`, where `fsync` is nearly free and the
  number stops meaning anything as "storage overhead." Comparing this
  number meaningfully across host/VM/container requires each side to write
  to a comparably real filesystem.

`--bench` selects which bench(es) run (default `all`, in the fixed order
syscall → mem → io). `--iters` overrides the loop count for whichever
bench(es) run: it means "getpid calls" for `syscall`, "buffer passes" for
`mem`, and "create/write/fsync/unlink cycles" for `io`. Omitted, each bench
uses its own default (chosen so a run takes tens to a few hundred
milliseconds): `syscall` 200,000 calls, `mem` 16 passes, `io` 200 cycles.

Bad usage (an unknown `--bench` value, a flag with a missing value, an
unknown flag, or a non-numeric/zero `--iters`) prints
`usage: overheadbench [--bench syscall|mem|io|all] [--iters N]` to stderr
and exits 2 — never a silently wrong number.

## Layout

```
35-virtualization-and-kvm/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI (host-local sanity checks)
├── cpp/         # CMake preset build, demo.sh
├── go/          # go build, demo.sh
└── rust/        # cargo build, demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the same interface, identical
across the whole book:

- `./demo.sh` — build then run (`--bench all`)
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary with the given arguments
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally — this is how the chapter gets the VM-side numbers.

## Verification

`verify.lua` runs from this directory with `LSP_LANG` set to one language at
a time. It asserts, per language:

- the built binary exists;
- a default run (`--bench all`) exits 0 and prints exactly the three rows,
  in order `syscall`, `mem`, `io`, each parsing as `bench=<name>
  metric=<number> unit=<unit>` with the metric inside a generous but real
  sanity window (e.g. `syscall` between 1 ns and 5 ms per call, `mem`
  between 10 MB/s and 1 TB/s, `io` between 0.1 and 5,000,000 ops/s) — wide
  enough to tolerate a loaded CI host or unusual hardware, narrow enough to
  catch a zeroed-out, negative, or nonsensical measurement;
- selecting a single bench with `--bench` prints only that bench's row;
- `--iters` with a small explicit value (e.g. 10 or 500) still produces a
  sane, parseable row — guarding against a divide-by-zero at low iteration
  counts;
- bad usage (unknown `--bench` value, missing flag value, unknown flag,
  non-numeric or zero `--iters`) exits 2 and prints the usage line.

Real host numbers from this machine (Fedora 44, kernel 7.1.3-200.fc44,
`examples/` on an NVMe-backed btrfs filesystem — not `/tmp`):

| lang | syscall (ns/call) | mem (GB/s) | io (ops/s) |
|------|-------------------:|-----------:|-----------:|
| cpp  | 89–120             | 24–29      | 1,480–1,590 |
| go   | 90–159             | 12–14      | 1,450–1,670 |
| rust | 87–96              | 16–19      | 1,260–1,580 |

The exact numbers vary run to run — that's expected of a microbenchmark;
`verify.lua` checks shape and sane range, not a fixed value.

## What the chapter does with it (not asserted by `verify.lua`)

1. Run `./demo.sh <lang> run` on the bare host (the numbers above).
2. Deploy the same binary to `systems-target`
   (`TARGET=systems-target ./demo.sh <lang> run`) and record the VM numbers —
   expect a real `syscall` gap (vmexit/vmenter round trip on every trapped
   instruction) and comparable `mem`/`io` numbers once the guest is warmed
   up, modulo whatever the host's virtio/virtualization layer costs for
   storage.
3. Build the chapter-34 UBI 10 Containerfile for the same language, run the
   binary with `podman run --rm <image>` on the host, and record the
   container numbers — expect `syscall` and `mem` close to bare-host (a
   container's process runs directly on the host kernel and CPU), with `io`
   depending on the container's storage driver/overlay filesystem.
4. Tabulate host vs VM vs container for the pros/cons discussion: containers
   trade isolation strength for near-zero syscall/CPU overhead; a KVM guest
   pays a real (if small, with modern hardware virtualization) tax per
   trapped instruction in exchange for a separate kernel and stronger
   isolation.

## Mapping to a chapter

This is a copy of `examples/_template` (`scripts/new-example.sh
35-virtualization-and-kvm`) with the program body replaced by the
`overheadbench` benches described above. Keep the demo contract and
`verify.lua` shape intact so CI and the manifest (`examples/manifest.yaml`)
keep working.
