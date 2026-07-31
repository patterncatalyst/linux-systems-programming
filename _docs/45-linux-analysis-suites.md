---
title: "Linux analysis suites: Cockpit, SystemTap, and PCP watch a whole fleet"
order: 45
part: "Appendices: Tooling"
description: "linux-analysis-suites builds a single Fedora-based, multi-stage Containerfile (lsp45-pcp, 265 MB) shipping Performance Co-Pilot's pmcd with two live PMDAs -- openmetrics and podman -- run rootless with podman, alongside Cockpit's zero-scripting web console and SystemTap's kernel-module tracing on the systems-target VM: verified PASS 10/0 with a real pminfo value 42, live podman-socket container visibility, a working SystemTap kernel-module smoke test, and Cockpit's socket answering HTTP 200."
duration: "35 minutes"
---

Every observation tool Part 8 reached for — `strace`, `gdb`, bcc-tools,
`bpftrace` — answers questions about **one process**, watched while you sit
at the terminal running it. That framing stops scaling the moment there is
more than one machine to worry about: nobody wants to SSH into a fleet and
run `strace -p` by hand on each host. This chapter is the first of Part 13's
appendices, and it pivots from single-process tools to three fleet-wide
**suites**, each built on a different philosophy for the same underlying
question — what is this system doing right now. Cockpit answers it with a
web console you never script. SystemTap answers it by compiling a kernel
module per probe — bcc's harder-to-build sibling from Chapter 30. And PCP
(Performance Co-Pilot) answers it by pulling metrics out of things this book
already built: a `podman.sock` (Chapter 34) and an OpenMetrics document
(Chapter 38).

{% include excalidraw.html
   file="45-analysis-suite-landscape"
   alt="Three side-by-side boxes under a banner reading three philosophies for fleet-wide observation. Left: a browser icon into a Cockpit box, labeled zero-scripting web console, cockpit-ws multiplexing sessions over a socket answering on port 9090. Middle: a stap CLI box compiling into a kernel-module icon, labeled SystemTap -- bcc's harder-to-build kernel-module-compiling sibling from Chapter 30, needing matching kernel-devel. Right: a pmcd hub box with two labeled PMDA arms -- pmda-openmetrics reading a file:// document and pmda-podman reading a podman.sock -- both arrows curving back to two smaller ghost boxes captioned Chapter 34 containers and Chapter 38 OpenMetrics, showing PCP pulls metrics out of things this book already built. Caption: three ways to observe a fleet -- click, script, or scrape."
   caption="Figure 45.1 — three philosophies for fleet-wide observation: Cockpit's zero-scripting console, SystemTap's kernel-module scripting (bcc's harder-to-build sibling from Chapter 30), and PCP pulling metrics out of two things this book already built" %}

> **Tools used** — `podman` (host, build/run/exec — in
> `scripts/check-host.sh`); `pmcd`/`pminfo`/`pmrep`/`pcp` (a third location:
> **inside** the `lsp45-pcp` container this chapter builds, neither host nor
> VM); `cockpit`/`cockpit-podman` (`systems-target` VM, cloud-init;
> `cockpit.socket` enabled); `stap` (`systems-target` VM, root, kernel-module
> backend, needs `kernel-devel-$(uname -r)`); `curl` (host, socket check).

A note on structure before any of that: `examples/45-linux-analysis-suites/`
has no C++, Go, or Rust source at all. PCP, Cockpit, and SystemTap are
OS-level suites you point at a running system, not libraries you link into
one, so there is nothing per-language to write. `./demo.sh [cpp|go|rust]
[build|run]`'s language argument exists purely so
`scripts/test-all-examples.py`'s per-language loop has something to pass —
`examples/manifest.yaml` lists `langs: [cpp]` for exactly this reason,
running the check once rather than three times for no behavioral gain.
Whichever of the three is given, the image built and the container run are
byte-identical. Code excerpts below are therefore plain fenced blocks, never
the tri-language `codetabs` include this book uses everywhere else — there
is only one path through this example, the same handling Chapter 44 used for
its single-language runtime deep dive.

