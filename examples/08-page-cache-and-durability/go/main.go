// iobench: page cache vs durability — buffered writes, periodic fdatasync(2),
// and O_DIRECT, with identical CLI and output across the three languages.
//
//	iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE
//
// Writes M MiB (default 64) in 64 KiB blocks.
//
//	buffered     plain write(2)s; reports write time, then a second line
//	             "fsync_ms=<t>" for the closing fdatasync(2)
//	fsync-every  fdatasync(2) every N blocks (default 8), timed end to end
//	direct       O_DIRECT with 4096-aligned buffers; if open(2) fails with
//	             EINVAL, prints "direct: unsupported on this filesystem"
//	             and exits 4
package main

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	blockSize = 64 * 1024
	alignment = 4096
	mib       = 1024 * 1024
	usageLine = "usage: iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE"
)

type options struct {
	mode   string
	every  uint64
	sizeMB uint64
	file   string
}

var errUsage = errors.New("usage")

func parsePositive(s string) (uint64, error) {
	n, err := strconv.ParseUint(s, 10, 64)
	if err != nil || n == 0 {
		return 0, errUsage
	}
	return n, nil
}

func parseArgs(args []string) (options, error) {
	opt := options{every: 8, sizeMB: 64}
	haveMode := false
	for i := 0; i < len(args); i++ {
		a := args[i]
		switch a {
		case "--mode", "--every", "--size-mb":
			if i+1 >= len(args) {
				return opt, errUsage
			}
			i++
			v := args[i]
			switch a {
			case "--mode":
				if v != "buffered" && v != "fsync-every" && v != "direct" {
					return opt, errUsage
				}
				opt.mode = v
				haveMode = true
			case "--every":
				n, err := parsePositive(v)
				if err != nil {
					return opt, err
				}
				opt.every = n
			case "--size-mb":
				n, err := parsePositive(v)
				if err != nil {
					return opt, err
				}
				opt.sizeMB = n
			}
		default:
			if strings.HasPrefix(a, "--") || opt.file != "" {
				return opt, errUsage
			}
			opt.file = a
		}
	}
	if !haveMode || opt.file == "" {
		return opt, errUsage
	}
	return opt, nil
}

// alignedBlock returns one 64 KiB block whose base address is 4096-aligned
// (required by O_DIRECT, harmless otherwise). The Go heap does not move
// allocations, so the alignment computed here stays valid.
func alignedBlock() []byte {
	raw := make([]byte, blockSize+alignment)
	addr := uintptr(unsafe.Pointer(&raw[0]))
	off := int((alignment - addr%alignment) % alignment)
	block := raw[off : off+blockSize]
	for i := range block {
		block[i] = byte(i)
	}
	return block
}

// writeAll retries write(2) across short writes and EINTR.
func writeAll(fd int, data []byte) error {
	for len(data) > 0 {
		n, err := unix.Write(fd, data)
		if err != nil {
			if errors.Is(err, unix.EINTR) {
				continue
			}
			return fmt.Errorf("write: %w", err)
		}
		data = data[n:]
	}
	return nil
}

func datasync(fd int) error {
	if err := unix.Fdatasync(fd); err != nil {
		return fmt.Errorf("fdatasync: %w", err)
	}
	return nil
}

// clampNS keeps the throughput division defined on a sub-nanosecond clock read.
func clampNS(d time.Duration) int64 {
	if ns := d.Nanoseconds(); ns > 0 {
		return ns
	}
	return 1
}

func run(opt options) (int, error) {
	flags := unix.O_WRONLY | unix.O_CREAT | unix.O_TRUNC
	if opt.mode == "direct" {
		flags |= unix.O_DIRECT
	}
	fd, err := unix.Open(opt.file, flags, 0o644)
	if err != nil {
		if opt.mode == "direct" && errors.Is(err, unix.EINVAL) {
			fmt.Fprintln(os.Stderr, "direct: unsupported on this filesystem")
			return 4, nil
		}
		return 1, fmt.Errorf("open %s: %w", opt.file, err)
	}
	defer unix.Close(fd)

	block := alignedBlock()
	nblocks := opt.sizeMB * (mib / blockSize)
	bytes := opt.sizeMB * mib

	t0 := time.Now()
	for i := uint64(0); i < nblocks; i++ {
		if err := writeAll(fd, block); err != nil {
			return 1, fmt.Errorf("%s: %w", opt.file, err)
		}
		if opt.mode == "fsync-every" && (i+1)%opt.every == 0 {
			if err := datasync(fd); err != nil {
				return 1, fmt.Errorf("%s: %w", opt.file, err)
			}
		}
	}
	if opt.mode == "fsync-every" && nblocks%opt.every != 0 {
		if err := datasync(fd); err != nil {
			return 1, fmt.Errorf("%s: %w", opt.file, err)
		}
	}
	ns := clampNS(time.Since(t0))

	mibPerS := (float64(bytes) / float64(mib)) / (float64(ns) / 1e9)
	fmt.Printf("mode=%s bytes=%d ms=%d MiB/s=%.1f\n", opt.mode, bytes, ns/1e6, mibPerS)

	if opt.mode == "buffered" {
		t2 := time.Now()
		if err := datasync(fd); err != nil {
			return 1, fmt.Errorf("%s: %w", opt.file, err)
		}
		fmt.Printf("fsync_ms=%d\n", clampNS(time.Since(t2))/1e6)
	}
	return 0, nil
}

func main() {
	opt, err := parseArgs(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, usageLine)
		os.Exit(2)
	}
	code, err := run(opt)
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
	}
	os.Exit(code)
}
