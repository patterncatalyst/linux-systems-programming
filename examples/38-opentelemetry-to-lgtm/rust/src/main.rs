// lsp-otel-rust — a traced request/response loop (chapter 38: OpenTelemetry
// to LGTM).
//
//	app serve --port PORT [--otel-endpoint URL]
//	app drive --n N --addr HOST:PORT
//
// serve accepts one persistent line-oriented connection and treats every
// line as a "request": a parent span ("request") wraps two children
// ("work", "respond"), a monotonic counter (requests_total) is incremented,
// and a histogram (request_duration, milliseconds) records the parent
// span's wall time. drive is an untraced TCP client that sends N requests
// down one connection and waits for N replies — the load generator for
// serve's telemetry, not a telemetry source itself.
//
// Every signal appears exactly once in the OTLP stream: serve's own
// SIGINT/SIGTERM handling flushes and shuts down the SDK before exit, the
// Rust way of RAII (explicit shutdown calls, since there is no deferred
// cleanup guaranteed to run before `std::process::exit`).
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result};
use opentelemetry::metrics::{Counter, Histogram, MeterProvider as _};
use opentelemetry::trace::{Span as _, TraceContextExt, Tracer as _, TracerProvider as _};
use opentelemetry::{Context, KeyValue};
use opentelemetry_otlp::WithExportConfig;
use opentelemetry_sdk::Resource;
use opentelemetry_sdk::metrics::{PeriodicReader, SdkMeterProvider};
use opentelemetry_sdk::trace::{
    BatchConfigBuilder, BatchSpanProcessor, Sampler, SdkTracer, SdkTracerProvider,
};
use signal_hook::consts::{SIGINT, SIGTERM};
use signal_hook::iterator::Signals;

const SERVICE_NAME: &str = "lsp-otel-rust";

// EXPORT_INTERVAL is short on purpose: the demo and verify.lua both need the
// last batch flushed within a few seconds of shutdown, not the SDK
// defaults (5s span batches are fine; 60s metric batches are not).
const EXPORT_INTERVAL: Duration = Duration::from_secs(2);

fn usage() -> ! {
    eprintln!("usage: app serve --port PORT [--otel-endpoint URL]");
    eprintln!("       app drive --n N --addr HOST:PORT");
    std::process::exit(2);
}

fn die(msg: &str) -> ! {
    eprintln!("app: error: {msg}");
    std::process::exit(1);
}

// ---------------------------------------------------------------- telemetry

struct Telemetry {
    tracer_provider: SdkTracerProvider,
    meter_provider: SdkMeterProvider,
    tracer: SdkTracer,
    requests_total: Counter<u64>,
    request_duration: Histogram<f64>,
}

impl Telemetry {
    // shutdown flushes and closes both providers. Errors are reported but
    // never fatal — the process is exiting regardless, this is best effort
    // draining of whatever the batch/periodic exporters still hold.
    fn shutdown(&self) {
        if let Err(e) = self.tracer_provider.shutdown() {
            eprintln!("app: telemetry shutdown: {e}");
        }
        if let Err(e) = self.meter_provider.shutdown() {
            eprintln!("app: telemetry shutdown: {e}");
        }
    }
}

// init_telemetry wires up the OTLP HTTP/protobuf exporters (reqwest blocking
// client — no tokio runtime needed), a short-interval batch span processor
// and periodic metric reader, and the two instruments serve records against.
// The per-signal OTLP HTTP exporters POST to exactly the URL given -- unlike
// the base-endpoint env-var path, an explicit with_endpoint() does NOT append
// the signal path in opentelemetry-otlp 0.32 -- so the caller's base
// (http://localhost:4318) is joined to /v1/traces and /v1/metrics here.
// Posting to the bare base instead 404s at the collector and the export
// silently fails (surfacing only as a shutdown error).
fn init_telemetry(endpoint: &str) -> Result<Telemetry> {
    let base = endpoint.trim_end_matches('/');
    let traces_url = format!("{base}/v1/traces");
    let metrics_url = format!("{base}/v1/metrics");

    let resource = Resource::builder_empty()
        .with_service_name(SERVICE_NAME)
        .with_attribute(KeyValue::new("service.version", "1.0"))
        .with_attribute(KeyValue::new("deployment.environment", "local"))
        .build();

    let span_exporter = opentelemetry_otlp::SpanExporter::builder()
        .with_http()
        .with_endpoint(&traces_url)
        .build()
        .context("trace exporter")?;

    let batch_processor = BatchSpanProcessor::builder(span_exporter)
        .with_batch_config(
            BatchConfigBuilder::default()
                .with_scheduled_delay(EXPORT_INTERVAL)
                .build(),
        )
        .build();

    let tracer_provider = SdkTracerProvider::builder()
        .with_span_processor(batch_processor)
        .with_resource(resource.clone())
        .with_sampler(Sampler::AlwaysOn)
        .build();

    let metric_exporter = opentelemetry_otlp::MetricExporter::builder()
        .with_http()
        .with_endpoint(&metrics_url)
        .build()
        .context("metric exporter")?;

    let reader = PeriodicReader::builder(metric_exporter)
        .with_interval(EXPORT_INTERVAL)
        .build();

    let meter_provider = SdkMeterProvider::builder()
        .with_resource(resource)
        .with_reader(reader)
        .build();

    let tracer = tracer_provider.tracer(SERVICE_NAME);
    let meter = meter_provider.meter(SERVICE_NAME);

    let requests_total = meter
        .u64_counter("requests_total")
        .with_description("Total requests handled")
        .with_unit("1")
        .build();
    let request_duration = meter
        .f64_histogram("request_duration")
        .with_description("Request duration")
        .with_unit("ms")
        .build();

    Ok(Telemetry {
        tracer_provider,
        meter_provider,
        tracer,
        requests_total,
        request_duration,
    })
}

