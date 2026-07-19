# 29-valgrind-sanitizers-miri — bugfarm

Chapter 29 is a menu of five seeded defects, one program each in C++, Go,
and Rust:

```
app <leak|uaf|uninit|overflow|race>
```

Each subcommand runs a small routine that exhibits exactly one memory or
concurrency bug — *where the language lets it*. This is not a program with
a normal happy path: the point of the chapter is that a plain run of most
of these subcommands looks fine (or crashes somewhere unhelpful, far from
the real bug) and it takes the right tool, pointed at the right binary, to
actually name the defect.

## What's expressible in each language

Memory safety isn't a spectrum here so much as a cliff: C++ has none of it
compiled in, Go trades manual memory management for a GC and runtime
bounds/nil checks, and Rust's borrow checker rejects three of these five
bugs before the program can exist at all.

| bug | C++ | Go | Rust |
|---|---|---|---|
| `leak` | heap alloc, never freed | goroutine blocked forever (leaked) | `Box::leak` (safe API, opts out of reclamation) |
| `uaf` | use-after-free | — (GC won't free a live reference) | — (borrow checker) |
| `uninit` | read of indeterminate value | — (every value is zeroed) | — (no way to name an uninitialised value in safe code) |
| `overflow` | heap buffer overflow, one past the end | — (slice/array access panics, checked) | — (bounds-checked, or rejected at compile time for a fixed index) |
| `race` | data race on a shared int | data race on a shared int (allowed; GC keeps it memory-safe) | — (`Send`/`Sync` rules reject the unsynchronized sharing) |

Where a bug isn't expressible, the subcommand still exists (so the CLI
shape is identical across all three) — it just prints a fixed note and
exits **64** (`EX_USAGE`) instead of attempting anything:

- Go: `go: prevented by the runtime — see chapter`
- Rust: `rust: prevented at compile time — see chapter`

That message *is* the finding for those cells, not a cop-out: it's the
same "no bug to reproduce" result you'd get chasing the equivalent C++ bug
in that language, just arrived at differently (a runtime check vs. a
rejected compilation) — and it's reproducible non-interactively, which is
this chapter's whole bar.

Bad usage (no subcommand, or an unrecognized one) prints
`usage: app <leak|uaf|uninit|overflow|race>` on stderr and exits 2, same
shape as every other chapter's `app`.

## Catching each C++ bug: the tool table

C++ is the only language that expresses all five, so it's the one where
"run it under the matching tool" actually means something for every
subcommand:

| bug | tool | build | signature this chapter asserts | exit |
|---|---|---|---|---|
| `leak` | valgrind memcheck | plain (`build/release`) | `definitely lost` | 1 |
| `uninit` | valgrind memcheck | plain (`build/release`) | `Conditional jump or move depends on uninitialised value(s)` | 1 |
| `uaf` | ASan (+UBSan) | `cmake --preset asan` | `heap-use-after-free` | 1 |
| `overflow` | ASan (+UBSan) | `cmake --preset asan` | `heap-buffer-overflow` | 1 |
| `race` | TSan | `cmake --preset tsan` | `WARNING: ThreadSanitizer: data race` | 66 |

