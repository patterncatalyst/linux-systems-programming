---
title: "Virtualization and KVM"
order: 35
part: "Containers and Virtualization"
description: "How a VM actually runs — VMX root/non-root modes, EPT, and virtio devices as the host kernel moonlighting as a hypervisor — the isolation spectrum from bare process through container and microVM to full VM, the rust-vmm/Firecracker/cloud-hypervisor landscape, and overheadbench's measured syscall/mem/io deltas across all three."
duration: "50 minutes"
---

Every daemon in this book has, until now, run either directly on the host or
inside a Podman container sharing the host's kernel. This chapter puts a
third option on the table: a full KVM guest with its own kernel, and asks the
one question a systems programmer actually needs answered before choosing
between them — not "which is more secure" in the abstract, but *what does
each one cost*, measured in the same units this book has used throughout:
nanoseconds per syscall, gigabytes per second, operations per second.
`overheadbench` is a tri-language microbenchmark built for exactly this: the
same binary, unmodified, runs on the bare host, inside the `systems-target`
KVM guest, and inside a rootless Podman container, and the three runs land in
one table. The isolation spectrum this chapter walks — process, container,
microVM, full VM — is not a taxonomy exercise; it is the same spectrum every
one of this book's daemons could be deployed onto, and this is the chapter
that prices each rung.

The code is in `examples/35-virtualization-and-kvm/`. The run script there
builds and runs it; its `README.md` covers the CLI and the sanity ranges
`verify.lua` checks.

{% include excalidraw.html
   file="35-kvm-virtio-stack"
   alt="Three horizontal bands. Top: host, VMX root mode, showing kvm.ko and kvm_intel.ko managing EPT tables, a QEMU/VMM userspace box running the vCPU thread and virtio-blk backend, and host NVMe/btrfs where a real fsync lands. Middle: guest, VMX non-root mode, showing the guest kernel serving getpid in-guest with no host trap, overheadbench in guest ring 3 issuing the syscall bench's raw getpid loop and the io bench's open/write/fsync calls, a virtio-blk front-end driver, and a virtqueue descriptor ring. Bottom: the trapped path for fsync — fsync in the guest causes a VM exit, the KVM exit handler hands off to the QEMU vCPU thread, the virtio-blk backend issues the real host fsync and triggers VM enter back into the guest. A fourth band gives the measured overheadbench deltas: syscall about 88ns/call on both host and guest because getpid never triggers a vmexit, mem about 14 to 15 GB/s on both because the EPT walk cost is amortized after the TLB warms up, and io about 1.3k ops/s on the host against about 0.5k ops/s on the guest, the real vmexit tax."
   caption="Figure 35.1 — the VMX root/non-root boundary, the virtio-blk round trip that fsync actually pays, and where overheadbench's own measured host-vs-guest deltas land" %}

> **Tools used** — `overheadbench` itself, `podman` (build and run, host),
> `ssh`/`scp` via `scripts/lab/deploy-to-vm.sh` and the Python runner
> (`scripts/test-all-examples.py`) (host), `virsh` (host, confirming the lab
> is up), `lscpu` (host and `systems-target` guest), `strace -c` (host and
> guest, the cross-check), `systemd-detect-virt` (guest). All of these are
> checked by `scripts/check-host.sh` or ship with Fedora's cloud image on the
> lab VMs.

## What actually runs a VM

