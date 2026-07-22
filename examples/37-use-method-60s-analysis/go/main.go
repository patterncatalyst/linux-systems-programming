// loadmix (chapter 37): a saturation generator + guided-analysis harness for
// the classic Brendan Gregg 60-second USE-method checklist.
//
//	app --resource cpu|mem|io|net --seconds N   saturate that resource
//	app analyze [--seconds N]                    run the checklist and name
//	                                              the saturated resource
//
// `analyze` shells out to the real system tools (uptime, dmesg, vmstat,
// mpstat, pidstat, iostat, free, sar, ss, top) exactly as a human running the
// checklist would, parses their plain-text tables generically (by column
// *name*, read from each tool's own header row, never a hardcoded position),
// and for each of the four resources reports which USE signals — Utilization
// and Saturation — fired against a fixed threshold. Errors is reported too,
// fixed false: this chapter induces load, not kernel-logged faults, so a
// real checklist run finds none.
//
// Go idioms used throughout, in place of the C++ reference's jthread pools,
// std::atomic counters, and std::expected: goroutines joined with
// sync.WaitGroup, atomic counters from sync/atomic, golang.org/x/sys/unix
// for the O_DSYNC pwrite path, and net.UDPConn with SetReadDeadline for the
// net saturator's receiver. Every observable line below is a byte-for-byte
// match of the C++ std::println output (field names, order, and spacing);
// only floating-point rounding may differ, which is why verify.lua checks
// shapes (%d+%.%d%d) rather than exact decimals.
package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/sys/unix"
)

// ------------------------------------------------------------- utilities --

