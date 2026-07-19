// telemetry.hpp — OTel wiring shared by chatterd and sysagent, following the
// book's house C++ pattern (opentelemetry-cpp via Conan, OTLP/HTTP). Endpoint
// comes from the standard OTEL_EXPORTER_OTLP_ENDPOINT env var, which the
// exporters read themselves — the same variable
// scripts/lab/deploy-to-vm.sh forwards from a caller's OTEL_ENDPOINT.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/trace/tracer.h>

namespace telemetry {

// Count of OTel export failures observed via the global error/exception
// handler, the deterministic stand-in for "the exporter got a 200" (the
// batch/periodic exporters only invoke it on a non-2xx response or a
// transport failure).
extern std::atomic<long> export_errors;

struct Handle {
    bool enabled = false;
    std::shared_ptr<opentelemetry::trace::Tracer> tracer;
    std::shared_ptr<opentelemetry::metrics::Meter> meter;

    ~Handle();
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = default;
    Handle& operator=(Handle&&) = default;
};

// init sets up trace+metric providers if OTEL_EXPORTER_OTLP_ENDPOINT is set;
// otherwise returns a disabled, no-op handle (the service still runs).
Handle init(const std::string& service, const std::string& node);

} // namespace telemetry
