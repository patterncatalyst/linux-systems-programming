// shmkv v1 — cross-process coordination over a shared-memory key-value file.
//
// Same SHKV2 layout as the C++ and Rust versions:
//
//	offset  size  field
//	0       8     magic "SHKV2\0\0\0"
//	8       4     seqlock word   (u32; odd = writer in critical section)
//	12      4     futex word     (u32; low 32 bits of the update counter)
//	16      8     update counter (u64; total updates published)
//	24      40    reserved
//	64      512   8 slots x 64 bytes: key[24] NUL-padded, value[40] NUL-padded
//
// Go futex caveat: FUTEX_WAIT is a blocking syscall, so it parks the calling
// OS thread inside the kernel — the goroutine scheduler cannot preempt it.
// The runtime notices the thread is gone (sysmon) and hands its P to another
// thread, so the rest of the program keeps running, but every concurrent
// waiter pins one kernel thread for the duration. Channels and the netpoller
// multiplex thousands of waiters onto a few threads; raw futex waiters do
// not. Fine for one watcher per process, wrong as a general Go pattern.
//
// Go atomics are sequentially consistent, so the seqlock uses atomic ops on
// the seq word around a plain copy of the slot bytes; a torn copy is
// detected by the seq re-check and retried, as in the other languages.
package main

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	fileSize   = 4096
	offSeq     = 8
	offFutex   = 12
	offCounter = 16
	offSlots   = 64
	slotCount  = 8
	keySize    = 24
	valSize    = 40
	slotSize   = 64
	slotsBytes = slotCount * slotSize
)

var magic = [8]byte{'S', 'H', 'K', 'V', '2', 0, 0, 0}

// futex(2) op codes (uapi linux/futex.h); x/sys/unix does not export them.
// No FUTEX_PRIVATE_FLAG: the whole point is waking across processes.
const (
	futexWaitOp = 0 // FUTEX_WAIT
	futexWakeOp = 1 // FUTEX_WAKE
)

// --- the shared file -------------------------------------------------------

type shm struct {
	f *os.File
	m []byte
}

func (s *shm) seq() *uint32     { return (*uint32)(unsafe.Pointer(&s.m[offSeq])) }
func (s *shm) futex() *uint32   { return (*uint32)(unsafe.Pointer(&s.m[offFutex])) }
func (s *shm) counter() *uint64 { return (*uint64)(unsafe.Pointer(&s.m[offCounter])) }

func (s *shm) close() {
	_ = unix.Munmap(s.m)
	_ = s.f.Close()
}

func openShm(path string, create bool) (*shm, error) {
	flags := os.O_RDWR
	if create {
		flags |= os.O_CREATE
	}
	f, err := os.OpenFile(path, flags, 0o644)
	if err != nil {
		return nil, err // "open <path>: no such file or directory"
	}
	if create {
		if err := f.Truncate(fileSize); err != nil {
			f.Close()
			return nil, fmt.Errorf("ftruncate %s: %w", path, err)
		}
	} else {
		st, err := f.Stat()
		if err != nil {
			f.Close()
			return nil, fmt.Errorf("fstat %s: %w", path, err)
		}
		if st.Size() < fileSize {
			f.Close()
			return nil, fmt.Errorf("%s: bad magic (want SHKV2)", path)
		}
	}
	m, err := unix.Mmap(int(f.Fd()), 0, fileSize, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("mmap %s: %w", path, err)
	}
	s := &shm{f: f, m: m}
	if create {
		clear(s.m)
		copy(s.m, magic[:])
	} else if string(s.m[:8]) != string(magic[:]) {
		s.close()
		return nil, fmt.Errorf("%s: bad magic (want SHKV2)", path)
	}
	return s, nil
}

// --- seqlock ---------------------------------------------------------------

func seqlockWrite(s *shm, mutate func()) {
	atomic.AddUint32(s.seq(), 1) // odd: writer active
	mutate()
	atomic.AddUint32(s.seq(), 1) // even: consistent again
}

type snapshot struct {
	counter uint64
	slots   [slotsBytes]byte
}