func nproc() int {
	n := runtime.NumCPU()
	if n == 0 {
		return 1
	}
	return n
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func trim(s string) string {
	return strings.Trim(s, " \t\r\n")
}

func splitWs(line string) []string {
	return strings.Fields(line)
}

// splitLines mirrors std::getline over an istringstream: split on '\n', and
// drop the final empty element produced when the text ends with a trailing
// newline (getline never yields that last empty read).
func splitLines(text string) []string {
	if text == "" {
		return nil
	}
	parts := strings.Split(text, "\n")
	if parts[len(parts)-1] == "" {
		parts = parts[:len(parts)-1]
	}
	return parts
}

// row is a parsed table row: column names (from a tool's own header line)
// zipped with this row's values, so every parser below reads fields *by
// name* rather than by a hardcoded position — robust to sysstat/procps
// column reordering across versions.
type row struct {
	names  []string
	values []string
}

func (r row) getOr(name string, def float64) float64 {
	n := len(r.names)
	if len(r.values) < n {
		n = len(r.values)
	}
	for i := 0; i < n; i++ {
		if r.names[i] == name {
			v, err := strconv.ParseFloat(r.values[i], 64)
			if err != nil {
				return def
			}
			return v
		}
	}
	return def
}

func makeRow(headerTokens []string, headerSkip int, dataTokens []string, dataSkip int) row {
	var r row
	if headerSkip < len(headerTokens) {
		r.names = append(r.names, headerTokens[headerSkip:]...)
	}
	if dataSkip < len(dataTokens) {
		r.values = append(r.values, dataTokens[dataSkip:]...)
	}
	return r
}

// captureResult is the Go analogue of std::expected<string, error_code>: err
// is set only when the shell itself could not be started (the "truly
// fallible operation" per the book's convention), never when the captured
// command exits non-zero — its output is still valid text either way.
type captureResult struct {
	out string
	err error
}

// runCapture runs a short command to completion and captures combined
// stdout+stderr. Forced to the C locale so decimal points/dates are
// unambiguous regardless of the guest's configured locale. Grouped in a
// subshell so a single trailing "2>&1" captures every stage of a pipeline
// (e.g. "dmesg | tail -n 5") — without the parens, 2>&1 binds only to the
// pipeline's last stage and an earlier stage's stderr (e.g. dmesg's own
// permission-denied message) leaks past the capture.
func runCapture(cmd string) captureResult {
	full := "( env LC_ALL=C LANG=C " + cmd + " ) 2>&1"
	out, err := exec.Command("sh", "-c", full).Output()
	if err != nil {
		if _, ok := err.(*exec.ExitError); ok {
			// the command ran and produced output but exited non-zero --
			// that's not a capture failure, just like popen()/pclose()
			// never surfacing the child's exit status to the caller here.
			return captureResult{out: string(out)}
		}
		return captureResult{err: err}
	}
	return captureResult{out: string(out)}
}

// runToFile runs `env LC_ALL=C LANG=C <argv...>` to completion, redirecting
// stdout+stderr to outfile. Used for the five interval samplers, each on its
// own goroutine, so they run *concurrently* — a 12s analyze window costs
// ~12s wall-clock, not 5x that from running them one at a time.
func runToFile(argv []string, outfile string) {
	full := append([]string{"env", "LC_ALL=C", "LANG=C"}, argv...)
	f, err := os.OpenFile(outfile, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0600)
	if err != nil {
		return
	}
	defer f.Close()
	cmd := exec.Command(full[0], full[1:]...)
	cmd.Stdout = f
	cmd.Stderr = f
	_ = cmd.Run()
}

func readAll(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(b)
}

// ------------------------------------------------------- checklist parsers --
// Each parser reads a header line's column names from the tool's own output
// and zips subsequent data lines against that name list — see row.getOr
// above.

type vmstatResult struct {
	runQueueMax float64
	swapIOMax   float64 // si+so, KB/s
}

// vmstat has no aggregate "Average:" line (unlike mpstat/pidstat), and its
// *first* sample is a since-boot average, not a live interval — both quirks
// are handled here: we scan every data row after the header, skip row 0, and
// take the max across the rest (the steady-state saturation signal, not
// diluted by an average across the whole window).
func parseVmstat(text string) vmstatResult {
	var res vmstatResult
	var header []string
	var dataRows [][]string
	for _, line := range splitLines(text) {
		toks := splitWs(line)
		if len(toks) == 0 {
			continue
		}
		if header == nil {
			hasSwpd := false
			for _, t := range toks {
				if t == "swpd" {
					hasSwpd = true
					break
				}
			}
			if hasSwpd {
				header = toks
			}
			continue
		}
		if len(toks)+2 >= len(header) {
			dataRows = append(dataRows, toks)
		}
	}
	for i := 1; i < len(dataRows); i++ { // skip row 0 (since-boot average)
		r := makeRow(header, 0, dataRows[i], 0)
		res.runQueueMax = max(res.runQueueMax, r.getOr("r", 0.0))
		si, so := r.getOr("si", 0.0), r.getOr("so", 0.0)
		res.swapIOMax = max(res.swapIOMax, si+so)
	}
	return res
}

type mpstatResult struct {
	busyPct   float64
	iowaitPct float64
}

// mpstat -P ALL's own "Average:" row over the whole window (excludes the
// since-boot first report automatically — sysstat's job, not ours).
func parseMpstat(text string) mpstatResult {
	res := mpstatResult{}
	var header, data []string
	for _, line := range splitLines(text) {
		toks := splitWs(line)
		if len(toks) < 2 {
			continue
		}
		if toks[0] == "Average:" && toks[1] == "CPU" {
			header = toks
		} else if toks[0] == "Average:" && toks[1] == "all" {
			data = toks
		}
	}
	if header == nil || data == nil {
		return res
	}
	r := makeRow(header, 2, data, 2)
	idle := r.getOr("%idle", 100.0)
	res.iowaitPct = r.getOr("%iowait", 0.0)
	res.busyPct = 100.0 - idle
	return res
}

func parseFreeUsedPct(text string) float64 {
	var header, data []string
	for _, line := range splitLines(text) {
		toks := splitWs(line)
		if len(toks) == 0 {
			continue
		}
		hasAvailable := false
		for _, t := range toks {
			if t == "available" {
				hasAvailable = true
				break
			}
		}
		if hasAvailable {
			header = toks
		} else if toks[0] == "Mem:" {
			data = toks
		}
	}
	if header == nil || data == nil {
		return 0.0
	}
	r := makeRow(header, 0, data, 1)
	total := r.getOr("total", 0.0)
	avail := r.getOr("available", 0.0)
	if total <= 0.0 {
		return 0.0
	}
	return (total - avail) / total * 100.0
}

type iostatResult struct {
	utilMax  float64
	awaitMax float64 // w_await, ms
}

// iostat -xz has no Average line either, AND -z hides idle devices entirely
// (they just don't appear in a quiet interval) — so we scan every per-second
// block, skip block 0 (since-boot), skip loopback/removable/compressed
// pseudo-devices, and take the max over the guest's real block device.
func parseIostat(text string) iostatResult {
	var res iostatResult
	excl := []string{"zram", "sr", "loop", "dm-"}
	blockIdx := -1
	var header []string
	for _, line := range splitLines(text) {
		toks := splitWs(line)
		if len(toks) == 0 {
			continue
		}
		if toks[0] == "avg-cpu:" {
			blockIdx++
			continue
		}
		if toks[0] == "Device" {
			header = toks
			continue
		}
		if header == nil || blockIdx < 1 { // no header yet, or since-boot block
			continue
		}
		dev := toks[0]
		skip := false
		for _, p := range excl {
			if strings.HasPrefix(dev, p) {
				skip = true
				break
			}
		}
		if skip {
			continue
		}
		r := makeRow(header, 1, toks, 1)
		res.utilMax = max(res.utilMax, r.getOr("%util", 0.0))
		res.awaitMax = max(res.awaitMax, r.getOr("w_await", 0.0))
	}
	return res
}

func parseSarPkts(text, iface string) float64 {
	var header, data []string
	for _, line := range splitLines(text) {
		toks := splitWs(line)
		if len(toks) < 2 {
			continue
		}
		if toks[0] == "Average:" && toks[1] == "IFACE" {
			header = toks
		} else if toks[0] == "Average:" && toks[1] == iface {
			data = toks
		}
	}
	if header == nil || data == nil {
		return 0.0
	}
	r := makeRow(header, 2, data, 2)
	return r.getOr("rxpck/s", 0.0) + r.getOr("txpck/s", 0.0)
}

// ---------------------------------------------------------------- saturate --

func satCPU(seconds, workers int) (uint64, uint64) {
	deadline := time.Now().Add(time.Duration(seconds) * time.Second)
	var totalIters, checksum uint64
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		go func(w int) {
			defer wg.Done()
			x := uint64(0x9E3779B97F4A7C15) ^ uint64(w+1)
			var iters uint64
			for {
				x ^= x << 7
				x ^= x >> 9
				iters++
				if iters&0xFFFFF == 0 && !time.Now().Before(deadline) {
					break
				}
			}
			atomic.AddUint64(&totalIters, iters)
			atomic.AddUint64(&checksum, x) // keeps x provably live (no DCE)
		}(w)
	}
	wg.Wait() // each goroutine only returns once its own deadline check fires
	return totalIters, checksum
}

