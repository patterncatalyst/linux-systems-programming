// app -- bugfarm scenario 1: a crashing pmon-like supervisor
// (chapter 28: gdb and remote debugging).
//
//	app run    -- normal operation: reap a child, report its exit, exit 0
//	app crash  -- the SAME call path, fed a child record whose exit-status
//	              pointer was never populated (as if handleChild ran before
//	              the reaper filled it in) -> a nil pointer dereference
//	              three frames down:
//
//	                main -> runSupervisor -> handleChild -> deref
//
// This binary is deliberately dual-use: a normal program in "run" mode, a
// debugging TARGET in "crash" mode.
//
// Go does not core-dump a nil dereference the way C++ does. The runtime
// itself catches the hardware SIGSEGV, converts it to a runtime.Error, and
// panics. Left unrecovered, that panic unwinds to main, prints
// "panic: runtime error: invalid memory address or nil pointer dereference"
// plus a full goroutine trace naming every frame below, and the process
// exits with status 2 -- not 139, and there is no core file for gdb to load.
// A goroutine trace (or dlv) is the tool for this crash, not gdb + core.
package main

import (
	"fmt"
	"os"
)

// childRecord is a child the supervisor is reporting on. exitSlot is filled
// in by the reaper once the wait status is known; handleChild reads through
// it to print the status line.
type childRecord struct {
	pid      int
	exitSlot *int
}

//go:noinline
func deref(child childRecord) {
	// BUG: no nil check before the read. In "crash" mode exitSlot is nil
	// because handleChild ran before the reaper recorded the exit status --
	// exactly the kind of race a real supervisor can hit.
	fmt.Printf("pmon: child %d exited status=%d\n", child.pid, *child.exitSlot)
}

//go:noinline
func handleChild(child childRecord) {
	deref(child)
}

//go:noinline
func runSupervisor(injectBug bool) int {
	fmt.Println("pmon: supervisor starting")
	reapedStatus := 0
	child := childRecord{pid: 4242}
	if injectBug {
		handleChild(child) // crash path: exitSlot stays nil
		return 0            // unreachable
	}
	child.exitSlot = &reapedStatus
	handleChild(child)
	fmt.Println("pmon: supervisor exiting cleanly")
	return 0
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: app run")
	fmt.Fprintln(os.Stderr, "       app crash")
	os.Exit(2)
}

func main() {
	if len(os.Args) != 2 {
		usage()
	}
	switch os.Args[1] {
	case "run":
		os.Exit(runSupervisor(false))
	case "crash":
		os.Exit(runSupervisor(true))
	default:
		usage()
	}
}
