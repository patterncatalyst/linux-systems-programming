//go:generate go run ./internal/gen

// toolbox -- the reference Go program for ch47 (go/toolchain version
// management, go generate discipline, gofmt vs gofumpt, go vet, pprof,
// golangci-lint/staticcheck config authoring, delve, benchstat). Go's analog
// of ch46's C++ toolbox: the Go toolchain itself is the subject.
//
// Subcommands:
//
//	toolbox report          -- deterministic FNV-1a digest over a fixed
//	                            embedded payload, integer/string output
//	                            only (no floats, addresses, timing, or map
//	                            iteration) so the digest is reproducible.
//	toolbox tools            -- prints the go-generate-produced tool table,
//	                            proof that `go generate` output is real and
//	                            checkable, not merely "it ran".
//	toolbox defect divzero   -- deliberately triggers an integer
//	                            divide-by-zero panic (delve/panic-recovery
//	                            narrative parity with ch46's UBSan defect).
package main

import (
	"fmt"
	"os"
)

func printUsage() {
	fmt.Fprintln(os.Stderr, "usage: toolbox <report|tools|defect divzero>")
}

func cmdReport() int {
	d := fnv1a(kPayload[:])
	fmt.Printf("toolbox report: payload_len=%d digest=0x%016x\n", len(kPayload), d.FNV)
	return 0
}

func cmdTools() int {
	fmt.Printf("tools: count=%d\n", len(generatedTools))
	for _, t := range generatedTools {
		fmt.Printf("tools: %-13s -- %s\n", t.Name, t.Role)
	}
	return 0
}

func cmdDefectDivZero() int {
	// `int32` variables (not compile-time constants) so the compiler cannot
	// fold the division at build time -- the point is a *runtime* panic,
	// not a compiler diagnostic. This one statement is the entire seeded
	// defect.
	var divisor int32
	dividend := int32(42)
	fmt.Printf("toolbox defect divzero: dividend=%d divisor=%d\n", dividend, divisor)
	os.Stdout.Sync()
	result := dividend / divisor // runtime panic: integer divide by zero
	fmt.Printf("unreachable: result=%d\n", result)
	return 0
}

func main() {
	if len(os.Args) < 2 {
		printUsage()
		os.Exit(2)
	}
	switch os.Args[1] {
	case "report":
		os.Exit(cmdReport())
	case "tools":
		os.Exit(cmdTools())
	case "defect":
		if len(os.Args) < 3 || os.Args[2] != "divzero" {
			printUsage()
			os.Exit(2)
		}
		os.Exit(cmdDefectDivZero())
	default:
		printUsage()
		os.Exit(2)
	}
}
