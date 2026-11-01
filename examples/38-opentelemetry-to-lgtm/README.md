# 38-opentelemetry-to-lgtm: lsp-otel, a traced server whose export gets checked, not trusted

`lsp-otel` is a traced request/response server. Every request it handles
produces a parent span wrapping two child spans, a counter increment, and a
histogram observation, all exported over OTLP/HTTP to the local LGTM stack
(Loki, Grafana, Tempo, Mimir plus an OpenTelemetry collector). The point of
the example is the verification, not the server: it queries Tempo and
Prometheus through Grafana's datasource proxy to confirm the export actually
landed, instead of trusting the program's own clean exit.

```console
[host]$ ./go/bin/app serve --port 8080 &
app: serve listening on 127.0.0.1:8080
[host]$ ./go/bin/app drive --n 10 --addr 127.0.0.1:8080
app: drive sent 10/10 ok
[host]$ kill -INT %1
app: shutting down (interrupt)
```

`serve --port PORT [--otel-endpoint URL]` accepts connections and traces each
one; `--otel-endpoint` defaults to `http://localhost:4318`, the collector the
`lsp-lgtm` stack publishes. `drive --n N --addr HOST:PORT` is a plain TCP
client that sends `N` requests and reads `N` replies; it stays untraced by
design, since the service under observation is `serve`, not the thing poking
it. This is a **local-mode example that requires the LGTM stack**: both
subcommands run on the host, and the exporter posts to the collector on
`localhost:4318`.

## Three signals, one request

Each request `serve` handles starts a parent span named `request`, carrying a
`request.seq` attribute, with two children nested inside it:

- **`work`**: a tiny `1 + seq%4` millisecond sleep standing in for actual
  computation.
- **`respond`**: writing the `ok <seq>` reply line.

Alongside that span tree, every request records two metrics: `requests_total`,
a monotonic counter that goes up by one per request, and `request_duration`,
a histogram (in milliseconds) that records one observation per request, the
parent span's wall-clock duration. The counter answers "how many," the
histogram answers "how long," and Tempo later shows the parent/child nesting
itself: whether a request's time went into `work` or `respond`.

## Carrying the parent span, three ways

One function, `handleRequest`/`handle_request`, starts the parent span, nests
the two children, writes the reply, and records both metrics. The API shape
is the same across all three languages; how the parent/child relationship is
carried is not:

- **Go** threads an explicit `context.Context`. `t.tracer.Start(ctx, "request", ...)`
  returns a *new* context carrying the parent span, and passing that context
  into each child `Start` call is what makes it a child.
- **Rust** makes the same context explicit: `Context::current_with_span(parent_span)`
  builds a context holding the parent, and each child is started with
  `start_with_context(&t.tracer, &parent_cx)`.
- **C++** uses a `Scope` guard. Constructing `trace_api::Scope parent_scope(parent_span)`
  installs the parent as the current span for the lifetime of that RAII
  object, so children started while the scope is alive attach to it
  implicitly.

Three mechanisms, the same `request -> {work, respond}` tree.

## Exporting over OTLP/HTTP

Each language's telemetry init wires up two exporters (traces and metrics)
against a shared resource, the set of attributes identifying this service in
the stack. The one that matters here is `service.name`, set differently per
language by design: `lsp-otel-cpp`, `lsp-otel-go`, `lsp-otel-rust`. All three
export into the same shared LGTM stack, so the distinct name is what keeps
their signals from colliding and lets verification query each language's
telemetry on its own.

Both exporters speak OTLP over HTTP/protobuf to the collector and batch on a
short (~2 second) interval, a batch span processor for traces and a periodic
reader for metrics, instead of the SDK defaults (5-second span batches,
60-second metric batches), which are too slow for a demo that starts, drives
ten requests, and stops within a couple of seconds. A flush on shutdown, tied
to the SIGINT handler, is what guarantees the last batch leaves the process
before it exits.

## Layout

```
38-opentelemetry-to-lgtm/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check: drives serve+drive, queries Tempo+Prometheus
├── cpp/         # CMake preset build, demo.sh
├── go/          # go build, demo.sh
└── rust/        # cargo build, demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the interface used across the
book: `./demo.sh` (build then run), `./demo.sh build` (build only), and
`./demo.sh run [args]` (run the built binary; with `TARGET` set, deploy to a
lab VM instead). The top-level `demo.sh` here is only a dispatcher.

`serve`'s exit codes: a usage error (no subcommand, `serve` without `--port`,
`drive` with `--n 0` or missing) prints the two-line usage banner and exits
`2`; a runtime error (`drive` against a dead address) exits `1` with an
`app: error: …` prefix; a clean run exits `0`, `serve` after SIGINT, `drive`
after `N/N` successful replies.

## Verification

`verify.lua` runs from this directory with `LSP_LANG` set to one language at
a time. It first checks Grafana's `/api/health` endpoint; if the LGTM stack
isn't reachable, the whole example SKIPs rather than failing. Against a live
stack, it builds, checks the usage/error exit codes and messages, then runs
`serve --port 47380` in the background, drives ten requests through it,
sends SIGINT, and waits for `serve` to exit `0` after its flush.

With the data presumably exported, it queries the backend instead of trusting
`serve`'s output: `requests_total{service_name="lsp-otel-<lang>"}` and
`request_duration_milliseconds_count{service_name="lsp-otel-<lang>"}` through
Prometheus, and a trace search with `service.name=lsp-otel-<lang>` through
Tempo, both via Grafana's datasource proxy (`/api/datasources/proxy/uid/prometheus/...`
and `.../uid/tempo/...`) on `localhost:3000` with `admin:admin`. Each query is
polled with a bounded retry to absorb the exporter's batch interval and the
backends' own ingest lag. The example passes only once both counters read at
least 10 and at least one matching trace shows up in Tempo.

## Mapping to a chapter

This is the example for the OpenTelemetry-to-LGTM chapter in the
Observability part: instrumenting a request/response server with traces and
metrics, exporting them over OTLP/HTTP, and verifying the export by reading
the backend back instead of trusting the program's own word.
