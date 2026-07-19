// fwatch v0 — snapshot/diff a directory tree with dirfd-relative syscalls.
//
//	fwatch snapshot DIR              one "<relpath> <size> <mtime>" line per
//	                                 regular file, sorted by path
//	fwatch diff DIR SNAPSHOT_FILE    re-scan and print +/-/~ lines plus a
//	                                 "fwatch: A added, R removed, M modified"
//	                                 summary
//
// The walk is deliberately explicit: unix.Openat relative to the parent
// directory fd, Readdirnames on an os.File wrapping that fd, and
// unix.Fstatat with AT_SYMLINK_NOFOLLOW to classify entries. os.File is the
// fd's owner: one deferred Close per directory, no leaks on any error path.
package main

import (
	"errors"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"

	"golang.org/x/sys/unix"
)

type info struct {
	size  int64
	mtime int64
}

type tree map[string]info

const dirFlags = unix.O_RDONLY | unix.O_DIRECTORY | unix.O_CLOEXEC | unix.O_NOFOLLOW

// errnoOf digs the raw errno out of a wrapped error chain so every language
// implementation reports the same "(errno N)" suffix.
func errnoOf(err error) int {
	var errno unix.Errno
	if errors.As(err, &errno) {
		return int(errno)
	}
	return 0
}

func failf(what, path string, err error) error {
	return fmt.Errorf("cannot %s '%s' (errno %d)", what, path, errnoOf(err))
}

func join(base, name string) string {
	if base == "" {
		return name
	}
	return base + "/" + name
}

// walk enumerates the directory owned by f, recording every regular file
// into out. display is the path for error messages, rel the DIR-relative
// prefix. f is closed before walk returns.
func walk(f *os.File, display, rel string, out tree) error {
	defer f.Close()

	names, err := f.Readdirnames(-1)
	if err != nil {
		return fmt.Errorf("reading '%s': %w", display, err)
	}

	dirfd := int(f.Fd())
	for _, name := range names {
		var st unix.Stat_t
		if err := unix.Fstatat(dirfd, name, &st, unix.AT_SYMLINK_NOFOLLOW); err != nil {
			if errors.Is(err, unix.ENOENT) {
				continue // vanished mid-walk
			}
			return failf("stat", join(display, name), err)
		}

		switch st.Mode & unix.S_IFMT {
		case unix.S_IFREG:
			out[join(rel, name)] = info{size: st.Size, mtime: st.Mtim.Sec}
		case unix.S_IFDIR:
			childFd, err := unix.Openat(dirfd, name, dirFlags, 0)
			if err != nil {
				if errors.Is(err, unix.ENOENT) {
					continue
				}
				return failf("open directory", join(display, name), err)
			}
			child := os.NewFile(uintptr(childFd), join(display, name))
			if err := walk(child, join(display, name), join(rel, name), out); err != nil {
				return err
			}
		}
		// Symlinks, sockets, pipes, devices: not part of the v0 snapshot.
	}
	return nil
}

func scan(dir string) (tree, error) {
	fd, err := unix.Open(dir, dirFlags, 0)
	if err != nil {
		return nil, failf("open directory", dir, err)
	}
	out := tree{}
	if err := walk(os.NewFile(uintptr(fd), dir), dir, "", out); err != nil {
		return nil, err
	}
	return out, nil
}

func sortedPaths(t tree) []string {
	paths := make([]string, 0, len(t))
	for path := range t {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	return paths
}

// parseSnapshot reads "path size mtime" lines. The path may contain spaces:
// size and mtime are the last two space-separated fields.
func parseSnapshot(text string) (tree, error) {
	out := tree{}
	for i, line := range strings.Split(text, "\n") {
		if line == "" {
			continue
		}
		malformed := fmt.Errorf("malformed snapshot line %d", i+1)

		sp2 := strings.LastIndexByte(line, ' ')
		if sp2 <= 0 {
			return nil, malformed
		}
		sp1 := strings.LastIndexByte(line[:sp2], ' ')
		if sp1 <= 0 {
			return nil, malformed
		}

		size, err1 := strconv.ParseInt(line[sp1+1:sp2], 10, 64)
		mtime, err2 := strconv.ParseInt(line[sp2+1:], 10, 64)
		if err1 != nil || err2 != nil {
			return nil, malformed
		}
		out[line[:sp1]] = info{size: size, mtime: mtime}
	}
	return out, nil
}

func cmdSnapshot(dir string) error {
	t, err := scan(dir)
	if err != nil {
		return err
	}
	for _, path := range sortedPaths(t) {
		fmt.Printf("%s %d %d\n", path, t[path].size, t[path].mtime)
	}
	return nil
}

func cmdDiff(dir, snapshotPath string) error {
	text, err := os.ReadFile(snapshotPath)
	if err != nil {
		return failf("read snapshot", snapshotPath, err)
	}
	before, err := parseSnapshot(string(text))
	if err != nil {
		return err
	}
	after, err := scan(dir)
	if err != nil {
		return err
	}

	// Sorted union of both key sets.
	union := make(map[string]struct{}, len(before)+len(after))
	for path := range before {
		union[path] = struct{}{}
	}
	for path := range after {
		union[path] = struct{}{}
	}
	paths := make([]string, 0, len(union))
	for path := range union {
		paths = append(paths, path)
	}
	sort.Strings(paths)

	added, removed, modified := 0, 0, 0
	for _, path := range paths {
		old, wasThere := before[path]
		now, isThere := after[path]
		switch {
		case !wasThere:
			fmt.Printf("+ %s\n", path)
			added++
		case !isThere:
			fmt.Printf("- %s\n", path)
			removed++
		case old != now:
			fmt.Printf("~ %s\n", path)
			modified++
		}
	}
	fmt.Printf("fwatch: %d added, %d removed, %d modified\n", added, removed, modified)
	return nil
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: fwatch snapshot DIR")
	fmt.Fprintln(os.Stderr, "       fwatch diff DIR SNAPSHOT_FILE")
	os.Exit(2)
}

func main() {
	args := os.Args[1:]
	var err error
	switch {
	case len(args) == 2 && args[0] == "snapshot":
		err = cmdSnapshot(args[1])
	case len(args) == 3 && args[0] == "diff":
		err = cmdDiff(args[1], args[2])
	default:
		usage()
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: error: %v\n", err)
		os.Exit(1)
	}
}
