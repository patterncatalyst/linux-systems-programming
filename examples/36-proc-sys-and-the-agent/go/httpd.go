// sysagent: the /metrics HTTP endpoint, using net/http (Go's stdlib server —
// the C++ and Rust versions hand-roll the same tiny surface on raw sockets).
package main

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// Serve exposes GET /metrics (JSON snapshot) on 0.0.0.0:port until
// SIGINT/SIGTERM. Each request takes a fresh snapshot with the given
// intervalMs. Returns the process exit code (0 on a clean signal shutdown).
func Serve(port, intervalMs int) int {
	mux := http.NewServeMux()
	mux.HandleFunc("/metrics", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.NotFound(w, r)
			return
		}
		snap, err := TakeSnapshot(intervalMs)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprintln(w, ToJSON(snap))
	})

	ln, err := net.Listen("tcp", fmt.Sprintf("0.0.0.0:%d", port))
	if err != nil {
		fmt.Fprintf(os.Stderr, "sysagent: listen: %v\n", err)
		return 1
	}
	srv := &http.Server{Handler: mux}

	serveErr := make(chan error, 1)
	go func() { serveErr <- srv.Serve(ln) }()

	fmt.Printf("sysagent: listening on 0.0.0.0:%d\n", port)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	select {
	case <-sigCh:
		fmt.Println("sysagent: shutting down")
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
		return 0
	case err := <-serveErr:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			fmt.Fprintf(os.Stderr, "sysagent: serve: %v\n", err)
			return 1
		}
		return 0
	}
}
