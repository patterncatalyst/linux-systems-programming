// pmon.go — the book's recurring process supervisor (ch11-14, 18-19, 32,
// 34), grown here into the capstone's fleet init: it drops the capability
// bounding set once, then fork/execs (self-re-exec, same technique ch34's
// container entrypoint uses) chatterd, sysagent, and fwatch as children,
// restarting any of them that exits unexpectedly, forwarding SIGTERM/SIGINT
// to all three on its own shutdown, and printing a health line every tick so
// an operator (or verify.lua, over ssh) can poll fleet state without
// touching /proc.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

type childSpec struct {
	name string
	args []string
}

type childState struct {
	state    atomic.Value // string: "starting"|"up"|"restarting"|"down"
	restarts atomic.Int64
	pid      atomic.Int64
}

func pmonRun(node, sandboxDir, peer, peerNode string, chatterdPort int, healthIntervalMs int) int {
	if err := os.MkdirAll(sandboxDir, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: mkdir %s: %v\n", sandboxDir, err)
		return 1
	}

	dropCapabilityBoundingSet()

	selfExe, err := os.Executable()
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: os.Executable: %v\n", err)
		return 1
	}
	selfExe, _ = filepath.Abs(selfExe)

	chatterdArgs := []string{"chatterd", "serve", "--host", "0.0.0.0",
		"--port", strconv.Itoa(chatterdPort), "--node", node}
	if peer != "" {
		chatterdArgs = append(chatterdArgs, "--peer", peer, "--peer-node", peerNode)
	}

	specs := []childSpec{
		{"chatterd", chatterdArgs},
		{"sysagent", []string{"sysagent", "--node", node, "--interval-ms", "2000"}},
		{"fwatch", []string{"fwatch", "watch", sandboxDir, "--sandbox"}},
	}

	states := make(map[string]*childState, len(specs))
	for _, s := range specs {
		cs := &childState{}
		cs.state.Store("starting")
		states[s.name] = cs
	}

	var shuttingDown atomic.Bool
	var wg sync.WaitGroup

	for _, spec := range specs {
		spec := spec
		cs := states[spec.name]
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				cmd := exec.Command(selfExe, spec.args...)
				cmd.Stdout = os.Stdout
				cmd.Stderr = os.Stderr
				cmd.Env = os.Environ()
				if err := cmd.Start(); err != nil {
					fmt.Fprintf(os.Stderr, "pmon: start service=%s: %v\n", spec.name, err)
					time.Sleep(500 * time.Millisecond)
					continue
				}
				cs.pid.Store(int64(cmd.Process.Pid))
				cs.state.Store("up")
				fmt.Printf("pmon: started service=%s pid=%d\n", spec.name, cmd.Process.Pid)

				waitErr := cmd.Wait()
				if shuttingDown.Load() {
					cs.state.Store("down")
					return
				}
				n := cs.restarts.Add(1)
				cs.state.Store("restarting")
				fmt.Printf("pmon: restart service=%s attempt=%d reason=%v\n", spec.name, n, waitErr)
				time.Sleep(300 * time.Millisecond)
			}
		}()
	}

	sigCh := make(chan os.Signal, 2)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		<-sigCh
		shuttingDown.Store(true)
		for name, cs := range states {
			if pid := cs.pid.Load(); pid > 0 {
				_ = syscall.Kill(int(pid), syscall.SIGTERM)
			}
			_ = name
		}
	}()

	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()

	order := []string{"chatterd", "sysagent", "fwatch"}
	ticker := time.NewTicker(time.Duration(healthIntervalMs) * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-done:
			fmt.Println("pmon: shutdown")
			return 0
		case <-ticker.C:
			printHealth(order, states)
		}
	}
}

func printHealth(order []string, states map[string]*childState) {
	parts := make([]string, 0, len(order))
	restartParts := make([]string, 0, len(order))
	for _, name := range order {
		cs := states[name]
		parts = append(parts, name+"="+cs.state.Load().(string))
		restartParts = append(restartParts, fmt.Sprintf("%s:%d", name, cs.restarts.Load()))
	}
	fmt.Print("pmon: health ")
	for i, p := range parts {
		if i > 0 {
			fmt.Print(" ")
		}
		fmt.Print(p)
	}
	fmt.Print(" restarts=")
	for i, p := range restartParts {
		if i > 0 {
			fmt.Print(",")
		}
		fmt.Print(p)
	}
	fmt.Println()
}
