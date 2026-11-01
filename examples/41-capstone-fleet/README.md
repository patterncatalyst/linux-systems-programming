# 41-capstone-fleet: the final project fleet

This is the final project: one binary, four subcommands, that assembles four
of the book's six recurring programs (`pmon`, `chatterd`, `sysagent`,
`fwatch`) into a single supervised system and runs it on two real hosts.
`pmon` is the fleet's own init: it drops the capability bounding set and sets
`no_new_privs`, then self-re-execs the same binary three times with three
different `argv[0]` commands, restarting whichever child exits unexpectedly
and forwarding its own `SIGTERM`/`SIGINT` to every child it supervises.

```
app pmon [--node NAME] [--sandbox-dir DIR] [--peer HOST:PORT] [--peer-node NAME]
         [--chatterd-port P] [--health-interval-ms N]
app chatterd serve [--host H] [--port P] [--node NAME] [--peer HOST:PORT] [--peer-node NAME]
app chatterd send   --host H --port P --nick NICK --text TEXT [--timeout-ms T]
app chatterd listen --host H --port P --nick NICK [--timeout-ms T]
app sysagent [--node NAME] [--interval-ms N] [--once]
app sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]
app fwatch snapshot DIR
app fwatch watch DIR [--sandbox] [--timeout-ms T]
```

A loopback run shows the whole fleet in one place:

```console
$ ./go/bin/app pmon --node p1 --sandbox-dir /tmp/fwatch-sandbox
pmon: capabilities bounding_set_dropped=0 no_new_privs=1
pmon: started service=chatterd pid=<PID>
chatterd: listening on 0.0.0.0:47100 node=p1
pmon: started service=sysagent pid=<PID>
sysagent: node=p1 cpu_pct=<CPU> mem_pct=<MEM> load1=<LOAD> ts=<TS>
pmon: started service=fwatch pid=<PID>
fwatch: landlock ABI=9 enforced dir=/tmp/fwatch-sandbox
pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0
^C
pmon: shutdown
```

`pmon` never execs a *different* binary for the other three services: it
resolves its own running path (`os.Executable()` in Go,
`readlink("/proc/self/exe")` in C++ and Rust) and forks+execs that same path
with three different argument lists, the same self-re-exec technique a
container entrypoint uses elsewhere in this book.

## pmon: capability drop, self-re-exec, supervise, forward signals

Before forking anything, `pmon` loops `prctl(PR_CAPBSET_DROP, c, …)` across
every known capability and ignores `EINVAL` once it runs past whatever the
running kernel's last capability is, then calls
`prctl(PR_SET_NO_NEW_PRIVS, 1, …)`. `PR_CAPBSET_DROP` removes a capability
from the *bounding set*, not the current effective set: the guarantee is that
no future `execve` of a setuid-root binary, by this process or any
descendant, can ever regain that capability, because the bounding set only
shrinks across `fork`/`exec`. That guarantee holds even on an
already-unprivileged account, which is why a loopback run prints
`pmon: capabilities bounding_set_dropped=0 no_new_privs=1`: `0` is correct
here, there was nothing left to drop, and the line still proves the `prctl`
calls ran.

Each of the three services then gets its own supervisor (a goroutine in Go,
a `std::thread` in C++ and Rust) running the same loop: fork+exec, record the
pid, print `pmon: started service=NAME pid=PID`, block on the child's exit,
and if the exit was not requested, count a restart, print
`pmon: restart service=NAME attempt=N reason=…`, back off 300ms, and retry.
A single failed start is logged and retried, never treated as fatal, because
a supervisor's job is to keep trying. `SIGTERM`/`SIGINT` are consumed once
(a dedicated signal-handling goroutine/thread, not a signal handler) and
forwarded to every recorded child pid, and `pmon` prints a
`pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0`
line on every `--health-interval-ms` tick until shutdown, when it prints
`pmon: shutdown` and exits `0`.

## chatterd, bridged across two hosts

With `--peer HOST:PORT`/`--peer-node NAME` set on both sides, `chatterd`
bridges across two nodes with no protocol change: a **bridge worker** dials
the other node's server and joins its client registry under the nick
`bridge@<local-node>`. A local `MSG` is broadcast as `DELIVER` to every
registered connection, including a bridge connection dialed in from the
other side, exactly like any other client; when a bridge worker itself
receives a `DELIVER` over the connection it dialed, it re-broadcasts locally
with `includeBridges=false`, so a message never bounces back out over the
same bridge. The one new piece of logic is tagging: if the delivered nick
has no `@` in it yet, the bridge worker tags it with the *sending* node's
name before re-broadcasting. A listener on the far side sees
`chatterd: received from=dave@target text=…`: the `@target` suffix is the
proof the message crossed the wire, not that it was echoed locally. Either
side can dial first, or restart under `pmon`, because the bridge worker just
retries on a backoff until the connection succeeds.

## sysagent: cpu/mem/load1 as OTel gauges

`sysagent` reads the same three signals it always has (`cpu_pct` from
`/proc/stat`, `mem_pct` from `/proc/meminfo`, `load1` from `/proc/loadavg`)
and prints them every `--interval-ms` as
`sysagent: node=… cpu_pct=… mem_pct=… load1=… ts=…`. When
`OTEL_EXPORTER_OTLP_ENDPOINT` is set, those same three numbers are also
registered as OTel observable gauges (`sysagent.cpu.pct`, `sysagent.mem.pct`,
`sysagent.load1`), tagged with a `node` attribute and exported over
OTLP/HTTP on a periodic reader; with no endpoint set, `sysagent` prints
`sysagent: otel disabled (OTEL_EXPORTER_OTLP_ENDPOINT not set)` and runs
otherwise unchanged. `sysagent saturate --resource cpu|mem --seconds N
[--workers K|--mb M]` spins worker threads or holds a resident allocation so
a concurrently-running `sysagent`'s own gauges visibly move.

