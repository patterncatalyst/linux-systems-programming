---
title: "valgrind, Sanitizers, and Miri"
order: 29
part: "Debugging"
description: "bugfarm seeds five defects — leak, use-after-free, an uninitialised read, a heap overflow, and a data race — behind one `app <bug>` CLI in C++, Go, and Rust, and this chapter builds a real catch/miss matrix from actual runs: valgrind's memcheck/helgrind/massif against a plain binary (10-50x slower, and outright confused by Go's own runtime), the compile-time-instrumented ASan/UBSan/TSan/LSan against a rebuilt one, and Rust's miri catching the identical use-after-free class by symbolic interpretation of a small `unsafe` snippet instead of either."
duration: "60 minutes"
---

Chapter 28 read a crash after it happened. This chapter catches five bugs
*before* they get the chance to crash anything — or, in one case, watches a
tool fail to catch one at all. `bugfarm` is a single CLI,
`app <leak|uaf|uninit|overflow|race>`, seeding one specific memory or
concurrency defect per subcommand, built three times over. C++ has no
compile-time protection against any of the five, so it is the only language
that can express all of them; Go's garbage collector and runtime checks rule
three out by construction; Rust's borrow checker rules out four. What is
left, in every language, is watched by a different kind of tool — a runtime
binary translator, a compile-time instrumentation pass, or a symbolic
interpreter — and this chapter runs all of them, for real, against the same
five bugs, so the catch/miss matrix below is built entirely from commands
executed this session, not from what each tool's documentation claims it does.

The code is in `examples/29-valgrind-sanitizers-miri/`. `./demo.sh build`
builds the plain binaries; each language's `README.md`-documented CMake
presets (`asan`, `tsan`) and `go build -race` build the instrumented ones
directly, since instrumented builds are outside every chapter's normal
`demo.sh` contract.

{% include excalidraw.html
   file="29-tool-coverage-matrix"
   alt="A grid with defect down the rows -- leak, uninit, uaf, overflow, race -- and C++, Go, Rust across the columns. C++/leak: valgrind memcheck caught it, definitely lost. C++/uninit: valgrind memcheck caught it, uninitialised value(s); ASan alone missed it, needs MemorySanitizer. C++/uaf: valgrind caught it (invalid write, freed) and ASan caught it (heap-use-after-free). C++/overflow: valgrind caught it (0 bytes after a block) and ASan caught it (heap-buffer-overflow). C++/race: memcheck is not built for this and was not the tool tried; helgrind caught it (possible data race) and ThreadSanitizer caught it. Go/leak: self-detected via goroutines before and after; valgrind was confused, 630 errors and 0 real signal. Go/uninit, uaf, overflow: prevented by the runtime, exit 64, no tool needed. Go/race: go -race caught it, WARNING DATA RACE, exit 66. Rust/leak: valgrind caught it, 65536 bytes lost, and miri on nightly additionally reported memory leaked. Rust/uninit, uaf, overflow, race: prevented at compile time by the borrow checker, exit 64; a note says unsafe Rust plus miri catches the use-after-free class separately, shown elsewhere in the chapter. A footer legend says amber cells mean a tool run this session caught the defect, dark cells mean the language prevents it outright, and plain cells mean a real blind spot this session measured; every cell traces to a command in Build run observe or Cross-check."
   caption="Figure 29.1 — the five bugfarm defects against three languages, every cell backed by a command this chapter actually ran" %}

> **Tools used** — `valgrind` 3.27.1 (`--tool=memcheck`, `--tool=helgrind`,
> `--tool=massif`), `cmake`/`g++` 16.1.1 with the `asan`/`tsan` CMake presets
> (`-fsanitize=address,undefined` / `-fsanitize=thread`), `go build -race`,
> `cargo +nightly miri` (all host). `valgrind` and the sanitizer runtimes
> (`libasan`, `libtsan`) are hard requirements in `scripts/check-host.sh`;
> `miri` is not — it tracks Rust nightly, and this chapter documents exactly
> what happens whether or not a nightly-plus-miri toolchain is installed.

## Two ways to watch a program: translate the binary, or instrument the source

