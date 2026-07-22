// benchlab (Go) — proper benchmarking of the chatterd frame codec (ch21).
//
// Usage:
//
//	app --op encode|decode|roundtrip --iters N [--warmup W]   # the real thing
//	app --lie [--op encode|decode|roundtrip]                  # the anti-pattern
//
// The "real thing" times each iteration individually with a monotonic clock
// (time.Now's monotonic reading), discards a warmup phase, and reports
// min/median/p99/max plus a coordinated-omission-corrected p99 (see
// coCorrect below). `--lie` times a single unwarmed call with wall-clock and
// reports one number — exactly the benchmark this example exists to
// discredit.
//
// Wire format (canonical chatterd chat frame, introduced ch21):
//
//	[ magic 0x43 0x48 ][ version 0x01 ][ type u8 ][ length u16 be ][ payload ]
//
// This file re-implements only encodeFrame/decodeFrame — the codec under
// test — not the daemon; see ch21/22/27 for the networked version.
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sort"
	"strconv"
	"time"
)

// ---------------------------------------------------------------------------
// Frame codec under test — canonical chatterd chat frame (see file header).
// ---------------------------------------------------------------------------
const (
	headerLen = 6
	magic0    = 0x43
	magic1    = 0x48
	version   = 0x01

	typeDeliver = 3
)

func encodeFrame(typ byte, body []byte) []byte {
	out := make([]byte, headerLen+len(body))
	out[0] = magic0
	out[1] = magic1
	out[2] = version
	out[3] = typ
	binary.BigEndian.PutUint16(out[4:6], uint16(len(body)))
	copy(out[6:], body)
	return out
}

// decodeFrame decodes exactly one frame that fills buf completely — no
// partial-read reassembly here, that lives in the networked chatterd
// (ch21/22/27); this harness only measures the codec's per-call cost.
func decodeFrame(buf []byte) (byte, []byte, error) {
	if len(buf) < headerLen {
		return 0, nil, fmt.Errorf("decode frame: %w", errShortHeader)
	}
	if buf[0] != magic0 || buf[1] != magic1 {
		return 0, nil, fmt.Errorf("decode frame: %w", errBadMagic)
	}
	if buf[2] != version {
		return 0, nil, fmt.Errorf("decode frame: %w", errBadVersion)
	}
	typ := buf[3]
	n := binary.BigEndian.Uint16(buf[4:6])
	if len(buf) != headerLen+int(n) {
		return 0, nil, fmt.Errorf("decode frame: %w", errLengthMismatch)
	}
	return typ, buf[headerLen:], nil
}

var (
	errShortHeader    = errors.New("frame shorter than header")
	errBadMagic       = errors.New("bad magic")
	errBadVersion     = errors.New("bad version")
	errLengthMismatch = errors.New("length mismatch")
)

// ---------------------------------------------------------------------------
// The fixed workload: a DELIVER frame carrying a realistic chat line. Every
// language in this example encodes/decodes the identical bytes.
// ---------------------------------------------------------------------------
const (
	nick = "alice"
	text = "the quick brown fox jumps over the lazy dog, three times, for benchlab"
)

func deliveryBody() []byte {
	body := make([]byte, 0, len(nick)+1+len(text))
	body = append(body, nick...)
	body = append(body, 0)
	body = append(body, text...)
	return body
}

type op int

const (
	opEncode op = iota
	opDecode
	opRoundtrip
)

func parseOp(s string) (op, error) {
	switch s {
	case "encode":
		return opEncode, nil
	case "decode":
		return opDecode, nil
	case "roundtrip":
		return opRoundtrip, nil
	default:
		return 0, fmt.Errorf("unknown --op %q", s)
	}
}

func (o op) String() string {
	switch o {
	case opEncode:
		return "encode"
	case opDecode:
		return "decode"
	case opRoundtrip:
		return "roundtrip"
	default:
		return "?"
	}
}

// runOnce performs one iteration of the requested op against the fixed
// workload, returning a small scalar derived from the result so the caller
// can fold it into a checksum — this is what keeps the compiler (and a human
// skimming the loop) from mistaking the call for dead code. A codec bug
// (never expected on this fixed workload) is wrapped and propagated.
func runOnce(o op, body, prebuiltFrame []byte) (uint64, error) {
	switch o {
	case opEncode:
		frame := encodeFrame(typeDeliver, body)
		if len(frame) == 0 {
			return 0, nil
		}
		return uint64(frame[len(frame)-1]), nil
	case opDecode:
		_, payload, err := decodeFrame(prebuiltFrame)
		if err != nil {
			return 0, fmt.Errorf("runOnce decode: %w", err)
		}
		return uint64(len(payload)), nil
	case opRoundtrip:
		frame := encodeFrame(typeDeliver, body)
		_, payload, err := decodeFrame(frame)
		if err != nil {
			return 0, fmt.Errorf("runOnce roundtrip: %w", err)
		}
		return uint64(len(payload)), nil
	default:
		return 0, fmt.Errorf("runOnce: unknown op %d", o)
	}
}

func percentile(sorted []int64, p float64) int64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(p * float64(len(sorted)-1))
	return sorted[idx]
}