func seqlockRead(s *shm) (snapshot, error) {
	for attempt := 0; attempt < 1_000_000; attempt++ {
		s1 := atomic.LoadUint32(s.seq())
		if s1&1 != 0 { // writer mid-update: retry
			runtime.Gosched()
			continue
		}
		var snap snapshot
		snap.counter = atomic.LoadUint64(s.counter())
		copy(snap.slots[:], s.m[offSlots:offSlots+slotsBytes])
		if atomic.LoadUint32(s.seq()) == s1 {
			return snap, nil
		}
	}
	return snapshot{}, fmt.Errorf("seqlock read livelocked")
}

// --- futex -----------------------------------------------------------------

// Shared (non-PRIVATE) futex: the kernel keys on the file's inode, so waiters
// and wakers in different processes rendezvous through the same mapping.
func futexWait(addr *uint32, expected uint32, timeout time.Duration) {
	ts := unix.NsecToTimespec(timeout.Nanoseconds())
	// EAGAIN (word already moved on), ETIMEDOUT and EINTR are all normal
	// here: the caller re-checks shared state before waiting again.
	_, _, _ = unix.Syscall6(unix.SYS_FUTEX, uintptr(unsafe.Pointer(addr)),
		futexWaitOp, uintptr(expected), uintptr(unsafe.Pointer(&ts)), 0, 0)
}

func futexWakeAll(addr *uint32) {
	_, _, _ = unix.Syscall6(unix.SYS_FUTEX, uintptr(unsafe.Pointer(addr)),
		futexWakeOp, uintptr(int(^uint32(0)>>1)), 0, 0, 0)
}

// --- POSIX message queue via raw syscalls ----------------------------------
// (glibc's mq_open lives in librt; the kernel interface is four syscalls.
// The kernel wants the name WITHOUT the leading slash glibc requires.)

type mqAttr struct {
	Flags   int64
	Maxmsg  int64
	Msgsize int64
	Curmsgs int64
	_       [4]int64
}

func mqOpen(name string) (int, error) {
	nptr, err := unix.BytePtrFromString(name)
	if err != nil {
		return -1, err
	}
	attr := mqAttr{Maxmsg: 8, Msgsize: 8}
	fd, _, errno := unix.Syscall6(unix.SYS_MQ_OPEN, uintptr(unsafe.Pointer(nptr)),
		uintptr(unix.O_CREAT|unix.O_EXCL|unix.O_RDWR), 0o600,
		uintptr(unsafe.Pointer(&attr)), 0, 0)
	if errno != 0 {
		return -1, fmt.Errorf("mq_open /%s: %w", name, errno)
	}
	return int(fd), nil
}

func mqUnlink(name string) {
	nptr, err := unix.BytePtrFromString(name)
	if err != nil {
		return
	}
	_, _, _ = unix.Syscall(unix.SYS_MQ_UNLINK, uintptr(unsafe.Pointer(nptr)), 0, 0)
}

func mqDeadline() unix.Timespec {
	var ts unix.Timespec
	_ = unix.ClockGettime(unix.CLOCK_REALTIME, &ts)
	ts.Sec += 10
	return ts
}

func mqSend(fd int, id uint64) error {
	var buf [8]byte
	for i := 0; i < 8; i++ {
		buf[i] = byte(id >> (8 * i)) // little-endian, same as the C++/Rust memcpy
	}
	ts := mqDeadline()
	_, _, errno := unix.Syscall6(unix.SYS_MQ_TIMEDSEND, uintptr(fd),
		uintptr(unsafe.Pointer(&buf[0])), 8, 0, uintptr(unsafe.Pointer(&ts)), 0)
	if errno != 0 {
		return fmt.Errorf("mq_timedsend: %w", errno)
	}
	return nil
}

