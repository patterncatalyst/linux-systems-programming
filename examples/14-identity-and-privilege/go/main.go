// pmon v3 — identity & privilege (chapter 14).
//
// Subcommands:
//
//	pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]
//	pmon bindprobe [--port 80]
//
// Go note — why this differs from the C++/Rust in-process drop:
//
// A Go program is multithreaded before main() runs, and setresuid(2) /
// capset(2) act on the CALLING thread only. There is no portable way to make
// every runtime thread drop privilege in lockstep from user code, so doing the
// dance in-process is a footgun. The idiomatic Go answer is to let the runtime
// do it in the child between fork and exec: syscall.SysProcAttr carries a
// Credential (the setgid/setgroups/setresuid) and AmbientCaps (the
// PR_SET_KEEPCAPS -> capset -> PR_CAP_AMBIENT_RAISE sequence) which the fork/exec
// child applies in the correct order for us. Same observable result as the raw
// C++/Rust path — the ordering lives in the standard library instead of here.
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/user"
	"strconv"
	"syscall"

	"golang.org/x/sys/unix"
)

// errnoText reconstructs libc's strerror(3) spelling (capitalised first letter,
// e.g. "Permission denied") from Go's lower-cased errno string, so the failure
// line is byte-for-byte identical to the C++ and Rust builds.
func errnoText(err error) string {
	s := err.Error()
	if s == "" {
		return s
	}
	b := []byte(s)
	if b[0] >= 'a' && b[0] <= 'z' {
		b[0] -= 'a' - 'A'
	}
	return string(b)
}

// ---- bindprobe -----------------------------------------------------------
//
// Bind a TCP socket on <port> and report the effective uid. Binding a port
// below 1024 is impossible for a non-root process without CAP_NET_BIND_SERVICE;
// success at uid!=0 on :80 proves the ambient cap survived the drop+exec.
func cmdBindprobe(args []string) int {
	port := 80
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--port":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "bindprobe: --port needs a value")
				return 2
			}
			p, err := strconv.Atoi(args[i+1])
			if err != nil {
				fmt.Fprintf(os.Stderr, "bindprobe: bad port: %s\n", args[i+1])
				return 2
			}
			port = p
			i++
		default:
			fmt.Fprintf(os.Stderr, "bindprobe: unexpected argument: %s\n", args[i])
			return 2
		}
	}

	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_STREAM, 0)
	if err != nil {
		fmt.Fprintf(os.Stderr, "bindprobe: socket: %s\n", errnoText(err))
		return 1
	}
	defer unix.Close(fd)
	_ = unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)

	sa := &unix.SockaddrInet4{Port: port} // Addr left zero => INADDR_ANY
	if err := unix.Bind(fd, sa); err != nil {
		fmt.Fprintf(os.Stderr, "bindprobe: bind :%d: %s\n", port, errnoText(err))
		return 3
	}
	fmt.Printf("bindprobe: uid=%d bound :%d\n", os.Getuid(), port)
	return 0
}

// ---- drop ----------------------------------------------------------------

func cmdDrop(args []string) int {
	var name string
	keepCap := false
	var cmd []string
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--user":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "drop: --user needs a value")
				return 2
			}
			name = args[i+1]
			i++
		case "--keep-cap":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "drop: --keep-cap needs a value")
				return 2
			}
			if args[i+1] != "net_bind_service" {
				fmt.Fprintf(os.Stderr, "drop: unsupported --keep-cap: %s\n", args[i+1])
				return 2
			}
			keepCap = true
			i++
		case "--":
			cmd = args[i+1:]
			i = len(args)
		default:
			fmt.Fprintf(os.Stderr, "drop: unexpected argument: %s\n", args[i])
			return 2
		}
	}
	if name == "" {
		fmt.Fprintln(os.Stderr, "drop: --user <name> is required")
		return 2
	}
	if len(cmd) == 0 {
		fmt.Fprintln(os.Stderr, "drop: missing -- CMD")
		return 2
	}
	if os.Getuid() != 0 {
		fmt.Fprintln(os.Stderr, "drop: must run as root")
		return 1
	}

	u, err := user.Lookup(name)
	if err != nil {
		fmt.Fprintf(os.Stderr, "drop: unknown user: %s\n", name)
		return 1
	}
	uid, _ := strconv.Atoi(u.Uid)
	gid, _ := strconv.Atoi(u.Gid)

	c := exec.Command(cmd[0], cmd[1:]...)
	c.Stdin, c.Stdout, c.Stderr = os.Stdin, os.Stdout, os.Stderr
	// The runtime's fork/exec child applies these in the kernel-correct order:
	// PR_SET_KEEPCAPS, setgroups/setgid, setuid, capset, PR_CAP_AMBIENT_RAISE.
	c.SysProcAttr = &syscall.SysProcAttr{
		Credential: &syscall.Credential{Uid: uint32(uid), Gid: uint32(gid)},
	}
	if keepCap {
		c.SysProcAttr.AmbientCaps = []uintptr{unix.CAP_NET_BIND_SERVICE}
	}

	if err := c.Run(); err != nil {
		var ee *exec.ExitError
		if errors.As(err, &ee) {
			return ee.ExitCode() // propagate CMD's own exit status (e.g. 3)
		}
		fmt.Fprintf(os.Stderr, "drop: %v\n", err)
		return 1
	}
	return 0
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage:")
	fmt.Fprintln(os.Stderr, "  pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]")
	fmt.Fprintln(os.Stderr, "  pmon bindprobe [--port 80]")
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	var code int
	switch os.Args[1] {
	case "drop":
		code = cmdDrop(os.Args[2:])
	case "bindprobe":
		code = cmdBindprobe(os.Args[2:])
	default:
		usage()
		code = 2
	}
	os.Exit(code)
}
