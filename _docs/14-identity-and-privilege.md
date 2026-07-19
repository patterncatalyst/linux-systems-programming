---
title: "Identity and privilege"
order: 14
part: "Processes, Signals, Privilege"
description: "Real, effective, and saved uids as one state machine that setresuid finally makes explicit; umask and the file bits; then capabilities done properly — file sets vs process sets, why ambient exists, and the KEEPCAPS ordering trap — proven live as pmon v3 binds :80 at uid 65534 carrying exactly one capability."
duration: "50 minutes"
---

`pmon` can spawn a child, shepherd it through signals, and supervise it
race-free through a pidfd. Version 3 faces the question every real supervisor
meets next: it starts as root — it has to, to become another user — and
everything it launches must *not* be root. The one new idea is that identity
on Linux is not one number but a handful of small state machines — three uids
and five capability sets — and the transitions between them are
order-sensitive. The same five syscalls in the wrong order either leave the
child privileged (you proved nothing) or strip the one capability it needed
(the child cannot do its job). `pmon drop` performs the transition correctly,
carrying exactly `CAP_NET_BIND_SERVICE` across a full identity change and an
`execve(2)`; `pmon bindprobe` is the witness, printing one line that is only
possible if the drop worked: an unprivileged uid bound to port 80.

The code is in `examples/14-identity-and-privilege/`. This is a **VM
example** — the drop path needs real root, so `./demo.sh <lang> run` deploys
to the `systems-target` guest when `TARGET` is set; the `README.md` there
specifies the CLI, the exit-code contract, and the paired
positive/negative verification.

{% include excalidraw.html
   file="14-credential-sets-caps"
   alt="Two bands. The top band shows the three uids — real, effective, saved — each moving 0 to 65534, with a note that setresuid moves all three at once and the saved uid is the way back that the drop deliberately destroys. The bottom band shows pmon's kept-cap path as six boxes: pmon as root with full permitted and effective sets, prctl PR_SET_KEEPCAPS, setresuid to 65534 with permitted kept, capset pinning 0x400 into permitted inheritable and effective, prctl AMBIENT_RAISE putting 0x400 into ambient, and execve into bindprobe bound to port 80 at uid 65534. A dashed negative-control row underneath skips the prctl and capset steps: setresuid scrubs permitted to zero and execve reaches bindprobe with every set zero and bind denied, exit 3. Notes carry the real /proc status values: the kept-cap child reads 0000000000000400 in CapInh CapPrm CapEff CapAmb with CapBnd 000001ffffffffff."
   caption="Figure 14.1 — the three-uid state machine and the five capability sets across pmon's drop; every hex value comes from /proc/&lt;pid&gt;/status on systems-target, read during the cross-check below" %}

> **Tools used** — `ssh`/`scp` and the Python runner
> (`scripts/test-all-examples.py`) on the host; `umask`, `touch`, `ls`
> (host); `getpcaps`, `capsh`, `getcap`, `sysctl`, `pgrep`, `bpftrace`
> (systems-target VM). Everything here is checked by
> `scripts/check-host.sh`, ships with Fedora's `libcap`, or is preinstalled
> in the lab VMs by cloud-init.

## Three uids, one state machine

Every process carries three user ids, and `/proc` shows them side by side —
this is the dropped child from the cross-check later in the chapter:

```bash
[vm]$ grep Uid /proc/8623/status
Uid:	65534	65534	65534	65534
```

Left to right: **real**, **effective**, **saved**, and filesystem uid (a
Linux relic that shadows the effective uid; NFS servers use it, nothing in
this book will). The *real* uid is who you are for accounting and for who
may signal you. The *effective* uid is the one that matters: every
permission check the kernel makes reads it. The *saved* uid is the escape
hatch — it exists so a set-uid program can drop its effective uid to do
unprivileged work and later climb back to the saved copy.

That escape hatch is also the source of thirty years of confusion, because
the classic calls mutate the machine implicitly. `setuid(2)` sets all three
ids if you are privileged but only the effective uid if you are not — the
same call is either a permanent drop or a temporary one depending on who
runs it. `seteuid(2)` moves only the middle value. `setreuid(2)` updates
the saved uid as a *side effect*, under rules baroque enough that the
manual page documents them as conditions. Daemons of the 1990s performed a
"setuid dance" across these calls and regularly got it wrong in one
direction — the direction where root comes back.

