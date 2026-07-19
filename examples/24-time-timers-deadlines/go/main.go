// chatterd v3 — heartbeats + deadlines (chapter 24: time, timers, deadlines).
//
//	chatterd clockprobe
//	chatterd listen  --name N --addr HOST:PORT     [opts]
//	chatterd connect --name N --peer NAME@HOST:PORT [opts]
//
// The Go take on deadlines: a time.Ticker fires the keepalive heartbeat, a
// time.Timer is the peer-liveness deadline (Reset on every received frame),
// and a context from signal.NotifyContext is the clean-shutdown signal. A
// per-connection reader goroutine turns the socket into a channel of frames,
// so the whole session is one select over ticker / deadline / frames /
// ctx.Done(). time.Now carries a monotonic reading, so "silence" is measured
// on the monotonic clock, never the wall clock.
//
// Wire format — this is THE canonical chatterd chat frame, unchanged since it
// was introduced in chapter 21 and extended only by adding TYPE values in
// later chapters; the header never changes. All integers big-endian:
//
//	byte 0   MAGIC0  = 0x43 ('C')
//	byte 1   MAGIC1  = 0x48 ('H')
//	byte 2   VERSION = 0x01
//	byte 3   TYPE    1=JOIN 2=MSG 3=DELIVER 4=WELCOME 5=PING
//	byte 4-5 LENGTH  uint16 payload length N
//	byte 6.. PAYLOAD N bytes: JOIN/MSG/WELCOME/PING are UTF-8 text (PING
//	                          empty); DELIVER is name + 0x00 (NUL) + text.
//
// This v3 (chapter 24) peer only ever emits JOIN, MSG, and PING — DELIVER and
// WELCOME belong to the server-broadcast versions (ch22/ch27) and, since the
// header is shared, are simply treated as liveness traffic here if ever seen.
// PING is the v3 addition over v0/v1. Any received frame is "traffic" and
// resets the deadline; a peer with no traffic for --timeout-ms is dropped.
package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

const (
	magic0  = 'C'
	magic1  = 'H'
	version = 1
	header  = 6

	typeJoin = 1
	typeMsg  = 2
	typePing = 5
	// typeDeliver = 3 and typeWelcome = 4 belong to other chatterd versions
	// (server broadcast / connect motd); v3 never emits them and treats them
	// as generic liveness traffic if ever received.
)

type frame struct {
	typ     byte
	payload []byte
}

func encode(typ byte, payload []byte) []byte {
	out := make([]byte, header+len(payload))
	out[0], out[1], out[2], out[3] = magic0, magic1, version, typ
	binary.BigEndian.PutUint16(out[4:6], uint16(len(payload)))
	copy(out[header:], payload)
	return out
}

func sendFrame(conn net.Conn, typ byte, payload []byte) error {
	if _, err := conn.Write(encode(typ, payload)); err != nil {
		return fmt.Errorf("send frame: %w", err)
	}
	return nil
}