The `asan` preset was already in this book's CMake template (`-O1 -g
-fno-omit-frame-pointer -fsanitize=address,undefined`); this chapter adds
a matching `tsan` preset. Neither is part of the `demo.sh` contract —
build them directly:

```console
$ cd cpp
$ cmake --preset asan && cmake --build --preset asan
$ cmake --preset tsan && cmake --build --preset tsan
$ ./build/asan/app uaf
==12345==ERROR: AddressSanitizer: heap-use-after-free on address ...
$ ./build/tsan/app race
WARNING: ThreadSanitizer: data race (pid=...)
```

For Rust, valgrind confirms the one expressible bug (`leak`), and — where
available — miri adds a second, independent confirmation (see below).

## A gotcha worth knowing: the optimizer can eat the bug

Building `leak`/`uaf`/`uninit`/`overflow` at `-O2` with no barrier, GCC
proved on this host that the allocation was never otherwise observed and
deleted the whole thing — `malloc`, the write, and all — before valgrind
ever saw an allocation to lose. Same story in Rust release builds: a
`Box::leak`'d `Vec` whose only observable use is `.len()` (a
compile-time-known constant) can be fully constant-folded away, allocation
included. Every `do_*` function in `cpp/src/main.cpp` routes its pointer
through a `touch()` helper (`asm volatile("" : : "r,m"(value) : "memory")`
— the same trick as benchmark libraries' `DoNotOptimize`), and the Rust
`leak()` does the same with `std::hint::black_box`. Pull either barrier out
and re-run valgrind: the leak silently disappears. This is itself a small
lesson about why "it didn't crash" is not evidence of "there's no bug" —
the compiler is allowed to make dead code disappear, and an unobserved bug
is dead code.

Separately: a plain (uninstrumented) run of C++ `overflow` can abort with
a glibc `Fatal glibc error: malloc.c:... assertion failed` — the
one-past-the-end write corrupts the next heap chunk's bookkeeping, and
`delete[]`'s consistency check catches the corruption far from where it
happened. That crash, when it happens, *is* the natural (if unhelpful)
consequence of the bug; it's why the ASan build exists.

## miri: available on nightly, not on the pinned toolchain

`rust-toolchain.toml` pins `1.97.1`, and `rustup component add miri
--toolchain 1.97.1` fails on this host (`component 'miri' ... is
unavailable for download for channel '1.97.1'`) — miri tracks nightly, not
stable. Where a `nightly` toolchain with the `miri` component happens to be
installed, `verify.lua` runs it as a **bonus, informational-only** check
(`cargo +nightly miri run -- leak`, expecting `memory leaked` in its
output) and prints what it found; where it isn't installed, it prints why
it's skipping and moves on. Neither path can fail the example — the
valgrind check above is what's actually asserted for Rust's `leak`. This
is exactly the "degrade gracefully" case: miri is a nice extra sample of
"a completely different technique (symbolic interpretation instead of
runtime instrumentation) finds the same bug," not a required tool for this
chapter.

## Try it

```console
$ ./demo.sh cpp run leak
bugfarm: leak: allocated 4096 bytes on the heap, never freed (intentional)
$ valgrind --leak-check=full --error-exitcode=1 ./cpp/build/release/app leak
==...== 4,096 bytes in 1 blocks are definitely lost in loss record 1 of 1

$ ./demo.sh go run leak
bugfarm: leak: goroutines before=1 after=2 leaked=1

$ ./demo.sh go run uaf
go: prevented by the runtime — see chapter          # exit 64

$ ./demo.sh rust run leak
bugfarm: leak: leaked 65536 bytes via Box::leak, never reclaimed (intentional)
$ valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
    --error-exitcode=1 ./rust/target/release/app leak
==...== 65,536 bytes in 1 blocks are definitely lost in loss record 2 of 2

$ ./demo.sh rust run race
rust: prevented at compile time — see chapter        # exit 64
```

(PIDs and valgrind's `==NNNNN==` prefixes vary by run; the signature
strings and exit codes are what's stable and what `verify.lua` checks.)

## Demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only (the plain, uninstrumented binary)
- `./demo.sh run [args]` — run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

The top-level `demo.sh` dispatches: `./demo.sh cpp run leak` execs
`cpp/demo.sh run leak`. The binary is always named `app`. Because this
chapter's whole point is tool-assisted detection, the *instrumented*
builds (C++ ASan/TSan presets, the Go `-race` binary) live outside that
contract — see the tables above for how to build and run them directly.

There's no sensible default action for a bare `./demo.sh` (no subcommand)
here — unlike most chapters, this one has no "just run it" happy path — so
running with no subcommand prints the usage line and exits 2. That's
intentional, not an oversight.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) builds the plain
binary, checks the shared bad-usage/unknown-bug behavior, then — per
language — builds whatever instrumented variant that language's real bugs
need and asserts the detecting tool exits nonzero (66 for both race
detectors' default halt code, 1 otherwise) with the expected signature
string in its output, and that every "prevented" subcommand exits 64 with
its fixed message. Rust's miri check is informational only, per the
"degrade gracefully" rule for a pinned toolchain that doesn't ship miri.
