---
title: "Namespaces and cgroups"
order: 32
part: "Containers and Virtualization"
description: "The eight Linux namespaces and cgroup v2's unified hierarchy as the two primitives every container runtime assembles from — unshare vs setns vs clone3, the pid-1 reparenting consequence, memory.max enforced by a real OOM kill, and the genuine ENOMEM Go hits calling unshare(CLONE_NEWPID) directly from a threaded runtime."
duration: "55 minutes"
---

Every `pmon` version so far has supervised, signaled, and identified a
child, but the child has always lived in the same world as its parent: same
process-id space, same hostname, same view of every device and byte of
RAM on the box. Real container runtimes give a process a *different*
world — its own pid 1, its own hostname, and a hard ceiling on memory and
CPU — without a hypervisor anywhere in sight. The one new idea this
chapter teaches is that "container" is not a kernel object at all: it is
two independent primitives, **namespaces** (what a process can *see*) and
**cgroups** (what a process is *allowed to use*), wired together by
whatever userspace tool assembles them. `pmon containerize` is that tool,
built by hand, with nothing between the syscalls and the terminal.

The code is in `examples/32-namespaces-and-cgroups/`. This is a **VM
example** — namespace and cgroup writes need real root and a real kernel,
so `./demo.sh <lang> run` deploys to `systems-target` when `TARGET` is
set; the `README.md` there covers the CLI and the paired positive/negative
verification in full.

{% include excalidraw.html
   file="32-namespace-overview"
   alt="Two columns, host and pmon containerize's PID-1 child, with eight namespace-type rows: PID, mount, UTS, network, time, IPC, user, cgroup. The first four rows are amber boxes with an arrow marked 'differs' between real /proc/&lt;pid&gt;/ns/* inode numbers — pid 4026531836 vs 4026532530, mount 4026531832 vs 4026532528, UTS 4026531838 vs 4026532529, network 4026531833 vs 4026532531 — read on systems-target. The last four rows are grey dashed boxes marked 'shared with host' with identical inode numbers on both sides — time 4026531834, IPC 4026531839, user 4026531837, cgroup 4026531835 — because pmon containerize only requests CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|CLONE_NEWPID and deliberately leaves the other four alone."
   caption="Figure 32.1 — the eight Linux namespaces, host vs pmon's PID-1 child, every inode number read live from /proc/&lt;pid&gt;/ns/* on systems-target" %}

> **Tools used** — `ssh`/`scp` and the Python runner
> (`scripts/test-all-examples.py`) on the host; `readlink`, `cat`, `pgrep`,
> `nsenter`, `dmesg`, `strace` (systems-target VM). Everything here ships
> with Fedora or is preinstalled in the lab VM by cloud-init.

## Eight namespaces, one job each

A Linux namespace does not hide anything from the kernel — the kernel
always has the complete, global view. It hides things from the *process*:
each namespace type wraps one kind of global kernel state so a process
inside it sees only its own slice.

- **PID** — a private process-id tree. The first process created inside a
  new PID namespace is *always* pid 1 of that namespace, no matter what
  its "real" (host-visible) pid is.
- **Mount** — a private copy of the mount table. Mounting or unmounting
  inside the namespace does not touch the host's view, once propagation is
  detached (below).
- **UTS** — the hostname and NIS domain name. One `sethostname(2)` call,
  fully isolated.
- **Network** — its own loopback, interfaces, routes, and port space; a
  bare network namespace starts with only `lo`, down.
- **IPC** — its own System V IPC objects and POSIX message queues.
- **User** — its own uid/gid mapping, letting an unprivileged host user
  become "root" inside the namespace over a mapped range — the one
  namespace `pmon containerize` does *not* request, discussed below.
- **Cgroup** — virtualizes which cgroup a process believes is its root,
  so `/proc/self/cgroup` reads `/` from inside even when the real path is
  nested five levels deep on the host.
- **Time** (Linux 5.6+) — lets a namespace offset `CLOCK_MONOTONIC` and
  `CLOCK_BOOTTIME`, the one facility built specifically for checkpoint/
  restore rather than isolation.

`pmon containerize` requests exactly four: `CLONE_NEWNS | CLONE_NEWUTS |
CLONE_NEWNET | CLONE_NEWPID`. Figure 32.1 is the receipt: every inode
number in it came from `readlink /proc/<pid>/ns/*` on `systems-target`
during this chapter's own run, and the four requested types read a
*different* inode on the child than on the host's pid 1, while the four
left alone — time, IPC, user, cgroup — read the *same* inode on both
sides. A namespace you didn't ask for is not a weaker namespace; it is no
namespace at all, just the host's, shared.

## unshare, setns, and clone3 — three ways into a namespace

The three syscalls answer three different questions:

