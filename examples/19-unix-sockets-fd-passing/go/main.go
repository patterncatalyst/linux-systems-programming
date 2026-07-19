// pmon v5 — a process supervisor with a UNIX-socket control plane.
//
// `pmon supervise` spawns a child command (its own process group,
// stdout+stderr appended to a log file), restarts it 300 ms after every
// exit, and serves a SOCK_STREAM control socket. `pmon pmctl` is the client
// side of the same binary:
//
//	status  -> "child=<pid> uptime=<s>s restarts=<n>"
//	stop    -> "stopping", then the supervisor tears down and exits 0
//	logfd   -> the supervisor passes its OPEN log file descriptor through an
//	           SCM_RIGHTS control message; pmctl reads the last 3 lines
//	           straight off the received fd ("via-fd: ..."), never touching
//	           the path.
//
// Where C++ and Rust build the cmsg by hand / via nix, Go rides
// net.UnixConn: WriteMsgUnix with unix.UnixRights on the way out,
// ReadMsgUnix + unix.ParseSocketControlMessage + unix.ParseUnixRights on the
// way in. The reaper is a goroutine; restart backoff and stop are a select.
package main

import (
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

const (
	restartBackoff = 300 * time.Millisecond
	maxLogRead     = 1 << 20 // pmctl reads at most 1 MiB of log
)

func usage() int {
	fmt.Fprintln(os.Stderr, "usage: pmon supervise --ctl SOCK --log FILE -- CMD [ARG...]")
	fmt.Fprintln(os.Stderr, "       pmon pmctl --ctl SOCK <status|stop|logfd>")
	return 2
}

// ---------------------------------------------------------------------------
// Child lifecycle
// ---------------------------------------------------------------------------

type state struct {
	mu       sync.Mutex
	child    int // pid of the current child
	started  time.Time
	restarts int
	stopping bool
}

// spawnChild starts argv in its own process group with stdout+stderr going to
// the (O_APPEND) log file — the fd is inherited directly, no pipe goroutine.
func spawnChild(argv []string, logFile *os.File) (*exec.Cmd, error) {
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Stdout = logFile
	cmd.Stderr = logFile
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("exec %s: %w", argv[0], err)
	}
	return cmd, nil
}

func exitCodeOf(err error) int {
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		if ws, ok := ee.Sys().(syscall.WaitStatus); ok {
			if ws.Signaled() {
				return 128 + int(ws.Signal())
			}
			return ws.ExitStatus()
		}
	}
	if err != nil {
		return 1
	}
	return 0
}

func logLine(logFile *os.File, line string) {
	// One write per line: O_APPEND makes it an atomic append.
	_, _ = logFile.WriteString(line + "\n")
}

// reaper waits for the current child; unless we are stopping, backs off
// 300 ms (or bails on stopCh) and respawns. Closes done when the final child
// is collected.
func reaper(s *state, cmd *exec.Cmd, argv []string, logFile *os.File, stopCh <-chan struct{}, done chan<- struct{}) {
	defer close(done)
	for {
		pid := cmd.Process.Pid
		err := cmd.Wait()
		s.mu.Lock()
		stopping := s.stopping
		s.mu.Unlock()
		if stopping {
			return
		}
		fmt.Fprintf(os.Stderr, "pmon: child pid=%d exited status=%d\n", pid, exitCodeOf(err))
		select {
		case <-stopCh:
			return
		case <-time.After(restartBackoff):
		}
		next, serr := spawnChild(argv, logFile)
		if serr != nil {
			fmt.Fprintf(os.Stderr, "pmon: error: %v\n", serr)
			return
		}
		s.mu.Lock()
		stopping = s.stopping
		var restartsNow int
		if !stopping {
			s.child = next.Process.Pid
			s.started = time.Now()
			s.restarts++
			restartsNow = s.restarts
		}
		s.mu.Unlock()
		if stopping { // stop raced with the respawn: undo it
			_ = unix.Kill(-next.Process.Pid, unix.SIGTERM)
			_ = next.Wait()
			return
		}
		fmt.Fprintf(os.Stderr, "pmon: restart %d child pid=%d\n", restartsNow, next.Process.Pid)
		logLine(logFile, fmt.Sprintf("pmon: start child pid=%d", next.Process.Pid))
		cmd = next
	}
}

