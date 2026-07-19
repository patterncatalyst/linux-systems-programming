---
title: "The local observability stack"
order: 3
part: "Setting Up"
description: "Stand up the Grafana LGTM all-in-one container with podman compose: OTLP ingest on :4318, Grafana on :3000, a curl-able trace smoke test through Tempo, and the gateway rule that lets lab VMs reach the stack."
duration: "30 minutes"
---

Chapter 1 gave you three toolchains; Chapter 2 gave you two disposable VMs.
This chapter adds the last piece of the workbench: somewhere for telemetry to
*go*. Many chapters in this book emit metrics, traces, or logs — a syscall
counter here, a request span there — and printing them to a terminal only gets
you so far. What you want is a real backend you can query, graph, and
cross-check against, without turning your workstation into a monitoring
deployment. The answer is one container: the Grafana **LGTM** all-in-one
image, run rootless under Podman, listening on exactly two ports you will
memorize — **4318** for sending and **3000** for looking.

The infrastructure is in `examples/_infra/` — one compose file and a README.
There is no per-chapter example here; every telemetry-emitting example from
Chapter 38 onward depends on what you stand up now.

{% include excalidraw.html
   file="03-telemetry-pipeline"
   alt="Telemetry pipeline: a host demo and a VM demo send OTLP HTTP to port 4318 on the lsp-lgtm container, where the OpenTelemetry Collector fans out to Loki for logs, Tempo for traces, and Mimir for metrics; Grafana on port 3000 queries all three and serves the developer's browser. The VM demo reaches the host at the libvirt gateway address 192.168.124.1."
   caption="Figure 3.1 — one container, whole pipeline: OTLP in on :4318, Grafana out on :3000" %}

## What LGTM is, and why one container

LGTM is Grafana Labs' acronym for its open-source backend quartet: **L**oki
(logs), **G**rafana (the UI), **T**empo (traces), **M**imir (metrics,
Prometheus-compatible). In production these are four separately scaled,
separately configured services. For local development that topology is pure
overhead — what you need is the *interfaces*, not the scale. So Grafana ships
`grafana/otel-lgtm`, a single image containing all four **plus an embedded
OpenTelemetry Collector** wired to receive OTLP and fan each signal out to the
right store, with Grafana's datasources pre-provisioned to read them back.
One `podman compose up` and the entire pipeline in Figure 3.1 exists.

The collector is the part worth pausing on. Your programs will never speak
"Loki" or "Mimir" directly — they speak **OTLP** (the OpenTelemetry protocol)
to the collector, and the collector routes logs, traces, and metrics to their
stores. That indirection is the whole point of OpenTelemetry: instrument once,
point `OTEL_EXPORTER_OTLP_ENDPOINT` somewhere, and the backend becomes
swappable. The endpoint you point at is the same whether the sender is a C++
binary on the host or a Go binary inside `systems-target`.

