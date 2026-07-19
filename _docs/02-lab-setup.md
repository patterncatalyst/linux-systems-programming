---
title: "The KVM lab"
order: 2
part: "Setting Up"
description: "Provision two disposable Fedora KVM guests with cloud-init — an observation-first toolset, snapshot/revert discipline, and a deploy script whose output proves your code ran on the guest kernel, not yours."
duration: 45 minutes
---

Chapter 1 proved that three toolchains build and run the same program on your
workstation. This chapter builds the other half of the book's infrastructure:
two disposable Fedora VMs — `systems-target` and `systems-peer` — that absorb
everything you should never do to the machine you work on. A systems-programming
book keeps asking you to run code as root, watch the kernel from the inside,
fill filesystems, kill process trees, and misconfigure networks on purpose.
The lab is where all of that happens, and one snapshot command makes any
mistake reversible in seconds.

The scripts are in `scripts/lab/`. They are parameterized by `LAB_PREFIX`
(default `systems`), so the same set provisions, starts, snapshots, and
destroys the guests by name.

{% include excalidraw.html
   file="02-lab-topology"
   alt="Lab topology: a host band with libvirtd/KVM, the ~/.cache/systems-lab image cache holding the pristine Fedora Cloud Base image plus one qcow2 overlay per guest, and the scripts/lab directory; below it, the libvirt NAT network 192.168.124.0/24 containing systems-target (4 GB, .7) and systems-peer (2 GB, .95), both on kernel 6.19.10. An amber deploy arrow runs from the scripts to systems-target; a dashed line links the two guests for later two-host chapters; a note marks the lab-ready snapshot taken on both."
   caption="Figure 2.1 — the systems-* lab: cached base image + per-VM overlays on the host, two guests on libvirt NAT, one amber deploy path" %}

> **Tools used** — `virt-install`, `qemu-img`, `cloud-localds`, `virsh`, `ssh`
> (host). Everything here is checked by `scripts/check-host.sh`; the guest
> toolset itself is installed by cloud-init.

## Why a disposable lab

Three kinds of chapters force the issue. **Privileged demos** — capabilities,
namespaces, cgroup writes, mount tricks — need root, and root that fat-fingers
a path on your workstation is a bad afternoon. **Kernel observation** — bpftrace
one-liners, perf profiles, the BCC tools — attaches instrumentation to a live
kernel, and you want that kernel to be one you can reboot without losing your
editor. **Destructive demos** — OOM kills, disk-full behavior, deliberately
broken network configs — only teach if you actually break something, which
means you need something safe to break.

A VM answers all three at once, and adds the property no container gives you:
**its own kernel**. Namespaces share the host kernel, so a container can't show
you kernel-version drift, can't be probed as an isolated kernel, and can't be
snapshot-reverted with its kernel state. The lab guests run their own
`6.19.10-300.fc44` kernel while the host runs `7.1.3-200.fc44` — a visible
difference this chapter turns into its cross-check.

That said, the VM is not the default. The book's decision rule, applied at the
top of every example:

> **Run locally unless the demo needs root, needs to observe the kernel, or
> needs two hosts.** Everything else — ordinary syscalls, file I/O, unprivileged
> process work — runs faster and debugs easier on your workstation.

## Host prerequisites

Provisioning needs four things on the host, and
`scripts/lab/provision-vm.sh` checks for `virt-install` and `cloud-localds`
before doing anything:

```bash
[host]$ sudo dnf install virt-install qemu-img cloud-utils libvirt
[host]$ sudo usermod -aG libvirt "$USER"     # then log out/in, or: sg libvirt -c '...'
[host]$ ls -l /dev/kvm                       # hardware virtualization is available
```

`virt-install` defines and boots the domain, `qemu-img` builds overlay disks,
`cloud-localds` (from `cloud-utils`) packs the cloud-init seed, and membership
in the `libvirt` group lets you talk to the system libvirt instance — every
script exports `LIBVIRT_DEFAULT_URI=qemu:///system` so the guests live in the
system instance, not a per-user session that vanishes with your login.

## How provisioning works

`scripts/lab/provision-vm.sh` is a straight pipeline: cached base image →
per-VM overlay → cloud-init seed → `virt-install`. Each stage exists for a
reason worth spelling out.

**The cached base image.** The script downloads
`Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2` once into
`~/.cache/systems-lab/` and never modifies it. The version is pinned in the
script ("Pin the exact image you tested; bump deliberately") — an unpinned
"latest" would silently change the guest kernel under the whole book.

**The overlay disk.** Instead of copying the 400 MB base per guest, the script
layers a qcow2 overlay on top of it:

```bash
qemu-img create -f qcow2 -F qcow2 -b "$CACHE_DIR/$BASE_IMG" "$OVERLAY" "${DISK_GB}G"
```

