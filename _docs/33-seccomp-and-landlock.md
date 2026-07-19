---
title: "Seccomp and Landlock"
order: 33
part: "Containers and Virtualization"
description: "fwatch sandboxes itself with two independent, unprivileged kernel layers -- a seccomp-bpf syscall allowlist built from a real strace profile, and a Landlock ruleset that restricts filesystem reads to one directory -- probing the Landlock ABI at runtime instead of assuming it, and proving both with negative controls that watch the exact denial happen."
duration: "55 minutes"
---

Chapter 32 gave `fwatch` its own view of the process tree and its own
resource limits through namespaces and cgroup v2 -- isolation the kernel
grants to a group of processes. This chapter hands the same idea to a
*single* process, no grouping required: `watch --sandbox DIR` now installs
a syscall allowlist and a filesystem allowlist against itself, using two
mechanisms any unprivileged process can invoke directly. **seccomp-bpf**
decides, syscall by syscall, whether the calling process is allowed to make
this call at all. **Landlock** decides, path by path, which parts of the
filesystem that process may still read. Neither needs root, a namespace, or
a VM -- which is also why every number in this chapter was reproduced on
the plain host and confirmed unchanged on the `systems-target` lab guest.

The one new idea worth sitting with: seccomp-bpf is **classic BPF**, the
same in-kernel bytecode VM whose modern descendant, eBPF, this book has so
far only used as an *observation* tool (`bpftrace`, `bcc-tools`, `bpftool`
in Part 8). A seccomp filter is the mirror image -- you are not attaching a
program that watches a syscall and reports it, you are attaching a program
that *decides* the syscall's outcome, on the hot path, before the kernel
does the work. Same VM, same verifier discipline, opposite job.

The code is in `examples/33-seccomp-and-landlock/`. The run script there
builds/sets up and runs it; its `README.md` covers the CLI, the exit-code
contract, and the paired positive/negative verification in full.

{% include excalidraw.html
   file="33-seccomp-filter-flow"
   alt="Two stacked bands. The top userspace band shows fwatch issuing a syscall such as epoll_wait, read, or socket, beside a note that this is classic BPF -- you write the filter, Part 8's bpftrace only observes. The bottom kernel band shows the filter running on every syscall entry: a BPF_LD_ABS load of the syscall number and architecture, a compiled jump table of 26 entries for cpp and rust or 39 for go, a match arrow to SECCOMP_RET_ALLOW that returns control to the caller, and a no-match arrow down to the filter's default action. Two default-action boxes sit side by side: SCMP_ACT_ERRNO(EPERM), the one fwatch actually installs, where the process keeps running and sees EPERM on socket(2); and a dashed, unused SCMP_ACT_KILL_PROCESS box, where a separate probe program's getpid() call ends the whole process with SIGSYS. Footer notes report the real guest transcript: socket(AF_INET,...) = -1 EPERM under fwatch's filter, and the ACT_KILL_PROCESS probe's getpid() ending in killed by SIGSYS (core dumped)."
   caption="Figure 33.1 — a syscall's path through fwatch's seccomp-bpf filter: one cBPF program, run again on every entry, deciding ALLOW versus its default action" %}