- **`unshare(2)`** — "take *me*, right now, out of my current namespaces
  and into brand-new ones." For most types this applies to the calling
  process/thread immediately. `CLONE_NEWPID` is the deliberate exception:
  unshare(2) can never move an already-running process into a new PID
  namespace (a process's own pid can't change mid-life), so it only
  arranges that the *next* child the caller creates becomes pid 1 of a
  fresh one. That single asymmetry is why every implementation in this
  chapter unshares first and forks second, not the other way around.
- **`setns(2)`** — "join a namespace that already exists," given an open
  file descriptor on one of the `/proc/<pid>/ns/*` links. This is what
  `nsenter` does under the hood: open the target's namespace fds, `setns`
  into each, then fork and exec the requested command. Like `unshare`,
  `setns(CLONE_NEWPID)` only takes effect for children the caller creates
  *after* the call — the caller's own pid never moves.
- **`clone3(2)`** — create the child and select its namespaces in the
  *same* syscall, via `struct clone_args.flags`. This is the one true
  atomic option: there is no window where the new task exists but hasn't
  yet joined its namespaces. `pmon`'s C++ build calls it directly (below);
  Go's fix reaches the same atomicity indirectly, through the ordinary
  `clone(2)` flags word the runtime already issues for every process it
  spawns.

## The pid-1 consequence

Becoming pid 1 of a new namespace is not a label, it is a job with two
real obligations the kernel enforces. First, **reparenting**: inside a
PID namespace, an orphaned process is reparented to namespace-local pid 1,
not to the host's real init — so pid 1 must be prepared to `wait()` for
processes it never directly spawned, exactly the way real init is.
Second, **reaping**: default signal dispositions are altered for pid 1 —
an unhandled `SIGTERM`/`SIGKILL` that would kill any other process is
ignored by pid 1 unless it installs its own handler. `pmon containerize`
sidesteps both obligations by design: its pid-1 child immediately
`execve`s CMD, so CMD — not pmon's own code — inherits the pid-1 role for
whatever it does next. That is a real, stated simplification, not an
oversight: a supervisor that expects to run *multiple* processes inside
one PID namespace (the way `podman run` with more than one process, or a
process manager container, would) needs an init that actually reaps —
which is exactly what chapters 11 and 12 already built, just not wired to
this namespace yet.

## Cgroup v2: the unified hierarchy

Cgroup v1 let different controllers (cpu, memory, …) organize processes
into *different* tree shapes — a process could be in one cpu group and an
unrelated memory group. Cgroup v2 unifies this: **one tree**, and every
controller you enable sees the same groups. `systems-target` mounts it at
the conventional single point:

```bash
[vm]$ mount | grep cgroup
cgroup2 on /sys/fs/cgroup type cgroup2 (rw,nosuid,nodev,noexec,relatime,seclabel,nsdelegate,memory_recursiveprot,memory_hugetlb_accounting)
[vm]$ cat /sys/fs/cgroup/cgroup.controllers
cpuset cpu io memory hugetlb pids rdma misc dmem
[vm]$ cat /sys/fs/cgroup/cgroup.subtree_control
cpu memory pids
```

