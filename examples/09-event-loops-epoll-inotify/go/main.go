// fwatch v1 — a file watcher growing chapter by chapter.
//
// v0 subcommands (snapshot/diff) still work; v1 adds `watch`. Where the C++
// and Rust versions run one explicit epoll loop over inotify + timerfd +
// signalfd, the Go version leans on the runtime: a goroutine reads the
// inotify fd through os.File (the netpoller does the epoll_wait for us) and
// feeds a channel, signals arrive on a channel via signal.Notify, and the
// main select loop multiplexes events, the debounce timer, and the overall
// timeout. Same observable behavior, no visible poller.
package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const debounce = 100 * time.Millisecond

// ---------------------------------------------------------------------------
// v0: snapshot + diff
// ---------------------------------------------------------------------------

func cmdSnapshot(dir string) error {
	entries, err := os.ReadDir(dir) // sorted by filename
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return fmt.Errorf("%s: no such directory", dir)
		}
		return fmt.Errorf("%s: %w", dir, err)
	}
	for _, e := range entries {
		info, err := os.Lstat(filepath.Join(dir, e.Name()))
		if err != nil {
			continue // raced with a concurrent delete
		}
		if !info.Mode().IsRegular() {
			continue
		}
		st := info.Sys().(*syscall.Stat_t)
		mtimeNs := int64(st.Mtim.Sec)*1_000_000_000 + int64(st.Mtim.Nsec)
		fmt.Printf("%s\t%d\t%d\n", e.Name(), info.Size(), mtimeNs)
	}
	return nil
}

// loadSnapshot parses "name<TAB>size<TAB>mtime_ns" lines into
// name -> "size<TAB>mtime_ns"; malformed lines are skipped.
func loadSnapshot(path string) (map[string]string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	out := make(map[string]string)
	for _, line := range strings.Split(string(data), "\n") {
		tab2 := strings.LastIndexByte(line, '\t')
		if tab2 <= 0 {
			continue
		}
		tab1 := strings.LastIndexByte(line[:tab2], '\t')
		if tab1 < 0 {
			continue
		}
		out[line[:tab1]] = line[tab1+1:]
	}
	return out, nil
}