// ---------------------------------------------------------------------------
// supervise
// ---------------------------------------------------------------------------

func peerCreds(conn *net.UnixConn) {
	raw, err := conn.SyscallConn()
	if err != nil {
		return
	}
	_ = raw.Control(func(fd uintptr) {
		cred, cerr := unix.GetsockoptUcred(int(fd), unix.SOL_SOCKET, unix.SO_PEERCRED)
		if cerr == nil {
			fmt.Fprintf(os.Stderr, "pmon: ctl connect uid=%d pid=%d\n", cred.Uid, cred.Pid)
		}
	})
}

func readCommand(conn *net.UnixConn) string {
	var cmd []byte
	one := make([]byte, 1)
	for len(cmd) < 64 {
		n, err := conn.Read(one)
		if n <= 0 || err != nil || one[0] == '\n' {
			break
		}
		cmd = append(cmd, one[0])
	}
	return string(cmd)
}

func cmdSupervise(ctl, logPath string, childArgv []string) error {
	// O_RDWR, not O_WRONLY: the fd handed out via SCM_RIGHTS shares this open
	// file description, and pmctl must be able to read from it.
	logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o644)
	if err != nil {
		return fmt.Errorf("open %s: %w", logPath, err)
	}
	defer logFile.Close()

	_ = os.Remove(ctl) // stale socket from a previous run
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: ctl, Net: "unix"})
	if err != nil {
		return fmt.Errorf("listen %s: %w", ctl, err)
	}
	// The stop path removes the socket explicitly; keep Go from doing it early.
	listener.SetUnlinkOnClose(false)
	defer listener.Close()
	fmt.Fprintf(os.Stderr, "pmon: listening on %s\n", ctl)

	first, err := spawnChild(childArgv, logFile)
	if err != nil {
		_ = os.Remove(ctl)
		return err
	}
	fmt.Fprintf(os.Stderr, "pmon: started child pid=%d\n", first.Process.Pid)
	logLine(logFile, fmt.Sprintf("pmon: start child pid=%d", first.Process.Pid))

	s := &state{child: first.Process.Pid, started: time.Now()}
	stopCh := make(chan struct{})
	done := make(chan struct{})
	go reaper(s, first, childArgv, logFile, stopCh, done)

	for {
		conn, aerr := listener.AcceptUnix()
		if aerr != nil {
			return fmt.Errorf("accept: %w", aerr)
		}
		peerCreds(conn)
		cmd := readCommand(conn)
		switch cmd {
		case "status":
			s.mu.Lock()
			reply := fmt.Sprintf("child=%d uptime=%ds restarts=%d\n",
				s.child, int(time.Since(s.started)/time.Second), s.restarts)
			s.mu.Unlock()
			_, _ = conn.Write([]byte(reply))
			conn.Close()
		case "logfd":
			rights := unix.UnixRights(int(logFile.Fd()))
			if _, _, werr := conn.WriteMsgUnix([]byte("ok\n"), rights, nil); werr != nil {
				fmt.Fprintf(os.Stderr, "pmon: sendmsg: %v\n", werr)
			}
			conn.Close()
		case "stop":
			_, _ = conn.Write([]byte("stopping\n"))
			conn.Close() // deliver the reply before tearing down
			fmt.Fprintln(os.Stderr, "pmon: stopping")
			s.mu.Lock()
			s.stopping = true
			child := s.child
			s.mu.Unlock()
			close(stopCh)
			if child > 0 {
				_ = unix.Kill(-child, unix.SIGTERM) // whole process group; ESRCH is fine
			}
			<-done
			_ = os.Remove(ctl)
			return nil
		default:
			_, _ = conn.Write([]byte("err unknown command\n"))
			conn.Close()
		}
	}
}

// ---------------------------------------------------------------------------
// pmctl
// ---------------------------------------------------------------------------

