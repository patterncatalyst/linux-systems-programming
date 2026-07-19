// sysprobe: a labeled syscall specimen — openat+write+close of an unlinked
// temp file, a 10 ms nanosleep, and 16 bytes of getrandom — built to be
// watched under strace(1). Prints "step=<name> ok" per step, then a summary.
package main

import (
	"errors"
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

const payload = "sysprobe scratch payload\n"

// openScratch is step 1: openat(2). Prefer an anonymous O_TMPFILE inode (no
// name ever appears in the directory); fall back to a named create+unlink on
// filesystems that lack O_TMPFILE.
func openScratch(dir string) (int, error) {
	fd, err := unix.Openat(unix.AT_FDCWD, dir,
		unix.O_TMPFILE|unix.O_RDWR|unix.O_CLOEXEC, 0o600)
	if err == nil {
		return fd, nil
	}
	if !errors.Is(err, unix.EOPNOTSUPP) && !errors.Is(err, unix.EISDIR) &&
		!errors.Is(err, unix.EINVAL) {
		return -1, fmt.Errorf("openat %s (O_TMPFILE): %w", dir, err)
	}
	path := fmt.Sprintf("%s/sysprobe.%d", dir, os.Getpid())
	fd, err = unix.Openat(unix.AT_FDCWD, path,
		unix.O_CREAT|unix.O_EXCL|unix.O_RDWR|unix.O_CLOEXEC, 0o600)
	if err != nil {
		return -1, fmt.Errorf("openat %s: %w", path, err)
	}
	if err := unix.Unlink(path); err != nil {
		unix.Close(fd)
		return -1, fmt.Errorf("unlink %s: %w", path, err)
	}
	return fd, nil
}

// writeAll is step 2: write(2), retrying on EINTR and short writes.
func writeAll(fd int, buf []byte) error {
	for len(buf) > 0 {
		n, err := unix.Write(fd, buf)
		if err != nil {
			if errors.Is(err, unix.EINTR) {
				continue
			}
			return fmt.Errorf("write: %w", err)
		}
		buf = buf[n:]
	}
	return nil
}

// sleepMS is step 3: nanosleep(2), resuming with the remaining time on EINTR.
func sleepMS(ms int64) error {
	req := unix.Timespec{Nsec: ms * 1_000_000}
	var rem unix.Timespec
	for {
		err := unix.Nanosleep(&req, &rem)
		if err == nil {
			return nil
		}
		if !errors.Is(err, unix.EINTR) {
			return fmt.Errorf("nanosleep: %w", err)
		}
		req = rem
	}
}

// fillRandom is step 4: getrandom(2), looping over partial reads.
func fillRandom(buf []byte) error {
	for len(buf) > 0 {
		n, err := unix.Getrandom(buf, 0)
		if err != nil {
			if errors.Is(err, unix.EINTR) {
				continue
			}
			return fmt.Errorf("getrandom: %w", err)
		}
		buf = buf[n:]
	}
	return nil
}

func fail(step string, err error) {
	fmt.Fprintf(os.Stderr, "sysprobe: %s: %v\n", step, err)
	os.Exit(1)
}

func main() {
	if len(os.Args) > 1 {
		fmt.Fprintln(os.Stderr, "usage: app")
		os.Exit(2)
	}

	fd, err := openScratch(os.TempDir())
	if err != nil {
		fail("open", err)
	}
	fmt.Println("step=open ok")

	if err := writeAll(fd, []byte(payload)); err != nil {
		fail("write", err)
	}
	fmt.Println("step=write ok")

	if err := unix.Close(fd); err != nil { // close(2) before the sleep
		fail("close", err)
	}

	if err := sleepMS(10); err != nil {
		fail("sleep", err)
	}
	fmt.Println("step=sleep ok")

	entropy := make([]byte, 16)
	if err := fillRandom(entropy); err != nil {
		fail("random", err)
	}
	fmt.Println("step=random ok")

	fmt.Println("sysprobe: 4 steps ok")
}
