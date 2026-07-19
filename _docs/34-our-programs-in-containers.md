---
title: "Our programs in containers"
order: 34
part: "Containers and Virtualization"
description: "The same three daemons this book has built all along, now shipped as multi-stage UBI10 Containerfiles and run under rootless podman -- pid 1's signal and reaping duties, the GOMAXPROCS-from-cgroup story that catches C++'s hardware_concurrency() lying about its own container, debugging across the namespace boundary with podman exec/nsenter/gdb, and the seccomp default that quietly denies io_uring."
duration: "60 minutes"
---

Chapter 32 gave one of our programs its own namespaces and cgroup directly,
by calling `unshare`/`clone3` itself. Chapter 33 had `fwatch` sandbox itself
with seccomp and Landlock. This chapter hands both jobs to something else:
a container runtime. `app`, a tiny stand-in entrypoint built three times,
runs as `podman`'s idea of a container -- and that changes what "process 1"
means, what "how many CPUs do I have" means, and what a debugger needs to
cross to reach it. None of this is exotic: it is exactly the daemons this
book already writes, deployed the way most of them actually ship.

The code is in `examples/34-our-programs-in-containers/`. The run script
there builds/sets up and runs it; its `README.md` covers the CLI, the
output contract, and the full verification story.

{% include excalidraw.html
   file="34-image-layering"
   alt="Two side-by-side bands. The left, discarded builder stage starts from registry.access.redhat.com/ubi10/ubi:10.2 at 220 MB measured, runs dnf install of gcc-c++, golang, or rustup, and statically links the binary (cpp -static-libstdc++ -static-libgcc, go CGO_ENABLED=0, rust rustup 1.97.1). An amber COPY --from=builder arrow crosses into the right, shipped runtime stage, which starts from registry.access.redhat.com/ubi10-micro:10.2 at 24.3 MB measured, copies only the one binary to /usr/local/bin/app, and sets ENTRYPOINT/CMD. A footer note reports the real measured final image sizes -- cpp and go both 27.2 MB, rust 24.9 MB -- barely above the bare micro base, and explains that static linking is what makes copying one binary sufficient."
   caption="Figure 34.1 -- app's multi-stage Containerfile: the 220 MB builder never ships, only the binary crosses into the 24.3 MB runtime base" %}

