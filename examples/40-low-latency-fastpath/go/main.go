// chatterd-fastpath — a stripped, pinned, allocation-free derivative of the
// chatterd echo/pub-sub hot path (chapter 21) built purely to answer one
// question: what does a low-latency network hot path actually cost, and what
// does removing that cost look like at the syscall level?
//
// Three subcommands, one 64-byte wire frame; observable behavior is identical
// to the C++ and Rust ports (same usage text, same "app: ..." lines, same
// percentiles_ns format), because verify.lua asserts against all three.
//
//	app fastpath --port P --pin CPU [--busy-poll]
//	app naive --port P
//	app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]
//
// THE FASTPATH FRAME (fixed 64 bytes):
//
//	+-------+---------+------+-----------+-----------------+---------+
//	| magic | version | type | seq (u64) | send_ns (u64)   | pad     |
//	| 2B    | 1B      | 1B   | 8B BE     | 8B BE           | 44B     |
//	+-------+---------+------+-----------+-----------------+---------+
//
// WHERE THE GO PORT DIVERGES FROM THE C++ ONE, AND WHY:
//
// The C++ version installs a SIGINT handler with sa_flags = 0 — deliberately
// NOT SA_RESTART — so a blocking read(2) or poll(2) already in progress fails
// with EINTR and the loop notices the stop flag. That technique is not
// available here. The Go runtime installs its own signal handlers with
// SA_RESTART, and a "blocking" read on a net.Conn does not sit in a syscall
// at all: the netpoller parks the goroutine on an epoll registration. No
// amount of signal handling will make that read return EINTR.
//
// So Go reaches the same observable behavior by a different mechanism: a
// signal.Notify goroutine sets the stop flag and then CLOSES the listener and
// the live connection. Closing is what unblocks a parked netpoller read —
// the same shutdown(2)-driven wakeup that chapter 21's chatterd uses via
// stop_callback, and the Go-idiomatic answer to "interrupt a blocked I/O
// wait". The accept loop additionally carries a 200ms deadline, mirroring the
// C++ poll(2) timeout, so shutdown stays bounded even if a close races an
// accept already in flight.
//
// The second divergence is pinning. sched_setaffinity(2) acts on a THREAD,
// and a goroutine is not a thread — it migrates between Ms at the scheduler's
// discretion. Pinning here therefore needs runtime.LockOSThread to nail the
// hot loop to one M, plus GOMAXPROCS(1) so the runtime does not keep other
// worker threads on other CPUs. See pinToCPU for why the affinity call is
// made twice.
package main

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"os/signal"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

const (
	frameSize   = 64
	magic0      = 0x43 // 'C'
	magic1      = 0x46 // 'F'
	wireVersion = 0x01
	typeEcho    = 0x01

	// Mirrors the C++ poll(2) timeout on the accept wait.
	acceptPollInterval = 200 * time.Millisecond
)

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

func buildFrame(seq, sendNs uint64) []byte {
	f := make([]byte, frameSize)
	f[0] = magic0
	f[1] = magic1
	f[2] = wireVersion
	f[3] = typeEcho
	binary.BigEndian.PutUint64(f[4:12], seq)
	binary.BigEndian.PutUint64(f[12:20], sendNs)
	// bytes 20..63 stay zero
	return f
}

func frameHeaderOK(f []byte) bool {
	return f[0] == magic0 && f[1] == magic1 && f[2] == wireVersion && f[3] == typeEcho
}

func nowNs() uint64 {
	var ts unix.Timespec
	_ = unix.ClockGettime(unix.CLOCK_MONOTONIC, &ts)
	return uint64(ts.Sec)*1_000_000_000 + uint64(ts.Nsec)
}

// ---------------------------------------------------------------------------
// Shutdown plumbing
// ---------------------------------------------------------------------------

// stopper carries the stop flag plus the closers that unblock whatever I/O is
// currently parked. Closing is the Go substitute for the C++ port's EINTR.
type stopper struct {
	stop atomic.Bool

	mu   sync.Mutex
	ln   net.Listener
	conn net.Conn
	// rawFD is the busy-poll path's dup'd descriptor. It is not registered
	// with the netpoller, so it is shut down rather than closed out from
	// under a spinning read.
	rawFD int
}