`setresuid(2)` ends the dance by making the state machine explicit: you
name all three values in one call (`-1` means leave that one alone), the
kernel applies them atomically, and the call either fully succeeds or
changes nothing. `pmon` calls `setresuid(uid, uid, uid)` — all three to
65534 — which is deliberately irreversible: with no root left in *any* of
the three slots, there is no privileged state left to return to, and a
later `setresuid(0,0,0)` fails. One ordering rule already applies here:
groups first. `setgroups(2)` and `setresgid(2)` are themselves privileged
operations, so they must run while the effective uid is still 0 — drop the
uid first and you are locked out of fixing the gids, leaving the child in
root's groups.

## umask and the file bits, quickly

File permission bits are the *other* identity mechanism, and one detail of
them belongs in this chapter because it silently edits every file you
create: the umask. A `creat`/`open(O_CREAT)` mode is filtered as
`mode & ~umask` before it ever reaches the inode. On the reference host:

```bash
[host]$ umask
022
[host]$ touch umask-demo.txt && ls -l umask-demo.txt
-rw-r--r-- 1 rsedor rsedor 0 Jul 18 23:56 umask-demo.txt
[host]$ (umask 077 && touch umask-demo-077.txt && ls -l umask-demo-077.txt)
-rw------- 1 rsedor rsedor 0 Jul 18 23:56 umask-demo-077.txt
```

Same `touch`, same requested 0666: umask 022 clears group/other write
(0644), umask 077 clears everything but owner (0600). The umask is
inherited across `fork` and `execve` like the uids are, which is why the
verify script `chmod 755`s the binary it stages on the guest instead of
trusting whatever mode survived `scp` — permissions you did not state
explicitly are permissions someone else chose. And mode bits are exactly
why this chapter stages that copy in `/tmp` at all: `/home/fedora` is mode
`700`, so once `pmon` becomes `nobody`, a binary under it is unreachable —
the drop is real enough to lock the child out of its own image.

## Capabilities: five process sets, three file sets

Root's power is not one bit; since Linux 2.2 it is ~41 distinct
**capabilities** — `CAP_NET_BIND_SERVICE` is the one that lets a process
bind ports below `net.ipv4.ip_unprivileged_port_start` (1024 on the guest,
confirmed below). Each *process* carries five sets, visible as the `Cap*`
lines of `/proc/<pid>/status`:

- **Permitted** — the ceiling: capabilities the process may use or raise.
- **Effective** — the subset the kernel actually consults in checks.
- **Inheritable** — offered across `execve`, but only into binaries whose
  *file* inheritable set accepts them.
- **Bounding** — a mask over what `execve` can ever grant. On the guest it
  reads `000001ffffffffff` for every process, root or not: it masks, it
  never grants.
- **Ambient** — the modern one, added in Linux 4.3: capabilities that
  survive an `execve` by a non-root process into a plain, uncapped binary.

Each *file* can carry three pieces in its `security.capability` xattr
(permitted, inheritable, and a one-bit effective flag — set with
`setcap(8)`, read with `getcap(8)`), and `execve` combines the two sides:
the new permitted set is the file's permitted masked by bounding, plus the
intersection of the process and file inheritable sets, plus ambient.

Why does ambient exist? Because the inheritable set is a trap. Before 4.3,
a supervisor that wanted to pass one capability to an unprivileged child
had to intersect its inheritable set with the *file's* inheritable set —
and ordinary binaries have no file capabilities at all, so the
intersection was empty and the capability died at `execve`. Your options
were `setcap` on every binary you might run (a fleet-management problem
and useless for scripts) or keeping root. Ambient is the bridge: a
capability raised into the ambient set — legal only while it is in both
permitted and inheritable — flows through a non-root `execve` of an
uncapped binary intact. The cross-check proves the child's capability came
this way and not from the file: `getcap` on the staged binary prints
nothing.

Root barely notices this machinery, and that is by design: at `execve`,
a uid-0 process is treated as if the file granted everything, which is why
a root shell on the guest shows `CapPrm` and `CapEff` of
`000001ffffffffff` with `CapInh` and `CapAmb` both zero. That empty
inheritable set matters in the next section.

## The ordering trap