> **Tools used** -- `podman` build/run/exec/inspect/unshare/stop (host,
> rootless), `nsenter` (host, both directly and via `podman unshare`),
> `gdb` (host, attached across the container's pid namespace), `ldd`/`file`
> (host, on binaries copied out of the built images), `python3`+`ctypes`
> (host, probing `io_uring_setup` under three seccomp postures). Everything
> here is checked by `scripts/check-host.sh`; podman ships with Fedora and
> needs no root.

## Multi-stage images: the builder never ships

A **Containerfile** -- podman's name for the same syntax Dockerfiles use --
can declare more than one `FROM`. Each `FROM` starts a fresh, unrelated
build stage; a later stage can `COPY --from=<earlier-stage>` specific files
out of it, and only the *last* stage's filesystem becomes the image podman
actually tags. `app`'s C++ build is the clearest read of the shape:

```dockerfile
FROM registry.access.redhat.com/ubi10/ubi:10.2 AS builder
RUN dnf -y install gcc-c++ cmake ninja-build libstdc++-static && dnf clean all
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY src ./src
RUN cmake --preset release \
      -DCMAKE_CXX_FLAGS="-static-libstdc++ -static-libgcc" \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
 && cmake --build --preset release

FROM registry.access.redhat.com/ubi10-micro:10.2 AS runtime
COPY --from=builder /src/build/release/app /usr/local/bin/app
ENTRYPOINT ["/usr/local/bin/app"]
CMD ["serve"]
```

The builder stage is `ubi10/ubi:10.2` -- measured on this host at 220 MB --
with a full `dnf` toolchain installed into it. The runtime stage is
`ubi10-micro:10.2`, measured at 24.3 MB: no shell, no package manager, only
`glibc` and the handful of libraries UBI's micro variant ships. `COPY
--from=builder` reaches back into the discarded stage for exactly one
file. Building all three languages' images fresh on this host: `lsp34-cpp`
and `lsp34-go` land at 27.2 MB, `lsp34-rust` at 24.9 MB -- a few MB above
the bare micro base, because the whole 220 MB builder is gone from the
final image.

That gap only closes because of the static-vs-dynamic linking recap
chapter 4 built with `sysprobe`: pulling the three languages' binaries back
out of these images with `podman cp` and running `ldd` on them tells the
same story `ldd` told there, sharpened by one deliberate choice. Go's
binary is `not a dynamic executable` -- `CGO_ENABLED=0` gives a fully
static binary, so the runtime stage needs nothing beyond the file itself.
Rust's binary still links `libgcc_s.so.1` and `libc.so.6` -- both already
present in `ubi10-micro`, so no extra `RUN` is needed. C++'s binary is the
interesting middle case: `ldd` lists `libm.so.6`, `libc.so.6`, and the vDSO,
but *not* `libstdc++.so.6` -- the Containerfile's `-static-libstdc++
-static-libgcc` flags embed exactly those two libraries into the binary,
which is what lets the runtime stage skip installing them. A plain,
dynamically-linked C++ build (chapter 4's default) would need `libstdc++`
copied or installed into the runtime stage too; this chapter's Containerfile
avoids that by choosing static linking specifically for the two libraries
the micro base does not carry. Rust's builder has its own wrinkle worth
naming: UBI10's `dnf` `rust` package is 1.92.0, older than this book's
pinned `rust-toolchain.toml` channel (1.97.1), so the builder installs the
exact toolchain via `rustup` instead of `dnf install rust`.

## Rootless podman: one user, one private uid range

Every container here runs **rootless** -- no `sudo`, no privileged daemon
-- which is possible because of a second Linux mechanism from chapter 32's
namespace tour: the **user namespace**. `podman` maps container uid 0 to
the invoking host user and a further range of container uids to a private,
per-user block of otherwise-unused host uids, declared in `/etc/subuid` and
`/etc/subgid`. On this host:

```bash
[host]$ grep "^$(whoami):" /etc/subuid /etc/subgid
/etc/subuid:rsedor:524288:65536
/etc/subgid:rsedor:524288:65536
```

That line grants `rsedor` a private range of 65,536 host uids starting at
524288 -- ids no other account owns. `podman unshare cat /proc/self/uid_map`,
run in podman's own user namespace, shows exactly how the two ranges
combine:

```bash
[host]$ podman unshare cat /proc/self/uid_map
         0       1000          1
     1     524288      65536
```

Container uid 0 (root, inside) maps to host uid 1000 (`rsedor`, outside) --
one id. Container uids 1 through 65536 map to the private subuid block.
The consequence worth sitting with: a process that is root *inside* the
container has, outside it, only the privileges of the ordinary host user
who launched podman -- root in the container cannot write to another
user's files, bind privileged host ports, or load kernel modules, because
outside the namespace it never had uid 0 to begin with. This is the same
"identity is a kernel-side mapping, not a label" lesson chapter 14 taught
for capabilities, applied to the uid itself: the mapping table, not the
number a `ps` inside the container prints, is what the kernel actually
checks.

## PID 1: signal duties naive skips

Every container's first process inherits a role most programs never
occupy: it is PID 1 of its own PID namespace. The kernel treats PID 1
specially -- a signal for which it has installed **no explicit handler**
is simply dropped instead of running that signal's default action. An
ordinary process ignoring `SIGTERM` still dies, because "terminate" is the
default action when no handler exists; PID 1 with no handler does not even
get that. Only `SIGKILL` and `SIGSTOP` still work. `app naive` demonstrates
this by installing nothing: it calls no `sigaction`, `signalfd`, or
`signal.Notify` at all before its heartbeat loop. `app serve` is the fix:
it blocks `SIGTERM`/`SIGINT`/`SIGCHLD`, receives them explicitly, stops
spawning new work, forwards `SIGTERM` to the worker process it supervises,
waits for that worker to exit, and only then exits itself -- so nothing is
left running or unreaped when the container stops.

{% include excalidraw.html
   file="34-pid1-signal-flow"
   alt="Two side-by-side bands contrasting serve and naive receiving SIGTERM as a container's PID 1. The serve band: podman stop sends SIGTERM, the signalfd/Notify/Signals wake fires, job_spawner.request_stop() runs, kill(worker, SIGTERM) plus waitpid/Wait() reaps it, a shutting-down line prints, and the process exits 0 -- measured at cpp 192ms, go 60 to 87ms, rust 101ms, all well inside a 3000ms grace budget. The naive band: podman stop sends SIGTERM into a kernel that finds no explicit handler on PID 1, so the signal is simply dropped for cpp and rust -- podman waits out the full grace period and falls back to SIGKILL, measured at cpp 2108ms and rust 2074ms. A muted box notes the Go exception: the Go runtime installs its own fatal-signal handler at startup regardless of user code, so naive still dies in about 60ms -- but with no shutting-down line and no cleanup, in every language."
   caption="Figure 34.2 -- serve's graceful pid-1 shutdown sequence beside naive's kernel-masked signal, with this session's real podman stop timings" %}

Reaping is the other pid-1 duty, and it splits into two real
implementation strategies across the three languages, because Go's and
Rust's runtimes are not safe to `fork()` once other threads or goroutines
exist. C++ uses a raw `fork(2)` for both the worker and each short-lived
job -- no `exec`, just a different function running in the child -- because
`app`'s C++ build is deliberately single-threaded up to that point, so the
forking thread and the whole process are the same thing. Go and Rust
instead **self-reexec**: `serve` resolves its own binary path
(`os.Executable()` / `std::env::current_exe()`) and starts `worker`/`job`
as genuinely separate processes running the same binary with a different
argument, then reaps each one with `Cmd.Wait()` / `Child::wait()` --
ordinary `wait4(2)` under the hood, one goroutine or thread parked per
child *is* the reap loop, no `SIGCHLD` involved anywhere in those two
builds.

## Container-aware CPU and memory detection

The headline trap: three ways of asking "how many CPUs do I have" give two
different answers under the same constraint. `podman run --cpus=2
--memory=128m` sets the container's cgroup v2 `cpu.max` to `200000
100000` (200ms of CPU time per 100ms period -- two cores' worth) and
`memory.max` to `134217728` bytes. Reading those files back from inside
each language's own process and reporting them alongside a
parallelism estimate is `app`'s first printed line:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
void print_container_line() {
    const unsigned n = std::thread::hardware_concurrency(); // THE TRAP: host cpus
    std::println("container: cpu.max={} effective_parallelism={} mem.max={}",
                 cpu_max_display(), n, mem_max_display());
}
```

```go
func printContainerLine() {
	fmt.Printf("container: cpu.max=%s effective_parallelism=%d mem.max=%s\n",
		cpuMaxDisplay(), runtime.GOMAXPROCS(0), memMaxDisplay())
}
```

```rust
fn print_container_line() {
    // Cgroup-aware, unlike the C++ version's hardware_concurrency().
    let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0);
    println!(
        "container: cpu.max={} effective_parallelism={} mem.max={}",
        cpu_max_display(),
        n,
        mem_max_display()
    );
}
```

All three share the same `cpu_max_display()`/`mem_max_display()` helpers
(not shown above -- they resolve `/proc/self/cgroup`'s `"0::<path>"` line
to find *this process's own* cgroup, then read `cpu.max`/`memory.max`
under `/sys/fs/cgroup` at that path, falling back to the root cgroup file
if the per-path one is missing). Those helpers agree completely across
languages, because they are just reading the same kernel files. The
`effective_parallelism` field is where they diverge, and the divergence is
real, not theoretical: run all three images with `--cpus=2` on this
16-core host,

```
cpp:  container: cpu.max=200000/100000 effective_parallelism=16 mem.max=134217728
go:   container: cpu.max=200000/100000 effective_parallelism=2  mem.max=134217728
rust: container: cpu.max=200000/100000 effective_parallelism=2  mem.max=134217728
```

`std::thread::hardware_concurrency()` is `sysconf(_SC_NPROCESSORS_ONLN)`
under the hood -- it asks how many CPUs the *host* has online and has no
concept of a cgroup, so it reports 16 regardless of `--cpus=2`. As of the
`go 1.26` language version pinned in `go.mod`, `runtime.GOMAXPROCS(0)`'s
*default* (unset by the environment or a prior call) divides the cgroup's
CPU bandwidth quota by its period whenever that is lower than the host's
core count, and re-checks periodically in case an orchestrator changes the
limit live -- confirmed here as `2`, matching the actual constraint
(`GODEBUG=containermaxprocs=0` opts back out, for anyone who needs the old
host-count behavior). Rust's `std::thread::available_parallelism()` on
Linux performs the same quota-over-period division against the calling
thread's affinity mask, landing on the same correct `2`. A C++ thread pool
sized off `hardware_concurrency()` in this container would spawn sixteen
threads to share two cores' worth of CPU time -- oversubscribed, thrashing
the scheduler, and never once reading the number that was actually true.

## How the code works

`serve`'s full startup, beyond the resource-detection line above, follows
one shape in all three languages: block/register the shutdown signals,
start the supervised worker, start a periodic job spawner, then block
until a signal arrives. C++ blocks `SIGTERM`/`SIGINT`/`SIGCHLD` with
`sigprocmask(SIG_BLOCK, ...)` and receives them as `signalfd(2)` reads
inside an ordinary `poll(2)` loop -- the same pattern chapter 12 built for
`pmon`, so no code ever runs in real signal-handler context. Go and Rust
skip that machinery entirely: Go's `signal.Notify` delivers onto a
channel, and Rust's `signal_hook::iterator::Signals` delivers as a
blocking iterator on the main thread; both are ordinary, safely-scheduled
code, not async-signal-restricted handlers, because neither language's
runtime lets you install a raw POSIX handler as directly as C does. The
`std::jthread` (C++) and background goroutine/thread (Go/Rust) that spawn
jobs every second all take the same shape: loop, sleep in short slices so
a stop request is noticed quickly, spawn one job, repeat -- and every one
is cooperatively stopped (`stop_token`, an atomic flag, a channel close)
before `serve` waits on the worker, so no job-spawn races the shutdown
sequence.

The one hardcoded assumption worth naming: `cpu_max_display()` only
handles cgroup v2's single-line `cpu.max` format (`"max"` or
`"<quota> <period>"`); a host still running cgroup v1 would need the older
`cpu.cfs_quota_us`/`cpu.cfs_period_us` pair instead, and none of these
builds falls back to that -- they print `"unknown"` from the empty-file
path instead of guessing.