// readFrames pumps decoded frames onto ch until the socket closes; a final
// zero-value frame (typ==0) marks EOF so the session can keep its deadline
// running rather than react to the FIN directly.
func readFrames(conn net.Conn, ch chan<- frame) {
	defer close(ch)
	r := io.Reader(conn)
	head := make([]byte, header)
	for {
		if _, err := io.ReadFull(r, head); err != nil {
			ch <- frame{} // EOF sentinel
			return
		}
		if head[0] != magic0 || head[1] != magic1 || head[2] != version {
			ch <- frame{} // protocol error: treat as gone
			return
		}
		n := binary.BigEndian.Uint16(head[4:6])
		payload := make([]byte, n)
		if _, err := io.ReadFull(r, payload); err != nil {
			ch <- frame{}
			return
		}
		ch <- frame{typ: head[3], payload: payload}
	}
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

type opts struct {
	name         string
	listenAddr   string
	peerName     string
	peerAddr     string
	heartbeatMs  int64
	timeoutMs    int64
	backoffMs    int64
	maxBackoffMs int64
	message      string
	hasMessage   bool
	seed         int64
	hasSeed      bool
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: chatterd clockprobe")
	fmt.Fprintln(os.Stderr, "       chatterd listen  --name N --addr HOST:PORT [opts]")
	fmt.Fprintln(os.Stderr, "       chatterd connect --name N --peer NAME@HOST:PORT [opts]")
	fmt.Fprintln(os.Stderr, "opts:  --heartbeat-ms H --timeout-ms T --backoff-ms B "+
		"--max-backoff-ms M --message MSG --seed S")
	os.Exit(2)
}

func parseCommon(args []string) (opts, error) {
	o := opts{heartbeatMs: 1000, timeoutMs: 3000, backoffMs: 200, maxBackoffMs: 5000}
	need := func(i *int, flag string) (string, error) {
		if *i+1 >= len(args) {
			return "", fmt.Errorf("%s needs a value", flag)
		}
		*i++
		return args[*i], nil
	}
	num := func(s, flag string, min int64) (int64, error) {
		v, err := strconv.ParseInt(s, 10, 64)
		if err != nil || v < min {
			return 0, fmt.Errorf("bad value for %s: %s", flag, s)
		}
		return v, nil
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch a {
		case "--name":
			v, err := need(&i, a)
			if err != nil {
				return o, err
			}
			o.name = v
		case "--addr":
			v, err := need(&i, a)
			if err != nil {
				return o, err
			}
			o.listenAddr = v
		case "--peer":
			v, err := need(&i, a)
			if err != nil {
				return o, err
			}
			at := strings.IndexByte(v, '@')
			if at < 0 {
				return o, fmt.Errorf("--peer wants NAME@HOST:PORT")
			}
			o.peerName, o.peerAddr = v[:at], v[at+1:]
		case "--heartbeat-ms", "--timeout-ms", "--backoff-ms", "--max-backoff-ms":
			v, err := need(&i, a)
			if err != nil {
				return o, err
			}
			n, err := num(v, a, 1)
			if err != nil {
				return o, err
			}
			switch a {
			case "--heartbeat-ms":
				o.heartbeatMs = n
			case "--timeout-ms":
				o.timeoutMs = n
			case "--backoff-ms":
				o.backoffMs = n
			default:
				o.maxBackoffMs = n
			}
		case "--message":
			v, err := need(&i, a)
			if err != nil {
				return o, err
			}
			o.message, o.hasMessage = v, true
		case "--seed":
			v, err := need(&i, a)
			if err != nil {
				return o, err
			}
			n, err := num(v, a, 0)
			if err != nil {
				return o, err
			}
			o.seed, o.hasSeed = n, true
		default:
			return o, fmt.Errorf("unknown flag %s", a)
		}
	}
	if o.name == "" {
		return o, fmt.Errorf("--name is required")
	}
	return o, nil
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

type outcome int

const (
	timedOut outcome = iota
	shutdown
	peerError
)

type sessionResult struct {
	outcome outcome
	linked  bool
	peer    string // name learned from the peer's JOIN, if any
}

// runSession drives one connection until the peer goes silent (timedOut), the
// context is cancelled (shutdown), or the socket errors.
func runSession(ctx context.Context, o opts, conn net.Conn, peerDisplay string) sessionResult {
	defer conn.Close()

	frames := make(chan frame, 8)
	go readFrames(conn, frames)

	ticker := time.NewTicker(time.Duration(o.heartbeatMs) * time.Millisecond)
	defer ticker.Stop()
	deadline := time.NewTimer(time.Duration(o.timeoutMs) * time.Millisecond)
	defer deadline.Stop()

	_ = sendFrame(conn, typeJoin, []byte(o.name))

	linked := false
	sockDead := false
	for {
		var frameCh <-chan frame
		if !sockDead {
			frameCh = frames
		}
		select {
		case <-ctx.Done():
			return sessionResult{shutdown, linked, peerDisplay}
		case <-deadline.C:
			return sessionResult{timedOut, linked, peerDisplay} // peer went silent
		case <-ticker.C:
			if !sockDead {
				_ = sendFrame(conn, typePing, nil)
			}
		case f, ok := <-frameCh:
			if !ok || f.typ == 0 {
				// EOF / protocol error: stop reading, let the deadline fire.
				sockDead = true
				continue
			}
			// Traffic: extend the deadline.
			if !deadline.Stop() {
				select {
				case <-deadline.C:
				default:
				}
			}
			deadline.Reset(time.Duration(o.timeoutMs) * time.Millisecond)
			switch f.typ {
			case typeJoin:
				peerDisplay = string(f.payload)
				if !linked {
					linked = true
					fmt.Printf("chatterd: %s linked with %s\n", o.name, peerDisplay)
					if o.hasMessage {
						_ = sendFrame(conn, typeMsg, []byte(o.message))
					}
				}
			case typeMsg:
				fmt.Printf("chatterd: %s message from %s: %s\n", o.name, peerDisplay, f.payload)
			}
			// PING / DELIVER / WELCOME / reserved: liveness only, already counted.
		}
	}
}

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------

func runListen(ctx context.Context, o opts) int {
	lc := net.ListenConfig{}
	ln, err := lc.Listen(ctx, "tcp", o.listenAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: listen: %v\n", err)
		return 1
	}
	defer ln.Close()
	fmt.Printf("chatterd: %s listening on %s\n", o.name, ln.Addr().String())

	go func() { <-ctx.Done(); ln.Close() }() // unblock Accept on shutdown

	for {
		conn, err := ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				fmt.Printf("chatterd: %s shutting down\n", o.name)
				return 0
			}
			continue
		}
		res := runSession(ctx, o, conn, "peer")
		switch res.outcome {
		case shutdown:
			fmt.Printf("chatterd: %s shutting down\n", o.name)
			return 0
		case timedOut:
			if res.linked {
				fmt.Printf("chatterd: peer %s timed out\n", res.peer)
			}
		}
		// Loop back to accept: a reconnecting peer is welcomed again.
	}
}