The overlay records only the blocks the guest changes; the base stays pristine
and shared. This is what makes the guests *disposable* — destroying and
re-provisioning a guest deletes one thin overlay and rebuilds it in seconds,
with no re-download.

**The cloud-init seed.** Fedora Cloud Base has no users, no packages, no SSH
access — cloud-init is how it becomes yours on first boot. The script fills
two placeholders (`__HOSTNAME__`, `__SSH_PUBKEY__`) in
`scripts/lab/cloud-init/user-data.tmpl` with `sed`, then `cloud-localds` packs
the result into a tiny seed image that is attached as a CD-ROM. On first boot
the guest reads it, creates the `fedora` user with your SSH key and
passwordless sudo, grows the root filesystem into the overlay, and installs
the toolset.

**Booting.** `virt-install --import` boots the overlay directly (no installer),
with `--cpu host-passthrough` so the guest sees your real CPU features,
virtio disk and network for near-native I/O, and `--graphics none
--noautoconsole` because everything from here on is SSH. The script also
refuses to clobber: an existing domain of the same name is an error, and the
fix is an explicit `./destroy-vm.sh <name>` — rebuilds are deliberate, never
accidental.

## What goes in the guest — and what deliberately doesn't

The package list in `scripts/lab/cloud-init/user-data.tmpl` *is* the book's
tooling philosophy, so read it as an argument, not a manifest. The first block
is **observation tooling**: `bcc-tools`, `bpftrace`, `bpftool`, `libbpf-tools`
(precompiled CO-RE tools — `opensnoop`, `execsnoop`, `biolatency`), `perf`,
`strace`, `ltrace`, `gdb` plus `gdb-gdbserver`, `valgrind`, `sysstat`. That
block is the point of the lab: this book uses eBPF **as tooling, not as a
build target**, which is why there is *no* clang/llvm/libbpf-devel eBPF build
chain in the guest — the template says so in a comment right above the list.
You will trace with bpftrace constantly; you will not compile BPF programs.

The second block is **per-language toolchains** (`gcc-c++`, `clang`, `cmake`,
`ninja-build`, `golang`, `rust`/`cargo`) so demos that must compile in-VM —
privileged code, namespace experiments, glibc-parity fallbacks — can. The rest
is `podman`/`crun` for the container chapters, networking tools (`tcpdump`,
`socat`, `nmap-ncat`) for the two-host chapters, and `cockpit`, whose socket
cloud-init enables so the web console is reachable at
`https://<guest-ip>:9090` — a useful visual second opinion on guest load.

The `runcmd` section ends with the **readiness stamp**: it writes
`bpftrace --version` and `uname -r` into `/var/log/lab-ready`. That file is
the lab's contract — if it exists and names a bpftrace, cloud-init finished
and the observation tooling works. Every later chapter can check one file
instead of guessing whether provisioning completed.

Packages a single demo needs (fio, rt-tests, numactl) stay *out* of the base
on purpose: install them on demand, then re-snapshot to bake them in. A lean
base keeps provisioning fast and the snapshot meaningful.

## Snapshots: the discipline that makes "destructive" safe

`scripts/lab/snapshot-vm.sh <vm>` captures a snapshot named `lab-ready` —
RAM *and* disk, because the guest is running, so a revert restores it
*running*, tools warm, in seconds. The discipline has two halves:

- **Snapshot immediately after provisioning** (and after any deliberate
  addition you want to keep). This is your known-good tooled baseline.
- **Revert after every destructive demo**: `scripts/lab/revert-vm.sh
  systems-target` puts the guest back to `lab-ready`, no forensics, no
  "did that experiment leave state behind?" doubt.

Re-running `snapshot-vm.sh` replaces the same-named snapshot, so refreshing
the baseline is idempotent. `destroy-vm.sh` is the nuclear option — it
undefines the domain and deletes its overlay and seed, leaving only the cached
base image, from which a fresh guest is minutes away.

## The daily lifecycle

Day to day you touch two scripts. `scripts/lab/lab-up.sh` starts whichever of
`systems-target`/`systems-peer` are defined but stopped, waits for DHCP
leases, and prints IPs; `eval "$(./lab-up.sh --export)"` drops `TARGET_IP` and
`PEER_IP` straight into your shell for scripted tests. `scripts/lab/lab-down.sh`
sends ACPI shutdowns, waits up to 60 seconds, and — deliberately — deletes
nothing; disks and snapshots survive, and `FORCE=1` hard-stops a guest that
ignores ACPI. `vm-ip.sh` answers "what address did libvirt lease this guest?"
by reading the DHCP lease (guest-agent fallback). One host-specific note: on
this machine the libvirt `default` network is `192.168.124.0/24` with gateway
`192.168.124.1` — not the stock `192.168.122.0/24` you'll see in most libvirt
docs — so never hardcode an address; always ask `vm-ip.sh`.

