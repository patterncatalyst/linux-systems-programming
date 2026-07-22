// telemetry.hpp — OpenTelemetry wiring for lsp-otel-cpp (chapter 38:
// OpenTelemetry to LGTM). Mirrors the Go reference's initTelemetry: a
// TracerProvider batching spans over OTLP/HTTP, a MeterProvider exporting a
// counter and a histogram on the same short interval, and one Handle that
// owns both so serve's signal path can flush-then-shutdown them explicitly
// -- the C++ RAII equivalent of the Go program's deferred cleanup func,
// called out by hand rather than left to a destructor because "flush before
// exit" has a deadline the process must observe (see flush_and_shutdown).
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/tracer.h>

namespace telemetry {

// service_name tags every span and metric this binary emits; it MUST differ
// from the Go (lsp-otel-go) and Rust (lsp-otel-rust) builds so verify.lua can
// query the LGTM stack for this language's signals alone.
inline constexpr const char* service_name = "lsp-otel-cpp";

// export_interval is short on purpose: the demo and verify.lua both need the
// last batch flushed within a few seconds of shutdown, not the SDK defaults
// (5s span batches are fine; 60s metric batches are not). Matches the Go
// reference's exportInterval exactly so both languages' telemetry lands on
// comparable timescales.
inline constexpr std::chrono::milliseconds export_interval{2000};

// Handle owns the trace/metric providers plus the two instruments serve
// records against; it is move-only (Counter/Histogram are non-copyable
// nostd::unique_ptr) and deliberately has no destructor-driven shutdown --
// flush_and_shutdown() must be called explicitly from the signal path so the
// caller controls the timeout instead of an unobservable process-exit race.
struct Handle {
    std::shared_ptr<opentelemetry::sdk::trace::TracerProvider> tracer_provider;
    std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider> meter_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<std::uint64_t>> requests_total;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> request_duration;

    // flush_and_shutdown force-flushes both providers (draining any batch
    // still in flight) and then shuts them down for good, each bounded by
    // `timeout`. Returns false if either step reports a failure; the caller
    // logs but does not treat that as fatal (mirrors the Go reference, which
    // logs a shutdown error to stderr and still exits 0).
    bool flush_and_shutdown(std::chrono::milliseconds timeout);
};

// init sets up the resource (service.name/version/deployment.environment),
// the OTLP/HTTP trace and metric exporters against `endpoint` (a full URL,
// e.g. "http://localhost:4318" -- the /v1/traces and /v1/metrics paths are
// appended here), and the requests_total counter + request_duration
// histogram instruments. Aborts the process via die() semantics is the
// caller's job; init itself only throws opentelemetry-cpp's own exceptions
// (none, in practice -- the factories are noexcept-safe by construction).
Handle init(const std::string& endpoint);

} // namespace telemetry
