// chatterd v0 — a thread-per-connection TCP chat room.
//
// One binary, two subcommands:
//
//	chatterd serve   --port P [--host H]
//	    Accept TCP connections; each client runs on its own goroutine. Every
//	    chat frame received is broadcast to all OTHER clients; a join notice
//	    is broadcast to ALL clients (including the newcomer). SIGINT closes
//	    the listener and exits 0.
//
//	chatterd chatctl --port P --name N [--host H]
//	    Connect, announce N with a join frame, then send one frame per line
//	    of stdin. A reader goroutine prints every received frame as
//	    "<name>: <text>".
//
// THE WIRE PROTOCOL (fixed for the whole book, ch21-ch27). Every message is
// the canonical chatterd CHAT FRAME:
//
//	+-------+---------+------+----------------+------------------+
//	| magic | version | type | length (u16BE) | payload (UTF-8)  |
//	| 2B    | 1B      | 1B   | 2B             | `length` bytes   |
//	+-------+---------+------+----------------+------------------+
//
// magic is the two bytes 0x43 0x48 ("CH"); version is 0x01. length counts the
// payload only (0..65535), never the 6-byte header. This chapter (v0) uses
// three of the five frame types:
//
//	JOIN    (1) payload = name             client -> server, once, at connect
//	MSG     (2) payload = text              client -> server, per stdin line
//	DELIVER (3) payload = name 0x00 text     server -> all clients, a broadcast
//
// WELCOME (4) and PING (5) are reserved for later chapters (ch22, ch24); this
// program never sends them, and a chatctl reader here ignores any frame type
// it doesn't recognize so it stays interoperable with newer servers. The
// server relays a client's MSG as a DELIVER and synthesises join/leave
// DELIVER notices from the reserved sender name "server".
//
// Go: net.Conn per connection, goroutines for readers and per-client writers,
// a context cancelled on SIGINT, and a select over the send/done/ctx channels
// so a slow or departed client never wedges a broadcaster.
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
	"sync"
	"syscall"
)

// The canonical chatterd chat frame header: magic "CH", version 1.
const (
	magic0  = 0x43
	magic1  = 0x48
	version = 0x01

	frameJoin    = 1
	frameMsg     = 2
	frameDeliver = 3
	frameWelcome = 4
	framePing    = 5

	headerSize = 6 // magic(2) + version(1) + type(1) + length(2)
)

func usage() int {
	fmt.Fprintln(os.Stderr, "usage: chatterd serve --port PORT [--host HOST]")
	fmt.Fprintln(os.Stderr, "       chatterd chatctl --port PORT --name NAME [--host HOST]")
	return 2
}

// ---------------------------------------------------------------------------
// The canonical chat frame: magic + version + type + u16BE length + payload.
// ---------------------------------------------------------------------------

func buildFrame(typ byte, payload []byte) []byte {
	// len(payload) is always <= 65535 for the frames this program builds
	// (names/messages typed on a terminal); the length field is u16BE.
	out := make([]byte, headerSize+len(payload))
	out[0] = magic0
	out[1] = magic1
	out[2] = version
	out[3] = typ
	binary.BigEndian.PutUint16(out[4:6], uint16(len(payload)))
	copy(out[headerSize:], payload)
	return out
}

func buildJoin(name string) []byte { return buildFrame(frameJoin, []byte(name)) }
func buildMsg(text string) []byte  { return buildFrame(frameMsg, []byte(text)) }

func buildDeliver(name, text string) []byte {
	payload := make([]byte, 0, len(name)+1+len(text))
	payload = append(payload, name...)
	payload = append(payload, 0)
	payload = append(payload, text...)
	return buildFrame(frameDeliver, payload)
}

// readFrame returns the frame's type and payload, or an error; io.EOF marks a
// clean close.
func readFrame(r *bufio.Reader) (byte, []byte, error) {
	var hdr [headerSize]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, nil, err
	}
	if hdr[0] != magic0 || hdr[1] != magic1 || hdr[2] != version {
		return 0, nil, fmt.Errorf("not a chatterd frame (bad magic/version)")
	}
	typ := hdr[3]
	n := binary.BigEndian.Uint16(hdr[4:6])
	payload := make([]byte, n)
	if n > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return 0, nil, err
		}
	}
	return typ, payload, nil
}

// splitDeliver separates a DELIVER payload's "name\0text" into its two parts.
func splitDeliver(payload []byte) (string, string, bool) {
	sep := -1
	for i, b := range payload {
		if b == 0 {
			sep = i
			break
		}
	}
	if sep < 0 {
		return "", "", false
	}
	return string(payload[:sep]), string(payload[sep+1:]), true
}

// ---------------------------------------------------------------------------
// The hub: every connected client, keyed by id.
// ---------------------------------------------------------------------------

type client struct {
	id   int
	name string
	out  chan []byte
	done chan struct{}
}

type hub struct {
	mu      sync.Mutex
	clients map[int]*client
}

func newHub() *hub { return &hub{clients: make(map[int]*client)} }

func (h *hub) add(c *client) {
	h.mu.Lock()
	h.clients[c.id] = c
	h.mu.Unlock()
}

func (h *hub) setName(id int, name string) {
	h.mu.Lock()
	if c, ok := h.clients[id]; ok {
		c.name = name
	}
	h.mu.Unlock()
}

// remove drops a client and returns the name it last carried.
func (h *hub) remove(id int) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	c, ok := h.clients[id]
	if !ok {
		return ""
	}
	delete(h.clients, id)
	close(c.done) // stop the writer goroutine
	return c.name
}