**valgrind** needs no rebuild and no source cooperation at all: point it at
any binary, plain `RelWithDebInfo` included, and its core (a virtual CPU
built on an IR called VEX) re-translates every basic block before it runs,
inserting extra instructions around every memory access. `memcheck`, the
default tool, gives every byte of memory a shadow bit tracking
*addressability* (has this byte been allocated?) and a shadow byte tracking
*definedness* (has this byte been written to?), checked in software on every
load and store — no hardware assist, because ordinary CPUs have no such
instruction. That software check, repeated for every single memory access of
the whole program, is the entire reason for the well-known 10-50x slowdown:
this chapter's own `uaf` subcommand took **0.002s** plain, **0.040s** under
ASan, and **0.97-0.99s** under valgrind memcheck — valgrind ran roughly
**25x slower than the sanitizer build** for the identical bug, on a binary so
small that most of that second is JIT-translation and process startup, not
steady-state emulation. `helgrind` swaps memcheck's shadow bytes for a
lockset-plus-happens-before model over the same binary-translation core (the
`race` subcommand measured 1.22s under it, slightly *heavier* than
memcheck's 1.02s on the same input); `massif` swaps in a heap-usage
sampler, taking periodic snapshots of live allocation size instead of
checking every access — running it against `leak` produced a real,
plottable curve peaking at **80.02 KB** before the process exits with the
leaked block still counted.

Because valgrind operates on the compiled binary with no notion of what
produced it, it also has no notion of runtimes that manage their own memory
outside libc — and Go is exactly that. Pointing valgrind at the Go `leak`
binary produces **630 errors from 411 contexts**, every single one a false
"Conditional jump or move depends on uninitialised value(s)" or "Use of
uninitialised value" deep inside `runtime.lock2`, `runtime.gopark`, and
`runtime.mcall` — the goroutine scheduler's own stack-switching and lock-free
bookkeeping look like impossible, undefined states to an instruction-level
model built around one C-style stack per OS thread. Worse, the `HEAP
SUMMARY` reports **0 allocs, 0 frees, 0 bytes allocated** for the entire run,
even though the program printed a real `leaked=1` on stdout — Go's allocator
talks to the kernel directly via `mmap`, bypassing the `malloc`/`free` calls
valgrind intercepts, so its leak-tracking machinery has nothing to attach to
at all. Neither the 630 false positives nor the invisible real leak is a
configuration mistake; valgrind and the Go runtime are built on
incompatible assumptions about what a stack and a heap look like, and this
chapter's own transcript is the proof.

**Sanitizers** take the opposite trade: instrument the *source*, at compile
time, and ship a differently-linked binary (`libasan.so`, `libtsan.so`)
whose every load and store already carries its own inline check — no
separate CPU, no re-translation, just a few extra machine instructions the
compiler emitted. ASan's mechanism is **shadow memory**: one shadow byte
covers eight bytes of application memory (`shadow_addr = (app_addr >> 3) +
offset`), and specific byte values mean specific things — `00` fully
addressable, `fa` a heap redzone the allocator placed around every block to
catch overflow, `fd` a freed block's bytes repainted so any further access
trips instantly.

{% include excalidraw.html
   file="29-shadow-memory"
   alt="Two stacked bands. The upper application-memory band shows three boxes: a left redzone at addresses ending ff80 through ffff, a 4-byte int* p at address ending 0010 marked as new'd then delete'd with the note that writing through it is undefined behavior, and a right redzone at addresses ending 0018 through 007f. The lower shadow-memory band shows the corresponding three shadow bytes: fa labeled heap left redzone, fd labeled freed heap region and marked as the exact byte the write trips, and fa labeled heap left redzone again for the next block's guard. An arrow labeled shift right by 3 then add offset connects the freed block down to its fd shadow byte. A footer formula reads shadow address equals app address shifted right 3 plus offset, one comparison before every load or store, not a full CPU emulation, and a note gives the real legend from this session's ASan run -- 00 addressable, 01 through 07 partially addressable, fa heap redzone, fd freed heap region, f1 through f8 stack states, f9 global redzone -- plus the measured gap: 0.002s native, 0.046s under ThreadSanitizer, 1.0s under valgrind memcheck, for the same tiny program."
   caption="Figure 29.2 — ASan's shadow-memory model, with the real freed-block shadow bytes this chapter's `uaf` run produced" %}

Running the `uaf` binary under ASan pulls exactly the `fd` byte at
`0x7b29be3e0010` and reports `heap-use-after-free` with the freed-at and
allocated-at stacks attached; running `overflow` reports
`heap-buffer-overflow` the instant the write lands one byte into the `fa`
redzone past a 40-byte block. **LSan** is bundled into the ASan runtime by
default on Linux and runs its own reachability walk at process exit — the
same `leak` binary rebuilt under the `asan` preset reports `detected memory
leaks` with no separate leak-checking flag needed. **UBSan** rides along in
the same build (`-fsanitize=address,undefined`) but none of this chapter's
five bugs happen to trip one of its specific checks (signed overflow,
invalid enum values, and the like are its domain, not heap safety), so it
stays silent here — worth naming so its absence in these transcripts doesn't
read as a miss. **TSan** needs its own separate build (ASan and TSan cannot
be linked into the same binary) and uses yet another shadow model — a vector
clock per thread recording happens-before relationships — to catch a race
*while it actually executes*, meaning it can only report interleavings the
run actually hits. The overhead lands close to native either way: `race`
measured **0.03-0.046s** under TSan versus **0.002s** plain, roughly the
constant-factor cost of a few inlined instructions per access, not
valgrind's order-of-magnitude software CPU.

**Go's `-race`** is the same idea under a different name: `go build -race`
links the ThreadSanitizer runtime directly into the Go binary, which is why
its `WARNING: DATA RACE` output — two conflicting accesses, each with a full
goroutine stack, plus the creation site of both goroutines — looks so much
like TSan's C++ report. It is TSan, wearing Go's tooling.

**miri** is neither of the above: it never runs your compiled machine code
at all. It interprets the program's MIR (Rust's mid-level IR) inside an
abstract machine that tracks every allocation's provenance and validity, so
it can flag undefined behavior the borrow checker never got a chance to rule
out — specifically, UB reachable only through `unsafe`. This host's pinned
toolchain (`rust-toolchain.toml`, `1.97.1`) has no miri component (miri
tracks nightly, not stable), but a `nightly-x86_64-unknown-linux-gnu`
toolchain with miri **is** installed here, so this chapter ran it for real
rather than only documenting it. Against the actual `bugfarm` `leak`
binary, `cargo +nightly miri run -- leak` reports `error: memory leaked:
alloc1026 (Rust heap, size: 24, align: 8)` naming the exact `Box::leak` call
site — the same finding valgrind reached by a completely different route.
To see miri catch a defect *only* `unsafe` can express, a five-line
standalone snippet frees a `Box` through a raw pointer and then writes
through it anyway:

```rust
let raw: *mut i32 = unsafe { Box::into_raw(Box::new(42i32)) };
unsafe {
    drop(Box::from_raw(raw)); // frees the allocation
    *raw = 7;                 // UB: write through a dangling pointer
}
```

`cargo +nightly run` on this prints `read back: 7` and exits 0 — the
write "worked," which is exactly the danger UB represents. `cargo +nightly
miri run` on the identical source refuses:

```console
error: Undefined Behavior: memory access failed: alloc236 has been freed, so this pointer is dangling
  --> src/main.rs:12:9
help: alloc236 was allocated here: src/main.rs:7:21
help: alloc236 was deallocated here: src/main.rs:11:9
```

That is the same use-after-free class ASan catches in C++ (right down to
naming the allocation and deallocation sites), reached through Rust's
`unsafe` escape hatch instead of a language with no such checks at all —
which is exactly the point: a type system cannot forbid what it never sees,
and `unsafe` is where Rust admits that plainly.

## How the code works

Each language's `main.cpp` / `main.go` / `main.rs` shares one shape: a
`Bug` enum with exactly five variants (`enum class Bug` in C++, a plain
string switch in Go, `enum Bug` in Rust), a `parse_bug` function returning
that enum or an error through each language's Chapter 5 error-handling
idiom (`std::expected`, `(string, error)`, `Result<Bug, String>`), and one
`do_*` function per variant holding the seeded defect. The enum, not a raw
string comparison scattered through `main`, is what makes "five bugs, three
languages, one dispatch shape" a closed, exhaustive match instead of an
open-ended set of string checks — C++'s `switch` over `Bug` even gets
`std::unreachable()` after it, telling the compiler (and a reader) that
every variant is handled.