func newStopper() *stopper { return &stopper{rawFD: -1} }

func (s *stopper) setListener(ln net.Listener) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.ln = ln
}

func (s *stopper) setConn(c net.Conn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.conn = c
}

func (s *stopper) setRawFD(fd int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.rawFD = fd
}

func (s *stopper) trigger() {
	s.stop.Store(true)
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.ln != nil {
		_ = s.ln.Close()
	}
	if s.conn != nil {
		_ = s.conn.Close()
	}
	if s.rawFD >= 0 {
		// SHUT_RDWR makes an in-flight recv(2) return 0 immediately; the fd
		// itself is closed by the loop that owns it.
		_ = unix.Shutdown(s.rawFD, unix.SHUT_RDWR)
	}
}

// installSignalHandlers wires SIGINT/SIGTERM to the stopper. Unlike the C++
// port there is no sa_flags choice to make: the Go runtime owns the signal
// disposition and hands us a channel, so unblocking is done by closing fds.
func installSignalHandlers(s *stopper) {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-ch
		s.trigger()
	}()
}

// ---------------------------------------------------------------------------
// Pinning
// ---------------------------------------------------------------------------

// onlineCPUs reports the number of CPUs online system-wide, matching the C++
// port's sysconf(_SC_NPROCESSORS_ONLN). runtime.NumCPU() is deliberately NOT
// used: it returns the size of this process's CPU affinity MASK, so under the
// bench driver's `taskset -c 1` it would report 1 and reject a pin to CPU 1 as
// "out of range" — the classic case of a count that lies about its container,
// hit here via taskset rather than a cgroup. Reading the kernel's own online
// list sidesteps the mask. Falls back to runtime.NumCPU() only if the sysfs
// node is unreadable.
func onlineCPUs() int {
	data, err := os.ReadFile("/sys/devices/system/cpu/online")
	if err != nil {
		return runtime.NumCPU()
	}
	count := 0
	for _, part := range strings.Split(strings.TrimSpace(string(data)), ",") {
		if part == "" {
			continue
		}
		if lo, hi, ok := strings.Cut(part, "-"); ok {
			a, err1 := strconv.Atoi(lo)
			b, err2 := strconv.Atoi(hi)
			if err1 != nil || err2 != nil || b < a {
				return runtime.NumCPU()
			}
			count += b - a + 1
		} else {
			if _, err := strconv.Atoi(part); err != nil {
				return runtime.NumCPU()
			}
			count++
		}
	}
	if count == 0 {
		return runtime.NumCPU()
	}
	return count
}

