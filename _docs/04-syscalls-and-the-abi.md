---
title: "Syscalls and the ABI"
order: 4
part: "Foundations"
description: "What a system call really is — the syscall instruction, the register-level kernel ABI, and the vDSO fast path — traced through sysprobe, one specimen program that shows how glibc, the Go runtime, and rustix each reach the same kernel."
duration: 40 minutes
---

Chapter 1 ended on a number it did not explain: the C++ hello made 64 syscalls,
the Go hello made 217, and we waved at "the runtime" and moved on. This chapter
is where that debt gets paid. With the lab and observability stack built, Part
1 starts at the only interface that actually matters in this book — the
user/kernel boundary — and answers three questions precisely: what *is* a
system call at the instruction level, why do some libc calls never reach the
kernel at all (the vDSO), and what path does each of our three languages take
from your function call down to the `syscall` instruction. The vehicle is
`sysprobe`, a program designed not to *do* anything interesting but to be
*watched*: four labeled steps, each a different kind of syscall, run under
`strace` three times.

The code is in `examples/04-syscalls-and-the-abi/`. `./demo.sh` builds and
runs all three implementations; the `README.md` there documents the shared
CLI contract and the exact five-line output that `verify.lua` asserts.

{% include excalidraw.html
   file="04-user-kernel-path"
   alt="A user-space band with your code, a language wrapper, and the syscall instruction, plus a dashed vdso page; an amber trap arrow crosses into a kernel band through entry_SYSCALL_64 and the syscall table to sys_getrandom, with a dashed return arrow carrying rax back."
   caption="Figure 4.1 — one getrandom(2) crossing the boundary: marshal registers, trap, dispatch by number, return rax — or take the vDSO fast path and never trap at all" %}

> **Tools used** — `strace`, `objdump`, `ldd`, `python3` (host); `bpftrace`
> (lab VM, exercised in Part 8). Everything here is checked by
> `scripts/check-host.sh` or preinstalled in the lab VMs.

## The boundary is an instruction, not an API

A system call is not a function call that happens to be important. Your
process runs in ring 3, where the CPU refuses to touch page tables, device
registers, or other processes' memory; the kernel runs in ring 0, where it
can. The *only* legitimate way across on x86-64 is a handful of trap
instructions, and on modern Linux that means `syscall`. The contract around
that instruction — the **syscall ABI** — is register-level and brutally
simple: the syscall *number* goes in `rax` (318 for `getrandom` on x86-64),
up to six arguments go in `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`, and the
result comes back in `rax`, where a small negative value means "error
`-errno`". Note the fourth register: the C function-call ABI would use `rcx`,
but the `syscall` instruction itself clobbers `rcx` to save the return
address, so the kernel ABI substitutes `r10`. That one register swap is why
every language needs at least a sliver of assembly or a wrapper library to
make a syscall — you cannot express "arg 4 in r10" in portable C.

When the instruction executes, the CPU switches to ring 0 and jumps to the
kernel's `entry_SYSCALL_64`, which moves onto a kernel stack, saves your
registers, and indexes `sys_call_table` by `rax` to dispatch the handler.
The handler runs *in your process*, just in kernel mode — `sys_getrandom`
writes directly into the buffer your `rdi` pointed at. Then the path unwinds
and `sysretq` drops back to ring 3 with the result in `rax`. Every trap is a
full round trip — two mode switches plus the kernel work — which is why
syscall *count* is a performance currency, and why `strace`, which uses
`ptrace` to stop the process at exactly this boundary, sees every trap a
process makes and nothing else.

## The vDSO: syscalls that never happen

That "nothing else" has teeth. Some things the kernel exports are read so
often — the clock, above all — that trapping for each read would be absurd.
So the kernel maps a small shared library into every process, the **vDSO**,
visible in any process's memory map:

```bash
[host]$ grep -E 'vdso|vvar' /proc/self/maps
7f9fc010b000-7f9fc010f000 r--p 00000000 00:00 0    [vvar]
7f9fc010f000-7f9fc0111000 r--p 00000000 00:00 0    [vvar_vclock]
7f9fc0111000-7f9fc0113000 r-xp 00000000 00:00 0    [vdso]
```

