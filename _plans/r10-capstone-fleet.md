---
title: "Plan — r10: the capstone fleet (example 41 + chapter 41)"
layout: plan
render_with_liquid: false
published: false
---

# Plan — r10: the capstone fleet

Complete the book's finale: `41-capstone-fleet`, where **pmon** supervises
**chatterd**, **sysagent**, and **fwatch** as a capability-dropped,
Landlock-sandboxed, OTLP-observed fleet spanning **two lab VMs**. The Go
reference is finished (~1328 lines / 9 files); this iteration writes the real
`verify.lua`, ports **C++** and **Rust**, gates on the `vm-peer` + LGTM
topology, and authors **chapter 41**. It is the largest example in the book by
2-3x, and the one that assembles the four recurring programs into one system.

## The fleet — one binary, self-re-exec, four programs

Usage surface (from `go/main.go`), the full contract to preserve:

```
pmon [--node NAME] [--sandbox-dir DIR] [--peer HOST:PORT] [--peer-node NAME]
     [--chatterd-port P] [--health-interval-ms N]
chatterd serve  [--host H] [--port P] [--node NAME] [--peer HOST:PORT] [--peer-node NAME]
chatterd send   --host H --port P --nick NICK --text TEXT [--timeout-ms T]
chatterd listen --host H --port P --nick NICK [--timeout-ms T]
sysagent [--node NAME] [--interval-ms N] [--once]
sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]
fwatch snapshot DIR
fwatch watch DIR [--sandbox] [--timeout-ms T]
```

- **pmon** (`pmon.go`, 158) — fleet init. `mkdir` the sandbox dir, drop the
  capability bounding set (`caps.go`), then self-re-exec (`os.Executable` +
  fork/exec, the ch34 entrypoint technique) chatterd/sysagent/fwatch as
  children; restart any that exits unexpectedly (`restart service=… attempt=N`);
  forward SIGTERM/SIGINT to all three on shutdown; print
  `pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,…` every
  `--health-interval-ms`. Terminal lines: `pmon: started service=… pid=…`,
  `pmon: health …`, `pmon: shutdown`.
- **chatterd** (`chatterd.go`, 264) — the ch21-27 p2p chat daemon reduced to:
  serve local clients, and **bridge two instances across hosts** (the new,
  capstone-specific piece) — with `--peer` set on both sides, each node dials
  the other and joins as `bridge@<node>`, so a MSG sent on one node is
  DELIVERed to clients on the other. `send`/`listen` are the client
  subcommands. Wire frame is `proto.go`.
- **sysagent** (`sysagent.go`, 235) — the ch36 `/proc` agent reduced to three
  signals (cpu%, mem%, load1), emitted both as a stdout line and as OTel gauges
  over OTLP when `OTEL_EXPORTER_OTLP_ENDPOINT` is set. Carries the `saturate
  cpu|mem` chaos driver the fleet's drill uses to move the numbers.
- **fwatch** (`fwatch.go`, 212) — the ch07→09→33 file watcher reduced to: watch
  a directory, print one line per create/write/delete event, optionally under a
  **Landlock** ruleset restricting reads to that dir. `snapshot` prints a
  one-shot listing. seccomp is not re-derived (proven in ch33); cap-drop +
  Landlock are the capstone's two sandbox layers.
- **proto** (`proto.go`, 86) — the chatterd frame: magic `CH`, version 1, type
  byte, big-endian u16 length, payload; types JOIN/MSG/DELIVER.
- **telemetry** (`telemetry.go`, 120) — OTel wiring shared by chatterd (traces
  for deliveries) and sysagent (metrics for `/proc` samples), OTLP/HTTP, endpoint
  from `OTEL_EXPORTER_OTLP_ENDPOINT` (forwarded by `deploy-to-vm.sh`).
- **caps** (`caps.go`, 38) — `PR_CAPBSET_DROP` bounding-set drop, meaningful
  even unprivileged (shrinks what any descendant could ever regain).

## Topology (`vm-peer`, requires `lgtm`)

Two lab guests, `systems-target` and `systems-peer`, each run `pmon` → the
fleet; `chatterd --peer` bridges the two so chat crosses hosts. LGTM runs on the
host; both guests export OTLP to it via the gateway IP. `verify.lua` drives both
guests over ssh and cross-checks telemetry in the stack — the most involved
verify in the book.

## What the ports can reuse

The capstone assembles pieces the book **already ported to C++ and Rust**, so
each port is more assembly than green-field:

| Subsystem | Reference port to lean on |
|---|---|
| `/proc` reading (sysagent) | ex 36 `sysagent` (cpp/go/rust, done) |
| OTLP traces + metrics (telemetry) | ex 38 `lsp-otel` (cpp/go/rust, done — incl. the `/v1/{traces,metrics}` and log-silencing lessons) |
| sockets, signals, framing (chatterd) | ex 40 `chatterd-fastpath` + ex 21-23 |
| Landlock (fwatch) | ex 33 `seccomp-and-landlock` |
| self-re-exec, cap drop (pmon) | ex 34 containers entrypoint; ex 11-14 pmon |
| inotify (fwatch) | ex 09 event loops |

## Dependencies and offline status

- **C++** — `cpp/conanfile.txt` already pins `opentelemetry-cpp/1.24.0` +
  `with_otlp_http` + `libcurl` (no-ssl), the same cached set ex 38 built
  offline. libcap/prctl and Landlock are kernel headers/syscalls (no package).
  **Ready.**