## Deploying host builds

`scripts/lab/deploy-to-vm.sh <vm> <binary> [-- args]` is how host-built code
reaches the guest: it resolves the IP, `scp`s the binary to
`/home/fedora/`, marks it executable, and `exec`s it over `ssh -t` so Ctrl-C
reaches the remote process. Two toggles matter. `SUDO=1` runs the binary under
sudo for the demos that genuinely need root — the default is unprivileged,
matching the decision rule. And if `OTEL_ENDPOINT` is set, the script forwards
it as `OTEL_EXPORTER_OTLP_ENDPOINT` through `env` explicitly, because `sudo`
strips the environment — without that, a privileged guest binary would fall
back to its own localhost and its telemetry would never reach the host's
observability stack. This works because host builds and the guest are both
Fedora 44 (glibc parity); the in-VM toolchains are the fallback when that
assumption breaks.

## Build, run, observe

Provision both guests — target at the 4 GB default, peer smaller:

```bash
[host]$ cd scripts/lab
[host]$ ./provision-vm.sh systems-target
[host]$ RAM_MB=2048 ./provision-vm.sh systems-peer
```

Give cloud-init a minute or two (package installation dominates), then read
the readiness stamp:

```bash
[host]$ ssh fedora@"$(./vm-ip.sh systems-target)" cat /var/log/lab-ready
bpftrace v0.24.2
6.19.10-300.fc44.x86_64
```

That one file confirms the whole chain: the guest booted, cloud-init ran to
completion, the observation tooling works, and you can see which kernel the
lab runs. On this run the guests came up at `192.168.124.7` (target) and
`192.168.124.95` (peer). Now freeze the baseline on both:

```bash
[host]$ ./snapshot-vm.sh systems-target
[host]$ ./snapshot-vm.sh systems-peer
```

`virsh snapshot-list` on each should show one `lab-ready` snapshot. That's the
lab: built once, reverted forever.

## Cross-check: prove the code runs *there*

Chapter 1's example prints the kernel it runs on — which makes it a perfect
independent check that `deploy-to-vm.sh` really executes on the guest rather
than, say, quietly running the local binary. Run it both ways:

```bash
[host]$ cd examples/01-hello-syscalls
[host]$ ./demo.sh cpp
pid <PID> on Linux 7.1.3-200.fc44.x86_64 (x86_64)
[host]$ TARGET=systems-target ./demo.sh cpp
→ copying app to fedora@192.168.124.7:/home/fedora/app
→ running on systems-target (Ctrl-C to stop):
pid <PID> on Linux 6.19.10-300.fc44.x86_64 (x86_64)
```

Same binary, two different kernel strings: `7.1.3-200.fc44` is your host,
`6.19.10-300.fc44` is the guest. The example's per-language `demo.sh` wires
this up — with `TARGET` set, its `run` step calls
`scripts/lab/deploy-to-vm.sh "$TARGET" <binary>` instead of executing locally,
so every example in the book gets remote execution for free. The Go build
behaves identically (`TARGET=systems-target ./demo.sh go`); both were run on
this lab and printed the guest kernel string. The kernel version in the output
is the observable proof — a claim like "it deployed" is worthless next to the
guest's own `uname` in your terminal.

## What you learned

- The lab exists for exactly three reasons — root, kernel observation, and
  two-host demos — and the decision rule says everything else runs locally.
- Provisioning is a cheap pipeline: pinned cached base image → qcow2 overlay
  per guest → cloud-init seed → `virt-install`; guests are disposable because
  only the overlay is theirs.
- The guest toolset is observation-first (bcc/bpftrace/perf/gdbserver), with
  in-VM toolchains as a fallback and deliberately no eBPF build chain.
- Snapshot `lab-ready` after provisioning, revert after destructive demos —
  and trust `/var/log/lab-ready` plus a guest kernel string in your output,
  not assumptions.

Next, the third and final piece of setup: the Podman **LGTM observability
stack**, so every later demo has somewhere to send its telemetry.

---

<p><span class="status status--verified">verified</span> — both guests were provisioned from Fedora-Cloud-Base-Generic-44-1.7 on this host (target 4 GB at 192.168.124.7, peer 2 GB at 192.168.124.95); <code>/var/log/lab-ready</code> read back <code>bpftrace v0.24.2</code> and kernel <code>6.19.10-300.fc44</code> on both; <code>lab-ready</code> snapshots exist on both; and <code>deploy-to-vm.sh</code> ran the host-built C++ and Go binaries on <code>systems-target</code>, printing the guest kernel string <code>6.19.10-300.fc44.x86_64</code> versus <code>7.1.3-200.fc44.x86_64</code> locally.</p>