The whole lesson of `pmon drop` is that five correct syscalls in the wrong
order fail silently. The correct sequence, as the C++ header comment
states it:

1. `prctl(PR_SET_KEEPCAPS, 1)` — **before** the uid change. When a
   process's uids go from 0 to all-nonzero, the kernel scrubs the
   permitted and effective sets; `KEEPCAPS` preserves permitted (effective
   is cleared regardless).
2. `setgroups` / `setresgid` / `setresuid` — become the target user,
   groups and gid while still privileged.
3. `capset(2)` — pin `CAP_NET_BIND_SERVICE` into permitted **and**
   inheritable, re-raise it into effective (cleared by step 2), and drop
   every other capability KEEPCAPS retained.
4. `prctl(PR_CAP_AMBIENT_RAISE, CAP_NET_BIND_SERVICE)` — legal only
   because step 3 put the cap in both permitted and inheritable.
5. `execvp(3)` — the ambient set is the only one that survives this for a
   non-root process running an uncapped binary.

Each step is a fence the others depend on. Skip step 1 and step 2 scrubs
permitted — steps 3 and 4 then have nothing to work with. Run step 3
before step 2 and you have dropped `CAP_SETUID`/`CAP_SETGID`, so the uid
change itself fails. Try step 4 without step 3 and `prctl` returns `EPERM`
because root's inheritable set is empty — the raise requires permitted ∩
inheritable. Skip step 4 and everything is technically held right up to
`execve`, where permitted and effective are recomputed from file caps the
binary does not have, and the capability evaporates. The negative control
run is exactly this failure family made observable: without `--keep-cap`,
steps 1, 3, and 4 never happen and the child arrives with every set zero.

## How the code works

The error plumbing comes first because every step uses it. C++ and Rust
declare the same two-field failure record — `SysErr { what, errno }` — and
a `checked(rc, what)` adapter that turns a negative syscall return into
`std::expected` / `Result` carrying the captured errno. All five steps
thread through it with `?`-style propagation, and exactly one printer at
the top formats `drop: <what>: <strerror>`. There is no `perror` scattered
mid-sequence: a failed step must abort the whole drop, because continuing
past, say, a failed `setresuid` would `execvp` a root child.

The heart is `arrange_and_exec`, and the three languages split into two
philosophies:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Perform the identity change (and optional cap retention), then execvp CMD.
// Returns only on failure; on success control never comes back.
std::expected<void, SysErr> arrange_and_exec(const passwd& pw, bool keep_cap,
                                             std::span<char*> cmd) {
  // (1) Retain the permitted set across the coming uid change. Must precede
  //     setresuid — after it, it is too late.
  if (keep_cap) {
    if (auto r = checked(::prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0), "PR_SET_KEEPCAPS");
        !r)
      return r;
  }

  // (2) Become the target user: groups first, gid before uid — once uid drops
  //     we can no longer change gids.
  gid_t gid = pw.pw_gid;
  if (auto r = checked(::setgroups(1, &gid), "setgroups"); !r) return r;
  if (auto r = checked(::setresgid(gid, gid, gid), "setresgid"); !r) return r;
  if (auto r = checked(::setresuid(pw.pw_uid, pw.pw_uid, pw.pw_uid), "setresuid");
      !r)
    return r;

  // (3) Pin exactly CAP_NET_BIND_SERVICE into permitted+inheritable (ambient
  //     demands the cap live in both), dropping every other retained cap.
  if (keep_cap) {
    __user_cap_header_struct hdr{};
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    std::array<__user_cap_data_struct, 2> data{};
    const std::uint32_t bit = 1u << CAP_NET_BIND_SERVICE;  // cap 10, word 0
    data[0].effective = bit;
    data[0].permitted = bit;
    data[0].inheritable = bit;
    if (auto r = checked(::syscall(SYS_capset, &hdr, data.data()), "capset"); !r)
      return r;

    // (4) Raise it into the ambient set — the only set an unprivileged execve
    //     of a file without file-caps carries forward.
    if (auto r = checked(::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE,
                                 CAP_NET_BIND_SERVICE, 0, 0),
                         "PR_CAP_AMBIENT_RAISE");
        !r)
      return r;
  }

  // (5) Hand off. CMD now runs as <name>, keeping :80 only if we raised it.
  std::vector<char*> args(cmd.begin(), cmd.end());
  args.push_back(nullptr);
  ::execvp(args[0], args.data());
  return std::unexpected(SysErr{std::string("execvp ") + args[0], errno});
}
```

```go
	u, err := user.Lookup(name)
	if err != nil {
		fmt.Fprintf(os.Stderr, "drop: unknown user: %s\n", name)
		return 1
	}
	uid, _ := strconv.Atoi(u.Uid)
	gid, _ := strconv.Atoi(u.Gid)

	c := exec.Command(cmd[0], cmd[1:]...)
	c.Stdin, c.Stdout, c.Stderr = os.Stdin, os.Stdout, os.Stderr
	// The runtime's fork/exec child applies these in the kernel-correct order:
	// PR_SET_KEEPCAPS, setgroups/setgid, setuid, capset, PR_CAP_AMBIENT_RAISE.
	c.SysProcAttr = &syscall.SysProcAttr{
		Credential: &syscall.Credential{Uid: uint32(uid), Gid: uint32(gid)},
	}
	if keepCap {
		c.SysProcAttr.AmbientCaps = []uintptr{unix.CAP_NET_BIND_SERVICE}
	}

	if err := c.Run(); err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) {
			return ee.ExitCode() // propagate CMD's own exit status (e.g. 3)
		}
		fmt.Fprintf(os.Stderr, "drop: %v\n", err)
		return 1
	}
	return 0