- **Rust** — `rust/Cargo.toml` is **still the template stub** (`template-hello`,
  only `rustix`); the port must set the deps. **All confirmed cached offline
  (checked S3-prep):** `landlock 0.4` (ex 33 uses it; `landlock-0.4.5.crate`
  cached), inotify via `nix` with the `inotify` feature (ex 09's approach; `nix
  0.30/0.31` cached — no separate inotify crate), `opentelemetry-otlp 0.32`
  (ex 38; cached), plus `libc`/`anyhow`. prctl via `libc`/`nix`. Landlock's ABI
  probe goes through the raw `syscall(2)` as ex 33's Rust already does. **No
  offline-availability risk remains for the Rust port.**
- **Go** — full opentelemetry-go stack + `google/uuid`, already in `go.sum`.

## Approach

Reference-passing gate first (the r09-batch method): write `verify.lua` against
the **Go** fleet, prove `LSP_LANG=go` PASS on the `vm-peer` topology, *then* port
C++ and Rust one at a time against that unchanged gate. Chapter last. Within each
port, build **by subsystem** — proto + caps + telemetry (shared infra) first,
then fwatch, sysagent, chatterd, and pmon — getting each to build before wiring
the dispatcher, rather than one 1300-line drop.

## Steps

- **S1 — Extract the exact contract.** Read the Go reference end to end; record
  every subcommand's stdout/stderr lines, exit codes, the wire frame bytes, the
  health-line format, and the telemetry shape (trace/span names, metric names)
  in this plan. No code yet.
- **S2 — `verify.lua` for the fleet (vm-peer + lgtm).** Stage the binary to
  **both** guests; start `pmon` on each (cross-`--peer`); poll `pmon: health`
  over ssh until all three services are `up`; exercise `chatterd send` on one
  node and `chatterd listen` on the other (cross-host delivery); drive
  `sysagent saturate` and confirm cpu_pct rises and the gauge reaches
  Prometheus; verify `fwatch` reports events and that Landlock actually blocks a
  read outside the sandbox dir; confirm the cap-bounding-set drop; SIGTERM pmon
  and confirm clean fleet shutdown. Prove `LSP_LANG=go` PASS. This is the
  hardest gate in the book — budget accordingly.
- **S3 — C++ port.** Subsystem by subsystem, reusing ex 33/34/36/38/40's C++.
  opentelemetry-cpp for telemetry; `prctl(PR_CAPBSET_DROP)` for caps; Landlock
  syscalls for fwatch; fork/exec + `sigwait` supervisor for pmon. Gate
  `LSP_LANG=cpp`.
- **S4 — Rust port.** Resolve deps offline first (see Dependencies). Reuse ex
  33/34/36/38/40's Rust. Gate `LSP_LANG=rust`.
- **S5 — Whole-fleet gate, all three.** `test-all-examples.py --only
  41-capstone-fleet --mode vm` (or vm-peer) with both guests + LGTM up; record
  real fleet numbers.
- **S6 — Chapter 41.** The capstone chapter: how the four programs compose; the
  two-VM bridge; cap-drop + Landlock as the sandbox layers; pmon as fleet init;
  the fleet's telemetry in Grafana. Figure(s) for the topology and the
  supervise/restart lifecycle. Verified footer from S5.

## Risks

- **Scale.** ~1300 lines × 2 ports, multi-file, plus the hardest `verify.lua`.
  Mitigate by subsystem-wise porting and the heavy reuse of existing ports.
- **`vm-peer` verify complexity.** Two guests, cross-host chat, health polling
  over ssh, LGTM queries, Landlock enforcement checks — many moving parts, each
  a flake surface. Build the gate incrementally in S2 and prove each assertion
  against Go before moving on.
- **Rust dep availability offline.** The `landlock`/`inotify` crates may not be
  cached; the raw-syscall fallback via `nix`/`libc` must be ready.
- **Landlock/cap semantics on the guests.** Kernel-version-dependent (ABI); the
  guests are Fedora 44 / kernel 6.19 — confirm the Landlock ABI the port probes
  (as ex 33 does) and that cap-drop is observable unprivileged.
- **Self-re-exec + telemetry init interaction.** Each re-exec'd child inits its
  own OTLP exporter; endpoint inheritance via env must survive the fork/exec, as
  the Go telemetry note relies on.
- **`OTEL_EXPORTER_OTLP_ENDPOINT` forwarding.** verify must set/forward it to the
  guests (gateway IP:4318); a missing endpoint silently drops telemetry (the ex
  38 lesson).

## Progress

| Step | Status |
|---|---|
| S1 contract extraction | done — captured empirically from the Go reference (usage, sysagent/fwatch/chatterd/pmon output shapes, the `from=NICK@node` bridge tag, `landlock ABI=N enforced`, `bounding_set_dropped=N no_new_privs=1`, `sysagent_cpu_pct` gauge) |
| S2 verify.lua (go gate) | done — `PASS 31 / FAIL 0` for `LSP_LANG=go` through the runner. Following ex23's convention the verify runs entirely on LOCAL loopback (fleet, cross-node bridge on two 127.0.0.1 ports, telemetry to the local LGTM) rather than orchestrating two guests over ssh — far more tractable, and the bridge logic is identical on loopback; the real two-host run is the chapter's demo |
| S3 C++ port | pending |
| S4 Rust port | pending |
| S5 whole-fleet gate | pending |
| S6 chapter 41 | pending |
