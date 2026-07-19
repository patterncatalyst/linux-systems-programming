# 32 — Namespaces & cgroups (`pmon` v6)

**VM example.** This chapter builds the two primitives every container
runtime is made of, by hand, with no runtime in between: **namespaces** to
give a process its own view of pid/mount/hostname/network, and a **cgroup
v2** subtree to cap what it's allowed to use. `pmon containerize` runs a
command inside both at once:

```
pmon containerize [--hostname NAME] [--mem-max BYTES|max]
                  [--cpu-max "QUOTA PERIOD"|max] [--cgroup NAME]
                  -- CMD [ARGS...]
```

It `unshare`s `CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|CLONE_NEWPID`, writes
`memory.max`/`cpu.max` (and disables swap) on a dedicated cgroup *before* CMD
ever runs, forks the pid-1 child into that setup, and — as the child, before
handing off to CMD — reports the two things that are otherwise invisible from
the outside:

```
pmon: child sees pid 1
pmon: hostname=<name>
```

...then execs CMD. Once CMD exits, the parent reports the cgroup-side
evidence and CMD's fate:

```
pmon: cgroup mem.pressure some=<x>
pmon: child exited status=<n>          # or: pmon: child killed signal=<n> (<NAME>)
```

## Why this is the real test, not an inference

- **pid 1** isn't a claim you can `strace` your way to from outside — a
  process either sees itself as pid 1 or it doesn't, and only a genuine
  `CLONE_NEWPID` child does.
- **The hostname** only proves the UTS namespace is real if it's compared
  against the *actual* host hostname, fetched independently — otherwise
  you're just checking that `sethostname(2)` didn't return an error.
- **The memory cap** only proves anything with a negative control: a hog
  given far less `memory.max` than it tries to allocate must be
  **OOM-killed** (`SIGKILL`, exit 137) before it finishes — not merely run
  slower. A cgroup that let the hog complete would be decorative, not
  enforcing.

## `pmon` lineage

