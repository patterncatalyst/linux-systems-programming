// chatterd v4 (Go) — a peer-to-peer chat daemon.
//
// This is the async chapter, and Go's answer is unchanged from v1: goroutines
// already ARE the async model. `serve --engine async` and `--engine thread`
// select the very same goroutine-per-connection server (a hub goroutine fans
// out broadcasts over channels via select + context for shutdown). The engine
// flag exists only for parity with the C++/Rust versions, which rewrite their
// connection handling for this chapter; here there is nothing to rewrite.
//
// Wire format (canonical chatterd chat frame, v1..v4):
//
//	[ magic 0x43 0x48 ][ version 0x01 ][ type u8 ][ length u16 be ][ payload ]
//	type: JOIN=1, MSG=2, DELIVER=3, WELCOME=4, PING=5.
//	The client's first frame is JOIN (payload = its nick). Every later client
//	frame is MSG (payload = message text). For a MSG with text T from the
//	client whose nick is N, the server sends every OTHER client a DELIVER
//	frame whose payload is N + 0x00 (NUL) + T. This chapter's engines use
//	only JOIN/MSG/DELIVER — WELCOME and PING are other versions' additions to
//	the same frame and are simply never sent here.
//
// Usage:
//
//	app serve [--engine thread|async] [--host H] [--port P]
//	app loadtest [--host H] [--port P] [--clients N]
package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

// ---------------------------------------------------------------------------
// Frame helpers — canonical chatterd chat frame (see file header).
// ---------------------------------------------------------------------------

const (
	magic0  = 0x43
	magic1  = 0x48
	version = 0x01

	typeJoin    = 1
	typeMsg     = 2
	typeDeliver = 3
)

func encodeFrame(typ byte, body string) []byte {
	b := make([]byte, 6+len(body))
	b[0] = magic0
	b[1] = magic1
	b[2] = version
	b[3] = typ
	binary.BigEndian.PutUint16(b[4:6], uint16(len(body)))
	copy(b[6:], body)
	return b
}

// readFrame returns the frame's type and payload.
func readFrame(r *bufio.Reader) (byte, string, error) {
	var hdr [6]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, "", err
	}
	typ := hdr[3]
	n := binary.BigEndian.Uint16(hdr[4:6])
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return 0, "", fmt.Errorf("read body: %w", err)
	}
	return typ, string(buf), nil
}

// ---------------------------------------------------------------------------
// Hub: owns the client set, fans out broadcasts over channels.
// ---------------------------------------------------------------------------

type client struct {
	conn net.Conn
	out  chan []byte
}

type broadcastReq struct {
	from  *client
	frame []byte
}

type hub struct {
	register   chan *client
	unregister chan *client
	broadcast  chan broadcastReq
}

func newHub() *hub {
	return &hub{
		register:   make(chan *client),
		unregister: make(chan *client),
		broadcast:  make(chan broadcastReq),
	}
}

func (h *hub) run(ctx context.Context) {
	clients := make(map[*client]struct{})
	for {
		select {
		case c := <-h.register:
			clients[c] = struct{}{}
		case c := <-h.unregister:
			if _, ok := clients[c]; ok {
				delete(clients, c)
				close(c.out)
			}
		case b := <-h.broadcast:
			for c := range clients {
				if c != b.from {
					c.out <- b.frame // buffered; delivery is guaranteed
				}
			}
		case <-ctx.Done():
			for c := range clients {
				c.conn.Close() // unblock the reader goroutine
				close(c.out)
			}
			return
		}
	}
}

// handleConn reads frames from one client: the first (JOIN) is its nick, each
// later one (MSG) is a message to broadcast as a DELIVER frame.
func handleConn(ctx context.Context, h *hub, conn net.Conn) {
	defer conn.Close()
	c := &client{conn: conn, out: make(chan []byte, 64)}
	r := bufio.NewReader(conn)

	// First frame is JOIN: its payload is the nick.
	_, nick, err := readFrame(r)
	if err != nil {
		return
	}
	select {
	case h.register <- c:
	case <-ctx.Done():
		return
	}
	// Writer goroutine: drains c.out until the hub closes it.
	go func() {
		for frame := range c.out {
			if _, err := conn.Write(frame); err != nil {
				return
			}
		}
	}()

	for {
		// Every later frame is MSG: its payload is the message text.
		_, body, err := readFrame(r)
		if err != nil {
			break
		}
		frame := encodeFrame(typeDeliver, nick+"\x00"+body)
		select {
		case h.broadcast <- broadcastReq{from: c, frame: frame}:
		case <-ctx.Done():
			return
		}
	}
	select {
	case h.unregister <- c:
	case <-ctx.Done():
	}
}

// listen builds a TCP listener with SO_REUSEADDR set via x/sys/unix, so the
// daemon can rebind its port immediately after a restart.
func listen(ctx context.Context, host string, port int) (net.Listener, error) {
	lc := net.ListenConfig{
		Control: func(network, address string, c syscall.RawConn) error {
			var serr error
			if err := c.Control(func(fd uintptr) {
				serr = unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)
			}); err != nil {
				return err
			}
			return serr
		},
	}
	return lc.Listen(ctx, "tcp", net.JoinHostPort(host, strconv.Itoa(port)))
}

