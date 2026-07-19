// shmkv v0 — a tiny mmap-backed key/value store (Go).
//
// On-disk format, byte-exact and shared with the C++ and Rust implementations:
//
//	offset 0   6 bytes   magic "SHKV1\0"            (53 48 4b 56 31 00)
//	offset 6   4 bytes   u32 little-endian slot_count
//	offset 10  slot_count x 256-byte slots:
//	           [  0.. 64)  key,   NUL-padded (max 63 bytes, key[0]==0 => empty)
//	           [ 64..256)  value, NUL-padded (max 191 bytes)
//
// The mapping is unix.Mmap with MAP_SHARED: the returned []byte aliases the
// page cache, so writes are visible to every other process mapping the same
// file, and unix.Msync(MS_SYNC) pushes them to disk before we report success.
package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sort"

	"golang.org/x/sys/unix"
)

const (
	headerSize = 10
	keyField   = 64
	valueField = 192
	slotSize   = keyField + valueField
	keyMax     = keyField - 1   // 63: room for one NUL
	valueMax   = valueField - 1 // 191
)

var magic = []byte{'S', 'H', 'K', 'V', '1', 0}

// errUsage makes main print the usage line and exit 2.
var errUsage = errors.New("usage")

// cliError pins the exact stderr line and exit code (byte-identical with the
// C++ and Rust binaries) while still wrapping the underlying cause for %w /
// errors.Is chains.
type cliError struct {
	code int
	msg  string
	err  error
}

func (e *cliError) Error() string {
	if e.err != nil {
		return fmt.Sprintf("%s: %v", e.msg, e.err)
	}
	return e.msg
}

func (e *cliError) Unwrap() error { return e.err }

func failf(code int, cause error, format string, a ...any) error {
	return &cliError{code: code, msg: fmt.Sprintf(format, a...), err: cause}
}

// store owns the fd and the shared mapping; close() unmaps then closes,
// mirroring the RAII types in the C++ and Rust versions.
type store struct {
	fd    int
	data  []byte
	slots uint32
}

func (s *store) close() {
	if s.data != nil {
		_ = unix.Munmap(s.data) // []byte discipline: never touch s.data after this
		s.data = nil
	}
	if s.fd >= 0 {
		_ = unix.Close(s.fd)
		s.fd = -1
	}
}

func mapShared(fd int, size int) ([]byte, error) {
	data, err := unix.Mmap(fd, 0, size, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		return nil, failf(1, fmt.Errorf("mmap: %w", err), "shmkv: mmap failed")
	}
	return data, nil
}

func createStore(path string, slots uint32) (*store, error) {
	// O_TRUNC first so a reused path starts from zero bytes; the following
	// Ftruncate then extends with guaranteed-zero pages, which makes every
	// slot "empty" (key[0] == 0) for free.
	fd, err := unix.Open(path, unix.O_RDWR|unix.O_CREAT|unix.O_TRUNC, 0o644)
	if err != nil {
		return nil, failf(1, fmt.Errorf("open: %w", err), "shmkv: cannot open %s", path)
	}
	size := headerSize + int(slots)*slotSize
	if err := unix.Ftruncate(fd, int64(size)); err != nil {
		unix.Close(fd)
		return nil, failf(1, fmt.Errorf("ftruncate: %w", err), "shmkv: ftruncate failed on %s", path)
	}
	data, err := mapShared(fd, size)
	if err != nil {
		unix.Close(fd)
		return nil, err
	}
	s := &store{fd: fd, data: data, slots: slots}
	copy(s.data, magic)
	binary.LittleEndian.PutUint32(s.data[len(magic):headerSize], slots)
	if err := unix.Msync(s.data, unix.MS_SYNC); err != nil {
		s.close()
		return nil, failf(1, fmt.Errorf("msync: %w", err), "shmkv: msync failed")
	}
	return s, nil
}

func openStore(path string) (*store, error) {
	fd, err := unix.Open(path, unix.O_RDWR, 0)
	if err != nil {
		return nil, failf(1, fmt.Errorf("open: %w", err), "shmkv: cannot open %s", path)
	}
	var st unix.Stat_t
	if err := unix.Fstat(fd, &st); err != nil {
		unix.Close(fd)
		return nil, failf(1, fmt.Errorf("fstat: %w", err), "shmkv: cannot open %s", path)
	}
	size := int(st.Size)
	badStore := func() error {
		return failf(1, nil, "shmkv: %s: not a shmkv v0 store", path)
	}
	if size < headerSize {
		unix.Close(fd)
		return nil, badStore()
	}
	data, err := mapShared(fd, size)
	if err != nil {
		unix.Close(fd)
		return nil, err
	}
	s := &store{fd: fd, data: data, slots: 0}
	if !bytes.Equal(s.data[:len(magic)], magic) {
		s.close()
		return nil, badStore()
	}
	s.slots = binary.LittleEndian.Uint32(s.data[len(magic):headerSize])
	if size != headerSize+int(s.slots)*slotSize {
		s.close()
		return nil, badStore()
	}
	return s, nil
}

