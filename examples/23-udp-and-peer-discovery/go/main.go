// chatterd v2 (Go) — UDP multicast peer discovery + a TCP chat exchange.
// Grows the same chatterd program introduced in chapter 21: the TCP frame is
// the canonical chatterd chat frame (2-byte magic "CH" + 1-byte version +
// 1-byte type + 2-byte big-endian length + UTF-8 payload) shared by every
// chatterd version (ch21-ch27). The UDP discovery beacon below is a
// completely separate wire object — a plain ASCII datagram, no relation to
// the chat frame's magic/version/type/length header.
//
//	chatterd discover --group 239.7.7.7 --port 51888 --name alice \
//	    --tcp-port 9101 --iface 127.0.0.1 [--announce-ms 200] [--rounds 10]
//
// The multicast listener uses net.ListenMulticastUDP; the beacon sender is a
// raw socket whose IP_MULTICAST_IF/LOOP/TTL are set through golang.org/x/sys/
// unix. The accept and receive loops run as goroutines coordinated by a context
// and a WaitGroup; errors are wrapped with %w.
package main

import (
	"context"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/sys/unix"
)

const beaconMagic = "CHATTERD1" // UDP beacon tag only

// --- canonical chatterd chat frame (TCP) ------------------------------------
// magic 'C' 'H', version 0x01, type (1 byte), length (2 bytes big-endian,
// 0..65535), payload (UTF-8, type-specific). This header is byte-identical
// across every chatterd version; ch23's post-discovery greeting uses only
// JOIN (announce the dialer's name) and DELIVER (name + NUL + text, the
// listener's reply) — the same types chapters 21/22/24/27 share.
const (
	frameMagic0  = 0x43 // 'C'
	frameMagic1  = 0x48 // 'H'
	frameVersion = 0x01
)

const (
	frameJoin    = 1
	frameMsg     = 2
	frameDeliver = 3
	frameWelcome = 4
	framePing    = 5
)

const maxFramePayload = 0xFFFF

type config struct {
	group      string
	port       int
	name       string
	tcpPort    int
	iface      string
	announceMs int
	rounds     int
}

func usage() {
	fmt.Fprintln(os.Stderr,
		"usage: chatterd discover --group <ip> --port <n> --name <s> "+
			"[--tcp-port <n>] [--iface <ip>] [--announce-ms <n>] [--rounds <n>]")
}

// parseArgs returns (cfg, exitCode, ok). On any parse error it prints usage and
// returns ok=false with exit code 2.
func parseArgs(argv []string) (config, int, bool) {
	c := config{tcpPort: 9101, iface: "127.0.0.1", announceMs: 200, rounds: 10}
	if len(argv) < 2 || argv[1] != "discover" {
		usage()
		return c, 2, false
	}
	args := argv[2:]
	next := func(i *int, name string) (string, bool) {
		if *i+1 >= len(args) {
			fmt.Fprintf(os.Stderr, "chatterd: %s needs a value\n", name)
			return "", false
		}
		*i++
		return args[*i], true
	}
	posInt := func(s string, allowZero bool) (int, bool) {
		v, err := strconv.Atoi(s)
		if err != nil || v < 0 || (!allowZero && v == 0) {
			return 0, false
		}
		return v, true
	}
	port16 := func(s string) (int, bool) {
		v, err := strconv.Atoi(s)
		if err != nil || v < 1 || v > 65535 {
			return 0, false
		}
		return v, true
	}
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch a {
		case "--group":
			v, ok := next(&i, a)
			if !ok {
				return c, 2, false
			}
			if ip := net.ParseIP(v); ip == nil || ip.To4() == nil {
				usage()
				return c, 2, false
			}
			c.group = v
		case "--name":
			v, ok := next(&i, a)
			if !ok {
				return c, 2, false
			}
			c.name = v
		case "--iface":
			v, ok := next(&i, a)
			if !ok {
				return c, 2, false
			}
			if ip := net.ParseIP(v); ip == nil || ip.To4() == nil {
				usage()
				return c, 2, false
			}
			c.iface = v
		case "--port":
			v, ok := next(&i, a)
			p, pok := port16(v)
			if !ok || !pok {
				usage()
				return c, 2, false
			}
			c.port = p
		case "--tcp-port":
			v, ok := next(&i, a)
			p, pok := port16(v)
			if !ok || !pok {
				usage()
				return c, 2, false
			}
			c.tcpPort = p
		case "--announce-ms":
			v, ok := next(&i, a)
			p, pok := posInt(v, false)
			if !ok || !pok {
				usage()
				return c, 2, false
			}
			c.announceMs = p
		case "--rounds":
			v, ok := next(&i, a)
			p, pok := posInt(v, false)
			if !ok || !pok {
				usage()
				return c, 2, false
			}
			c.rounds = p
		default:
			fmt.Fprintf(os.Stderr, "chatterd: unknown argument: %s\n", a)
			usage()
			return c, 2, false
		}
	}
	if c.group == "" || c.port == 0 || c.name == "" {
		usage()
		return c, 2, false
	}
	return c, 0, true
}