## fwatch under an optional Landlock ruleset

`fwatch watch DIR --sandbox` installs a Landlock ruleset scoped to the
watched directory before the `inotify` loop starts. It probes the running
kernel's enforced ABI with
`landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`, builds
the ruleset around that one directory, and calls `landlock_restrict_self`
after setting `no_new_privs`. `fwatch: landlock ABI=N enforced dir=…` prints
only once the kernel has accepted the restriction, and `N` is not a constant
the program carries: it is whatever the running kernel actually supports.
Capability-bounding-set drop (applied once, by `pmon`, before any child
exists) and Landlock (applied per-process, by `fwatch` itself) are this
fleet's two sandbox layers; seccomp is proven in an earlier example and is
not reapplied here.

## The two-VM lab

`examples/manifest.yaml` registers this example as `mode: vm-peer`,
`requires: [lgtm]`: the real demo runs `pmon` on both `systems-target`
(192.168.124.7) and `systems-peer` (192.168.124.95), bridged to each other,
each exporting OTLP through the libvirt gateway (192.168.124.1) to the
LGTM stack running on the host.

```console
[host]$ scp app fedora@192.168.124.7:~/app
[host]$ scp app fedora@192.168.124.95:~/app
[vm]$   OTEL_EXPORTER_OTLP_ENDPOINT=http://192.168.124.1:4318 \
          ./app pmon --node target --peer 192.168.124.95:47100 --peer-node peer
[peer]$ OTEL_EXPORTER_OTLP_ENDPOINT=http://192.168.124.1:4318 \
          ./app pmon --node peer --peer 192.168.124.7:47100 --peer-node target
```

Both guests reach full health (`pmon: health chatterd=up sysagent=up
fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0`), and a message sent on
`target` is delivered to a listener on `peer`:

```console
[vm]$   ./app chatterd send --host 127.0.0.1 --port 47100 --nick dave --text "hello across hosts"
[peer]$ ./app chatterd listen --host 127.0.0.1 --port 47100 --nick carol
chatterd: received from=dave@target text=hello across hosts
```

`fwatch --sandbox` on the lab guest's kernel (6.19.10) enforces a different
Landlock ABI than the host's own loopback run: the identical probe, two
different kernels, by design, not a discrepancy. `sysagent`'s gauges reach
the host's Prometheus labeled by node (`sysagent_cpu_pct{node="peer"}`),
confirming the guest-to-host OTLP path over the libvirt gateway.

## Layout

```
41-capstone-fleet/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI, run entirely on loopback
├── cpp/         # CMake preset build, demo.sh
├── go/          # go build, demo.sh
└── rust/        # cargo build, demo.sh
```

## The demo contract

Each language directory's `demo.sh` has the same interface as every other
example: `build`, `run [args]`, or bare (build then run). With `TARGET` set,
`run` deploys the built binary to that lab VM via
`scripts/lab/deploy-to-vm.sh` instead of running locally, which is how `app`
gets staged onto `systems-target` and `systems-peer` for the two-host run
above. The top-level `demo.sh` here is only a dispatcher.

## Verification

`verify.lua` follows the convention established elsewhere in the book for
`vm-peer` examples: the assertions themselves run entirely on loopback,
including the cross-node bridge (two nodes on distinct `127.0.0.1` ports)
and telemetry to a local LGTM stack, while the real two-host run is the
chapter's demo. Per language, it asserts:

1. **Usage and error shapes**: no args, `chatterd`/`fwatch` with no
   subcommand, `chatterd send` missing `--nick`/`--text`, and `sysagent
   saturate` missing `--resource` all exit 2 with the matching usage banner
   (`usage: app …` or `sysagent saturate`'s own, narrower banner).
2. **`sysagent --once`**: exit 0, the `sysagent: node=… cpu_pct=…` line, and
   `otel disabled` with no endpoint set.
3. **`fwatch`**: `snapshot DIR` lists directory entries; `watch DIR` reports
   `event: create|modify|delete PATH` lines; `watch DIR --sandbox` installs a
   Landlock ruleset and prints `fwatch: landlock ABI=N enforced dir=…`.
4. **`chatterd`**: a message sent to one node is received locally as
   `received from=NICK text=…`, and across the bridge (node A `--peer->`
   node B) a message sent on A is delivered on B as
   `received from=dave@A text=…`.
5. **The `pmon` fleet**: `pmon: capabilities bounding_set_dropped=N
   no_new_privs=1`, all three services start, health reaches
   `chatterd=up sysagent=up fwatch=up`, and `SIGTERM` produces
   `pmon: shutdown` and exit 0.
6. **Telemetry**: with `OTEL_EXPORTER_OTLP_ENDPOINT` set, `sysagent` prints
   `otel enabled` and its `sysagent_cpu_pct` gauge reaches Prometheus through
   Grafana's proxy. Skipped, not failed, if the LGTM stack is unreachable.

`python3 scripts/test-all-examples.py --only 41-capstone-fleet --mode vm`
builds and verifies all three languages.

## Mapping to the book

This example closes the arc that runs through the whole book: `pmon`
(fork/exec/wait, privilege drop, namespaces), `chatterd` (TCP, then epoll,
UDP peer discovery, async, a pinned-core fast path), `sysagent` (`/proc` and
`/sys/fs/cgroup` reading, then exporting), and `fwatch` (polling, then
`inotify`, then a Landlock sandbox around itself) each grew on its own in
earlier examples. This one does not add a new syscall; it assembles those
four into one supervised, sandboxed, observed system running on two real
hosts instead of four separate demos on one.