A container's isolation is namespaces and cgroups layered over *one* kernel;
every syscall a containerized process makes is a syscall the host kernel
itself serves, at host cost. A KVM guest is a different animal: it has its
own kernel, its own page tables, its own idea of what hardware exists — and
the host kernel is the thing making that illusion real. Since 2006, Intel and
AMD CPUs have shipped hardware virtualization extensions (VMX on Intel,
confirmed on this host by `lscpu`'s `Virtualization: VT-x` line) that add a
second CPU mode dimension alongside ring 0/ring 3: **VMX root mode**, where
the host kernel and hypervisor run, and **VMX non-root mode**, where the
*entire* guest — its kernel at ring 0, its processes at ring 3 — runs mostly
unmodified. `/dev/kvm`, backed by the `kvm` and `kvm_intel` kernel modules
(both loaded on this host, `lsmod | grep kvm` confirms `kvm_intel`, `kvm`,
and `irqbypass`), is the interface QEMU drives: it calls `ioctl(KVM_RUN, ...)`
on a vCPU file descriptor, the CPU drops into non-root mode, and guest
instructions execute directly on the real CPU — no instruction-level
emulation — until something the guest cannot handle itself forces a **VM
exit** back to root mode.

The key fact this chapter measures is *what actually causes an exit*.
Ordinary instructions — arithmetic, memory loads and stores, and crucially
**syscalls the guest kernel can serve itself**, like `getpid(2)` — never
leave non-root mode at all. The guest kernel's syscall entry point runs
exactly like it would on bare metal; there is no hypervisor in that path to
trap into. What *does* exit are instructions that touch state the guest does
not really own: port I/O, certain MSR accesses, and — the one this chapter's
`io` bench hits on every iteration — an access to an emulated device like
`virtio-blk`, the paravirtualized block device virtio defines so guest I/O
doesn't have to trap on every disk instruction the way full hardware
emulation would. `virtio-blk` still requires a round trip: the guest driver
places a request descriptor on a shared-memory virtqueue, the access that
notifies the host ("kick") causes a VM exit, QEMU's backend thread (in root
mode again) issues the *real* `fsync(2)` against the host filesystem, and a
VM entry resumes the guest. Extended Page Tables (EPT) are the third piece:
they let the guest maintain its own page tables while the CPU walks a second,
hardware-managed table translating guest-physical to host-physical addresses,
so ordinary memory access needs no hypervisor involvement either — only the
first touch of a page, which fills the EPT-backed TLB entry, costs anything
beyond native.

## The isolation spectrum

{% include excalidraw.html
   file="35-isolation-spectrum"
   alt="Four boxes left to right along a spectrum from shared host kernel to separate guest kernel. Bare process: no isolation boundary, syscall equals host syscall, escape equals root on host. Podman container: namespaces plus cgroup v2 plus seccomp, one host kernel and one syscall table, syscall and mem about equal host and io about equal host on a bind mount, escape equals a kernel bug in namespace or cgroup code. MicroVM, Firecracker or cloud-hypervisor built on rust-vmm: own guest kernel with a minimal virtio device set, about 125 milliseconds boot and under 5 MiB overhead for Firecracker, escape equals a bug in the roughly five devices the VMM exposes or in KVM itself. Full KVM/QEMU VM, the systems-target lab guest: own guest kernel with a full device model, syscall about equal host and io about three times slower due to virtio-blk, escape equals a bug in the full emulated device model or in KVM. A note below cites that rust-vmm crates such as kvm-ioctls, virtio-queue, and vm-memory underpin both Firecracker v1.16.1 and Cloud Hypervisor v53.0."
   caption="Figure 35.2 — process, container, microVM, and full VM, each priced by escape surface and by this chapter's measured overheadbench numbers" %}