func satMem(seconds, workers int) (uint64, uint64) {
	var memKB uint64
	if data, err := os.ReadFile("/proc/meminfo"); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "MemTotal:") {
				fields := splitWs(strings.TrimPrefix(line, "MemTotal:"))
				if len(fields) > 0 {
					if v, err := strconv.ParseUint(fields[0], 10, 64); err == nil {
						memKB = v
					}
				}
				break
			}
		}
	}
	var targetBytes uint64
	if memKB > 0 {
		targetBytes = uint64(float64(memKB) * 1024.0 * 1.35)
	} else {
		targetBytes = 2 << 30 // 2 GiB fallback if /proc/meminfo is unreadable
	}
	chunk := targetBytes / uint64(workers)
	if chunk < 4096*16 {
		chunk = 4096 * 16
	}

	deadline := time.Now().Add(time.Duration(seconds) * time.Second)
	var totalTouches uint64
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		go func() {
			defer wg.Done()
			buf := make([]byte, chunk)
			var touches uint64
			for time.Now().Before(deadline) {
				for off := 0; off < len(buf); off += 4096 {
					buf[off]++
					touches++
				}
			}
			atomic.AddUint64(&totalTouches, touches)
		}()
	}
	wg.Wait()
	return totalTouches, targetBytes
}