Three files matter for `containerize`. **`memory.max`** is a hard byte
ceiling — cross it and the kernel's OOM killer runs *inside this cgroup
only*, picking a victim from the cgroup's own processes regardless of
system-wide memory pressure. **`cpu.max`** is written as `"$QUOTA
$PERIOD"` microseconds — `"max 100000"` (pmon's default) means unthrottled
inside a 100 ms accounting period; a real ceiling would read e.g.
`"50000 100000"` for half a core. A sibling knob `containerize` doesn't
set, worth knowing: **`cpu.weight`** (default 100, range 1–10000) doesn't
cap anything — it only decides who wins when several cgroups *compete*
for the same CPU, the cgroup analogue of `nice`. `memory.max` and
`cpu.max` answer "how much," `cpu.weight` answers "whose turn," and a real
container runtime writes all three.

The fourth file is the pressure signal: **PSI** (Pressure Stall
Information), `memory.pressure`, is not a counter of *how much* memory a
cgroup used — it is how much time tasks in this cgroup spent *stalled*
waiting for memory, per-cgroup, independent of pressure anywhere else on
the box:

```bash
[vm]$ cat /sys/fs/cgroup/pmon-memcheck/memory.pressure
some avg10=0.00 avg60=0.00 avg300=0.00 total=0
full avg10=0.00 avg60=0.00 avg300=0.00 total=0
```

`pmon containerize` reads the `some avg10=` field after CMD exits and
prints it as `pmon: cgroup mem.pressure some=<x>` — a real number, not a
placeholder, confirming the kernel is actually accounting this cgroup, not
just enforcing a limit blindly.

One subtlety governs the whole design: a *non-root* cgroup cannot both
hold processes directly and delegate a controller to its children (the
"no internal process" constraint) — but the **root** cgroup is explicitly
exempt. That is why `setup_cgroup()` can write `+memory +cpu` into
`/sys/fs/cgroup/cgroup.subtree_control` without first relocating `pmon`
itself into a leaf: the root is the one place in the tree allowed to hold
processes and delegate at the same time.

## What a container runtime actually assembles

Strip away the marketing and a container is five primitives, wired
together by one userspace program: **namespaces** (this chapter, minus
user — `containerize` never requests `CLONE_NEWUSER`, so its "root" inside
the container is genuinely uid 0 on the host, which is why the demo needs
real root rather than rootless mapped uids), **cgroups** (this chapter),
a **rootfs** (a different filesystem view via `pivot_root`/bind mounts —
chapter 34 builds this), **capabilities** (chapter 14's five sets,
dropped to a minimal set before `exec`), and a **seccomp/Landlock**
filter (chapter 33, next). `pmon containerize` deliberately stops after
the first two, on the same host filesystem CMD would otherwise see — the
point is to make namespaces and cgroups *visible* as syscalls you could
write yourself, before chapter 34 shows what podman assembles from all
five at once inside a real multi-stage Containerfile.

{% include excalidraw.html
   file="32-cgroup-hierarchy"
   alt="The cgroup v2 unified hierarchy from root down to a leaf. The root band shows cgroup.controllers listing cpuset, cpu, io, memory, hugetlb, pids, rdma, misc, dmem, and cgroup.subtree_control enabling plus memory plus cpu, delegated to children. An arrow labeled enables leads down into the pmon-oomdemo leaf band, showing memory.max as 67108864 (64 MiB), memory.swap.max as 0 (no swap headroom), cpu.max as max 100000 (unthrottled), cgroup.procs holding pmon's own pid written before fork, and memory.pressure PSI reading some avg10=0.00. A dark box below states the hog process (sh, real anon-rss 64384kB) writes past the 64 MiB ceiling. An arrow labeled 'OOM killer fires' leads into a kernel band showing the real dmesg lines from this session: oom-kill constraint=CONSTRAINT_MEMCG oom_memcg=/pmon-oomdemo task=sh pid=27013, and 'Memory cgroup out of memory: Killed process 27013 (sh) anon-rss:64384kB', with pmon's own report alongside it: signal=9 (KILL), exit 137."
   caption="Figure 32.2 — the cgroup v2 unified hierarchy, root to leaf, ending in the real dmesg OOM-kill line this session produced" %}

## How the code works

All three languages share one shape: build the cgroup, unshare/clone into
fresh namespaces, become pid 1, exec CMD, then the parent waits and
reports the cgroup-side evidence. `setup_cgroup`/`setupCgroup` (identical
logic across C++, Go, and Rust — read `cgroup.subtree_control`, add
`+memory`/`+cpu` as whole tokens only if missing so `"cpu"` never
false-matches inside `"cpuset"`, `mkdir` the leaf, write `memory.max` then
`cpu.max` then a best-effort `memory.swap.max=0`, then write pmon's own
pid into `cgroup.procs`) runs **before** any namespace call in every
language, on purpose: cgroup membership is inherited across `fork`/
`clone`/`exec`, so writing pmon's pid first means every later child — the
pid-1 task, and CMD after it execs — is under the limits from its very
first instruction, never after.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
  // Mount, UTS and network namespaces take effect on THIS process right now.
  // The PID namespace is deferred: the next clone3(2) child becomes PID 1 of
  // a fresh one; we (the caller) stay in our original PID namespace so we
  // can waitpid(2) the child the ordinary way.
  if (auto r = checked(::unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID),
                       "unshare");
      !r) {
    std::println(stderr, "containerize: {}: {}", r.error().what, std::strerror(r.error().err));
    return 1;
  }

  // Detach mount propagation recursively so nothing we do in this namespace
  // (or CMD does) leaks a mount event back to the host's mount table.
  if (auto r = checked(::mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr),
                       "mount MS_PRIVATE");
      !r) {
    std::println(stderr, "containerize: {}: {}", r.error().what, std::strerror(r.error().err));
    return 1;
  }

  // sethostname(2) now lands in the fresh UTS namespace, not the host's.
  if (auto r = checked(::sethostname(hostname.c_str(), hostname.size()), "sethostname"); !r) {
    std::println(stderr, "containerize: {}: {}", r.error().what, std::strerror(r.error().err));
    return 1;
  }

  // clone3(2): flags=0 (the namespace work is already done above), stack=0
  // (kernel copy-on-write duplicates our stack exactly like fork(2)),
  // exit_signal=SIGCHLD (so waitpid(2) below works the ordinary way).
  clone_args_v0 cl{};
  cl.exit_signal = SIGCHLD;
  long rc = ::syscall(SYS_clone3, &cl, sizeof(cl));
  if (rc < 0) {
    std::println(stderr, "containerize: clone3: {}", std::strerror(errno));
    return 1;
  }

  if (rc == 0) {
    // --- child: PID 1 of the new PID namespace ---
    if (::getpid() == 1) {
      std::println("pmon: child sees pid 1");
    }
    std::fflush(stdout);

    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) {
      std::println("pmon: hostname={}", buf);
    }
    std::fflush(stdout);

    std::vector<char*> argv(cmd.begin(), cmd.end());
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    std::println(stderr, "pmon: exec {}: {}", argv[0], std::strerror(errno));
    std::fflush(stderr);
    _exit(127);
  }

  // --- parent: waits, then reports the cgroup-side evidence ---
  int status = 0;
  if (::waitpid(static_cast<pid_t>(rc), &status, 0) < 0) {
    std::println(stderr, "containerize: waitpid: {}", std::strerror(errno));
    return 1;
  }

  auto psi = read_file(cgroup_path + "/memory.pressure");
  if (psi) {
    std::println("pmon: cgroup mem.pressure some={}", parse_psi_some_avg10(*psi));
  }

  int exit_code = 0;
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    std::println("pmon: child exited status={}", code);
    exit_code = code;
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    std::println("pmon: child killed signal={} ({})", sig, signal_name(sig));
    exit_code = 128 + sig;
  }
