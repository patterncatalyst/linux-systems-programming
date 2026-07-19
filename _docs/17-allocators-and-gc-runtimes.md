---
title: "Allocators and GC runtimes"
order: 17
part: "Memory"
description: "What actually happens between your allocation and the kernel's pages: glibc malloc's arenas and tcache, what pmr and bumpalo really change, sync.Pool as Go's arena substitute, and the Go GC's pacer driven live with GOGC and GOMEMLIMIT — cross-checked with massif, /usr/bin/time, ltrace, and pprof."
duration: 50 minutes
---

The last two chapters dealt in wholesale memory: pages from the kernel, mapped
and shared with `mmap`. This chapter is retail. Between your `std::string`,
your `strconv.Itoa`, your `format!` and those pages sits an allocator — glibc
malloc for C++ *and* (this surprises people) for Rust, and a garbage-collected
runtime heap for Go — and the one new idea here is that the axis that
separates them is not allocation but **reclamation**: free every object
individually, free a whole batch at once, or free nothing and let a collector
trace what survived. `allocbench` runs the same allocation-heavy workload
through all three strategies in all three languages and prints the two numbers
that let you compare them: the process's peak RSS and the wall time — plus,
for Go only, how many GC cycles the churn cost.

The code is in `examples/17-allocators-and-gc-runtimes/`. `./demo.sh` builds
all three implementations and runs the workload; the `README.md` there
specifies the CLI, the one-line report format, and the exact behavior contract
the three binaries share.

{% include excalidraw.html
   file="17-arena-vs-gc"
   alt="Three columns compare reclamation strategies for the same 200,000 short-lived strings. Left, free one by one: objects flow into glibc's main arena and tcache with 64 cached chunks per size class, pages stay with the process (mallinfo2 reports arena 6340KB after everything is freed), peak RSS 3908KB for C++ and 2548KB for Rust with one free per object. Middle, arena and pool: a reusable 256KiB slab with a bump pointer, survivors copied out, the whole batch reclaimed in O(1) by reset, peak RSS 4240KB C++ and 2544KB Rust with zero per-object frees. Right, tracing GC: the churn becomes garbage the collector must find, the pacer triggers on GOGC growth or the GOMEMLIMIT soft limit, concurrent mark and sweep runs 9 cycles by default for 8064KB peak, or 47 cycles at 5676KB with GOMEMLIMIT=8MiB."
   caption="Figure 17.1 — three ways to take memory back: per-object free through glibc malloc, wholesale arena release, and traced collection under a pacer; every number is from an allocbench run on the reference host" %}

> **Tools used** — `valgrind --tool=massif` + `ms_print`, `/usr/bin/time`,
> `ltrace`, `nm`, `gcc`, `go tool pprof`, `go doc` (host); `memleak` from
> bcc-tools (lab VM, exercised in Part 8). All host tools are checked by
> `scripts/check-host.sh` or ship with the pinned toolchains.

## What malloc does with your free

`free(3)` almost never gives memory back to the kernel. glibc's allocator
carves the pages it got from `brk`/`mmap` into an **arena** — a heap region
plus the bookkeeping to subdivide it — and a freed chunk goes onto a list to
be handed out again: first into the calling thread's **tcache** (a tiny
per-thread stash, by default up to 64 chunks in each of 64 size classes,
touchable without any lock), then into the arena's small/large bins. The pages
themselves stay with the process, which is exactly what RSS measures and why
"my program freed everything but RSS didn't drop" is normal, not a leak.

You can watch this bookkeeping directly with `mallinfo2(3)` — a ten-line probe
(allocate 100,000 48-byte chunks, free every other one, then the rest) prints,
on this host:

```console
[host]$ gcc -O2 -o minfo minfo.c && ./minfo
start:       arena=0KB in-use=0KB free-on-list=0KB
allocated:   arena=6340KB in-use=6254KB free-on-list=85KB
holey:       arena=6340KB in-use=3130KB free-on-list=3209KB
all freed:   arena=6340KB in-use=5KB free-on-list=6334KB
```

The `holey` line is **fragmentation** in one row: half the bytes are free, but
they are interleaved hole-by-hole with live chunks, so the arena cannot shrink
— 6340KB of address space held to service 3130KB of live data. This recycling
is also why the C++ default variant below stays so flat: 200,000 short-lived
strings churn through the *same* few chunks, tcache-fast, and peak RSS barely
notices. (An aside: `mallinfo2` is glibc-specific and its counters are
process-wide approximations — use it for intuition, `getrusage` for numbers
you report.)