func satIO(seconds, workers int) (uint64, uint64) {
	dir := fmt.Sprintf("/var/tmp/loadmix-io-%d", os.Getpid())
	_ = os.Mkdir(dir, 0700)

	deadline := time.Now().Add(time.Duration(seconds) * time.Second)
	var totalWrites, totalBytes uint64
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		go func(w int) {
			defer wg.Done()
			path := fmt.Sprintf("%s/w%d.dat", dir, w)
			fd, err := unix.Open(path, unix.O_WRONLY|unix.O_CREAT|unix.O_TRUNC|unix.O_DSYNC, 0600)
			if err != nil {
				return
			}
			const kBuf = 16384
			buf := make([]byte, kBuf)
			// xorshift64*: cheap per-worker PRNG so the payload isn't
			// trivially compressible (this guest's /var can be btrfs with
			// zstd compression, which would otherwise let a real disk
			// write shrink to nearly nothing and mute the iowait signal).
			seed := uint64(0x2545F4914F6CDD1D) ^ uint64(w+1)
			var writes, bytesWritten uint64
			for time.Now().Before(deadline) {
				for i := 0; i < kBuf; i += 8 {
					seed ^= seed << 13
					seed ^= seed >> 7
					seed ^= seed << 17
					binary.LittleEndian.PutUint64(buf[i:i+8], seed)
				}
				n, err := unix.Pwrite(fd, buf, 0) // same 16 KiB region: bounded file size
				if err == nil && n > 0 {
					writes++
					bytesWritten += uint64(n)
				}
			}
			_ = unix.Close(fd)
			_ = os.Remove(path)
			atomic.AddUint64(&totalWrites, writes)
			atomic.AddUint64(&totalBytes, bytesWritten)
		}(w)
	}
	wg.Wait()
	_ = os.Remove(dir)
	return totalWrites, totalBytes
}

func satNet(seconds, workers int) (uint64, uint64) {
	rconn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		return 0, 0
	}
	defer rconn.Close()
	port := rconn.LocalAddr().(*net.UDPAddr).Port

	var stop atomic.Bool
	var recvWg sync.WaitGroup
	recvWg.Add(1)
	go func() {
		defer recvWg.Done()
		buf := make([]byte, 256)
		for !stop.Load() {
			_ = rconn.SetReadDeadline(time.Now().Add(200 * time.Millisecond)) // 200ms so the receiver notices stop promptly
			_, _ = rconn.Read(buf)
		}
	}()

	deadline := time.Now().Add(time.Duration(seconds) * time.Second)
	var totalPkts, totalBytes uint64
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		go func() {
			defer wg.Done()
			sconn, err := net.DialUDP("udp4", nil, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port})
			if err != nil {
				return
			}
			defer sconn.Close()
			payload := make([]byte, 64)
			var pkts, bytesSent uint64
			for time.Now().Before(deadline) {
				n, err := sconn.Write(payload)
				if err == nil && n > 0 {
					pkts++
					bytesSent += uint64(n)
				}
			}
			atomic.AddUint64(&totalPkts, pkts)
			atomic.AddUint64(&totalBytes, bytesSent)
		}()
	}
	wg.Wait() // sender pool done
	stop.Store(true)
	recvWg.Wait() // receiver notices `stop` within ~200ms
	return totalPkts, totalBytes
}

func cmdSaturate(resource string, seconds int) int {
	cpus := nproc()
	var workers int
	switch resource {
	case "mem":
		workers = maxInt(2, cpus)
	case "cpu":
		workers = maxInt(4, cpus*3)
	default: // io, net
		workers = maxInt(4, cpus*4)
	}

	fmt.Printf("loadmix: start resource=%s seconds=%d workers=%d\n", resource, seconds, workers)
	var ops, bytesN uint64
	switch resource {
	case "cpu":
		ops, bytesN = satCPU(seconds, workers)
	case "mem":
		ops, bytesN = satMem(seconds, workers)
	case "io":
		ops, bytesN = satIO(seconds, workers)
	default:
		ops, bytesN = satNet(seconds, workers)
	}
	fmt.Printf("loadmix: done resource=%s seconds=%d workers=%d ops=%d bytes=%d\n",
		resource, seconds, workers, ops, bytesN)
	return 0
}

