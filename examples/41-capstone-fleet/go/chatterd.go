// chatterd.go — the book's recurring peer-to-peer chat daemon (ch21-27),
// reduced to what the capstone needs: serve local clients, and (new in this
// chapter) bridge two chatterd instances across hosts so a message sent on
// one node is delivered to clients on the other. The wire frame (proto.go)
// is unchanged from every prior chatterd chapter.
//
// Bridging design: a bridge worker is just an ordinary client of the *other*
// node's server (nick "bridge@<local-node>"). Concretely, with --peer set on
// both sides:
//
//	target dials peer, joins as "bridge@target"   (this connection lives in
//	                                                peer's client registry)
//	peer   dials target, joins as "bridge@peer"   (lives in target's registry)
//
// A local MSG is broadcast as DELIVER to every registered connection
// (including a bridge connection dialed in from the other side) exactly like
// any other client — no protocol change. When a bridge *worker* itself
// receives a DELIVER over the connection it dialed, it re-broadcasts locally
// with includeBridges=false, so the message never goes back out over any
// bridge: no ping-pong between two nodes.
package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
)

type peerConn struct {
	conn     net.Conn
	nick     string
	isBridge bool
	wmu      sync.Mutex
}

func (c *peerConn) send(f frame) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	return writeFrame(c.conn, f)
}

type hub struct {
	mu      sync.Mutex
	clients map[*peerConn]struct{}
}

func newHub() *hub { return &hub{clients: make(map[*peerConn]struct{})} }

func (h *hub) add(c *peerConn) {
	h.mu.Lock()
	h.clients[c] = struct{}{}
	h.mu.Unlock()
}

func (h *hub) remove(c *peerConn) {
	h.mu.Lock()
	delete(h.clients, c)
	h.mu.Unlock()
}

func (h *hub) broadcastDeliver(exclude *peerConn, includeBridges bool, nick, text string) {
	f := frame{typ: typeDeliver, payload: deliverPayload(nick, text)}
	h.mu.Lock()
	targets := make([]*peerConn, 0, len(h.clients))
	for c := range h.clients {
		if c == exclude {
			continue
		}
		if c.isBridge && !includeBridges {
			continue
		}
		targets = append(targets, c)
	}
	h.mu.Unlock()
	for _, c := range targets {
		_ = c.send(f)
	}
}

func chatterdServe(host string, port int, node, peerAddr, peerNode string, tel *telemetry) int {
	addr := net.JoinHostPort(host, strconv.Itoa(port))
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: listen %s: %v\n", addr, err)
		return 1
	}
	fmt.Fprintf(os.Stderr, "chatterd: listening on %s node=%s\n", addr, node)

	h := newHub()
	sig := installSignalFlag()

	go func() {
		for !sig.Load() {
			time.Sleep(50 * time.Millisecond)
		}
		fmt.Fprintln(os.Stderr, "chatterd: shutdown")
		ln.Close()
	}()

	if peerAddr != "" {
		if peerNode == "" {
			peerNode = "remote"
		}
		go bridgeWorker(peerAddr, node, peerNode, h, sig)
	}

	for {
		conn, err := ln.Accept()
		if err != nil {
			if sig.Load() {
				return 0
			}
			continue
		}
		go handleClient(conn, h, node, tel)
	}
}

