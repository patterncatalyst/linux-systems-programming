// telemetry.rs — OTel wiring shared by chatterd and sysagent. Both signals
// (traces for chatterd deliveries, metrics for sysagent's /proc samples) are
// exported over OTLP/HTTP. Endpoint comes from the standard
// OTEL_EXPORTER_OTLP_ENDPOINT env var — this init deliberately never calls
// `.with_endpoint()`: opentelemetry-otlp 0.32's exporters only append the
// per-signal "/v1/traces" / "/v1/metrics" suffix when they resolve the
// endpoint themselves from OTEL_EXPORTER_OTLP_ENDPOINT (an explicit
// with_endpoint() is taken as the FULL, already-suffixed URL and posted to
// verbatim). Building with default options and letting the exporter read the
// env var is what makes this match go/telemetry.go, whose exporters read the
// same variable the same way.
//
// The SDK is kept silent on purpose: opentelemetry-rust only surfaces
// export failures through its internal `otel_error!` tracing events, which
// require a `tracing` subscriber to go anywhere. ex38 already established
// that installing one (even a filtered one) risks corrupting the stdout/
// stderr contract this capstone's verify.lua depends on line-for-line — so
// none is installed here, and (unlike go/telemetry.go's exportErrors counter,
// fed by otel.SetErrorHandler) this port has no live export-failure signal to
// report; the "otel export_errors=" line below is therefore always 0, a
// known, documented gap rather than a claim of a working counter.
use std::sync::Mutex;
use std::sync::atomic::{AtomicI64, Ordering};

use anyhow::{Context, Result};
use opentelemetry::metrics::{Meter, MeterProvider as _};
use opentelemetry::trace::{Span as _, Status, Tracer as _, TracerProvider as _};
use opentelemetry::{Context as OtelContext, KeyValue};
use opentelemetry_otlp::{MetricExporter, SpanExporter};
use opentelemetry_sdk::Resource;
use opentelemetry_sdk::metrics::{PeriodicReader, SdkMeterProvider};
use opentelemetry_sdk::trace::{
    BatchConfigBuilder, BatchSpanProcessor, Sampler, SdkTracer, SdkTracerProvider,
};

const SERVICE_VERSION: &str = "41-capstone-fleet";
const TRACE_BATCH_DELAY: std::time::Duration = std::time::Duration::from_millis(500);
const METRIC_EXPORT_INTERVAL: std::time::Duration = std::time::Duration::from_secs(2);

/// Count of OTel export failures. Always 0 in this port (see the module doc
/// comment): opentelemetry-rust 0.32 has no equivalent of Go's
/// `otel.SetErrorHandler` reachable without a `tracing` subscriber.
pub static EXPORT_ERRORS: AtomicI64 = AtomicI64::new(0);

/// Owns the trace/metric providers (kept alive so shutdown() can flush them)
/// plus the tracer/meter callers record against. When telemetry is disabled
/// every Option is None; callers gate on `enabled` before touching them.
pub struct Telemetry {
    pub enabled: bool,
    tracer_provider: Option<SdkTracerProvider>,
    meter_provider: Option<SdkMeterProvider>,
    tracer: Option<SdkTracer>,
    meter: Option<Meter>,
}

impl Telemetry {
    fn disabled() -> Self {
        Telemetry {
            enabled: false,
            tracer_provider: None,
            meter_provider: None,
            tracer: None,
            meter: None,
        }
    }

    pub fn tracer(&self) -> Option<&SdkTracer> {
        self.tracer.as_ref()
    }

    pub fn meter(&self) -> Option<&Meter> {
        self.meter.as_ref()
    }

    /// Flushes and shuts down both providers, best effort. A no-op when
    /// telemetry is disabled.
    pub fn shutdown(&self) {
        if !self.enabled {
            return;
        }
        if let Some(tp) = &self.tracer_provider
            && let Err(e) = tp.shutdown()
        {
            eprintln!("app: telemetry shutdown: {e}");
        }
        if let Some(mp) = &self.meter_provider
            && let Err(e) = mp.shutdown()
        {
            eprintln!("app: telemetry shutdown: {e}");
        }
    }
}