OTLP comes in two transports, and the image exposes both: gRPC on **4317**
and HTTP/protobuf-or-JSON on **4318**. This book standardizes on **HTTP
:4318**, for reasons that matter in a lab: you can drive it with `curl` (which
makes today's smoke test possible), it needs no gRPC client libraries or
HTTP/2 plumbing in minimal demos, and it traverses the NAT hop from the lab
VMs with nothing to debug. gRPC's streaming efficiency is a production
concern; debuggability wins on a workbench.

## The compose file, line by line

Read `examples/_infra/compose-lgtm.yaml` — it is short enough to understand
completely:

```yaml
services:
  lgtm:
    image: docker.io/grafana/otel-lgtm:0.8.1
    container_name: lsp-lgtm
    ports:
      - "3000:3000"      # Grafana UI
      - "4317:4317"      # OTLP gRPC
      - "4318:4318"      # OTLP HTTP
    mem_limit: 2g
    environment:
      # Anonymous admin access keeps local demos friction-free. Never ship this.
      - GF_AUTH_ANONYMOUS_ENABLED=true
      - GF_AUTH_ANONYMOUS_ORG_ROLE=Admin
      - GF_AUTH_DISABLE_LOGIN_FORM=true
    healthcheck:
      # curl, not wget — the otel-lgtm image ships curl only
      test: ["CMD", "curl", "-sf", "http://localhost:3000/api/health"]
      interval: 5s
      timeout: 3s
      retries: 20
      start_period: 30s   # the all-in-one image takes a while on first boot

networks:
  default:
    name: lsp-observability
```

Each choice has a why. The image is **pinned to `0.8.1`** — an unpinned
`latest` would let the stack silently change under a book whose chapters
promise reproducible output. `container_name: lsp-lgtm` gives scripts (and
you) a stable handle instead of a generated name. Only three ports are
published; Loki, Tempo, and Mimir listen on internal ports *inside* the
container, deliberately unpublished — everything goes in through the collector
and comes out through Grafana, which is exactly the discipline production
imposes. `mem_limit: 2g` exists because this one container runs five services;
the cap keeps a runaway ingest from competing with your builds for RAM. The
three `GF_AUTH_*` variables make Grafana skip its login entirely and treat
every visitor as an admin — indefensible anywhere reachable, perfect for a
loopback-only lab, and the comment in the file says so. The `healthcheck`
polls Grafana's `/api/health` from inside the container with a generous
`start_period`, because first boot unpacks and starts all five services and
takes ~30 seconds; the health state is what scripts wait on instead of
sleeping. Finally the network gets the fixed name `lsp-observability` so any
future container (the PCP suite in Chapter 45, for instance) can join it by
name rather than by guessing a generated one.

## The prerequisite: the Podman user socket

One thing must exist before `podman compose` works, and it is the step
everyone forgets:

```bash
[host]$ systemctl --user enable --now podman.socket
```

Why: `podman compose` is not a compose implementation. It is a thin
dispatcher that delegates to an external provider — on Fedora, the
`docker-compose` plugin — which speaks the Docker Engine API to a daemon
socket. Podman famously has no daemon, but it emulates one: the systemd user
unit `podman.socket` listens on
`$XDG_RUNTIME_DIR/podman/podman.sock` and socket-activates a Podman service
that answers Docker API calls, rootless, as you. Without the unit enabled
there is no socket, the provider finds no endpoint, and compose fails with a
connection error that says nothing about systemd. Enable it once
(`--now` also starts it immediately); it persists across reboots.

## How examples depend on the stack

Telemetry-emitting examples declare the dependency in `examples/manifest.yaml`:

```yaml
requires: [lgtm]
```

The verification harness (`scripts/test-all-examples.py`) probes
`http://localhost:4318` before running such an example — and counts *any*
HTTP response as alive, including a 4xx, because a bare GET to an OTLP ingest
path legitimately returns an error status while proving something is
listening. If nothing answers, the example is marked **SKIP (lgtm down)**
rather than FAIL; per-example `verify.lua` checks use `expect_port_open` the
same way. The principle: an absent optional backend should never turn a
working example red.

## Reaching the stack from inside a VM

A guest cannot use `localhost:4318` — its loopback is its own. But on a
libvirt NAT network, the **network gateway is the host**, so the host's
published ports are reachable at the gateway IP. On this lab that is
`192.168.124.1` (libvirt's stock default network is `192.168.122.0/24`, so
most references say `.122.1` — always check yours):

```bash
[vm]$ ip route show default
default via 192.168.124.1 dev eth0 proto dhcp metric 100
```

You pass the endpoint through `deploy-to-vm.sh` via `OTEL_ENDPOINT`:

```bash
[host]$ OTEL_ENDPOINT=http://192.168.124.1:4318 TARGET=systems-target ./demo.sh run
```

The script forwards it explicitly because of a subtlety worth knowing: `sudo`
strips the environment, so `scripts/lab/deploy-to-vm.sh` re-injects it with
`env` on the remote side —

```bash
RENV=""
[[ -n "${OTEL_ENDPOINT:-}" ]] && RENV="OTEL_EXPORTER_OTLP_ENDPOINT='$OTEL_ENDPOINT'"
...
exec ssh -t $SSH_OPTS "fedora@$IP" "sudo env $RENV '$REMOTE' $*"
```

— otherwise the guest binary would fall back to *its* localhost and telemetry
would vanish silently. One host-side requirement: ports 4317/4318 must be open
toward the libvirt bridge (on Fedora, the `libvirt` firewalld zone).

## Build, run, observe

Bring the stack up and wait for health:

```bash
[host]$ systemctl --user enable --now podman.socket
[host]$ podman compose -f examples/_infra/compose-lgtm.yaml up -d
```

{% raw %}
```bash
[host]$ podman inspect --format '{{.State.Health.Status}}' lsp-lgtm
healthy
[host]$ curl -s http://localhost:3000/api/health
{"database": "ok", ...}
```
{% endraw %}

Open `http://localhost:3000` — no login form, straight to Grafana as an
anonymous admin. Take the two-minute tour: **Explore** in the left navigation
is where you will live; its datasource picker already lists **Loki**,
**Tempo**, and **Mimir** (as a Prometheus datasource), provisioned by the
image. All three are empty. Let's fix that with the same smoke test the r02
gate used: hand-post one trace with `curl`, then query it back.

```bash
[host]$ TRACE_ID=$(openssl rand -hex 16); SPAN_ID=$(openssl rand -hex 8); NOW=$(date +%s%N)
[host]$ curl -s -o /dev/null -w '%{http_code}\n' -X POST http://localhost:4318/v1/traces \
    -H 'Content-Type: application/json' \
    -d '{"resourceSpans":[{"resource":{"attributes":[{"key":"service.name",
         "value":{"stringValue":"lsp-smoke"}}]},"scopeSpans":[{"spans":[{
         "traceId":"'$TRACE_ID'","spanId":"'$SPAN_ID'","name":"r02-gate-smoke","kind":1,
         "startTimeUnixNano":"'$NOW'","endTimeUnixNano":"'$((NOW+50000000))'"}]}]}]}'
200
```

That `200` is the collector accepting a minimal but complete OTLP/JSON trace:
one resource (carrying `service.name`, the identity every backend groups by),
one scope, one 50 ms span. Now query it back **through Grafana's datasource
proxy** — Tempo's own HTTP port is intentionally unpublished, so the proxy
route both reaches Tempo *and* proves the Grafana→Tempo wiring in a single
request:

```bash
[host]$ curl -s http://localhost:3000/api/datasources/proxy/uid/tempo/api/traces/$TRACE_ID | python3 -m json.tool
```

```json
{
    "batches": [
        {
            "resource": {
                "attributes": [
                    { "key": "service.name",
                      "value": { "stringValue": "lsp-smoke" } }
                ]
            },
            "scopeSpans": [
                { "spans": [ { "name": "r02-gate-smoke", ... } ] }
        ...
}
```

(Response trimmed.) The span went curl → collector → Tempo → Grafana proxy →
back to your terminal — every arrow in Figure 3.1's trace path, exercised
once. In the UI, Explore → Tempo → TraceQL query
`{ resource.service.name = "lsp-smoke" }` finds the same span with a waterfall
view of one bar.

Stop the stack the same way you started it (state is not preserved — fine for
a lab):

```bash
[host]$ podman compose -f examples/_infra/compose-lgtm.yaml down
```

## Cross-check

Confirm what compose claims with Podman itself, and Grafana with its own
health endpoint:

{% raw %}
```bash
[host]$ podman ps --filter name=lsp-lgtm --format '{{.Names}}  {{.Image}}  {{.Status}}'
lsp-lgtm  docker.io/grafana/otel-lgtm:0.8.1  Up ... (healthy)
[host]$ curl -s http://localhost:3000/api/health
{"database": "ok", ...}
```
{% endraw %}

`podman ps` is independent of the compose provider — it asks the container
runtime directly, so agreement here means the docker-compose plugin, the user
socket, and the runtime all see the same world. The `(healthy)` suffix is the
compose file's `curl` healthcheck reporting from *inside* the container (the
image ships curl, not wget — a detail that cost this chapter one debugging
round), while your `curl` hits the *published* port from outside: same
endpoint, two network paths, both answering.

