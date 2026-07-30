# 45 — linux analysis suites (PCP)

A single Fedora-based, multi-stage **Containerfile** that builds `pmcd`
(Performance Co-Pilot's collection daemon) with two PMDAs (Performance
Metrics Domain Agents) layered on top: `pmda-openmetrics` and `pmda-podman`.
Built and run with **rootless podman**.

Unlike every other numbered example in this book, there is no C++/Go/Rust
source here. PCP is an OS-level metrics suite you point at things, not a
program you write in a language — `./demo.sh [cpp|go|rust] [build|run]`
accepts the language argument purely so `scripts/test-all-examples.py`'s
per-language loop can drive it; the image built and run is identical
regardless of which language is passed.

## Why Fedora, not UBI

This project's other container examples (`examples/34-...`) use UBI10. This
one doesn't: checked directly against a real `dnf install`, `pcp` and its
PMDAs are **not resolvable on UBI 9/10** without a RHEL entitlement — no
free/anonymous UBI repo carries them, and this book's readers won't have a
subscription. Fedora's own repos carry `pcp 7.1.5` and every PMDA package
unauthenticated, so `registry.fedoraproject.org/fedora:44` is the base that
actually builds for everyone.

## What's inside, and what's deliberately not

| PMDA | Status | Why |
|---|---|---|
| `pmda-openmetrics` | **In, with a real scraped value** | Bridges an OpenMetrics/Prometheus-style document into PCP's namespace. Pointed at a `file://` source (`config/lsp45.prom`, shipped in this example) rather than an HTTP endpoint, so the scrape is real and reproducible with zero network dependency. |
| `pmda-podman` | **In, registers always; live values need a socket mount** | Reads podman's container/pod state over its API socket. Registers its full metric namespace (21 metrics) at build time with no socket present. At `podman run` time, `demo.sh` mounts the **host's own rootless** `podman.sock` into the container at `/run/podman/podman.sock`, and the PMDA then reports REAL running containers — verified live, including the demo container seeing itself. (There's a known upstream report, [performancecopilot/pcp#913](https://github.com/performancecopilot/pcp/issues/913), that pmda-podman only sees root-managed containers; that was NOT reproduced here — mounting a rootless user's own socket at the expected path worked directly, at least on PCP 7.1.5/podman 5.8.4.) |
| `pmda-bcc` | **Excluded entirely** | Needs a working BCC/eBPF stack, which needs a super-privileged container (raw BPF syscalls, `/sys/kernel/debug`, a matching `kernel-devel`) that cannot load rootless — the opposite of everything else in this example. It's covered in the chapter as PCP's privileged sibling PMDA, demonstrated on the `systems-target` VM, not shipped in this image. This matches the project's "eBPF is tooling only" scope rule. |

## Layout

```
45-linux-analysis-suites/
├── Containerfile        # multi-stage: builder -> smoke (build-time gate) -> runtime
├── config/
│   └── lsp45.prom       # a tiny, real OpenMetrics document (one known metric)
├── demo.sh               # ./demo.sh [cpp|go|rust] [build|run]
├── verify.lua            # podman-gated, behavioral checks
└── README.md
```

## Containerfile stage plan

| Stage | Does |
|---|---|
| `builder` | `dnf install pcp pcp-system-tools pcp-pmda-openmetrics pcp-pmda-podman`; ships `config/lsp45.prom`; points `openmetrics`'s `config.d` at it via a `file://` source; starts `pmcd` **inside the build** and activates both PMDAs against that live `pmcd`. |
| `smoke` | Starts `pmcd` again from the config the builder stage just wrote, confirms **both** PMDAs registered in `pcp`'s summary, and confirms **real metric values** come back — the scraped `openmetrics.lsp45.lsp45_answer` (`42`) and the core `kernel.all.load`. If either PMDA silently failed to attach, this stage — and therefore the whole `podman build` — fails. |
| `runtime` | `FROM smoke` (not `FROM builder`) — chaining onto `smoke` is what makes podman actually build that stage; an unreferenced intermediate stage with no `COPY --from`/`FROM` edge into it is silently skipped by `podman build`, which would turn the "build-time gate" into a no-op. Ships the smoke stage's already-activated PMNS/`pmcd.conf`. `ENTRYPOINT ["/usr/libexec/pcp/bin/pmcd", "-f"]`. |

## The one real build gotcha