`[vdso]` is executable kernel-provided code; the `[vvar]` pages are read-only
data the kernel keeps current (timekeeping state in `vvar_vclock`). A
`clock_gettime` through glibc calls into that page, reads the shared data,
and returns — ring 3 the whole way. To see the difference, I compiled a
16-line probe that loops `clock_gettime(CLOCK_MONOTONIC, …)` a thousand
times, then `getrandom(buf, 16, 0)` a thousand times, and ran it under the
book's strace 7.1 build:

```bash
[host]$ strace -c -e trace=clock_gettime,getrandom ./vdso-probe
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
  0.00    0.000000           0         2           getrandom
------ ----------- ----------- --------- --------- ----------------
100.00    0.000000           0         2           total
```

One thousand `clock_gettime` calls: **zero** syscalls. One thousand
`getrandom` calls: **two** — and neither is the one you would predict.
The full trace shows `getrandom("…", 8, GRND_NONBLOCK) = 8` (glibc seeding
internal state at startup) and `getrandom("…", 32, 0) = 32`. Where did our
sixteen-byte request go? Kernel 7.1 plus glibc 2.43 route `getrandom`
through the vDSO's `vgetrandom`: on first use it traps *once* to pull a
32-byte ChaCha20 key from the kernel, then generates every byte you asked
for — all thousand requests — in user space. The lesson generalizes: `strace`
shows you the boundary, not your API calls, and on a current kernel even
"obvious" syscalls like `getrandom` may have quietly stopped being syscalls.
Keep that 32-byte signature in mind; it reappears inside `sysprobe`.

## Three roads to the same instruction

{% include excalidraw.html
   file="04-runtime-syscall-layers"
   alt="Three side-by-side stacks — C++ code over glibc wrappers over syscall instructions in libc, Go code over the Go runtime over 511 syscall instructions in a static binary, Rust code over the rustix linux_raw backend over 32 inlined syscall instructions — all with amber arrows labeled 70, 283, and 73 calls into one dark kernel ABI box."
   caption="Figure 4.2 — the per-language layer stacks over the one register ABI, annotated with this chapter's real strace -c totals" %}

The kernel ABI is one; the roads to it differ, and `file`, `ldd`, and
`objdump` on the three freshly built `sysprobe` binaries map them exactly.
Counting literal `syscall` instructions with
`objdump -d <binary> | grep -cw syscall`:

- **C++ — 0.** The binary is dynamically linked (`ldd` lists `libstdc++`,
  `libm`, `libgcc_s`, `libc`, and `linux-vdso.so.1` itself). Your code calls
  glibc *wrappers*; every `syscall` instruction lives in `libc.so.6`. The
  wrapper layer is not thin: it flips `-errno` into the `errno` global,
  implements thread-cancellation points, chooses vDSO fast paths — and
  sometimes substitutes a different syscall entirely, as we will see with
  `nanosleep`.
- **Go — 511.** The binary is statically linked; `ldd` says
  `not a dynamic executable`. Go carries its own syscall stubs, its own
  calling convention, and its own stack discipline (goroutine stacks are
  small and growable, so the runtime must ensure a syscall never runs on
  one that might move). `golang.org/x/sys/unix` is a veneer over
  `runtime`-provided entry points — including, since Go 1.24, the vDSO:
  `x/sys` v0.47.0 links `unix.Getrandom` straight to `runtime.vgetrandom`
  via `go:linkname` and only falls back to the raw syscall where the vDSO
  is unsupported.
- **Rust — 32.** The binary still links `libc.so.6` — std uses it for
  process startup — but `rustix` compiles with its `linux_raw` backend on
  x86-64, inlining raw `syscall` instructions at each call site and
  returning the `rax` error directly as `Result<_, Errno>`. No `errno`
  global is ever involved, and no glibc policy sits between your call and
  the kernel — which is exactly why rustix's `getrandom` shows up in
  `strace` and glibc's does not.

## How the code works