```

```rust
#[repr(C)]
struct CapHeader {
    version: u32,
    pid: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CapData {
    effective: u32,
    permitted: u32,
    inheritable: u32,
}

/// Drop to `uid`/`gid`, optionally retaining CAP_NET_BIND_SERVICE, then exec.
/// Returns only on failure; on success control never comes back.
fn arrange_and_exec(
    uid: libc::uid_t,
    gid: libc::gid_t,
    keep_cap: bool,
    cmd: &[String],
) -> Result<(), SysErr> {
    // (1) Retain the permitted set across the uid change (must precede it).
    if keep_cap {
        check(
            unsafe { libc::prctl(libc::PR_SET_KEEPCAPS, 1, 0, 0, 0) } as i64,
            "PR_SET_KEEPCAPS",
        )?;
    }

    // (2) Become the target user: groups, then gid, then uid.
    check(unsafe { libc::setgroups(1, &gid) } as i64, "setgroups")?;
    check(
        unsafe { libc::setresgid(gid, gid, gid) } as i64,
        "setresgid",
    )?;
    check(
        unsafe { libc::setresuid(uid, uid, uid) } as i64,
        "setresuid",
    )?;

    // (3) Pin exactly CAP_NET_BIND_SERVICE into permitted+inheritable.
    if keep_cap {
        let hdr = CapHeader {
            version: LINUX_CAPABILITY_VERSION_3,
            pid: 0,
        };
        let bit = 1u32 << CAP_NET_BIND_SERVICE; // cap 10, word 0
        let data = [
            CapData {
                effective: bit,
                permitted: bit,
                inheritable: bit,
            },
            CapData {
                effective: 0,
                permitted: 0,
                inheritable: 0,
            },
        ];
        check(
            unsafe { libc::syscall(libc::SYS_capset, &hdr as *const CapHeader, data.as_ptr()) },
            "capset",
        )?;

        // (4) Raise it into the ambient set.
        check(
            unsafe {
                libc::prctl(
                    libc::PR_CAP_AMBIENT,
                    libc::PR_CAP_AMBIENT_RAISE as libc::c_ulong,
                    CAP_NET_BIND_SERVICE as libc::c_ulong,
                    0,
                    0,
                )
            } as i64,
            "PR_CAP_AMBIENT_RAISE",
        )?;
    }

    // (5) Hand off to CMD.
    let prog = CString::new(cmd[0].as_str()).map_err(|_| SysErr {
        what: "bad CMD".into(),
        errno: libc::EINVAL,
    })?;
    let cargs: Vec<CString> = cmd
        .iter()
        .map(|s| CString::new(s.as_str()).unwrap_or_default())
        .collect();
    let mut ptrs: Vec<*const c_char> = cargs.iter().map(|c| c.as_ptr()).collect();
    ptrs.push(std::ptr::null());
    unsafe { libc::execvp(prog.as_ptr(), ptrs.as_ptr()) };
    Err(SysErr {
        what: format!("execvp {}", cmd[0]),
        errno: last_errno(),
    })
}
```

C++ and Rust do the dance **in-process** with raw syscalls — no libcap on
purpose, so every step is visible. The `capset` ABI deserves a close look
because glibc does not even wrap it (the "public" API is libcap's; we call
`syscall(SYS_capset, …)` directly). The header names ABI version 3
(`0x20080522`) and pid 0, meaning "me"; version 3 represents the 64-bit
capability space as **two 32-bit words**, which is why `data` is a
two-element array. `CAP_NET_BIND_SERVICE` is capability number 10, so its
bit lives in word 0 and word 1 is deliberately all zero — writing zeros
there is what drops the *other* nine retained-by-KEEPCAPS capabilities
above 31. Setting `effective = bit` in the same call does double duty: the
uid change cleared the effective set, and this is what re-raises it. Rust
mirrors the C++ byte for byte, down to `#[repr(C)]` on the two structs so
their layout matches the kernel's, and defines the two constants the
`libc` crate omits (`LINUX_CAPABILITY_VERSION_3`, `CAP_NET_BIND_SERVICE`)
locally — both are stable kernel ABI.

Go is the deliberate inversion, and the reason is a rule worth memorizing:
**credentials are per-thread at the kernel level**, and a Go program is
multithreaded before `main` runs. Instead of attempting the dance
in-process, the Go build describes the drop declaratively —
`SysProcAttr.Credential` for step 2, `AmbientCaps` for steps 1, 3, and 4 —
and the runtime's fork/exec child, where exactly one thread exists,
replays the same kernel-correct order between `fork` and `exec`. Same
observable behavior; the ordering lives in the standard library instead of
in our code. The `ExitError` branch matters too: `drop` promises to
propagate CMD's own exit status, which is how the negative control's
exit 3 travels back through `pmon` to the verify script intact.

`bindprobe` is short by design: socket, `SO_REUSEADDR` (so rapid repeat
runs do not trip over `TIME_WAIT`), bind on `INADDR_ANY`, then one line —
`bindprobe: uid=<u> bound :<port>` — and exit 0, or the bind failure with
`strerror` text on stderr and exit 3. It prints `getuid()` precisely so
one line carries both facts the verification needs: *who* the process was
and *what* it was allowed to do.

Fragile bits, stated plainly: `--keep-cap` accepts only
`net_bind_service` — the cap number and its word-0 position are hardcoded,
and generalizing to caps ≥ 32 means touching word 1; the staged binary
must live somewhere the dropped user can read (`/tmp`, not `/home/fedora`);
and `pmon` does not call `setsid` or close inherited fds — real daemons
layer those on, and chapter 7's `O_CLOEXEC` discipline is half of why they
can skip the fd audit.

## Errors, three ways

The output contract is byte-identical diagnostics across languages, and
capabilities add a twist: the interesting failure happens in the *child*,
after `execve`, where `pmon`'s error plumbing no longer exists. So the
contract splits in two. Setup failures — not root, unknown user, a failed
step — flow through `SysErr`: C++ formats `std::expected` errors with
`std::strerror`, Rust calls `libc::strerror` through a safe wrapper, and
both exit 1 from a single printer. The child's bind failure is reported by
`bindprobe` itself with exit 3, and Go's `drop` forwards that code via
`errors.As(err, &ee)` → `ee.ExitCode()`. The subtle one is Go's
`errnoText`: Go spells errno strings lower-case (`permission denied`)
while `strerror(3)` capitalizes (`Permission denied`), so the Go build
re-capitalizes the first letter — without it, `verify.lua`'s
byte-for-byte match on `bindprobe: bind :80: Permission denied` would fail
in one language out of three. Usage errors exit 2 everywhere, before any
privileged call is attempted.

## Concurrency lens

`setresuid(2)` and `capset(2)` change the credentials of the **calling
thread**, not the process. Single-threaded C and C-like programs never
notice; glibc papers over it for threaded ones with its setxid machinery,
signaling every thread to run the same `setresuid` so the process moves in
lockstep. But `pmon`'s C++ and Rust builds bypass glibc for `capset`
(raw `syscall`), so their safety argument is structural instead: both are
**deliberately single-threaded** — no thread pool, no async runtime —
because a privilege drop must cover every task, and `execve` replaces the
whole process anyway. Go cannot make that promise: the runtime spawns
threads before `main`, and a raw in-process dance would leave sibling
threads still root — a real vulnerability class, not a theoretical one.
That is exactly why the Go build pushes the entire sequence into the
fork/exec child. The general rule: change identity either before threads
exist or in a fresh child — never "around" a running thread pool.

## Build, run, observe

```bash
[host]$ LIBVIRT_DEFAULT_URI=qemu:///system \
        python3 scripts/test-all-examples.py --only 14-identity-and-privilege --mode vm
```

On this host with the lab up, the runner built all three languages and
verified each one against `systems-target` (Fedora 44, kernel
6.19.10-300.fc44):

```
example                    cpp   go    rust
14-identity-and-privilege  PASS  PASS  PASS

3 passed, 0 failed, 0 skipped
```

Each language's verify run asserts six things, and the two that carry the
chapter are the paired runs. By hand (the verify script stages
`/tmp/pmon-verify`, a world-readable copy of the binary, first):

```bash
[host]$ cd examples/14-identity-and-privilege
[host]$ export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target
[host]$ SUDO=1 ./demo.sh cpp run drop --user nobody --keep-cap net_bind_service -- /tmp/pmon-verify bindprobe --port 80
→ copying app to fedora@192.168.124.7:/home/fedora/app
→ running on systems-target (Ctrl-C to stop):
bindprobe: uid=65534 bound :80
[host]$ echo $?
0
```

That single line is the positive proof: uid 65534 (`nobody`) is not root,
yet the bind on :80 succeeded. And the negative control — the same drop
with `--keep-cap` removed:

```bash
[host]$ SUDO=1 ./demo.sh cpp run drop --user nobody -- /tmp/pmon-verify bindprobe --port 80
→ copying app to fedora@192.168.124.7:/home/fedora/app
→ running on systems-target (Ctrl-C to stop):
bindprobe: bind :80: Permission denied
[host]$ echo $?
3
```

(An ssh pseudo-terminal warning line is trimmed from both transcripts.)
The Go and Rust binaries produce these exact same two lines and exit
codes — that is what `verify.lua` asserts per language. The pair matters
more than either half: without the negative control, a lowered
`ip_unprivileged_port_start` or a stray root uid would make the positive
run pass while proving nothing about capabilities.

## Cross-check on systems-target

`bindprobe` exits as soon as it has bound, so to inspect the dropped
child's credential state directly, run the same drop around a `sleep` and
read the child while it lives — three independent tools against the same
process:

```bash
[vm]$ sudo /home/fedora/app drop --user nobody --keep-cap net_bind_service -- sleep 120 &
[vm]$ pgrep -u nobody sleep
8623
[vm]$ getpcaps 8623
8623: cap_net_bind_service=eip
[vm]$ grep Cap /proc/8623/status
CapInh:	0000000000000400
CapPrm:	0000000000000400
CapEff:	0000000000000400
CapBnd:	000001ffffffffff
CapAmb:	0000000000000400
[vm]$ capsh --decode=0000000000000400
0x0000000000000400=cap_net_bind_service
```

Read the layers against Figure 14.1: `getpcaps` names the one capability
with `=eip` (effective, inheritable, permitted); the raw `/proc` view
shows the same bit — `0x400` is `1 << 10` — in all four transferable sets
including ambient, which is the set that carried it through `execve`; and
`capsh --decode` closes the loop by translating the hex back to the name.
`CapBnd` is still the full mask because `pmon` never touched the bounding
set. The negative-control child, inspected the same way, is the mirror
image:

```bash
[vm]$ sudo /home/fedora/app drop --user nobody -- sleep 120 &
[vm]$ getpcaps 8746
8746: =
[vm]$ grep Cap /proc/8746/status
CapInh:	0000000000000000
CapPrm:	0000000000000000
CapEff:	0000000000000000
CapBnd:	000001ffffffffff
CapAmb:	0000000000000000
```

Two more readings pin down where the capability did *not* come from.
`getcap /tmp/pmon-verify` prints nothing — no file capabilities, so the
ambient path is the only route the bit had. And
`sysctl net.ipv4.ip_unprivileged_port_start` reports `1024`, ruling out
the sysctl shortcut that would have made :80 free for everyone. For
contrast, a root shell on the same guest reads
`CapPrm`/`CapEff` `000001ffffffffff` with `CapInh` and `CapAmb` zero —
full power, nothing inheritable, nothing ambient: the default state the
drop starts from.

> **On the lab VM** — you can also watch the kernel make the decision
> itself. `bpftrace` on the `cap_capable` hook, filtered to the probe
> process, during both runs:
>
> ```bash
> [vm]$ sudo bpftrace -e 'kprobe:cap_capable /comm=="pmon-verify"/ { @cap[tid]=arg2 } kretprobe:cap_capable /@cap[tid]/ { printf("%s uid=%d cap=%d ret=%d\n", comm, uid, @cap[tid], retval); delete(@cap[tid]); }'
> pmon-verify uid=65534 cap=10 ret=0
> pmon-verify uid=65534 cap=10 ret=-1
> ```
>
> Capability 10 is `CAP_NET_BIND_SERVICE`: the kept-cap run's check
> returns 0 (granted) and the negative control's returns -1 (`EPERM`) —
> the same verdict the bind lines reported, observed at the kernel hook
> that issued it. (Both runs also emit a burst of `cap=21` — 
> `CAP_SYS_ADMIN` — denials from unrelated kernel probing; only the
> `cap=10` verdicts differ. bcc-tools' `capable` would show the same
> stream with symbolic names, but it failed to compile against kernel
> 6.19 on this guest; Part 8 works through that toolchain properly.)

One deliberate non-observation: you cannot follow the drop with `strace`
from outside, because ptrace attachment is severed at a privilege
transition. The port bind *is* the observation surface here — and there is
no dashboard panel by design; this example emits no telemetry.

## What you learned

- The three uids are a state machine, and `setresuid(u, u, u)` is the one
  transition that says what it does: all three slots move atomically, the
  saved-uid escape hatch is destroyed, and the drop is irreversible —
  groups and gid must move first, while you are still privileged.
- Capabilities live in five process sets and three file sets; `execve`
  recombines them, and the ambient set exists so a non-root process can
  carry a capability into an ordinary uncapped binary — the case the
  inheritable set was never able to serve.
- Ordering is the whole game: `PR_SET_KEEPCAPS` before `setresuid`,
  `capset` into permitted *and* inheritable after it, `PR_CAP_AMBIENT_RAISE`
  last — and in Go the same sequence must run in the fork/exec child,
  because credentials are per-thread and the runtime is threaded before
  `main`.
- A privilege claim needs a negative control: `uid=65534 bound :80`
  proves nothing until the same drop without the cap yields
  `Permission denied` — paired evidence, checked by `getpcaps`,
  `/proc/<pid>/status` + `capsh --decode`, and the kernel's own
  `cap_capable` verdict.

Next, **virtual memory**: what an address actually is, why `pmon`'s
children each believe they own the whole address space, and what a page
fault costs.

---

<p><span class="status status--verified">verified</span> — every transcript
and number above was produced this session against the
<code>systems-target</code> guest (Fedora 44, kernel 6.19.10-300.fc44):
the runner printed <code>14-identity-and-privilege  PASS  PASS  PASS</code>
(3 passed, 0 failed, 0 skipped) in vm mode; the positive run emitted
<code>bindprobe: uid=65534 bound :80</code> (exit 0) and the negative
control <code>bindprobe: bind :80: Permission denied</code> (exit 3), both
byte-identical across cpp/go/rust; the kept-cap <code>sleep</code> child
(pid 8623) read <code>cap_net_bind_service=eip</code> from
<code>getpcaps</code> and <code>0000000000000400</code> in
CapInh/CapPrm/CapEff/CapAmb with CapBnd <code>000001ffffffffff</code>,
decoded by <code>capsh</code>; the negative child (pid 8746) read all-zero
sets; <code>getcap</code> on the staged binary printed nothing;
<code>ip_unprivileged_port_start</code> was 1024; and the
<code>bpftrace</code> <code>cap_capable</code> trace showed
<code>cap=10 ret=0</code> vs <code>ret=-1</code> live on the guest. The
umask lines were run on the reference host. bcc's <code>capable</code>
failing to build on kernel 6.19 was also observed, as stated.</p>
