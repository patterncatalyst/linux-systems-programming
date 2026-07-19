# 14 — Identity & privilege (`pmon` v3)

**VM example.** This chapter drops privilege the right way: a root process
hands a command to an unprivileged user while carrying exactly one capability —
`CAP_NET_BIND_SERVICE` — across the identity change and the following
`execve(2)`. The whole lesson is the **order of operations**. Get the sequence
wrong and either the capability evaporates (the command can't bind `:80`) or it
never drops (the command runs as root, and you've proven nothing).

`pmon drop` becomes user `<name>` and then `exec`s `CMD`. `pmon bindprobe`
tries to bind a TCP port and reports its uid — the observable proof: **binding a
port below 1024 as a non-root uid is impossible without the ambient cap.**

| Language | How the drop is performed |
|---|---|
| **C++23** | Raw `prctl(2)` / `capset(2)` / `setres[ug]id(2)` in-process, then `execvp`. Single-threaded on purpose (a privilege drop must cover every task, and `execve` only makes sense from one thread). RAII owns the socket; `std::expected` carries every syscall failure; no C-style `perror`/`exit`. |
| **Rust (edition 2024)** | The same raw `libc` sequence, mirroring the C++ path; `OwnedFd` gives the socket RAII and errors flow through `Result`/`?`. Capability structs are `#[repr(C)]`; the two ABI constants libc omits (`_LINUX_CAPABILITY_VERSION_3`, `CAP_NET_BIND_SERVICE`) are defined locally. |
| **Go 1.26** | The idiomatic inversion. A Go program is already multithreaded and `setuid`/`capset` act on the calling thread only, so doing this in-process is a footgun. Instead the drop is arranged **pre-exec** via `syscall.SysProcAttr` — `Credential` (setgid/setgroups/setuid) plus `AmbientCaps` (which the runtime turns into the `PR_SET_KEEPCAPS → capset → PR_CAP_AMBIENT_RAISE` dance in the fork/exec child). Same observable result; the ordering lives in the standard library instead of in our code. |

All three produce byte-for-byte identical output and exit codes, so one
`verify.lua` covers them.

## The sequence (C++/Rust in-process path)

```
1. prctl(PR_SET_KEEPCAPS, 1)     # BEFORE the uid change, so the permitted set
                                 # is not scrubbed when uid goes 0 -> non-zero
2. setgroups([gid])              # groups first...
   setresgid(gid, gid, gid)      # ...gid before uid (can't set gid once !root)
   setresuid(uid, uid, uid)      # now unprivileged, permitted caps retained
3. capset(CAP_NET_BIND_SERVICE)  # pin it into permitted AND inheritable
                                 # (ambient requires both), drop everything else
4. prctl(PR_CAP_AMBIENT_RAISE,   # raise into the ambient set — the only set a
        CAP_NET_BIND_SERVICE)    # non-root, no-file-cap execve preserves
5. execvp(CMD)                   # CMD runs as <name>, still able to bind :80
```

Drop `--keep-cap` and steps 1, 3 and 4 vanish: after `execvp` the child has an
empty permitted set (no file caps, non-root uid) and `:80` is denied. That's
the negative control.

## CLI

```
usage:
  pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]
  pmon bindprobe [--port 80]
```

- `drop --user <name>` — must run as root; becomes `<name>` and `exec`s `CMD`.
  With `--keep-cap net_bind_service` it carries `CAP_NET_BIND_SERVICE` across
  the drop; without it, `CMD` runs with no capabilities. `CMD`'s own exit
  status is propagated.
- `bindprobe [--port 80]` — binds a TCP socket on the port. On success prints
  `bindprobe: uid=<u> bound :<port>` to stdout and exits `0`. On failure prints
  `bindprobe: bind :<port>: <strerror>` to stderr and exits `3`.
- Exit codes: `0` success, `1` runtime error (not root, unknown user, exec
  failure), `2` usage error, `3` `bindprobe` bind failure.

## Demo contract

Each language directory has the standard `demo.sh`:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (`app`)
- With env `TARGET` set, `run` deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh`; set `SUDO=1` for the `drop` path, which needs
  root on the guest

The top-level `./demo.sh [cpp|go|rust|all|build]` dispatches per language.

## Try it (on the lab VM)

```sh
export LIBVIRT_DEFAULT_URI=qemu:///system TARGET=systems-target
cd examples/14-identity-and-privilege

# stage a world-readable copy of the exec target (/home/fedora is mode 700,
# unreachable by the dropped user; /tmp is world-accessible)
IP=$(../../scripts/lab/vm-ip.sh systems-target)
scp cpp/build/release/app "fedora@$IP:/tmp/pmon-verify"
ssh "fedora@$IP" 'chmod 755 /tmp/pmon-verify'

# keep the cap: nobody binds :80  ->  "bindprobe: uid=65534 bound :80", exit 0
SUDO=1 ./demo.sh cpp run drop --user nobody --keep-cap net_bind_service -- /tmp/pmon-verify bindprobe --port 80

# negative control: drop the cap  ->  "bindprobe: bind :80: Permission denied", exit 3
SUDO=1 ./demo.sh cpp run drop --user nobody -- /tmp/pmon-verify bindprobe --port 80
```

## Verification

`verify.lua` (run per language under the runner's **vm mode**,
`TARGET=systems-target` with `SUDO=1`) asserts the paired behavior that makes
this example real, not merely exit-0:

1. **Positive** — `drop --user nobody --keep-cap net_bind_service` running
   `bindprobe --port 80` prints `bindprobe: uid=65534 bound :80` and exits `0`.
   The uid is captured and asserted **non-zero**: an unprivileged process bound
   a privileged port, which is only possible because the ambient cap survived.
2. **Negative control** — the *same* drop **without** `--keep-cap` prints
   `bindprobe: bind :80: Permission denied` and exits `3`. Same uid, cap
   withheld, port denied — this is what proves assertion 1 came from the
   capability and not from a stray root or a lowered
   `net.ipv4.ip_unprivileged_port_start`.

Run it:

```sh
LIBVIRT_DEFAULT_URI=qemu:///system \
  python3 scripts/test-all-examples.py --only 14-identity-and-privilege --mode vm
```

**Why observe the effect, not a trace:** you cannot `strace` across a
`setuid`+`execve` boundary to confirm the cap moved — `ptrace` is dropped at
the privilege transition. The port bind *is* the observation: `:80` bound at
`uid=65534` is a fact the kernel only permits when `CAP_NET_BIND_SERVICE` is in
the effective set. The negative control closes the loop by showing the same
process denied when the cap is withheld.