```

```go
	self, err := os.Executable()
	if err != nil {
		fmt.Fprintf(os.Stderr, "containerize: os.Executable: %v\n", err)
		return 1
	}
	// The hostname has to travel to the re-exec'd child: it, not this
	// process, is the one that ends up inside the fresh UTS namespace (see
	// the package comment).
	initArgs := append([]string{reexecMarker, hostname, "--"}, cmd...)
	c := exec.Command(self, initArgs...)
	c.Stdin, c.Stdout, c.Stderr = os.Stdin, os.Stdout, os.Stderr
	// Cloneflags asks the runtime's own fork/exec trampoline to create the
	// child already inside fresh mount/uts/net/pid namespaces, atomically,
	// in the SAME clone(2) that creates the process — no separate unshare(2)
	// on our side, and so none of the "one child per fresh PID namespace"
	// restriction that broke the direct-unshare attempt.
	c.SysProcAttr = &syscall.SysProcAttr{
		Cloneflags: syscall.CLONE_NEWNS | syscall.CLONE_NEWUTS | syscall.CLONE_NEWNET | syscall.CLONE_NEWPID,
	}
	if err := c.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "containerize: exec %s: %v\n", cmd[0], err)
		return 1
	}

	waitErr := c.Wait()

	if psi, err := os.ReadFile(cgroupPath + "/memory.pressure"); err == nil {
		fmt.Printf("pmon: cgroup mem.pressure some=%s\n", parsePSISomeAvg10(string(psi)))
	}

	exitCode := 0
	switch {
	case waitErr == nil:
		fmt.Println("pmon: child exited status=0")
	default:
		var ee *exec.ExitError
		if errors.As(waitErr, &ee) {
			ws := ee.Sys().(syscall.WaitStatus)
			if ws.Signaled() {
				sig := int(ws.Signal())
				fmt.Printf("pmon: child killed signal=%d (%s)\n", sig, signalName(sig))
				exitCode = 128 + sig
			} else {
				exitCode = ws.ExitStatus()
				fmt.Printf("pmon: child exited status=%d\n", exitCode)
			}
		} else {
			fmt.Fprintf(os.Stderr, "containerize: wait: %v\n", waitErr)
			exitCode = 1
		}
	}

	_ = os.Remove(cgroupPath) // best-effort; a leftover cgroup is harmless

	return exitCode
