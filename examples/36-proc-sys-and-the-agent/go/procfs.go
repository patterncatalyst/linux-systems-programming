// sysagent: /proc + cgroup PSI readers and the snapshot they compose into.
package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// DiskStat is one line of /proc/diskstats reduced to the USE-method fields.
type DiskStat struct {
	Name         string
	Reads        int64
	Writes       int64
	ReadSectors  int64
	WriteSectors int64
}

// NetStat is one interface's counters from /proc/net/dev.
type NetStat struct {
	Iface   string
	RxBytes int64
	TxBytes int64
}

// Snapshot is one fully-formed sample. Field names here are the canonical,
// deterministic names shared with the C++ and Rust implementations (see
// README.md) — only the encoding (Go struct vs C++ struct vs Rust struct)
// differs.
type Snapshot struct {
	CPUUtilPct   float64
	CPUUserPct   float64
	CPUSystemPct float64

	Load1        float64
	Load5        float64
	Load15       float64
	Runnable     int64
	TotalThreads int64

	MemTotalKB     int64
	MemAvailableKB int64
	MemUsedKB      int64

	Disks []DiskStat
	Net   []NetStat

	PSIAvailable    bool
	PSICPUSomeAvg10 float64
	PSIMemSomeAvg10 float64
	PSIIOSomeAvg10  float64
}

type cpuTicks struct {
	user, nice, system, idle, iowait, irq, softirq, steal int64
}

// readCPUTicks parses the aggregate "cpu " line of /proc/stat.
func readCPUTicks() (cpuTicks, error) {
	f, err := os.Open("/proc/stat")
	if err != nil {
		return cpuTicks{}, fmt.Errorf("open /proc/stat: %w", err)
	}
	defer f.Close()

	sc := bufio.NewScanner(f)
	if !sc.Scan() {
		return cpuTicks{}, fmt.Errorf("read /proc/stat: %w", sc.Err())
	}
	fields := strings.Fields(sc.Text())
	if len(fields) < 9 || fields[0] != "cpu" {
		return cpuTicks{}, fmt.Errorf("/proc/stat: unexpected first line %q", sc.Text())
	}
	vals := make([]int64, 8)
	for i := range vals {
		v, err := strconv.ParseInt(fields[i+1], 10, 64)
		if err != nil {
			return cpuTicks{}, fmt.Errorf("/proc/stat: parse field %d: %w", i, err)
		}
		vals[i] = v
	}
	return cpuTicks{
		user: vals[0], nice: vals[1], system: vals[2], idle: vals[3],
		iowait: vals[4], irq: vals[5], softirq: vals[6], steal: vals[7],
	}, nil
}

type loadAvg struct {
	load1, load5, load15   float64
	runnable, totalThreads int64
}

func readLoadAvg() (loadAvg, error) {
	data, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return loadAvg{}, fmt.Errorf("read /proc/loadavg: %w", err)
	}
	fields := strings.Fields(string(data))
	if len(fields) < 4 {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: unexpected shape %q", string(data))
	}
	var la loadAvg
	if la.load1, err = strconv.ParseFloat(fields[0], 64); err != nil {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: load1: %w", err)
	}
	if la.load5, err = strconv.ParseFloat(fields[1], 64); err != nil {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: load5: %w", err)
	}
	if la.load15, err = strconv.ParseFloat(fields[2], 64); err != nil {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: load15: %w", err)
	}
	run, total, ok := strings.Cut(fields[3], "/")
	if !ok {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: bad running/total field %q", fields[3])
	}
	if la.runnable, err = strconv.ParseInt(run, 10, 64); err != nil {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: runnable: %w", err)
	}
	if la.totalThreads, err = strconv.ParseInt(total, 10, 64); err != nil {
		return loadAvg{}, fmt.Errorf("/proc/loadavg: total: %w", err)
	}
	return la, nil
}

type memInfo struct {
	totalKB, availableKB int64
}