## Arenas: pay once, free once

The arena idea removes per-object reclamation entirely: allocate by bumping a
pointer through a slab, never free individuals, release the whole slab in one
move when the batch of work is done. C++23 spells it
`std::pmr::monotonic_buffer_resource` — a memory resource whose `deallocate`
is deliberately a **no-op**; everything comes back when the resource is
destroyed. What pmr actually changes is therefore two things and only two
things: **allocation locality** (the batch's strings sit contiguously in one
slab instead of scattered across malloc chunks) and **free-at-once** (batch
teardown is one release, not a thousand `free` calls). It does not magically
lower your high-water mark — the slab is a fixed cost you always touch, and
the numbers below show it.

Rust's story starts with a fact worth proving rather than asserting: the
default `GlobalAlloc` on Linux **is malloc**. The release binary imports it
from glibc —

```console
[host]$ nm -D rust/target/release/app | grep -wE 'malloc|free|realloc|posix_memalign'
                 U free@GLIBC_2.2.5
                 U malloc@GLIBC_2.2.5
                 U posix_memalign@GLIBC_2.2.5
                 U realloc@GLIBC_2.2.5
```

— and `ltrace -x malloc -c` counts 415 live `malloc` calls for a 100-iteration
default run against 317 for the arena run (at tiny N the two copies into the
long-lived index dominate; the churn savings grow with N). For the arena
variant the example uses `bumpalo 3.20.3`: a `Bump` is the same bump-pointer
design, `bump.reset()` is the O(1) wholesale release, and the borrow checker
statically guarantees nothing bump-allocated survives the reset. Go gets
neither pmr nor bumpalo — the language's `arena` experiment stalled — so the
idiomatic third spelling is `sync.Pool`: churn objects are *recycled* rather
than batch-freed, which attacks the same cost (garbage pressure) from the
other side.

## The GC's bargain, with the knobs live

Go's collector trades your CPU for your RSS on a schedule called the pacer,
and its two knobs are environment variables, so you can renegotiate the
bargain without recompiling. `go doc runtime` on this host's go1.26.5: *"The
GOGC variable sets the initial garbage collection target percentage. A
collection is triggered when the ratio of freshly allocated data to live data
remaining after the previous collection reaches this percentage."* And
GOMEMLIMIT *"sets a soft memory limit for the runtime"* that includes the heap
and everything else the runtime manages. Here is the same binary, four
policies, run back to back on this host:

```console
[host]$ ./demo.sh go run
allocbench: variant=default allocs=200000 peak_rss=8064KB ms=47 gc_cycles=9
[host]$ GOGC=25 ./demo.sh go run
allocbench: variant=default allocs=200000 peak_rss=5516KB ms=63 gc_cycles=32
[host]$ GOGC=off GOMEMLIMIT=8MiB ./demo.sh go run
allocbench: variant=default allocs=200000 peak_rss=5676KB ms=60 gc_cycles=47
[host]$ GOGC=off GOMEMLIMIT=64MiB ./demo.sh go run
allocbench: variant=default allocs=200000 peak_rss=37164KB ms=50 gc_cycles=0
```

Read it as a supply curve: 9 cycles at 8064KB by default; four times the
cycles (32) buys the peak down to 5516KB at a wall-time cost; with pacing off,
an 8MiB soft limit alone drives 47 cycles to stay under it; and a 64MiB limit
the workload never approaches means the collector *never fires* — zero cycles,
and the garbage piles up to 37164KB of RSS. That last line is the sharpest
lesson: a GC's memory use is a policy outcome, not a property of your code.

One note on *which* collector: go1.26.5's Green Tea GC. On this host the
toolchain accepts `GOEXPERIMENT=greenteagc` and `nogreenteagc` (and rejects
made-up names), and the version stamps tell you the default: a
`nogreenteagc` build is marked `go1.26.5-X:nogreenteagc` — a deviation —
while a `greenteagc` build stamps plain `go1.26.5`, meaning Green Tea *is*
this toolchain's default state. `go doc runtime` here documents GOGC and
GOMEMLIMIT but nothing Green-Tea-specific, so that observable stamp is as far
as this chapter's claims go.

## How the code works