## Errors, three ways

Setup failures -- a `fork`/`spawn` that fails, a `signalfd`/`Signals` that
cannot be installed -- exit `1` with the syscall name and its error text,
formatted identically across languages. Usage errors (an unrecognized
subcommand, missing arguments to `job`) exit `2` before anything is
spawned. There is no third "confirmed denial" exit code in this example
the way chapter 33's sandbox probes had one -- `app`'s only externally
visible contract is its stdout lines and its exit code on a clean
shutdown (`0`), and the *absence* of the `shutting down` line from
`naive`'s log is itself the evidence `verify.lua` checks, not a distinct
exit status. The subtler three-way split is behavioral, not code-based: a
`SIGTERM`-forwarded worker in C++ needs one line of insurance no other
language needs, covered next.

## Concurrency lens

The one real bug this example produced during development lived in C++'s
raw `fork(2)` path, and it is the same class of mistake chapter 14 warned
about for credentials: **signal masks are per-thread kernel state, and
`fork()` copies only the calling thread into the child.** `serve` blocks
`SIGTERM`/`SIGINT`/`SIGCHLD` with `sigprocmask(SIG_BLOCK, ...)` so it can
receive them via `signalfd`. The worker, forked from that same thread,
inherited the *blocked* mask -- so when `serve` later forwarded `SIGTERM`
to it with `kill(worker_pid, SIGTERM)`, the signal did not run its default
(terminate) action. It just sat **pending**, forever: the worker was
unkillable by anything short of `SIGKILL`, exactly the failure mode
`naive` demonstrates deliberately at the container's pid-1 layer, now
reproduced accidentally one process down. The fix is one line in the
child, immediately after `fork()`:

```cpp
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
```

which mirrors `pmon`'s reset-before-`execvp` from chapter 12 almost
exactly -- the difference is that there is no `exec` here to reset the
mask for free, so the reset has to be written explicitly. Go and Rust
never meet this bug at all, and not by luck: both languages avoid
`fork()`-without-`exec()` entirely, because forking a multithreaded
runtime (goroutine scheduler threads, Rust's allocator and async
executors) risks a forked child whose only surviving thread might be
holding a lock some other, vanished thread owned at the moment of the
fork. Self-reexec sidesteps the whole hazard class by never forking a
live multithreaded process in the first place -- the new process starts
clean, mask and all, from `execve`.

## Debugging across the namespace boundary

Three tools reach the same PID 1 from three different starting points.
`podman exec` is the easiest: it is already inside the container runtime's
view, so `podman exec <name> cat /proc/1/status` just works. Reaching the
same process from the **bare host** needs a different approach, because a
rootless container's namespaces are private to podman's own unprivileged
user namespace -- plain `nsenter` targeting the container's host-side pid
fails outright:

```bash
[host]$ nsenter --target "$HOSTPID" --pid --mount -- readlink /proc/1/exe
nsenter: reassociate to namespaces failed: Operation not permitted
```

`podman unshare` is the missing step: it re-executes its argument already
inside podman's rootless user namespace, and from there `nsenter` can join
the container's pid and mount namespaces on top:

```bash
[host]$ podman unshare nsenter --target "$HOSTPID" --pid --mount -- readlink /proc/1/exe
/usr/local/bin/app
```

{% include excalidraw.html
   file="34-ns-debugging-map"
   alt="A host band above a podman-user-namespace band. podman exec reaches the container PID 1 directly from inside the container runtime. podman unshare nsenter first joins podman's own user namespace, then nsenter --target --pid --mount reaches the same PID 1 from there -- both confirming Name: app and /proc/1/exe resolving to /usr/local/bin/app. A dashed, failing arrow shows plain nsenter without podman unshare returning EPERM: reassociate to namespaces failed. Below, real evidence boxes show the subuid mapping rsedor:524288:65536 and a live zombie scan of /proc/[0-9]*/stat reporting zero."
   caption="Figure 34.3 -- podman exec and podman unshare nsenter reach the same container PID 1 from two different starting namespaces; plain nsenter cannot" %}

A live `gdb` attach is the surprise worth reporting plainly: because
rootless podman's user namespace is *owned by the same host uid* that is
running `gdb`, ordinary ptrace permission checks succeed even with no
`podman unshare` step at all --

```bash
[host]$ gdb -batch -p "$HOSTPID" -ex "info proc exe" -ex detach -ex quit
warning: Target and debugger are in different PID namespaces; thread
lists and other data are likely unreliable.  Connect to gdbserver inside
the container.
exe = '/usr/local/bin/app'
```

