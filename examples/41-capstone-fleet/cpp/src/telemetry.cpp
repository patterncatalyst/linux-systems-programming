#include "telemetry.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/common/global_log_handler.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/view/view_registry.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/samplers/always_on_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

namespace telemetry {

std::atomic<long> export_errors{0};

namespace {

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace resource_sdk = opentelemetry::sdk::resource;
namespace otlp = opentelemetry::exporter::otlp;
namespace internal_log = opentelemetry::sdk::common::internal_log;

// Counts every Error-level internal SDK log line and prints it, the closest
// opentelemetry-cpp equivalent of Go's otel.SetErrorHandler callback (which
// both increments exportErrors and Fprintf's to stderr on every export
// failure). LogLevel is pinned to Error below, so Info/Debug/Warning lines —
// the ones that would otherwise corrupt the stdout contract, ch38's hard-won
// lesson — never reach this handler at all.
class CountingLogHandler : public internal_log::LogHandler {
public:
    explicit CountingLogHandler(std::string service) : service_(std::move(service)) {}

    void Handle(internal_log::LogLevel level, const char* /*file*/, int /*line*/, const char* msg,
                const opentelemetry::sdk::common::AttributeMap& /*attrs*/) noexcept override {
        if (level == internal_log::LogLevel::Error) {
            export_errors.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "%s: otel export error: %s\n", service_.c_str(), msg);
        }
    }

private:
    std::string service_;
};

} // namespace

void Handle::shutdown() {
    if (!enabled) {
        return;
    }
    constexpr auto kTimeout = std::chrono::seconds(5);
    if (tracer_provider) {
        tracer_provider->ForceFlush(kTimeout);
        tracer_provider->Shutdown(kTimeout);
    }
    if (meter_provider) {
        meter_provider->ForceFlush(kTimeout);
        meter_provider->Shutdown(kTimeout);
    }
}

Handle init(const std::string& service, const std::string& node) {
    const char* endpoint = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    if (endpoint == nullptr || endpoint[0] == '\0') {
        std::fprintf(stderr, "%s: otel disabled (OTEL_EXPORTER_OTLP_ENDPOINT not set)\n", service.c_str());
        Handle h;
        h.enabled = false;
        h.tracer = trace_api::Provider::GetTracerProvider()->GetTracer(service);
        h.meter = metrics_api::Provider::GetMeterProvider()->GetMeter(service);
        return h;
    }

    internal_log::GlobalLogHandler::SetLogHandler(
        opentelemetry::nostd::shared_ptr<internal_log::LogHandler>(new CountingLogHandler(service)));
    internal_log::GlobalLogHandler::SetLogLevel(internal_log::LogLevel::Error);

    auto resource = resource_sdk::Resource::Create({
        {"service.name", service},
        {"service.version", "41-capstone-fleet"},
        {"deployment.environment", "lab"},
        {"node.name", node},
    });

    // --- traces: OTLP/HTTP exporter, batched every 500ms (matches the Go
    // reference's sdktrace.WithBatchTimeout). ---
    otlp::OtlpHttpExporterOptions trace_opts;
    trace_opts.url = std::string(endpoint) + "/v1/traces";
    auto trace_exporter = otlp::OtlpHttpExporterFactory::Create(trace_opts);

    trace_sdk::BatchSpanProcessorOptions batch_opts;
    batch_opts.schedule_delay_millis = std::chrono::milliseconds(500);
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(trace_exporter), batch_opts);

    auto sampler = trace_sdk::AlwaysOnSamplerFactory::Create();
    std::shared_ptr<trace_sdk::TracerProvider> tracer_provider(
        trace_sdk::TracerProviderFactory::Create(std::move(processor), resource, std::move(sampler)));
    trace_api::Provider::SetTracerProvider(
        opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>(tracer_provider));

    // --- metrics: OTLP/HTTP exporter, exported every 2s (matches the Go
    // reference's sdkmetric.WithInterval). ---
    otlp::OtlpHttpMetricExporterOptions metric_opts;
    metric_opts.url = std::string(endpoint) + "/v1/metrics";
    auto metric_exporter = otlp::OtlpHttpMetricExporterFactory::Create(metric_opts);

    metrics_sdk::PeriodicExportingMetricReaderOptions reader_opts;
    reader_opts.export_interval_millis = std::chrono::milliseconds(2000);
    reader_opts.export_timeout_millis = std::chrono::milliseconds(1000);
    auto reader =
        std::make_shared<metrics_sdk::PeriodicExportingMetricReader>(std::move(metric_exporter), reader_opts);

    std::shared_ptr<metrics_sdk::MeterProvider> meter_provider(
        metrics_sdk::MeterProviderFactory::Create(std::make_unique<metrics_sdk::ViewRegistry>(), resource));
    meter_provider->AddMetricReader(reader);
    metrics_api::Provider::SetMeterProvider(
        opentelemetry::nostd::shared_ptr<metrics_api::MeterProvider>(meter_provider));

    std::fprintf(stderr, "%s: otel enabled endpoint=%s node=%s\n", service.c_str(), endpoint, node.c_str());

    Handle h;
    h.enabled = true;
    h.tracer_provider = tracer_provider;
    h.meter_provider = meter_provider;
    h.tracer = trace_api::Provider::GetTracerProvider()->GetTracer(service);
    h.meter = metrics_api::Provider::GetMeterProvider()->GetMeter(service);
    return h;
}

} // namespace telemetry
