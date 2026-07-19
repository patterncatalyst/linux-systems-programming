// memmap: self-inspecting virtual-memory tool — perform one kind of
// allocation, touch its pages, then report what the kernel now shows for
// this very process: the /proc/self/maps region backing the allocation,
// the VmRSS growth, and the getrusage(2) page-fault deltas.
//
//	memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|fault-walk [--mb N]
//
// Output contract (identical across C++, Go, Rust):
//
//	memmap: mode=<m> bytes=<b> pages=<p>
//	[fault-walk only] memmap: walk file=<path> steps=8
//	[fault-walk only] memmap: step=<i>/8 pages=<p> minor=<d> major=<d>
//	memmap: maps excerpt
//	memmap:   <raw /proc/self/maps line>   <-- target (mode=<m>)
//	memmap: vmrss_before=<kb>KB vmrss_after=<kb>KB
//	memmap: faults minor=<n> major=<n>
//
// Go note: goroutine stacks are runtime-managed memory, not the kernel's
// [stack] mapping, so the stack-mode excerpt shows an anonymous region.
package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	mib        = 1 << 20
	stackCapMb = 4 // stays well inside typical stack expectations
	walkSteps  = 8
)

// sink defeats dead-store elimination of the touch loops.
var sink byte

type options struct {
	mode string
	mb   int
	file string
}

type baseline struct {
	rssKB int64
	minor int64
	major int64
}

// ---------------------------------------------------------------------------
// /proc and getrusage probes
// ---------------------------------------------------------------------------

func vmrssKB() (int64, error) {
	f, err := os.Open("/proc/self/status")
	if err != nil {
		return 0, fmt.Errorf("open /proc/self/status: %w", err)
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "VmRSS:") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return 0, fmt.Errorf("cannot parse VmRSS: %q", line)
		}
		kb, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil {
			return 0, fmt.Errorf("cannot parse VmRSS: %w", err)
		}
		return kb, nil
	}
	if err := sc.Err(); err != nil {
		return 0, fmt.Errorf("read /proc/self/status: %w", err)
	}
	return 0, fmt.Errorf("VmRSS not found in /proc/self/status")
}

func faultCounts() (minor, major int64, err error) {
	var ru unix.Rusage
	if err := unix.Getrusage(unix.RUSAGE_SELF, &ru); err != nil {
		return 0, 0, fmt.Errorf("getrusage: %w", err)
	}
	return ru.Minflt, ru.Majflt, nil
}

func takeBaseline() (baseline, error) {
	rss, err := vmrssKB()
	if err != nil {
		return baseline{}, err
	}
	minor, major, err := faultCounts()
	if err != nil {
		return baseline{}, err
	}
	return baseline{rssKB: rss, minor: minor, major: major}, nil
}

// printMapsExcerpt prints every /proc/self/maps line overlapping
// [addr, addr+length).
func printMapsExcerpt(addr, length uintptr, mode string) error {
	f, err := os.Open("/proc/self/maps")
	if err != nil {
		return fmt.Errorf("open /proc/self/maps: %w", err)
	}
	defer f.Close()
	fmt.Println("memmap: maps excerpt")
	lo, hi := uint64(addr), uint64(addr+length)
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64*1024), 64*1024)
	for sc.Scan() {
		line := sc.Text()
		rangePart, _, ok := strings.Cut(line, " ")
		if !ok {
			continue
		}
		startStr, endStr, ok := strings.Cut(rangePart, "-")
		if !ok {
			continue
		}
		start, err1 := strconv.ParseUint(startStr, 16, 64)
		end, err2 := strconv.ParseUint(endStr, 16, 64)
		if err1 != nil || err2 != nil {
			continue
		}
		if start < hi && end > lo {
			fmt.Printf("memmap:   %s   <-- target (mode=%s)\n", line, mode)
		}
	}
	return sc.Err()
}