func cmdDiff(oldPath, newPath string) error {
	oldSnap, err := loadSnapshot(oldPath)
	if err != nil {
		return err
	}
	newSnap, err := loadSnapshot(newPath)
	if err != nil {
		return err
	}
	seen := make(map[string]bool, len(oldSnap)+len(newSnap))
	names := make([]string, 0, len(oldSnap)+len(newSnap))
	for name := range oldSnap {
		seen[name] = true
		names = append(names, name)
	}
	for name := range newSnap {
		if !seen[name] {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	for _, name := range names {
		o, inOld := oldSnap[name]
		n, inNew := newSnap[name]
		switch {
		case !inOld:
			fmt.Printf("created %s\n", name)
		case !inNew:
			fmt.Printf("deleted %s\n", name)
		case o != n:
			fmt.Printf("modified %s\n", name)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// v1: watch — goroutine + channels; the runtime owns the epoll loop.
// ---------------------------------------------------------------------------

type fsEvent struct {
	kind string // "created" | "modified" | "deleted"
	name string
}

func classify(mask uint32) (string, bool) {
	switch {
	case mask&(unix.IN_CREATE|unix.IN_MOVED_TO) != 0:
		return "created", true
	case mask&(unix.IN_DELETE|unix.IN_MOVED_FROM) != 0:
		return "deleted", true
	case mask&(unix.IN_MODIFY|unix.IN_ATTRIB) != 0:
		return "modified", true
	}
	return "", false
}

// mergeKinds is the coalescing rule shared by all three implementations:
// within one debounce window, delete wins, a fresh creation stays "created"
// through later writes, and a delete+recreate pair reads as "modified".
func mergeKinds(oldKind, newKind string) string {
	switch {
	case oldKind == "":
		return newKind
	case newKind == "deleted":
		return "deleted"
	case oldKind == "created":
		return "created"
	default:
		return "modified"
	}
}

// readEvents pumps parsed inotify events into ch. The fd is O_NONBLOCK, so
// os.File hands it to the runtime netpoller: Read parks this goroutine, not
// an OS thread, until the kernel marks the fd readable.
func readEvents(f *os.File, ch chan<- fsEvent) {
	defer close(ch)
	buf := make([]byte, 4096)
	for {
		n, err := f.Read(buf)
		if err != nil {
			return // fd closed at exit, or watcher torn down
		}
		for off := 0; off+unix.SizeofInotifyEvent <= n; {
			raw := (*unix.InotifyEvent)(unsafe.Pointer(&buf[off]))
			nameBytes := buf[off+unix.SizeofInotifyEvent : off+unix.SizeofInotifyEvent+int(raw.Len)]
			off += unix.SizeofInotifyEvent + int(raw.Len)
			name := strings.TrimRight(string(nameBytes), "\x00")
			if name == "" {
				continue // event on the directory itself
			}
			if kind, ok := classify(raw.Mask); ok {
				ch <- fsEvent{kind: kind, name: name}
			}
		}
	}
}

type pendingEvent struct {
	kind string
	due  time.Time
}

func cmdWatch(dir string, timeoutMs int) error {
	ifd, err := unix.InotifyInit1(unix.IN_NONBLOCK | unix.IN_CLOEXEC)
	if err != nil {
		return fmt.Errorf("inotify_init1: %w", err)
	}
	watchMask := uint32(unix.IN_CREATE | unix.IN_MODIFY | unix.IN_ATTRIB |
		unix.IN_DELETE | unix.IN_MOVED_FROM | unix.IN_MOVED_TO)
	if _, err := unix.InotifyAddWatch(ifd, dir, watchMask); err != nil {
		unix.Close(ifd)
		if errors.Is(err, unix.ENOENT) {
			return fmt.Errorf("%s: no such directory", dir)
		}
		return fmt.Errorf("%s: %w", dir, err)
	}
	inotifyFile := os.NewFile(uintptr(ifd), "inotify") // registers with the netpoller
	defer inotifyFile.Close()

	events := make(chan fsEvent, 64)
	go readEvents(inotifyFile, events)

	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	defer signal.Stop(sigs)

	fmt.Fprintf(os.Stderr, "fwatch: watching %s\n", dir)
	timeout := time.After(time.Duration(timeoutMs) * time.Millisecond)

	pending := make(map[string]*pendingEvent)
	flushTimer := time.NewTimer(time.Hour)
	flushTimer.Stop()
	defer flushTimer.Stop()

	flush := func(all bool) {
		now := time.Now()
		names := make([]string, 0, len(pending))
		for name, p := range pending {
			if all || !p.due.After(now) {
				names = append(names, name)
			}
		}
		sort.Strings(names) // deterministic batch order
		for _, name := range names {
			fmt.Printf("event: %s %s\n", pending[name].kind, name)
			delete(pending, name)
		}
	}
	rearm := func() {
		if len(pending) == 0 {
			flushTimer.Stop()
			return
		}
		earliest := time.Time{}
		for _, p := range pending {
			if earliest.IsZero() || p.due.Before(earliest) {
				earliest = p.due
			}
		}
		flushTimer.Reset(time.Until(earliest))
	}

	for {
		select {
		case ev, ok := <-events:
			if !ok {
				return errors.New("inotify reader stopped unexpectedly")
			}
			oldKind := ""
			if p, exists := pending[ev.name]; exists {
				oldKind = p.kind
			}
			pending[ev.name] = &pendingEvent{
				kind: mergeKinds(oldKind, ev.kind),
				due:  time.Now().Add(debounce),
			}
			rearm()
		case <-flushTimer.C:
			flush(false)
			rearm()
		case <-timeout:
			flush(true)
			fmt.Println("fwatch: exiting (timeout)")
			return nil
		case <-sigs:
			flush(true)
			fmt.Println("fwatch: exiting (signal)")
			return nil
		}
	}
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func usage() {
	fmt.Fprintln(os.Stderr, "usage: fwatch <command>")
	fmt.Fprintln(os.Stderr, "  snapshot DIR                  one line per regular file: name<TAB>size<TAB>mtime_ns")
	fmt.Fprintln(os.Stderr, "  diff OLD NEW                  compare two snapshots: created|modified|deleted <name>")
	fmt.Fprintln(os.Stderr, "  watch DIR [--timeout-ms T]    watch DIR (default 2000 ms) until timeout or SIGINT/SIGTERM")
	os.Exit(2)
}

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		usage()
	}
	var err error
	var cmd string
	switch {
	case args[0] == "snapshot" && len(args) == 2:
		cmd = "snapshot"
		err = cmdSnapshot(args[1])
	case args[0] == "diff" && len(args) == 3:
		cmd = "diff"
		err = cmdDiff(args[1], args[2])
	case args[0] == "watch" && (len(args) == 2 || len(args) == 4):
		cmd = "watch"
		timeoutMs := 2000
		if len(args) == 4 {
			if args[2] != "--timeout-ms" {
				usage()
			}
			var perr error
			timeoutMs, perr = strconv.Atoi(args[3])
			if perr != nil || timeoutMs <= 0 {
				usage()
			}
		}
		err = cmdWatch(args[1], timeoutMs)
	default:
		usage()
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %s: %v\n", cmd, err)
		os.Exit(1)
	}
}