func runServe(engine, host string, port int) int {
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	ln, err := listen(ctx, host, port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: listen: %v\n", err)
		return 1
	}

	fmt.Fprintf(os.Stderr, "chatterd: listening on %s:%d engine=%s\n", host, port, engine)

	h := newHub()
	go h.run(ctx)

	// Unblock Accept on shutdown.
	go func() {
		<-ctx.Done()
		ln.Close()
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			break // listener closed on shutdown
		}
		go handleConn(ctx, h, conn)
	}

	fmt.Fprintln(os.Stderr, "chatterd: shutdown")
	return 0
}

// ---------------------------------------------------------------------------
// loadtest client — drives the broadcast assertion.
// ---------------------------------------------------------------------------

func runLoadtest(host string, port, nclients int) int {
	addr := net.JoinHostPort(host, strconv.Itoa(port))
	receivers := make([]net.Conn, nclients)
	readers := make([]*bufio.Reader, nclients)

	for i := 0; i < nclients; i++ {
		c, err := net.Dial("tcp", addr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "loadtest: connect failed: %v\n", err)
			return 1
		}
		receivers[i] = c
		readers[i] = bufio.NewReader(c)
		if _, err := c.Write(encodeFrame(typeJoin, fmt.Sprintf("r%d", i))); err != nil {
			fmt.Fprintf(os.Stderr, "loadtest: hello failed: %v\n", err)
			return 1
		}
	}

	sender, err := net.Dial("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "loadtest: sender connect failed: %v\n", err)
		return 1
	}
	sender.Write(encodeFrame(typeJoin, "sender"))

	// Let the server register every client before the broadcast goes out.
	time.Sleep(250 * time.Millisecond)
	sender.Write(encodeFrame(typeMsg, "hello world"))

	const wantNick = "sender"
	const wantText = "hello world"
	delivered := 0
	for i := 0; i < nclients; i++ {
		receivers[i].SetReadDeadline(time.Now().Add(5 * time.Second))
		typ, body, err := readFrame(readers[i])
		ok := err == nil && typ == typeDeliver
		if ok {
			if idx := strings.IndexByte(body, 0); idx >= 0 {
				ok = body[:idx] == wantNick && body[idx+1:] == wantText
			} else {
				ok = false
			}
		}
		if ok {
			delivered++
		} else {
			fmt.Fprintf(os.Stderr, "loadtest: client r%d missed the broadcast\n", i)
		}
	}
	fmt.Printf("loadtest: delivered %d/%d\n", delivered, nclients)
	if delivered == nclients {
		return 0
	}
	return 1
}

// ---------------------------------------------------------------------------
// CLI.
// ---------------------------------------------------------------------------

func usage() {
	fmt.Fprintln(os.Stderr, "usage:")
	fmt.Fprintln(os.Stderr, "  app serve [--engine thread|async] [--host H] [--port P]")
	fmt.Fprintln(os.Stderr, "  app loadtest [--host H] [--port P] [--clients N]")
}

type opts struct {
	engine  string
	host    string
	port    int
	clients int
}

// parseOpts consumes --key value pairs. Returns false on malformed input.
func parseOpts(args []string, o *opts) bool {
	for i := 0; i < len(args); i++ {
		next := func() (string, bool) {
			if i+1 >= len(args) {
				return "", false
			}
			i++
			return args[i], true
		}
		var v string
		var ok bool
		switch args[i] {
		case "--engine":
			if v, ok = next(); !ok {
				return false
			}
			o.engine = v
		case "--host":
			if v, ok = next(); !ok {
				return false
			}
			o.host = v
		case "--port":
			if v, ok = next(); !ok {
				return false
			}
			n, err := strconv.Atoi(v)
			if err != nil {
				return false
			}
			o.port = n
		case "--clients":
			if v, ok = next(); !ok {
				return false
			}
			n, err := strconv.Atoi(v)
			if err != nil {
				return false
			}
			o.clients = n
		default:
			return false
		}
	}
	return true
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	cmd := os.Args[1]
	o := opts{engine: "async", host: "127.0.0.1", port: 47100, clients: 20}
	if !parseOpts(os.Args[2:], &o) {
		usage()
		os.Exit(2)
	}

	switch cmd {
	case "serve":
		if o.engine != "thread" && o.engine != "async" {
			fmt.Fprintf(os.Stderr, "chatterd: unknown engine '%s' (want thread|async)\n", o.engine)
			os.Exit(2)
		}
		os.Exit(runServe(o.engine, o.host, o.port))
	case "loadtest":
		if o.clients < 1 || o.clients > 65535 {
			fmt.Fprintln(os.Stderr, "loadtest: --clients out of range")
			os.Exit(2)
		}
		os.Exit(runLoadtest(o.host, o.port, o.clients))
	default:
		usage()
		os.Exit(2)
	}
}