func mqReceive(fd int) (uint64, error) {
	var buf [8]byte
	for {
		ts := mqDeadline()
		n, _, errno := unix.Syscall6(unix.SYS_MQ_TIMEDRECEIVE, uintptr(fd),
			uintptr(unsafe.Pointer(&buf[0])), 8, 0, uintptr(unsafe.Pointer(&ts)), 0)
		if errno == unix.EINTR {
			continue
		}
		if errno != 0 {
			return 0, fmt.Errorf("mq_timedreceive: %w", errno)
		}
		if n != 8 {
			return 0, fmt.Errorf("mq_timedreceive: short message (%d bytes)", n)
		}
		var id uint64
		for i := 0; i < 8; i++ {
			id |= uint64(buf[i]) << (8 * i)
		}
		return id, nil
	}
}

// --- helpers ---------------------------------------------------------------

func cstr(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}

func writeSlot(s *shm, id uint64, key, val string) {
	off := offSlots + int((id-1)%slotCount)*slotSize
	slot := s.m[off : off+slotSize]
	clear(slot)
	copy(slot[:keySize-1], key)
	copy(slot[keySize:keySize+valSize-1], val)
}

func readSlot(snap *snapshot, id uint64) (string, string) {
	off := int((id-1)%slotCount) * slotSize
	slot := snap.slots[off : off+slotSize]
	return cstr(slot[:keySize]), cstr(slot[keySize:])
}

// --- serve -----------------------------------------------------------------

func cmdServe(path string, updates, intervalMs uint64) error {
	s, err := openShm(path, true)
	if err != nil {
		return err
	}
	defer s.close()
	fmt.Printf("serve: file=%s updates=%d interval_ms=%d\n", path, updates, intervalMs)
	for k := uint64(1); k <= updates; k++ {
		time.Sleep(time.Duration(intervalMs) * time.Millisecond)
		key := fmt.Sprintf("k%d", k)
		val := fmt.Sprintf("value-%d", k)
		seqlockWrite(s, func() {
			writeSlot(s, k, key, val)
			atomic.StoreUint64(s.counter(), k)
		})
		atomic.StoreUint32(s.futex(), uint32(k))
		futexWakeAll(s.futex())
		fmt.Printf("published update %d: %s=%s\n", k, key, val)
	}
	fmt.Printf("serve: complete updates=%d\n", updates)
	return nil
}

// --- watch -----------------------------------------------------------------

func cmdWatch(path string, events uint64) error {
	s, err := openShm(path, false)
	if err != nil {
		return err
	}
	defer s.close()
	fmt.Printf("watch: file=%s events=%d\n", path, events)
	deadline := time.Now().Add(30 * time.Second)
	var last, printed uint64
	for printed < events {
		if time.Now().After(deadline) {
			return fmt.Errorf("watch: timed out waiting for updates")
		}
		snap, err := seqlockRead(s)
		if err != nil {
			return err
		}
		if snap.counter > last {
			// The slot ring holds the last slotCount updates, so a watcher
			// that slept through a wake can still back-fill missed ids.
			for id := last + 1; id <= snap.counter && printed < events; id++ {
				key, val := readSlot(&snap, id)
				fmt.Printf("observed update %d: %s=%s\n", id, key, val)
				printed++
			}
			last = snap.counter
			continue
		}
		w := atomic.LoadUint32(s.futex())
		if w != uint32(last) {
			continue // moved on while we looked: re-read now
		}
		futexWait(s.futex(), w, 2*time.Second)
	}
	fmt.Printf("watch: complete events=%d\n", events)
	return nil
}

// --- bench -----------------------------------------------------------------

