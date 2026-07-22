---
title: "OpenTelemetry to LGTM: instrument a service, then prove the spans and metrics actually arrived"
order: 38
part: "Observability"
description: "lsp-otel is a traced request/response server that exports OpenTelemetry traces and metrics over OTLP/HTTP to the LGTM stack from Chapter 3 — a parent request span wrapping work and respond children, a requests_total counter and a request_duration histogram — and the verify step proves it by querying Tempo and Prometheus through Grafana rather than trusting the program's own word, across C++, Go, and Rust each tagged with its own service.name."
duration: "50 minutes"
---

Chapter 37's `loadmix analyze` printed a USE-method verdict to a terminal, and
Chapter 36's `sysagent` served one JSON snapshot per HTTP request. Both make you
*present* to read the number: someone has to be running the command, or scraping
the endpoint, at the moment the system is in trouble. This chapter closes that
gap. `lsp-otel` is a small traced server that emits the three OpenTelemetry
signal shapes — a distributed **trace** per request, a monotonic **counter**, and
a latency **histogram** — and exports them over OTLP/HTTP into the LGTM stack
(Loki, Grafana, Tempo, Mimir plus an OpenTelemetry collector) that Chapter 3
stood up, so the next time a request is slow you can go *look* at when it
happened instead of needing to have been watching. And because "I sent
telemetry" is the easiest claim in observability to get quietly wrong, the whole
point of the verification here is that it queries the stack back — Tempo for the
traces, Prometheus for the metrics — and refuses to believe the export happened
until the data is actually there to read.

The code is in `examples/38-opentelemetry-to-lgtm/`. It is a **local-mode
example that requires the LGTM stack**: `serve` and `drive` run on the host, and
the exporter posts to the collector on `localhost:4318`.

{% include excalidraw.html
   file="38-otlp-pipeline"
   alt="A left-to-right pipeline. Left, a band labeled 'serve — one traced request per input line' containing: a parent span box 'request (attr request.seq)' with two child span boxes below it, 'work' (sleep 1+seq%4 ms) and 'respond' (write ok seq), plus two metric boxes, 'requests_total (+1 per request)' and 'request_duration (ms)'. An arrow labeled OTLP leaves the band into an 'OTLP/HTTP exporter' box annotated 'batch ~2s, flush on SIGINT', which POSTs, over an amber arrow labeled POST /v1/{traces,metrics}, into an 'OTel collector :4318' box. The collector fans out along two arrows — 'spans' up to a 'Tempo — traces' box and 'metrics' across to a 'Prometheus — metrics' box — and both of those feed a 'Grafana query :3000' box on the right. A caption along the bottom reads: cpp · go · rust — each tags service.name = lsp-otel-<lang>, so the three never collide in the shared stack."
   caption="Figure 38.1 — lsp-otel's telemetry path: a per-request span tree plus two metrics, batched through the OTLP/HTTP exporter to the collector on :4318, fanned to Tempo and Prometheus, and read back through Grafana — which is exactly how the export is verified" %}

> **Tools used** — `podman` (host, runs the `lsp-lgtm` observability stack —
> OTLP on `:4318`, Grafana on `:3000`) and `curl` (host, the only tool the
> verification needs: it queries Prometheus and Tempo through Grafana's
> datasource proxy). The stack is the same `lsp-lgtm` container the
> `lgtm-podman-stack` setup from Chapter 3 provides; nothing else is required on
> the host.

## Three signals, one request

Every OpenTelemetry story is told in three signal types, and `lsp-otel` emits
all three from a single unit of work — one line of input to `serve`, treated as
one "request":

- A **trace** describes the shape and timing of that request as a tree of
  spans. `lsp-otel` starts a parent span named `request`, and *inside* it two
  child spans: `work` (a deliberately tiny, variable `1 + seq%4` ms sleep
  standing in for real computation) and `respond` (writing the `ok <seq>` reply).
  The parent/child nesting is what lets Tempo later show you that the request's
  time was spent in `work`, not `respond`, or vice versa.
- A **counter** — `requests_total`, a monotonic `Int64` that goes up by one per
  request — answers "how many?" It only ever increases, which is what makes it
  a counter rather than a gauge, and what lets Prometheus compute a rate from it.