// ------------------------------------------------------------------ analyze --

func boolStr(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

// runSamplers runs the five interval samplers CONCURRENTLY (each redirected
// to its own temp file), waits for all of them, then reads and deletes each
// file, returning the captured text keyed by tool name.
func runSamplers(seconds int, dir string) map[string]string {
	n := strconv.Itoa(seconds)
	type sampler struct {
		name string
		argv []string
		file string
	}
	samplers := []sampler{
		{"vmstat", []string{"vmstat", "1", n}, dir + "/vmstat.out"},
		{"mpstat", []string{"mpstat", "-P", "ALL", "1", n}, dir + "/mpstat.out"},
		{"iostat", []string{"iostat", "-xz", "1", n}, dir + "/iostat.out"},
		{"sar", []string{"sar", "-n", "DEV", "1", n}, dir + "/sar.out"},
		{"pidstat", []string{"pidstat", "1", n}, dir + "/pidstat.out"},
	}
	var wg sync.WaitGroup
	wg.Add(len(samplers))
	for _, s := range samplers {
		go func(s sampler) {
			defer wg.Done()
			runToFile(s.argv, s.file)
		}(s)
	}
	wg.Wait()

	out := make(map[string]string, len(samplers))
	for _, s := range samplers {
		out[s.name] = readAll(s.file)
		_ = os.Remove(s.file)
	}
	return out
}

func cmdAnalyze(seconds int) int {
	cpus := nproc()
	fmt.Printf("analyze: start seconds=%d cpus=%d\n", seconds, cpus)

	up := runCapture("uptime")
	uptimeRaw := "unavailable"
	if up.err == nil {
		uptimeRaw = trim(up.out)
	}
	fmt.Printf("analyze: tool uptime raw=\"%s\"\n", uptimeRaw)

	dm := runCapture("dmesg | tail -n 5")
	dmStatus := "ok"
	if dm.err != nil || strings.Contains(dm.out, "Operation not permitted") {
		dmStatus = "denied"
	}
	fmt.Printf("analyze: tool dmesg status=%s\n", dmStatus)

	topOut := runCapture("top -b -n1")
	var cpuLine, tasksLine string
	if topOut.err == nil {
		for _, l := range splitLines(topOut.out) {
			if strings.HasPrefix(l, "%Cpu") {
				cpuLine = trim(l)
			}
			if strings.HasPrefix(l, "Tasks:") {
				tasksLine = trim(l)
			}
		}
	}
	fmt.Printf("analyze: tool top cpu=\"%s\" tasks=\"%s\"\n", cpuLine, tasksLine)

	ssOut := runCapture("ss -s")
	var ssLine string
	if ssOut.err == nil {
		lines := splitLines(ssOut.out)
		if len(lines) > 0 {
			ssLine = trim(lines[0])
		}
	}
	fmt.Printf("analyze: tool ss raw=\"%s\"\n", ssLine)

	// The five interval samplers run CONCURRENTLY so an N-second checklist
	// costs ~N seconds, not 5N.
	dir := fmt.Sprintf("/tmp/loadmix-analyze-%d", os.Getpid())
	_ = os.Mkdir(dir, 0700)
	samplerOut := runSamplers(seconds, dir)
	_ = os.Remove(dir)

	active := 0
	for _, l := range splitLines(samplerOut["pidstat"]) {
		t := splitWs(l)
		if len(t) >= 2 && t[0] == "Average:" && t[1] != "UID" {
			active++
		}
	}
	fmt.Printf("analyze: tool pidstat active_processes=%d\n", active)

	freeOut := runCapture("free -m")
	memUsedPct := 0.0
	if freeOut.err == nil {
		memUsedPct = parseFreeUsedPct(freeOut.out)
	}

	vm := parseVmstat(samplerOut["vmstat"])
	mp := parseMpstat(samplerOut["mpstat"])
	io := parseIostat(samplerOut["iostat"])
	netPkts := parseSarPkts(samplerOut["sar"], "lo")

	fmt.Printf("analyze: metric resource=cpu name=busy_pct value=%.2f unit=pct\n", mp.busyPct)
	fmt.Printf("analyze: metric resource=cpu name=run_queue value=%.2f unit=procs\n", vm.runQueueMax)
	fmt.Printf("analyze: metric resource=mem name=used_pct value=%.2f unit=pct\n", memUsedPct)
	fmt.Printf("analyze: metric resource=mem name=swap_io value=%.2f unit=kbps\n", vm.swapIOMax)
	fmt.Printf("analyze: metric resource=io name=util_pct value=%.2f unit=pct\n", io.utilMax)
	fmt.Printf("analyze: metric resource=io name=iowait_pct value=%.2f unit=pct\n", mp.iowaitPct)
	fmt.Printf("analyze: metric resource=io name=await_ms value=%.2f unit=ms\n", io.awaitMax)
	fmt.Printf("analyze: metric resource=net name=pkts value=%.2f unit=per_s\n", netPkts)

	// signal thresholds: Utilization/Saturation pair per resource. cpu's
	// saturation threshold scales with this guest's own core count — a run
	// queue longer than (cpus + headroom) means threads are waiting for a
	// CPU, the textbook USE definition of saturation.
	type candidate struct {
		resource   string
		uVal, uThr float64
		sVal, sThr float64
	}
	table := []candidate{
		{"cpu", mp.busyPct, 60.0, vm.runQueueMax, float64(cpus) + 2.0},
		{"mem", memUsedPct, 75.0, vm.swapIOMax, 300.0},
		{"io", io.utilMax, 40.0, mp.iowaitPct, 8.0},
		{"net", netPkts, 2000.0, netPkts, 8000.0},
	}

	verdict := ""
	bestRatio := -1.0
	for _, c := range table {
		ufired := c.uVal >= c.uThr
		sfired := c.sVal >= c.sThr
		fmt.Printf("analyze: signal resource=%s type=Utilization fired=%s value=%.2f threshold=%.2f\n",
			c.resource, boolStr(ufired), c.uVal, c.uThr)
		fmt.Printf("analyze: signal resource=%s type=Saturation fired=%s value=%.2f threshold=%.2f\n",
			c.resource, boolStr(sfired), c.sVal, c.sThr)
		fmt.Printf("analyze: signal resource=%s type=Errors fired=false\n", c.resource)
		ratio := c.sVal / c.sThr
		if ratio > bestRatio {
			bestRatio = ratio
			verdict = c.resource
		}
	}
	fmt.Printf("analyze: verdict resource=%s ratio=%.2f\n", verdict, bestRatio)
	fmt.Println("analyze: done")
	return 0
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app --resource cpu|mem|io|net --seconds N")
	fmt.Fprintln(os.Stderr, "       app analyze [--seconds N]")
}

func main() {
	args := os.Args[1:]

	if len(args) > 0 && args[0] == "analyze" {
		seconds := 60
		for i := 1; i < len(args); {
			if args[i] == "--seconds" && i+1 < len(args) {
				v, err := strconv.Atoi(args[i+1])
				if err != nil {
					usage()
					os.Exit(2)
				}
				seconds = v
				i += 2
			} else {
				usage()
				os.Exit(2)
			}
		}
		if seconds <= 0 {
			usage()
			os.Exit(2)
		}
		os.Exit(cmdAnalyze(seconds))
	}

	resource := ""
	seconds := -1
	for i := 0; i < len(args); {
		switch {
		case args[i] == "--resource" && i+1 < len(args):
			resource = args[i+1]
			i += 2
		case args[i] == "--seconds" && i+1 < len(args):
			v, err := strconv.Atoi(args[i+1])
			if err != nil {
				usage()
				os.Exit(2)
			}
			seconds = v
			i += 2
		default:
			usage()
			os.Exit(2)
		}
	}
	if resource != "cpu" && resource != "mem" && resource != "io" && resource != "net" {
		usage()
		os.Exit(2)
	}
	if seconds <= 0 {
		usage()
		os.Exit(2)
	}
	os.Exit(cmdSaturate(resource, seconds))
}
