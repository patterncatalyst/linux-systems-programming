// copyx SRC DST — file copier that establishes the book's error taxonomy.
//
//	exit 0 — success; "copied <N> bytes" on stdout
//	exit 2 — source-side failure (open/read SRC); "copyx: <reason>" on stderr
//	exit 3 — destination-side failure (open/write/close DST); same shape
//
// EINTR on read(2)/write(2) is retried (the syscall was interrupted before
// completing; reissuing it is the only correct policy), and short writes are
// resumed until the whole buffer is on its way.
package main

import (
	"errors"
	"fmt"
	"os"
	"unicode"

	"golang.org/x/sys/unix"
)

// opError pins a failure to a syscall phase so main can map it onto the
// taxonomy: source-side failures exit 2, destination-side failures exit 3.
type opError struct {
	op   string // "read", "write", "close"; "" means open(2)
	path string
	err  unix.Errno // reachable via errors.Is / errors.As through Unwrap
	exit int
}

func (e *opError) Error() string {
	if e.op == "" {
		return fmt.Sprintf("%s: %s", e.path, strerror(e.err))
	}
	return fmt.Sprintf("%s %s: %s", e.op, e.path, strerror(e.err))
}

func (e *opError) Unwrap() error { return e.err }

// newOpError wraps whatever the unix package returned; the raw errno is
// recovered with errors.As so the chain stays inspectable.
func newOpError(op, path string, err error, exit int) *opError {
	var errno unix.Errno
	if !errors.As(err, &errno) {
		errno = unix.EIO
	}
	return &opError{op: op, path: path, err: errno, exit: exit}
}

// strerror renders an errno the way glibc's strerror(3) does — first letter
// capitalized — so all three implementations print identical reason text.
func strerror(errno unix.Errno) string {
	r := []rune(errno.Error())
	if len(r) > 0 {
		r[0] = unicode.ToUpper(r[0])
	}
	return string(r)
}

// readSome issues read(2) once, retrying EINTR: interrupted means nothing
// was consumed, so reissue the call.
func readSome(fd int, buf []byte) (int, error) {
	for {
		n, err := unix.Read(fd, buf)
		if errors.Is(err, unix.EINTR) {
			continue // interrupted before transferring anything: retry
		}
		return n, err
	}
}

// writeAll pushes the whole buffer through write(2): EINTR restarts the
// call, a short write resumes from where the kernel stopped.
func writeAll(fd int, buf []byte) error {
	for len(buf) > 0 {
		n, err := unix.Write(fd, buf)
		if errors.Is(err, unix.EINTR) {
			continue // interrupted: reissue the same span
		}
		if err != nil {
			return err
		}
		buf = buf[n:]
	}
	return nil
}

func copyFile(srcPath, dstPath string) (int64, error) {
	src, err := unix.Open(srcPath, unix.O_RDONLY|unix.O_CLOEXEC, 0)
	if err != nil {
		return 0, newOpError("", srcPath, fmt.Errorf("open %s: %w", srcPath, err), 2)
	}
	defer unix.Close(src) // read side: nothing actionable in the close result

	dst, err := unix.Open(dstPath, unix.O_WRONLY|unix.O_CREAT|unix.O_TRUNC|unix.O_CLOEXEC, 0o644)
	if err != nil {
		return 0, newOpError("", dstPath, fmt.Errorf("open %s: %w", dstPath, err), 3)
	}

	var total int64
	buf := make([]byte, 64*1024)
	for {
		n, err := readSome(src, buf)
		if err != nil {
			unix.Close(dst)
			return 0, newOpError("read", srcPath, err, 2)
		}
		if n == 0 {
			break // EOF
		}
		if err := writeAll(dst, buf[:n]); err != nil {
			unix.Close(dst)
			return 0, newOpError("write", dstPath, err, 3)
		}
		total += int64(n)
	}

	// The write side must observe close(2): deferred IO errors land here.
	if err := unix.Close(dst); err != nil {
		return 0, newOpError("close", dstPath, err, 3)
	}
	return total, nil
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: copyx SRC DST")
		os.Exit(2)
	}
	n, err := copyFile(os.Args[1], os.Args[2])
	if err != nil {
		fmt.Fprintf(os.Stderr, "copyx: %s\n", err)
		var op *opError
		if errors.As(err, &op) {
			os.Exit(op.exit)
		}
		os.Exit(3)
	}
	fmt.Printf("copied %d bytes\n", n)
}