## What you learned

- The LGTM all-in-one image packs Loki, Grafana, Tempo, Mimir, and an OTel
  Collector into one pinned container — production interfaces, workbench
  footprint; senders only ever speak OTLP, and this book standardizes on the
  curl-able HTTP transport at `:4318`.
- `podman compose` delegates to the docker-compose plugin over the Docker
  API socket that `systemctl --user enable --now podman.socket` provides —
  the one-time prerequisite behind every `up -d`.
- Examples declare `requires: [lgtm]`; the harness probes `:4318` and skips,
  never fails, when the stack is down.
- Guests reach host ports at the libvirt gateway (`192.168.124.1` here, stock
  `.122.1`), and `deploy-to-vm.sh` forwards `OTEL_ENDPOINT` through `sudo
  env` because sudo strips the environment.

The stack sits idle until Chapter 38 instruments real programs in all three
languages against it, and Chapter 45 adds the PCP, Cockpit, and SystemTap
suites alongside. With the toolchains, the lab, and the telemetry pipeline in
place, the workbench is complete — next, Foundations begins where everything
in this book bottoms out: the syscall boundary and the ABI beneath it.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44 host: <code>podman.socket</code> enabled, <code>podman compose -f examples/_infra/compose-lgtm.yaml up -d</code> brought <code>lsp-lgtm</code> (otel-lgtm:0.8.1) to healthy, <code>/api/health</code> returned ok, the OTLP HTTP POST to <code>:4318/v1/traces</code> returned 200, and the Tempo query by trace ID through Grafana's datasource proxy returned the span <code>r02-gate-smoke</code> (service <code>lsp-smoke</code>). Not yet exercised: exporting telemetry from inside a lab VM via the <code>192.168.124.1</code> gateway — the endpoint-forwarding path is verified only as far as <code>deploy-to-vm.sh</code> running host-built binaries on the guest; a VM-side OTLP export lands with Chapter 38.</p>