`sysprobe` performs four steps, chosen so each exercises a different species
of syscall: **open** creates a resource (an fd), **write** moves data through
one, **sleep** blocks in the kernel, and **random** asks the kernel for a
service with partial-result semantics. Each step prints `step=<name> ok`, the
program ends with `sysprobe: 4 steps ok`, and any argument gets `usage: app`
and exit 2 — identical in all three languages, so one `verify.lua` asserts
all five lines appear in order for each. Step 1 is the richest, so it is the
excerpt; all three are the real functions from
`examples/04-syscalls-and-the-abi/`, verbatim.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Step 1: openat(2). Prefer an anonymous O_TMPFILE inode (no name ever
// appears in the directory); fall back to mkstemp(3) + unlink(2) on
// filesystems that lack O_TMPFILE.
[[nodiscard]] std::expected<unique_fd, std::error_code>
open_scratch(const std::string& dir) {
    if (const int fd =
            ::openat(AT_FDCWD, dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        fd >= 0) {
        return unique_fd{fd};
    }
    if (errno != EOPNOTSUPP && errno != EISDIR && errno != EINVAL) {
        return std::unexpected(last_error());
    }
    std::string tmpl = dir + "/sysprobe.XXXXXX";
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        return std::unexpected(last_error());
    }
    unique_fd owned{fd};
    if (::unlink(tmpl.c_str()) != 0) {
        return std::unexpected(last_error());
    }
    return owned;
}
```

```go
// openScratch is step 1: openat(2). Prefer an anonymous O_TMPFILE inode (no
// name ever appears in the directory); fall back to a named create+unlink on
// filesystems that lack O_TMPFILE.
func openScratch(dir string) (int, error) {
	fd, err := unix.Openat(unix.AT_FDCWD, dir,
		unix.O_TMPFILE|unix.O_RDWR|unix.O_CLOEXEC, 0o600)
	if err == nil {
		return fd, nil
	}
	if !errors.Is(err, unix.EOPNOTSUPP) && !errors.Is(err, unix.EISDIR) &&
		!errors.Is(err, unix.EINVAL) {
		return -1, fmt.Errorf("openat %s (O_TMPFILE): %w", dir, err)
	}
	path := fmt.Sprintf("%s/sysprobe.%d", dir, os.Getpid())
	fd, err = unix.Openat(unix.AT_FDCWD, path,
		unix.O_CREAT|unix.O_EXCL|unix.O_RDWR|unix.O_CLOEXEC, 0o600)
	if err != nil {
		return -1, fmt.Errorf("openat %s: %w", path, err)
	}
	if err := unix.Unlink(path); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("unlink %s: %w", path, err)
	}
	return fd, nil
}
```

```rust
/// Step 1: openat(2). Prefer an anonymous O_TMPFILE inode (no name ever
/// appears in the directory); fall back to a named create+unlink on
/// filesystems that lack O_TMPFILE.
fn open_scratch(dir: &str) -> Result<OwnedFd> {
    let mode = Mode::RUSR | Mode::WUSR;
    match fs::openat(
        CWD,
        dir,
        OFlags::TMPFILE | OFlags::RDWR | OFlags::CLOEXEC,
        mode,
    ) {
        Ok(fd) => return Ok(fd),
        Err(Errno::OPNOTSUPP | Errno::ISDIR | Errno::INVAL) => {}
        Err(e) => return Err(e).with_context(|| format!("openat {dir} (O_TMPFILE)")),
    }
    let path = format!("{dir}/sysprobe.{}", std::process::id());
    let fd = fs::openat(
        CWD,
        &path,
        OFlags::CREATE | OFlags::EXCL | OFlags::RDWR | OFlags::CLOEXEC,
        mode,
    )
    .with_context(|| format!("openat {path}"))?;
    fs::unlinkat(CWD, &path, AtFlags::empty()).with_context(|| format!("unlink {path}"))?;
    Ok(fd)
}
```

The `openat` call opens the *directory itself* with `O_TMPFILE`: the kernel
allocates an inode with a link count of zero, so no name ever appears — no
cleanup, no symlink races, gone on close. Not every filesystem supports it,
so each version distinguishes "this filesystem can't" (`EOPNOTSUPP`, or the
`EISDIR`/`EINVAL` older kernels and filesystems report) — recoverable, take
the create-then-`unlink` fallback, which reaches the same anonymous-inode
state through a brief named window — from any other errno, which is a real
failure to propagate. Getting that triage *wrong* is invisible on your
machine and a crash on someone else's; getting it right is most of what
"handling" an errno means.

Step 2, `write_all`, loops because `write(2)` may legally write *fewer*
bytes than asked, so a single unchecked call is a data-loss bug even when it
"works"; each iteration re-slices the buffer by the returned count. The fd
is then closed *deliberately early* — RAII scope exit in C++, a checked
`unix.Close` in Go, `drop(fd)` in Rust — so the trace shows a clean
`openat → write → close` group before the sleep starts. Step 3 sleeps 10 ms
via `nanosleep(2)`, and on `EINTR` resumes with the *remainder* the kernel
wrote back — resuming with the original request would restart the full 10 ms
every time a signal landed. Step 4 loops `getrandom(2)` over partial reads
for the same reason as `write_all`. `main` in each language is only
sequencing: check `argc`, resolve `$TMPDIR` (default `/tmp`), run the four
steps, print per-step lines as they succeed.

### Errors, three ways

All three languages receive the identical fact — `rax` came back negative —
and immediately disagree about what to do with it. glibc stores it into the
thread-local `errno` and returns `-1`, so the C++ code's first act is
`last_error()`: capture `errno` into a `std::error_code` *now*, before any
other libc call can overwrite it, and carry it in `std::expected` as a
value. Go's `unix` package returns a `syscall.Errno` as an ordinary `error`;
the code wraps with `%w` (`"openat %s (O_TMPFILE): %w"`) so `errors.Is(err,
unix.EOPNOTSUPP)` still matches through the added context. rustix never
materializes a global at all: the negative `rax` *is* the `Err(Errno)`, and
the fallback triage becomes a match arm —
`Err(Errno::OPNOTSUPP | Errno::ISDIR | Errno::INVAL)`. Same kernel fact,
three idioms; Chapter 5 turns this three-way comparison into the book's
error-handling policy.

### Concurrency lens

`strace -c` needs `-f` for the Go binary only, and the reason previews Part
7. Before `main` ran, the Go runtime made 4 `clone` calls (its worker
threads) and 114 `rt_sigaction` calls (installing handlers so it can
preempt goroutines); while our program slept its one 10 ms sleep, a runtime
thread issued 61 `nanosleep` calls of its own — visible in the trace as
`{tv_nsec=20000}` repeated, then doubling `40000`, `80000`, up to
`2560000` as the runtime's monitor backs off polling an idle program.
C++ and Rust stay a single thread from `execve` to exit, which is exactly
why their traces need no `-f`. The portable lesson is the `EINTR` loops:
once any process has signal handlers (as Go's runtime just demonstrated),
*every* blocking syscall can return early, so retry-on-`EINTR` is not
defensive decoration but the baseline discipline — Chapter 12 (signals)
makes it precise.

## Build, run, observe

```bash
[host]$ cd examples/04-syscalls-and-the-abi && ./demo.sh
```

Each language builds (CMake `release` preset, `go build`, `cargo build
--release`) and prints the identical five lines:

```
step=open ok
step=write ok
step=sleep ok
step=random ok
sysprobe: 4 steps ok
```

The book's harness agrees — `python3 scripts/test-all-examples.py --only
04-syscalls-and-the-abi` reports `PASS` for cpp, go, and rust. Now run it
the way it was designed to be run. This book's strace lives at
`scratchpad/usr/bin/strace` (v7.1) on the reference host; the C++ binary,
filtered to the interesting calls:

```bash
[host]$ strace -e trace=openat,write,close,nanosleep,clock_nanosleep,getrandom ./cpp/build/release/app
...
openat(AT_FDCWD, "/tmp", O_RDWR|O_CLOEXEC|O_TMPFILE, 0600) = 3
write(3, "sysprobe scratch payload\n", 25) = 25
close(3)                                = 0
clock_nanosleep(CLOCK_REALTIME, 0, {tv_sec=0, tv_nsec=10000000}, 0x7ffecebd5fc0) = 0
getrandom("\xc3\x2e\x14\xc6...", 32, 0) = 32
write(1, "step=open ok\nstep=write ok\nstep="..., 77) = 77
```

Read that against the source and two "discrepancies" jump out, both
wrapper-layer fingerprints. The source says `nanosleep`; the trace says
`clock_nanosleep(CLOCK_REALTIME, …)` — glibc's `nanosleep()` is implemented
on the more general syscall. The source asks for 16 random bytes; the trace
shows one 32-byte `getrandom` — the vDSO key-seeding trap from earlier, after
which our request was served in user space. The Rust trace inverts both:
a raw `nanosleep({tv_sec=0, tv_nsec=10000000}, …)` and a literal
`getrandom("…", 16, 0) = 16` (plus `O_LARGEFILE` spelled out in its
`openat` flags — rustix passes explicitly what glibc implies). And Go, whose
`unix.Getrandom` now rides `runtime.vgetrandom`, shows a single 32-byte
`getrandom` for the whole process. One source-level contract, three
distinct boundary signatures — none of them wrong, all of them visible.

## Cross-check: counting every trap

Chapter 1 counted 64 syscalls for the C++ hello and 217 for Go's. `strace
-c` on the three `sysprobe` binaries (`-f` for Go) deepens that finding with
a full accounting; trimmed to the load-bearing rows from this session's
runs:

```
cpp  : 70 total  —  21 mmap, 6 openat, 6 close, 2 getrandom, 1 clock_nanosleep
go   : 283 total —  114 rt_sigaction, 61 nanosleep, 24 mmap, 11 futex,
                    10 sigaltstack, 4 clone, 1 getrandom