func cmdBench(path string, rounds uint64, channel string) error {
	s, err := openShm(path, true)
	if err != nil {
		return err
	}
	defer s.close()

	mqFd := -1
	if channel == "mq" {
		name := fmt.Sprintf("shmkv-bench-%d", os.Getpid())
		mqFd, err = mqOpen(name)
		if err != nil {
			return err
		}
		defer func() {
			unix.Close(mqFd)
			mqUnlink(name)
		}()
	}

	sendTs := make([]time.Time, rounds)
	recvTs := make([]time.Time, rounds)
	var ack atomic.Uint64
	done := make(chan error, 1)

	go func() {
		for i := uint64(1); i <= rounds; i++ {
			switch channel {
			case "futex":
				for {
					w := atomic.LoadUint32(s.futex())
					if w >= uint32(i) {
						break
					}
					futexWait(s.futex(), w, 200*time.Millisecond)
				}
			case "poll":
				// Sleep FIRST, then look: by construction every observation
				// costs at least one full 1 ms nap — what futex wakes avoid.
				for {
					time.Sleep(time.Millisecond)
					if atomic.LoadUint64(s.counter()) >= i {
						break
					}
				}
			case "mq":
				if _, err := mqReceive(mqFd); err != nil {
					done <- err
					return
				}
			}
			recvTs[i-1] = time.Now()
			ack.Store(i)
		}
		done <- nil
	}()

	spinDeadline := time.Now().Add(30 * time.Second)
	for i := uint64(1); i <= rounds; i++ {
		// Spin until the consumer acknowledged round i-1 so rounds never
		// overlap (overlap would measure queueing, not wakeup latency).
		for ack.Load() != i-1 {
			if time.Now().After(spinDeadline) {
				return fmt.Errorf("bench: consumer stalled")
			}
			runtime.Gosched()
		}
		sendTs[i-1] = time.Now()
		if channel == "mq" {
			if err := mqSend(mqFd, i); err != nil {
				return err
			}
		} else {
			k := i
			seqlockWrite(s, func() { atomic.StoreUint64(s.counter(), k) })
			atomic.StoreUint32(s.futex(), uint32(k))
			if channel == "futex" {
				futexWakeAll(s.futex())
			}
		}
	}

	select {
	case err := <-done:
		if err != nil {
			return err
		}
	case <-time.After(30 * time.Second):
		return fmt.Errorf("bench: consumer did not finish")
	}

	us := make([]int64, rounds)
	for i := range us {
		d := recvTs[i].Sub(sendTs[i]).Microseconds()
		if d < 0 {
			d = 0
		}
		us[i] = d
	}
	sort.Slice(us, func(a, b int) bool { return us[a] < us[b] })
	pct := func(p uint64) int64 {
		idx := rounds * p / 100
		if idx >= rounds {
			idx = rounds - 1
		}
		return us[idx]
	}
	fmt.Printf("bench: channel=%s rounds=%d p50_us=%d p99_us=%d\n",
		channel, rounds, pct(50), pct(99))
	return nil
}

// --- CLI -------------------------------------------------------------------

func usage() {
	fmt.Fprintln(os.Stderr, "usage: shmkv serve FILE [--updates N] [--interval-ms T]")
	fmt.Fprintln(os.Stderr, "       shmkv watch FILE [--events N]")
	fmt.Fprintln(os.Stderr, "       shmkv bench FILE [--rounds N] [--channel futex|mq|poll]")
	os.Exit(2)
}

func parseU64(s string) uint64 {
	v, err := strconv.ParseUint(s, 10, 64)
	if err != nil || v == 0 || v > 1_000_000 {
		usage()
	}
	return v
}

func main() {
	args := os.Args[1:]
	if len(args) < 2 {
		usage()
	}
	cmd, file := args[0], args[1]

	updates, intervalMs, events, rounds := uint64(8), uint64(100), uint64(4), uint64(100)
	channel := "futex"
	for i := 2; i < len(args); i += 2 {
		if i+1 >= len(args) {
			usage()
		}
		flag, val := args[i], args[i+1]
		switch {
		case flag == "--updates" && cmd == "serve":
			updates = parseU64(val)
		case flag == "--interval-ms" && cmd == "serve":
			intervalMs = parseU64(val)
		case flag == "--events" && cmd == "watch":
			events = parseU64(val)
		case flag == "--rounds" && cmd == "bench":
			rounds = parseU64(val)
		case flag == "--channel" && cmd == "bench":
			channel = val
		default:
			usage()
		}
	}

	var err error
	switch cmd {
	case "serve":
		err = cmdServe(file, updates, intervalMs)
	case "watch":
		err = cmdWatch(file, events)
	case "bench":
		if channel != "futex" && channel != "mq" && channel != "poll" {
			usage()
		}
		err = cmdBench(file, rounds, channel)
	default:
		usage()
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "shmkv: %v\n", err)
		os.Exit(1)
	}
}