## Cockpit: a web console with no scripting required

Cockpit is the philosophy of "point a browser at port 9090 and read the
dashboard" — no query language, no client to install, no script to write.
`cockpit-ws` is the one process on the target machine; the browser does the
rendering, and a single WebSocket carries every session it multiplexes.
`cockpit-podman` extends that same console with a Podman-aware panel — the
containers, images, and pod state a fleet operator would otherwise have to
`podman ps` for by hand across every host. This chapter's `systems-target`
VM ships both, enabled by cloud-init: `cockpit.socket` is active and
answering before this chapter's demo ever runs.

## SystemTap: bcc's kernel-module-compiling sibling

Chapter 30 put SystemTap side by side with bcc-tools and bpftrace and found
its kernel-module backend blocked by a real `kernel-devel`/running-kernel
mismatch — a friction point flagged, not resolved, in that chapter. This
appendix is where it gets resolved: once `kernel-devel-$(uname -r)` matches
the running kernel, `stap` compiles a real, loadable kernel module per probe
script, runs it, and unloads it when the script exits — the same
observation goal as bcc's BPF programs, reached by compiling and inserting
an actual `.ko` instead of loading a verified BPF bytecode program. That
extra step — a real compile, against real kernel headers, every invocation
— is exactly why bcc and bpftrace are this book's default toolkit and
SystemTap is introduced as the sibling technology: more setup cost, same
observational reach once the setup is right.

## PCP: metrics pulled from what this book already built

Performance Co-Pilot takes a third stance again: instead of a console you
click through or a script you write per question, PCP runs one collector
daemon, `pmcd`, and lets independent **PMDAs** (Performance Metrics Domain
Agents) register whatever metrics they know how to produce into one shared
namespace, queryable with `pminfo`, `pmrep`, or the summary command `pcp`
itself. This chapter's image layers exactly two PMDAs, and both pull their
numbers from things this book already built rather than from anything new:
`pmda-openmetrics` bridges an OpenMetrics/Prometheus-style document — the
same format Chapter 38 wired a real exporter around — into PCP's namespace,
and `pmda-podman` reads container and pod state over a `podman.sock`, the
same rootless socket Chapter 34 built a container fleet around. PCP doesn't
introduce a new thing to observe; it gives a uniform namespace to metrics
this book's own examples already produce.

The openmetrics side is scraped from a tiny, real document this example
ships, not a live network endpoint:

```
# HELP lsp45_answer A known constant this chapter ships so the openmetrics
# PMDA has something real to scrape without any network access -- pointed at
# via a `file://` config.d source, not an HTTP endpoint.
# TYPE lsp45_answer gauge
lsp45_answer 42
```

`pmda-openmetrics`'s Python source supports a `file://` config.d source
scheme directly (`self.url.startswith('file://')` reads the path with a
plain `open()`), so pointing its config at `config/lsp45.prom` gives a real
scrape of a real file, every run, with zero network dependency.

### Why Fedora, not UBI

Every other container example in this book — Chapter 34's — builds on a
UBI 10 base, per this project's own convention. This one doesn't, and the
reason is worth stating plainly: checked directly against a real `dnf
install`, `pcp` and its PMDA packages are **not resolvable on UBI 9 or UBI
10** without a RHEL entitlement — no free, unauthenticated UBI repository
carries them, nor does EPEL for RHEL 10. `registry.redhat.io/rhel9/pcp`
exists, but it needs Red Hat portal authentication, and there is no
`rhel10/pcp` image at all. This book's readers, by this project's own
conventions, are assumed **not** to have a RHEL subscription. Fedora's own
repositories carry `pcp 7.1.5` and every PMDA package this chapter needs,
fully unauthenticated — so `registry.fedoraproject.org/fedora:44` is the
base that actually builds for everyone, not just readers with a Red Hat
account.

