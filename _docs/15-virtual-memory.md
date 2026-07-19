---
title: "Virtual memory"
order: 15
part: "Memory"
description: "The address space read line by line from /proc/self/maps, brk versus mmap in each language's allocator under strace, minor and major page faults measured with getrusage and perf, RSS versus VSZ versus PSS from a live smaps_rollup, overcommit and OOM-killer policy, and a tour of madvise — with memmap, a self-inspecting memory tool in all three languages."
duration: 45 minutes
---

Part 3 closed with `pmon` dropping privileges in chapter 14; this part turns
inward, to the memory every one of those processes lives in. The one new idea
is that **an address is a promise, not a page**: when `malloc`, `make`, or
`Vec` hands you 64 MiB, the kernel has recorded a range in a bookkeeping
structure (a *virtual memory area*) and given you nothing else — no RAM moves
until you touch a page and the resulting fault forces the kernel to make good
on the promise, one 4 KiB installment at a time. Everything in this chapter —
the `/proc/self/maps` layout, the brk/mmap split, minor versus major faults,
RSS versus VSZ versus PSS, overcommit — is a different view of that
promise-then-pay machinery, and this part's artifact, `shmkv`, will spend the
next chapters building a shared key-value store directly on top of it.

The code is in `examples/15-virtual-memory/`: `memmap`, a tool that performs
one kind of allocation (stack, heap, anonymous mmap, file mmap, or a stepped
fault walk), touches the pages, and then reports what the kernel now says
about *its own* address space. `./demo.sh` builds all three implementations;
the `README.md` pins the CLI, the output contract, and the exit codes.

{% include excalidraw.html
   file="15-address-space-map"
   alt="A vertical stack of address-space regions from high to low: vsyscall, the stack growing down, an unmapped gap, the mmap region containing vdso and vvar pages, ld-linux and libc segments, and an accented 64 MiB anonymous rw-p mapping, another gap, the brk-grown heap, data and relro segments, and the executable text at the bottom, each labeled with a real address from this chapter's runs."
   caption="Figure 15.1 — one x86-64 process address space, high addresses at the top; every address label comes from the /proc maps output walked below" %}

> **Tools used** — `strace`, `perf`, `dd`, `ps`, `pgrep`, and `cat`/`grep`
> over `/proc` and `/sys` (host); `oomkill`/`bpftrace` (lab VM, exercised in
> Part 8). Everything here is checked by `scripts/check-host.sh` or ships
> with Fedora.

## The address space, line by line

`/proc/self/maps` lists every promise the kernel is currently holding for a
process — one line per VMA. The cleanest specimen is `cat` reading its own
maps, because whatever process opens that file sees *itself*:

```bash
[host]$ cat /proc/self/maps
55a5a49ff000-55a5a4a05000 r-xp 00000000 00:24 28358601    /usr/bin/cat
55a5a4a05000-55a5a4a08000 r--p 00006000 00:24 28358601    /usr/bin/cat
55a5a4a08000-55a5a4a09000 r--p 00008000 00:24 28358601    /usr/bin/cat
55a5a4a09000-55a5a4a0a000 rw-p 00009000 00:24 28358601    /usr/bin/cat
55a5cdb86000-55a5cdba7000 rw-p 00000000 00:00 0           [heap]
7f7c5ac00000-7f7c68a92000 r--p 00000000 00:24 29927626    /usr/lib/locale/locale-archive
7f7c68b11000-7f7c68b56000 rw-p 00000000 00:00 0
7f7c68b56000-7f7c68cc8000 r-xp 00000000 00:24 29927031    /usr/lib64/libc.so.6
7f7c68cc8000-7f7c68d41000 r--p 00172000 00:24 29927031    /usr/lib64/libc.so.6
7f7c68d41000-7f7c68d45000 r--p 001eb000 00:24 29927031    /usr/lib64/libc.so.6
7f7c68d45000-7f7c68d47000 rw-p 001ef000 00:24 29927031    /usr/lib64/libc.so.6
7f7c68d47000-7f7c68d4f000 rw-p 00000000 00:00 0
7f7c68d6b000-7f7c68d6d000 rw-p 00000000 00:00 0
7f7c68d6d000-7f7c68d71000 r--p 00000000 00:00 0           [vvar]
7f7c68d71000-7f7c68d73000 r--p 00000000 00:00 0           [vvar_vclock]
7f7c68d73000-7f7c68d75000 r-xp 00000000 00:00 0           [vdso]
7f7c68d75000-7f7c68d9f000 r-xp 00000000 00:24 29927028    /usr/lib64/ld-linux-x86-64.so.2
7f7c68d9f000-7f7c68dab000 r--p 0002a000 00:24 29927028    /usr/lib64/ld-linux-x86-64.so.2
7f7c68dab000-7f7c68dad000 r--p 00036000 00:24 29927028    /usr/lib64/ld-linux-x86-64.so.2
7f7c68dad000-7f7c68dae000 rw-p 00038000 00:24 29927028    /usr/lib64/ld-linux-x86-64.so.2
7f7c68dae000-7f7c68daf000 rw-p 00000000 00:00 0
7ffcda504000-7ffcda526000 rw-p 00000000 00:00 0           [stack]
ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0   [vsyscall]
```