rust : 73 total  —  12 mmap, 5 openat, 6 write, 2 getrandom, 1 nanosleep
```

The program's own work is four steps in every case; the rest is runtime
overhead, and the ratio matches Chapter 1's almost exactly (roughly 4× for
Go, near-parity for C++ and Rust). C++'s 21 `mmap`s are the dynamic loader
mapping four libraries; Rust's 73 is C++-shaped because its *startup* still
goes through glibc even though its syscalls do not. Go's 283 is dominated by
runtime self-management — signals, threads, and its sleeping monitor. As an
independent surface, the `objdump` census (0, 511, and 32 embedded `syscall`
instructions) confirms *where* each binary keeps its boundary crossings:
nowhere (glibc's), everywhere (a static runtime's), and inline at 32 rustix
call sites. Two tools, one story.

<p><span class="status status--unverified">unverified</span> — <strong>On the lab VM:</strong> the kernel-side view of this chapter — watching <code>sys_enter_*</code> tracepoints fire with <code>bpftrace</code> as sysprobe runs, counting traps from inside the kernel instead of via ptrace — needs root and a disposable kernel, so it is not run here; chapter 30 (Debugging part) exercises exactly this on <code>systems-target</code>.</p>

## What you learned

- A syscall is a register protocol around one trapping instruction: number
  in `rax`, args in `rdi rsi rdx r10 r8 r9` (`r10` because `syscall`
  clobbers `rcx`), result or `-errno` back in `rax`.
- The vDSO answers hot calls without a trap — 1000 `clock_gettime` calls
  produced zero syscalls here, and on kernel 7.1/glibc 2.43 even
  `getrandom` is served in user space after one 32-byte key-seeding trap.
- The three languages take three roads to the same ABI — glibc wrappers
  (0 `syscall` instructions in the binary), the Go runtime's own stubs and
  ABI (511, static), rustix's inlined raw backend (32) — and each road
  leaves a distinct fingerprint in `strace`.
- `strace -c` puts numbers on runtime overhead: 70 (C++) vs 283 (Go, all
  threads) vs 73 (Rust) traps for four steps of real work — the same shape
  as Chapter 1's 64-vs-217, now fully attributed.

Next, **errors, three ways**: the `-errno` fact each wrapper just handed us
becomes a policy — `std::expected`, wrapped Go errors, and `thiserror`-style
Rust — that every later example in the book obeys.

---

<p><span class="status status--verified">verified</span> — all numbers in this chapter are from this session's runs on the Fedora 44 reference host (kernel <code>7.1.3-200.fc44</code>, glibc 2.43, strace 7.1): the harness reported <code>PASS</code> for cpp/go/rust and each binary printed the five-line block ending <code>sysprobe: 4 steps ok</code>; <code>strace -c</code> totaled 70 (cpp), 283 (go, <code>-cf</code>), and 73 (rust); the vdso-probe run showed 0 <code>clock_gettime</code> and 2 <code>getrandom</code> syscalls for 1000 calls each, including <code>getrandom("…", 32, 0) = 32</code>; the quoted sequence traces (<code>O_TMPFILE</code> openat, 25-byte write, <code>clock_nanosleep</code> vs raw <code>nanosleep</code>, Go's 20 µs-doubling monitor sleeps) are trimmed real output; <code>objdump</code> counted 0/511/32 <code>syscall</code> instructions; and <code>/proc/self/maps</code> showed the quoted <code>[vdso]</code>/<code>[vvar_vclock]</code> mappings. Not run here: the bpftrace kernel-side count (lab-VM callout above).</p>