- A **histogram** — `request_duration`, in milliseconds — answers "how long?"
  across the whole population, recording one observation (the parent span's
  wall-clock duration) per request, so the backend can later report a p50, a
  p99, or a bucketed distribution without the server having to keep every sample.

The second subcommand, `drive --n N --addr HOST:PORT`, is a plain TCP client
that sends `N` requests and reads `N` replies. It is deliberately **untraced**:
it generates the load that makes `serve`'s telemetry interesting, but emits none
of its own, because the service under observation is `serve`, not the thing
poking it.

## The request pipeline, three ways

The heart of the instrumentation is one function — `handleRequest` — that starts
the parent span, nests the two children inside it, writes the reply, and records
both metrics. It reads almost identically across the three languages because the
OpenTelemetry API is deliberately the same shape in each; the differences are
idiomatic, not structural:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
bool handle_request(const telemetry::Handle& t, int fd, std::int64_t seq) {
    const auto start = std::chrono::steady_clock::now();

    auto parent_span = t.tracer->StartSpan("request", {{"request.seq", seq}});
    trace_api::Scope parent_scope(parent_span);

    // work: a tiny, deterministic-but-variable amount of simulated cost.
    auto work_span = t.tracer->StartSpan("work", {{"request.seq", seq}});
    std::this_thread::sleep_for(std::chrono::milliseconds(1 + seq % 4));
    work_span->End();

    // respond: write the reply line.
    auto respond_span = t.tracer->StartSpan("respond", {{"request.seq", seq}});
    const std::string reply = "ok " + std::to_string(seq) + "\n";
    const bool wrote = write_all(fd, reply);
    if (!wrote) {
        respond_span->SetStatus(trace_api::StatusCode::kError, "write reply failed");
    }
    respond_span->End();
    parent_span->End();

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    t.requests_total->Add(1);
    // Histogram::Record(T) alone is an ABI-v2-only overload; this build is
    // pinned to ABI v1 (matching the rest of the conan cache), so an
    // explicit empty Context is required to select an ABI-v1 overload.
    t.request_duration->Record(elapsed_ms, opentelemetry::context::Context{});

    return wrote;
}
```

```go
func handleRequest(ctx context.Context, t *telemetry, w *bufio.Writer, seq int) error {
	start := time.Now()
	ctx, parent := t.tracer.Start(ctx, "request",
		trace.WithAttributes(attribute.Int("request.seq", seq)))
	defer parent.End()

	// work: a tiny, deterministic-but-variable amount of simulated cost.
	_, work := t.tracer.Start(ctx, "work",
		trace.WithAttributes(attribute.Int("request.seq", seq)))
	time.Sleep(time.Duration(1+seq%4) * time.Millisecond)
	work.End()

	// respond: write the reply line.
	_, respond := t.tracer.Start(ctx, "respond",
		trace.WithAttributes(attribute.Int("request.seq", seq)))
	if _, err := fmt.Fprintf(w, "ok %d\n", seq); err != nil {
		respond.RecordError(err)
		respond.End()
		return fmt.Errorf("write reply: %w", err)
	}
	if err := w.Flush(); err != nil {
		respond.RecordError(err)
		respond.End()
		return fmt.Errorf("flush reply: %w", err)
	}
	respond.End()

	elapsedMs := float64(time.Since(start)) / float64(time.Millisecond)
	t.requestsTotal.Add(ctx, 1)
	t.requestDur.Record(ctx, elapsedMs)
	return nil
}
```

```rust
fn handle_request(t: &Telemetry, w: &mut TcpStream, seq: i64) -> std::io::Result<()> {
    let start = Instant::now();
    let parent_span = t
        .tracer
        .span_builder("request")
        .with_attributes(vec![KeyValue::new("request.seq", seq)])
        .start_with_context(&t.tracer, &Context::new());
    let parent_cx = Context::current_with_span(parent_span);

    // work: a tiny, deterministic-but-variable amount of simulated cost.
    let mut work_span = t
        .tracer
        .span_builder("work")
        .with_attributes(vec![KeyValue::new("request.seq", seq)])
        .start_with_context(&t.tracer, &parent_cx);
    thread::sleep(Duration::from_millis((1 + seq % 4) as u64));
    work_span.end();

    // respond: write the reply line.
    let mut respond_span = t
        .tracer
        .span_builder("respond")
        .with_attributes(vec![KeyValue::new("request.seq", seq)])
        .start_with_context(&t.tracer, &parent_cx);
    let result = (|| -> std::io::Result<()> {
        writeln!(w, "ok {seq}")?;
        w.flush()
    })();
    if let Err(ref e) = result {
        respond_span.record_error(e);
    }
    respond_span.end();
    parent_cx.span().end();
    result?;

    let elapsed_ms = start.elapsed().as_secs_f64() * 1000.0;
    t.requests_total.add(1, &[]);
    t.request_duration.record(elapsed_ms, &[]);
    Ok(())
}
```

The parent/child relationship is carried differently in each, and it is the one
part worth reading closely. Go threads a `context.Context`: `tracer.Start`
returns a *new* context carrying the parent span, and passing that context into
the two child `Start` calls is what makes them children. Rust makes the same
context explicit — `Context::current_with_span(parent_span)` builds a context
holding the parent, and each child is started `start_with_context(..., &parent_cx)`.
C++ uses a `Scope` object: constructing `trace_api::Scope parent_scope(parent_span)`
installs the parent as the current span for the lifetime of that RAII object, so
the two children, started while the scope is alive, attach to it implicitly.
Three mechanisms — an explicit context value, an explicit context value again,
and a scope guard — for the identical span tree, which Tempo later shows as
`request` with `work` and `respond` beneath it.

## How the code works

Each language's `initTelemetry`/`init_telemetry`/`telemetry::init` wires up two
exporters against a shared **resource** — the set of attributes that identify
*this* service in the stack. The critical one is `service.name`, and it is
deliberately different per language: `lsp-otel-cpp`, `lsp-otel-go`,
`lsp-otel-rust`. Because all three export into the *same* shared LGTM stack, a
distinct `service.name` is what keeps their signals from colliding and lets the
verification query each language's telemetry by its own name.

Both exporters speak **OTLP over HTTP/protobuf** to the collector, and both are
**batched on a short (~2 second) interval** — a batch span processor for traces,
a periodic reader for metrics. That interval is a deliberate choice: the SDK
defaults (5-second span batches, 60-second metric batches) are fine for a
long-running service but far too slow for a demo that starts, drives ten
requests, and stops within a couple of seconds. A short interval plus a **flush
on shutdown** is what guarantees the last batch — often *all* the batches, at
this scale — actually leaves the process before it exits. That shutdown path is
the subject of the Concurrency lens below, because it turned out to be where the
three languages diverge most.

## Errors, three ways

`lsp-otel` has two exit codes for failure. A **usage error** — no subcommand, an
unknown subcommand, `serve` without `--port`, or `drive` with `--n 0` or missing
— prints the two-line usage banner to stderr and exits **2**:

```
usage: app serve --port PORT [--otel-endpoint URL]
       app drive --n N --addr HOST:PORT
