package main

import "strconv"

// smells.go collects deliberate, behavior-preserving smells for the
// if-present static-analysis gates (verify.lua gates G "golangci-lint" and
// H "staticcheck") and the chapter's "Errors, three ways" section. None of
// these functions is ever called at runtime -- they compile cleanly and
// have zero effect on `toolbox report`'s digest -- so they exist purely as
// designed-for targets for tools this host does not have installed.

// unusedHelper is never called. staticcheck's U1000 ("unusedHelper is
// unused") and golangci-lint's "unused" linter both flag unreferenced
// unexported functions like this one.
func unusedHelper(n int) string {
	return strconv.Itoa(n * 2)
}

// deadStore is also never called (a second U1000 hit), and its body
// contains its own smell: the initial value assigned to x is overwritten
// before it is ever read. staticcheck's SA4006 ("this value of x is never
// used") targets exactly this pattern; go vet does not catch it because the
// variable itself is read eventually (just not the first value).
func deadStore() int {
	x := 10
	x = 20
	return x
}

// unused_smell_name violates Go naming conventions (mixedCaps, no
// underscores) on purpose. staticcheck's stylecheck companion, ST1003,
// flags underscored identifiers like this one; it is also never called, so
// it doubles as a third U1000 hit.
func unused_smell_name() int {
	return 0
}
