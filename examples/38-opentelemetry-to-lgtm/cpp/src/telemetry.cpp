#include "telemetry.hpp"

#include <memory>
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

namespace {

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace resource_sdk = opentelemetry::sdk::resource;
namespace otlp = opentelemetry::exporter::otlp;

} // namespace

bool Handle::flush_and_shutdown(std::chrono::milliseconds timeout) {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    bool ok = true;
    if (tracer_provider && !tracer_provider->ForceFlush(us)) {
        ok = false;
    }
    if (tracer_provider && !tracer_provider->Shutdown(us)) {
        ok = false;
    }
    if (meter_provider && !meter_provider->ForceFlush(us)) {
        ok = false;
    }
    if (meter_provider && !meter_provider->Shutdown(us)) {
        ok = false;
    }
    return ok;
}

Handle init(const std::string& endpoint) {
    // The SDK's default log handler writes Warning/Error to stderr and
    // Info/Debug to stdout -- the latter would corrupt serve's stdout
    // contract (exactly two lines, ever). A no-op handler keeps this build
    // as quiet as the Go and Rust ports, which have no internal SDK logging
    // to begin with.
    opentelemetry::sdk::common::internal_log::GlobalLogHandler::SetLogHandler(
        opentelemetry::nostd::shared_ptr<opentelemetry::sdk::common::internal_log::LogHandler>(
            new opentelemetry::sdk::common::internal_log::NoopLogHandler()));
    opentelemetry::sdk::common::internal_log::GlobalLogHandler::SetLogLevel(
        opentelemetry::sdk::common::internal_log::LogLevel::None);

    auto resource = resource_sdk::Resource::Create({
        {"service.name", service_name},
        {"service.version", "1.0"},
        {"deployment.environment", "local"},
    });

    // --- traces: OTLP/HTTP exporter, batched every export_interval. ---
    otlp::OtlpHttpExporterOptions trace_opts;
    trace_opts.url = endpoint + "/v1/traces";
    auto trace_exporter = otlp::OtlpHttpExporterFactory::Create(trace_opts);

    trace_sdk::BatchSpanProcessorOptions batch_opts;
    batch_opts.schedule_delay_millis = export_interval;
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(trace_exporter), batch_opts);

    auto sampler = trace_sdk::AlwaysOnSamplerFactory::Create();
    // Keep the concrete sdk::trace::TracerProvider alive as a shared_ptr:
    // Handle needs it (by its sdk type, for ForceFlush/Shutdown) and the
    // global API registration needs it too (upcast to the API's
    // trace::TracerProvider via nostd::shared_ptr's std::shared_ptr ctor).
    std::shared_ptr<trace_sdk::TracerProvider> tracer_provider(
        trace_sdk::TracerProviderFactory::Create(std::move(processor), resource, std::move(sampler)));
    trace_api::Provider::SetTracerProvider(
        opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>(tracer_provider));

    // --- metrics: OTLP/HTTP exporter, exported every export_interval. ---
    otlp::OtlpHttpMetricExporterOptions metric_opts;
    metric_opts.url = endpoint + "/v1/metrics";
    auto metric_exporter = otlp::OtlpHttpMetricExporterFactory::Create(metric_opts);

    metrics_sdk::PeriodicExportingMetricReaderOptions reader_opts;
    reader_opts.export_interval_millis = export_interval;
    reader_opts.export_timeout_millis = export_interval;
    auto reader =
        std::make_shared<metrics_sdk::PeriodicExportingMetricReader>(std::move(metric_exporter), reader_opts);

    std::shared_ptr<metrics_sdk::MeterProvider> meter_provider(
        metrics_sdk::MeterProviderFactory::Create(std::make_unique<metrics_sdk::ViewRegistry>(), resource));
    meter_provider->AddMetricReader(reader);
    metrics_api::Provider::SetMeterProvider(
        opentelemetry::nostd::shared_ptr<metrics_api::MeterProvider>(meter_provider));

    Handle h;
    h.tracer_provider = tracer_provider;
    h.meter_provider = meter_provider;
    h.tracer = trace_api::Provider::GetTracerProvider()->GetTracer(service_name);
    auto meter = metrics_api::Provider::GetMeterProvider()->GetMeter(service_name);
    h.requests_total = meter->CreateUInt64Counter("requests_total", "Total requests handled", "1");
    h.request_duration = meter->CreateDoubleHistogram("request_duration", "Request duration", "ms");
    return h;
}

} // namespace telemetry