```

A **runtime error** is exit **1** with an `app: error: …` message: the one
`verify.lua` exercises is `drive` against an address nothing is listening on,
which fails to connect and prints `app: error: dial …: connection refused`. This
is the one message that is *not* byte-identical across the three languages —
each surfaces its own dial/`errno` wording, the same way Chapter 40's `connect`
errors carry each platform's `strerror` text — so `verify.lua` asserts only the
shared `app: error:` prefix and the exit code, not the full string. A clean run
of either subcommand exits **0**: `serve` after a signal-driven shutdown, `drive`
after `N/N` successful replies.

## Concurrency lens

`serve` has to wait on two fundamentally different things at once: a new
connection arriving, and a shutdown signal arriving. All three ports solve this
the same way in shape — a signal source and an accept loop both feed a *single*
channel that the main loop consumes one event at a time — and all three write it
with the idiom their language reaches for:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    for (;;) {
        Event e = channel.pop();
        if (e.kind == Event::Kind::Signal) {
            std::println("app: shutting down ({})", e.signal_name);
            std::fflush(stdout);
            const bool ok = t.flush_and_shutdown(std::chrono::milliseconds(5000));
            if (!ok) {
                std::println(stderr, "app: telemetry shutdown: one or more providers failed to flush/shutdown");
            }
            return 0;
        }
        serve_conn(t, e.conn_fd);
    }
```