// coCorrect applies the coordinated-omission correction (HdrHistogram's
// recordValueWithExpectedInterval): this harness is a closed loop —
// iteration N+1 never starts until N returns — so a stall (a GC pause, a
// page fault, a scheduling hiccup) is recorded as a single slow sample. A
// fixed-rate caller would instead have had many requests queue up behind
// that stall, each experiencing a similar multiple-of-the-target-interval
// delay. This backfills those missing virtual samples so the tail reflects
// what a real caller would have seen, not just what one lucky/unlucky
// iteration measured.
func coCorrect(raw []int64, expectedIntervalNs int64, cap int) []int64 {
	out := make([]int64, 0, len(raw))
	for _, v := range raw {
		out = append(out, v)
		if expectedIntervalNs <= 0 || v <= expectedIntervalNs {
			continue
		}
		missing := v - expectedIntervalNs
		for missing >= expectedIntervalNs {
			if len(out) >= cap {
				return out
			}
			out = append(out, missing)
			missing -= expectedIntervalNs
		}
	}
	return out
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: benchlab --op encode|decode|roundtrip --iters N [--warmup W]")
	fmt.Fprintln(os.Stderr, "       benchlab --lie [--op encode|decode|roundtrip]")
	os.Exit(2)
}

func parseUint(s string) (int64, bool) {
	if s == "" {
		return 0, false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	v, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

func main() {
	var lie bool
	var opStr string
	var iters int64 = -1
	var warmup int64 = 1000

	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--lie":
			lie = true
		case "--op":
			if i+1 >= len(args) {
				usage()
			}
			i++
			opStr = args[i]
		case "--iters":
			if i+1 >= len(args) {
				usage()
			}
			i++
			v, ok := parseUint(args[i])
			if !ok {
				usage()
			}
			iters = v
		case "--warmup":
			if i+1 >= len(args) {
				usage()
			}
			i++
			v, ok := parseUint(args[i])
			if !ok {
				usage()
			}
			warmup = v
		default:
			usage()
		}
	}

	if !lie && opStr == "" {
		usage()
	}
	selected := opRoundtrip
	if opStr != "" {
		parsed, err := parseOp(opStr)
		if err != nil {
			usage()
		}
		selected = parsed
	}

	body := deliveryBody()
	prebuilt := encodeFrame(typeDeliver, body)

	if lie {
		// The anti-pattern: no warmup, one call, wall-clock, done. This is
		// exactly the benchmark this example's README argues never to trust.
		t0 := time.Now()
		sink, err := runOnce(selected, body, prebuilt)
		elapsed := time.Since(t0)
		if err != nil {
			fmt.Fprintln(os.Stderr, "benchlab: codec bug:", err)
			os.Exit(1)
		}
		fmt.Printf("benchlab: lie op=%s (no warmup, single wall-clock sample, ignores variance)\n", selected)
		fmt.Printf("benchlab: lie elapsed_ns=%d sink=%d\n", elapsed.Nanoseconds(), sink)
		return
	}

	if iters < 2 || warmup < 0 || iters <= warmup {
		usage()
	}

	// Warmup: run the op without timing it, letting the allocator/GC/branch
	// predictors reach steady state before any sample is recorded.
	var checksum uint64
	for i := int64(0); i < warmup; i++ {
		v, err := runOnce(selected, body, prebuilt)
		if err != nil {
			fmt.Fprintln(os.Stderr, "benchlab: codec bug:", err)
			os.Exit(1)
		}
		checksum += v
	}

	raw := make([]int64, 0, iters)
	for i := int64(0); i < iters; i++ {
		t0 := time.Now()
		v, err := runOnce(selected, body, prebuilt)
		elapsed := time.Since(t0)
		if err != nil {
			fmt.Fprintln(os.Stderr, "benchlab: codec bug:", err)
			os.Exit(1)
		}
		checksum += v
		raw = append(raw, elapsed.Nanoseconds())
	}

	sorted := append([]int64(nil), raw...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	minNs := sorted[0]
	medianNs := percentile(sorted, 0.5)
	p99Ns := percentile(sorted, 0.99)
	maxNs := sorted[len(sorted)-1]

	expectedIntervalNs := medianNs
	if expectedIntervalNs <= 0 {
		expectedIntervalNs = 1
	}
	const coCap = 5_000_000
	corrected := coCorrect(raw, expectedIntervalNs, coCap)
	sort.Slice(corrected, func(i, j int) bool { return corrected[i] < corrected[j] })
	coP99Ns := percentile(corrected, 0.99)

	fmt.Printf("benchlab: op=%s iters=%d warmup=%d\n", selected, iters, warmup)
	fmt.Printf("benchlab: n=%d min_ns=%d median_ns=%d p99_ns=%d max_ns=%d\n",
		len(sorted), minNs, medianNs, p99Ns, maxNs)
	fmt.Printf("benchlab: co_p99_ns=%d expected_interval_ns=%d co_n=%d\n",
		coP99Ns, expectedIntervalNs, len(corrected))
	fmt.Printf("benchlab: checksum=%016x\n", checksum)
}