func readMemInfo() (memInfo, error) {
	f, err := os.Open("/proc/meminfo")
	if err != nil {
		return memInfo{}, fmt.Errorf("open /proc/meminfo: %w", err)
	}
	defer f.Close()

	var mi memInfo
	haveTotal, haveAvail := false, false
	sc := bufio.NewScanner(f)
	for sc.Scan() && !(haveTotal && haveAvail) {
		fields := strings.Fields(sc.Text())
		if len(fields) < 2 {
			continue
		}
		v, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil {
			continue
		}
		switch fields[0] {
		case "MemTotal:":
			mi.totalKB = v
			haveTotal = true
		case "MemAvailable:":
			mi.availableKB = v
			haveAvail = true
		}
	}
	if !haveTotal || !haveAvail {
		return memInfo{}, fmt.Errorf("/proc/meminfo: missing MemTotal/MemAvailable")
	}
	return mi, nil
}

func readDiskStats() ([]DiskStat, error) {
	f, err := os.Open("/proc/diskstats")
	if err != nil {
		return nil, fmt.Errorf("open /proc/diskstats: %w", err)
	}
	defer f.Close()

	var out []DiskStat
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		// major minor name reads_completed reads_merged sectors_read
		// ms_reading writes_completed writes_merged sectors_written ...
		if len(fields) < 10 {
			continue
		}
		name := fields[2]
		if strings.HasPrefix(name, "loop") || strings.HasPrefix(name, "ram") {
			continue
		}
		reads, _ := strconv.ParseInt(fields[3], 10, 64)
		readSectors, _ := strconv.ParseInt(fields[5], 10, 64)
		writes, _ := strconv.ParseInt(fields[7], 10, 64)
		writeSectors, _ := strconv.ParseInt(fields[9], 10, 64)
		out = append(out, DiskStat{
			Name: name, Reads: reads, Writes: writes,
			ReadSectors: readSectors, WriteSectors: writeSectors,
		})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("read /proc/diskstats: %w", err)
	}
	return out, nil
}

func readNetDev() ([]NetStat, error) {
	f, err := os.Open("/proc/net/dev")
	if err != nil {
		return nil, fmt.Errorf("open /proc/net/dev: %w", err)
	}
	defer f.Close()

	var out []NetStat
	sc := bufio.NewScanner(f)
	line := 0
	for sc.Scan() {
		line++
		if line <= 2 {
			continue // two header lines
		}
		iface, rest, ok := strings.Cut(sc.Text(), ":")
		if !ok {
			continue
		}
		fields := strings.Fields(rest)
		if len(fields) < 9 {
			continue
		}
		rx, _ := strconv.ParseInt(fields[0], 10, 64)
		tx, _ := strconv.ParseInt(fields[8], 10, 64)
		out = append(out, NetStat{Iface: strings.TrimSpace(iface), RxBytes: rx, TxBytes: tx})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("read /proc/net/dev: %w", err)
	}
	return out, nil
}

type psi struct {
	cpuSomeAvg10, memSomeAvg10, ioSomeAvg10 float64
}

// parseAvg10 pulls "avg10=X.XX" out of a PSI line such as
// "some avg10=0.00 avg60=0.00 avg300=0.00 total=12345".
func parseAvg10(line string) float64 {
	for _, tok := range strings.Fields(line) {
		if v, ok := strings.CutPrefix(tok, "avg10="); ok {
			f, err := strconv.ParseFloat(v, 64)
			if err == nil {
				return f
			}
		}
	}
	return 0
}

func readPSIFromDir(dir string) (psi, bool) {
	var p psi
	any := false
	for _, ent := range []struct {
		file string
		out  *float64
	}{
		{"cpu.pressure", &p.cpuSomeAvg10},
		{"memory.pressure", &p.memSomeAvg10},
		{"io.pressure", &p.ioSomeAvg10},
	} {
		data, err := os.ReadFile(dir + "/" + ent.file)
		if err != nil {
			continue
		}
		line, _, _ := strings.Cut(string(data), "\n")
		*ent.out = parseAvg10(line)
		any = true
	}
	return p, any
}