```

```rust
    // Mount, UTS and network namespaces take effect on THIS process right
    // now. The PID namespace is deferred: the next fork(2) child becomes
    // PID 1 of a fresh one; we (the caller) stay in our original PID
    // namespace so we can waitpid(2) the child the ordinary way.
    if let Err(e) = unshare(
        CloneFlags::CLONE_NEWNS
            | CloneFlags::CLONE_NEWUTS
            | CloneFlags::CLONE_NEWNET
            | CloneFlags::CLONE_NEWPID,
    ) {
        eprintln!("containerize: unshare: {e}");
        return 1;
    }

    // Detach mount propagation recursively so nothing we (or CMD) do in
    // this namespace leaks a mount event back to the host's mount table.
    if let Err(e) = mount(
        Some("none"),
        "/",
        None::<&str>,
        MsFlags::MS_REC | MsFlags::MS_PRIVATE,
        None::<&str>,
    ) {
        eprintln!("containerize: mount MS_PRIVATE: {e}");
        return 1;
    }

    // sethostname(2) now lands in the fresh UTS namespace, not the host's.
    if let Err(e) = sethostname(&hostname) {
        eprintln!("containerize: sethostname: {e}");
        return 1;
    }

    // SAFETY: the child performs only async-signal-safe operations
    // (getpid, gethostname, writes to already-open stdio fds, execvp)
    // before it either execs or _exits — it never returns up through this
    // stack frame into arbitrary Rust code, satisfying fork(2)'s contract.
    match unsafe { fork() } {
        Ok(ForkResult::Child) => {
            if getpid().as_raw() == 1 {
                println!("pmon: child sees pid 1");
            }
            if let Ok(hn) = gethostname() {
                println!("pmon: hostname={}", hn.to_string_lossy());
            }
            let _ = std::io::stdout().flush();

            let prog = CString::new(cmd[0].as_str()).unwrap_or_default();
            let cargs: Vec<CString> = cmd
                .iter()
                .map(|s| CString::new(s.as_str()).unwrap_or_default())
                .collect();
            // execvp never returns on success.
            let err = execvp(&prog, &cargs).unwrap_err();
            eprintln!("pmon: exec {}: {err}", cmd[0]);
            std::process::exit(127);
        }
        Ok(ForkResult::Parent { child }) => {
            let status = waitpid(child, None);

            if let Ok(psi) = fs::read_to_string(format!("{cgroup_path}/memory.pressure")) {
                println!(
                    "pmon: cgroup mem.pressure some={}",
                    parse_psi_some_avg10(&psi)
                );
            }

            let exit_code = match status {
                Ok(WaitStatus::Exited(_, code)) => {
                    println!("pmon: child exited status={code}");
                    code
                }
                Ok(WaitStatus::Signaled(_, sig, _)) => {
                    let n = sig as i32;
                    println!("pmon: child killed signal={n} ({})", signal_name(n));
                    128 + n
                }
                Ok(_) => 0,
                Err(e) => {
                    eprintln!("containerize: waitpid: {e}");
                    1
                }
            };

            let _ = fs::remove_dir(&cgroup_path); // best-effort cleanup

            exit_code
        }
        Err(e) => {
            eprintln!("containerize: fork: {e}");
            1
        }
    }