func recvLogFd(conn *net.UnixConn) (int, error) {
	buf := make([]byte, 16)
	oob := make([]byte, unix.CmsgSpace(4))
	_, oobn, _, _, err := conn.ReadMsgUnix(buf, oob)
	if err != nil {
		return -1, fmt.Errorf("recvmsg: %w", err)
	}
	msgs, err := unix.ParseSocketControlMessage(oob[:oobn])
	if err != nil {
		return -1, fmt.Errorf("parse cmsg: %w", err)
	}
	for _, m := range msgs {
		fds, ferr := unix.ParseUnixRights(&m)
		if ferr == nil && len(fds) > 0 {
			return fds[0], nil
		}
	}
	return -1, errors.New("no SCM_RIGHTS control message in reply")
}

func logTailViaFd(fd int, count int) ([]string, error) {
	var data []byte
	chunk := make([]byte, 4096)
	var off int64
	for len(data) < maxLogRead {
		n, err := unix.Pread(fd, chunk, off)
		if err != nil {
			return nil, fmt.Errorf("pread: %w", err)
		}
		if n == 0 {
			break
		}
		data = append(data, chunk[:n]...)
		off += int64(n)
	}
	text := strings.TrimSuffix(string(data), "\n")
	if text == "" {
		return nil, nil
	}
	lines := strings.Split(text, "\n")
	if len(lines) > count {
		lines = lines[len(lines)-count:]
	}
	return lines, nil
}

func cmdPmctl(ctl, action string) error {
	addr := &net.UnixAddr{Name: ctl, Net: "unix"}
	conn, err := net.DialUnix("unix", nil, addr)
	if err != nil {
		return fmt.Errorf("connect %s: %w", ctl, err)
	}
	defer conn.Close()
	if _, err := conn.Write([]byte(action + "\n")); err != nil {
		return fmt.Errorf("write %s: %w", ctl, err)
	}

	if action == "logfd" {
		fd, ferr := recvLogFd(conn)
		if ferr != nil {
			return ferr
		}
		defer unix.Close(fd)
		lines, terr := logTailViaFd(fd, 3)
		if terr != nil {
			return terr
		}
		for _, line := range lines {
			fmt.Printf("via-fd: %s\n", line)
		}
		return nil
	}

	// status / stop: the supervisor replies with one line and closes.
	var reply []byte
	buf := make([]byte, 256)
	for {
		n, rerr := conn.Read(buf)
		if n > 0 {
			reply = append(reply, buf[:n]...)
		}
		if rerr != nil {
			break
		}
	}
	text := strings.TrimRight(string(reply), "\n")
	if text == "" {
		return errors.New("empty reply from supervisor")
	}
	if strings.HasPrefix(text, "err ") {
		return errors.New(strings.TrimPrefix(text, "err "))
	}
	fmt.Println(text)
	return nil
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		os.Exit(usage())
	}

	switch args[0] {
	case "supervise":
		var ctl, logPath string
		var child []string
		i := 1
		for i < len(args) {
			switch {
			case args[i] == "--ctl" && i+1 < len(args):
				ctl = args[i+1]
				i += 2
			case args[i] == "--log" && i+1 < len(args):
				logPath = args[i+1]
				i += 2
			case args[i] == "--":
				child = args[i+1:]
				i = len(args)
			default:
				os.Exit(usage())
			}
		}
		if ctl == "" || logPath == "" || len(child) == 0 {
			os.Exit(usage())
		}
		if err := cmdSupervise(ctl, logPath, child); err != nil {
			fmt.Fprintf(os.Stderr, "pmon: error: %v\n", err)
			os.Exit(1)
		}
	case "pmctl":
		var ctl, action string
		for i := 1; i < len(args); i++ {
			switch {
			case args[i] == "--ctl" && i+1 < len(args):
				ctl = args[i+1]
				i++
			case action == "":
				action = args[i]
			default:
				os.Exit(usage())
			}
		}
		if ctl == "" || (action != "status" && action != "stop" && action != "logfd") {
			os.Exit(usage())
		}
		if err := cmdPmctl(ctl, action); err != nil {
			fmt.Fprintf(os.Stderr, "pmctl: error: %v\n", err)
			os.Exit(1)
		}
	default:
		os.Exit(usage())
	}
}