Each PMDA's `./Install` script (`pmdaproc.sh`) tries to restart `pmcd` via
`systemctl restart pmcd.service` whenever it thinks a restart — rather than
a lighter SIGHUP — is needed. This build has no systemd, so that call fails,
and Install's failure path **reverts its own `pmcd.conf` edit**, silently
dropping the PMDA it just tried to add. The fix, visible in the
Containerfile: start `pmcd` **before** either `./Install` call. `pmdaproc.sh`
has a cheaper path — if `pminfo -v pmcd.version` already succeeds against a
running `pmcd`, it just sends `pmsignal -a -s HUP pmcd` instead of trying a
full restart, and that path needs no systemd at all. Installing against an
already-running `pmcd` takes that path for both PMDAs and nothing gets
reverted.

## Try it

```bash
./demo.sh cpp build              # podman build only
./demo.sh cpp run                # build + run + a real pminfo/pmrep transcript
```

`run` mounts the host's own rootless `podman.sock` into the container (if
one is active for the invoking user) so the podman PMDA reports live
container state, not just an empty registered namespace. If no socket is
found, it says so and continues — the podman PMDA still registers, it just
reports zero values.

```bash
# by hand, equivalent to demo.sh run
podman build -t lsp45-pcp -f Containerfile .
podman run -d --name lsp45-pcp-demo --cpus=2 --memory=256m \
  -v "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/podman/podman.sock:/run/podman/podman.sock" \
  lsp45-pcp
podman exec lsp45-pcp-demo pcp
podman exec lsp45-pcp-demo pminfo -f openmetrics.lsp45.lsp45_answer
podman exec lsp45-pcp-demo pminfo -f podman.container.name podman.container.running
podman exec lsp45-pcp-demo pmrep -t 1 -s 1 kernel.all.load
podman rm -f lsp45-pcp-demo; podman rmi -f lsp45-pcp
```

## Verification (behavioral)

`verify.lua` requires podman on the host (`skip()`, exit 77, if missing) and
runs only under `LSP_LANG=cpp` (the manifest lists `langs: [cpp]` for this
example, since there's no per-language behavior to triple-run). It actually
builds the image and drives a real container:

- the build succeeds — meaningful because the Containerfile's own smoke
  stage already gates this: a PMDA that fails to register fails the build,
  not just the demo, confirmed by deliberately breaking it once during
  development (starting `pmcd` *after* the `./Install` calls) and watching
  the build fail with the exact `pmcd.conf` revert described above;
- `pmcd` answers `pminfo` within a bounded 20s poll;
- `pcp`'s own summary lists both `openmetrics` and `podman` as active PMDAs;
- `pminfo -f openmetrics.lsp45.lsp45_answer` returns `value 42` — a real
  parse of the real file this example ships, every run;
- the podman PMDA's namespace registers (`podman.container.*` metrics
  exist) — this script does not gate on live container *values*, since that
  needs a mounted socket not guaranteed on every host/CI environment; the
  live-value behavior is real and demonstrated by `demo.sh`, just not
  asserted here;
- `pmrep -t 1 -s 1 kernel.all.load` returns a real sample of a core PCP
  metric, independent of anything this chapter added;
- the container and the built image are removed at the end, whether the
  checks passed or not.

A real run on the reference host (Fedora 44, kernel 7.1.4-204.fc44, podman
5.8.4 rootless) passes all 10 checks:

```
PASS 10 / FAIL 0
```

Image size on that host: **265 MB** (`podman images lsp45-pcp`). This is
much larger than `examples/34-our-programs-in-containers`'s few-tens-of-MB
runtime images — PCP's own dependency chain (Python, `pcp-libs`,
`pcp-system-tools`) is the reason; there's no further multi-stage trim
available, since the shipped `runtime` stage needs the exact same `pmcd`/
PMDA/Python userspace the `smoke` stage just proved works.

## A cosmetic quirk, noted rather than hidden

`pminfo openmetrics` (no `-f`) additionally lists
`openmetrics.grafana.go_memstats_last_gc_since_start_time_seconds` even
though this image's `config.d` only ever contains `lsp45.url`. Fetching it
(`pminfo -f openmetrics.grafana...`) always returns "No value(s) available!"
— it's an inert, unexplained leftover PMNS entry, not a real metric source,
and it does not affect any assertion this example makes (which targets the
specific `openmetrics.lsp45.lsp45_answer` name by exact match). Tracing it
further — it's not in the PMDA's on-disk instance-cache pickle
(`/var/lib/pcp/config/pmda/144.*.py`), which only ever contains `control`
and `lsp45` — went past the point of diminishing returns for a chapter
appendix; flagged here rather than silently ignored, per this project's
verification-discipline rule.