The C++ `do_leak`, `do_uaf`, `do_uninit`, and `do_overflow` functions each
route their pointer through a `touch()` helper —
`asm volatile("" : : "r,m"(value) : "memory")`, an empty inline-asm block
that tells the compiler "something outside this translation unit might read
this value," the same trick benchmark libraries call `DoNotOptimize`. It
matters more than it looks: rebuilding `do_leak` with `touch(buf)` removed
but its per-element `buf[i] = i` loop left in place *still* reports the
leak, because GCC's escape analysis treats a write into heap memory as an
observable side effect it won't remove without proof nothing depends on it.
Strip the loop too — an allocation nothing ever writes to or reads — and
`HEAP SUMMARY` drops straight to **0 allocs, 0 frees, 0 bytes allocated**:
GCC proved the entire `new[]` unobservable and deleted it, malloc and all,
confirmed directly on this host. `touch()` is what closes that gap; without
it (or without a loop that keeps writing), there is no bug left to catch,
which is itself the lesson — an unobserved bug is dead code, and the
compiler is entitled to remove dead code. Rust's `leak()` uses
`std::hint::black_box(leaked.as_ptr())` for the identical reason: LLVM can
see nothing observable depends on the buffer's *contents* (only its
statically-known `.len()` gets printed), so without the barrier the whole
64 KiB `vec![0xAAu8; 65536]` — and its `Box::leak` — can vanish too.