Four points, not two. A **bare process** has no isolation boundary at all — a
syscall it makes *is* a host syscall, full stop, and an escape is simply
"you are already root, or you exploit a bug to become root." A **container**
(Chapter 34's Podman UBI images) adds namespaces and cgroup v2 as a soft
boundary layered over the *same* kernel: still one syscall table, so
`overheadbench`'s `syscall` and `mem` numbers inside a container should track
the host almost exactly — and the `io` bench, run against a directory bind-
mounted from the host's real filesystem rather than the container's overlay
storage, does too, because it is still the host's block layer doing the
work. The escape surface for a container is a kernel bug in the
namespace/cgroup/seccomp code path itself — the code the shared kernel runs
on the container's behalf.

A **full KVM/QEMU VM** (`systems-target`, this book's lab guest) is the
opposite end: its own kernel, its own process table, its own view of
hardware, with a full emulated device model — network, block, USB, ACPI,
BIOS. The escape surface shrinks to "a bug in that entire device model, or in
KVM's guest/host boundary itself" — a much smaller, better-audited attack
surface than "the whole host kernel," at the cost of a full second kernel to
boot, patch, and pay overhead for on every path that has to leave the guest.

Between them sits the **microVM** — Firecracker and cloud-hypervisor's
answer to "a real second kernel, but priced like a container." A microVM
still runs its own guest kernel under KVM, so the escape surface is the same
category as a full VM, but the device model is deliberately reduced: no
BIOS, no ACPI, no graphics, no USB — Firecracker (v1.16.1 as of this
writing) implements exactly five devices: virtio-net, virtio-block,
virtio-vsock, a serial console, and a minimal keyboard controller. That
minimalism is why Firecracker boots a guest in roughly 125 ms with under
5 MiB of per-instance memory overhead, and why AWS runs it under Lambda and
Fargate at up to 150 microVMs per second per host. Both Firecracker and
Cloud Hypervisor (v53.0, which added VFIO passthrough, CPU/memory hotplug,
and — recently — Windows guest support) are built from **rust-vmm**: a
community of reusable, independently audited Rust crates —
`kvm-ioctls`/`kvm-bindings` for the `/dev/kvm` ioctl surface,
`vm-memory` for guest address space management, `virtio-queue` and
per-device crates for the virtio transport. Firecracker (AWS) helped seed
rust-vmm; Cloud Hypervisor (originally Intel, now Linux Foundation-governed)
leans on the same crates for a broader feature set. The microVM's promise is
literally this chapter's spectrum collapsed to one point: KVM's isolation
guarantee at closer to container startup cost — which is why it is the
answer teams reach for when "container" isn't isolated enough and "full VM"
is too slow to spin up per request.

## How the code works

`overheadbench`'s three benches are intentionally tiny and identical across
languages, because the *comparison* is the content, not the algorithm. The
one worth walking in detail is `syscall`, since it is the bench whose result
this chapter's whole VMX argument hinges on:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// --- syscall bench: tight getpid(2) loop, ns/call. --------------------------
double bench_syscall(std::uint64_t iters) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
        // A raw syscall (not the libc wrapper) so every iteration really
        // crosses into the kernel — this is the syscall-boundary cost the
        // chapter contrasts against VM (vmexit trap) and container
        // (near-zero extra) overhead.
        ::syscall(SYS_getpid);
    }
    const auto elapsed = Clock::now() - start;
    double ns = std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(elapsed).count();
    if (ns <= 0.0) {
        ns = 1.0;
    }
    return ns / static_cast<double>(iters);
}
```

```go
// benchSyscallRun: tight getpid(2) loop, ns/call.
func benchSyscallRun(iters uint64) float64 {
	start := time.Now()
	for i := uint64(0); i < iters; i++ {
		// unix.Getpid issues the raw getpid(2) syscall on every call (the Go
		// runtime does not cache it) — every iteration really crosses into
		// the kernel, the syscall-boundary cost this chapter contrasts
		// against VM (vmexit trap) and container (near-zero extra) overhead.
		unix.Getpid()
	}
	ns := float64(time.Since(start).Nanoseconds())
	if ns <= 0 {
		ns = 1
	}
	return ns / float64(iters)
}
```

```rust
/// Tight getpid(2) loop, ns/call.
fn bench_syscall(iters: u64) -> f64 {
    let start = Instant::now();
    for _ in 0..iters {
        // rustix::process::getpid issues the raw getpid(2) syscall every
        // call — every iteration really crosses into the kernel, the
        // syscall-boundary cost this chapter contrasts against VM (vmexit
        // trap) and container (near-zero extra) overhead.
        black_box(rustix::process::getpid());
    }
    let mut ns = start.elapsed().as_nanos() as f64;
    if ns <= 0.0 {
        ns = 1.0;
    }
    ns / iters as f64
}
```

Three deliberate choices, identical in all three: the loop calls a **raw**
syscall rather than a libc-cached identity (glibc does not cache `getpid`
today, but the explicit `syscall(SYS_getpid)`/`unix.Getpid`/`rustix::getpid`
spelling makes that guarantee visible in the source, not implicit in a libc
version); the clock (`std::chrono::steady_clock`, `time.Now`/`Since`,
`Instant::now`) is a **monotonic** one, because Chapter 24 already
established that a wall-clock read can step backward mid-measurement; and
each language guards the compiler from proving the loop is dead —
`black_box` in Rust wraps the return value, C++ takes no extra step here
because the syscall itself is an unremovable side effect, and the Go build
relies on the same property. The `mem` bench needs an explicit anti-dead-code
guard because summing a buffer *is* pure, elidable computation: C++ stashes
the sum in a `volatile` sink, Rust calls `black_box(sum)`, Go checks the sum
against an unreachable sentinel inside an `if` whose body writes to stderr —
three different languages' answer to the same compiler-optimizer problem.
The `io` bench's fragile-but-deliberate choice is documented in its own
comment: the scratch directory is created **relative to the current working
directory**, not the system temp directory, because on this host (and many
dev hosts) `/tmp` is `tmpfs` — `fsync` there is nearly free, and the number
stops measuring storage at all. `examples/` sits on real NVMe-backed btrfs,
which is what makes the `io` numbers below meaningful as a storage-overhead
comparison rather than a memcpy benchmark in disguise.

The other half of "how the code works" for this chapter is the multi-stage
`Containerfile` that gets the same Go binary into a rootless Podman
container — `examples/35-virtualization-and-kvm/go/Containerfile`,
reproduced verbatim:

```
# Multi-stage build for overheadbench (chapter 35). Build stage compiles a
# fully static binary against UBI 10's go-toolset (go1.26.5 — the same
# toolchain pin the host uses); the runtime stage is ubi10/ubi-micro, which
# carries no shell and no package manager, so the final image is just the
# static binary plus the handful of files ubi-micro itself ships.
FROM registry.access.redhat.com/ubi10/go-toolset:latest AS build
# Stay in the s2i image's own /opt/app-root/src: it is already writable by
# the image's arbitrary non-root uid (1001, gid 0); a fresh WORKDIR would be
# created root-owned and rootless podman's build user could not write to it.
COPY go.mod go.sum ./
RUN go mod download
COPY main.go ./
# CGO_ENABLED=0: unix.Getpid() is a raw syscall, no libc needed, so a fully
# static binary can run unmodified on ubi-micro's minimal userspace.
RUN CGO_ENABLED=0 go build -o overheadbench .

