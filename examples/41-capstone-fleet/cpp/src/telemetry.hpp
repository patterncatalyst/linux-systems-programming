// telemetry.hpp — OTel wiring shared by chatterd and sysagent, following the
// book's house C++ pattern established in ch38 (opentelemetry-cpp via Conan,
// OTLP/HTTP, a NoopLogHandler-derived quiet SDK). Endpoint comes from the
// standard OTEL_EXPORTER_OTLP_ENDPOINT env var, the same variable
// scripts/lab/deploy-to-vm.sh forwards from a caller's OTEL_ENDPOINT — the
// exporters are pointed at "<endpoint>/v1/traces" and "<endpoint>/v1/metrics"
// explicitly here (opentelemetry-cpp's own OTEL_EXPORTER_OTLP_ENDPOINT
// auto-read does exactly this same per-signal suffixing per the OTLP env-var
// spec; setting it explicitly keeps init() self-contained and independent of
// the SDK's own env parsing).
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/tracer.h>

namespace telemetry {

// Count of OTel export failures observed via the SDK's internal Error-level
// log line — opentelemetry-cpp has no direct equivalent of Go's
// otel.SetErrorHandler, so this is the deterministic stand-in the internal
// log handler feeds: printed as "<service>: otel export error: ..." exactly
// once per Error-level line, mirroring what Go's handler does on every
// export failure.
extern std::atomic<long> export_errors;

// Handle owns the trace/metric providers (kept alive so shutdown() can flush
// them) plus the tracer/meter callers actually record against. Move-only;
// when telemetry is disabled every field but `enabled` is left default
// (tracer/meter still resolve to the process-wide no-op API implementations,
// so callers never need to branch on `enabled` before calling into them).
struct Handle {
    bool enabled = false;
    std::shared_ptr<opentelemetry::sdk::trace::TracerProvider> tracer_provider;
    std::shared_ptr<opentelemetry::sdk::metrics::MeterProvider> meter_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter;

    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = default;
    Handle& operator=(Handle&&) = default;

    // Flushes and shuts down both providers, bounded by a short timeout. A
    // no-op when telemetry is disabled. The Go reference never gets a chance
    // to run its own deferred shutdown (every subcommand's dispatch in
    // main.go wraps its run in os.Exit(), which skips deferred calls) — this
    // is deliberately stricter than that, not a divergence in anything
    // observable: it prints nothing either way, and only improves the odds
    // of the last batch actually reaching the collector.
    void shutdown();
};

// init sets up trace+metric providers if OTEL_EXPORTER_OTLP_ENDPOINT is set;
// otherwise returns a disabled, no-op handle (the service still runs — OTel
// is additive, never a hard dependency of the fleet coming up).
Handle init(const std::string& service, const std::string& node);

} // namespace telemetry