func handleClient(conn net.Conn, h *hub, node string, tel *telemetry) {
	pc := &peerConn{conn: conn}
	defer func() {
		h.remove(pc)
		conn.Close()
	}()

	first, err := readFrame(conn)
	if err != nil || first.typ != typeJoin {
		return
	}
	pc.nick = string(first.payload)
	pc.isBridge = strings.HasPrefix(pc.nick, "bridge@")
	h.add(pc)

	for {
		f, err := readFrame(conn)
		if err != nil {
			return
		}
		if f.typ != typeMsg {
			continue
		}
		text := string(f.payload)

		ctx, span := tel.tracer.Start(context.Background(), "chatterd.deliver")
		span.SetAttributes(
			attribute.String("chat.from", pc.nick),
			attribute.Int("chat.text_len", len(text)),
			attribute.String("chat.node", node),
			attribute.Bool("chat.from_bridge", pc.isBridge),
		)
		h.broadcastDeliver(pc, true, pc.nick, text)
		span.SetStatus(codes.Ok, "")
		if tel.enabled {
			fmt.Printf("chatterd: trace_id=%s node=%s from=%s\n",
				span.SpanContext().TraceID().String(), node, pc.nick)
		}
		span.End()
		_ = ctx
	}
}

func bridgeWorker(peerAddr, localNode, peerNode string, h *hub, sig interface{ Load() bool }) {
	backoff := 300 * time.Millisecond
	for !sig.Load() {
		conn, err := net.DialTimeout("tcp", peerAddr, 3*time.Second)
		if err != nil {
			time.Sleep(backoff)
			continue
		}
		bridgeNick := "bridge@" + localNode
		if err := writeFrame(conn, frame{typ: typeJoin, payload: []byte(bridgeNick)}); err != nil {
			conn.Close()
			time.Sleep(backoff)
			continue
		}
		fmt.Fprintf(os.Stderr, "chatterd: bridge connected peer=%s as=%s\n", peerAddr, bridgeNick)

		for {
			f, err := readFrame(conn)
			if err != nil {
				break
			}
			if f.typ != typeDeliver {
				continue
			}
			nick, text, ok := splitDeliver(f.payload)
			if !ok {
				continue
			}
			if !strings.Contains(nick, "@") {
				nick = nick + "@" + peerNode
			}
			h.broadcastDeliver(nil, false, nick, text)
		}
		conn.Close()
		fmt.Fprintf(os.Stderr, "chatterd: bridge disconnected peer=%s (retrying)\n", peerAddr)
		time.Sleep(backoff)
	}
}

// ---------------------------------------------------------------------------
// send / listen — minimal test clients used by verify to prove cross-host
// delivery deterministically (this capstone trims ch27's fuller "loadtest").
// ---------------------------------------------------------------------------

func chatterdSend(host string, port int, nick, text string, timeoutMs int) int {
	addr := net.JoinHostPort(host, strconv.Itoa(port))
	conn, err := net.DialTimeout("tcp", addr, time.Duration(timeoutMs)*time.Millisecond)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: send: dial %s: %v\n", addr, err)
		return 1
	}
	defer conn.Close()
	if err := writeFrame(conn, frame{typ: typeJoin, payload: []byte(nick)}); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: send: join: %v\n", err)
		return 1
	}
	if err := writeFrame(conn, frame{typ: typeMsg, payload: []byte(text)}); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: send: msg: %v\n", err)
		return 1
	}
	time.Sleep(150 * time.Millisecond) // let the write actually flush before closing
	fmt.Printf("chatterd: sent nick=%s text=%s\n", nick, text)
	return 0
}

func chatterdListen(host string, port int, nick string, timeoutMs int) int {
	addr := net.JoinHostPort(host, strconv.Itoa(port))
	conn, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: listen: dial %s: %v\n", addr, err)
		return 1
	}
	defer conn.Close()
	if err := writeFrame(conn, frame{typ: typeJoin, payload: []byte(nick)}); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: listen: join: %v\n", err)
		return 1
	}
	conn.SetReadDeadline(time.Now().Add(time.Duration(timeoutMs) * time.Millisecond))
	for {
		f, err := readFrame(conn)
		if err != nil {
			fmt.Fprintln(os.Stderr, "chatterd: listen timeout")
			return 1
		}
		if f.typ != typeDeliver {
			continue
		}
		fromNick, text, ok := splitDeliver(f.payload)
		if !ok {
			continue
		}
		fmt.Printf("chatterd: received from=%s text=%s\n", fromNick, text)
		return 0
	}
}