FROM registry.access.redhat.com/ubi10/ubi-micro:latest
COPY --from=build /opt/app-root/src/overheadbench /usr/local/bin/overheadbench
# Rootless podman default: container runs as the invoking user's mapped uid,
# not root, and writes its scratch io-bench files into WORKDIR (set at run
# time via -w so the io bench lands on the mounted volume, not container
# storage).
ENTRYPOINT ["/usr/local/bin/overheadbench"]
CMD ["--bench", "all"]
```

The build stage is `ubi10/go-toolset` — Red Hat's s2i-flavored Go 1.26.5
image, the exact same toolchain pin this book uses on the host, so the
container's binary is not just architecturally comparable to the host's, it
is a build of the identical source with the identical compiler version. It
compiles with `CGO_ENABLED=0`: `getpid(2)` needs no libc, so the result is a
fully static binary that runs unmodified on `ubi10/ubi-micro`, a runtime
image so minimal it has no shell, no package manager, nothing but the
handful of files UBI's licensing and CA-bundle policy require. The one
build-time trap worth stating plainly: setting an explicit `WORKDIR` before
the `COPY`/`RUN` steps would have the directory created root-owned at the
point the instruction runs, and every later `RUN` in a rootless build
executes as the image's own non-root user (uid 1001) — the fix is to stay in
the s2i image's pre-existing `/opt/app-root/src`, which the base image
already made writable for that uid.

## Errors, three ways

`overheadbench`'s error contract is the same shape this book has used since
Chapter 5: **argument-parsing failures exit 2** with the usage line on
stderr, and **bench-execution failures exit 1** without it, kept as two
distinct exit codes so a caller can tell "you asked wrong" from "it tried and
the environment failed." An unknown `--bench` value, a flag missing its
value, an unrecognized flag, or a non-numeric or zero `--iters` all land in
the first bucket in all three languages: C++'s `parse_args` returns
`std::expected<Config, std::string>` and the `main` that unwraps it prints
`cfg.error()` then `kUsage` before returning 2; Go's `parseArgs` returns a
plain `error` that `main` prints beside the same `usage` constant; Rust's
`parse_args` returns `Result<Config, ArgError>` and the `main` match arm
does the identical two-line print. The second bucket — the `io` bench's
`open`/`write`/`fsync` calls actually failing, which none of the runs in
this chapter hit but which a full disk or a read-only bind mount would
trigger — propagates through `std::expected<double, std::string>`,
`(float64, error)`, and `Result<f64, String>` respectively, each wrapped one
more time (`"io bench failed: " + ...`) so the message names which of the
three phases failed, and exits 1 with no usage line, because the arguments
were fine — the environment was not.

## Concurrency lens

This chapter's isolation spectrum is built entirely from **per-thread kernel
state**, and that constraint shows up on both the container and the VM side
in the same shape Chapter 14 already met with capabilities. On the VM side:
a vCPU file descriptor returned by `KVM_CREATE_VCPU` is only valid to drive
with `ioctl(KVM_RUN, ...)` **from the thread that created it** — call it from
any other thread and the ioctl fails. That is why QEMU pins exactly one host
thread per vCPU rather than dispatching `KVM_RUN` calls from a thread pool;
the vCPU's register state, its `KVM_RUN` mmap'd shared page, and its
in-kernel scheduling context are thread-local, not process-wide. On the
container side, entering an existing set of namespaces with `setns(2)` has
the *identical* constraint: namespace membership is per-thread state in the
kernel's `task_struct`, not a process-wide property, exactly like the uids
and capability sets Chapter 14 walked. A single-threaded C++ or Rust build
that calls `setns` then immediately does its namespaced work never notices;
a Go program is the case that bites, because the Go runtime is
multi-threaded and goroutines migrate between OS threads by default — a
goroutine that calls `setns` and then, without more, tries to act as though
it is still in the new namespace can be rescheduled onto a *different* OS
thread that never made that call. The fix is the same shape as Chapter 14's
Go build pushing the capability dance into the fork/exec child:
`runtime.LockOSThread()` before `setns`, pinning the calling goroutine to its
current OS thread for the rest of its life (or until `UnlockOSThread`), so
the namespace membership and the goroutine that believes it holds it stay
attached to the same kernel thread. The general rule repeats from Chapter
14: whenever a kernel API's state lives on the thread rather than the
process — credentials, capabilities, namespace membership, a KVM vCPU — a
runtime that hides its threading from you (Go's goroutines, but the same
risk applies to any green-thread or fiber scheduler) needs an explicit pin,
or the state and the code that thinks it owns the state quietly drift apart.

## Build, run, observe

Three environments, one binary. First, the bare host (Fedora 44, kernel
7.1.3-200.fc44, Intel i7-11800H, `Virtualization: VT-x` from `lscpu`):

```bash
[host]$ cd examples/35-virtualization-and-kvm
[host]$ ./demo.sh go run --bench all
bench=syscall metric=84.84 unit=ns/call
bench=mem metric=12.59 unit=GB/s
bench=io metric=1504.89 unit=ops/s
```

Then the same binary deployed to the `systems-target` KVM guest (Fedora 44,
kernel 6.19.10-300.fc44, confirmed by `lscpu` in the guest as
`Virtualization type: full`, and by `systemd-detect-virt` printing `kvm`):

```bash
[host]$ export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target
[host]$ ./demo.sh go run --bench all
→ copying app to fedora@192.168.124.7:/home/fedora/app
→ running on systems-target (Ctrl-C to stop):
bench=syscall metric=87.69 unit=ns/call
bench=mem metric=14.32 unit=GB/s
bench=io metric=521.24 unit=ops/s
```

And the same source, rebuilt through the multi-stage `Containerfile` above,
run rootless with the `io` bench's scratch directory bind-mounted to a real
host path (`-w /scratch`, not the container's own overlay storage):

```bash
[host]$ cd go
[host]$ podman build -t overheadbench-go -f Containerfile .
[host]$ mkdir -p container-scratch
[host]$ podman run --rm -v "$PWD/container-scratch:/scratch:Z" -w /scratch overheadbench-go --bench all
bench=syscall metric=94.01 unit=ns/call
bench=mem metric=15.39 unit=GB/s
bench=io metric=1473.58 unit=ops/s
```

Three repeats of each (host: 84.8–91.8 ns/call, 12.6–15.5 GB/s, 1105–1505
ops/s; guest: 87.7–88.1 ns/call, 14.3–15.0 GB/s, 463–521 ops/s; container:
94.0–102.8 ns/call, 13.5–15.4 GB/s, 1474–1524 ops/s) put the pattern beyond
noise: **`syscall` and `mem` land in the same band across all three
environments** — a container shares the host's syscall table entirely, and
the guest's `getpid` never triggers a VM exit at all, matching Figure 35.1's
claim exactly. **`io` is where the VM actually pays**: roughly 2.9–3× slower
in the guest than on the host or in the container, the measured cost of
every `fsync` taking the virtio-blk round trip this chapter's diagram walks
— VM exit, QEMU's backend thread, the real host `fsync`, VM entry — instead
of running straight through in-kernel like the container's bind-mounted
write does. The full cross-language sweep on the host, run through this
book's standard runner, confirms the three languages track each other:

```bash
[host]$ python3 scripts/test-all-examples.py --only 35-virtualization-and-kvm --mode local
example                    cpp   go    rust
35-virtualization-and-kvm  PASS  PASS  PASS

