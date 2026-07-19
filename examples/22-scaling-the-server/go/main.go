// chatterd v1 — a peer-to-peer chat daemon growing chapter by chapter.
//
// ch21 introduced the length-prefixed frame protocol and a threaded server;
// ch22 — "scaling the server" — is about how a server holds thousands of
// connections. The C++ and Rust versions offer two engines: a thread-per-
// connection "threaded" engine and a single-thread "epoll" engine with
// explicit nonblocking read/write state machines.
//
// Go exposes the SAME "--engine threaded|epoll" flag, but BOTH engines are the
// same code: one goroutine per connection reading, one writing, and a central
// hub goroutine that fans messages out over channels. There is no hand-rolled
// epoll here on purpose — the Go runtime's network poller already multiplexes
// every socket through epoll_wait(2) under the hood and parks a goroutine
// (not an OS thread) on each blocked Read/Write. The "epoll" the other
// languages write by hand, Go's netpoller runs for us; the engine flag only
// changes the label we print. Observable behavior is identical across all three.
//
// Wire format (identical across cpp/go/rust and every version), big-endian:
//
//   offset 0  2  magic   'C' 'H'  (0x43 0x48)
//   offset 2  1  version 0x01
//   offset 3  1  type
//   offset 4  2  length  u16 payload byte count
//   offset 6  N  payload
//
//   type 0x01 JOIN     c->s  payload = nickname (UTF-8)
//   type 0x02 MSG      c->s  payload = message text (UTF-8)
//   type 0x03 DELIVER  s->c  payload = nick + 0x00 (NUL) + text
//   type 0x04 WELCOME  s->c  payload = empty (sent on accept)
package main