// --- canonical chatterd chat frame I/O ---------------------------------------

func writeFrame(w io.Writer, typ byte, payload []byte) error {
	if len(payload) > maxFramePayload {
		return fmt.Errorf("frame payload too large: %d", len(payload))
	}
	hdr := [6]byte{
		frameMagic0, frameMagic1, frameVersion, typ,
		byte(len(payload) >> 8), byte(len(payload)),
	}
	if _, err := w.Write(hdr[:]); err != nil {
		return fmt.Errorf("write frame header: %w", err)
	}
	if _, err := w.Write(payload); err != nil {
		return fmt.Errorf("write frame body: %w", err)
	}
	return nil
}

func readFrame(r io.Reader) (byte, []byte, error) {
	var hdr [6]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return 0, nil, fmt.Errorf("read frame header: %w", err)
	}
	if hdr[0] != frameMagic0 || hdr[1] != frameMagic1 {
		return 0, nil, fmt.Errorf("bad frame magic")
	}
	if hdr[2] != frameVersion {
		return 0, nil, fmt.Errorf("unsupported frame version: %d", hdr[2])
	}
	typ := hdr[3]
	n := int(hdr[4])<<8 | int(hdr[5])
	body := make([]byte, n)
	if _, err := io.ReadFull(r, body); err != nil {
		return 0, nil, fmt.Errorf("read frame body: %w", err)
	}
	return typ, body, nil
}

func interfaceForIP(ip net.IP) (*net.Interface, error) {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil, fmt.Errorf("list interfaces: %w", err)
	}
	for i := range ifaces {
		addrs, err := ifaces[i].Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			var aip net.IP
			switch v := a.(type) {
			case *net.IPNet:
				aip = v.IP
			case *net.IPAddr:
				aip = v.IP
			}
			if aip.Equal(ip) {
				return &ifaces[i], nil
			}
		}
	}
	return nil, fmt.Errorf("no interface has address %s", ip)
}

// makeSender builds the raw UDP socket used to emit beacons, pinning the
// outgoing multicast interface and enabling local loopback so a second peer on
// the same host still receives our beacons.
func makeSender(ip4 [4]byte) (int, error) {
	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_DGRAM, 0)
	if err != nil {
		return -1, fmt.Errorf("socket: %w", err)
	}
	if err := unix.SetsockoptInet4Addr(fd, unix.IPPROTO_IP, unix.IP_MULTICAST_IF, ip4); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("IP_MULTICAST_IF: %w", err)
	}
	if err := unix.SetsockoptByte(fd, unix.IPPROTO_IP, unix.IP_MULTICAST_LOOP, 1); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("IP_MULTICAST_LOOP: %w", err)
	}
	if err := unix.SetsockoptByte(fd, unix.IPPROTO_IP, unix.IP_MULTICAST_TTL, 1); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("IP_MULTICAST_TTL: %w", err)
	}
	return fd, nil
}