Each chapter that grows `pmon` adds one thing under a subcommand of its own
(the CLI doesn't retroactively merge unrelated features into one command):

| Version | Chapter | Subcommand(s) added |
|---|---|---|
| v0 | 11 — process lifecycle | `run -- CMD` |
| v1 | 12 — signals | `supervise` (sigchld restart loop) |
| v2 | 13 — pidfd | `supervise --engine pidfd\|sigchld` |
| v3 | 14 — identity & privilege | `drop --user`, `bindprobe` |
| v4 | 18 — pipes/FIFOs/splice | `supervise --log`, `tail --fifo` |
| v5 | 19 — UNIX sockets | `supervise --ctl`, `pmctl status\|stop\|logfd` |
| **v6** | **32 — namespaces & cgroups** | **`containerize`** |

## The three implementations

Same two syscalls-worth of work, three ways to reach them:

| Language | Mechanism |
|---|---|
| **C++23** | Raw `unshare(2)` (mount/uts/net namespaces land on the caller immediately; the pid namespace is deferred to the next child), then raw `clone3(2)` via `<sys/syscall.h>` — `flags=0`, `stack=0` gives fork(2) semantics, `exit_signal=SIGCHLD` makes it `waitpid`-able. The child prints its proof lines, flushes, and `execvp`s CMD. `std::expected` carries every syscall failure; RAII (`Fd`) owns every descriptor; single-threaded throughout, on purpose — see the Go entry for what goes wrong when a runtime isn't. |
| **Rust (edition 2024)** | `nix::sched::unshare` + `nix::unistd::fork` (the same "no code runs between clone and exec except what we write" shape as C++'s `clone3(flags=0)`), `nix::mount::mount`, `nix::unistd::{sethostname, execvp}`. Errors flow through `Result`/`?`; cgroup file I/O is plain `std::fs`. |
| **Go 1.26** | The idiomatic inversion, one step further than chapter 14's: Go can't safely `fork()` and keep running Go code in the child (the runtime assumes a live multi-threaded process on both sides), so `unshare(2)` happens on a `runtime.LockOSThread()`-pinned goroutine, and the **fork+exec is a re-exec of pmon itself** (`/proc/self/exe __ns_init__ CMD...`) via `os/exec`, whose fork always runs on the calling goroutine's *current* OS thread — the one we just unshared and pinned. The freshly exec'd pmon is born inside the new namespaces (PID 1 included), prints the proof lines, and `syscall.Exec`s into the real CMD. `LockOSThread` is load-bearing: without it, the goroutine that called `Unshare` could be rescheduled onto a different, un-unshared OS thread before the fork, silently discarding every namespace change. |

All three: identical CLI, output lines, and exit codes, so one `verify.lua`
covers them.

## Cgroup mechanics worth knowing

- **The root cgroup is exempt from the "no internal process" rule.** A
  non-root cgroup can't hold processes directly *and* delegate a controller
  to children at the same time — but the root cgroup can, which is exactly
  how `containerize` enables `+memory +cpu` in `/sys/fs/cgroup/cgroup.subtree_control`
  without first relocating itself into a leaf.
- **Cgroup membership is inherited across fork, not reset by exec.**
  `containerize` writes its *own* pid into the new cgroup before unsharing
  or forking, so the pid-1 child — and CMD after it execs — are under the
  limits from their very first instruction.
- **`memory.swap.max=0` makes the limit mean something.** Without it, a
  cgroup that hits `memory.max` may just swap and slow down instead of
  triggering the OOM killer — on a host with swap enabled, the negative
  control would flake. `containerize` sets it (best-effort) alongside
  `memory.max`.
- **PSI (`memory.pressure`) is a per-cgroup file**, not a single systemwide
  number — reading `<cgroup>/memory.pressure` after the child exits shows the
  stall this specific cgroup experienced, independent of pressure anywhere
  else on the box.

## Demo contract

Each language directory has the standard `demo.sh`:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (`app`)
- With env `TARGET` set, `run` deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh`; set `SUDO=1` — `containerize` needs root for
  `unshare(2)` and cgroup writes.

The top-level `./demo.sh [cpp|go|rust|all|build]` dispatches per language.

## Try it (on the lab VM)

```sh
export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target
cd examples/32-namespaces-and-cgroups

# pid + uts namespaces: a trivial CMD proves both
SUDO=1 ./demo.sh cpp run containerize --hostname pmon-box -- /usr/bin/echo hi
#   pmon: child sees pid 1
#   pmon: hostname=pmon-box
#   hi
#   pmon: cgroup mem.pressure some=0.00
#   pmon: child exited status=0

# negative control: a real memory hog under a small memory.max is OOM-killed
IP=$(../../scripts/lab/vm-ip.sh systems-target)
ssh "fedora@$IP" 'cat > /tmp/hog.sh' <<'EOF'
#!/bin/sh
a=$(head -c 200000000 /dev/zero | tr '\0' x)
sleep 2
echo should-not-print
EOF
ssh "fedora@$IP" 'chmod +x /tmp/hog.sh'
SUDO=1 ./demo.sh cpp run containerize --mem-max 67108864 -- sh /tmp/hog.sh
#   pmon: child sees pid 1
#   pmon: hostname=pmon-containerized
#   pmon: cgroup mem.pressure some=<x>
#   pmon: child killed signal=9 (KILL)     <- never reaches "should-not-print"
```

Cross-check from a second guest shell while a `containerize` run is live:
`nsenter -t <pid> -p -m -u -n ps -ef` shows the contained process tree with
its own pid 1 and hostname, from the host's side of the fence.

## A deploy-to-vm.sh gotcha this example works around

`deploy-to-vm.sh` rebuilds the remote command line with a bare `$*`, which
flattens argv and loses any quoting a single argument relied on. A `CMD` like
`bash -c 'a=$(head -c N /dev/zero | tr ...); sleep 2'` — one argument full of
spaces and shell metacharacters — arrives on the guest corrupted. Both
`verify.lua` and the walkthrough above route around it the same way: keep
every `CMD` token free of embedded spaces (`/usr/bin/echo hi`, `sh
/tmp/hog.sh`), staging anything that needs real shell logic as a file on the
guest first.

## Verification

`verify.lua` (run per language under the runner's **vm mode**,
`TARGET=systems-target` with `SUDO=1`) asserts:

1. **`containerize -- /usr/bin/echo cmd-ran`** prints `pmon: child sees pid 1`
   and `pmon: hostname=pmon-vm-check`, the echo's own output appears, a
   `pmon: cgroup mem.pressure some=<x>` line is present, and the run exits 0
   with `pmon: child exited status=0`.
2. The guest's **real** hostname is fetched independently over `ssh` and
   asserted **different** from `pmon-vm-check` — the fact pmon is even
   allowed to compare the two is only interesting because they're both real,
   independently-obtained values.
3. **Negative control** — a real memory hog (`head -c 200000000 /dev/zero |
   tr '\0' x` captured into a shell variable, staged as a guest script) run
   under `containerize --mem-max 67108864` (64 MiB, far below the ~190 MiB it
   tries to hold) exits **137**, prints `pmon: child killed signal=9 (KILL)`,
   and never reaches its own trailing `echo should-not-print` — proof the
   cgroup limit terminated it mid-allocation rather than merely slowing it
   down.

Run it:

```sh
LIBVIRT_DEFAULT_URI=qemu:///system \
  python3 scripts/test-all-examples.py --only 32-namespaces-and-cgroups --mode vm
```

**Why observe the effect, not a trace:** there is no syscall you can
`strace` from *outside* a process to confirm it believes it's pid 1 — that
belief is only visible in what the process itself reports, or in what
happens to it. Pairing "reports pid 1" with an independently-fetched host
hostname, and pairing "cgroup configured" with "hog actually dies", is what
turns three plausible-looking print statements into a demonstration that the
namespaces and the cgroup limit are real.