// pinToCPU restricts this process to a single CPU, and — critically — makes
// that restriction visible in /proc/<pid>/status, which is where verify.lua
// reads Cpus_allowed_list to prove the pinning happened at the kernel level
// rather than trusting the line we print.
//
// The affinity call is made twice on purpose. sched_setaffinity(0, ...) acts
// on the CALLING thread, which after LockOSThread is the one running the hot
// loop — that call is what makes the pinning real. But /proc/<pid>/status
// reports the mask of the THREAD GROUP LEADER (the thread whose tid == pid),
// and the main goroutine is not guaranteed to be running on it. Passing
// os.Getpid() explicitly targets the leader, so the observable proof matches
// the actual behavior. When both are the same thread the second call is a
// harmless no-op.
func pinToCPU(cpu int) error {
	nproc := onlineCPUs()
	if cpu < 0 || (nproc > 0 && cpu >= nproc) {
		last := 0
		if nproc > 0 {
			last = nproc - 1
		}
		return fmt.Errorf("cpu %d out of range (0..%d)", cpu, last)
	}

	// One P, one hot thread: without this the runtime is free to schedule the
	// loop's goroutine onto another M running on another CPU.
	runtime.GOMAXPROCS(1)
	runtime.LockOSThread()

	var set unix.CPUSet
	set.Zero()
	set.Set(cpu)
	if err := unix.SchedSetaffinity(0, &set); err != nil {
		return fmt.Errorf("sched_setaffinity(%d): %v", cpu, err)
	}
	if err := unix.SchedSetaffinity(os.Getpid(), &set); err != nil {
		return fmt.Errorf("sched_setaffinity(%d): %v", cpu, err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

func listenTCP(port uint16) (net.Listener, error) {
	lc := net.ListenConfig{
		Control: func(_, _ string, c syscall.RawConn) error {
			var serr error
			if err := c.Control(func(fd uintptr) {
				serr = unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)
			}); err != nil {
				return err
			}
			return serr
		},
	}
	ln, err := lc.Listen(context.Background(), "tcp4", fmt.Sprintf("0.0.0.0:%d", port))
	if err != nil {
		return nil, fmt.Errorf("bind 0.0.0.0:%d: %v", port, unwrapSyscallErr(err))
	}
	return ln, nil
}

func connectTCP(host string, port uint16) (net.Conn, error) {
	// Deliberately no DNS: the C++ port uses inet_pton(3), so a non-literal
	// host is an error here too rather than a lookup.
	ip := net.ParseIP(host)
	if ip == nil || ip.To4() == nil {
		return nil, fmt.Errorf("%s: not an IPv4 address", host)
	}
	conn, err := net.DialTCP("tcp4", nil, &net.TCPAddr{IP: ip.To4(), Port: int(port)})
	if err != nil {
		return nil, fmt.Errorf("connect %s:%d: %v", host, port, unwrapSyscallErr(err))
	}
	return conn, nil
}

// unwrapSyscallErr digs the bare errno out of Go's layered net errors so the
// printed text reads like the C++ port's strerror output rather than
// "dial tcp4 127.0.0.1:1: connect: connection refused".
func unwrapSyscallErr(err error) error {
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return errno
	}
	return err
}

func setNoDelay(c net.Conn) {
	if tc, ok := c.(*net.TCPConn); ok {
		_ = tc.SetNoDelay(true)
	}
}

// readFullBusy spins a non-blocking recv(2) with no other syscall in the loop
// — no sched_yield, no nanosleep — which is the entire point of "busy-poll".
func readFullBusy(fd int, buf []byte, s *stopper) bool {
	off := 0
	for off < len(buf) {
		n, err := unix.Read(fd, buf[off:])
		if err != nil {
			if errors.Is(err, unix.EAGAIN) || errors.Is(err, unix.EWOULDBLOCK) {
				if s.stop.Load() {
					return false
				}
				continue // spin
			}
			if errors.Is(err, unix.EINTR) {
				continue
			}
			return false
		}
		if n == 0 {
			return false // peer closed
		}
		off += n
	}
	return true
}

func writeAllFD(fd int, buf []byte) {
	off := 0
	for off < len(buf) {
		n, err := unix.Write(fd, buf[off:])
		if err != nil {
			if errors.Is(err, unix.EINTR) || errors.Is(err, unix.EAGAIN) {
				continue
			}
			return // peer gone; nothing sensible to do
		}
		off += n
	}
}

// acceptLoop runs the accept/serve skeleton shared by both servers: a
// deadline-bounded Accept so the stop flag is noticed within one interval,
// then handOff for the per-connection loop.
func acceptLoop(ln net.Listener, s *stopper, handOff func(net.Conn)) error {
	for !s.stop.Load() {
		if tl, ok := ln.(*net.TCPListener); ok {
			_ = tl.SetDeadline(time.Now().Add(acceptPollInterval))
		}
		conn, err := ln.Accept()
		if err != nil {
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				continue
			}
			if s.stop.Load() || errors.Is(err, net.ErrClosed) {
				break
			}
			return fmt.Errorf("accept: %v", unwrapSyscallErr(err))
		}
		setNoDelay(conn)
		s.setConn(conn)
		handOff(conn)
		s.setConn(nil)
		_ = conn.Close()
	}
	return nil
}

// ---------------------------------------------------------------------------
// fastpath / naive servers
// ---------------------------------------------------------------------------