Two structures anchor all six builds. The long-lived survivor is a plain hash
map — `std::unordered_map<std::string, std::string>`, `map[string]string`,
`HashMap<String, String>` — deliberately ordinary, because the chapter is
about the churn, not the index. The churn carrier is what varies: C++ holds
each batch's intermediates in a `std::pmr::vector<std::pmr::string>` wired to
a `monotonic_buffer_resource` over one reusable 256KiB slab (a
`std::vector<std::byte>` allocated once, outside the loop); Rust holds
`bumpalo::collections::String`s in a `BumpVec` inside a `Bump`; Go pools a
`scratch` struct of three `bytes.Buffer`s plus a `[]byte` for
`strconv.AppendInt`. The scratch containers exist so the *default* variant
also keeps a batch's fragments alive to batch end — both variants hold the
same live set, so their peaks are comparable and only reclamation differs.

Here is the arena-side builder in each language, verbatim from
`examples/17-allocators-and-gc-runtimes/{cpp/src/main.cpp,go/main.go,rust/src/main.rs}`:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// arena variant: same shape, but the intermediates come from a monotonic
// buffer over one reusable slab and are released wholesale per batch.
[[nodiscard]] Index build_arena(long allocs) {
    std::vector<std::byte> slab(kSlabBytes);  // reused by every batch
    Index index;
    index.reserve(kKeyspace);
    for (long start = 0; start < allocs; start += kBatch) {
        const long end = std::min(start + kBatch, allocs);
        std::pmr::monotonic_buffer_resource arena{slab.data(), slab.size()};
        std::pmr::vector<std::pmr::string> scratch{&arena};
        scratch.reserve(static_cast<std::size_t>(end - start));
        for (long i = start; i < end; ++i) {
            std::pmr::string key{&arena};
            std::pmr::string frag{&arena};
            std::pmr::string value{&arena};
            make_kv(i, key, frag, value);
            index.insert_or_assign(std::string{key}, std::string{value});
            scratch.push_back(std::move(frag));
        }
    }  // ~arena: the whole batch vanishes at once; no per-object frees
    return index;
}
```

```go
// buildArena: same logical work, but the intermediates live in pooled
// buffers that are recycled instead of collected. Only the final key/value
// strings are copied out into the long-lived index.
func buildArena(allocs int) map[string]string {
	index := make(map[string]string, keyspace)
	for i := 0; i < allocs; i++ {
		s := scratchPool.Get().(*scratch)
		s.reset()
		idx := i % keyspace
		s.key.WriteString("key-")
		s.writeNum(&s.key, idx)
		s.frag.WriteString("value-")
		s.writeNum(&s.frag, idx)
		s.frag.WriteByte('-')
		s.writeNum(&s.frag, i%97)
		s.frag.WriteByte('/')
		for r := 0; r < repeat; r++ {
			s.value.Write(s.frag.Bytes())
		}
		index[s.key.String()] = s.value.String()
		scratchPool.Put(s)
	}
	return index
}
```

```rust
/// arena variant: same shape, but the intermediates are bump-allocated and
/// the whole batch is reclaimed at once by `bump.reset()`.
fn build_arena(allocs: usize) -> HashMap<String, String> {
    let mut bump = Bump::with_capacity(SLAB_BYTES);
    let mut index = HashMap::with_capacity(KEYSPACE);
    let mut start = 0;
    while start < allocs {
        let end = (start + BATCH).min(allocs);
        {
            let mut scratch: BumpVec<BumpString> = BumpVec::with_capacity_in(end - start, &bump);
            for i in start..end {
                let idx = i % KEYSPACE;
                let mut key = BumpString::new_in(&bump);
                write!(key, "key-{idx}").expect("bump string write cannot fail");
                let mut frag = BumpString::new_in(&bump);
                write!(frag, "value-{idx}-{}/", i % 97).expect("bump string write cannot fail");
                let mut value = BumpString::new_in(&bump);
                for _ in 0..REPEAT {
                    value.push_str(frag.as_str());
                }
                index.insert(key.as_str().to_owned(), value.as_str().to_owned());
                scratch.push(frag); // keeps the churn alive per batch
            }
        } // borrows on the bump end here...
        bump.reset(); // ...so the whole batch vanishes in O(1)
        start = end;
    }
    index
}
```

Walk the C++ one first. The slab lives *outside* the batch loop and the
`monotonic_buffer_resource` is constructed *inside* it, over
`slab.data()/slab.size()`: each batch gets a fresh bump pointer into the same
already-touched pages, so batch teardown (`~arena` at the closing brace) frees
1000 iterations of strings without a single `free` call, and the next batch
pays no new page faults. The `scratch.reserve` is load-bearing in a way it
usually isn't: a monotonic resource never reclaims, so letting the vector grow
geometrically would leave every outgrown copy as dead weight *inside the
slab*. Same reason `make_kv` builds numbers with `std::to_chars` via
`append_num` — `std::to_string` would hand back an ordinary heap
`std::string`, silently routing churn around the arena. The survivors are
copied out explicitly (`std::string{key}`) because the index must outlive the
resource; forgetting that copy is the classic pmr lifetime bug, and it is the
exact discipline in one line: *bulk-free the churn, copy out what survives.*

The Rust version encodes the same rules in types instead of comments.
`BumpString::new_in(&bump)` borrows the arena, so the inner block must end —
every borrow dropped — before `bump.reset()` will compile. Where the C++
version would let you keep a `pmr::string` past `~arena` and crash, here the
borrow checker rejects the program. `reset()` keeps bumpalo's largest chunk,
so after the first batch the steady state is the same reused-slab pattern as
C++. The Go version has no batch boundary at all: `Get` → `reset` → build in
place → copy out via `String()` → `Put`, per iteration. `writeNum` reuses one
`[]byte` through `strconv.AppendInt` so digit formatting allocates nothing;
the only per-iteration allocations left are the two `String()` copies that the
index keeps — visible below in the heap profile, and the reason `gc_cycles`
drops from 9 to 3.

The wiring is the book's standard shape: a hand-rolled flag loop
(`--allocs`, `--variant`) so all three usage diagnostics are byte-identical;
the build+query phase timed with a monotonic clock; `getrusage(RUSAGE_SELF)`
read afterwards for `ru_maxrss` (kilobytes on Linux — it is bytes on the
BSDs, one of the fragile bits stated plainly); and Go additionally reads
`runtime.MemStats` before and after, reporting the `NumGC` delta so the GC
cost of *this workload* — not the process's whole life — is what prints. The
query phase re-derives every key and fails on a missing key or a zero byte
sum, so neither compiler nor collector can quietly optimize the work away.
Two more fragile bits: the 256KiB slab is sized to a batch, and a
monotonic resource that overflows silently falls through to its upstream
(the default resource — malloc), correct but no longer arena-fast; and
`ru_maxrss` is a high-water mark, so it can never show the arena *releasing*
memory — that is what massif is for below.

## Errors, three ways

The contract: bad usage (unknown flag, non-positive `--allocs`, unknown
variant) prints a diagnostic plus the usage line on stderr and exits 2;
runtime failures print `allocbench: ...` and exit 1. Running all three with
`--variant marble` on this host produced byte-identical stderr — confirmed
with `cmp` across the three captures — and exit 2 from each:

```console
[host]$ ./cpp/build/release/app --variant marble
allocbench: unknown variant: marble
usage: allocbench [--allocs N] [--variant default|arena]
```

The mechanisms differ by language, the policy doesn't. C++ parses into
`std::expected<Config, std::string>` — a usage error is a *value* carried to
`main`, which owns both stderr lines and the exit code; the one true runtime
error path (`getrusage` failing) converts `errno` into `std::error_code`
first. Go returns wrapped errors (`fmt.Errorf` with `%w`) from `parseArgs`
and `run`, and `main` chooses exit 2 or 1 by *which call* failed rather than
by inspecting strings. Rust splits the same way structurally:
`parse_args() -> Result<Config, String>` for usage, `anyhow::Result` with
`.context()` for runtime, printed with `{err:#}` so a failure would show the
whole chain (`getrusage: ...`) on one line. Same taxonomy in all three: usage
errors are boring values, runtime errors carry their cause.

## Concurrency lens

Everything in this chapter is single-threaded, and almost none of it is
thread-neutral. glibc malloc scales by giving *threads* their own arenas (up
to 8x the core count) plus the lock-free tcache — which is why a
multi-threaded allocation-heavy program's RSS can multiply: each arena
fragments separately. `monotonic_buffer_resource` is explicitly *not*
thread-safe; the pattern that survives contact with threads is one arena per
thread or per task, never one shared bump pointer. bumpalo says the same
thing in the type system: `Bump` is not `Sync`, so two threads cannot even
share a reference. `sync.Pool` is the one built for concurrency — it shards
per-P (per scheduler processor) so `Get`/`Put` on different cores don't
contend, and the GC empties it between cycles, which is precisely why it
suits churn-recycling and not long-term caching. And the Go collector itself
is concurrent: those 9 (or 47) cycles ran on other cores while `buildDefault`
kept allocating, so `gc_cycles` costs less wall time than it suggests — but
`GOMEMLIMIT` is global, so in a real service every goroutine shares the same
bargain you tuned above.

## Build, run, observe

```bash
[host]$ cd examples/17-allocators-and-gc-runtimes && ./demo.sh
```

Each language builds and runs both variants. Driven by hand on this host,
one line per run:

```console
[host]$ ./demo.sh cpp run
allocbench: variant=default allocs=200000 peak_rss=3908KB ms=18
[host]$ ./demo.sh cpp run --variant arena
allocbench: variant=arena allocs=200000 peak_rss=4240KB ms=20
[host]$ ./demo.sh go run
allocbench: variant=default allocs=200000 peak_rss=8064KB ms=47 gc_cycles=9
[host]$ ./demo.sh go run --variant arena
allocbench: variant=arena allocs=200000 peak_rss=7212KB ms=33 gc_cycles=3
[host]$ ./demo.sh rust run
allocbench: variant=default allocs=200000 peak_rss=2612KB ms=45
[host]$ ./demo.sh rust run --variant arena
allocbench: variant=arena allocs=200000 peak_rss=2424KB ms=45
```

Three readings. The C++ arena's peak is *higher* (4240 vs 3908KB): malloc
recycled the default variant's churn through the same tcache chunks, while
the always-touched slab is a fixed cost — the arena's win is deterministic
wholesale release, not a lower high-water mark at this size. Go shows the GC
account directly: recycling via `sync.Pool` cut cycles 9 → 3 and time 47 →
33 ms. Rust's arena win is wall time — on repeated runs the default settles
around 39–45 ms and the arena at 26–27 ms (the tied 45/45 pair above caught
a cold first run). The runner
(`python3 scripts/test-all-examples.py --only 17-allocators-and-gc-runtimes`)
verified the full behavior contract this session: `PASS PASS PASS`, 3 passed,
0 failed, 0 skipped.

## Cross-check: massif, /usr/bin/time, pprof

**Heap shape, under massif.** `ru_maxrss` is one number; valgrind's massif
records the whole heap-over-time curve (at ~20x slowdown, so 20,000 allocs).
Peak snapshots from `ms_print`, default first:

```console
[host]$ valgrind --tool=massif --massif-out-file=massif-default.out \
        ./cpp/build/release/app --allocs 20000
[host]$ ms_print massif-default.out
Number of snapshots: 74
 Detailed snapshots: [5 (peak), 12, 38, 41, 45, 49, 59, 69]
  n        time(i)         total(B)   useful-heap(B) extra-heap(B)    stacks(B)
  5      3,960,468          273,800          254,786        19,014            0
93.06% (254,786B) (heap allocation functions) malloc/new/new[], --alloc-fns, etc.
->29.16% (79,840B) 0x402BD6: allocate (new_allocator.h:162)
|           ->29.16% (79,840B) 0x402BD6: insert_or_assign<std::__cxx11::basic_string<char> > (unordered_map.h:786)
|             ->29.16% (79,840B) 0x402BD6: build_default (main.cpp:141)
```

(Intermediate frames of the allocation tree trimmed.)

The default peak is 273,800 bytes of many small blocks, the largest tree
being the index's hashtable nodes out of `build_default`. The arena run's
peak (snapshot 4 of 59) is 504,000 bytes — *larger*, matching the RSS story —
and 52.01% of it is a single 262,144-byte block: `_M_create_storage` →
`build_arena (main.cpp:151)`, the 256KiB slab, allocated once. The per-string
churn has vanished from malloc's ledger entirely, because inside the slab it
is pointer arithmetic — which is the pmr claim (locality plus free-at-once)
made visible by an independent tool.

**RSS agreement, with `/usr/bin/time -v`.** The kernel's accounting of the
same runs: the Go binary's self-reported `peak_rss=8320KB` matched `Maximum
resident set size (kbytes): 8320` exactly (both read the same `ru_maxrss`,
one via `getrusage`, one via `wait4`). The C++ pair read 3916 self vs 4388
from time, and 4312 vs 4356 for arena — the binary samples before printing
and process teardown, time observes the whole life; same ordering, sub-500KB
skew, no surprises.

**Where the Go bytes came from, with pprof.** Adding a temporary ten-line
`MEMPROFILE` hook (`runtime.GC()` then `pprof.WriteHeapProfile`) to a scratch
copy of `main.go` and reading the profiles with
`go tool pprof -sample_index=alloc_space -top`:

```console
[host]$ go tool pprof -sample_index=alloc_space -top -nodecount=3 app heap-default.pb.gz
Showing nodes accounting for 41474.84kB, 100% of 41474.84kB total
      flat  flat%   sum%        cum   cum%
38913.61kB 93.82% 93.82% 40449.63kB 97.53%  main.buildDefault
 1536.02kB  3.70% 97.53%  1536.02kB  3.70%  internal/strconv.FormatInt
     513kB  1.24% 98.77%      513kB  1.24%  runtime.mallocgc
```

The default variant allocated 41.5MB total, 93.82% of it flat in
`buildDefault` — the churn. The arena profile totals 13.3MB, topped by
`bytes.(*Buffer).String` at 10752.55kB (80.76%): almost exactly the two
surviving copies per iteration, with the churn recycled through the pool.
Three tools, three vantage points — allocator ledger, kernel RSS, runtime
profile — one consistent story.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the eBPF view of this chapter is allocator tracing without recompiling:
> `[vm]$ /usr/share/bcc/tools/memleak -p <pid>` samples outstanding
> allocations by stack, kernel-side, on any binary. bcc-tools need root and
> a matching kernel, so they run on the `systems-target` VM, and Part 8
> exercises them there against these exact binaries.

## What you learned

- `free` returns chunks to glibc's arena and tcache, not pages to the kernel
  — `mallinfo2` showed 6340KB of arena still held with 5KB in use, and a
  half-freed heap is fragmentation you can read out (`in-use 3130KB`
  interleaved with `free-on-list 3209KB`).
- An arena changes two things — allocation locality and free-at-once — and
  massif proved both: the churn collapsed into one 262,144-byte slab
  allocation, while peak RSS stayed *comparable* (4240 vs 3908KB) because
  malloc recycles churn well too.
- Rust's default allocator is glibc malloc (`nm -D` shows the imports;
  `ltrace` counts the calls), so everything in the malloc story applies until
  you opt into `bumpalo` — where `reset()` is O(1) and the borrow checker
  enforces the copy-out-what-survives rule that pmr leaves to discipline.
- Go's GC is a negotiable policy: the same binary ran 9 cycles at 8064KB, 47
  cycles at 5676KB under `GOMEMLIMIT=8MiB`, and 0 cycles at 37164KB when the
  limit was never approached — and `sync.Pool` is the arena-shaped tool that
  cut the default workload's cycles from 9 to 3.

Next, **pipes, FIFOs, and splice**: the oldest IPC primitive on the system,
and how to move bytes between processes without copying them through
userspace at all.

---

<p><span class="status status--verified">verified</span> — every number above
was produced on the Fedora 44 reference host (kernel 7.1.3-200.fc44) this
session: the runner printed <code>17-allocators-and-gc-runtimes PASS PASS
PASS</code> (3 passed, 0 failed, 0 skipped); the six variant lines, the four
GOGC/GOMEMLIMIT runs (gc_cycles 9/32/47/0), the mallinfo2 probe output, the
<code>nm -D</code> imports, the ltrace counts (415 vs 317 mallocs), the
massif peak snapshots (273,800B default, 504,000B arena with the 262,144B
slab block at <code>main.cpp:151</code>), the <code>/usr/bin/time -v</code>
agreements (8320KB == 8320KB for Go), and the pprof totals (41474.84kB vs
13314.77kB) are all real captured output. The Green Tea note claims only
what this host's go1.26.5 toolchain shows: <code>greenteagc</code> builds
carry no experiment deviation stamp while <code>nogreenteagc</code> does.
Bad usage was exercised on all three binaries: byte-identical stderr
(<code>cmp</code>-confirmed) and exit 2. The <code>memleak</code> callout is
unverified as marked and deferred to Part 8.</p>
