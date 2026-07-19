// hello-syscall: print "pid <PID> on <sysname> <release> (<machine>)".
package main

import (
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

func helloLine() (string, error) {
	var uts unix.Utsname
	if err := unix.Uname(&uts); err != nil {
		return "", fmt.Errorf("uname: %w", err)
	}
	return fmt.Sprintf("pid %d on %s %s (%s)",
		unix.Getpid(),
		unix.ByteSliceToString(uts.Sysname[:]),
		unix.ByteSliceToString(uts.Release[:]),
		unix.ByteSliceToString(uts.Machine[:])), nil
}

func main() {
	line, err := helloLine()
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	fmt.Println(line)
}