`gdb` attaches and correctly resolves the executable, but its own warning
names the limitation precisely: it is reading `/proc/<hostpid>/*` from
*outside* the container's pid namespace, so anything that depends on pid
numbers matching what the container sees (`info threads`, breakpoints set
by a pid the container reports) is unreliable from here. A `gdbserver`
started with `podman exec` inside the container, with `gdb`'s `target
remote` reaching it from the host, is the reliable version of this same
cross-boundary attach -- the same shape chapter 28's remote-debugging
topology already covers.

## Performance and security trade-offs

Two costs are worth pricing accurately against what a container buys. The
**default seccomp profile** podman ships (`/usr/share/containers/seccomp.json`
on this host) declares `defaultAction: SCMP_ACT_ERRNO` with
`defaultErrnoRet: 38` (`ENOSYS`) for anything not on its allowlist -- and
`io_uring_setup`, `io_uring_enter`, and `io_uring_register` are not on it.
That is directly relevant after chapter 10: a program built to use
`io_uring` for its I/O will silently fail to set up a ring inside a
default-profile container. Probing the same call three ways on this host
makes the boundary observable rather than assumed:

```bash
[host]$ python3 -c "..."   # io_uring_setup(1, NULL) via ctypes.syscall(425, ...)
# bare host:                          -1 errno=14 (Bad address)        -- reaches the kernel
# default podman seccomp profile:     -1 errno=38 (Function not implemented) -- blocked before the kernel sees it
# --security-opt seccomp=unconfined:  -1 errno=14 (Bad address)        -- reaches the kernel again
```

`EFAULT` (14, "Bad address") is what the real kernel handler returns for a
null `params` pointer -- proof the call reached it. `ENOSYS` (38) under
the default profile is seccomp's own default-deny return, generated
without the syscall ever running; `seccomp=unconfined` removes the filter
and the exact same `EFAULT` reappears, isolating the profile as the cause.
Namespace and cgroup accounting add their own small, measured tax --
every `read`/`write` a namespaced process makes still walks the same
cgroup memory accounting chapter 32 introduced, and every `cpu.max`
lookup this chapter's own `app` performs is one extra `/proc`+`/sys` read
at startup, not a per-syscall cost. What a container buys in return is
concrete and already demonstrated above: one image that carries its own
static-linked dependencies, a private pid/mount/user namespace so a
compromised process cannot see or touch the rest of the host's process
tree or filesystem by default, and a resource ceiling the kernel enforces
whether or not the program inside ever asks about it. The next chapter's
`overheadbench` puts hard numbers on the remaining rungs of this same
ladder -- container versus full KVM guest versus bare host.

## Build, run, observe

```bash
[host]$ python3 scripts/test-all-examples.py --only 34-our-programs-in-containers --mode local
```

Against podman 5.8.4 rootless on this host (Fedora Linux 44, kernel
`7.1.3-200.fc44.x86_64`, 16 cpus), a fresh build/run/teardown of all three
languages:

```
building 3 example-lang combinations (jobs=1)...
  build 34-our-programs-in-containers [cpp]: ok
  build 34-our-programs-in-containers [go]: ok
  build 34-our-programs-in-containers [rust]: ok

verifying...
  verify 34-our-programs-in-containers [cpp]: PASS
  verify 34-our-programs-in-containers [go]: PASS
  verify 34-our-programs-in-containers [rust]: PASS

example                        cpp   go    rust
34-our-programs-in-containers  PASS  PASS  PASS

3 passed, 0 failed, 0 skipped (logs in build-logs/)
```

By hand, one consolidated run against the Go image, constrained the same
way `verify.lua` constrains it:

```bash
[host]$ podman run -d --name lsp34demo --cpus=2 --memory=128m lsp34-go serve
[host]$ podman logs lsp34demo | head -5
container: cpu.max=200000/100000 effective_parallelism=2 mem.max=134217728
app: pid=1 ppid=0
app: worker started pid=8
app: job pid=13 seq=1 done
app: reaped pid=13 status=0
[host]$ podman stop -t 3 lsp34demo   # elapsed_ms=87
[host]$ podman logs lsp34demo | tail -1
app: shutting down (SIGTERM)
```

The `naive` half of the trap, run the same way with a 2s grace instead of
3s, on all three images: `podman stop -t 2` measured `cpp` at 2108ms and
`rust` at 2074ms -- the full grace period, both `SIGKILL`ed -- while `go`
stopped in 60ms because its runtime installs its own fatal-`SIGTERM`
handler regardless of user code. Every one of the three logs, checked with
`grep -c "shutting down"`, reported zero: none of the three ever ran the
graceful path, which is exactly the point `naive` exists to make.

## Cross-check

`podman inspect` is independent of anything `app` prints about its own
limits, and it agrees with the container's self-report exactly:

```bash
[host]$ podman inspect lsp34demo --format \
    'CPUQuota={{.HostConfig.CpuQuota}} CPUPeriod={{.HostConfig.CpuPeriod}} Memory={{.HostConfig.Memory}}'
