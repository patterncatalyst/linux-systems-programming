// telemetry.go — OTel wiring shared by chatterd and sysagent. Both signals
// (traces for chatterd deliveries, metrics for sysagent's /proc samples) are
// exported over OTLP/HTTP. Endpoint comes from the standard
// OTEL_EXPORTER_OTLP_ENDPOINT env var (the exporters read it themselves, per
// the OTel env-var spec) — this is exactly the variable
// scripts/lab/deploy-to-vm.sh forwards from a caller's OTEL_ENDPOINT, so a
// service started under pmon on a lab guest picks it up with no extra flag.
package main

import (
	"context"
	"fmt"
	"os"
	"sync/atomic"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/metric"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"go.opentelemetry.io/otel/trace"
)

// exportErrors counts OTel export failures observed via the global error
// handler. verify polls this (via the "otel: export_errors=N" line each
// service prints at exit / on request) as the deterministic stand-in for
// "the exporter got a 200" — the batch/periodic exporters only ever call the
// error handler on a non-2xx response or a transport failure.
var exportErrors atomic.Int64

type telemetry struct {
	enabled  bool
	tracer   trace.Tracer
	meter    metric.Meter
	shutdown func(context.Context) error
}

func noopTelemetry(service string) *telemetry {
	return &telemetry{
		enabled:  false,
		tracer:   otel.Tracer(service),
		meter:    otel.Meter(service),
		shutdown: func(context.Context) error { return nil },
	}
}

// initTelemetry sets up trace+metric providers if OTEL_EXPORTER_OTLP_ENDPOINT
// is set; otherwise it returns a disabled, no-op telemetry (the service still
// runs — OTel is additive, never a hard dependency of the fleet coming up).
func initTelemetry(ctx context.Context, service, node string) *telemetry {
	endpoint := os.Getenv("OTEL_EXPORTER_OTLP_ENDPOINT")
	if endpoint == "" {
		fmt.Fprintf(os.Stderr, "%s: otel disabled (OTEL_EXPORTER_OTLP_ENDPOINT not set)\n", service)
		return noopTelemetry(service)
	}

	otel.SetErrorHandler(otel.ErrorHandlerFunc(func(err error) {
		exportErrors.Add(1)
		fmt.Fprintf(os.Stderr, "%s: otel export error: %v\n", service, err)
	}))

	res, err := resource.New(ctx,
		resource.WithAttributes(
			semconv.ServiceName(service),
			semconv.ServiceVersion("41-capstone-fleet"),
			semconv.DeploymentEnvironment("lab"),
			attribute.String("node.name", node),
		),
	)
	if err != nil {
		// resource.New only fails on attribute-schema conflicts, which cannot
		// happen with the static set above; treat as unreachable but degrade
		// to no-op rather than crash a supervised service over telemetry.
		fmt.Fprintf(os.Stderr, "%s: otel resource: %v (telemetry disabled)\n", service, err)
		return noopTelemetry(service)
	}

	traceExp, err := otlptracehttp.New(ctx)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s: otel trace exporter: %v (telemetry disabled)\n", service, err)
		return noopTelemetry(service)
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(traceExp, sdktrace.WithBatchTimeout(500*time.Millisecond)),
		sdktrace.WithResource(res),
	)
	otel.SetTracerProvider(tp)

	metricExp, err := otlpmetrichttp.New(ctx)
	if err != nil {
		fmt.Fprintf(os.Stderr, "%s: otel metric exporter: %v (telemetry disabled)\n", service, err)
		return noopTelemetry(service)
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithResource(res),
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExp, sdkmetric.WithInterval(2*time.Second))),
	)
	otel.SetMeterProvider(mp)

	fmt.Fprintf(os.Stderr, "%s: otel enabled endpoint=%s node=%s\n", service, endpoint, node)

	return &telemetry{
		enabled: true,
		tracer:  otel.Tracer(service),
		meter:   otel.Meter(service),
		shutdown: func(ctx context.Context) error {
			err1 := tp.Shutdown(ctx)
			err2 := mp.Shutdown(ctx)
			if err1 != nil {
				return err1
			}
			return err2
		},
	}
}
