// 41-capstone-fleet — pmon supervises chatterd + sysagent + fwatch across
// two lab hosts, capability-dropped and Landlock-sandboxed, all telemetry
// exported to the host LGTM stack. One binary, four subcommands (the same
// self-re-exec shape ch34's container entrypoint uses): pmon is the fleet's
// init; chatterd/sysagent/fwatch are the services it supervises.
package main

import (
	"context"
	"fmt"
	"os"
	"strconv"
)

func usage() {
	fmt.Fprintln(os.Stderr, `usage: app <command>
  pmon [--node NAME] [--sandbox-dir DIR] [--peer HOST:PORT] [--peer-node NAME]
       [--chatterd-port P] [--health-interval-ms N]
  chatterd serve [--host H] [--port P] [--node NAME] [--peer HOST:PORT] [--peer-node NAME]
  chatterd send   --host H --port P --nick NICK --text TEXT [--timeout-ms T]
  chatterd listen --host H --port P --nick NICK [--timeout-ms T]
  sysagent [--node NAME] [--interval-ms N] [--once]
  sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]
  fwatch snapshot DIR
  fwatch watch DIR [--sandbox] [--timeout-ms T]`)
}

// argFlag scans args for "--name value" and returns value (and true), or def.
func argFlag(args []string, name, def string) (string, bool) {
	for i, a := range args {
		if a == name {
			if i+1 < len(args) {
				return args[i+1], true
			}
			return "", false
		}
	}
	return def, true
}

func hasFlag(args []string, name string) bool {
	for _, a := range args {
		if a == name {
			return true
		}
	}
	return false
}

func firstPositional(args []string) string {
	skip := false
	for _, a := range args {
		if skip {
			skip = false
			continue
		}
		if len(a) >= 2 && a[:2] == "--" {
			skip = true
			continue
		}
		return a
	}
	return ""
}

func atoiOr(s string, def int) int {
	if s == "" {
		return def
	}
	v, err := strconv.Atoi(s)
	if err != nil {
		return def
	}
	return v
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	cmd := os.Args[1]
	rest := os.Args[2:]

	switch cmd {
	case "pmon":
		node, ok1 := argFlag(rest, "--node", "node1")
		sandboxDir, ok2 := argFlag(rest, "--sandbox-dir", "/tmp/fwatch-sandbox")
		peer, ok3 := argFlag(rest, "--peer", "")
		peerNode, ok4 := argFlag(rest, "--peer-node", "")
		portS, ok5 := argFlag(rest, "--chatterd-port", "47100")
		healthS, ok6 := argFlag(rest, "--health-interval-ms", "2000")
		if !ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6 {
			usage()
			os.Exit(2)
		}
		os.Exit(pmonRun(node, sandboxDir, peer, peerNode, atoiOr(portS, 47100), atoiOr(healthS, 2000)))

	case "chatterd":
		if len(rest) == 0 {
			usage()
			os.Exit(2)
		}
		switch rest[0] {
		case "serve":
			args := rest[1:]
			host, _ := argFlag(args, "--host", "0.0.0.0")
			portS, _ := argFlag(args, "--port", "47100")
			node, _ := argFlag(args, "--node", "node1")
			peer, _ := argFlag(args, "--peer", "")
			peerNode, _ := argFlag(args, "--peer-node", "")
			tel := initTelemetry(context.Background(), "chatterd", node)
			defer tel.shutdown(context.Background())
			os.Exit(chatterdServe(host, atoiOr(portS, 47100), node, peer, peerNode, tel))
		case "send":
			args := rest[1:]
			host, _ := argFlag(args, "--host", "127.0.0.1")
			portS, _ := argFlag(args, "--port", "47100")
			nick, ok1 := argFlag(args, "--nick", "")
			text, ok2 := argFlag(args, "--text", "")
			timeoutS, _ := argFlag(args, "--timeout-ms", "3000")
			if !ok1 || !ok2 || nick == "" || text == "" {
				usage()
				os.Exit(2)
			}
			os.Exit(chatterdSend(host, atoiOr(portS, 47100), nick, text, atoiOr(timeoutS, 3000)))
		case "listen":
			args := rest[1:]
			host, _ := argFlag(args, "--host", "127.0.0.1")
			portS, _ := argFlag(args, "--port", "47100")
			nick, ok1 := argFlag(args, "--nick", "")
			timeoutS, _ := argFlag(args, "--timeout-ms", "5000")
			if !ok1 || nick == "" {
				usage()
				os.Exit(2)
			}
			os.Exit(chatterdListen(host, atoiOr(portS, 47100), nick, atoiOr(timeoutS, 5000)))
		default:
			usage()
			os.Exit(2)
		}

	case "sysagent":
		if len(rest) > 0 && rest[0] == "saturate" {
			args := rest[1:]
			resource, _ := argFlag(args, "--resource", "")
			secondsS, _ := argFlag(args, "--seconds", "10")
			workersS, _ := argFlag(args, "--workers", "0")
			mbS, _ := argFlag(args, "--mb", "0")
			os.Exit(saturate(resource, atoiOr(secondsS, 10), atoiOr(workersS, 0), atoiOr(mbS, 0)))
		}
		node, _ := argFlag(rest, "--node", "node1")
		intervalS, _ := argFlag(rest, "--interval-ms", "2000")
		once := hasFlag(rest, "--once")
		tel := initTelemetry(context.Background(), "sysagent", node)
		defer tel.shutdown(context.Background())
		os.Exit(sysagentRun(node, atoiOr(intervalS, 2000), once, tel))

	case "fwatch":
		if len(rest) == 0 {
			usage()
			os.Exit(2)
		}
		switch rest[0] {
		case "snapshot":
			args := rest[1:]
			dir := firstPositional(args)
			if dir == "" {
				usage()
				os.Exit(2)
			}
			os.Exit(fwatchSnapshot(dir))
		case "watch":
			args := rest[1:]
			dir := firstPositional(args)
			if dir == "" {
				usage()
				os.Exit(2)
			}
			sandbox := hasFlag(args, "--sandbox")
			timeoutS, _ := argFlag(args, "--timeout-ms", "0")
			os.Exit(fwatchWatch(dir, sandbox, atoiOr(timeoutS, 0)))
		default:
			usage()
			os.Exit(2)
		}

	default:
		usage()
		os.Exit(2)
	}
}