(Whitespace compressed; the columns are start–end, permissions, file offset,
device, inode, and path.) The fourth permission character is the one people
miss: `p` means *private* — writes trigger copy-on-write and never reach the
file — while `s` would mean *shared*, the mode chapter 16 is built on. Read
the file top to bottom and the classic layout falls out. The binary itself
appears four times — `r-xp` text, `r--p` read-only data, a second `r--p` that
is the relro region (writable during relocation, sealed read-only before
`main`), and `rw-p` data/bss — all file-backed at increasing offsets, all
shareable between every process running `cat`. Above it sits `[heap]`, the
one region the legacy `brk(2)` syscall grows. Then a jump to the high
`7f7c…` range: the **mmap region**, where the kernel hands out addresses
top-down — the locale archive `cat` mapped read-only, `libc` in the same
four-segment pattern as the binary, anonymous `rw-p` slabs with inode 0
(allocator arenas and thread machinery), and the kernel-provided `[vvar]` +
`[vdso]` pages that let chapter 4's `clock_gettime` skip the syscall
entirely. Near the top, `[stack]` grows downward toward the mmap region; the
`[vsyscall]` page at the very top is a frozen compatibility relic. The gaps
between regions are the point, not an accident: unmapped gigabytes that turn
stray pointers into segfaults instead of silent corruption, with layout
randomized per-exec (ASLR) — rerun the command and every address shifts.

## brk or mmap? Ask strace

Two syscalls create writable anonymous memory: `brk`, which moves the single
high-water mark of `[heap]`, and `mmap`, which drops an independent VMA
anywhere in the mmap region. Which one does a 64 MiB allocation use? The
brief answer for all three languages is *mmap* — but the trace is more
instructive than the answer:

```bash
[host]$ strace -f -e trace=mmap,brk,madvise ./cpp/build/release/app --mode heap
brk(NULL)                               = 0x2504b000
brk(0x2507e000)                         = 0x2507e000
mmap(NULL, 67112960, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f44e00ef000
```

(Loader mmaps trimmed.) glibc's malloc probes `brk(NULL)` for the current
break, grows it by 204 KiB (`0x2504b000` → `0x2507e000`) for its startup
arena — that is small allocations living in `[heap]` — and then satisfies the
64 MiB `std::vector` with a single `mmap` of 67,112,960 bytes: our 67,108,864
plus one 4 KiB page for the chunk header. The dividing line is malloc's
`M_MMAP_THRESHOLD`, 128 KiB by default (dynamically adjusted): below it,
carve from the arena; above it, a dedicated mapping that `free` can return to
the kernel instantly with `munmap` instead of hoping the arena top shrinks.
Rust tells the same story with the same syscalls — its default global
allocator *is* glibc malloc, and its trace shows the identical `brk` pair
(`0x55cf01269000` → `0x55cf0128a000`) followed by the same 67,112,960-byte
`mmap`. Go is the outlier: zero `brk` calls, ever. Its runtime reserves heap
arenas with `PROT_NONE` mappings and commits pages as needed — the
cross-check below shows that dance. This is why `memmap --mode heap` prints a
target line with *no* `[heap]` tag: the region backing a big allocation is an
anonymous mapping in the mmap region, in every language. The `[heap]` name in
maps is a fossil; the working heap is wherever the allocator put it.