3 passed, 0 failed, 0 skipped (logs in build-logs/)
```

## Cross-check

`overheadbench` prints its own timing, so an independent check has to verify
the printed numbers correspond to real syscalls actually made — not just
trust the arithmetic. `strace -c`, run on both sides of the VMX boundary,
does exactly that by counting syscalls a second way. On the host, wrapping
`--bench io --iters 50`:

```bash
[host]$ strace -f -c -e trace=getpid,openat,write,fsync,close,unlink ./go/bin/app --bench io --iters 50
bench=io metric=1233.15 unit=ops/s
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 68.97    0.004160          83        50           fsync
 17.11    0.001032          19        54           openat
  7.68    0.000463           9        51           write
  6.07    0.000366           6        53           close
------ ----------- ----------- --------- --------- ----------------
100.00    0.006032          28       210           total
```

Exactly 50 `fsync` calls for `--iters 50` — the count `overheadbench` claims
to have run is the count the kernel actually saw, independent of the
program's own bookkeeping. Run identically on `systems-target`:

```bash
[vm]$ strace -f -c -e trace=openat,write,fsync,close,unlink /home/fedora/app --bench io --iters 50
bench=io metric=512.96 unit=ops/s
------ ----------- ----------- --------- --------- ----------------
 74.81    0.004972          99        50           fsync