> **Tools used** — `strace` (host and `systems-target` VM, to build and
> confirm the syscall profile), `cmake`+`ninja`/`go build`/`cargo` (the
> Python runner's build phase, host, all three toolchains), `ssh`/`scp`
> (via `scripts/test-all-examples.py --mode vm` and its staging step onto
> `systems-target`), a small ad hoc `gcc`-built C program (host and guest,
> to observe `SCMP_ACT_KILL_PROCESS`), `grep`/`/proc/<pid>/status` (both).
> Everything here ships with Fedora's toolchain or is preinstalled on the
> lab guest per `references/vm-lab.md`; nothing needs root.

## seccomp-bpf: the filter you write

`seccomp(2)` installs a cBPF program that the kernel runs on *every*
syscall the process makes from that point on. The program reads two fields
out of `struct seccomp_data` -- the architecture and the syscall number --
and returns one of a small set of verdicts: `SECCOMP_RET_ALLOW` (run it),
`SECCOMP_RET_ERRNO` (fail it with a chosen errno, without ever entering the
real syscall handler), or `SECCOMP_RET_KILL_PROCESS` (end the process with
`SIGSYS`, no return). Once a filter is loaded it can only ever be
*tightened* -- filters stack, and a later one can narrow what an earlier
one allowed but never widen it.

`fwatch`'s allowlist did not come from guessing. The comment in the C++
source says it directly: the syscall set is "empirically confirmed with
`strace -f` across a full watch session." That is the practical way to
build a seccomp profile -- run the real workload under `strace -f -c`,
read off the syscalls it actually issues, and allowlist exactly those.
Reproduced on the host against the unsandboxed watch loop:

```bash
[host]$ strace -f -c ./cpp/build/release/app watch /tmp/lsp33-profile --timeout-ms 2000
fwatch: watching /tmp/lsp33-profile
fwatch: exiting (timeout)
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ------------------
 37.74    0.000080           8        10           close
 13.21    0.000028           4         7           fstat
 12.74    0.000027           9         3           write
 11.79    0.000025           4         6           read
  9.43    0.000020          20         1           epoll_wait
  3.77    0.000008           2         3           epoll_ctl
  3.30    0.000007           7         1           inotify_add_watch
  2.36    0.000005           5         1           inotify_init1
  1.42    0.000003           3         1           timerfd_create
  1.42    0.000003           3         1           signalfd4
  1.42    0.000003           3         1           epoll_create1
  0.94    0.000002           2         1           timerfd_settime
  0.47    0.000001           1         1           rt_sigprocmask
  0.00    0.000000           0        22           mmap
  0.00    0.000000           0         7           mprotect
  0.00    0.000000           0         1           munmap
  0.00    0.000000           0         3           brk
  ...                                                 (dynamic-linker startup:
                                                        execve, openat, pread64,
                                                        access, arch_prctl, …)
------ ----------- ----------- --------- --------- ------------------
100.00    0.000212           2        87         1 total
```

That is exactly the source of `kWatchSyscalls` in `examples/33-seccomp-and-landlock/cpp/src/main.cpp`
(`read`/`write`/`close`, the inotify/timerfd/signalfd trio, epoll's three
calls, the allocator's `mmap`/`munmap`/`brk`, and `fstat`/`rt_sigprocmask`).
The rows elided above (`execve`, `openat`, `pread64`, `access`,
`arch_prctl`, `set_tid_address`, `set_robust_list`, `prlimit64`, `rseq`) are
the dynamic linker resolving `libc`/`libstdc++` *before* `main` runs --
real syscalls this same process makes, but not ones the watch loop itself
issues, so they are outside what the filter needs to allow once it loads
after startup.

`install_seccomp` (same file) calls `seccomp_init(SCMP_ACT_ERRNO(deny_errno))`
-- that argument is the filter's **default action**, applied to anything
not explicitly allowed -- then `seccomp_rule_add(ctx, SCMP_ACT_ALLOW, nr, 0)`
once per syscall, and `seccomp_load(ctx)` to install it. `seccomp_load`
requires `no_new_privs` first (set via `prctl`), a kernel guard against a
sandboxed process regaining privilege through a setuid binary it execs.

## `SCMP_ACT_ERRNO` versus `SCMP_ACT_KILL`

`fwatch` deliberately chose `SCMP_ACT_ERRNO(EPERM)`, not
`SCMP_ACT_KILL_PROCESS`, as its default action, and the difference is
observable, not cosmetic. With `ACT_ERRNO`, a denied syscall returns `-1`
with `errno` set and the calling code continues to run -- the process can
log the denial, clean up, and exit on its own terms; that is what makes
`probe --forbidden-syscall`'s own `EPERM`-and-continue behavior possible.
With `ACT_KILL_PROCESS`, a denied syscall never returns at all: the kernel
delivers an uncatchable `SIGSYS` and the process dies mid-instruction. To
see the contrast for real rather than assert it, a small standalone probe
(not part of `fwatch`, built only to observe this) installs a filter
allowing only `write`/`exit_group`/`rt_sigreturn` under
`SCMP_ACT_KILL_PROCESS`, then calls the unlisted `getpid()`:

```bash
[vm]$ strace -f -e trace=getpid /tmp/kill-demo 2>&1 | tail -3
before getpid
getpid()                                = 39
+++ killed by SIGSYS (core dumped) +++
```

`strace` shows the syscall entering the kernel and *never returning* --
the trailing line is `strace` reporting the process's death, not a normal
return value. `ACT_ERRNO` is the right choice whenever the caller has
useful cleanup to do or a human is watching the diagnostic; `ACT_KILL` is
the right choice when a denied syscall means the process's memory can no
longer be trusted (a common posture for hardened service sandboxes) and
continuing at all is the greater risk.

## Landlock: rulesets, rules, and an ABI you probe

Landlock restricts what a process (and everything it execs from here on)
may still do to the filesystem, expressed as three calls with no
convenience wrapper in any of the three languages' standard libraries --
`landlock_create_ruleset`, `landlock_add_rule`, `landlock_restrict_self`,
reached through a raw `syscall(2)` in C++ and Go and through the `landlock`
crate (which itself wraps the same three numbers) in Rust. A **ruleset**
declares *which access rights this program intends to handle at all* --
`fwatch` handles only `LANDLOCK_ACCESS_FS_READ_FILE` and `_READ_DIR`, and
leaves `handled_access_net` at zero. A **rule** then grants a subset of
those handled rights to one filesystem object, here a `PATH_BENEATH` rule
binding read rights to a directory fd. `restrict_self` is the point of no
return: it activates the ruleset against the *calling* process and every
descendant, and there is no syscall to lift it -- the only way out is
`execve`-ing a program that never asked for the restriction to begin with,
and even then the new program inherits the same ruleset.

The ABI matters here in a way it rarely does elsewhere in this book, and
the chapter's own two machines make the point better than any prose:

```bash
[host]$ ./cpp/build/release/app probe --sandbox /tmp --outside /etc/hostname
fwatch: landlock ABI=9 enforced
fwatch: probe outside /etc/hostname: EACCES (Permission denied)
[vm]$ /tmp/lsp33-cpp/app probe --sandbox /tmp --outside /etc/hostname
fwatch: landlock ABI=7 enforced
fwatch: probe outside /etc/hostname: EACCES (Permission denied)
```

Same distribution, same syscall numbers, and yet the host (kernel
`7.1.3-200.fc44.x86_64`) reports ABI 9 while `systems-target` (kernel
`6.19.10-300.fc44.x86_64`) reports ABI 7 -- two real Fedora 44 kernels a
few point releases apart, disagreeing about what Landlock supports, while
agreeing completely on the observable behavior the ruleset enforces. Every
call in this example reaches that number the only real way: calling
`landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` and
reading back the version the *running* kernel returns, never a compile-time
constant. A binary that assumed "ABI 9 because that's what built it" would
misbehave the moment it ran on the guest.

{% include excalidraw.html
   file="33-landlock-ruleset"
   alt="Two stacked bands. The top band builds the ruleset: landlock_create_ruleset called first with NULL/0/VERSION to probe the ABI, returning 9 on the host versus 7 on the systems-target guest despite both being Fedora 44 kernels; then landlock_create_ruleset(attr) with handled_access_fs set to READ_FILE and READ_DIR, feeding into landlock_add_rule binding a PATH_BENEATH rule to one directory's O_PATH file descriptor; then prctl(PR_SET_NO_NEW_PRIVS,1) feeding into landlock_restrict_self, marked irrevocable, with a note that Go's build also passes the TSYNC flag here to cover every OS thread already running. The bottom band shows the filesystem view after restrict_self: the sandboxed directory /tmp/lsp33-watch-sandbox with READ_FILE and READ_DIR still allowed and the watch loop working normally, beside /tmp/lsp33-outside-target/secret.txt where open(O_RDONLY) returns EACCES, with an arrow from the ruleset's restrict_self box down to each outcome, solid to the grant and dashed to the denial. A footer note across the bottom states that handled_access_net was left at zero in this ruleset, and that Landlock's TCP bind/connect rights have existed since ABI 4 and UDP rights only since ABI 10 -- so even at ABI 7 or 9 a Landlock-only sandbox does not cover the network by itself."
   caption="Figure 33.2 — the Landlock ruleset/rule model, with the ABI probed live rather than assumed and the real grant/deny split from this chapter's probes" %}

**What Landlock can and cannot do.** Filesystem mediation --
open/read/write/execute/rename, scoped to a path -- has been solid since
ABI 1 (Linux 5.13). Network mediation is newer and arrived in pieces:
`LANDLOCK_ACCESS_NET_BIND_TCP`/`_CONNECT_TCP` since ABI 4, but UDP
bind/connect rights not until ABI 10; `fwatch` targets neither, so on both
of these machines a Landlock-restricted process can still open arbitrary
sockets outside the tree entirely -- that gap is exactly why `fwatch`
layers seccomp *underneath* it rather than relying on Landlock alone.
IPC-scoping flags (restricting signals and abstract-socket connections
between sandboxed and unsandboxed processes) arrived at ABI 6, so both
machines here have them even though neither example uses them. The
practical rule this chapter teaches: **probe the ABI, then gate each
feature on the version number it actually needs** -- never gate on "did
`landlock_create_ruleset` return non-negative."

## How the code works

Both mechanisms share one setup shape across all three languages: probe
the Landlock ABI, build and enforce the ruleset, then install the seccomp
filter, each step logging a status line before the identical epoll watch
loop from chapter 09 runs unmodified. C++ and Rust do this **in-process**
with raw syscalls; Go does something structurally different, and the
concurrency lens below explains why.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Create a ruleset that handles READ_FILE + READ_DIR (available since ABI
// v1), add one path-beneath rule granting exactly those rights under `dir`,
// then restrict_self. On success the calling process — and everything it
// execs from here on — can only read inside `dir`.
[[nodiscard]] std::expected<void, std::error_code> apply_landlock(const std::string& dir) {
    constexpr std::uint64_t access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    landlock_ruleset_attr attr{.handled_access_fs = access, .handled_access_net = 0, .scoped = 0};

    const long rs = sys_landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (rs < 0) {
        return std::unexpected(last_error());
    }
    Fd ruleset{static_cast<int>(rs)};

    auto dirfd = checked(::open(dir.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!dirfd) {
        return std::unexpected(dirfd.error());
    }
    landlock_path_beneath_attr pb{.allowed_access = access, .parent_fd = dirfd->get()};
    if (sys_landlock_add_rule(ruleset.get(), LANDLOCK_RULE_PATH_BENEATH, &pb, 0) < 0) {
        return std::unexpected(last_error());
    }
    dirfd->reset();

    // Landlock requires no_new_privs (or CAP_SYS_ADMIN) before restrict_self.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        return std::unexpected(last_error());
    }
    if (sys_landlock_restrict_self(ruleset.get(), 0) < 0) {
        return std::unexpected(last_error());
    }
    return {};
}
```

```go
// installSeccomp loads an allowlisting filter with SECCOMP_FILTER_FLAG_TSYNC
// so every OS thread already running in this process is covered, not just
// the calling one. Returns the number of syscalls admitted.
func installSeccomp(allowed []uint32, denyErrno uint32) (int, error) {
	// seccomp(2) requires no_new_privs (or CAP_SYS_ADMIN), same as Landlock;
	// harmless to set again if applyLandlock already did.
	if err := unix.Prctl(unix.PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); err != nil {
		return 0, fmt.Errorf("prctl(PR_SET_NO_NEW_PRIVS): %w", err)
	}
	prog := buildAllowlist(allowed, denyErrno)
	fprog := sockFprog{Len: uint16(len(prog)), Filter: &prog[0]}
	_, _, errno := unix.Syscall(unix.SYS_SECCOMP, uintptr(unix.SECCOMP_SET_MODE_FILTER),
		uintptr(unix.SECCOMP_FILTER_FLAG_TSYNC), uintptr(unsafe.Pointer(&fprog)))
	if errno != 0 {
		return 0, errno
	}
	return len(allowed), nil
}
```

```rust
/// Installs an allowlist of `allowed` syscalls; everything else returns
/// errno `deny_errno`. Returns the number of syscalls admitted.
fn install_seccomp(allowed: &[i64], deny_errno: i32) -> Result<usize> {
    let rules = allowed.iter().map(|&nr| (nr, vec![])).collect();
    let filter = SeccompFilter::new(
        rules,
        SeccompAction::Errno(deny_errno as u32),
        SeccompAction::Allow,
        TargetArch::x86_64,
    )
    .context("seccomp: build filter")?;
    let program: BpfProgram = filter.try_into().context("seccomp: compile filter")?;
    seccompiler::apply_filter(&program).context("seccomp: install filter")?;
    Ok(allowed.len())
}
```

C++'s `apply_landlock` is the clearest read of the three-call shape: build
the `landlock_ruleset_attr` naming the *rights this program is willing to
have restricted at all* (`handled_access_fs`, leaving `handled_access_net`
at zero -- an explicit, auditable "we do not sandbox network here", not an
oversight), open the target directory `O_PATH`-only (no read/write access
needed just to name it as a rule target), bind the rule with
`landlock_add_rule`, and only then call `prctl(PR_SET_NO_NEW_PRIVS)` and
`landlock_restrict_self` -- in that order, because `restrict_self` demands
`no_new_privs` already be set. Rust's `landlock` crate (source in
`rust/src/main.rs`) compresses the same four calls into one builder chain
-- `Ruleset::default().handle_access(...).create()?.add_rule(...)?.restrict_self()?`
-- and adds one thing the raw syscall path does not give you for free: it
checks the returned `RulesetStatus`, so a kernel that silently
best-effort-ignores an unsupported right is caught (`bail!` if
`status.ruleset != RulesetStatus::FullyEnforced`) instead of quietly
running unsandboxed.

Go's `installSeccomp` is the odd one out for a reason worth reading
closely: `golang.org/x/sys/unix` exposes the raw `seccomp(2)` syscall
number but no BPF assembler, so `buildAllowlist` (same file) hand-emits the
cBPF program byte for byte -- `BPF_LD|BPF_W|BPF_ABS` to load the syscall
number, one `BPF_JMP|BPF_JEQ` comparison per allowed syscall that jumps to
`RET_ALLOW` on a match, and a fall-through `RET_ERRNO`. That is, in
miniature, exactly what `libseccomp` and `seccompiler` compile down to
underneath their friendlier APIs -- Go's version just makes the compiled
form visible because nothing hides it. The other detail to notice is
`SECCOMP_FILTER_FLAG_TSYNC` in the syscall arguments and its Landlock
counterpart in `applyLandlock`: both matter only because of what the Go
runtime does before `main` ever runs, covered next.

## Errors, three ways

Every failure path in this example distinguishes three outcomes, and the
distinction is what makes a negative control trustworthy rather than
decorative. A **setup failure** -- Landlock unsupported, `seccomp_load`
rejecting a malformed filter, a permission error opening the sandbox
directory -- is a real bug and exits `1` with the syscall name and
`strerror` text, identically formatted across languages (`SysErr`-style in
C++/Rust, a wrapped `error` in Go). A **confirmed denial** -- the sandbox
did exactly what it claims, `EPERM` on the forbidden syscall or `EACCES` on
the outside path -- exits `20`, `fwatch`'s own "probe passed" convention.
An **unconfirmed denial** -- the probed operation *succeeded* when it
should have been blocked -- exits `21`: a real sandboxing bug, distinct
from both a clean pass and an unrelated error. Reusing the ordinary `0/1/2`
contract for this would erase exactly the distinction the chapter is about:
a script that only checks "exit code is nonzero" cannot tell "the sandbox
worked" from "the sandbox is broken."

## Concurrency lens

Both Landlock and seccomp are, by default, **per-thread** kernel state --
they restrict the calling thread, not automatically every thread in the
process. That is invisible in C++ and Rust here because both builds are
deliberately single-threaded before the sandbox goes on: `main` calls
`apply_landlock` and `install_seccomp` before the epoll loop starts any
concurrency of its own, so "the calling thread" and "the whole process"
are the same thing.

Go cannot make that assumption, and its `main.go` says so in a comment
worth quoting directly: "the Go runtime schedules goroutines across OS
threads it creates and destroys on its own schedule (GC workers, sysmon,
threads parked in blocking syscalls) -- applying either sandbox to only the
calling thread would leave those other threads unrestricted, and a
goroutine the runtime later migrates onto one of them would silently run
outside the sandbox." The fix is two-part, mirroring the C++/Rust identity
lesson from chapter 14 almost exactly: `runtime.LockOSThread()` pins the
sandboxing goroutine to one OS thread so the thread issuing the Landlock
and seccomp syscalls never changes out from under it mid-sequence, and
both `landlock_restrict_self`'s `TSYNC` flag and `SECCOMP_FILTER_FLAG_TSYNC`
apply the new restriction to *every* thread already running in the process
atomically, not just the caller's. `applyLandlock` tries `TSYNC` first and
falls back to per-thread-only only if the running kernel rejects the flag
-- a second, smaller instance of "probe, don't assume." Confirmed
behaviorally, not just by reading the flag names: a goroutine the runtime
schedules onto a different OS thread *after* the sandbox is installed
still gets `EACCES`/`EPERM` on the restricted operations, because `TSYNC`
already covered that thread before it existed as a goroutine destination.

## Build, run, observe

```bash
[host]$ LIBVIRT_DEFAULT_URI=qemu:///system \
        python3 scripts/test-all-examples.py --only 33-seccomp-and-landlock --mode vm
```

Against `systems-target` this session (the build phase compiles cpp with
`cmake`+`ninja`, go with `go build`, and rust with `cargo build --release`
before any of the three ever touches the guest):

```
building 3 example-lang combinations (jobs=1)...
  build 33-seccomp-and-landlock [cpp]: ok
  build 33-seccomp-and-landlock [go]: ok
  build 33-seccomp-and-landlock [rust]: ok

verifying...
  verify 33-seccomp-and-landlock [cpp]: PASS
  verify 33-seccomp-and-landlock [go]: PASS
  verify 33-seccomp-and-landlock [rust]: PASS

example                  cpp   go    rust
33-seccomp-and-landlock  PASS  PASS  PASS

3 passed, 0 failed, 0 skipped (logs in build-logs/)
```

By hand, `run-sandbox-checks.sh` staged and run as root on the guest (the
book's convention for every namespace/cgroup/seccomp/Landlock demo, even
though neither mechanism here actually requires root):

```bash
[vm]$ sudo bash /tmp/lsp33-run-sandbox-checks.sh /tmp/lsp33-cpp/app
=== forbidden-syscall ===
fwatch: probe forbidden-syscall: EPERM (Operation not permitted)
fwatch: seccomp filter installed (26 syscalls allowed)
forbidden-syscall-exit=20
=== outside ===
fwatch: landlock ABI=7 enforced
fwatch: probe outside /tmp/lsp33-outside-target/secret.txt: EACCES (Permission denied)
outside-exit=20
=== watch-sandbox (positive control: watching inside the tree still works) ===
fwatch: landlock ABI=7 enforced
fwatch: seccomp filter installed (26 syscalls allowed)
fwatch: watching /tmp/lsp33-watch-sandbox
event: deleted inside.txt
fwatch: exiting (timeout)
watch-sandbox-exit=0
```

The third block is the control that makes the first two mean anything:
sandboxing that broke the watch loop itself would be worthless no matter
how convincingly it denied the two probes.

## Cross-check on `systems-target`

`strace` and `/proc` are independent of `fwatch`'s own exit-code claims,
so both are worth reading directly against the same guest run. First, the
denied syscall itself, traced from outside the process:

```bash
[vm]$ strace -f -e trace=socket,seccomp /tmp/lsp33-cpp/app probe --forbidden-syscall 2>&1 | tail -4
seccomp(SECCOMP_SET_MODE_FILTER, 0, {len=34, filter=0x26638170}) = 0
fwatch: seccomp filter installed (26 syscalls allowed)
socket(AF_INET, SOCK_STREAM, IPPROTO_IP) = -1 EPERM (Operation not permitted)
fwatch: probe forbidden-syscall: EPERM (Operation not permitted)
```

Second, the kernel's own record of the sandbox state, read from a
long-lived sandboxed process rather than a probe that exits immediately:

```bash
[vm]$ /tmp/lsp33-cpp/app watch --sandbox /tmp/lsp33-status-check --timeout-ms 3000 &
[vm]$ grep -i seccomp /proc/$!/status
Seccomp:	2
Seccomp_filters:	1
```

`Seccomp: 2` is `SECCOMP_MODE_FILTER` (mode 1 is the older, filter-less
"strict" mode; 0 is unsandboxed) -- `/proc` confirms the filter is live
independently of anything the process prints about itself.
`ip_unprivileged_port_start`-style sysctl shortcuts do not apply here since
neither mechanism has a global toggle to check against; the closest
equivalent negative control not shown above is simply repeating the
`outside` probe against a path *inside* the sandboxed tree, which the demo
script's third block already exercises implicitly by watching successfully
there.

## What you learned

- Seccomp-bpf is classic BPF running the opposite job from the eBPF tools
  in Part 8: a program you write decides each syscall's outcome
  (`SECCOMP_RET_ALLOW`/`_ERRNO`/`_KILL_PROCESS`) instead of merely
  observing it, built from a real `strace -f -c` profile of the workload,
  not a guess.
- `SCMP_ACT_ERRNO` lets a denied call return and the caller keep running;
  `SCMP_ACT_KILL_PROCESS` ends the process on `SIGSYS` with no return --
  confirmed by watching `strace` show `getpid()` never return in the
  kill-mode probe.
- Landlock's ruleset/rule/restrict_self model is layered access control you
  build yourself, and the ABI must be probed at runtime
  (`landlock_create_ruleset(NULL, 0, VERSION)`) rather than assumed -- this
  session's own host and `systems-target` guest returned different
  versions (9 vs 7) from the same distribution.
- Both mechanisms are per-thread by default; a single-threaded process
  never notices, but Go's always-threaded runtime needs
  `runtime.LockOSThread()` plus the `TSYNC` flags to cover threads the
  scheduler creates around the sandboxing call -- the same "identity is
  per-thread, not per-process" lesson chapter 14 taught for capabilities.

Next, **`examples/34-our-programs-in-containers`**: the same daemons this
book has built, running inside rootless Podman containers built from
multi-stage UBI 10 Containerfiles -- where seccomp and Landlock profiles
like this chapter's stop being something a program installs on itself and
start being something a container runtime installs *for* it.

---

<p><span class="status status--verified">verified</span> — every transcript
and number above was produced this session, reproduced on both the plain
host (kernel <code>7.1.3-200.fc44.x86_64</code>, libseccomp 2.6.0, GCC
16.1.1, Go 1.26.5, Rust/Cargo 1.97.1) and the <code>systems-target</code>
guest (kernel <code>6.19.10-300.fc44.x86_64</code>): the runner printed
<code>33-seccomp-and-landlock  PASS  PASS  PASS</code> (3 passed, 0 failed,
0 skipped) in vm mode; <code>run-sandbox-checks.sh</code> on the guest
produced <code>forbidden-syscall-exit=20</code> with the printed
<code>EPERM</code> line, <code>outside-exit=20</code> with the printed
<code>EACCES</code> line, and <code>watch-sandbox-exit=0</code> with an
<code>event: deleted inside.txt</code> line, byte-identical in shape across
cpp/go/rust (allowed-syscall counts differ: 26 for cpp/rust, 39 for go);
<code>strace</code> on the guest showed <code>socket(...) = -1 EPERM</code>
under the installed filter and, in a separate ad hoc
<code>SCMP_ACT_KILL_PROCESS</code> probe built for this chapter,
<code>getpid()</code> followed by <code>killed by SIGSYS (core dumped)</code>;
<code>/proc/&lt;pid&gt;/status</code> read <code>Seccomp: 2</code> with
<code>Seccomp_filters: 1</code> for a live sandboxed process; and the
Landlock ABI probe returned 9 on the host versus 7 on
<code>systems-target</code>, read live via
<code>landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)</code>
in every language, never hardcoded. Landlock's network-rights history
(TCP since ABI 4, UDP since ABI 10) and current crate/library pins
(<code>landlock</code> 0.4.5, <code>seccompiler</code> 0.5.0, libseccomp
2.6.1 as the latest upstream release) were confirmed against upstream
sources at authoring time and are worth re-checking before a future
release, as with any dependency pin in this book.</p>