## Page faults, minor and major

{% include excalidraw.html
   file="15-fault-flow"
   alt="A left-to-right flow: a user write to buf, then an MMU miss with a CPU trap, then the kernel page-fault handler looking up the VMA, fanning out to four outcomes: no VMA or bad permissions raising SIGSEGV, an anonymous first write getting a new zero-filled frame as a minor fault, a file page found in the page cache mapped with fault-around as a minor fault, and an uncached file page requiring a disk read as a major fault. Notes list the measured counts from this chapter's runs."
   caption="Figure 15.2 — one touched page's journey through the fault handler; the counts in the notes are this chapter's getrusage measurements" %}

The touch is where the promise gets paid. The MMU finds no page-table entry,
the CPU traps, and the kernel's fault handler asks: is this address inside a
VMA, with compatible permissions? No → `SIGSEGV`. Yes → resolve it. If the
page can be produced without I/O — a zero-filled frame for a first anonymous
write, or a file page already sitting in the page cache — that is a **minor
fault**: microseconds, bookkeeping only. If the kernel must read the disk,
the process sleeps through a **major fault**: the same event, three to six
orders of magnitude slower. `memmap` measures both with `getrusage(2)` deltas
around the touch loop. Writing 64 MiB of heap costs `minor=16385 major=0` —
one fault per touched 4 KiB page, plus one for the chunk-header page. The
read-side is cheaper than you would guess: `fault-walk` writes a 16 MiB temp
file, maps it read-only, and touches its 4,096 pages in eight steps of 512 —
and every step reports `minor=32`, not 512. That is **fault-around**: for
file pages already in the page cache, one fault maps a batch of 16
surrounding pages, so sequential reads of cached files fault 16× less often
than anonymous writes (which must give each page its own private zeroed
frame). And `major=0` throughout, because the walk file was written moments
earlier and still sits in the page cache — a major fault would mean the
machine actually went to disk.

## Three sizes and a policy: VSZ, RSS, PSS, and overcommit

Promise-then-pay means every process has at least three sizes. **VSZ** is
promises: the sum of all VMAs. **RSS** is payments: pages currently resident.
The gap is routinely enormous —

```bash
[host]$ ps -o pid,vsz,rss,comm $$
    PID    VSZ   RSS COMMAND
2620509 233356  4808 zsh
```

— a 227 MiB address space with 4.7 MiB resident. But RSS double-counts:
every process using `libc` counts its shared pages at full price. **PSS**
(proportional set size, from `/proc/<pid>/smaps_rollup`) divides each shared
page by the number of processes sharing it, so PSS sums to something
meaningful across a fleet; the cross-check below watches `memmap`'s PSS jump
from 403 kB to 65,948 kB as it dirties private pages no one else can share.

Unbacked promises need a policy, and it lives in `/proc/sys/vm` — read these,
do **not** write them:

```bash
[host]$ cat /proc/sys/vm/overcommit_memory /proc/sys/vm/overcommit_ratio
0
50
[host]$ grep -E 'CommitLimit|Committed_AS' /proc/meminfo
CommitLimit:    41161532 kB
Committed_AS:   48534240 kB
```

Mode `0` is the default heuristic: refuse only absurd single allocations,
otherwise assume most promises are never fully touched. The proof is in the
second read: this 64 GiB host has *already* promised 48.5 GiB against a
nominal `CommitLimit` of 41.2 GiB (swap + 50% of RAM, per
`overcommit_ratio`), and nothing is wrong — in mode 0 the limit is
advisory. Mode `2` enforces it (`malloc` starts returning `NULL` for real);
mode `1` promises anything. The price of overcommit is paid at touch time: if
a fault needs a frame and there is nothing left to reclaim, the **OOM
killer** picks the process with the highest badness score — roughly its
share of RAM, adjusted by `/proc/<pid>/oom_score_adj` (−1000 exempts, +1000
volunteers). This shell runs at `100` — `cat /proc/self/oom_score_adj` — a
nudge from systemd making user sessions die before system services. The
lesson for a systems programmer: on mode-0 Linux, allocation failure mostly
does not arrive as a clean error at the call site; it arrives as `SIGKILL`
at the worst touch. Chapter 22's cgroup work sharpens this with per-service
memory limits.