func runConnect(ctx context.Context, o opts) int {
	fmt.Printf("chatterd: %s connecting to %s at %s\n", o.name, o.peerName, o.peerAddr)

	var rng *rand.Rand
	if o.hasSeed {
		rng = rand.New(rand.NewSource(o.seed))
	} else {
		rng = rand.New(rand.NewSource(time.Now().UnixNano()))
	}
	backoff := o.backoffMs
	dialer := net.Dialer{}

	for {
		conn, err := dialer.DialContext(ctx, "tcp", o.peerAddr)
		if err == nil {
			backoff = o.backoffMs // a fresh TCP connection resets backoff
			res := runSession(ctx, o, conn, o.peerName)
			switch res.outcome {
			case shutdown:
				fmt.Printf("chatterd: %s shutting down\n", o.name)
				return 0
			case timedOut:
				fmt.Printf("chatterd: peer %s timed out\n", o.peerName)
			}
		} else if ctx.Err() != nil {
			fmt.Printf("chatterd: %s shutting down\n", o.name)
			return 0
		}
		// Jittered exponential backoff (equal jitter: half fixed, half random).
		half := backoff / 2
		delay := half
		if half > 0 {
			delay += rng.Int63n(half + 1)
		}
		fmt.Printf("chatterd: reconnecting to %s in %dms\n", o.peerName, delay)
		select {
		case <-ctx.Done():
			fmt.Printf("chatterd: %s shutting down\n", o.name)
			return 0
		case <-time.After(time.Duration(delay) * time.Millisecond):
		}
		backoff = min(backoff*2, o.maxBackoffMs)
	}
}

func runClockprobe() int {
	var res unix.Timespec
	if err := unix.ClockGetres(unix.CLOCK_MONOTONIC, &res); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: clock_getres: %v\n", err)
		return 1
	}
	resNs := res.Sec*1_000_000_000 + int64(res.Nsec)

	t0 := time.Now()
	req := unix.NsecToTimespec(1_000_000) // 1 ms
	for unix.Nanosleep(&req, &req) == unix.EINTR {
	}
	sleepUs := time.Since(t0).Microseconds()

	tfd, err := unix.TimerfdCreate(unix.CLOCK_MONOTONIC, unix.TFD_CLOEXEC)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: timerfd_create: %v\n", err)
		return 1
	}
	defer unix.Close(tfd)
	its := unix.ItimerSpec{Value: unix.Timespec{Nsec: 1_000_000}} // one-shot 1 ms
	if err := unix.TimerfdSettime(tfd, 0, &its, nil); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: timerfd_settime: %v\n", err)
		return 1
	}
	t1 := time.Now()
	var ticks uint64
	buf := (*[8]byte)(unsafe.Pointer(&ticks))[:]
	_, _ = unix.Read(tfd, buf)
	tfdUs := time.Since(t1).Microseconds()

	fmt.Printf("clockprobe: CLOCK_MONOTONIC res=%dns nanosleep(1ms) actual=%dus "+
		"timerfd(1ms) actual=%dus\n", resNs, sleepUs, tfdUs)
	return 0
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		usage()
	}
	sub, rest := args[0], args[1:]

	if sub == "clockprobe" {
		os.Exit(runClockprobe())
	}
	if sub != "listen" && sub != "connect" {
		usage()
	}

	o, err := parseCommon(rest)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: %v\n", err)
		usage()
	}

	ctx, stop := signal.NotifyContext(context.Background(), unix.SIGINT, unix.SIGTERM)
	defer stop()

	switch sub {
	case "listen":
		if o.listenAddr == "" {
			fmt.Fprintln(os.Stderr, "chatterd: error: listen needs --addr")
			usage()
		}
		os.Exit(runListen(ctx, o))
	case "connect":
		if o.peerAddr == "" {
			fmt.Fprintln(os.Stderr, "chatterd: error: connect needs --peer")
			usage()
		}
		os.Exit(runConnect(ctx, o))
	}
}