// readPSI prefers this process's own cgroup (v2 unified hierarchy) and falls
// back to the system-wide /proc/pressure/* files. Returns ok=false if PSI is
// not exposed anywhere on this kernel/host.
func readPSI() (psi, bool) {
	if data, err := os.ReadFile("/proc/self/cgroup"); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			rel, ok := strings.CutPrefix(line, "0::")
			if !ok {
				continue
			}
			if p, ok := readPSIFromDir("/sys/fs/cgroup" + rel); ok {
				return p, true
			}
			break
		}
	}
	return readPSIFromDir("/proc/pressure")
}

// namedErr carries a source label so TakeSnapshot can report which /proc
// source failed, fanned in over a channel from the goroutines below.
type namedErr struct {
	source string
	err    error
}

// TakeSnapshot takes two /proc/stat readings intervalMs apart and fills in
// every other field concurrently in between, via goroutines fanned in over a
// channel — the Go analogue of the C++ jthread pool and the Rust scoped
// threads.
func TakeSnapshot(intervalMs int) (Snapshot, error) {
	t0, err := readCPUTicks()
	if err != nil {
		return Snapshot{}, err
	}

	var snap Snapshot
	results := make(chan namedErr, 5)

	go func() {
		la, err := readLoadAvg()
		if err == nil {
			snap.Load1, snap.Load5, snap.Load15 = la.load1, la.load5, la.load15
			snap.Runnable, snap.TotalThreads = la.runnable, la.totalThreads
		}
		results <- namedErr{"loadavg", err}
	}()
	go func() {
		mi, err := readMemInfo()
		if err == nil {
			snap.MemTotalKB, snap.MemAvailableKB = mi.totalKB, mi.availableKB
			snap.MemUsedKB = mi.totalKB - mi.availableKB
		}
		results <- namedErr{"meminfo", err}
	}()
	go func() {
		disks, err := readDiskStats()
		if err == nil {
			snap.Disks = disks
		}
		results <- namedErr{"diskstats", err}
	}()
	go func() {
		net, err := readNetDev()
		if err == nil {
			snap.Net = net
		}
		results <- namedErr{"netdev", err}
	}()
	go func() {
		if p, ok := readPSI(); ok {
			snap.PSIAvailable = true
			snap.PSICPUSomeAvg10, snap.PSIMemSomeAvg10, snap.PSIIOSomeAvg10 =
				p.cpuSomeAvg10, p.memSomeAvg10, p.ioSomeAvg10
		}
		results <- namedErr{"psi", nil}
	}()

	time.Sleep(time.Duration(intervalMs) * time.Millisecond)

	var firstErr error
	required := 0
	for i := 0; i < 5; i++ {
		r := <-results
		if r.source == "psi" {
			continue
		}
		required++
		if r.err != nil && firstErr == nil {
			firstErr = fmt.Errorf("%s: %w", r.source, r.err)
		}
	}
	if firstErr != nil {
		return Snapshot{}, firstErr
	}
	_ = required

	t1, err := readCPUTicks()
	if err != nil {
		return Snapshot{}, err
	}

	busy0 := t0.user + t0.nice + t0.system
	busy1 := t1.user + t1.nice + t1.system
	idle0 := t0.idle + t0.iowait
	idle1 := t1.idle + t1.iowait
	total0 := busy0 + idle0 + t0.irq + t0.softirq + t0.steal
	total1 := busy1 + idle1 + t1.irq + t1.softirq + t1.steal

	dTotal := float64(total1 - total0)
	dBusy := float64((busy1 - busy0) + (t1.irq - t0.irq) + (t1.softirq - t0.softirq) + (t1.steal - t0.steal))
	dUser := float64(t1.user - t0.user)
	dSystem := float64(t1.system - t0.system)

	if dTotal > 0 {
		snap.CPUUtilPct = 100 * dBusy / dTotal
		snap.CPUUserPct = 100 * dUser / dTotal
		snap.CPUSystemPct = 100 * dSystem / dTotal
	}

	return snap, nil
}