The bugs themselves are small and literal. `do_uaf` calls `delete p`
then writes `*p = 7` through the now-dangling pointer; `do_uninit`
allocates `new int` with no initializer and reads it; `do_overflow`
writes `arr[10]` into a 10-element array; `do_race` spawns two
`std::jthread`s that each increment a shared, unguarded `int counter` in a
tight loop, then joins both (via `jthread`'s own RAII) before printing the
result. Go's `leak()` spawns a goroutine blocked forever on an unbuffered
channel receive nobody will ever send to, then compares
`runtime.NumGoroutine()` before and after — there is no external tool
watching the Go heap for this the way valgrind watches `malloc`, so the
program has to notice its own leak. Rust's `leak()` is the one bug safe
Rust can still express: `Box::leak` is a real, safe standard-library API
whose entire purpose is opting an allocation out of `Drop`-based
reclamation, which is why it is the only row in the matrix where Rust has
anything for valgrind or miri to find at all.

## Errors, three ways

Bad usage is identical across all three: no subcommand, or an unrecognized
one, prints `usage: app <leak|uaf|uninit|overflow|race>` alone (for no
argument) or `app: unknown bug: <name>` above that same usage line (for an
unrecognized one), and exits **2** — the same shape as every other
chapter's `app`. The interesting three-way split is what each language does
with a bug it *cannot* express: Go prints `go: prevented by the runtime —
see chapter` for `uaf`/`uninit`/`overflow`, Rust prints
`rust: prevented at compile time — see chapter` for
`uaf`/`uninit`/`overflow`/`race`, and both exit **64** (`EX_USAGE`) rather
than attempting anything. That fixed message and exit code *is* the
finding for those cells — not a placeholder, but the same "no reproducible
bug here" result a real investigation would reach chasing the equivalent
C++ defect in that language, arrived at by a runtime check or a rejected
compilation instead of a crash.

## Concurrency lens: TSan vs. helgrind vs. `-race`

The race is one unsynchronized `int counter` incremented from two threads
with no lock — the canonical lost-update bug — and this chapter ran all
three of that bug's detectors against it directly. **ThreadSanitizer**
(the C++ `tsan` preset) named the exact colliding line,
`main.cpp:67` (`++counter;`), printing both threads' stacks and a summary
that exits **66**; a plain, uninstrumented three-run sample of the same
binary showed the race is not guaranteed to *manifest* every time —
`counter=400000` (the mathematically correct total for the workload) came
back in one plain sample, while the TSan-instrumented run's altered timing
produced `counter=200000`, half the expected total, alongside its report.
**helgrind**, valgrind's other thread tool, found the identical line
through a completely different mechanism — a lockset-and-happens-before
model instead of TSan's vector clocks — reporting "Possible data race...
at (anonymous namespace)::bump() (main.cpp:67)" between the same two
threads, both created with "Locks held: none." **`go -race`** found Go's
version of the same shape at `main.go:70`, printing `WARNING: DATA RACE`
for the conflicting pair and `Found 2 data race(s)` before exiting **66**;
a plain Go run in this same session came back `counter=100302` instead of
the expected `200000` — a visibly corrupted count, unlike the C++ plain
run's lucky correct total, which is itself a reminder that "it printed the
right number" is not evidence a race is absent. Rust needs none of this:
the race subcommand exits 64 with `prevented at compile time`, because
sharing `&mut i32` across `std::thread::spawn` boundaries without
synchronization is rejected by the `Send`/`Sync` rules before there is a
binary to run a detector against at all.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
int counter = 0; // race: shared, unsynchronized on purpose

void bump() {
    for (int i = 0; i < 200'000; ++i) {
        ++counter;
    }
}
```

```go
var counter int // race: shared, unsynchronized on purpose

func bump(wg *sync.WaitGroup) {
	defer wg.Done()
	for i := 0; i < 100_000; i++ {
		counter++
	}
}
```

```rust
    match bug {
        Bug::Leak => {
            leak();
            Ok(())
        }
        Bug::Uaf | Bug::Uninit | Bug::Overflow | Bug::Race => {
            eprintln!("{PREVENTED}");
            Err(64)
        }
    }
```

The C++ and Go excerpts are the bug itself, written by hand in both
languages; the Rust excerpt is the same `Bug::Race` variant reaching the
`match` arm that refuses to try — there is no racy Rust function to show,
because the compiler never let one exist.

## Build, run, observe

```console
[host]$ cd examples/29-valgrind-sanitizers-miri && ./demo.sh build
```

```console
[host]$ valgrind --leak-check=full --error-exitcode=1 ./cpp/build/release/app leak
bugfarm: leak: allocated 4096 bytes on the heap, never freed (intentional)
==...== 4,096 bytes in 1 blocks are definitely lost in loss record 1 of 1
```

Build the instrumented variants directly — they sit outside `demo.sh`'s
build/run contract on purpose, since this chapter (unlike most) is about
the tool, not a happy-path run:

```console
[host]$ cd cpp && cmake --preset asan && cmake --build --preset asan
[host]$ cmake --preset tsan && cmake --build --preset tsan
[host]$ ./build/asan/app uaf
==...==ERROR: AddressSanitizer: heap-use-after-free on address ...
[host]$ ./build/tsan/app race
WARNING: ThreadSanitizer: data race (pid=...)
[host]$ go build -race -o bin/app-race . && ./bin/app-race race
WARNING: DATA RACE
```

`python3 scripts/test-all-examples.py --only 29-valgrind-sanitizers-miri`
reports `PASS PASS PASS`: the plain build and bad-usage/prevented-message
contract per language, then the C++ suite (memcheck on `leak`/`uninit`,
ASan on `uaf`/`overflow`, TSan on `race`), Go's self-detected `leak` plus
`-race`'s `race`, and Rust's valgrind-on-`leak` plus its informational
miri probe.

## Cross-check: the same use-after-free, two tools, two speeds

The clearest single proof that these are two different techniques, not two
brands of the same one, is running the identical bug under both: the C++
`uaf` binary, unmodified, first under valgrind, then rebuilt under ASan.

```console
[host]$ valgrind --error-exitcode=1 ./cpp/build/release/app uaf
==...== Invalid write of size 4
==...==    at 0x400A3B: do_uaf (main.cpp:87)
==...==  Address 0x4e7f080 is 0 bytes inside a block of size 4 free'd
==...== ERROR SUMMARY: 1 errors from 1 contexts (suppressed: 0 from 0)
```

```console
[host]$ ./cpp/build/asan/app uaf
==...==ERROR: AddressSanitizer: heap-use-after-free on address 0x7b29be3e0010
WRITE of size 4 at 0x7b29be3e0010 thread T0
    #0 do_uaf .../main.cpp:87
freed by thread T0 here: ... #1 do_uaf .../main.cpp:85
previously allocated by thread T0 here: ... #1 do_uaf .../main.cpp:84
```

Both name line 87 as the bad write and lines 84/85 as the allocation and
free — the same defect, independently confirmed. What differs is *how much
work* got them there: valgrind's memcheck took **0.97-0.99s** (repeated
twice for stability) against the plain binary with no rebuild required;
the ASan build, needing `cmake --preset asan` first, then ran in
**0.040s** — about **25x faster** — and its report additionally carries a
full shadow-byte dump (`fa fa fa fa[fd]fa fa ...`) that pinpoints exactly
which byte tripped the check, something memcheck's report describes in
words but ASan shows you directly. Neither number should be read as "the"
overhead for real programs — this binary does almost nothing, so both
times are dominated by fixed startup cost — but the *ratio*, valgrind an
order of magnitude slower than a rebuild-and-relink sanitizer for
identical output, is exactly the trade-off the tool-selection question in
this chapter's opening comes down to: rebuild if you can, translate the
binary if you cannot.

## What you learned

- valgrind instruments the *binary* at runtime (no rebuild, works on
  anything) at the cost of a software-emulated CPU — measured at roughly
  **25x** the ASan build's time for an identical `uaf` report — and that
  same binary-translation model has no concept of a runtime that manages
  its own stacks and heap outside libc, which is why pointing it at a Go
  binary produced **630 false-positive errors** and a heap summary blind
  to a real, on-stdout goroutine leak.
- Sanitizers instrument the *source* at compile time via shadow memory (one
  byte per eight application bytes, `>>3 + offset`), trading a required
  rebuild for overhead close to native (TSan's race run at **0.03-0.046s**
  versus plain's **0.002s**) — and LSan/UBSan ride along in the same ASan
  build rather than needing separate invocations.
- miri interprets MIR symbolically instead of running compiled machine
  code at all, which lets it catch undefined behavior reachable only
  through `unsafe` — demonstrated live on this host's nightly toolchain
  against both the real `Box::leak` bugfarm binary and a standalone
  use-after-free snippet, naming the exact allocation and deallocation
  sites the same way ASan does for C++.
- The same race bug, checked three ways (TSan, helgrind, `go -race`), gets
  three different detection mechanisms (vector clocks, lockset/happens-before,
  and TSan borrowed wholesale) converging on the identical source line —
  while Rust's `Send`/`Sync` rules mean there is no racy binary for any of
  them to point at in the first place.

Next, **the eBPF observation toolkit**: bcc-tools and bpftrace watch these
same kinds of programs from outside entirely, with no instrumentation and
no rebuild, on the lab VM this chapter's tools never needed.

---

<p><span class="status status--verified">verified</span> — every transcript,
number, and diagnostic above was produced on the Fedora 44 reference host
(kernel 7.1.3-200.fc44, valgrind-3.27.1, gcc 16.1.1, go1.26.5, rustc/cargo
1.97.1, miri 0.1.0 nightly-2026-07-18) this session. Confirmed live:
valgrind memcheck reported <code>definitely lost</code> for <code>leak</code>
and <code>uninitialised value(s)</code> for <code>uninit</code>, and also
caught <code>uaf</code> (<code>Invalid write ... freed</code>) and
<code>overflow</code> (<code>Invalid write ... 0 bytes after a block</code>)
on the plain binary; the ASan build reported <code>heap-use-after-free</code>
and <code>heap-buffer-overflow</code> with real shadow-byte dumps, and its
bundled LSan reported <code>detected memory leaks</code> for <code>leak</code>;
the TSan build reported <code>data race</code> at <code>main.cpp:67</code>
(exit 66) and helgrind independently reported <code>Possible data race</code>
at the same line; <code>go build -race</code> reported
<code>WARNING: DATA RACE</code> at <code>main.go:70</code>,
<code>Found 2 data race(s)</code> (exit 66), and a plain Go run showed a
corrupted <code>counter=100302</code> against the expected 200000; valgrind
against the Go <code>leak</code> binary produced <code>630 errors from 411
contexts</code> (all inside the Go runtime) and a heap summary of
<code>0 allocs, 0 frees, 0 bytes allocated</code>; valgrind against the Rust
<code>leak</code> binary reported <code>65,536 bytes ... definitely lost</code>,
and <code>cargo +nightly miri run -- leak</code> independently reported
<code>memory leaked: alloc1026</code>; a standalone unsafe snippet ran clean
under <code>cargo +nightly run</code> (<code>read back: 7</code>) and was
rejected by <code>cargo +nightly miri run</code> with
<code>Undefined Behavior: memory access failed: alloc236 has been freed</code>;
<code>massif</code> against <code>leak</code> produced a real peak of
<code>80.02 KB</code>; timing the same <code>uaf</code> binary gave
<code>0.002s</code> plain, <code>0.040s</code> under ASan, and
<code>0.97-0.99s</code> under valgrind memcheck (repeated twice); and
removing every observable use of <code>do_leak</code>'s buffer (not just its
<code>touch()</code> barrier, but its per-element writes too) reproducibly
collapsed valgrind's report to <code>0 allocs, 0 frees, 0 bytes allocated</code>
on this host's GCC 16.1.1 at <code>-O2</code>.</p>