import (
	"bytes"
	"errors"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const version = 0x01

const (
	typeJoin    = 0x01
	typeMsg     = 0x02
	typeDeliver = 0x03
	typeWelcome = 0x04
)

// ---------------------------------------------------------------------------
// Frame protocol
// ---------------------------------------------------------------------------

func encode(typ byte, payload []byte) []byte {
	f := make([]byte, 6+len(payload))
	f[0], f[1], f[2], f[3] = 'C', 'H', version, typ
	f[4] = byte(len(payload) >> 8)
	f[5] = byte(len(payload))
	copy(f[6:], payload)
	return f
}

func deliverPayload(nick string, text []byte) []byte {
	p := make([]byte, 0, len(nick)+1+len(text))
	p = append(p, nick...)
	p = append(p, 0)
	p = append(p, text...)
	return p
}

func splitDeliver(payload []byte) (nick, text string) {
	i := bytes.IndexByte(payload, 0)
	if i < 0 {
		return "?", string(payload)
	}
	return string(payload[:i]), string(payload[i+1:])
}

// frameReader pulls whole frames off a stream, buffering partial reads.
type frameReader struct {
	conn net.Conn
	buf  []byte
	tmp  []byte
}

func newFrameReader(conn net.Conn) *frameReader {
	return &frameReader{conn: conn, tmp: make([]byte, 4096)}
}

// next blocks until one whole frame is available; returns the read error
// (io.EOF or a timeout) once the stream can yield no more.
func (fr *frameReader) next() (typ byte, payload []byte, err error) {
	for {
		if len(fr.buf) >= 6 {
			if fr.buf[0] != 'C' || fr.buf[1] != 'H' || fr.buf[2] != version {
				return 0, nil, errors.New("protocol desync")
			}
			ln := int(fr.buf[4])<<8 | int(fr.buf[5])
			if len(fr.buf) >= 6+ln {
				typ = fr.buf[3]
				payload = append([]byte(nil), fr.buf[6:6+ln]...)
				fr.buf = append([]byte(nil), fr.buf[6+ln:]...)
				return typ, payload, nil
			}
		}
		n, rerr := fr.conn.Read(fr.tmp)
		if n > 0 {
			fr.buf = append(fr.buf, fr.tmp[:n]...)
		}
		if rerr != nil {
			return 0, nil, rerr
		}
	}
}

func writeAll(conn net.Conn, data []byte) error {
	for len(data) > 0 {
		n, err := conn.Write(data)
		if err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}

// ---------------------------------------------------------------------------
// Server — one hub goroutine fans out to one goroutine-pair per connection.
// ---------------------------------------------------------------------------

type client struct {
	conn net.Conn
	send chan []byte
	nick string // owned by this client's readPump goroutine
}

type hub struct {
	register   chan *client
	unregister chan *client
	broadcast  chan []byte
	quit       chan struct{}
	done       chan struct{}
	clients    map[*client]bool
	messages   atomic.Uint64
	peak       atomic.Int64
}

func newHub() *hub {
	return &hub{
		register:   make(chan *client),
		unregister: make(chan *client),
		broadcast:  make(chan []byte),
		quit:       make(chan struct{}),
		done:       make(chan struct{}),
		clients:    make(map[*client]bool),
	}
}

func (h *hub) run() {
	defer close(h.done)
	for {
		select {
		case c := <-h.register:
			h.clients[c] = true
			if int64(len(h.clients)) > h.peak.Load() {
				h.peak.Store(int64(len(h.clients)))
			}
			c.send <- encode(typeWelcome, nil)
		case c := <-h.unregister:
			if h.clients[c] {
				delete(h.clients, c)
				close(c.send)
			}
		case frame := <-h.broadcast:
			h.messages.Add(1)
			// Blocking send is the backpressure: a slow client slows the hub
			// rather than dropping a frame. writePump keeps draining even on
			// write errors, so this never deadlocks on a dead peer.
			for c := range h.clients {
				c.send <- frame
			}
		case <-h.quit:
			for c := range h.clients {
				c.conn.Close()
			}
			return
		}
	}
}

func (h *hub) readPump(c *client) {
	defer func() {
		select {
		case h.unregister <- c:
		case <-h.done:
		}
	}()
	fr := newFrameReader(c.conn)
	for {
		typ, payload, err := fr.next()
		if err != nil {
			return
		}
		switch typ {
		case typeJoin:
			c.nick = string(payload)
		case typeMsg:
			frame := encode(typeDeliver, deliverPayload(c.nick, payload))
			select {
			case h.broadcast <- frame:
			case <-h.done:
				return
			}
		}
	}
}

func (c *client) writePump() {
	failed := false
	for frame := range c.send {
		if failed {
			continue // keep draining so the hub never blocks on us
		}
		if err := writeAll(c.conn, frame); err != nil {
			failed = true
			c.conn.Close()
		}
	}
	c.conn.Close()
}

func serve(engine string, port int) error {
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	bound := ln.Addr().(*net.TCPAddr).Port
	fmt.Fprintf(os.Stderr, "chatterd: serving engine=%s on 127.0.0.1:%d\n", engine, bound)

	h := newHub()
	go h.run()

	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return // listener closed at shutdown
			}
			c := &client{conn: conn, send: make(chan []byte, 64), nick: "?"}
			select {
			case h.register <- c:
				go c.writePump()
				go h.readPump(c)
			case <-h.done:
				conn.Close()
				return
			}
		}
	}()

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	<-sigs

	ln.Close()
	close(h.quit)
	<-h.done
	fmt.Fprintf(os.Stderr, "chatterd: stopped engine=%s messages=%d peak_conns=%d\n",
		engine, h.messages.Load(), h.peak.Load())
	return nil
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

func dial(port int) (net.Conn, error) {
	conn, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return nil, fmt.Errorf("cannot connect to 127.0.0.1:%d: %w", port, err)
	}
	return conn, nil
}

func cmdSend(port int, nick, text string) error {
	conn, err := dial(port)
	if err != nil {
		return err
	}
	defer conn.Close()
	if err := writeAll(conn, encode(typeJoin, []byte(nick))); err != nil {
		return fmt.Errorf("send: %w", err)
	}
	if err := writeAll(conn, encode(typeMsg, []byte(text))); err != nil {
		return fmt.Errorf("send: %w", err)
	}
	fr := newFrameReader(conn)
	for {
		typ, payload, err := fr.next()
		if err != nil {
			return nil
		}
		if typ != typeDeliver {
			continue // skip WELCOME
		}
		dn, dt := splitDeliver(payload)
		fmt.Printf("%s: %s\n", dn, dt)
		if dn == nick && dt == text {
			return nil // saw our own message echoed back
		}
	}
}

func cmdListen(port int, nick string, count int) error {
	conn, err := dial(port)
	if err != nil {
		return err
	}
	defer conn.Close()
	if err := writeAll(conn, encode(typeJoin, []byte(nick))); err != nil {
		return fmt.Errorf("send: %w", err)
	}
	conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	fr := newFrameReader(conn)
	got := 0
	for got < count {
		typ, payload, err := fr.next()
		if err != nil {
			return fmt.Errorf("timed out after %d of %d messages", got, count)
		}
		if typ != typeDeliver {
			continue
		}
		dn, dt := splitDeliver(payload)
		fmt.Printf("%s: %s\n", dn, dt)
		got++
	}
	return nil
}