func f2(v float64) string { return strconv.FormatFloat(v, 'f', 2, 64) }

// ToJSON renders the deterministic single-line JSON object (see README.md).
func ToJSON(s Snapshot) string {
	var b strings.Builder
	b.WriteByte('{')
	fmt.Fprintf(&b, `"cpu_util_pct":%s,`, f2(s.CPUUtilPct))
	fmt.Fprintf(&b, `"cpu_user_pct":%s,`, f2(s.CPUUserPct))
	fmt.Fprintf(&b, `"cpu_system_pct":%s,`, f2(s.CPUSystemPct))
	fmt.Fprintf(&b, `"load1":%s,`, f2(s.Load1))
	fmt.Fprintf(&b, `"load5":%s,`, f2(s.Load5))
	fmt.Fprintf(&b, `"load15":%s,`, f2(s.Load15))
	fmt.Fprintf(&b, `"runnable":%d,`, s.Runnable)
	fmt.Fprintf(&b, `"total_threads":%d,`, s.TotalThreads)
	fmt.Fprintf(&b, `"mem_total_kb":%d,`, s.MemTotalKB)
	fmt.Fprintf(&b, `"mem_available_kb":%d,`, s.MemAvailableKB)
	fmt.Fprintf(&b, `"mem_used_kb":%d,`, s.MemUsedKB)
	b.WriteString(`"disks":[`)
	for i, d := range s.Disks {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, `{"name":"%s","reads":%d,"writes":%d,"read_sectors":%d,"write_sectors":%d}`,
			d.Name, d.Reads, d.Writes, d.ReadSectors, d.WriteSectors)
	}
	b.WriteString(`],"net":[`)
	for i, n := range s.Net {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, `{"iface":"%s","rx_bytes":%d,"tx_bytes":%d}`, n.Iface, n.RxBytes, n.TxBytes)
	}
	b.WriteString(`],`)
	fmt.Fprintf(&b, `"psi_available":%t,`, s.PSIAvailable)
	fmt.Fprintf(&b, `"psi_cpu_some_avg10":%s,`, f2(s.PSICPUSomeAvg10))
	fmt.Fprintf(&b, `"psi_mem_some_avg10":%s,`, f2(s.PSIMemSomeAvg10))
	fmt.Fprintf(&b, `"psi_io_some_avg10":%s`, f2(s.PSIIOSomeAvg10))
	b.WriteByte('}')
	return b.String()
}

// ToText renders human-readable key=value lines with the same field names.
func ToText(s Snapshot) string {
	var b strings.Builder
	fmt.Fprintf(&b, "cpu_util_pct=%s cpu_user_pct=%s cpu_system_pct=%s\n",
		f2(s.CPUUtilPct), f2(s.CPUUserPct), f2(s.CPUSystemPct))
	fmt.Fprintf(&b, "load1=%s load5=%s load15=%s runnable=%d total_threads=%d\n",
		f2(s.Load1), f2(s.Load5), f2(s.Load15), s.Runnable, s.TotalThreads)
	fmt.Fprintf(&b, "mem_total_kb=%d mem_available_kb=%d mem_used_kb=%d\n",
		s.MemTotalKB, s.MemAvailableKB, s.MemUsedKB)
	for _, d := range s.Disks {
		fmt.Fprintf(&b, "disk name=%s reads=%d writes=%d read_sectors=%d write_sectors=%d\n",
			d.Name, d.Reads, d.Writes, d.ReadSectors, d.WriteSectors)
	}
	for _, n := range s.Net {
		fmt.Fprintf(&b, "net iface=%s rx_bytes=%d tx_bytes=%d\n", n.Iface, n.RxBytes, n.TxBytes)
	}
	fmt.Fprintf(&b, "psi_available=%t psi_cpu_some_avg10=%s psi_mem_some_avg10=%s psi_io_some_avg10=%s\n",
		s.PSIAvailable, f2(s.PSICPUSomeAvg10), f2(s.PSIMemSomeAvg10), f2(s.PSIIOSomeAvg10))
	return b.String()
}