func field(b []byte) string {
	if i := bytes.IndexByte(b, 0); i >= 0 {
		return string(b[:i])
	}
	return string(b)
}

func (s *store) slot(i uint32) []byte {
	off := headerSize + int(i)*slotSize
	return s.data[off : off+slotSize]
}

func (s *store) slotKey(i uint32) string   { return field(s.slot(i)[:keyField]) }
func (s *store) slotValue(i uint32) string { return field(s.slot(i)[keyField:]) }

// set: linear probe — overwrite the slot holding key, else claim the first
// empty slot; exit-5 failure when every slot is taken.
func (s *store) set(key, value string) error {
	target := s.slots
	overwrite := false
	firstEmpty := s.slots
	for i := uint32(0); i < s.slots; i++ {
		k := s.slotKey(i)
		if k == key {
			target = i
			overwrite = true
			break
		}
		if k == "" && firstEmpty == s.slots {
			firstEmpty = i
		}
	}
	if !overwrite {
		target = firstEmpty
	}
	if target == s.slots {
		return failf(5, nil, "shmkv: store full (%d slots)", s.slots)
	}
	slot := s.slot(target)
	for i := range slot { // clear any longer previous value
		slot[i] = 0
	}
	copy(slot[:keyField], key)
	copy(slot[keyField:], value)
	if err := unix.Msync(s.data, unix.MS_SYNC); err != nil {
		return failf(1, fmt.Errorf("msync: %w", err), "shmkv: msync failed")
	}
	return nil
}

// parseSlots: digits only, in range [1, u32 max] — identical rules in all
// three languages so the CLIs reject exactly the same inputs.
func parseSlots(text string) (uint32, error) {
	if text == "" || len(text) > 10 {
		return 0, errUsage
	}
	var v uint64
	for _, c := range []byte(text) {
		if c < '0' || c > '9' {
			return 0, errUsage
		}
		v = v*10 + uint64(c-'0')
	}
	if v == 0 || v > 0xffffffff {
		return 0, errUsage
	}
	return uint32(v), nil
}

func cmdCreate(file, slotsText string) error {
	slots, err := parseSlots(slotsText)
	if err != nil {
		return err
	}
	s, err := createStore(file, slots)
	if err != nil {
		return err
	}
	defer s.close()
	fmt.Printf("created %s: %d slots, %d bytes\n", file, slots, headerSize+int(slots)*slotSize)
	return nil
}

func cmdSet(file, key, value string) error {
	if key == "" {
		return failf(2, nil, "shmkv: empty key")
	}
	if len(key) > keyMax {
		return failf(2, nil, "shmkv: key too long (max 63 bytes)")
	}
	if len(value) > valueMax {
		return failf(2, nil, "shmkv: value too long (max 191 bytes)")
	}
	s, err := openStore(file)
	if err != nil {
		return err
	}
	defer s.close()
	if err := s.set(key, value); err != nil {
		return err
	}
	fmt.Printf("set %s\n", key)
	return nil
}

func cmdGet(file, key string) error {
	s, err := openStore(file)
	if err != nil {
		return err
	}
	defer s.close()
	for i := uint32(0); i < s.slots; i++ {
		if s.slotKey(i) == key {
			fmt.Println(s.slotValue(i))
			return nil
		}
	}
	return failf(4, nil, "shmkv: key not found")
}

func cmdDump(file string) error {
	s, err := openStore(file)
	if err != nil {
		return err
	}
	defer s.close()
	type pair struct{ k, v string }
	var pairs []pair
	for i := uint32(0); i < s.slots; i++ {
		if k := s.slotKey(i); k != "" {
			pairs = append(pairs, pair{k, s.slotValue(i)})
		}
	}
	sort.Slice(pairs, func(a, b int) bool { return pairs[a].k < pairs[b].k }) // bytewise
	for _, p := range pairs {
		fmt.Printf("%s=%s\n", p.k, p.v)
	}
	fmt.Fprintf(os.Stderr, "shmkv: %d/%d slots used\n", len(pairs), s.slots)
	return nil
}

func usage() {
	fmt.Fprintln(os.Stderr,
		"usage: shmkv create FILE --slots N | set FILE KEY VALUE | get FILE KEY | dump FILE")
	os.Exit(2)
}

func main() {
	args := os.Args[1:]
	var err error
	switch {
	case len(args) == 4 && args[0] == "create" && args[2] == "--slots":
		err = cmdCreate(args[1], args[3])
	case len(args) == 4 && args[0] == "set":
		err = cmdSet(args[1], args[2], args[3])
	case len(args) == 3 && args[0] == "get":
		err = cmdGet(args[1], args[2])
	case len(args) == 2 && args[0] == "dump":
		err = cmdDump(args[1])
	default:
		usage()
	}
	if err == nil {
		return
	}
	if errors.Is(err, errUsage) {
		usage()
	}
	var ce *cliError
	if errors.As(err, &ce) {
		fmt.Fprintln(os.Stderr, ce.msg)
		os.Exit(ce.code)
	}
	fmt.Fprintf(os.Stderr, "shmkv: %v\n", err)
	os.Exit(1)
}
