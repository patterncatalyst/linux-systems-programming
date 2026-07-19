---
title: "The eBPF Observation Toolkit"
order: 30
part: "Debugging"
description: "app work --seconds N runs an ordinary, unmodified workload on systems-target, and this chapter points the standard eBPF observation stack at it live: bcc-tools' execsnoop/opensnoop/funccount/offcputime/biolatency (and a real, plainly-reported profile failure), a bpftrace crash course from one-liners to a uprobe+uretprobe latency histogram, bpftool naming the loaded programs, c++filt demangling the exact mangled symbol a uprobe attaches to in C++ and Rust, and a live, measured RDI-vs-RAX register mismatch that shows exactly why uprobes on Go need more care than the other two — with SystemTap's kernel-module and dyninst backends compared alongside, one working and one blocked by a real kernel-devel mismatch."
duration: "55 minutes"
---

Chapter 28 read a crash gdb already knew about; Chapter 29 caught bugs a
compile-time instrumentation pass or a binary-translating emulator watched
for. Both chapters needed something extra glued onto the target: debug
info gdb could walk, or a sanitizer/valgrind rebuild. This chapter needs
neither. `app work --seconds N` is an ordinary, unmodified binary — no
special build, no `-fsanitize`, not even a debugger attached — and the
standard eBPF observation stack (bcc-tools, bpftrace, bpftool) watches it
entirely from outside, live, on the `systems-target` lab VM this book has
used since Part 3. The workload is deliberately small and legible: every
~230ms it opens/writes/closes one fixed file, forks and execs a short-lived
`true` every fourth iteration, calls one named hot function `busy_hash()`,
and sleeps through most of the interval — four separate baits for five
different tools, wired identically into C++23, Go, and Rust so the same
observation session works against all three languages' binaries. This
chapter also attaches uprobes to *our own* symbols directly, which is where
C++ name mangling, Rust's own mangling scheme, and Go's register-based
calling convention each become a real, measured concern rather than a
footnote.

The code is in `examples/30-ebpf-observation-toolkit/`. `./demo.sh build`
builds all three; `observe.sh <path-to-binary> [seconds]`, run as root on
the lab guest, is this chapter's actual demo — it starts the workload and
points five tools at it in turn, and the top-level `README.md` documents
the full contract and a real run's tail.

{% include excalidraw.html
   file="30-ebpf-tooling-stack"
   alt="Three stacked bands. The top kernel band holds five small boxes -- kprobe/kretprobe on kernel functions, uprobe/uretprobe on userspace functions, tracepoint as the stable kernel ABI, perf_event as timer/PMU sampling, and BTF/CO-RE for struct layout without headers. Amber arrows descend from three of these into the middle eBPF-tooling band, which holds bcc-tools (execsnoop/opensnoop/biolatency/offcputime/funccount/profile), bpftrace (one-liners and scripts, probe:target /filter/ action), bpftool (prog show/map show), and c++filt (demangle the exact symbol you attach to), with a note that libbpf loads a small verified BPF program per probe that the kernel runs in-kernel on each hit. Below that, a third band holds SystemTap as the sibling technology: its classic kernel-module backend needing matching kernel-devel, beside stap --runtime=dyninst, which patches the target binary directly in userspace with no kernel module. A fourth band at the bottom shows the three target binaries -- C++'s busy_hash under the SysV ABI with a correct arg0, Rust's cmd_work also correct under the SysV ABI once c++filt demangles its v0-mangled name, and Go's cmdWork in a dark box reading arg0/RDI wrong at 1, RAX right at 9, with a dashed amber arrow from it up to the eBPF-tooling band labeled arg0 mismatch. A footer note says every amber box ran live against systems-target this session; SystemTap's kernel-module path failed on a real kernel-devel/running-kernel mismatch, and the dyninst backend attached without erroring."
   caption="Figure 30.1 — the eBPF observation stack this chapter exercises live: kernel probe primitives at the bottom, bcc-tools/bpftrace/bpftool/c++filt above them, SystemTap as the sibling technology beside them, and the one real register-ABI mismatch that only shows up on the Go binary" %}

> **Tools used** — bcc-tools `execsnoop`, `opensnoop`, `funccount`,
> `offcputime`, `biolatency`, `profile` (`systems-target` VM, root);
> `bpftrace` v0.24.2 (`systems-target` VM, root); `bpftool` (`systems-target`
> VM, root); `c++filt` (host, to demangle before attaching); `nm` (host, to
> find the real symbol names); `stap` 5.5 (`systems-target` VM, root, for
> the SystemTap comparison callout). Everything under `systems-target` is
> provisioned by `cloud-init` there per `references/vm-lab.md`; `c++filt`
> and `nm` are part of `binutils` on `scripts/check-host.sh`.

## Watching from outside: the third way to observe a program