```

C++ and Rust get there through the same shape — `unshare` first, taking
mount/UTS/network effect immediately on the caller; the mount-private
remount so nothing propagates back to the host; `sethostname`; then a
fork-shaped child creation (`clone3(flags=0)` in C++, `nix::fork` in
Rust — both give "no code runs between clone and exec except what we
wrote," fork(2)'s classic contract) whose child becomes pid 1 of the
deferred PID namespace and immediately prints its two proof lines before
`execvp`. The parent side is identical in shape across both: `waitpid`,
read `memory.pressure`, print the mem-pressure line, then translate the
wait status into `pmon: child exited status=<n>` or `pmon: child killed
signal=<n> (<NAME>)` with a hand-rolled signal-name table (not
`strsignal(3)`/`Signal::from_raw`'s locale-or-library-dependent prose,
reused verbatim from every earlier `pmon` chapter) and the `128+n`
convention. Go's excerpt is the odd one out and is the subject of the
Concurrency lens below.

**Fragile bits, stated plainly**: `containerize` never remounts a fresh
`/proc` inside the new mount namespace, so tools that read `/proc` from
inside — including `ps` run via `nsenter` — still see the host's process
table (the cross-check below demonstrates this directly); a real rootfs
container fixes this with `mount -t proc proc /proc` after `pivot_root`,
which is chapter 34's job, not this one's. `memory.swap.max` is written
best-effort because not every kernel exposes swap accounting identically.
And `deploy-to-vm.sh` rebuilds the remote command line with a bare `$*`,
which flattens quoting — every `CMD` in this chapter's runs is therefore
space-free tokens (`/usr/bin/echo hi`, `sh /tmp/hog.sh`), with anything
needing real shell logic staged as a script file on the guest first.

## Errors, three ways

The exit-code contract is identical across all three binaries, and it
splits by *when* the kernel or pmon rejects something. A **usage** error
(exit 2) is caught before any privileged call — a missing `-- CMD`:

```bash
[host]$ ./app containerize --hostname x
containerize: missing -- CMD
```

A **setup** error (exit 1) is a syscall that failed before CMD ever ran.
The ordinary case is the privilege check — `containerize: must run as
root` when invoked as a normal user, real output from this session,
unprivileged, on the reference host. The instructive case is the one Go's
first implementation actually produced on `systems-target`, verbatim:

```
containerize: exec /usr/bin/echo: fork/exec /home/fedora/app: cannot allocate memory
```

That is `ENOMEM` from a real `clone(2)` call, captured live — the
Concurrency lens below explains exactly why. Finally, a **child-side**
error is CMD's own fate, reported through the same wait-status
translation in every language: the OOM negative control's `pmon: child
killed signal=9 (KILL)` becomes exit `137` (`128 + 9`), and `pmon`
propagates it unchanged rather than collapsing it to a generic failure —
the caller can tell "CMD chose to fail" from "CMD was killed" from the
exit code alone, no log-scraping required.

## Concurrency lens

{% include excalidraw.html
   file="32-clone3-flow"
   alt="Three lanes. The left lane, C++/Rust, shows unshare taking effect on the caller, then clone3 with flags=0 (fork semantics) creating a child that prints its pid/hostname proof lines and execs CMD — labeled PASS, real run this session. The middle lane, Go's first attempt, shows runtime.LockOSThread pinning the goroutine then a raw unshare call, followed by os/exec.Start issuing an internal vfork/pidfd probe fork, then the real fork for CMD failing with clone returning -1 ENOMEM and the message fork/exec: cannot allocate memory — real, this session. The bottom lane, Go's shipped fix, shows setup_cgroup writing its own pid first same as the other two languages, then exec.Command with SysProcAttr.Cloneflags carrying CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|CLONE_NEWPID, one clone(2) call creating the child already inside all four namespaces with no prior unshare and no LockOSThread needed, the child re-execing itself as __ns_init__ to mount / MS_PRIVATE, sethostname, print the proof lines, and exec CMD as PID 1 — labeled PASS 13/13 across cpp, go, rust on systems-target, this session. A note explains the ENOMEM is a real, kernel-documented restriction: unshare(CLONE_NEWPID) allows the caller to create exactly one child in the fresh namespace before further forks there return ENOMEM, and os/exec's own vfork/pidfd fast path performs an internal probe fork that silently spends that one shot."
   caption="Figure 32.3 — C++/Rust's unshare-then-clone3 shape beside Go's two attempts: a real ENOMEM, then the SysProcAttr.Cloneflags fix that passes" %}

Namespace changes made by `unshare(2)`/`setns(2)` are per-**thread**
kernel state, not per-process — the same fact chapter 14 hit with
capabilities. A Go program is multithreaded before `main` even runs, and
the goroutine scheduler is free to migrate any goroutine onto any OS
thread between two lines of Go code. The textbook answer is
`runtime.LockOSThread()`: pin the calling goroutine to its current OS
thread so a subsequent raw `unshare()` and the fork/exec that must land on
the *same*, now-unshared thread can't be split across two different ones.
That is exactly what this chapter's Go build tried first — `LockOSThread`,
then `unix.Unshare(CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|CLONE_NEWPID)`,
then re-exec itself via `os/exec` so the fork would land on the pinned
thread. It built. Its pid/hostname proof lines were real. And it failed
the memory-hog negative control on real hardware, every time, with exit 1
and `fork/exec /home/fedora/app: cannot allocate memory` — not a typo, not
a flaky VM, a `strace` on `systems-target` shows the actual syscall:

```
[vm]$ sudo strace -f -e trace=clone,unshare,execve -o /tmp/go-trace.log \
      /home/fedora/app containerize --hostname pmon-vm-check -- /usr/bin/echo cmd-ran
containerize: exec /usr/bin/echo: fork/exec /home/fedora/app: cannot allocate memory
```
```
24652 unshare(CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWPID|CLONE_NEWNET) = 0
24652 clone(child_stack=NULL, flags=CLONE_VM|CLONE_PIDFD|CLONE_VFORK, parent_tid=[7]) = 24657
24657 +++ exited with 0 +++
24652 clone(child_stack=NULL, flags=CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD, parent_tid=...) = -1 ENOMEM (Cannot allocate memory)
```

`unshare` genuinely succeeds. The first `clone` after it — a vfork/pidfd
probe `os/exec` performs internally on first use, unrelated to our
request — also succeeds and exits immediately, and in doing so becomes
the fresh PID namespace's pid 1. That is the real, kernel-documented
restriction: `unshare(CLONE_NEWPID)` lets the caller create exactly *one*
child in the new namespace; every fork after that one returns `ENOMEM`.
LockOSThread solved the problem it was built for — thread affinity — and
was powerless against this one, because this failure has nothing to do
with which thread is running: it is about how many times the *same*
thread group is allowed to fork into a namespace it merely unshared
rather than was born into.

The fix is the same move chapter 14 made for capabilities: stop calling
the namespace syscall directly from live, threaded Go code, and describe
it declaratively instead. `exec.Cmd.SysProcAttr.Cloneflags` asks the
runtime's own fork/exec trampoline to create the namespaces **and** the
real child in one `clone(2)` call — there is no separate `unshare()`
anywhere, so there is no "used up my one child" state to exhaust, and
`LockOSThread` becomes unnecessary because there is no longer a
namespace-mutating syscall for a goroutine to be scheduled away from. The
general rule, twice proven now across two different kernel primitives:
whenever Go needs to change something that is per-thread and
irreversible, push the whole operation into the runtime's fork/exec
trampoline via `SysProcAttr` rather than performing it by hand on a
pinned goroutine — the trampoline is the one place in a Go binary
guaranteed to run alone, between one `clone(2)` and the `execve` right
after it.

## Build, run, observe

```bash
[host]$ LIBVIRT_DEFAULT_URI=qemu:///system \
        python3 scripts/test-all-examples.py --only 32-namespaces-and-cgroups --mode vm
