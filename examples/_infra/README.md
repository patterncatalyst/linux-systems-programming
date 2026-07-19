# Shared example infrastructure

Host-side services that some examples depend on. Today that is one thing: the
Grafana LGTM observability stack (Loki + Grafana + Tempo + Mimir, with a
built-in OpenTelemetry Collector), run as a single container via podman compose.

## LGTM stack

### Start / stop

From the repo root:

```sh
podman compose -f examples/_infra/compose-lgtm.yaml up -d
podman compose -f examples/_infra/compose-lgtm.yaml down
```

The container is named `lsp-lgtm` and has a healthcheck; wait for it to report
healthy before pointing examples at it (first boot can take ~30s):

```sh
podman inspect --format '{{.State.Health.Status}}' lsp-lgtm
```

### Ports

| Port | Purpose |
|------|---------|
| 3000 | Grafana UI (`http://localhost:3000`, anonymous admin) |
| 4317 | OTLP gRPC ingest |
| 4318 | OTLP HTTP ingest |

### How examples declare the dependency

An example that emits telemetry declares the stack in its manifest:

```yaml
requires: [lgtm]
```

The verification harness reads this and checks that the OTLP port is reachable
(via `expect_port_open`) before running the example, skipping (exit 77) when
the stack is not up rather than failing.

### Exporting from inside the lab VM

Examples deployed to the lab VM (`TARGET=...` with `deploy-to-vm.sh`) cannot
reach `localhost` on the host. From a libvirt guest on the default NAT network,
the host is reachable at the network's gateway IP — on this lab that is
`192.168.124.1` (libvirt's stock default is `192.168.122.1`; check yours with
`ip route show default` inside the guest). Pass the endpoint through
`OTEL_ENDPOINT`:

```sh
OTEL_ENDPOINT=http://192.168.124.1:4318 TARGET=systems-target ./demo.sh run
```

Verify the actual gateway from inside the guest with `ip route show default`
if the network is not the libvirt default. Ports 4317/4318 must be open to the
libvirt bridge on the host firewall (on Fedora: the `libvirt` firewalld zone).