```go
	for {
		select {
		case sig := <-sigCh:
			fmt.Printf("app: shutting down (%s)\n", sig)
			ln.Close()
			shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			if err := shutdown(shutCtx); err != nil {
				fmt.Fprintln(os.Stderr, "app: telemetry shutdown:", err)
			}
			cancel()
			return 0
		case conn := <-connCh:
			serveConn(t, conn)
		}
	}
```

```rust
    loop {
        match rx.recv() {
            Ok(Event::Signal(name)) => {
                println!("app: shutting down ({name})");
                t.shutdown();
                return 0;
            }
            Ok(Event::Conn(stream)) => {
                serve_conn(&t, stream);
            }
            Err(_) => return 0,
        }
    }
}
```

Go's `select` over `sigCh` and `connCh` is the native version — the language has
a built-in construct for exactly "wait on whichever of these channels is ready
first." Rust rebuilds the same thing from an `mpsc::channel` carrying an `Event`
enum (`Signal` or `Conn`), fed by a `signal-hook` thread and an accept thread.
C++ does likewise with a hand-written `EventChannel` fed by a `sigwait()` thread
and an accept thread. Same architecture, three spellings.

Getting there surfaced three portability lessons worth keeping, because each is
the kind of bug that leaves the program looking fine while silently doing
nothing:

1. **The OTLP/HTTP exporter must POST to the full per-signal URLs** —
   `http://localhost:4318/v1/traces` and `…/v1/metrics`, not the bare base
   `http://localhost:4318`. Both the Rust and C++ ports first tried the base URL;
   the collector answered `404`, the export silently failed, and the *only*
   outward sign was a shutdown-time flush error. The Go SDK hides this by
   appending the path for you; the others make you spell it out.
2. **Each SDK logs internally by default, and that breaks the output contract.**
   `serve`'s output must be exactly two lines. opentelemetry-cpp writes
   Info/Debug to stdout and Warning/Error to stderr unless you install a
   `NoopLogHandler` at `LogLevel::None`; the Rust build had to *not* register a
   `tracing-subscriber` at all. Silence is part of the port.
3. **C++ only: a background job inherits `SIGINT` set to `SIG_IGN`.** A
   non-interactive shell running `serve &` — exactly how `verify.lua` starts it —
   sets `SIGINT`/`SIGQUIT` to *ignore* in the child before `exec`, and that
   disposition is inherited. `pthread_sigmask` + `sigwait()` alone cannot consume
   a signal whose disposition is "ignore"; the port has to install a real (no-op)
   `sigaction` first to move the disposition off `SIG_IGN` — precisely what Go's
   `signal.Notify` and Rust's `signal-hook` do under the hood, which is why only
   the C++ port had to do it by hand. (Two smaller C++-only wrinkles rounded it
   out: `Histogram::Record` needs the `(value, Context{})` overload under this
   build's ABI v1, and the OTLP static archives have a genuine link cycle that
   needs `-Wl,--start-group`.)

## Build, run, observe

```bash
[host]$ cd examples/38-opentelemetry-to-lgtm && ./demo.sh build
```

Run the server, drive ten requests through it, and stop it — the SIGINT flush is
what pushes the final batch out:

```console
[host]$ ./go/bin/app serve --port 8080 &
app: serve listening on 127.0.0.1:8080
[host]$ ./go/bin/app drive --n 10 --addr 127.0.0.1:8080
app: drive sent 10/10 ok
[host]$ kill -INT %1
app: shutting down (interrupt)
```

Now read the telemetry back out of the stack — not out of the program. Metrics
land in Prometheus; querying `requests_total` through Grafana's datasource proxy
shows the counter for whichever language just ran:

```console
[host]$ curl -s -u admin:admin \
    'http://localhost:3000/api/datasources/proxy/uid/prometheus/api/v1/query?query=requests_total{service_name="lsp-otel-go"}'
... "value":[ ..., "10"] ...
```

The runner drives all three languages against the live stack and asserts the
full contract — including the stack queries — for each:

```console
[host]$ python3 scripts/test-all-examples.py --only 38-opentelemetry-to-lgtm --mode local
verifying...
  verify 38-opentelemetry-to-lgtm [cpp]: PASS
  verify 38-opentelemetry-to-lgtm [go]: PASS
  verify 38-opentelemetry-to-lgtm [rust]: PASS

example                   cpp   go    rust
38-opentelemetry-to-lgtm  PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

## Cross-check: two signals, the same ten requests

The verification's real strength is that it confirms the export from **two
independent backends**, tagged by each language's own `service.name`. After a
`drive --n 10`, both the counter and the histogram's observation count read
exactly **10** in Prometheus, for all three languages:

| `service_name` | `requests_total` | `request_duration_milliseconds_count` |
|---|---|---|
| `lsp-otel-cpp` | 10 | 10 |
| `lsp-otel-go` | 10 | 10 |
| `lsp-otel-rust` | 10 | 10 |

And the *traces* for the same runs are in Tempo: a search by
`service.name=lsp-otel-<lang>` returns traces whose `rootServiceName` is that
name and whose `rootTraceName` is `request`, and fetching one back shows its
spans are exactly `work`, `respond`, and the `request` parent — the tree
`handle_request` built. Metrics saying "ten requests happened" and traces saying
"here is the shape of each of those requests" are two different subsystems
agreeing about the same work, which is a far stronger signal that the
instrumentation is correct than either backend alone — and neither number came
from the program telling us about itself.

## What you learned

- **A service emits observability as three signal types** — traces (a tree of
  parent/child spans describing one request's shape and timing), a monotonic
  counter (`requests_total`, "how many"), and a histogram (`request_duration`,
  "how long", one observation per request) — and `lsp-otel` produces all three
  from a single `handle_request`.
- **Parent/child span nesting is carried by context** — Go and Rust thread an
  explicit context value into each child `Start`, C++ installs the parent with a
  `Scope` guard — and it is what turns three separate spans into the
  `request → {work, respond}` tree Tempo shows.
- **Export is batched, and a flush on shutdown is what makes the last data
  arrive** — a short (~2s) interval plus a signal-driven flush, not the slow SDK
  defaults, is why a start-drive-stop demo's telemetry shows up at all.
- **Prove the export landed by querying the backend, never the program.** The
  verify step reads `requests_total` and `request_duration_milliseconds_count`
  out of Prometheus and the trace tree out of Tempo, through Grafana's proxy — an
  export that 404'd silently (the wrong-endpoint bug both non-Go ports hit) fails
  this check even though the program looked healthy.
- **The same OTLP contract needs per-language care**: the full `/v1/traces` and
  `/v1/metrics` URLs, silencing each SDK's default logging to keep the output
  contract, and — in C++ — installing a real signal handler to override the
  `SIG_IGN` a background shell hands a child before `sigwait` can ever see the
  signal.

Chapter 39 turns from *collecting* numbers to *trusting* them: how to benchmark
your own code without the warmup, coordinated-omission, and dead-code traps that
make most microbenchmarks quietly measure the wrong thing.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host (kernel 7.1.3-200.fc44) this session, with the <code>lsp-lgtm</code>
LGTM stack running (OTLP on <code>:4318</code>, Grafana on <code>:3000</code>):
<code>python3 scripts/test-all-examples.py --only 38-opentelemetry-to-lgtm
--mode local</code> printed <code>PASS PASS PASS</code> (3 passed, 0 failed;
<code>verify.lua</code> reported <code>PASS 15 / FAIL 0</code> for each of
cpp/go/rust). After a <code>serve</code> + <code>drive --n 10</code> + SIGINT for
each language, querying through Grafana's datasource proxy returned
<code>requests_total{service_name="lsp-otel-&lt;lang&gt;"}</code> = 10 and
<code>request_duration_milliseconds_count{service_name="lsp-otel-&lt;lang&gt;"}</code>
= 10 for all three, and Tempo returned traces with
<code>rootServiceName=lsp-otel-&lt;lang&gt;</code>,
<code>rootTraceName=request</code>, and spans <code>work</code>/<code>respond</code>/<code>request</code>.
<code>serve</code>'s output was exactly its <code>listening</code> and
<code>shutting down (SIGINT)</code> lines with no SDK log noise;
<code>drive</code> printed <code>drive sent 10/10 ok</code> and exited 0; a drive
to a dead address exited 1 with an <code>app: error:</code> prefix. Not
exercised: any run with the LGTM stack down (<code>verify.lua</code> SKIPs that
case rather than failing) and any lab-VM run — this is a host-only, local-mode
example.</p>