CPUQuota=200000 CPUPeriod=100000 Memory=134217728
```

`200000/100000` from `podman inspect` matches `app`'s own
`cpu.max=200000/100000` line, and `134217728` matches `mem.max=134217728`
-- two independent readers of the same cgroup files agreeing. The `SIGTERM`
grace behavior is its own cross-check in miniature: `serve`'s stop time
(87-192ms across languages) sits nowhere near the 3-second grace budget
`podman stop -t 3` allows, while `naive`'s cpp/rust stop time (2074-2108ms)
sits right at the 2-second grace budget `podman stop -t 2` allows before
`SIGKILL` -- the timing itself is the proof that one path ran a graceful
shutdown and the other waited out a timeout.

## What you learned

- A multi-stage Containerfile ships only its last stage; pairing a full
  `ubi10/ubi:10.2` builder (220 MB measured) with an `ubi10-micro:10.2`
  runtime (24.3 MB measured) and copying one static-linked binary across
  keeps the final image within a few MB of the bare runtime base.
- A container's PID 1 gets no default signal action for anything it has
  not explicitly handled -- `naive`'s absent handler is why `podman stop`
  has to wait out the full grace period and fall back to `SIGKILL`, and
  why `serve`'s explicit forward-then-reap sequence is not optional
  ceremony.
- "How many CPUs do I have" has three real answers under the same
  `--cpus=2` constraint: C++'s `hardware_concurrency()` reports the host's
  16, while Go's cgroup-aware `GOMAXPROCS` default and Rust's
  `available_parallelism()` both report the true 2 -- confirmed live on
  this host, not merely documented.
- `podman exec`, `podman unshare nsenter`, and even a direct `gdb -p`
  attach (because rootless podman's user namespace shares the host uid)
  all reach the same container PID 1 from different starting points, while
  the default seccomp profile silently denies `io_uring` calls that a
  chapter-10-style program might depend on.

Next, **virtualization and KVM**: the same daemons run a rung further out
-- inside a full guest kernel instead of sharing the host's -- and
`overheadbench` prices the difference in nanoseconds, gigabytes per
second, and operations per second, across the bare host, this chapter's
containers, and a real `systems-target` guest.

---

<p><span class="status status--verified">verified</span> -- every
transcript and number above was produced this session on this host
(Fedora Linux 44, kernel <code>7.1.3-200.fc44.x86_64</code>, 16 cpus,
podman 5.8.4 rootless): <code>python3 scripts/test-all-examples.py --only
34-our-programs-in-containers --mode local</code> printed
<code>34-our-programs-in-containers  PASS  PASS  PASS</code> (3 passed, 0
failed, 0 skipped); fresh <code>podman build</code>s measured
<code>ubi10/ubi:10.2</code> at 220 MB, <code>ubi10-micro:10.2</code> at
24.3 MB, and the three final images at cpp/go 27.2 MB and rust 24.9 MB;
<code>ldd</code> on binaries copied out of each image showed cpp linking
only <code>libm</code>/<code>libc</code> (no <code>libstdc++</code>), go
"not a dynamic executable", and rust linking only
<code>libgcc_s</code>/<code>libc</code>; a <code>--cpus=2 --memory=128m</code>
run reported <code>cpu.max=200000/100000</code> and
<code>mem.max=134217728</code> in all three languages with
<code>effective_parallelism=16</code> for cpp against <code>=2</code> for
go and rust, matching <code>podman inspect</code>'s
<code>CPUQuota=200000 CPUPeriod=100000 Memory=134217728</code>;
<code>podman exec</code>, <code>podman unshare nsenter</code>, and a live
<code>gdb -p</code> attach all reached the same container PID 1 (exe
<code>/usr/local/bin/app</code>), while plain <code>nsenter</code> failed
<code>EPERM</code>; measured <code>podman stop</code> times were serve
cpp 192ms/go 60-87ms/rust 101ms (well within a 3s grace) against naive cpp
2108ms/rust 2074ms (full 2s grace, SIGKILLed)/go 60ms (runtime exception),
with zero <code>shutting down</code> lines from any <code>naive</code>
log; and the <code>io_uring_setup</code> probe returned
<code>errno=14</code> (EFAULT) on the bare host and under
<code>seccomp=unconfined</code>, versus <code>errno=38</code> (ENOSYS,
matching <code>/usr/share/containers/seccomp.json</code>'s
<code>defaultErrnoRet</code>) under podman's default profile. Go 1.25/1.26's
container-aware <code>GOMAXPROCS</code> default and its
<code>GODEBUG=containermaxprocs=0</code> opt-out were additionally checked
against the upstream Go blog and release notes at authoring time.</p>