/// Sets up trace+metric providers if OTEL_EXPORTER_OTLP_ENDPOINT is set;
/// otherwise returns a disabled, no-op handle (the service still runs — OTel
/// is additive, never a hard dependency of the fleet coming up). `service` is
/// always one of this binary's own subcommand names ("chatterd"/"sysagent"),
/// passed as a `&'static str` literal by every caller — the Meter/Tracer
/// provider APIs require that lifetime.
pub fn init(service: &'static str, node: &str) -> Telemetry {
    let endpoint = std::env::var("OTEL_EXPORTER_OTLP_ENDPOINT").unwrap_or_default();
    if endpoint.is_empty() {
        eprintln!("{service}: otel disabled (OTEL_EXPORTER_OTLP_ENDPOINT not set)");
        return Telemetry::disabled();
    }

    match init_enabled(service, node, &endpoint) {
        Ok(t) => {
            eprintln!("{service}: otel enabled endpoint={endpoint} node={node}");
            t
        }
        Err(e) => {
            eprintln!("{service}: otel init: {e:#} (telemetry disabled)");
            Telemetry::disabled()
        }
    }
}

fn init_enabled(service: &'static str, node: &str, _endpoint: &str) -> Result<Telemetry> {
    let resource = Resource::builder_empty()
        .with_service_name(service.to_string())
        .with_attribute(KeyValue::new("service.version", SERVICE_VERSION))
        .with_attribute(KeyValue::new("deployment.environment", "lab"))
        .with_attribute(KeyValue::new("node.name", node.to_string()))
        .build();

    // Deliberately no `.with_endpoint(...)` — see the module doc comment:
    // building with default options lets the exporter resolve
    // OTEL_EXPORTER_OTLP_ENDPOINT itself and append the per-signal path.
    let span_exporter = SpanExporter::builder()
        .with_http()
        .build()
        .context("trace exporter")?;
    let batch_processor = BatchSpanProcessor::builder(span_exporter)
        .with_batch_config(
            BatchConfigBuilder::default()
                .with_scheduled_delay(TRACE_BATCH_DELAY)
                .build(),
        )
        .build();
    let tracer_provider = SdkTracerProvider::builder()
        .with_span_processor(batch_processor)
        .with_resource(resource.clone())
        .with_sampler(Sampler::AlwaysOn)
        .build();

    let metric_exporter = MetricExporter::builder()
        .with_http()
        .build()
        .context("metric exporter")?;
    let reader = PeriodicReader::builder(metric_exporter)
        .with_interval(METRIC_EXPORT_INTERVAL)
        .build();
    let meter_provider = SdkMeterProvider::builder()
        .with_resource(resource)
        .with_reader(reader)
        .build();

    let tracer = tracer_provider.tracer(service);
    let meter = meter_provider.meter(service);

    Ok(Telemetry {
        enabled: true,
        tracer_provider: Some(tracer_provider),
        meter_provider: Some(meter_provider),
        tracer: Some(tracer),
        meter: Some(meter),
    })
}

/// Starts a "chatterd.deliver" span (chatterd.rs's one instrumented
/// operation), matching go/chatterd.go's handleClient span attributes
/// exactly. Returns the span's lowercase-hex trace id alongside the span
/// itself, since callers print it only when telemetry is enabled.
pub fn start_deliver_span(
    tel: &Telemetry,
    from: &str,
    text_len: usize,
    node: &str,
    from_bridge: bool,
) -> Option<(SdkSpanHandle, String)> {
    let tracer = tel.tracer()?;
    let span = tracer
        .span_builder("chatterd.deliver")
        .with_attributes(vec![
            KeyValue::new("chat.from", from.to_string()),
            KeyValue::new("chat.text_len", text_len as i64),
            KeyValue::new("chat.node", node.to_string()),
            KeyValue::new("chat.from_bridge", from_bridge),
        ])
        .start_with_context(tracer, &OtelContext::new());
    let trace_id = format!("{}", span.span_context().trace_id());
    Some((SdkSpanHandle(span), trace_id))
}