// report prints the common tail: maps excerpt, RSS growth, fault deltas.
func report(addr, length uintptr, mode string, base baseline) error {
	if err := printMapsExcerpt(addr, length, mode); err != nil {
		return err
	}
	rssAfter, err := vmrssKB()
	if err != nil {
		return err
	}
	minor, major, err := faultCounts()
	if err != nil {
		return err
	}
	fmt.Printf("memmap: vmrss_before=%dKB vmrss_after=%dKB\n", base.rssKB, rssAfter)
	fmt.Printf("memmap: faults minor=%d major=%d\n", minor-base.minor, major-base.major)
	return nil
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

func touchWritable(b []byte, page int) {
	for i := 0; i < len(b); i += page {
		b[i] = 1
	}
	if len(b) > 0 {
		sink = b[0]
	}
}

func touchReadable(b []byte, page int) {
	var sum byte
	for i := 0; i < len(b); i += page {
		sum += b[i]
	}
	sink = sum
}

// runStack touches a large local buffer on a dedicated goroutine so the
// growth happens on a fresh stack; the result comes back over a channel.
// The baseline is captured before the goroutine starts so the runtime work
// of growing (or relocating) that stack is part of the measured window.
func runStack(bytes, page int) error {
	base, err := takeBaseline()
	if err != nil {
		return err
	}
	errc := make(chan error, 1)
	go func() {
		errc <- stackWorker(bytes, page, base)
	}()
	return <-errc
}

//go:noinline
func stackWorker(n, page int, base baseline) error {
	var buf [stackCapMb * mib]byte
	touchWritable(buf[:n], page)
	return report(uintptr(unsafe.Pointer(&buf[0])), uintptr(n), "stack", base)
}

func runHeap(n, page int) error {
	base, err := takeBaseline()
	if err != nil {
		return err
	}
	buf := make([]byte, n)
	touchWritable(buf, page)
	return report(uintptr(unsafe.Pointer(&buf[0])), uintptr(n), "heap", base)
}

func runMmapAnon(n, page int) error {
	base, err := takeBaseline()
	if err != nil {
		return err
	}
	m, err := unix.Mmap(-1, 0, n, unix.PROT_READ|unix.PROT_WRITE,
		unix.MAP_PRIVATE|unix.MAP_ANONYMOUS)
	if err != nil {
		return fmt.Errorf("mmap: %w", err)
	}
	defer unix.Munmap(m)
	touchWritable(m, page)
	return report(uintptr(unsafe.Pointer(&m[0])), uintptr(n), "mmap-anon", base)
}

func openForMap(path string) (*os.File, int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, 0, err
	}
	st, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, 0, fmt.Errorf("%s: stat: %w", path, err)
	}
	if st.Size() <= 0 {
		f.Close()
		return nil, 0, fmt.Errorf("%s: file is empty", path)
	}
	return f, st.Size(), nil
}

func runMmapFile(path string, page int) error {
	f, size, err := openForMap(path)
	if err != nil {
		return err
	}
	defer f.Close()
	pages := (size + int64(page) - 1) / int64(page)
	fmt.Printf("memmap: mode=mmap-file bytes=%d pages=%d\n", size, pages)
	base, err := takeBaseline()
	if err != nil {
		return err
	}
	m, err := unix.Mmap(int(f.Fd()), 0, int(size), unix.PROT_READ, unix.MAP_PRIVATE)
	if err != nil {
		return fmt.Errorf("mmap: %w", err)
	}
	defer unix.Munmap(m)
	touchReadable(m, page)
	return report(uintptr(unsafe.Pointer(&m[0])), uintptr(size), "mmap-file", base)
}

func writeWalkFile(n int) (string, error) {
	path := filepath.Join(os.TempDir(),
		fmt.Sprintf("memmap-walk-%d.bin", os.Getpid()))
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return "", err
	}
	defer f.Close()
	chunk := make([]byte, mib)
	for i := range chunk {
		chunk[i] = 0xA5
	}
	for left := n; left > 0; left -= len(chunk) {
		c := chunk
		if left < len(c) {
			c = c[:left]
		}
		if _, err := f.Write(c); err != nil {
			return "", fmt.Errorf("%s: write: %w", path, err)
		}
	}
	return path, nil
}