The last piece of the VMA toolkit is `madvise(2)` — hints on a range you
already own. `MADV_WILLNEED` starts readahead; `MADV_SEQUENTIAL` and
`MADV_RANDOM` tune it; `MADV_DONTNEED` drops pages *now* (anonymous memory
reads back as zeros — it is destructive, not advisory, despite the name);
`MADV_FREE` marks pages reclaimable-when-convenient, cheaper because the
kernel may never bother; `MADV_HUGEPAGE`/`MADV_NOHUGEPAGE` opt a range in or
out of transparent huge pages — which matters here because this host's THP
mode is `madvise` (`cat /sys/kernel/mm/transparent_hugepage/enabled` prints
`always [madvise] never`), meaning 2 MiB pages only where explicitly
requested. This is not exotic: the Go runtime in the cross-check below calls
`madvise(…, MADV_NOHUGEPAGE)` on its own metadata as a matter of course, and
chapter 17 uses `MADV_DONTNEED` semantics to explain what "freeing" memory
to an allocator actually returns to the kernel.

## How the code works

Three small structures carry `memmap`. A `Baseline` — `{rss_kb, minor,
major}` — is captured *before* each allocation because `/proc/self/status`'s
`VmRSS` and `getrusage`'s `ru_minflt`/`ru_majflt` are cumulative since
process start; only deltas isolate the window we care about. A `Mapping`
owns an mmap'd range exactly the way chapter 7's `Fd` owns a descriptor —
move-only in C++, `Drop` calls `munmap` in Rust, `defer unix.Munmap(m)` in
Go — so no error path leaks a VMA. A `TempFile` unlinks its path on
destruction, which is why `verify.lua` can assert the walk file is gone even
when a run fails halfway.

The probes are deliberately boring text-parsing: `vmrss_kb()` scans
`/proc/self/status` for the `VmRSS:` line; `fault_counts()` wraps
`getrusage(RUSAGE_SELF)`; and the maps excerpt re-reads `/proc/self/maps`,
hex-parses each line's range, and prints those overlapping the target — the
entire selection logic is one interval test, C++ spelling:

```cpp
        if (start < hi && end > lo) {
            std::println("memmap:   {}   <-- target (mode={})", line,
                         mode_name(mode));
        }
```

The touch loops write (or read) one byte per page, and each language must
actively defend that loop against its optimizer, because a loop whose stores
are never read is dead code: C++ uses an empty `asm volatile` barrier taking
the pointer, Rust `std::hint::black_box`, Go a package-level `sink` variable.
Delete the barrier and `--mode heap` still *allocates*, but release-build
touch loops vanish and the fault counts collapse — the tool would silently
measure nothing.

The mode worth reading three times is `stack`, because each language's
runtime forces a different shape:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Baseline is captured by the caller so that any page the compiler touches
// while setting up this 4 MiB frame still lands inside the measured window.
[[gnu::noinline]] std::expected<void, std::string>
stack_worker(std::size_t bytes, std::size_t page, const Baseline& base) {
    std::array<std::byte, kStackCapMb * kMiB> buf{};
    touch_writable(std::span{buf.data(), bytes}, page);
    return report(reinterpret_cast<std::uintptr_t>(buf.data()), bytes,
                  Mode::stack, base);
}

[[nodiscard]] std::expected<void, std::string>
run_stack(std::size_t bytes, std::size_t page) {
    auto base = take_baseline();
    if (!base) return std::unexpected{base.error()};
    return stack_worker(bytes, page, *base);
}
```

```go
// runStack touches a large local buffer on a dedicated goroutine so the
// growth happens on a fresh stack; the result comes back over a channel.
// The baseline is captured before the goroutine starts so the runtime work
// of growing (or relocating) that stack is part of the measured window.
func runStack(bytes, page int) error {
	base, err := takeBaseline()
	if err != nil {
		return err
	}
	errc := make(chan error, 1)
	go func() {
		errc <- stackWorker(bytes, page, base)
	}()
	return <-errc
}