func cmdFastpath(port uint16, pinCPU int, busyPoll bool) int {
	if err := pinToCPU(pinCPU); err != nil {
		fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
		return 1
	}
	ln, err := listenTCP(port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
		return 1
	}
	defer ln.Close()

	s := newStopper()
	s.setListener(ln)
	installSignalHandlers(s)

	busy := "off"
	if busyPoll {
		busy = "on"
	}
	fmt.Fprintf(os.Stderr, "app: fastpath listening on 0.0.0.0:%d pinned-cpu=%d busy-poll=%s\n",
		port, pinCPU, busy)

	// The ONE buffer for the whole life of the server: no allocation anywhere
	// in the loops below.
	buf := make([]byte, frameSize)

	err = acceptLoop(ln, s, func(conn net.Conn) {
		tc, ok := conn.(*net.TCPConn)
		if busyPoll && ok {
			// Take a dup'd descriptor out of the netpoller entirely and drive
			// it non-blocking by hand. TCPConn.File() hands back a blocking
			// dup, so O_NONBLOCK is set explicitly.
			f, ferr := tc.File()
			if ferr != nil {
				return
			}
			defer f.Close()
			fd := int(f.Fd())
			if serr := unix.SetNonblock(fd, true); serr != nil {
				return
			}
			s.setRawFD(fd)
			defer s.setRawFD(-1)
			for {
				if !readFullBusy(fd, buf, s) {
					break
				}
				writeAllFD(fd, buf)
			}
			return
		}
		for {
			if _, rerr := io.ReadFull(conn, buf); rerr != nil {
				break
			}
			if _, werr := conn.Write(buf); werr != nil {
				break
			}
		}
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
		return 1
	}

	fmt.Fprintln(os.Stderr, "app: fastpath shutting down")
	return 0
}

func cmdNaive(port uint16) int {
	ln, err := listenTCP(port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
		return 1
	}
	defer ln.Close()

	s := newStopper()
	s.setListener(ln)
	installSignalHandlers(s)

	fmt.Fprintf(os.Stderr, "app: naive listening on 0.0.0.0:%d\n", port)

	err = acceptLoop(ln, s, func(conn net.Conn) {
		for {
			// The "naive" part: a fresh heap buffer, every single message.
			heapBuf := make([]byte, frameSize)
			if _, rerr := io.ReadFull(conn, heapBuf); rerr != nil {
				break
			}
			if _, werr := conn.Write(heapBuf); werr != nil {
				break
			}
			// heapBuf becomes garbage here, every iteration.
		}
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
		return 1
	}

	fmt.Fprintln(os.Stderr, "app: naive shutting down")
	return 0
}

// ---------------------------------------------------------------------------
// measure
// ---------------------------------------------------------------------------

func percentileNs(sorted []uint64, p float64) uint64 {
	if len(sorted) == 0 {
		return 0
	}
	n := len(sorted)
	idx := int(math.Ceil(p / 100.0 * float64(n)))
	if idx == 0 {
		idx = 1
	}
	if idx > n {
		idx = n
	}
	return sorted[idx-1]
}

func cmdMeasure(host string, port uint16, n, warmup uint64, tag string) int {
	conn, err := connectTCP(host, port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "app: error: %v\n", err)
		return 1
	}
	defer conn.Close()
	setNoDelay(conn)

	fmt.Printf("app: measure target=%s:%d n=%d warmup=%d\n", host, port, n, warmup)

	samples := make([]uint64, 0, n)
	in := make([]byte, frameSize)
	total := n + warmup
	for i := uint64(0); i < total; i++ {
		out := buildFrame(i, 0)
		tSend := nowNs()
		if _, werr := conn.Write(out); werr != nil {
			fmt.Fprintf(os.Stderr, "app: error: measure: connection closed at iteration %d\n", i)
			return 1
		}
		if _, rerr := io.ReadFull(conn, in); rerr != nil {
			fmt.Fprintf(os.Stderr, "app: error: measure: connection closed at iteration %d\n", i)
			return 1
		}
		tRecv := nowNs()
		if !frameHeaderOK(in) || binary.BigEndian.Uint64(in[4:12]) != i {
			fmt.Fprintf(os.Stderr,
				"app: error: measure: malformed/mismatched echo at iteration %d\n", i)
			return 1
		}
		if i >= warmup {
			samples = append(samples, tRecv-tSend)
		}
	}

	sort.Slice(samples, func(a, b int) bool { return samples[a] < samples[b] })
	p50 := percentileNs(samples, 50.0)
	p90 := percentileNs(samples, 90.0)
	p99 := percentileNs(samples, 99.0)
	p999 := percentileNs(samples, 99.9)
	mn := samples[0]
	mx := samples[len(samples)-1]
	sum := 0.0
	for _, v := range samples {
		sum += float64(v)
	}
	mean := sum / float64(len(samples))

	if tag == "" {
		tag = "-"
	}
	fmt.Printf(
		"app: percentiles_ns tag=%s p50=%d p90=%d p99=%d p99.9=%d min=%d max=%d mean=%.2f n=%d\n",
		tag, p50, p90, p99, p999, mn, mx, mean, len(samples))
	fmt.Println("app: table")
	fmt.Printf("  p50    %d ns\n", p50)
	fmt.Printf("  p90    %d ns\n", p90)
	fmt.Printf("  p99    %d ns\n", p99)
	fmt.Printf("  p99.9  %d ns\n", p999)
	fmt.Printf("  min    %d ns\n", mn)
	fmt.Printf("  max    %d ns\n", mx)
	fmt.Printf("  mean   %.2f ns\n", mean)
	return 0
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() int {
	fmt.Fprintln(os.Stderr, "usage: app fastpath --port P --pin CPU [--busy-poll]")
	fmt.Fprintln(os.Stderr, "       app naive --port P")
	fmt.Fprintln(os.Stderr, "       app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]")
	return 2
}

func parsePort(s string) (uint16, bool) {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil || v == 0 || v > 65535 {
		return 0, false
	}
	return uint16(v), true
}

func parseU64(s string) (uint64, bool) {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	if len(args) == 0 {
		return usage()
	}

	switch args[0] {
	case "fastpath":
		portStr := ""
		pinCPU := -1
		busyPoll := false
		for i := 1; i < len(args); i++ {
			switch {
			case args[i] == "--port" && i+1 < len(args):
				i++
				portStr = args[i]
			case args[i] == "--pin" && i+1 < len(args):
				i++
				v, ok := parseU64(args[i])
				if !ok {
					return usage()
				}
				pinCPU = int(v)
			case args[i] == "--busy-poll":
				busyPoll = true
			default:
				return usage()
			}
		}
		port, ok := parsePort(portStr)
		if !ok || pinCPU < 0 {
			return usage()
		}
		return cmdFastpath(port, pinCPU, busyPoll)

	case "naive":
		portStr := ""
		for i := 1; i < len(args); i++ {
			if args[i] == "--port" && i+1 < len(args) {
				i++
				portStr = args[i]
			} else {
				return usage()
			}
		}
		port, ok := parsePort(portStr)
		if !ok {
			return usage()
		}
		return cmdNaive(port)

	case "measure":
		target := ""
		var n uint64
		warmup := uint64(200)
		tag := ""
		haveN := false
		for i := 1; i < len(args); i++ {
			switch {
			case args[i] == "--target" && i+1 < len(args):
				i++
				target = args[i]
			case args[i] == "--n" && i+1 < len(args):
				i++
				v, ok := parseU64(args[i])
				if !ok {
					return usage()
				}
				n = v
				haveN = true
			case args[i] == "--warmup" && i+1 < len(args):
				i++
				v, ok := parseU64(args[i])
				if !ok {
					return usage()
				}
				warmup = v
			case args[i] == "--tag" && i+1 < len(args):
				i++
				tag = args[i]
			default:
				return usage()
			}
		}
		colon := strings.LastIndex(target, ":")
		if !haveN || n == 0 || colon < 0 {
			return usage()
		}
		port, ok := parsePort(target[colon+1:])
		if !ok {
			return usage()
		}
		return cmdMeasure(target[:colon], port, n, warmup, tag)
	}

	return usage()
}
