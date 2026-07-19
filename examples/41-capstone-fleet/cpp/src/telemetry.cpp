#include "telemetry.hpp"

#include <cstdio>
#include <cstdlib>

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/common/global_log_handler.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
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

// Counts every Error-level internal log line (the SDK's own signal that an
// export attempt did not get a 2xx / hit a transport failure) instead of
// crashing a supervised service over a telemetry hiccup.
class CountingLogHandler : public opentelemetry::sdk::common::internal_log::LogHandler {
public:
    void Handle(opentelemetry::sdk::common::internal_log::LogLevel level, const char* file, int line,
                const char* msg, const opentelemetry::sdk::common::AttributeMap& attrs) noexcept override {
        using opentelemetry::sdk::common::internal_log::LogLevel;
        if (level == LogLevel::Error) {
            export_errors.fetch_add(1);
            std::fprintf(stderr, "otel: export error: %s\n", msg);
        }
        (void)file;
        (void)line;
        (void)attrs;
    }
};

std::shared_ptr<trace_api::TracerProvider> g_tracer_provider;
std::shared_ptr<metrics_api::MeterProvider> g_meter_provider;

} // namespace

Handle::~Handle() = default;

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

    using namespace opentelemetry::sdk::common::internal_log;
    GlobalLogHandler::SetLogHandler(
        opentelemetry::nostd::shared_ptr<LogHandler>(new CountingLogHandler()));
    GlobalLogHandler::SetLogLevel(LogLevel::Error);

    auto resource = resource_sdk::Resource::Create({
        {"service.name", service},
        {"service.version", "41-capstone-fleet"},
        {"deployment.environment", "lab"},
        {"node.name", node},
    });

    otlp::OtlpHttpExporterOptions trace_opts;
    trace_opts.url = std::string(endpoint) + "/v1/traces";
    auto trace_exporter = otlp::OtlpHttpExporterFactory::Create(trace_opts);
    trace_sdk::BatchSpanProcessorOptions batch_opts;
    batch_opts.schedule_delay_millis = std::chrono::milliseconds(500);
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(trace_exporter), batch_opts);
    g_tracer_provider = trace_sdk::TracerProviderFactory::Create(std::move(processor), resource);
    trace_api::Provider::SetTracerProvider(g_tracer_provider);

    otlp::OtlpHttpMetricExporterOptions metric_opts;
    metric_opts.url = std::string(endpoint) + "/v1/metrics";
    auto metric_exporter = otlp::OtlpHttpMetricExporterFactory::Create(metric_opts);
    metrics_sdk::PeriodicExportingMetricReaderOptions reader_opts;
    reader_opts.export_interval_millis = std::chrono::milliseconds(2000);
    reader_opts.export_timeout_millis = std::chrono::milliseconds(1000);
    auto reader = std::make_shared<metrics_sdk::PeriodicExportingMetricReader>(std::move(metric_exporter), reader_opts);
    auto mp = metrics_sdk::MeterProviderFactory::Create(
        std::make_unique<metrics_sdk::ViewRegistry>(), resource);
    static_cast<metrics_sdk::MeterProvider*>(mp.get())->AddMetricReader(reader);
    g_meter_provider = std::shared_ptr<metrics_api::MeterProvider>(std::move(mp));
    metrics_api::Provider::SetMeterProvider(g_meter_provider);

    std::fprintf(stderr, "%s: otel enabled endpoint=%s node=%s\n", service.c_str(), endpoint, node.c_str());

    Handle h;
    h.enabled = true;
    h.tracer = trace_api::Provider::GetTracerProvider()->GetTracer(service);
    h.meter = metrics_api::Provider::GetMeterProvider()->GetMeter(service);
    return h;
}

} // namespace telemetry
