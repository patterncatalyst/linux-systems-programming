package messy

import "fmt"

// messy.go is deliberately misformatted (irregular spacing, no gofmt-style
// alignment) so `gofmt -l` lists this file. It lives under testdata/, which
// the go tool ignores for build/vet/test purposes -- only gofmt is ever
// pointed at it directly, by name.
func Add(a,b int)int{
	return a+b
}

func    Greet( name string )  {
fmt.Println("hello,",name)
}
