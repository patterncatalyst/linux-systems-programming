# 15-virtual-memory — memmap

A self-inspecting virtual-memory tool, implemented three times (C++23, Go,
Rust) with identical CLI, output shapes, and exit codes. `memmap` performs one
kind of allocation, touches its pages, and then reports what the kernel now
says about *its own* address space:

```
memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|fault-walk [--mb N]
```

| mode | allocation | touch |
|------|-----------|-------|
| `stack` | large local buffer on the call stack (clamped to 4 MiB) | write 1 byte/page |
| `heap` | allocator memory (`std::vector` / `make([]byte,…)` / `Vec`) | write 1 byte/page |
| `mmap-anon` | private anonymous `mmap(2)` | write 1 byte/page |
| `mmap-file FILE` | read-only private `mmap(2)` of FILE (sized by the file) | read 1 byte/page |
| `fault-walk` | temp file of N MiB, mapped read-only, touched in 8 steps | read 1 byte/page |

`--mb N` sizes the allocation (default 64, range 1..1024; `stack` clamps to 4
so the buffer stays inside the default 8 MiB stack rlimit).

## Output contract

```
memmap: mode=<m> bytes=<b> pages=<p>
memmap: walk file=<path> steps=8                        (fault-walk only)
memmap: step=<i>/8 pages=<p> minor=<d> major=<d>        (fault-walk only, ×8)
memmap: maps excerpt
memmap:   <raw /proc/self/maps line>   <-- target (mode=<m>)
memmap: vmrss_before=<kb>KB vmrss_after=<kb>KB
memmap: faults minor=<n> major=<n>
```

- The maps excerpt echoes every `/proc/self/maps` line whose range overlaps
  the allocation, annotated with `<-- target`. For `mmap-file` the line names
  the mapped file and shows `r--p`; for the writable modes it shows `rw-p`.
- `vmrss_before` is sampled just before the allocation, `vmrss_after` after
  the touch loop, both from `VmRSS:` in `/proc/self/status`.
- The fault counts are `getrusage(RUSAGE_SELF)` deltas over the same window.
- Usage errors (missing/unknown `--mode`, bad `--mb`, stray arguments) print
  the usage line on stderr and exit 2. Runtime errors (missing or empty FILE)
  print `memmap: error: …` on stderr and exit 1; the exact message text is
  each language's native errno rendering.

Example (heap, defaults — 64 MiB touched means ~64 MiB of RSS growth and one
minor fault per touched 4 KiB page):

```
memmap: mode=heap bytes=67108864 pages=16384
memmap: maps excerpt
memmap:   7fe0badd9000-7fe0bedda000 rw-p 00000000 00:00 0    <-- target (mode=heap)
memmap: vmrss_before=3908KB vmrss_after=69448KB
memmap: faults minor=16385 major=0
```

## What it teaches

- **Demand paging.** Every mode allocates address space first (VSZ) and only
  pays for physical pages when they are touched: the RSS delta and the minor
  fault delta line up with the touched page count, not the allocation call.
- **Fault-around.** In `fault-walk` and `mmap-file`, 512 touched file pages
  produce ~32 minor faults per step, not 512 — the kernel maps a batch of
  surrounding page-cache pages per fault. Anonymous writes fault one page at
  a time, so `heap`/`mmap-anon` report ~1 fault per page.
- **Minor vs major.** The walk file is written immediately before it is
  mapped, so every page is in the page cache and `major=0`; a major fault
  would mean a real disk read.
- **Where allocations live.** A 64 MiB `malloc`/`Vec`/`make` does not extend
  `[heap]` — the allocator satisfies it with a private anonymous mapping, and
  the excerpt shows exactly that region.

## Per-language notes

- **C++** (`cpp/src/main.cpp`): RAII wrappers (`Fd`, `Mapping`, `TempFile`),
  `std::expected` error plumbing, `std::println`. The stack buffer lives in a
  `noinline` worker whose caller takes the fault baseline first.
- **Go** (`go/main.go`): wrapped errors, `golang.org/x/sys/unix` for
  `Mmap`/`Getrusage`. The stack touch runs on a dedicated goroutine with the
  result returned over a channel. Goroutine stacks are runtime-managed heap
  memory, so the stack-mode excerpt shows an anonymous region, not `[stack]`.
- **Rust** (`rust/src/main.rs`): `Result` + `?` with `anyhow`, `rustix::mm`
  for the mappings (RAII `Mapping` unmaps on drop), `libc::getrusage`. The
  baseline is taken before entering the big-frame worker because LLVM's stack
  probing touches every frame page at function entry.

## Run it

```
./demo.sh              # build + run all three (usage exits 2 without a mode)
./demo.sh cpp run --mode heap
./demo.sh go run --mode fault-walk --mb 16
./demo.sh rust run --mode mmap-file /etc/os-release
```

Each language directory follows the book-wide demo contract: `build`, `run
[args]`, and `TARGET=<vm> … run` to deploy via `scripts/lab/deploy-to-vm.sh`
(this chapter's verification runs entirely on the local host).

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts behavior, not
just exit codes: usage/error exit codes, exact header lines (`bytes`/`pages`
arithmetic, stack clamping, file-sized `mmap-file`), the annotated maps
excerpt including permissions and the mapped file name, heap and `mmap-anon`
RSS deltas of at least half the touched size, nonzero minor-fault deltas, the
8-step fault-walk table shape with accumulating counts, and that the walk's
temp file is unlinked afterwards.
