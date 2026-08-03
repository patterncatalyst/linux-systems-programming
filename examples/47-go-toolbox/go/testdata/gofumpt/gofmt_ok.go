package gofmtok

import "fmt"

// gofmt_ok.go is gofmt-clean (gofmt -l prints nothing for this file) but
// gofumpt-dirty: gofumpt additionally forbids a blank line at the start of
// a block, which Greet's body has. It lives under testdata/, which the go
// tool ignores for build/vet/test -- only gofmt/gofumpt are ever pointed at
// it directly, by name.
func Greet(name string) {

	fmt.Println("hello,", name)
}