Chapter 29 drew a line between two ways of catching a bug: **translate the
binary** (valgrind, no rebuild, a software-emulated CPU) or **instrument the
source** (sanitizers, a required rebuild, near-native overhead). eBPF-based
observation is a third way, and it needs neither: point a probe at a running
kernel function, a running userspace function, or a stable tracepoint, and
the **kernel itself** runs a small, verified BPF program on every hit,
copying whatever you asked for back to userspace through a map or a ring
buffer. No rebuild, because the probe attaches to the compiled code exactly
as it stands; no restart even, because a `uprobe` can attach to a process
that is already running. The trade is `CAP_BPF`/`CAP_PERFMON` — on this lab
image that means root, which is why every tool in this chapter runs under
`sudo` on `systems-target` and never on the host, matching this book's rule
that eBPF is tooling we point at our own userspace programs, never kernel
code we write ourselves.

Four probe types cover everything this chapter touches. A **kprobe** (and
its return-side twin, **kretprobe**) attaches to almost any non-inlined
kernel function by name — `vfs_write`, say — and fires on entry or return.
A **uprobe**/**uretprobe** is the identical idea one level up: instead of a
kernel symbol, it takes a userspace **binary path plus a symbol name** (or a
raw offset), and it needs nothing from the target process except that the
symbol exist in its ELF symbol table — no debug info, no cooperation, no
special build. A **tracepoint** is a stable, kernel-maintained hook baked
into specific spots (`syscalls:sys_enter_openat` fires on every `openat(2)`
entry, arguments included) that will not move or disappear the way a raw
kprobe's target function occasionally does across kernel versions. A
**perf_event** probe fires on a timer or a PMU counter rather than a
specific code location — that is what an on-CPU sampling profiler rides on.
BTF (BPF Type Format) is the piece that lets modern tools read kernel
`struct` layouts without a matching `kernel-devel` package at all, which
matters directly to this chapter's SystemTap comparison below.

## bcc-tools: five purpose-built tools, one workload

**bcc** ships as a directory of small, ready-made tools
(`/usr/share/bcc/tools/` on this image), each one a purpose-built BPF
program with a Python front end — you do not write BPF C for any of these,
you invoke the tool and read its output. `observe.sh` runs five of them
against `app work --seconds N` in sequence, each one confirming a specific
bait fired. A real run against the C++ binary, staged at
`/tmp/lsp30-cpp/app` on `systems-target`:

```console
[vm]$ sudo bash /tmp/lsp30-observe.sh /tmp/lsp30-cpp/app 40
=== opensnoop (open() bait) ===
15320  app                 3   0 /tmp/lsp-ebpf-work-bait.txt
15320  app                 3   0 /tmp/lsp-ebpf-work-bait.txt
...
=== execsnoop (fork/exec bait) ===
true             15339   15320     0 /usr/sbin/true
true             15340   15320     0 /usr/sbin/true
...
=== funccount busy_hash (uprobe/funccount bait) ===
FUNC                                    COUNT
busy_hash                                  21
=== offcputime (sleep off-CPU bait) ===
    -                app (15322)
    __clock_nanosleep_2
    do_nanosleep
    hrtimer_nanosleep
=== bpftrace uprobe:busy_hash ===
Attached 2 probes
@calls: 22
=== summary ===
opensnoop_opens=20
execsnoop_execs=6
funccount_calls=21
offcputime_hits=2
bpftrace_calls=22
app_summary: work: done seconds=40 iters=173 opens=173 execs=44 busy_calls=173 busy_hash=1836395105209072940
```

`opensnoop` traces every `open`/`openat` and names the exact path our
workload writes, by comm `app`, not an inference from timing. `execsnoop`
shows `true` with **PPID 15320**, our workload's own pid — proof this is
*our* fork/exec, not unrelated system noise. `funccount` attaches a uprobe
at the pattern `<binary>:*busy_hash*` and just counts hits, reporting
**21** for a ~40s window at ~230ms/iteration (173 total iterations, sampled
mid-run for a shorter tool budget). `offcputime` samples stacks of threads
that are *not* running, bucketed by how long they were off-CPU, and the
sleep inside every iteration reliably parks our thread in
`__clock_nanosleep_2` — exactly the bait. `biolatency` is the fifth tool in
the family tour, run separately against real block I/O (the lab guest's
`/tmp` is tmpfs, so this needs the root filesystem):

```console
[vm]$ sudo timeout 6 /usr/share/bcc/tools/biolatency 1 3
Tracing block device I/O... Hit Ctrl-C to end.
     usecs               : count     distribution
       128 -> 255        : 3        |***************                        |
       256 -> 511        : 4        |****************************************|
       512 -> 1023       : 2        |*********************                  |
```

That histogram came from five `dd ... conv=fsync` writes to `/var/tmp`
completing in the low hundreds of microseconds — a real, measured
distribution, not a synthetic example. `profile`, the sixth named tool, is
where this tour hit a real wall worth reporting rather than hiding:

```console
[vm]$ sudo /usr/share/bcc/tools/profile -F 49 -p 15912 3
include/linux/ns_common.h:25:23: error: no member named 'ns_id' in 'struct ns_common'
...
1 warning and 3 errors generated.
Traceback (most recent call last):
  File "/usr/share/bcc/tools/profile", line 314, in <module>
    b = BPF(text=bpf_text)
```

`profile` compiles a small C snippet against the running kernel's own
headers at load time — even bcc tools with BTF support are not entirely
immune to kernel drift — and on this lab guest's kernel (6.19.10) that
snippet references a `struct ns_common` field the installed headers no
longer have under that name. bpftrace's own `profile:hz:N` probe type
sidesteps the header dependency and attached cleanly, but on this exact
workload it reported **zero samples** at both 49Hz and 99Hz over several
seconds — not a bug, but the real result of an on-CPU sampler pointed at
a process that spends over 99% of its time asleep in `nanosleep`.
`offcputime` and `profile` are answering two different questions (where did
the *waiting* time go, versus where did the *running* time go), and a
workload this sleep-dominated has an answer for exactly one of them.

## bpftrace: a crash course from one-liners to a script

**bpftrace** is a small, high-level language over the same probe types:
`probe:target /filter/ { action }`, with built-ins (`pid`, `comm`, `arg0`
through `argN`, `retval`, `str()`, `nsecs`) and aggregations (`count()`,
`hist()`, `@name` maps) that erase most of the boilerplate a hand-written
BPF program would need. A tracepoint one-liner, filtered to our workload's
real pid:

```console
[vm]$ sudo timeout 4 bpftrace -e 'tracepoint:syscalls:sys_enter_openat
    /pid == 17092/ { printf("openat comm=%s file=%s\n", comm, str(args.filename)); }'
Attached 1 probe
openat comm=app file=/tmp/lsp-ebpf-work-bait.txt
openat comm=app file=/tmp/lsp-ebpf-work-bait.txt
...
```

A kprobe one-liner aggregating into a histogram — every `vfs_write` call
from that same pid, bucketed by byte count (`arg2` is `vfs_write`'s third
kernel argument, the length):

```console
[vm]$ sudo timeout 3 bpftrace -e 'kprobe:vfs_write /pid == 17092/
    { @bytes = hist(arg2); } interval:s:3 { print(@bytes); exit(); }'
@bytes:
[8, 16)               13 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[16, 32)               3 |@@@@@@@@@@@@                                        |
```

Every bait line the workload writes (`"iter N\n"`) is 8-15 bytes most of
the time, 16-31 once digits roll over — the histogram is reading the exact
shape of our own `write(2)` calls. The short script below is the crash
course's third piece: a **uprobe/uretprobe pair**, timing one function
entry-to-return and bucketing the result, saved as
`busy_hash_latency.bt`:

```
uprobe:$1:busy_hash {
    @start[tid] = nsecs;
}

uretprobe:$1:busy_hash /@start[tid]/ {
    @latency_ns = hist(nsecs - @start[tid]);
    delete(@start[tid]);
}

interval:s:6 { exit(); }
```

`@start[tid]` is a per-thread-id associative map — the entry probe stashes
a timestamp keyed by the calling thread, and the matching return probe
looks it up, computes the delta, and folds it into a histogram before
deleting the entry so a later call cannot read a stale timestamp. Run
against the live C++ binary:

```console
[vm]$ sudo timeout 8 bpftrace /tmp/busy_hash_latency.bt /tmp/lsp30-cpp/app
Attached 5 probes
@latency_ns:
[256K, 512K)           4 |@@@@@@@@@@                                          |
[512K, 1M)             3 |@@@@@@@@                                            |
[1M, 2M)              19 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
```

`busy_hash`'s 300,000-round xorshift loop takes roughly 1-2ms most calls on
this host — a real, measured latency distribution for a function this
book's example code deliberately keeps simple, produced with five lines of
bpftrace and no rebuild of `app` at all.

## bpftool: naming what is actually loaded

**bpftool** is the inspection half of the story — it reads back what the
kernel currently has loaded, independent of whichever tool put it there.
With the `busy_hash` uprobe from the previous section still attached in a
background shell, a second root shell on the same guest:

```console
[vm]$ sudo bpftool prog show
935: kprobe  name uprobe__tmp_lsp30_cpp_app_busy_hash_1  tag 1c2c09b63327bc49  gpl
	loaded_at 2026-07-19T17:16:51+0000  uid 0
	xlated 848B  jited 467B  memlock 4096B  map_ids 24
```

Two things are worth reading closely here. First, `bpftrace` compiled the
attach point's name straight into the BPF program's own name
(`uprobe__tmp_lsp30_cpp_app_busy_hash_1`) — `bpftool` is reading that name
back from the kernel, not from bpftrace's own bookkeeping, so this is an
independent confirmation the probe really loaded. Second, the kernel
classifies this program's `type` as plain **`kprobe`** even though we asked
bpftrace for a `uprobe` — uprobes and kprobes share the same underlying BPF
program type at the kernel ABI level; the distinction between "kernel
function" and "userspace function" lives in how the probe is *attached*
(a kernel symbol versus a binary path + symbol), not in a separate program
type. `bpftool map show` in the same window lists the maps bpftrace
allocated for its own script state, each one tagged with the owning
process:

```console
[vm]$ sudo bpftool map show
555: array  name 56464433.rodata  flags 0x480
	key 4B  value 16B  max_entries 1  memlock 8192B
	btf_id 605  frozen
	pids bpftrace(16154)
```

`pids bpftrace(16154)` is the cross-check: this map is held open by the
exact bpftrace process running our script, not a leftover from some
earlier session — `bpftool` and `bpftrace` are two independent programs
agreeing on what is actually resident in the kernel right now.

## SystemTap: the sibling technology

**SystemTap** predates eBPF-based tracing by close to a decade and solves
the same problem — attach a probe, run a small script, read the answer —
through a different mechanism: its classic backend compiles your script
into a loadable **kernel module**, which means it needs a `kernel-devel`
package that matches the *exact* running kernel, not just a recent one.
This lab guest's mismatch is real and was captured, not invented:

```console
[vm]$ sudo stap -e 'probe begin { println("stap alive"); exit() }'
Checking "/lib/modules/6.19.10-300.fc44.x86_64/build/.config" failed with error: No such file or directory
Incorrect version or missing kernel-devel package, use: dnf install kernel-devel-6.19.10-300.fc44.x86_64
```

The installed `kernel-devel` on this image is built for `7.1.3-201.fc44`,
one version off the actually-running `6.19.10-300.fc44` — exactly the
class of drift bpftrace and bcc's BTF-backed tools were built to avoid.
SystemTap also ships a userspace-only **dyninst** backend
(`--runtime=dyninst`), which patches the target process's own code directly
and needs no kernel module at all:

```console
[vm]$ sudo stap --runtime=dyninst -e
    'probe process("/tmp/lsp30-cpp/app").function("busy_hash") { println("stap saw busy_hash"); exit() }' \
    -x 17092
Pass 5: run completed in 9810usr/680sys/13771real ms.
```

That attached and ran to completion with no compile or attach error, but it
printed nothing in this session's test window — a real, if inconclusive,
result reported as such rather than claimed as a hit. The real comparison
this callout can make from live evidence: SystemTap's classic path is more
sensitive to exact kernel-version matching than the BTF-based bpftrace/bcc
path used everywhere else in this chapter (which needed no matching
`kernel-devel` at all), while its dyninst backend trades that dependency
for a userspace-patching approach this session did not get a positive
result from in the time available. SystemTap has real strengths bpftrace
does not — a proper scripting language and years of production hardening
among them — and Chapter 45 covers it in depth; this callout is only the
comparison this chapter's own live evidence supports.

## How the code works

Every observation above needs a target worth watching, and `app work
--seconds N` is written specifically to be one: four deliberate baits, one
per tool family, wired identically into C++23, Go, and Rust so `observe.sh`
never needs a per-language branch. Each language keeps one counters
structure the loop mutates and the final summary line reads back:

```cpp
struct Counters {
    std::atomic<long> iters{0};
    std::atomic<long> opens{0};
    std::atomic<long> execs{0};
    std::atomic<long> busy_calls{0};
    // The accumulated busy_hash() result. Printed in the summary line below --
    // that's what keeps the optimizer from proving the whole busy_hash call
    // chain's result is unused and deleting it outright (noinline only stops
    // inlining, not interprocedural dead-code elimination of a provably-pure,
    // provably-unused call chain).
    std::atomic<std::uint64_t> busy_hash_val{0};
};
```

C++'s fields are `std::atomic` because the workload loop runs on a
`std::jthread` while `main` could in principle read them concurrently;
Go's equivalent `result` struct has no such requirement (a single goroutine
owns it end to end, handed to `main` once, complete, over a channel) and
Rust's `Counters` is a plain `Default`-derived struct for the same reason —
the concurrency shape, not the language, decides whether the counters need
to be atomic. The comment on `busy_hash_val` documents this chapter's own
`README.md`-recorded gotcha: the first version of this workload printed
`busy_calls` but never the accumulated hash, and GCC's interprocedural
analysis proved the entire `busy_hash()` call chain had no observable
effect and deleted it — `funccount` and the bpftrace uprobe both reported
**zero** calls even though `nm`/`objdump` still showed the symbol sitting
right there in the binary. `noinline` only forbids folding the function into
its caller; it says nothing about deleting a call whose result nothing
reads. Printing the final hash in the `done` line, in all three languages,
is what makes the call chain provably necessary again — a fragile-but-named
requirement worth restating here: if you trim this program down, keep the
final `busy_hash` print or the whole bait disappears.

The bait-producing helpers are small and literal, and each closes its own
resource idiomatically rather than by hand. C++'s `Fd` is a one-field RAII
wrapper whose destructor calls `::close` no matter how the scope is left;
Rust's `open_bait` returns a `rustix::fs::OwnedFd` and never calls
`close(2)` at all, because the `OwnedFd`'s `Drop` impl does it the moment
the value goes out of scope at the end of the function; Go relies on
`defer f.Close()` right after the successful open, the same "close no
matter what" guarantee spelled a third way. All three open the bait file
`O_CREAT|O_WRONLY|O_TRUNC` at mode `0666` — world-writable on purpose,
since the file may be created once by a root-run `observe.sh` and again by
an unprivileged `demo.sh run`, and a stricter mode would make the second
owner's open fail. `spawn_true`/`spawnTrue` forks (or lets the language
runtime fork on its behalf, for Go and Rust's `std::process::Command`) and
execs `true`, then waits for it — the exact shape `execsnoop` watches, with
the child's real parent pid always the workload's own.

The loop itself, `run_workload`/`runWorkload`, is the wiring: on every pass
it calls `open_bait` (opensnoop bait), execs `true` every fourth iteration
(execsnoop bait), calls `busy_hash()` once (funccount/uprobe bait), then
sleeps ~230ms (offcputime bait), incrementing counters as it goes until a
deadline computed from `--seconds` at start. C++ runs this loop on a
`std::jthread` and `main` calls `worker.join()` explicitly, so the summary
line always sees the final counters rather than racing the still-running
thread; Go starts it as a goroutine and blocks `main` on a buffered result
channel — the channel receive *is* the join; Rust runs it straight-line on
the main thread, no concurrency at all, since nothing else in the Rust
build needs to run alongside it. `cmd_work`/`cmdWork` is the one-line
dispatcher tying `main`'s parsed `--seconds` value to this loop and printing
the `start`/`done` lines around it — the exact function this chapter's
uprobe/mangling section below attaches a probe to directly. The named hot
function itself, verbatim in all three languages:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// The named hot function every uprobe/funccount/bpftrace command in this
// chapter targets by name. extern "C" so the symbol isn't mangled, matching
// the Rust build; noinline so -O2 can't fold it into the caller and make the
// uprobe's attach point disappear.
extern "C" [[gnu::noinline]] std::uint64_t busy_hash(std::uint64_t x) noexcept {
    for (std::uint64_t i = 0; i < kBusyRounds; ++i) {
        x ^= x << 7;
        x ^= x >> 9;
    }
    return x;
}
```

```go
// busy_hash is the named hot function every uprobe/funccount/bpftrace command
// in this chapter targets by name. Go 1.26 emits it as "main.busy_hash" in
// the symbol table; //go:noinline keeps the compiler from folding it into its
// caller and making the uprobe's attach point disappear.
//
//go:noinline
func busy_hash(x uint64) uint64 {
	for i := 0; i < busyRounds; i++ {
		x ^= x << 7
		x ^= x >> 9
	}
	return x
}
```

```rust
/// The named hot function every uprobe/funccount/bpftrace command in this
/// chapter targets by name. `#[unsafe(no_mangle)]` keeps the symbol as the
/// plain string "busy_hash" (matching the C++ build); `#[inline(never)]`
/// keeps the optimizer from folding it into its caller and making the
/// uprobe's attach point disappear.
#[unsafe(no_mangle)]
#[inline(never)]
pub extern "C" fn busy_hash(mut x: u64) -> u64 {
    for _ in 0..BUSY_ROUNDS {
        x ^= x << 7;
        x ^= x >> 9;
    }
    x
}
```

## Errors, three ways

The CLI contract is this book's usual shape and is byte-identical across
languages — `app work --seconds N` on success, and a bad or missing
`--seconds` prints a two-line usage/diagnostic message and exits **2** in
every language, confirmed live:

```console
[vm]$ /tmp/lsp30-cpp/app work; echo exit=$?
usage: app work --seconds N
exit=2
[vm]$ /tmp/lsp30-go/app work --seconds nope; echo exit=$?
work: bad --seconds value: nope
exit=2
[vm]$ /tmp/lsp30-rust/app; echo exit=$?
usage: app work --seconds N
exit=2
```

The more interesting three-way split is what the *observation tools*
report when a target symbol does not exist, and it differs by tool as much
as by language. Asking `bpftrace` to attach to a function that is not in
the binary at all fails loudly and refuses to attach:

```console
[vm]$ sudo bpftrace -e 'uprobe:/tmp/lsp30-cpp/app:no_such_function { printf("never\n"); }'
ERROR: Could not resolve symbol: /tmp/lsp30-cpp/app:no_such_function
ERROR: Unable to attach probe: uprobe:.../app:no_such_function. If this is expected, set the 'missing_probes' config variable to 'warn'.
```

`funccount`, by contrast, treats an unmatched pattern as an **empty result
set**, not an error — asking it for the exact unqualified name `busy_hash`
against the *Go* binary, which actually exports `main.busy_hash`:

```console
[vm]$ sudo /usr/share/bcc/tools/funccount -p 1 -d 1 '/tmp/lsp30-go/app:busy_hash'
No functions matched by pattern b'^busy_hash$'
```

That single line is the entire reason `observe.sh` and `verify.lua` use the
wildcard `<binary>:*busy_hash*` instead of the exact name everywhere in this
chapter: it is the one pattern that matches C++'s and Rust's plain
`busy_hash` *and* Go's package-qualified `main.busy_hash`, without a
per-language branch anywhere in the driver script.

## What each binary gives the uprobe tooling

`busy_hash` is deliberately kept unmangled in every language
(`extern "C"` in C++, `#[unsafe(no_mangle)]` in Rust, and Go never mangles
at all) precisely so one wildcard pattern (`*busy_hash*`) works across all
three without a per-language branch anywhere in `observe.sh`. Attaching to
any *other* function in the same binaries is where each language's real
symbol story — and one real ABI trap — shows up. `cmd_work`, the function
that dispatches `--seconds` into the workload loop, carries no such
protection in any language, so `nm` shows what each compiler really does
with a name by default:

```console
[host]$ nm cpp/build/release/app | grep cmd_work
00000000004025b0 t _ZN12_GLOBAL__N_1L8cmd_workEi.isra.0
[host]$ nm rust/target/release/app | grep cmd_work
000000000001b1d0 t _RNvCs6FzKXswKZCc_3app8cmd_work
[host]$ nm go/bin/app | grep -i cmdWork
00000000004c6760 T main.cmdWork
```

**C++** mangles by the Itanium ABI: `cmd_work` sits in an anonymous
namespace (`(anonymous namespace)::`), which the mangler folds into
`_ZN12_GLOBAL__N_1L8cmd_workEi`, and GCC's interprocedural specialization
pass appended `.isra.0` (a clone with some arguments replaced by scalars,
"interprocedural scalar replacement of aggregates" — here a no-op clone,
but the suffix survives regardless). `c++filt` demangles it without any
flag, auto-detecting the Itanium scheme:

```console
[host]$ c++filt _ZN12_GLOBAL__N_1L8cmd_workEi.isra.0
(anonymous namespace)::cmd_work(int) [clone .isra.0]
```

**Rust** mangles under its own **v0** scheme (the `_R` prefix), and modern
`c++filt` auto-detects that too — no `--format=rust` flag needed on this
host's binutils:

```console
[host]$ c++filt _RNvCs6FzKXswKZCc_3app8cmd_work
app[4db24ee1e6ed4506]::cmd_work
```

**Go never mangles anything.** `main.cmdWork` *is* the linker symbol,
package-qualified and immediately readable — there is no demangling step
for Go at all, which sounds like the simplest story of the three until the
next section shows what it costs.

{% include excalidraw.html
   file="30-uprobe-attachment"
   alt="Three parallel columns, one per language, each four boxes tall. C++ column: nm reports the mangled symbol t _ZN12_GLOBAL__N_1L8cmd_workEi.isra.0; c++filt (auto Itanium ABI) demangles it to (anonymous namespace)::cmd_work(int); a uprobe attaches using the MANGLED string as the attach target; the probe fires with seconds_arg=5 for --seconds 5, and arg0==RDI is correct because C++ follows the plain SysV x86-64 ABI. Rust column: nm reports t _RNvCs6FzKXswKZCc_3app8cmd_work; c++filt auto-detects Rust v0 mangling and demangles it to app[hash]::cmd_work; a uprobe attaches using that same mangled string; the probe fires with seconds_arg=7 for --seconds 7, and arg0==RDI is again correct under the SysV ABI. Go column: nm reports T main.cmdWork, never mangled; no demangling step is needed since the package-qualified name is already the symbol; a uprobe attaches to the plain unmangled name main.cmdWork; the probe fires for --seconds 9 but RDI(arg0) reads 1, which is wrong, while RAX correctly reads 9 -- a dashed amber arrow connects this box back up to the tooling band above, labeled arg0 mismatch. A footer band spans all three columns explaining that Go's ABIInternal (1.17+) passes integer arguments in the order RAX, RBX, RCX, RDI, RSI, R8, R9, ... rather than the SysV order RDI, RSI, RDX, ... that bpftrace's arg0/arg1 assume, and that goroutine stacks also grow by copying, so a captured stack address can move out from under a probe that read it from the stack instead of a register."
   caption="Figure 30.2 — the same uprobe-attach recipe (find the symbol, demangle it to read it, attach to the mangled string) across three binaries, ending in the one real, measured argument-register mismatch that shows up only on Go" %}

Attaching directly to the mangled string on the C++ and Rust binaries, with
the workload started immediately after so the one-shot call to `cmd_work`
is still ahead of it:

```console
[vm]$ sudo timeout 4 bpftrace -e 'uprobe:/tmp/lsp30-cpp/app:_ZN12_GLOBAL__N_1L8cmd_workEi.isra.0
    { printf("hit cmd_work seconds_arg=%d\n", arg0); }' &
[vm]$ sudo /tmp/lsp30-cpp/app work --seconds 5 >/tmp/cpp2.log 2>&1
hit cmd_work seconds_arg=5
```

```console
[vm]$ sudo timeout 4 bpftrace -e 'uprobe:/tmp/lsp30-rust/app:_RNvCs6FzKXswKZCc_3app8cmd_work
    { printf("rust hit cmd_work seconds_arg=%d\n", arg0); }' &
[vm]$ sudo /tmp/lsp30-rust/app work --seconds 7 >/tmp/rust2.log 2>&1
rust hit cmd_work seconds_arg=7
```

Both print exactly the `--seconds` value passed on the command line —
`arg0` (bpftrace's shorthand for the first System V x86-64 integer
argument register, `RDI`) is correct in both, because both C++ (this
function is not `extern "C"`, but the Itanium C++ ABI still lays out
integer arguments the plain SysV way) and Rust place their first integer
argument in `RDI` in practice on this target. **Go does not.** Since Go
1.17, the Go compiler's own **ABIInternal** passes integer and pointer
arguments in registers in the order `RAX, RBX, RCX, RDI, RSI, R8, R9, R10,
R11` — a different order than the SysV convention every uprobe tool's
`arg0`/`arg1` shorthand assumes. The same attach recipe against Go's
`main.cmdWork(seconds int) int`, reading both the naive `arg0` and the true
first-argument register side by side:

```console
[vm]$ sudo timeout 4 bpftrace -e 'uprobe:/tmp/lsp30-go/app:main.cmdWork
    { printf("go cmdWork rdi(arg0)=%d rax=%d\n", arg0, reg("ax")); }' &
[vm]$ sudo /tmp/lsp30-go/app work --seconds 9 >/tmp/go2.log 2>&1
go cmdWork rdi(arg0)=1 rax=9
```

`rax=9` is exactly `--seconds 9`; `rdi(arg0)=1` is not — it is whatever
happened to be sitting in `RDI` from a previous call, because Go's compiler
never put the argument there in the first place. This is the concrete
version of "uprobes on Go are trickier": a tool that reads arguments by the
SysV convention's positional shorthand silently returns a *plausible-looking
wrong number* rather than an error, and the only way to get the right
answer is to know Go's own calling convention for that specific function
(which can itself vary by whether the function is inlined, its argument
count, and the Go toolchain version) rather than trust a generic `argN`.
The second half of "trickier" is one this session did not need to
reproduce to state accurately: Go's goroutine stacks are **movable** — the
runtime grows a goroutine's stack by copying it to a larger allocation and
rewriting every pointer *it* tracks into the new location, but a raw stack
address a uprobe captured externally is not one of the pointers the runtime
knows to fix up, so an argument read off the stack (rather than a register)
can point at stale memory after a stack-growth event the tracer had no way
to see coming.

## Build, run, observe

```console
[vm]$ export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target
[host]$ cd examples/30-ebpf-observation-toolkit && ./cpp/demo.sh build
[host]$ scp cpp/build/release/app "fedora@$IP:/tmp/lsp30-cpp/app"
[host]$ scp observe.sh "fedora@$IP:/tmp/lsp30-observe.sh"
[vm]$ sudo bash /tmp/lsp30-observe.sh /tmp/lsp30-cpp/app 60
```

`observe.sh` prints the five-tool tour and a `=== summary ===` block
deriving one hit count per tool, all nonzero, all tied to the workload's own
pid or bait path — never merely "the tool ran and exited 0." The runner
drives the same contract for all three languages against the live VM:

```console
[host]$ python3 scripts/test-all-examples.py --only 30-ebpf-observation-toolkit --mode vm
30-ebpf-observation-toolkit  PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

## Cross-check: three independent views of one workload agreeing

No single tool in this chapter is trusted alone — every tool's count is
checked against either another tool watching the same process, or against
`bpftool` reading back what the kernel actually loaded. From the same
`observe.sh` run quoted above, `execsnoop` (fork/exec) and `bpftrace`'s
uprobe (function calls) are two completely different probe types — a
tracepoint-adjacent syscall tracer versus a userspace-symbol uprobe —
watching the same 40-second window of the same pid, and they agree with
each other and with the program's own printed counters:

```
execsnoop_execs=6          <- 6 "true" children (44 execs at 4s granularity, sampled)
bpftrace_calls=22          <- 22 busy_hash() uprobe hits
app_summary: work: done seconds=40 iters=173 opens=173 execs=44 busy_calls=173 busy_hash=1836395105209072940
```

`execsnoop`'s 6 and the program's own `execs=44` differ only because the
tool's own capture window (a `timeout`-bounded slice near the start of the
run) is shorter than the workload's full 40 seconds — exactly what
`observe.sh`'s design note says to expect, and the ratio (6 execs in roughly
6 seconds of `execsnoop` runtime, at one exec every ~1 second given the
4-iteration cadence) lines up. The second, independent cross-check is
`bpftool prog show` naming the exact uprobe program bpftrace loaded while
it was still attached (`uprobe__tmp_lsp30_cpp_app_busy_hash_1`, `type
kprobe`), read from the kernel's own bookkeeping rather than from anything
bpftrace itself reported — two separate programs reading the same kernel
state and agreeing. The third is the mangled-symbol pair: `nm` and
`c++filt` independently identify and translate `cmd_work`'s real linker
name off the binary, and a live uprobe attached to that exact string is
what actually fires and reports the right `--seconds` value back — proof
the demangled name and the attach target are the same symbol, not a
coincidence of two unrelated strings.

## What you learned

- eBPF-based observation is a third way to watch a program, beside binary
  translation (valgrind) and compile-time instrumentation (sanitizers): the
  kernel runs a small verified program on a real kprobe/uprobe/tracepoint/
  perf_event hit, live, with no rebuild and no restart — bcc-tools'
  `execsnoop`/`opensnoop`/`funccount`/`offcputime`/`biolatency` and
  bpftrace's one-liners and scripts all ran this way against an unmodified
  binary on `systems-target`.
- `bpftool prog show`/`map show` is the independent check on what the
  kernel actually has loaded — it named bpftrace's own uprobe program by
  the exact attach-point string bpftrace compiled into it, confirmed from
  the kernel's bookkeeping, not bpftrace's.
- A uprobe attaches to the ELF symbol table's literal string, mangled or
  not: `c++filt` auto-detects and demangles both C++'s Itanium scheme and
  Rust's v0 scheme for reading, but the attach target itself stays the
  mangled string in both languages — and both fire correctly (`arg0`
  matching `--seconds`) because both follow the plain SysV x86-64 ABI.
- Go never mangles, but its own register-based ABIInternal (`RAX` first,
  not `RDI`) means the same `arg0` shorthand silently reads the *wrong*
  register — measured live as `rdi(arg0)=1` (wrong) beside `rax=9` (right,
  matching `--seconds 9`) — and goroutine stacks moving under a copying
  stack-grow makes any stack-based argument read riskier still.

Next, **per-language toolbelts**: `perf`, `pprof`, `delve`, and
`cargo-flamegraph` each answer "where does the time go" their own native
way, on the same kind of unmodified binary this chapter just watched from
outside.

---

<p><span class="status status--verified">verified</span> — every transcript
above (except the SystemTap dyninst run's silence, reported as
inconclusive rather than a positive result) was produced against the
running `systems-target` lab VM (kernel 6.19.10-300.fc44.x86_64,
bpftrace v0.24.2, bcc-tools bundled with that image, bpftool from the same
kernel package, stap 5.5) and the Fedora 44 host (binutils
<code>c++filt</code>/<code>nm</code>) this session. Confirmed live:
<code>observe.sh</code> against the staged C++ binary printed
<code>opensnoop_opens=20</code>, <code>execsnoop_execs=6</code>,
<code>funccount_calls=21</code>, <code>offcputime_hits=2</code>,
<code>bpftrace_calls=22</code>, and
<code>app_summary: work: done seconds=40 iters=173 opens=173 execs=44
busy_calls=173 busy_hash=1836395105209072940</code>;
<code>biolatency 1 3</code> produced a real block-I/O latency histogram
against five <code>dd conv=fsync</code> writes; bcc's <code>profile</code>
failed to load with a genuine
<code>no member named 'ns_id' in 'struct ns_common'</code> compile error on
this kernel, while bpftrace's own <code>profile:hz</code> probe attached
cleanly but reported zero on-CPU samples against a workload that sleeps
over 99% of the time; the <code>busy_hash_latency.bt</code>
uprobe/uretprobe script reported a real latency histogram peaking at
<code>[1M, 2M)  19</code> (nanoseconds); <code>bpftool prog show</code>
named a live kprobe-type program
<code>uprobe__tmp_lsp30_cpp_app_busy_hash_1</code> while bpftrace held it
attached, and <code>bpftool map show</code> attributed its maps to
<code>pids bpftrace(16154)</code>; <code>c++filt</code> demangled
<code>_ZN12_GLOBAL__N_1L8cmd_workEi.isra.0</code> to
<code>(anonymous namespace)::cmd_work(int) [clone .isra.0]</code> and
auto-detected Rust v0 mangling on
<code>_RNvCs6FzKXswKZCc_3app8cmd_work</code> as
<code>app[4db24ee1e6ed4506]::cmd_work</code>, and live uprobes on both exact
mangled strings fired with <code>seconds_arg=5</code>/<code>7</code>
matching their real <code>--seconds</code> flags; the Go ABI experiment
reported <code>rdi(arg0)=1</code> (wrong) beside <code>rax=9</code> (right)
for <code>--seconds 9</code> on <code>main.cmdWork</code>; the bad-usage
contract exited <b>2</b> identically in all three languages;
<code>bpftrace</code> refused to attach to a nonexistent symbol with
<code>ERROR: Could not resolve symbol</code>, and bcc's
<code>funccount</code> reported <code>No functions matched by pattern
b'^busy_hash$'</code> against the Go binary's real
<code>main.busy_hash</code>; and <code>stap</code>'s kernel-module backend
failed with a real <code>kernel-devel</code> version mismatch
(<code>6.19.10-300.fc44</code> running versus
<code>kernel-devel-7.1.3-201.fc44</code> installed) while
<code>--runtime=dyninst</code> attached and ran to completion with no
error. The formal three-language runner
(<code>scripts/test-all-examples.py --only
30-ebpf-observation-toolkit --mode vm</code>) confirmed the same
<code>verify.lua</code> contract end to end against this VM this session,
printing <code>30-ebpf-observation-toolkit  PASS  PASS  PASS</code> (3
passed, 0 failed, 0 skipped) for cpp/go/rust in turn — each language's own
run independently reproducing opensnoop's bait-path match, execsnoop's
PPID-matched <code>true</code> child, funccount's nonzero
<code>busy_hash</code> count, offcputime's comm-<code>app</code> stack, and
bpftrace's nonzero <code>@calls</code>.</p>