/// A thin wrapper so chatterd.rs doesn't need to name opentelemetry_sdk's
/// concrete span type or import the `Span` trait itself.
pub struct SdkSpanHandle(opentelemetry_sdk::trace::Span);

impl SdkSpanHandle {
    pub fn end_ok(mut self) {
        self.0.set_status(Status::Ok);
        self.0.end();
    }
}

/// A shared, atomically-observed sample the sysagent metric callbacks read
/// from — the Rust analogue of go/sysagent.go's mutex-guarded `latest
/// sample` plus RegisterCallback closure, and sysagent.cpp's GaugeState.
#[derive(Default, Clone, Copy)]
pub struct GaugeSample {
    pub cpu_pct: f64,
    pub mem_pct: f64,
    pub load1: f64,
}

pub struct SysagentGauges {
    state: std::sync::Arc<Mutex<GaugeSample>>,
    // Keeping the builder-returned instrument handles alive is what keeps
    // their callbacks registered with the meter for the process lifetime.
    _cpu: Option<opentelemetry::metrics::ObservableGauge<f64>>,
    _mem: Option<opentelemetry::metrics::ObservableGauge<f64>>,
    _load: Option<opentelemetry::metrics::ObservableGauge<f64>>,
}

impl SysagentGauges {
    pub fn update(&self, s: GaugeSample) {
        if let Ok(mut guard) = self.state.lock() {
            *guard = s;
        }
    }
}

/// Registers the three observable gauges (sysagent_cpu_pct, sysagent_mem_pct,
/// sysagent_load1), each carrying a `node` attribute — the exact names and
/// label verify.lua queries through Grafana's Prometheus proxy. A no-op
/// (empty) registration when telemetry is disabled.
pub fn register_sysagent_gauges(tel: &Telemetry, node: &str) -> SysagentGauges {
    let state = std::sync::Arc::new(Mutex::new(GaugeSample::default()));
    let Some(meter) = tel.meter() else {
        return SysagentGauges {
            state,
            _cpu: None,
            _mem: None,
            _load: None,
        };
    };

    let node_cpu = node.to_string();
    let state_cpu = state.clone();
    let cpu = meter
        .f64_observable_gauge("sysagent_cpu_pct")
        .with_description("CPU utilization percent, from /proc/stat")
        .with_callback(move |observer| {
            let v = state_cpu.lock().map(|g| g.cpu_pct).unwrap_or(0.0);
            observer.observe(v, &[KeyValue::new("node", node_cpu.clone())]);
        })
        .build();

    let node_mem = node.to_string();
    let state_mem = state.clone();
    let mem = meter
        .f64_observable_gauge("sysagent_mem_pct")
        .with_description("Memory-used percent, from /proc/meminfo")
        .with_callback(move |observer| {
            let v = state_mem.lock().map(|g| g.mem_pct).unwrap_or(0.0);
            observer.observe(v, &[KeyValue::new("node", node_mem.clone())]);
        })
        .build();

    let node_load = node.to_string();
    let state_load = state.clone();
    let load1 = meter
        .f64_observable_gauge("sysagent_load1")
        .with_description("1-minute load average, from /proc/loadavg")
        .with_callback(move |observer| {
            let v = state_load.lock().map(|g| g.load1).unwrap_or(0.0);
            observer.observe(v, &[KeyValue::new("node", node_load.clone())]);
        })
        .build();

    SysagentGauges {
        state,
        _cpu: Some(cpu),
        _mem: Some(mem),
        _load: Some(load1),
    }
}

/// Prints the "otel export_errors=N" line sysagent emits when telemetry is
/// enabled (see the module doc comment for why N is always 0 here).
pub fn print_export_errors(service: &str) {
    eprintln!(
        "{service}: otel export_errors={}",
        EXPORT_ERRORS.load(Ordering::Relaxed)
    );
}