// errQuiet flags a nonzero exit without printing a "chatctl: ..." line — the
// flood counts are already on stdout for the caller to inspect.
var errQuiet = errors.New("flood incomplete")

func cmdFlood(port, n int, text string) error {
	var joined, delivered atomic.Int64
	release := make(chan struct{})
	var wg sync.WaitGroup

	for k := 0; k < n; k++ {
		wg.Add(1)
		go func(k int) {
			defer wg.Done()
			conn, err := dial(port)
			if err != nil {
				return
			}
			defer conn.Close()
			conn.SetReadDeadline(time.Now().Add(10 * time.Second))
			if err := writeAll(conn, encode(typeJoin, []byte(fmt.Sprintf("flooder%d", k)))); err != nil {
				return
			}
			fr := newFrameReader(conn)
			// Wait for the WELCOME: proof the server has us in its fan-out set.
			for {
				typ, _, err := fr.next()
				if err != nil {
					return
				}
				if typ == typeWelcome {
					break
				}
			}
			joined.Add(1)
			<-release
			if k == 0 {
				if err := writeAll(conn, encode(typeMsg, []byte(text))); err != nil {
					return
				}
			}
			// Exactly one broadcast is expected — conn0's message reaches all.
			for {
				typ, _, err := fr.next()
				if err != nil {
					return
				}
				if typ == typeDeliver {
					delivered.Add(1)
					return
				}
			}
		}(k)
	}

	// Wait for every connection to be welcomed before releasing the sender.
	for i := 0; i < 5000 && joined.Load() < int64(n); i++ {
		time.Sleep(time.Millisecond)
	}
	close(release)
	wg.Wait()

	j, d := int(joined.Load()), int(delivered.Load())
	fmt.Printf("flood: connected %d\n", j)
	fmt.Printf("flood: delivered %d\n", d)
	if j != n || d != n {
		return errQuiet
	}
	return nil
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() {
	fmt.Fprintln(os.Stderr, "usage: chatterd <command>")
	fmt.Fprintln(os.Stderr, "  serve --engine threaded|epoll --port P   run the chat daemon")
	fmt.Fprintln(os.Stderr, "  send  --port P NICK TEXT                 join, broadcast, print echoes")
	fmt.Fprintln(os.Stderr, "  listen --port P NICK --count N           join, print N delivered lines")
	fmt.Fprintln(os.Stderr, "  flood --port P N [--text TEXT]           open N conns, broadcast, count")
	os.Exit(2)
}

func flagVal(args []string, name string) (string, bool) {
	for i := 0; i+1 < len(args); i++ {
		if args[i] == name {
			return args[i+1], true
		}
	}
	return "", false
}

func parsePort(s string) (int, bool) {
	p, err := strconv.Atoi(s)
	if err != nil || p < 0 || p > 65535 {
		return 0, false
	}
	return p, true
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		usage()
	}

	var err error
	switch {
	case args[0] == "serve":
		engine, ok1 := flagVal(args, "--engine")
		portStr, ok2 := flagVal(args, "--port")
		port, ok3 := 0, false
		if ok2 {
			port, ok3 = parsePort(portStr)
		}
		if !ok1 || !ok3 || (engine != "threaded" && engine != "epoll") {
			usage()
		}
		err = serve(engine, port)

	case args[0] == "send" && len(args) == 5 && args[1] == "--port":
		port, ok := parsePort(args[2])
		if !ok {
			usage()
		}
		err = cmdSend(port, args[3], args[4])

	case args[0] == "listen" && len(args) == 6 && args[1] == "--port" && args[4] == "--count":
		port, ok := parsePort(args[2])
		count, err2 := strconv.Atoi(args[5])
		if !ok || err2 != nil || count <= 0 {
			usage()
		}
		err = cmdListen(port, args[3], count)

	case args[0] == "flood" && len(args) >= 4 && args[1] == "--port":
		port, ok := parsePort(args[2])
		n, err2 := strconv.Atoi(args[3])
		if !ok || err2 != nil || n <= 0 {
			usage()
		}
		text := "flood"
		if t, ok := flagVal(args, "--text"); ok {
			text = t
		}
		err = cmdFlood(port, n, text)

	default:
		usage()
	}

	if err != nil {
		if !errors.Is(err, errQuiet) {
			fmt.Fprintf(os.Stderr, "chatctl: %v\n", err)
		}
		os.Exit(1)
	}
}