// broadcast queues frame on every client's outbound channel; when except >= 0
// that id is skipped. A departed client (done closed) is passed over rather
// than blocking the broadcaster.
func (h *hub) broadcast(frame []byte, except int) {
	h.mu.Lock()
	targets := make([]*client, 0, len(h.clients))
	for _, c := range h.clients {
		if c.id != except {
			targets = append(targets, c)
		}
	}
	h.mu.Unlock()
	for _, c := range targets {
		select {
		case c.out <- frame:
		case <-c.done:
		}
	}
}

const broadcastAll = -1

// serveClient owns one connection: a writer goroutine drains c.out to the
// socket, while this goroutine reads frames and drives the hub.
func (h *hub) serveClient(ctx context.Context, conn net.Conn, id int) {
	c := &client{id: id, out: make(chan []byte, 64), done: make(chan struct{})}
	h.add(c)

	go func() {
		for {
			select {
			case frame := <-c.out:
				if _, err := conn.Write(frame); err != nil {
					return
				}
			case <-c.done:
				return
			case <-ctx.Done():
				return
			}
		}
	}()

	defer func() {
		name := h.remove(id)
		conn.Close()
		if name != "" {
			fmt.Fprintf(os.Stderr, "chatterd: %s left\n", name)
			h.broadcast(buildDeliver("server", name+" left"), broadcastAll)
		}
	}()

	r := bufio.NewReader(conn)
	var myName string
	for {
		typ, payload, err := readFrame(r)
		if err != nil {
			return
		}
		switch typ {
		case frameJoin:
			if len(payload) == 0 {
				return // malformed: JOIN carries no name
			}
			myName = string(payload)
			h.setName(id, myName)
			fmt.Fprintf(os.Stderr, "chatterd: %s joined\n", myName)
			h.broadcast(buildDeliver("server", myName+" joined"), broadcastAll)
		case frameMsg:
			if myName == "" {
				continue // MSG before JOIN: nothing sensible to attribute it to
			}
			h.broadcast(buildDeliver(myName, string(payload)), id)
		default:
			// DELIVER/WELCOME/PING are not sent by a client in this chapter;
			// ignore rather than tearing the connection down.
		}
	}
}

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------

func cmdServe(host string, port int) error {
	addr := fmt.Sprintf("%s:%d", host, port)
	ln, err := net.Listen("tcp", addr) // Go sets SO_REUSEADDR on TCP listeners
	if err != nil {
		return fmt.Errorf("listen %s: %w", addr, err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT)
	go func() {
		<-sigCh
		fmt.Fprintln(os.Stderr, "chatterd: shutting down")
		cancel()
		ln.Close() // unblock Accept
	}()

	fmt.Fprintf(os.Stderr, "chatterd: listening on %s\n", addr)

	h := newHub()
	id := 0
	for {
		conn, err := ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil // shutdown requested; a clean exit
			}
			return fmt.Errorf("accept: %w", err)
		}
		id++
		go h.serveClient(ctx, conn, id)
	}
}

// ---------------------------------------------------------------------------
// chatctl
// ---------------------------------------------------------------------------

func cmdChatctl(host string, port int, name string) error {
	addr := fmt.Sprintf("%s:%d", host, port)
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return fmt.Errorf("connect %s: %w", addr, err)
	}
	defer conn.Close()

	// Announce ourselves: a JOIN frame's payload is just our name.
	if _, err := conn.Write(buildJoin(name)); err != nil {
		return fmt.Errorf("write %s: %w", addr, err)
	}

	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		r := bufio.NewReader(conn)
		for {
			typ, payload, err := readFrame(r)
			if err != nil {
				return
			}
			if typ != frameDeliver {
				continue // WELCOME/PING/etc. are for later chapters; ignore them
			}
			n, t, ok := splitDeliver(payload)
			if !ok {
				continue // malformed DELIVER; skip rather than crash
			}
			fmt.Printf("%s: %s\n", n, t)
		}
	}()

	sc := bufio.NewScanner(os.Stdin)
	for sc.Scan() {
		line := sc.Text()
		if line == "" {
			continue // never emit an empty-text frame; that reads as a join
		}
		if _, err := conn.Write(buildMsg(line)); err != nil {
			break
		}
	}
	// stdin EOF: close the socket so the reader sees EOF, then wait for it to
	// finish so every received line is flushed before we exit.
	conn.Close()
	<-readerDone
	return nil
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		os.Exit(usage())
	}
	sub := args[0]
	if sub != "serve" && sub != "chatctl" {
		os.Exit(usage())
	}

	host := "127.0.0.1"
	portStr := ""
	name := ""
	for i := 1; i < len(args); i++ {
		switch {
		case args[i] == "--port" && i+1 < len(args):
			portStr = args[i+1]
			i++
		case args[i] == "--host" && i+1 < len(args):
			host = args[i+1]
			i++
		case args[i] == "--name" && i+1 < len(args):
			name = args[i+1]
			i++
		default:
			os.Exit(usage())
		}
	}
	if portStr == "" {
		os.Exit(usage())
	}
	port, err := strconv.Atoi(portStr)
	if err != nil || port < 1 || port > 65535 {
		os.Exit(usage())
	}

	if sub == "serve" {
		if err := cmdServe(host, port); err != nil {
			fmt.Fprintf(os.Stderr, "chatterd: error: %v\n", err)
			os.Exit(1)
		}
		return
	}
	if strings.TrimSpace(name) == "" {
		os.Exit(usage())
	}
	if err := cmdChatctl(host, port, name); err != nil {
		fmt.Fprintf(os.Stderr, "chatctl: error: %v\n", err)
		os.Exit(1)
	}
}