One PMDA is deliberately absent from this image: `pmda-bcc`, PCP's own
bridge into the eBPF world Chapter 30 covered directly. Activating it needs
a working BCC/eBPF stack underneath — raw `BPF` syscalls, `/sys/kernel/
debug`, a kernel-devel package matching the exact running kernel — which in
turn needs a **super-privileged container**, the opposite of the rootless
posture every other example in this book targets. It's covered here only as
a name: PCP's privileged sibling PMDA, demonstrated properly on the
`systems-target` VM where Chapter 30's real eBPF tooling already runs, not
shipped inside this rootless image.

## How the code works

`Containerfile` is three real stages — `builder`, `smoke`, `runtime` — and
the ordering between the last two is itself the chapter's first lesson.

**Stage 1 — `builder`.** After `dnf install`ing `pcp pcp-system-tools
pcp-pmda-openmetrics pcp-pmda-podman` and pointing the openmetrics PMDA's
config at `config/lsp45.prom`, the build has to activate both PMDAs. This
is the one step that took real iteration to get right, and the fix is
right there in the `RUN` line:

```dockerfile
RUN /usr/libexec/pcp/bin/pmcd -f >/tmp/pmcd-build.log 2>&1 & \
    for i in $(seq 1 20); do \
      pminfo -f pmcd.version >/dev/null 2>&1 && break; sleep 1; \
    done && \
    cd /var/lib/pcp/pmdas/openmetrics && yes '' | ./Install && \
    cd /var/lib/pcp/pmdas/podman      && yes '' | ./Install
```

Each PMDA's `./Install` script (`pmdaproc.sh`) decides, at the end of its
run, whether `pmcd` needs restarting to pick up the change — and its
default way of doing that is `systemctl restart pmcd.service`. A build
layer has no systemd, so that call fails, and `Install`'s failure path
**reverts its own `pmcd.conf` edit** — the PMDA it just tried to register
silently disappears, with no loud error at all. `pmdaproc.sh` has a
cheaper path, though: if `pminfo -v pmcd.version` already succeeds against
a `pmcd` that's already running, it sends a plain `pmsignal -a -s HUP
pmcd` instead of attempting a restart — no systemd required. Starting
`pmcd` in the background **before** either `./Install` call, and polling
`pminfo` until it answers, takes that cheaper path for both PMDAs, and
nothing gets reverted.

**Stage 2 — `smoke`.** This stage exists purely as a build-time gate that
is never shipped: it starts `pmcd` fresh from the config `builder` just
wrote, and asserts real values come back, not just that the daemon starts.

```dockerfile
FROM builder AS smoke
RUN /usr/libexec/pcp/bin/pmcd -f >/tmp/pmcd.log 2>&1 & \
    for i in $(seq 1 20); do \
      pminfo -f pmcd.version >/dev/null 2>&1 && break; sleep 1; \
    done; \
    pcp | tee /tmp/pcp-summary.txt; \
    grep -q openmetrics /tmp/pcp-summary.txt && \
    grep -q podman      /tmp/pcp-summary.txt && \
    pminfo -f openmetrics.lsp45.lsp45_answer | tee /tmp/om.txt && \
    grep -q "value 42" /tmp/om.txt && \
    pminfo -f kernel.all.load | tee /tmp/load.txt && \
    grep -q "kernel.all.load" /tmp/load.txt