//go:noinline
func stackWorker(n, page int, base baseline) error {
	var buf [stackCapMb * mib]byte
	touchWritable(buf[:n], page)
	return report(uintptr(unsafe.Pointer(&buf[0])), uintptr(n), "stack", base)
}
```

```rust
// Baseline is captured by the caller: the compiler's stack probing touches
// every page of this 4 MiB frame at function entry, so faults counted from
// inside the frame would miss the allocation itself.
#[inline(never)]
fn stack_worker(bytes: usize, page: usize, base: &Baseline) -> Result<()> {
    let mut buf = [0u8; STACK_CAP_MB * MIB];
    touch_writable(&mut buf[..bytes], page);
    report(buf.as_ptr() as usize, bytes, "stack", base)
}

fn run_stack(bytes: usize, page: usize) -> Result<()> {
    let base = take_baseline()?;
    stack_worker(bytes, page, &base)
}
```

The shared discipline: the worker is `noinline` so the 4 MiB frame really
exists as a frame, and the baseline is taken by the *caller*, because the
compiler (stack probing in C++/Rust, stack growth in Go) touches frame pages
at function entry — measure from inside and you miss the very faults you came
for. The divergence: C++ and Rust run the worker on the OS thread, so their
excerpt shows the `[stack]` VMA growing; Go runs it on a fresh goroutine
whose stack is runtime-managed heap spans, so its excerpt shows an anonymous
region — same contract, different mechanism, and the accurate place to see
that difference is the output itself, which is why the maps line is part of
the contract. The wiring is a strict flag parser
(exit 2 with a usage line on anything malformed), a per-mode header
computing `bytes`/`pages` (with `stack` clamped to 4 MiB to stay inside the
8 MiB rlimit, and `mmap-file` sized by `fstat` instead of `--mb`), and the
common `report()` tail. Fragile bits, stated plainly: the page size comes
from `sysconf` at runtime (4096 here; 16 KiB Apple-style kernels would change
every count); `fault-walk`'s step quota is `pages/8` with the remainder
folded into step 8, so `--mb` values whose page count is not divisible by 8
print an uneven last step; and the fault window unavoidably includes the
probe's own work — one page of malloc header here, a few hundred runtime
pages in Go — which is why the verifier asserts *floors*, not exact counts.

## Errors, three ways

`memmap` splits failures on one line: *usage* errors (unknown mode, bad
`--mb`, stray argument) print the usage string and exit 2 before anything is
allocated; *runtime* errors print `memmap: error: …` and exit 1. The
interesting decision is what the contract does **not** pin. Point each
implementation at a missing file:

```
memmap: error: /no/such/file: No such file or directory            (C++, exit 1)
memmap: error: open /no/such/file: no such file or directory       (Go, exit 1)
memmap: error: /no/such/file: No such file or directory (os error 2)  (Rust, exit 1)
```

Three renderings of the same `ENOENT`, because each rides its language's
native machinery — `std::error_code::message()` inside `std::expected`
plumbing, a `%w`-wrapped `*os.PathError`, `anyhow`'s `{:#}` chain over
`io::Error`. Unlike chapter 5's `fwatch`, this contract pins the prefix and
the exit code but leaves message text native, and `verify.lua` asserts
exactly that split — while errors the *program* generates, like mapping an
empty file (`mmap` of length 0 would fail anyway, so `open_for_map` checks
first), produce the identical `memmap: error: empty.bin: file is empty` in
all three, exit 1, byte for byte.

## Concurrency lens

All three `memmap`s are logically single-threaded, but the measurements say
otherwise, and the noise is the lesson. Rust reports `minor=16384` — exact,
one fault per page, because `vec![0; n]` allocates through `calloc`, which
knows fresh mmap pages are already zero and skips touching them; the touch
loop pays for every page itself. C++ reports `16385`: `std::vector`
zero-fills eagerly during construction, so the faults land in the `memset`
(plus the header page), and the later touch loop faults nothing. Go reports
`16598`: the same 16,384 plus a few hundred faults from runtime machinery —
GC metadata, spans, a second OS thread growing something — because
`getrusage(RUSAGE_SELF)` sums *every thread in the process*, and a Go
program is never really alone. The same reasoning explains why `perf stat`
whole-process counts (below) always exceed the in-window deltas, and why
`verify.lua` asserts `minor > 0` and RSS floors instead of equalities.
One more thread-shaped trap from the stack tabs: Go's worker goroutine could
be *rescheduled onto another OS thread* mid-measurement, which is fine for
fault counting precisely because `RUSAGE_SELF` is process-wide — the
per-thread `RUSAGE_THREAD` would be the wrong tool here.

## Build, run, observe

```bash
[host]$ cd examples/15-virtual-memory && ./demo.sh cpp run --mode heap
memmap: mode=heap bytes=67108864 pages=16384
memmap: maps excerpt
memmap:   7f7e52806000-7f7e56807000 rw-p 00000000 00:00 0    <-- target (mode=heap)
memmap: vmrss_before=3872KB vmrss_after=69412KB
memmap: faults minor=16385 major=0
```

Read it against the concepts: the target is an anonymous `rw-p` region (not
`[heap]`), spanning `0x4001000` bytes — 64 MiB plus the header page — and
the RSS delta is 65,540 kB, the *same* number, because every promised page
was touched. Go printed 2508 → 68728 (`minor=16598`) and Rust 2128 → 67668
(`minor=16384`) on the same host. Then contrast the modes: `--mode stack`
clamps to `bytes=4194304 pages=1024` and faults `minor=1024` into the
`[stack]` VMA for C++/Rust but into an anonymous region for Go;
`--mode mmap-anon --mb 32` faults `minor=8192` for 8,192 pages; and the
file-backed modes break the one-fault-per-page pattern:

```bash
[host]$ dd if=/dev/urandom of=sample.bin bs=1M count=2
[host]$ ./cpp/build/release/app --mode mmap-file sample.bin
memmap: mode=mmap-file bytes=2097152 pages=512
memmap: maps excerpt
memmap:   7fe01c400000-7fe01c600000 r--p 00000000 00:24 24538803   …/sample.bin   <-- target (mode=mmap-file)
memmap: vmrss_before=4052KB vmrss_after=6100KB
memmap: faults minor=32 major=0
```

512 pages read, 32 faults — fault-around's 16× batching, live. `--mode
fault-walk --mb 16` shows the same ratio held across all eight steps
(`step=1/8 pages=512 minor=32 major=0` … eight times), with a final RSS
delta of 16,384 kB: 4,096 resident pages bought with 256 faults. The full
matrix is one command: `python3 scripts/test-all-examples.py --only
15-virtual-memory` builds and behavior-checks all three (`PASS PASS PASS`,
58 `verify.lua` checks per language on this host).

## Cross-check, three ways

**The allocators' syscalls, across all three.** The C++ trace appeared
above; the other two confirm the split. Rust, under `strace -f -e
trace=mmap,brk,madvise`, shows glibc's fingerprint — `brk(NULL)`, a 132 KiB
break growth, then `mmap(NULL, 67112960, …)`. Go shows a different species
entirely:

```bash
[host]$ strace -f -e trace=mmap,brk,madvise ./go/bin/app --mode heap
mmap(0x2ed440000000, 67108864, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x2ed440000000
mmap(0x2ed443400000, 67108864, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) = 0x2ed443400000
madvise(0x7fee9f000000, 33554432, MADV_NOHUGEPAGE) = 0
```

(Trimmed to the shape; pid columns dropped.) Reserve arenas with
`PROT_NONE` — promises so weak even a *read* faults fatally — then commit
the needed span read-write with `MAP_FIXED`, and `madvise` runtime metadata
out of transparent huge pages. Zero `brk` calls in the entire trace.

**`smaps_rollup`, before and after, on a live process.** `memmap` reads
`/proc/self/status` (openat #6, pre-allocation) and `/proc/self/maps`
(openat #7, post-touch); injecting an 8-second delay into those opens holds
it still at exactly the right two moments:

```bash
[host]$ strace -o /dev/null -e trace=openat -e inject=openat:delay_enter=8000000:when=6+ \
        ./cpp/build/release/app --mode heap > /dev/null &
[host]$ APP=$(pgrep -n -x app)
[host]$ grep -E '^(Rss|Pss|Private_Dirty):' /proc/$APP/smaps_rollup    # pause 1
Rss:                3736 kB
Pss:                 403 kB
Private_Dirty:       224 kB
[host]$ grep -E '^(Rss|Pss|Private_Dirty):' /proc/$APP/smaps_rollup    # pause 2, 8s later
Rss:               69344 kB
Pss:               65948 kB
Private_Dirty:     65768 kB
```

Before: 3.7 MB resident but only 403 kB PSS — nearly everything is `libc`
and loader pages shared with the whole system, billed proportionally. After:
the deltas are private dirty pages nobody can share, so RSS and PSS converge.
`VmSize` in `/proc/$APP/status` moved 6860 → 72400 kB across the same pause
— a delta of 65,540 kB, agreeing to the kilobyte with the 67,112,960-byte
`mmap` strace showed and with the tool's own printed RSS growth.

**`perf stat`, against the tool's own numbers.** An independent counter over
the whole process lifetime:

```bash
[host]$ perf stat -e page-faults ./cpp/build/release/app --mode heap
memmap: faults minor=16385 major=0
            16,520      page-faults:u
       0.030148349 seconds time elapsed
```

Go: 16,879 against its in-window 16,606; Rust: 16,458 against 16,384. In
each pair perf sits a stable one-to-three hundred faults above the
`getrusage` delta — precisely the loader, runtime startup, and probe pages
that fall outside the measured window. Three observers (the kernel's rusage
accounting, `/proc` VMA accounting, the perf software event), one consistent
story.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet-wide view of this chapter is bcc-tools territory: `oomkill`
> logging every OOM-killer decision with the victim's badness score, and
> `bpftrace` histograms over the `page_fault_user` tracepoint replacing
> per-process `getrusage`. Those need root on a kernel you can afford to
> pressure — chapter 30, on the `systems-target` VM, does exactly that.

## What you learned

- An address is a promise: allocation creates VMAs (visible line by line in
  `/proc/self/maps`, gaps and all), and pages arrive only at first touch —
  which is why VSZ, RSS, and PSS are three different truths, and why
  `smaps_rollup`'s PSS is the number worth summing across processes.
- All three languages satisfy a 64 MiB allocation with `mmap`, not `brk`:
  glibc (C++ and Rust alike) via `M_MMAP_THRESHOLD`, Go via `PROT_NONE`
  arena reservations committed with `MAP_FIXED` — the `[heap]` VMA is where
  small glibc allocations live, nothing more.
- Minor faults are bookkeeping (one per anonymous page written: 16,384–16,385
  measured; 16 pages per fault for cached file reads, thanks to
  fault-around), major faults are disk reads, and `getrusage`, `perf stat`,
  and `smaps_rollup` triangulate the same counts from three directions.
- Overcommit mode 0 means `Committed_AS` may exceed `CommitLimit` (it does,
  on this host); the bill arrives at touch time via reclaim or the OOM
  killer's `oom_score_adj`-weighted choice, and `madvise` is your lever for
  telling the kernel what a range is really for.

Next, **mmap and shared mappings**: `MAP_SHARED`, `msync`, and two processes
seeing one page — the machinery `shmkv` v0 is built from.

---

<p><span class="status status--verified">verified</span> — every number and
excerpt above was produced on the Fedora 44 reference host (kernel
7.1.3-200.fc44, THP=madvise) this session: the runner printed
<code>15-virtual-memory  PASS  PASS  PASS</code> (3 passed, 0 failed, 0
skipped; 58 verify.lua checks per language); the heap runs reported
<code>minor=16385/16598/16384</code> (C++/Go/Rust) with the anonymous
<code>rw-p</code> target lines and RSS growth shown; the maps listing is a
real <code>cat /proc/self/maps</code>; the strace excerpts show glibc's
<code>brk</code>+67,112,960-byte <code>mmap</code> for C++ and Rust and Go's
<code>PROT_NONE</code>/<code>MAP_FIXED</code>/<code>MADV_NOHUGEPAGE</code>
sequence with zero brk; the delay-injection pause yielded the live
<code>smaps_rollup</code> readings (Pss 403 kB → 65,948 kB, VmSize delta
65,540 kB matching the mmap size); perf stat counted 16,520/16,879/16,458
page-faults; and the overcommit, THP, and <code>oom_score_adj</code> values
were read (never written) from <code>/proc</code> and <code>/sys</code>. The
"On the lab VM" bcc-tools callout is unverified as marked and is exercised
in Part 8.</p>
