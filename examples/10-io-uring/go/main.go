// fwatch v2 — batched sync engine (chapter 10: io_uring).
//
// Subcommands:
//
//	fwatch scan DIR                                 (v0: polling snapshot)
//	fwatch watch DIR --timeout MS                   (v1: inotify events)
//	fwatch sync SRCDIR DSTDIR [--engine rw|uring]   (v2: batched copy)
//
// Go has no supported io_uring story: the runtime already multiplexes
// goroutines over epoll in the netpoller, and the io_uring proposal
// (golang/go#31908) has been on hold for years. So the uring engine is
// deliberately absent here — "fwatch sync ... --engine uring" reports it
// unsupported and exits 64. That asymmetry IS the chapter's point.
package main

import (
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const chunk = 128 * 1024

func usage() int {
	fmt.Fprintln(os.Stderr, "usage: fwatch <command>")
	fmt.Fprintln(os.Stderr, "  fwatch scan DIR")
	fmt.Fprintln(os.Stderr, "  fwatch watch DIR --timeout MS")
	fmt.Fprintln(os.Stderr, "  fwatch sync SRCDIR DSTDIR [--engine rw|uring]")
	return 2
}

// walk visits every entry under src depth-first and reports its path and type.
func walk(src string, visit func(path string, d fs.DirEntry) error) error {
	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return fmt.Errorf("walk %s: %w", path, err)
		}
		if path == src {
			return nil
		}
		return visit(path, d)
	})
}

func cmdScan(dir string) int {
	var files, bytes uint64
	err := walk(dir, func(path string, d fs.DirEntry) error {
		if !d.Type().IsRegular() {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return fmt.Errorf("stat %s: %w", path, err)
		}
		files++
		bytes += uint64(info.Size())
		return nil
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %v\n", err)
		return 1
	}
	fmt.Printf("scanned %d files %d bytes\n", files, bytes)
	return 0
}

// copyFileRW copies src to dst with a plain read/write loop.
func copyFileRW(src, dst string) (uint64, error) {
	in, err := os.Open(src)
	if err != nil {
		return 0, fmt.Errorf("open %s: %w", src, err)
	}
	defer in.Close()
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return 0, fmt.Errorf("create %s: %w", dst, err)
	}
	defer out.Close()

	buf := make([]byte, chunk)
	var total uint64
	for {
		n, err := in.Read(buf)
		if n > 0 {
			if _, werr := out.Write(buf[:n]); werr != nil {
				return total, fmt.Errorf("write %s: %w", dst, werr)
			}
			total += uint64(n)
		}
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return total, fmt.Errorf("read %s: %w", src, err)
		}
	}
	if err := out.Close(); err != nil {
		return total, fmt.Errorf("close %s: %w", dst, err)
	}
	return total, nil
}

func cmdSync(src, dst, engine string) int {
	if engine == "uring" {
		fmt.Fprintln(os.Stderr, "engine=uring: unsupported in Go (see chapter 10)")
		return 64
	}
	start := time.Now()
	if err := os.MkdirAll(dst, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %v\n", err)
		return 1
	}
	var files, bytes uint64
	err := walk(src, func(path string, d fs.DirEntry) error {
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return fmt.Errorf("rel %s: %w", path, err)
		}
		target := filepath.Join(dst, rel)
		switch {
		case d.IsDir():
			return os.MkdirAll(target, 0o755)
		case d.Type().IsRegular():
			n, err := copyFileRW(path, target)
			if err != nil {
				return err
			}
			files++
			bytes += n
		}
		return nil
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %v\n", err)
		return 1
	}
	ms := time.Since(start).Milliseconds()
	fmt.Printf("synced %d files %d bytes engine=%s ms=%d\n", files, bytes, engine, ms)
	return 0
}

type inotifyEvent struct {
	kind string
	name string
}

func kindOf(mask uint32) string {
	switch {
	case mask&unix.IN_CREATE != 0:
		return "CREATE"
	case mask&unix.IN_MODIFY != 0:
		return "MODIFY"
	case mask&unix.IN_DELETE != 0:
		return "DELETE"
	}
	return ""
}

// readEvents parses one inotify read(2) buffer into events with names.
func readEvents(buf []byte) []inotifyEvent {
	var out []inotifyEvent
	for off := 0; off+unix.SizeofInotifyEvent <= len(buf); {
		raw := (*unix.InotifyEvent)(unsafe.Pointer(&buf[off]))
		nameLen := int(raw.Len)
		name := ""
		if nameLen > 0 {
			b := buf[off+unix.SizeofInotifyEvent : off+unix.SizeofInotifyEvent+nameLen]
			name = strings.TrimRight(string(b), "\x00")
		}
		if kind := kindOf(raw.Mask); kind != "" && name != "" {
			out = append(out, inotifyEvent{kind: kind, name: name})
		}
		off += unix.SizeofInotifyEvent + nameLen
	}
	return out
}

func cmdWatch(dir string, timeoutMS int) int {
	fd, err := unix.InotifyInit1(unix.IN_CLOEXEC)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: inotify_init1: %v\n", err)
		return 1
	}
	defer unix.Close(fd)
	if _, err := unix.InotifyAddWatch(fd, dir,
		unix.IN_CREATE|unix.IN_MODIFY|unix.IN_DELETE); err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %s: %v\n", dir, err)
		return 1
	}

	events := make(chan inotifyEvent, 64)
	errs := make(chan error, 1)
	// Reader goroutine: blocking read(2) on the inotify fd, events flow out
	// over a channel; process exit reclaims it once the deadline fires.
	go func() {
		buf := make([]byte, 16*1024)
		for {
			n, err := unix.Read(fd, buf)
			if err != nil {
				if errors.Is(err, unix.EINTR) {
					continue
				}
				errs <- fmt.Errorf("inotify read: %w", err)
				return
			}
			for _, ev := range readEvents(buf[:n]) {
				events <- ev
			}
		}
	}()

	deadline := time.After(time.Duration(timeoutMS) * time.Millisecond)
	var count uint64
	for {
		select {
		case ev := <-events:
			fmt.Printf("event %s %s\n", ev.kind, ev.name)
			count++
		case err := <-errs:
			fmt.Fprintf(os.Stderr, "fwatch: %v\n", err)
			return 1
		case <-deadline:
			fmt.Printf("watched %d events\n", count)
			return 0
		}
	}
}

func run(args []string) int {
	if len(args) == 0 {
		return usage()
	}
	switch cmd := args[0]; {
	case cmd == "scan" && len(args) == 2:
		return cmdScan(args[1])
	case cmd == "watch" && len(args) == 4 && args[2] == "--timeout":
		timeoutMS, err := strconv.Atoi(args[3])
		if err != nil || timeoutMS < 0 {
			return usage()
		}
		return cmdWatch(args[1], timeoutMS)
	case cmd == "sync" && (len(args) == 3 || len(args) == 5):
		engine := "rw"
		if len(args) == 5 {
			if args[3] != "--engine" {
				return usage()
			}
			engine = args[4]
		}
		if engine != "rw" && engine != "uring" {
			return usage()
		}
		return cmdSync(args[1], args[2], engine)
	}
	return usage()
}

func main() {
	os.Exit(run(os.Args[1:]))
}
