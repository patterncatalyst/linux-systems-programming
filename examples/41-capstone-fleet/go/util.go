// util.go — small helpers shared across the fleet's subcommands.
package main

import (
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
)

// installSignalFlag returns an atomic bool that flips to true the first time
// SIGTERM or SIGINT arrives, so a poll loop can unwind cleanly instead of
// dying mid-write.
func installSignalFlag() *atomic.Bool {
	flag := &atomic.Bool{}
	ch := make(chan os.Signal, 2)
	signal.Notify(ch, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		<-ch
		flag.Store(true)
	}()
	return flag
}
