// sysagent.go — the book's /proc-reading metrics agent (ch36), reduced to
// three signals (cpu%, mem%, load1) exported both as a stdout line (cheap,
// always-on local observability) and as OTel gauges over OTLP when
// OTEL_EXPORTER_OTLP_ENDPOINT is set. Also carries the chaos driver's
// "saturate" helper, which the fleet's chaos drill uses to induce load that
// the emitted cpu_pct/mem_pct should visibly reflect.
package main

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"
)

type cpuTimes struct{ idle, total uint64 }

func readCPUTimes() (cpuTimes, error) {
	f, err := os.Open("/proc/stat")
	if err != nil {
		return cpuTimes{}, err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	if !sc.Scan() {
		return cpuTimes{}, fmt.Errorf("empty /proc/stat")
	}
	fields := strings.Fields(sc.Text())
	if len(fields) < 5 || fields[0] != "cpu" {
		return cpuTimes{}, fmt.Errorf("unexpected /proc/stat format: %q", sc.Text())
	}
	var total uint64
	var idle uint64
	for i, s := range fields[1:] {
		v, err := strconv.ParseUint(s, 10, 64)
		if err != nil {
			continue
		}
		total += v
		if i == 3 { // idle is the 4th value
			idle = v
		}
	}
	return cpuTimes{idle: idle, total: total}, nil
}

func cpuPct(prev, cur cpuTimes) float64 {
	dTotal := float64(cur.total - prev.total)
	dIdle := float64(cur.idle - prev.idle)
	if dTotal <= 0 {
		return 0
	}
	return (1 - dIdle/dTotal) * 100
}

func memPct() (float64, error) {
	f, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0, err
	}
	defer f.Close()
	var total, avail float64
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) < 2 {
			continue
		}
		v, err := strconv.ParseFloat(fields[1], 64)
		if err != nil {
			continue
		}
		switch strings.TrimSuffix(fields[0], ":") {
		case "MemTotal":
			total = v
		case "MemAvailable":
			avail = v
		}
	}
	if total <= 0 {
		return 0, fmt.Errorf("MemTotal not found")
	}
	return (1 - avail/total) * 100, nil
}

func load1() (float64, error) {
	b, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return 0, err
	}
	fields := strings.Fields(string(b))
	if len(fields) < 1 {
		return 0, fmt.Errorf("empty /proc/loadavg")
	}
	return strconv.ParseFloat(fields[0], 64)
}

type sample struct {
	cpuPct, memPct, load1 float64
}

func takeSample(prev *cpuTimes) (sample, error) {
	cur, err := readCPUTimes()
	if err != nil {
		return sample{}, err
	}
	var s sample
	if prev.total > 0 {
		s.cpuPct = cpuPct(*prev, cur)
	}
	*prev = cur
	if s.memPct, err = memPct(); err != nil {
		return sample{}, err
	}
	if s.load1, err = load1(); err != nil {
		return sample{}, err
	}
	return s, nil
}

func sysagentRun(node string, intervalMs int, once bool, tel *telemetry) int {
	var mu sync.Mutex
	var latest sample

	if tel.enabled {
		cpuGauge, _ := tel.meter.Float64ObservableGauge("sysagent.cpu.pct",
			metric.WithDescription("CPU utilization percent, from /proc/stat"))
		memGauge, _ := tel.meter.Float64ObservableGauge("sysagent.mem.pct",
			metric.WithDescription("Memory-used percent, from /proc/meminfo"))
		loadGauge, _ := tel.meter.Float64ObservableGauge("sysagent.load1",
			metric.WithDescription("1-minute load average, from /proc/loadavg"))
		_, err := tel.meter.RegisterCallback(func(ctx context.Context, o metric.Observer) error {
			mu.Lock()
			s := latest
			mu.Unlock()
			attrs := metric.WithAttributes(attribute.String("node", node))
			o.ObserveFloat64(cpuGauge, s.cpuPct, attrs)
			o.ObserveFloat64(memGauge, s.memPct, attrs)
			o.ObserveFloat64(loadGauge, s.load1, attrs)
			return nil
		}, cpuGauge, memGauge, loadGauge)
		if err != nil {
			fmt.Fprintf(os.Stderr, "sysagent: otel callback registration: %v\n", err)
		}
	}

	sig := installSignalFlag()
	var prev cpuTimes
	// Prime the CPU-time baseline so the first printed sample isn't a bogus 0.
	prev, _ = readCPUTimes()
	time.Sleep(200 * time.Millisecond)

	for {
		s, err := takeSample(&prev)
		if err != nil {
			fmt.Fprintf(os.Stderr, "sysagent: sample: %v\n", err)
			return 1
		}
		mu.Lock()
		latest = s
		mu.Unlock()
		fmt.Printf("sysagent: node=%s cpu_pct=%.2f mem_pct=%.2f load1=%.2f ts=%d\n",
			node, s.cpuPct, s.memPct, s.load1, time.Now().Unix())

		if once {
			break
		}
		if sig.Load() {
			break
		}
		time.Sleep(time.Duration(intervalMs) * time.Millisecond)
		if sig.Load() {
			break
		}
	}
	if tel.enabled {
		fmt.Fprintf(os.Stderr, "sysagent: otel export_errors=%d\n", exportErrors.Load())
	}
	return 0
}

// saturate — chaos helper: busy-spin goroutines (cpu) or hold allocated,
// touched memory (mem) for --seconds, so a concurrently-running sysagent's
// cpu_pct/mem_pct visibly rises (this is the fleet's USE-method callback).
func saturate(resource string, seconds, workers, mb int) int {
	switch resource {
	case "cpu":
		if workers <= 0 {
			workers = runtime.NumCPU()
		}
		fmt.Printf("sysagent: saturate resource=cpu seconds=%d workers=%d started\n", seconds, workers)
		stop := time.Now().Add(time.Duration(seconds) * time.Second)
		var wg sync.WaitGroup
		for i := 0; i < workers; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				x := 0.0001
				for time.Now().Before(stop) {
					x = x*1.0000001 + 1.0
				}
				if x == 0 { // never true; keeps the compiler from eliding the loop
					fmt.Println(x)
				}
			}()
		}
		wg.Wait()
		fmt.Println("sysagent: saturate done")
		return 0
	case "mem":
		if mb <= 0 {
			mb = 256
		}
		fmt.Printf("sysagent: saturate resource=mem seconds=%d mb=%d started\n", seconds, mb)
		buf := make([]byte, mb*1024*1024)
		for i := range buf {
			buf[i] = byte(i) // touch every page so it's resident, not just reserved
		}
		time.Sleep(time.Duration(seconds) * time.Second)
		_ = buf[0]
		fmt.Println("sysagent: saturate done")
		return 0
	default:
		fmt.Fprintf(os.Stderr, "usage: sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]\n")
		return 2
	}
}