```

If either PMDA silently failed to register — exactly the failure mode the
previous paragraph describes — one of these `grep -q` checks fails, the
`RUN` exits non-zero, and the whole `podman build` fails right here. That
is a stronger guarantee than any runtime-only health check: a broken PMDA
never gets the chance to ship.

**Stage 3 — `runtime`, and the second real bug.** A gate that nothing
chains onto is a gate that never runs — and `podman build` (like Docker)
only builds the stages reachable from the final stage's own `FROM` chain.
An earlier version of this Containerfile wrote `FROM builder AS runtime`,
which compiles cleanly and looks correct, but it leaves `smoke` completely
disconnected: no `COPY --from=smoke` and no `FROM smoke` anywhere means
`podman build` silently skips the `smoke` stage's `RUN` altogether, and the
"build-time gate" above never executes at all. The fix is one word:

```dockerfile
# Shipped runtime. Reuses the smoke stage's already-activated PMNS/pmcd.conf
# rather than re-running Install a third time -- FROM smoke (not FROM
# builder) also means podman actually builds the smoke stage by default:
# with no COPY --from/FROM edge into it, an unreferenced intermediate stage
# is silently skipped by `podman build`, which would make the "build-time
# gate" a no-op. Chaining runtime FROM smoke is what makes the gate real.
FROM smoke AS runtime
EXPOSE 44321
ENTRYPOINT ["/usr/libexec/pcp/bin/pmcd", "-f"]
```

`FROM smoke AS runtime` both reuses `smoke`'s already-activated PMDA state
(no third `./Install` re-run needed) and forces `podman build` to actually
build `smoke` first — the gate stage only runs because something real
depends on it existing.

{% include excalidraw.html
   file="45-pcp-build-gate"
   alt="A vertical three-stage pipeline. Stage one, builder, installs pcp packages, copies lsp45.prom, starts pmcd, and runs two ./Install calls against that live pmcd. An arrow labeled FROM builder AS smoke descends into stage two, smoke, which restarts pmcd and asserts both PMDAs are listed plus a real value 42 and a real kernel.all.load sample, gated by grep -q calls that can fail the whole podman build. A ghost, disconnected box beside stage two reads FROM builder AS runtime -- the skipped path, dashed and crossed out, with a note that podman silently never builds a stage nothing chains onto. The real arrow, solid and amber, is labeled FROM smoke AS runtime and descends into stage three, runtime, which reuses the smoke stage's already-activated PMNS/pmcd.conf and carries only EXPOSE 44321 and the pmcd entrypoint. Caption: chaining onto smoke, not builder, is what forces the gate stage to actually build."
   caption="Figure 45.2 — the PCP image's build gate: chaining runtime onto smoke forces the gate stage to build, and a PMDA that fails to register fails the build, not just the demo" %}

## Errors, three ways

With one example and no per-language split, "three ways" means three
different moments and volumes an error can surface at, rather than three
languages producing the same one.

**Loud, at build time.** The `smoke` stage's `grep -q` chain is a real gate:
during this example's own development, deliberately starting `pmcd` *after*
the `./Install` calls (undoing the fix above) reproduced the exact failure
this chapter warns about — `podman build` failed outright, with the
`pmcd.conf` revert visible in the build log, rather than shipping a broken
image that merely looked fine.

**Silent, inside a passing build.** Before the fix, the failure mode isn't
loud at all: `./Install`'s revert of its own `pmcd.conf` edit produces no
error text a casual read of the build log would catch — the PMDA simply
never appears in `pcp`'s summary, and nothing about `podman build`'s exit
code says so. This is the errors-three-ways lesson worth sitting with: a
build can succeed and still have silently dropped a component, which is
exactly why the `smoke` stage's assertions exist as a second, independent
check rather than trusting `podman build`'s own exit status.

**A graceful, reported degradation at runtime.** Not every gap here is a
bug. If no rootless `podman.sock` is present when the container starts,
`demo.sh` reports it plainly (`note: no rootless podman.sock at $sock --
podman PMDA will register but report zero container values`) and continues
— the podman PMDA's namespace still registers correctly, it simply has no
live container data to report. That is a documented, expected runtime
condition, not a failure, and treating it as one would make an
environment-dependent optional feature look like a broken build.

## Concurrency lens

Each suite here answers "how many things are running at once" a different
way. `pmcd` is one reactor process fronting **N independent PMDA
processes**, each talking to `pmcd` over its own pipe — the same
process-isolation discipline Chapter 11 built `pmon` around, applied here
to metrics collection instead of service supervision: a PMDA that hangs or
crashes takes down only its own slice of the namespace, never `pmcd`
itself or any sibling agent. SystemTap's `<<<` operator is the sharpest
concurrency idea in this chapter: it's a per-CPU-safe **statistical
aggregation** — every CPU accumulates into its own private bucket with no
shared-memory contention at all, and only the `end` probe's `@count()`
call folds those per-CPU buckets together into the single number a script
prints. That's the same lock-free instinct Chapter 26 built atomics and
CAS loops around, reached here through a completely different mechanism:
avoid the race by never sharing the counter in the first place, rather
than serializing access to one. Cockpit's concurrency story is the
simplest of the three: one `cockpit-ws` process multiplexes every browser
session over a single WebSocket-per-client model, with no per-session
process fork at all.

## Build, run, observe

```console
[host]$ cd examples/45-linux-analysis-suites && ./demo.sh cpp run
```

`run()` mounts the host's own rootless `podman.sock` into the container, if
one is present, before starting it:

```bash
run() {
  podman rm -f "$CONTAINER" >/dev/null 2>&1 || true

  # If a rootless podman.sock is up for this user, mount it so the layered
  # podman PMDA sees REAL running containers (including this one) instead
  # of just registering with zero values. This is a genuine, verified
  # result: pmda-podman's own Install script only checks for
  # /run/podman/podman.sock to decide whether to prompt about starting the
  # podman service -- it works identically well whether the socket behind
  # that path belongs to root or, as here, an unprivileged user.
  local sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/podman/podman.sock"
  local mount_args=()
  if [ -S "$sock" ]; then
    mount_args=(-v "$sock:/run/podman/podman.sock")
  else
    echo "note: no rootless podman.sock at $sock -- podman PMDA will" \
         "register but report zero container values" >&2
  fi

  podman run -d --name "$CONTAINER" --cpus=2 --memory=256m \
    "${mount_args[@]}" "$IMAGE"
```

This session's real transcript, on the Fedora 44 host (kernel
`7.1.4-204.fc44`, podman 5.8.4 rootless):

```console
--- pcp: suite summary (openmetrics/podman should both be listed) ---
Performance Co-Pilot configuration on 9fb541ff035b:
 platform: Linux 9fb541ff035b 7.1.4-204.fc44.x86_64 ...
 pmcd: Version 7.1.5-3, 11 agents
 pmda: root pmcd proc pmproxy xfs podman linux mmv kvm jbd2 openmetrics
--- pminfo -f: a real scraped openmetrics value (file:// source, no network) ---
openmetrics.lsp45.lsp45_answer
    value 42
--- pminfo -f: real podman container visibility (via the mounted socket) ---
podman.container.name
    inst [0 or "9fb541ff...ea3a3b5"] value "lsp45-pcp-demo"
podman.container.running
    inst [0 or "9fb541ff...ea3a3b5"] value 1
--- pmrep: one live sample of a core, always-present metric ---
  k.a.load  k.a.load  k.a.load
  1 minute  5 minute  15 minut
     0.970     0.540     0.420
```

`pmcd: Version 7.1.5-3, 11 agents` — the two PMDAs this chapter layers
(`openmetrics`, `podman`) plus the nine that ship in any PCP install
(`root pmcd proc pmproxy xfs linux mmv kvm jbd2`), all eleven active. The
openmetrics PMDA hands back the exact `42` `config/lsp45.prom` ships, every
run. The podman PMDA, once the container's own `podman.sock` is mounted
into it, sees `lsp45-pcp-demo` — the very container running the query —
reporting itself as `podman.container.running value 1`. `pmrep`'s three
load-average columns come from `kernel.all.load`, a core PCP metric with
nothing to do with either PMDA this chapter added — proof that `pmcd`'s
base collection works independent of anything layered on top.

Cockpit lives on the `systems-target` VM, not this container, and the
check here is a socket check rather than a browser session:

```console
[vm]$ systemctl is-active cockpit.socket
active
[vm]$ curl -sk https://192.168.124.7:9090/
```

That `curl` returns a real HTTP 200 body — the socket answers, and
`cockpit-364-1.fc44` plus `cockpit-podman` are both installed by
cloud-init.

> **Cockpit's web-UI panels** <span class="status
> status--unverified">unverified</span> — the Overview, Podman, and Logs
> panels a browser would render from that same socket are described here,
> not exercised: this repo has no browser-automation tooling, and the
> Cockpit console is meant to be looked at, not scraped. What's verified is
> narrower and mechanical — the socket is up and answers `HTTP 200` — and
> that's exactly the boundary this chapter's status footer draws.

SystemTap runs on the same VM, as root, against its kernel-module backend:

```console
[vm]$ sudo stap -e 'probe oneshot { println("stap-module-ok") }'
stap-module-ok
```

That one line is a real kernel module compiled against
`kernel-devel-$(uname -r)` (matched to the running `6.19.10-300.fc44`
kernel), loaded, run once, and unloaded — the payoff Chapter 30 flagged as
blocked by a kernel-devel mismatch, resolved here once the matching
package is installed. A slightly larger script aggregates real syscall
activity from a controlled workload:

```console
[vm]$ sudo stap -c '<workload>' -e 'global opens; probe syscall.openat { opens[execname()] <<< 1 } probe end { foreach (e in opens-) printf("%s: %d openat\n", e, @count(opens[e])) }'
cat: 155 openat
sh: 43 openat
```

Five `cat /etc/hostname` invocations under a driving `sh` produce 155
`openat` calls attributed to `cat` (the binary itself, its shared
libraries, and the target file, each invocation) and 43 to the shell
driving it — `<<<` accumulating per-CPU, `@count()` folding those buckets
together only once, at the `end` probe. A practical caveat worth stating
rather than hiding: this VM has no `kernel-debuginfo` installed, so a
`WARNING: cannot find module kernel debuginfo` line appears alongside the
real output above; `syscall.*` probes (kprobes and tracepoints, both used
here) don't need it — only DWARF-based variable inspection would.

The gate the runner checks:

```console
[host]$ LSP_LANG=cpp lua verify.lua
...
PASS 10 / FAIL 0
```

Image size on this host: **265 MB** (`podman images lsp45-pcp`) — much
larger than Chapter 34's few-tens-of-MB runtime images, because PCP's own
dependency chain (Python, `pcp-libs`, `pcp-system-tools`) is the size
driver, and there's no further multi-stage trim available when `runtime`
needs the exact same userspace `smoke` just proved works.

## Cross-check: two independent gates agree on the same real value

This chapter's central claim — the `openmetrics` PMDA genuinely parses
`config/lsp45.prom` rather than the value `42` being hardcoded somewhere in
the image — is checkable two independent ways, and both agree. The first
is the `smoke` stage's own build-time assertion, running entirely inside
`podman build`, with no external harness watching: `grep -q "value 42"
/tmp/om.txt` against a fresh `pminfo -f openmetrics.lsp45.lsp45_answer`
call. The second is `verify.lua`, run from outside the image entirely,
against a container the harness itself started:

```lua
-- ---------------------------------------------------------------------------
-- 4. a REAL scraped openmetrics value, from the file:// source this example
--    ships (config/lsp45.prom) -- not a live network scrape, but a real
--    parse of a real file, every time.
-- ---------------------------------------------------------------------------

local om = checks.run("podman exec " .. c .. " pminfo -f openmetrics.lsp45.lsp45_answer")
checks.expect_match(om.out, "openmetrics%.lsp45%.lsp45_answer",
  "pminfo -f resolves the scraped openmetrics metric name")
checks.expect_match(om.out, "value 42",
  "the scraped value is the real constant this chapter's config ships (42)")
```

Two code paths — one baked into the `Containerfile` itself, one driven by
`verify.lua` against a container it started fresh — independently query
the same PMDA and land on the identical value, exactly once each. That's a
stronger claim than either check alone: the build-time gate proves the
PMDA works at image-build time, before the image is ever shipped;
`verify.lua`'s check proves it still works after the image is built,
shipped, and started cold. Neither check trusts the other's result.

## What you learned

- **Three suites, three philosophies for the same question.** Cockpit
  answers "what is this system doing" with a browser and zero scripting;
  SystemTap answers it by compiling a real kernel module per probe — bcc's
  harder-to-build sibling from Chapter 30; PCP answers it by giving a
  uniform namespace to metrics pulled out of things this book already
  built — a `podman.sock` (Chapter 34) and an OpenMetrics document
  (Chapter 38).
- **Fedora, not UBI, because the packages simply aren't there.** `pcp` and
  its PMDAs are unresolvable on UBI 9/10 without a RHEL entitlement this
  book's readers are assumed not to have;
  `registry.fedoraproject.org/fedora:44` carries `pcp 7.1.5` unauthenticated.
- **A disconnected build stage is a build stage that never runs.**
  `FROM builder AS runtime` compiles cleanly and silently skips `smoke`
  entirely; `FROM smoke AS runtime` is what forces `podman build` to
  actually execute the gate.
- **A PMDA's own `Install` script can silently revert itself.** With no
  systemd in a build layer, `systemctl restart pmcd.service` fails and
  `Install`'s failure path un-does its own `pmcd.conf` edit — starting
  `pmcd` before `./Install` takes a cheaper, systemd-free `pmsignal -a -s
  HUP pmcd` path instead, and nothing gets reverted.
- **Two independent gates on the same value beat one.** The `smoke`
  stage's build-time `grep -q "value 42"` and `verify.lua`'s runtime
  `pminfo` call check the same PMDA through two unrelated code paths and
  agree — proof the value is genuinely parsed, not hardcoded, at both
  build time and run time.
- **`pmda-bcc` is excluded on purpose, not by oversight.** Real eBPF
  attachment needs a super-privileged container — the opposite of every
  rootless posture in this book — so it's named here as PCP's privileged
  sibling PMDA and left to Chapter 30's real eBPF toolkit on
  `systems-target`.

Part 13's appendices continue from here — tooling this book leaned on
without ever giving it a chapter of its own.

---

<p><span class="status status--verified">verified</span> — every transcript
above except the span marked <span class="status
status--unverified">unverified</span> (Cockpit's Overview/Podman/Logs
web-UI panels, described but not browser-automated) was produced this
session. On the Fedora 44 host (kernel <code>7.1.4-204.fc44</code>, podman
5.8.4 rootless): <code>./demo.sh cpp run</code> produced the exact
transcript quoted above, including <code>pmcd: Version 7.1.5-3, 11
agents</code>, <code>openmetrics.lsp45.lsp45_answer value 42</code>, and
live podman-socket container visibility (<code>podman.container.name
... "lsp45-pcp-demo"</code>, <code>podman.container.running ... value
1</code>); <code>LSP_LANG=cpp lua verify.lua</code> reported <code>PASS 10 /
FAIL 0</code>; and the built image measured 265 MB
(<code>podman images lsp45-pcp</code>). On the <code>systems-target</code>
lab VM (kernel <code>6.19.10-300.fc44.x86_64</code>): the SystemTap
kernel-module backend produced <code>stap-module-ok</code> from a real
compiled-loaded-unloaded kernel module, and the openat aggregate against a
controlled five-<code>cat</code> workload produced <code>cat: 155
openat</code> and <code>sh: 43 openat</code> exactly as quoted (this
required installing <code>kernel-devel-$(uname -r)</code> to match the
running kernel — now folded into this project's cloud-init); and
<code>curl -sk https://192.168.124.7:9090/</code> against
<code>cockpit.socket</code> (active, <code>cockpit-364-1.fc44</code> +
<code>cockpit-podman</code> installed by cloud-init) returned a real
<code>HTTP 200</code>. Not exercised: the Cockpit web console's own
rendered panels, as marked above — this repo has no browser-automation
tooling to drive one.</p>