// ------------------------------------------------------------------- serve

// handle_request is the parse->work->respond pipeline for one line of input.
// "parse" happens inline (cheap: one i64 parse, done by the caller); "work"
// and "respond" are the two child spans under the "request" parent, per the
// trace contract every language in this example follows identically.
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

fn serve_conn(t: &Telemetry, mut stream: TcpStream) {
    let reader_side = match stream.try_clone() {
        Ok(s) => s,
        Err(_) => return,
    };
    let mut reader = BufReader::new(reader_side);
    let mut line = String::new();
    loop {
        line.clear();
        let n = match reader.read_line(&mut line) {
            Ok(n) => n,
            Err(_) => return,
        };
        if n == 0 {
            return; // EOF: client closed the connection.
        }
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        match trimmed.parse::<i64>() {
            Ok(seq) => {
                if let Err(e) = handle_request(t, &mut stream, seq) {
                    eprintln!("app: request error: {e}");
                    return;
                }
            }
            Err(_) => {
                if stream.write_all(b"err bad-request\n").is_err() {
                    return;
                }
            }
        }
    }
}

// Event unifies the two sources serve waits on — an incoming connection and
// a shutdown signal — into the single channel a Go select over sigCh/connCh
// would use.
enum Event {
    Signal(&'static str),
    Conn(TcpStream),
}

fn run_serve(args: &[String]) -> i32 {
    let mut port = String::new();
    let mut endpoint =
        std::env::var("OTEL_ENDPOINT").unwrap_or_else(|_| "http://localhost:4318".to_string());
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                port = args[i].clone();
            }
            "--otel-endpoint" => {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                endpoint = args[i].clone();
            }
            _ => usage(),
        }
        i += 1;
    }
    if port.is_empty() {
        usage();
    }

    let t = match init_telemetry(&endpoint) {
        Ok(t) => t,
        Err(e) => die(&format!("telemetry init: {e}")),
    };

    let listener = match TcpListener::bind(format!("127.0.0.1:{port}")) {
        Ok(l) => l,
        Err(e) => die(&format!("listen: {e}")),
    };

    let (tx, rx) = mpsc::channel::<Event>();

    // Signal watcher: a dedicated thread blocks on signal-hook's iterator
    // and forwards the first SIGINT/SIGTERM onto the shared event channel.
    {
        let tx = tx.clone();
        thread::spawn(move || {
            let mut signals = match Signals::new([SIGINT, SIGTERM]) {
                Ok(s) => s,
                Err(_) => return,
            };
            if let Some(sig) = signals.forever().next() {
                let name = match sig {
                    SIGINT => "SIGINT",
                    SIGTERM => "SIGTERM",
                    _ => "signal",
                };
                let _ = tx.send(Event::Signal(name));
            }
        });
    }

    // Accept loop: a dedicated thread forwards each accepted connection onto
    // the same event channel. The process exits (via std::process::exit)
    // from the shutdown path below rather than closing the listener, so this
    // thread never needs to be joined — same lifecycle as Go's accept
    // goroutine, which os.Exit simply cuts short.
    {
        let tx = tx.clone();
        thread::spawn(move || {
            for stream in listener.incoming() {
                match stream {
                    Ok(s) => {
                        if tx.send(Event::Conn(s)).is_err() {
                            return;
                        }
                    }
                    Err(_) => return, // listener closed during shutdown
                }
            }
        });
    }

    println!("app: serve listening on 127.0.0.1:{port}");

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

// ------------------------------------------------------------------- drive

// run_drive is a plain, untraced TCP client: it generates the load that
// makes serve's telemetry interesting, but emits none of its own — the
// service under observation is serve, not drive.
fn run_drive(args: &[String]) -> i32 {
    let mut n: i64 = 0;
    let mut addr = "127.0.0.1:8080".to_string();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--n" => {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                match args[i].parse::<i64>() {
                    Ok(v) => n = v,
                    Err(e) => die(&format!("--n: {e}")),
                }
            }
            "--addr" => {
                i += 1;
                if i >= args.len() {
                    usage();
                }
                addr = args[i].clone();
            }
            _ => usage(),
        }
        i += 1;
    }
    if n <= 0 {
        usage();
    }

    let stream = match TcpStream::connect(&addr) {
        Ok(s) => s,
        Err(e) => die(&format!("dial {addr}: {e}")),
    };
    let mut writer = match stream.try_clone() {
        Ok(s) => s,
        Err(e) => die(&format!("dial {addr}: {e}")),
    };
    let mut reader = BufReader::new(stream);

    let mut ok: i64 = 0;
    let mut line = String::new();
    for seq in 1..=n {
        if let Err(e) = writeln!(writer, "{seq}") {
            die(&format!("send request {seq}: {e}"));
        }
        if let Err(e) = writer.flush() {
            die(&format!("flush request {seq}: {e}"));
        }
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => break, // EOF
            Ok(_) => {
                if line.trim_end().starts_with("ok ") {
                    ok += 1;
                }
            }
            Err(e) => die(&format!("read reply {seq}: {e}")),
        }
    }
    println!("app: drive sent {ok}/{n} ok");
    if ok != n { 1 } else { 0 }
}

// ---------------------------------------------------------------------main

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
    }
    let code = match args[1].as_str() {
        "serve" => run_serve(&args[2..]),
        "drive" => run_drive(&args[2..]),
        _ => usage(),
    };
    std::process::exit(code);
}