```

On this host with the lab up, the runner built all three languages and
verified each against `systems-target` (Fedora 44, kernel
6.19.10-300.fc44.x86_64):

```
example                    cpp   go    rust
32-namespaces-and-cgroups  PASS  PASS  PASS

3 passed, 0 failed, 0 skipped
```

Each language's verify run passed all 13 assertions
(`PASS 13 / FAIL 0`). By hand, the positive run — a trivial CMD proves
both the PID and UTS namespaces at once:

```bash
[host]$ export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target SUDO=1
[host]$ ./demo.sh cpp run containerize --hostname pmon-vm-check -- /usr/bin/echo cmd-ran
→ running on systems-target (Ctrl-C to stop):
pmon: child sees pid 1
pmon: hostname=pmon-vm-check
cmd-ran
pmon: cgroup mem.pressure some=0.00
pmon: child exited status=0
```

And the negative control — a real memory hog (`head -c 200000000
/dev/zero | tr '\0' x`, held in a shell variable so it can't be swapped or
paged away, staged as a guest script since `deploy-to-vm.sh` would mangle
its embedded pipe) under `--mem-max 67108864` (64 MiB, far below the
~190 MiB it tries to hold):

```bash
[host]$ ./demo.sh cpp run containerize --hostname pmon-oomdemo --mem-max 67108864 --cgroup pmon-oomdemo -- sh /tmp/hog.sh
pmon: child sees pid 1
pmon: hostname=pmon-oomdemo
pmon: cgroup mem.pressure some=0.00
pmon: child killed signal=9 (KILL)
[host]$ echo $?
137
```

The hog never reaches its own trailing `echo should-not-print` in any of
the three languages — proof the cgroup terminated it mid-allocation,
not merely slowed it down. Go, Rust, and C++ all produce byte-identical
output and the same exit 137; that identity is exactly what
`verify.lua`'s 13 assertions per language check.

## Cross-check on systems-target

Three independent tools confirm the same two facts Figure 32.1 claims —
that the namespaces are real, and that the missing `/proc` remount has a
real, visible consequence. First, the namespace identities themselves,
read directly while a long-lived `containerize` child is alive:

```bash
[vm]$ sudo /home/fedora/app containerize --hostname pmon-box --cgroup pmon-crosscheck -- sleep 60 &
[vm]$ CHILD=$(pgrep -f '^sleep 60$')
[vm]$ readlink /proc/1/ns/pid /proc/1/ns/mnt /proc/1/ns/uts /proc/1/ns/net
pid:[4026531836]
mnt:[4026531832]
uts:[4026531838]
net:[4026531833]
[vm]$ sudo readlink /proc/$CHILD/ns/pid /proc/$CHILD/ns/mnt /proc/$CHILD/ns/uts /proc/$CHILD/ns/net
pid:[4026532530]
mnt:[4026532528]
uts:[4026532529]
net:[4026532531]
```

Every one of the four requested namespaces reads a different inode than
the host's pid 1. The three namespaces `containerize` never requests read
*identical* inodes on both sides — `time:[4026531834]`,
`ipc:[4026531839]`, `user:[4026531837]`, `cgroup:[4026531835]`, on host
and child alike — confirming those are genuinely shared, not silently
isolated by some other mechanism.

Second, `nsenter` — the `setns(2)`-based tool — demonstrates the missing
`/proc` remount directly instead of asserting it:

```bash
[vm]$ sudo nsenter -t $CHILD -p -m -u -n ps -ef | head -3
UID          PID    PPID  C STIME TTY          TIME CMD
root           1       0  0 14:42 ?        00:00:06 /usr/lib/systemd/systemd --switched-root...
root           2       0  0 14:42 ?        00:00:00 [kthreadd]
```

`nsenter` genuinely `setns`'d into the child's pid/mount/uts/net
namespaces before forking `ps` — but `ps` reads `/proc`, and the mount
namespace it entered never got a *fresh* procfs mounted after `unshare`;
it still resolves to the host's original procfs instance, so `ps` prints
the host's entire process table instead of a scoped one. This is not a
bug in `containerize`; it is precisely why chapter 34's Containerfile
build includes a `pivot_root` and a fresh `/proc` mount — a namespace
without a matching filesystem view is only half a container.

Third, the kernel's own OOM log, read independently of anything `pmon`
printed:

```bash
[vm]$ sudo dmesg | grep -A1 "oom-kill:constraint=CONSTRAINT_MEMCG"
oom-kill:constraint=CONSTRAINT_MEMCG,nodemask=(null),cpuset=/,mems_allowed=0,oom_memcg=/pmon-oomdemo,task_memcg=/pmon-oomdemo,task=sh,pid=27013,uid=0
Memory cgroup out of memory: Killed process 27013 (sh) total-vm:71332kB, anon-rss:64384kB, file-rss:3488kB, shmem-rss:0kB, UID:0 pgtables:176kB oom_score_adj:0
```

`CONSTRAINT_MEMCG` and `oom_memcg=/pmon-oomdemo` are the kernel's own
words for "this cgroup's `memory.max`, not global memory pressure, is why
this process died" — the same verdict `pmon`'s `signal=9 (KILL)` line
reported, confirmed at the source that actually made the decision.

## What you learned

- Namespaces hide global kernel state from a *process*, not from the
  kernel; `unshare(2)` moves the caller into new ones now (except
  `CLONE_NEWPID`, which only ever affects the *next* child), `setns(2)`
  joins one that already exists via an open `/proc/<pid>/ns/*` fd, and
  `clone3(2)` creates the child and its namespaces atomically in one
  syscall — the only option with no in-between state.
- A cgroup v2 leaf's `memory.max`/`cpu.max` only prove anything paired
  with a negative control: a real hog under a 64 MiB ceiling must be
  OOM-killed (exit 137, `dmesg`'s `oom_memcg=` naming the cgroup) — a
  cgroup that let it finish would be decorative, not enforcing — and PSI
  (`memory.pressure`) is the per-cgroup stall signal, not a system-wide one.
- Namespace changes are per-thread state, and a Go program is threaded
  before `main` runs: `unshare(CLONE_NEWPID)` also caps the caller to
  exactly one child in the fresh namespace, a restriction `os/exec`'s own
  internal probe fork silently spends, producing a real `ENOMEM` this
  chapter reproduced live — the fix is `SysProcAttr.Cloneflags`, pushing
  the whole operation into the runtime's own fork/exec trampoline, the
  same move chapter 14 made for capabilities.
- A container is namespaces + cgroups + rootfs + capabilities + seccomp,
  assembled by userspace, not one kernel object — and a namespace without
  its matching filesystem view is observably incomplete: `nsenter`'s `ps
  -ef` proved it by showing the host's whole process table through a
  correctly-`setns`'d pid namespace that never got a fresh `/proc`.

Next, **seccomp and Landlock**: filtering which syscalls a process may
even attempt, and restricting which files it may reach, as the two
primitives chapter 32 left out of "what a container runtime assembles."

---

<p><span class="status status--verified">verified</span> — every
transcript and number above was produced this session against the
<code>systems-target</code> guest (Fedora 44, kernel
6.19.10-300.fc44.x86_64): the runner printed
<code>32-namespaces-and-cgroups  PASS  PASS  PASS</code> (3 passed, 0
failed, 0 skipped) in vm mode, each language's <code>verify.lua</code>
run at 13/13; the positive run emitted <code>pmon: child sees pid 1</code>
/ <code>pmon: hostname=pmon-vm-check</code> (exit 0) and the memory-hog
negative control emitted <code>pmon: child killed signal=9 (KILL)</code>
(exit 137), byte-identical across cpp/go/rust; <code>readlink
/proc/&lt;pid&gt;/ns/*</code> showed the host's pid 1 and the containerized
child's PID/mount/UTS/network namespaces at different inodes and their
time/IPC/user/cgroup namespaces at identical inodes; <code>dmesg</code>
showed the real <code>CONSTRAINT_MEMCG</code>/<code>oom_memcg=/pmon-oomdemo</code>
kill of pid 27013; <code>nsenter -p -m -u -n ps -ef</code> was run live and
showed the host's full process table, confirming the missing
<code>/proc</code> remount. The Go <code>ENOMEM</code> failure quoted in
the Concurrency lens is this session's own <code>strace</code> output
against the pre-fix build, kept as evidence of a real, reproduced defect
before the <code>Cloneflags</code> fix that now ships and passes. A
one-line <code>verify.lua</code> bug this chapter also surfaced and fixed
— an unescaped <code>-</code> in a Lua pattern silently failing the
hostname assertion — is recorded in the example's own history, not
restated here as a finding about the chapter's subject matter.</p>