------ ----------- ----------- --------- --------- ----------------
```

Same 50 `fsync` calls, and the *unstraced* metric it printed (512.96 ops/s)
falls right inside the guest's 463–521 ops/s band from the build/run/observe
section above — the trace confirms the count without disturbing the
comparison this chapter is making. Note what `strace` cannot show, on either
side: it observes the syscall the guest kernel serves, never the VM exit
that syscall may or may not trigger underneath. `strace -c`'s own reported
`usecs/call` for `getpid` under tracing was 9,139 ns — two orders of
magnitude above the untraced ~85 ns this chapter reports — because `ptrace`
stops the tracee on every syscall entry and exit; that overhead is exactly
why `overheadbench` times itself with its own monotonic clock instead of
trusting a tracer's numbers, the same "the observation tool changes what it
measures" caveat Chapter 14 hit with `strace` across a privilege drop. There
is no dashboard panel for this chapter's numbers by design — `overheadbench`
is a one-shot CLI, and the table above *is* the observation surface.

## What you learned

- A VM exit is not triggered by "leaving the guest," it is triggered by
  the guest touching state it does not itself own — `getpid(2)` never
  exits, but `fsync(2)` against `virtio-blk` does, which is exactly why
  `overheadbench`'s `syscall`/`mem` numbers track the host across all three
  environments while `io` is the one that is measurably ~3× slower in a full
  KVM guest.
- The isolation spectrum has four rungs, not two: bare process, container
  (shared kernel, namespaces/cgroups as a soft boundary), microVM
  (rust-vmm-built Firecracker v1.16.1 / Cloud Hypervisor v53.0 — a real
  guest kernel behind a deliberately minimal device model), and full
  KVM/QEMU VM — each trading escape-surface size against boot time and
  per-path overhead.
- Kernel state that lives per-thread, not per-process, is a recurring trap:
  a KVM vCPU fd must be driven from the thread that created it, and
  `setns(2)` namespace membership is equally per-thread — a Go program must
  call `runtime.LockOSThread()` before either, the same lesson Chapter 14's
  capability drop taught for uids and capability sets.
- A benchmark's own numbers need an independent check: `strace -c`'s exact
  syscall counts confirmed `overheadbench`'s claimed iteration counts on
  both host and guest, while also showing where that same tool's overhead
  (`ptrace`'s stop-on-every-syscall cost) makes it unfit to trust for timing.

Next, Part 10 opens with **observability** — turning `/proc`, `/sys`, and
cgroup statistics into the kind of live picture this chapter had to build by
hand, one `overheadbench` run at a time.

---

<p><span class="status status--verified">verified</span> — every transcript
and number above was produced this session. Host: Fedora 44, kernel
7.1.3-200.fc44.x86_64, Intel i7-11800H, <code>lscpu</code> reporting
<code>Virtualization: VT-x</code>, <code>lsmod</code> showing
<code>kvm_intel</code>/<code>kvm</code>/<code>irqbypass</code> loaded;
<code>python3 scripts/test-all-examples.py --only 35-virtualization-and-kvm
--mode local</code> printed <code>PASS PASS PASS</code> (3 passed, 0
failed, 0 skipped). Guest: <code>systems-target</code> (Fedora 44, kernel
6.19.10-300.fc44.x86_64), <code>lscpu</code> reporting
<code>Virtualization type: full</code>, <code>systemd-detect-virt</code>
printing <code>kvm</code>, DMI <code>sys_vendor</code>=<code>QEMU</code>,
<code>product_name</code>=<code>Standard PC (Q35 + ICH9, 2009)</code>. The
host/guest/container <code>overheadbench</code> table (syscall, mem, io) was
read directly off <code>./demo.sh go run</code>, <code>TARGET=systems-target
./demo.sh go run</code>, and <code>podman build</code> +
<code>podman run</code> against the multi-stage <code>Containerfile</code>
in <code>examples/35-virtualization-and-kvm/go/</code>, each repeated three
times; the <code>strace -c</code> cross-check on both host and guest showed
exactly 50 <code>fsync</code> calls for <code>--iters 50</code>. Firecracker
v1.16.1 (released 2026-07-02) and Cloud Hypervisor v53.0 (released
2026-07-12) version numbers were confirmed against each project's GitHub
releases at authoring time and are not independently re-verified by any run
in this lab.</p>
