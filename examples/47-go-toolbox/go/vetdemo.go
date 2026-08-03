//go:build vetdemo

package main

import "fmt"

// vetdemo.go is excluded from the default build/test (`go build ./...`,
// `go test ./...`) by the vetdemo build tag above. It exists only so
// `go vet -tags vetdemo .` has something real to report: printBadFormat's
// Printf call passes a string argument to a %d verb, a stable "printf"
// finding go vet's printf analyzer always reports for this exact mismatch.
func printBadFormat() {
	fmt.Printf("bad format: %d\n", "not-an-int")
}