func runFaultWalk(n, page int) error {
	path, err := writeWalkFile(n)
	if err != nil {
		return err
	}
	defer os.Remove(path)
	f, size, err := openForMap(path)
	if err != nil {
		return err
	}
	defer f.Close()
	base, err := takeBaseline()
	if err != nil {
		return err
	}
	m, err := unix.Mmap(int(f.Fd()), 0, int(size), unix.PROT_READ, unix.MAP_PRIVATE)
	if err != nil {
		return fmt.Errorf("mmap: %w", err)
	}
	defer unix.Munmap(m)

	fmt.Printf("memmap: walk file=%s steps=%d\n", path, walkSteps)
	pages := int(size) / page
	prevMinor, prevMajor := base.minor, base.major
	done := 0
	for step := 1; step <= walkSteps; step++ {
		quota := pages / walkSteps
		if step == walkSteps {
			quota = pages - done
		}
		touchReadable(m[done*page:(done+quota)*page], page)
		minor, major, err := faultCounts()
		if err != nil {
			return err
		}
		fmt.Printf("memmap: step=%d/%d pages=%d minor=%d major=%d\n",
			step, walkSteps, quota, minor-prevMinor, major-prevMajor)
		prevMinor, prevMajor = minor, major
		done += quota
	}
	return report(uintptr(unsafe.Pointer(&m[0])), uintptr(size), "fault-walk", base)
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() {
	fmt.Fprintln(os.Stderr,
		"usage: memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|fault-walk [--mb N]")
}

func parseArgs(args []string) (options, error) {
	opts := options{mb: 64}
	modeSet := false
	for i := 0; i < len(args); i++ {
		switch a := args[i]; {
		case a == "--mode":
			i++
			if i >= len(args) {
				return opts, fmt.Errorf("--mode needs a value")
			}
			switch args[i] {
			case "stack", "heap", "mmap-anon", "mmap-file", "fault-walk":
				opts.mode = args[i]
				modeSet = true
			default:
				return opts, fmt.Errorf("unknown mode: %s", args[i])
			}
		case a == "--mb":
			i++
			if i >= len(args) {
				return opts, fmt.Errorf("--mb needs a value")
			}
			mb, err := strconv.Atoi(args[i])
			if err != nil || mb < 1 || mb > 1024 {
				return opts, fmt.Errorf("--mb must be 1..1024")
			}
			opts.mb = mb
		case !strings.HasPrefix(a, "--") && opts.file == "":
			opts.file = a
		default:
			return opts, fmt.Errorf("unexpected argument: %s", a)
		}
	}
	if !modeSet {
		return opts, fmt.Errorf("--mode is required")
	}
	if opts.mode == "mmap-file" && opts.file == "" {
		return opts, fmt.Errorf("mmap-file needs a FILE argument")
	}
	if opts.mode != "mmap-file" && opts.file != "" {
		return opts, fmt.Errorf("only mmap-file takes a FILE argument")
	}
	return opts, nil
}

func main() {
	opts, err := parseArgs(os.Args[1:])
	if err != nil {
		usage()
		os.Exit(2)
	}
	page := unix.Getpagesize()

	switch opts.mode {
	case "stack":
		n := min(opts.mb, stackCapMb) * mib
		fmt.Printf("memmap: mode=stack bytes=%d pages=%d\n", n, n/page)
		err = runStack(n, page)
	case "heap":
		n := opts.mb * mib
		fmt.Printf("memmap: mode=heap bytes=%d pages=%d\n", n, n/page)
		err = runHeap(n, page)
	case "mmap-anon":
		n := opts.mb * mib
		fmt.Printf("memmap: mode=mmap-anon bytes=%d pages=%d\n", n, n/page)
		err = runMmapAnon(n, page)
	case "mmap-file":
		err = runMmapFile(opts.file, page) // prints its own header line
	case "fault-walk":
		n := opts.mb * mib
		fmt.Printf("memmap: mode=fault-walk bytes=%d pages=%d\n", n, n/page)
		err = runFaultWalk(n, page)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "memmap: error: %v\n", err)
		os.Exit(1)
	}
}