func dial(ip string, port int) (net.Conn, error) {
	addr := net.JoinHostPort(ip, strconv.Itoa(port))
	// Retry: the peer's beacon may arrive a hair before its listener is ready.
	for i := 0; i < 40; i++ {
		c, err := net.DialTimeout("tcp", addr, 500*time.Millisecond)
		if err == nil {
			return c, nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return nil, fmt.Errorf("connect %s: giving up", addr)
}

func acceptLoop(ctx context.Context, wg *sync.WaitGroup, ln *net.TCPListener, deliverPayload []byte) {
	defer wg.Done()
	for {
		_ = ln.SetDeadline(time.Now().Add(200 * time.Millisecond))
		conn, err := ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			continue
		}
		// Read the dialer's JOIN frame, reply with our DELIVER frame (our
		// name + NUL + greeting text), close.
		if _, _, rerr := readFrame(conn); rerr == nil {
			_ = writeFrame(conn, frameDeliver, deliverPayload)
		}
		conn.Close()
	}
}

func recvLoop(ctx context.Context, wg *sync.WaitGroup, udp *net.UDPConn, cfg config) {
	defer wg.Done()
	seen := make(map[string]bool)
	buf := make([]byte, 2048)
	for {
		_ = udp.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, _, err := udp.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			continue
		}
		tok := strings.Fields(string(buf[:n]))
		if len(tok) != 4 || tok[0] != beaconMagic {
			continue
		}
		pname := tok[1]
		if pname == cfg.name || seen[pname] { // our own beacon or already known
			continue
		}
		pport, err := strconv.Atoi(tok[2])
		if err != nil || pport < 1 || pport > 65535 {
			continue
		}
		pip := tok[3]
		seen[pname] = true

		fmt.Printf("discovered peer %s at %s:%d\n", pname, pip, pport)
		conn, err := dial(pip, pport)
		if err != nil {
			fmt.Fprintf(os.Stderr, "chatterd: error: dial %s: %v\n", pname, err)
			continue
		}
		// JOIN (announce ourselves), then read the peer's DELIVER reply.
		if err := writeFrame(conn, frameJoin, []byte(cfg.name)); err != nil {
			fmt.Fprintf(os.Stderr, "chatterd: error: %v\n", err)
			conn.Close()
			continue
		}
		_, payload, err := readFrame(conn)
		conn.Close()
		if err != nil {
			fmt.Fprintf(os.Stderr, "chatterd: error: %v\n", err)
			continue
		}
		parts := strings.SplitN(string(payload), "\x00", 2)
		rname, rtext := parts[0], ""
		if len(parts) == 2 {
			rtext = parts[1]
		}
		fmt.Printf("peer %s says: %s\n", rname, rtext)
	}
}

func discover(cfg config) error {
	ip := net.ParseIP(cfg.iface)
	if ip == nil || ip.To4() == nil {
		return fmt.Errorf("bad --iface address: %s", cfg.iface)
	}
	gip := net.ParseIP(cfg.group)
	if gip == nil || gip.To4() == nil {
		return fmt.Errorf("bad --group address: %s", cfg.group)
	}
	var ip4, group4 [4]byte
	copy(ip4[:], ip.To4())
	copy(group4[:], gip.To4())

	ifi, err := interfaceForIP(ip)
	if err != nil {
		return err
	}

	lnAny, err := net.Listen("tcp", net.JoinHostPort(cfg.iface, strconv.Itoa(cfg.tcpPort)))
	if err != nil {
		return fmt.Errorf("listen tcp: %w", err)
	}
	ln := lnAny.(*net.TCPListener)
	defer ln.Close()

	udp, err := net.ListenMulticastUDP("udp4", ifi, &net.UDPAddr{IP: gip, Port: cfg.port})
	if err != nil {
		return fmt.Errorf("listen multicast: %w", err)
	}
	defer udp.Close()

	sfd, err := makeSender(ip4)
	if err != nil {
		return err
	}
	defer unix.Close(sfd)

	fmt.Fprintf(os.Stderr, "chatterd: announcing as %s on %s:%d (tcp %s:%d)\n",
		cfg.name, cfg.group, cfg.port, cfg.iface, cfg.tcpPort)

	deliverPayload := []byte(cfg.name + "\x00hello from " + cfg.name)
	ctx, cancel := context.WithCancel(context.Background())
	var wg sync.WaitGroup
	wg.Add(2)
	go acceptLoop(ctx, &wg, ln, deliverPayload)
	go recvLoop(ctx, &wg, udp, cfg)

	beacon := []byte(fmt.Sprintf("%s %s %d %s", beaconMagic, cfg.name, cfg.tcpPort, cfg.iface))
	dst := &unix.SockaddrInet4{Port: cfg.port, Addr: group4}
	for r := 0; r < cfg.rounds; r++ {
		if err := unix.Sendto(sfd, beacon, 0, dst); err != nil {
			cancel()
			wg.Wait()
			return fmt.Errorf("sendto beacon: %w", err)
		}
		time.Sleep(time.Duration(cfg.announceMs) * time.Millisecond)
	}
	// Grace window so final exchanges complete, then unblock the loops.
	time.Sleep(800 * time.Millisecond)
	cancel()
	ln.Close()
	udp.Close()
	wg.Wait()
	return nil
}

func main() {
	cfg, code, ok := parseArgs(os.Args)
	if !ok {
		os.Exit(code)
	}
	if err := discover(cfg); err != nil {
		fmt.Fprintf(os.Stderr, "chatterd: error: %v\n", err)
		os.Exit(1)
	}
}
