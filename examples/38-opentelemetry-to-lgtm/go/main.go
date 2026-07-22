// lsp-otel — a traced request/response loop (chapter 38: OpenTelemetry to LGTM).
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
// Go way of RAII (deferred cleanup, no destructors to rely on).
package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
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

const serviceName = "lsp-otel-go"

// exportInterval is short on purpose: the demo and verify.lua both need the
// last batch flushed within a few seconds of shutdown, not the SDK
// defaults (5s span batches are fine; 60s metric batches are not).
const exportInterval = 2 * time.Second

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app serve --port PORT [--otel-endpoint URL]")
	fmt.Fprintln(os.Stderr, "       app drive --n N --addr HOST:PORT")
	os.Exit(2)
}

func die(err error) {
	fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
	os.Exit(1)
}

// ---------------------------------------------------------------- telemetry

type telemetry struct {
	tracerProvider *sdktrace.TracerProvider
	meterProvider  *sdkmetric.MeterProvider
	tracer         trace.Tracer
	requestsTotal  metric.Int64Counter
	requestDur     metric.Float64Histogram
}

// otlpHostPort strips a URL scheme: the otlptrace/otlpmetric HTTP exporters
// take host:port, not a URL (they prepend the protocol themselves).
func otlpHostPort(endpoint string) string {
	hp := strings.TrimPrefix(endpoint, "http://")
	hp = strings.TrimPrefix(hp, "https://")
	return hp
}

func initTelemetry(ctx context.Context, endpoint string) (*telemetry, func(context.Context) error, error) {
	res, err := resource.New(ctx,
		resource.WithAttributes(
			semconv.ServiceName(serviceName),
			semconv.ServiceVersion("1.0"),
			semconv.DeploymentEnvironment("local"),
		),
	)
	if err != nil {
		return nil, nil, fmt.Errorf("resource: %w", err)
	}

	hostPort := otlpHostPort(endpoint)

	traceExp, err := otlptracehttp.New(ctx,
		otlptracehttp.WithEndpoint(hostPort),
		otlptracehttp.WithInsecure(),
	)
	if err != nil {
		return nil, nil, fmt.Errorf("trace exporter: %w", err)
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(traceExp, sdktrace.WithBatchTimeout(exportInterval)),
		sdktrace.WithResource(res),
		sdktrace.WithSampler(sdktrace.AlwaysSample()),
	)
	otel.SetTracerProvider(tp)

	metricExp, err := otlpmetrichttp.New(ctx,
		otlpmetrichttp.WithEndpoint(hostPort),
		otlpmetrichttp.WithInsecure(),
	)
	if err != nil {
		return nil, nil, fmt.Errorf("metric exporter: %w", err)
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithResource(res),
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExp,
			sdkmetric.WithInterval(exportInterval))),
	)
	otel.SetMeterProvider(mp)

	meter := mp.Meter(serviceName)
	requestsTotal, err := meter.Int64Counter("requests_total",
		metric.WithDescription("Total requests handled"),
		metric.WithUnit("1"))
	if err != nil {
		return nil, nil, fmt.Errorf("counter: %w", err)
	}
	requestDur, err := meter.Float64Histogram("request_duration",
		metric.WithDescription("Request duration"),
		metric.WithUnit("ms"))
	if err != nil {
		return nil, nil, fmt.Errorf("histogram: %w", err)
	}

	t := &telemetry{
		tracerProvider: tp,
		meterProvider:  mp,
		tracer:         tp.Tracer(serviceName),
		requestsTotal:  requestsTotal,
		requestDur:     requestDur,
	}

	shutdown := func(ctx context.Context) error {
		var errs []error
		if err := tp.Shutdown(ctx); err != nil {
			errs = append(errs, err)
		}
		if err := mp.Shutdown(ctx); err != nil {
			errs = append(errs, err)
		}
		return errors.Join(errs...)
	}
	return t, shutdown, nil
}

// ------------------------------------------------------------------- serve

// handleRequest is the parse->work->respond pipeline for one line of input.
// "parse" happens inline (cheap: one strconv.Atoi); "work" and "respond"
// are the two child spans under the "request" parent, per the trace
// contract every language in this example follows identically.
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

func serveConn(t *telemetry, conn net.Conn) {
	defer conn.Close()
	r := bufio.NewScanner(conn)
	w := bufio.NewWriter(conn)
	for r.Scan() {
		line := strings.TrimSpace(r.Text())
		if line == "" {
			continue
		}
		seq, err := strconv.Atoi(line)
		if err != nil {
			fmt.Fprintf(w, "err bad-request\n")
			w.Flush()
			continue
		}
		if err := handleRequest(context.Background(), t, w, seq); err != nil {
			fmt.Fprintln(os.Stderr, "app: request error:", err)
			return
		}
	}
}

func runServe(args []string) int {
	port := ""
	endpoint := os.Getenv("OTEL_ENDPOINT")
	if endpoint == "" {
		endpoint = "http://localhost:4318"
	}
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--port":
			i++
			if i >= len(args) {
				usage()
			}
			port = args[i]
		case "--otel-endpoint":
			i++
			if i >= len(args) {
				usage()
			}
			endpoint = args[i]
		default:
			usage()
		}
	}
	if port == "" {
		usage()
	}

	ctx := context.Background()
	t, shutdown, err := initTelemetry(ctx, endpoint)
	if err != nil {
		die(fmt.Errorf("telemetry init: %w", err))
	}

	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		die(fmt.Errorf("listen: %w", err))
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	connCh := make(chan net.Conn)
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return // listener closed during shutdown
			}
			connCh <- conn
		}
	}()

	fmt.Printf("app: serve listening on 127.0.0.1:%s\n", port)

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
}

// ------------------------------------------------------------------- drive

// runDrive is a plain, untraced TCP client: it generates the load that
// makes serve's telemetry interesting, but emits none of its own — the
// service under observation is serve, not drive.
func runDrive(args []string) int {
	n := 0
	addr := "127.0.0.1:8080"
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--n":
			i++
			if i >= len(args) {
				usage()
			}
			v, err := strconv.Atoi(args[i])
			if err != nil {
				die(fmt.Errorf("--n: %w", err))
			}
			n = v
		case "--addr":
			i++
			if i >= len(args) {
				usage()
			}
			addr = args[i]
		default:
			usage()
		}
	}
	if n <= 0 {
		usage()
	}

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		die(fmt.Errorf("dial %s: %w", addr, err))
	}
	defer conn.Close()

	w := bufio.NewWriter(conn)
	r := bufio.NewScanner(conn)
	ok := 0
	for seq := 1; seq <= n; seq++ {
		if _, err := fmt.Fprintf(w, "%d\n", seq); err != nil {
			die(fmt.Errorf("send request %d: %w", seq, err))
		}
		if err := w.Flush(); err != nil {
			die(fmt.Errorf("flush request %d: %w", seq, err))
		}
		if !r.Scan() {
			if err := r.Err(); err != nil && !errors.Is(err, io.EOF) {
				die(fmt.Errorf("read reply %d: %w", seq, err))
			}
			break
		}
		if strings.HasPrefix(r.Text(), "ok ") {
			ok++
		}
	}
	fmt.Printf("app: drive sent %d/%d ok\n", ok, n)
	if ok != n {
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------main

func main() {
	if len(os.Args) < 2 {
		usage()
	}
	var code int
	switch os.Args[1] {
	case "serve":
		code = runServe(os.Args[2:])
	case "drive":
		code = runDrive(os.Args[2:])
	default:
		usage()
	}
	os.Exit(code)
}
