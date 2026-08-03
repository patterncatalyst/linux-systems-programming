---
title: "The Go toolbox: go.mod toolchain pins, go generate discipline, and the pprof-to-benchstat workflow"
order: 47
part: "Appendices: Tooling"
description: "go-toolbox is one Go binary and the toolchain wrapped around it -- go.mod's go/toolchain directives resolved through GOTOOLCHAIN, a self-contained go generate pipeline producing a DO-NOT-EDIT tool table, and gofmt/go vet/pprof as go-native hard gates -- with committed configs for gofumpt, golangci-lint, staticcheck, delve, and benchstat shown as reference but absent on the reference host. Verified on the Fedora 44 host: verify.lua PASS 16/FAIL 0 offline, toolbox report digest=0x481984990deee5ff (byte-identical to ch46's C++ digest), go generate regenerating tools_generated.go byte-for-byte, go vet -tags vetdemo catching a printf mismatch at vetdemo.go:13, and go tool pprof -top naming hotLoop."
duration: "50 minutes"
---

Chapter 46 turned all the way around on C++: one language, one small binary,
and every tool that touches it before it ever runs — two compilers, a
build-system generator, a package manager, a static analyzer, a debugger
taught a custom type. Go gets the identical treatment here, and the shape of
the problem is the mirror image of C++'s. Where `toolbox` needed seven named
`CMakePresets.json` configurations just to make "which compiler, which build
type" an explicit choice, Go ships one command, `go build`, and pins its own
version inside the module that needs it. That difference does not mean Go has
nothing left to say about tooling — it means the depth moves somewhere else:
into `go.mod`'s own toolchain directives, into `go generate` as a first-class
source-of-truth discipline, into what a formatter versus a deeper static
analyzer versus a debugger each choose to notice about the identical source.
`toolbox` (the Go one) is a single stdlib-only binary, engineered the same
way its C++ sibling was — every tool pointed at it has something concrete to
catch or confirm, never just a clean exit.

{% include excalidraw.html
   file="47-go-toolchain-pipeline"
   alt="A go.mod box showing the go 1.26 and toolchain go1.26.5 directives feeds through a GOTOOLCHAIN resolution step into the go binary. From the go binary, two arrows fan out: one into an amber toolbox report box that computes an FNV-1a digest over a fixed embedded payload and prints digest=0x481984990deee5ff, the other into an amber go generate box that runs internal/gen against a hand-maintained tool table and diffs its output against the committed tools_generated.go, landing in a diff-empty gate. Both boxes are labeled go-native, verified offline on this host."
   caption="Figure 47.1 — the Go toolchain and version-management flow: go.mod's go/toolchain directives resolve through GOTOOLCHAIN into the go binary, which drives the deterministic build (report digest) and the go-generate diff-clean gate" %}

> **Tools used** — `go` (host; `build`/`run`/`generate`/`vet`/`tool
> pprof`/`fmt` all ship inside the single `go` binary, which
> `scripts/check-host.sh` gates as a hard requirement via its `go 1.26.x`
> check), `gofmt` (host; ships alongside `go`, not separately gated).
> `golangci-lint`, `staticcheck`, and `dlv` are warn-only entries in
> `scripts/check-host.sh` ("nice to have locally"); `gofumpt`, `govulncheck`,
> and `benchstat` are not gated by it at all. All six of those —
> `gofumpt`, `golangci-lint`, `staticcheck`, `govulncheck`, `dlv`, and
> `benchstat` — are absent on this reference host, so their sections below
> are shown as reference, not verified. No VM, no root, no LGTM stack —
> `examples/47-go-toolbox` is `mode: local` in `examples/manifest.yaml`.

Because the Go toolchain is itself the subject, `examples/47-go-toolbox/`
ships `langs: [go]` only, the same way Chapter 44's Go-runtime chapter and
Chapter 46's C++ chapter each stayed inside one language — a C++ or Rust
build would have nothing new to say about a `go.mod` toolchain directive or a
`.golangci.yml`. Every code block below is a plain fenced `go`/`console`/
`yaml` block; there is no `codetabs` include to reach for when only one
language is in the room.

## Go toolchain and version management

`go/go.mod` is four lines, and two of them are the whole subject of this
section:

```
module github.com/patterncatalyst/linux-systems-programming/examples/47-go-toolbox/go

go 1.26

toolchain go1.26.5
```

`go 1.26` and `toolchain go1.26.5` answer two different questions that look
like the same question. `go 1.26` is a **language-version floor**: it tells
every tool that reads this module (the compiler, `go vet`, an IDE's language
server) which Go language and standard-library semantics this source relies
on — it is the same directive C++'s `CMAKE_CXX_STANDARD` or Rust's `edition`
line answers, a promise about the language, not the binary that compiles it.
`toolchain go1.26.5` is a stricter, separate promise: the exact `go` command
release this module was developed and verified against. The `go` command
itself reads that second line and decides, via the `GOTOOLCHAIN` environment
variable (default `auto`), whether it can proceed with the toolchain already
on `PATH` or needs to fetch a different one. If the installed `go` binary's
own version already satisfies the `toolchain` line — which it does here,
`go1.26.5` on `PATH` matching `toolchain go1.26.5` exactly — resolution stops
immediately with no network access at all. Only when the installed binary is
*older* than the pinned toolchain does `GOTOOLCHAIN=auto` reach out to the Go
module proxy and download a newer one; `GOTOOLCHAIN=local` disables that
fallback entirely and fails instead of fetching. That is the property this
whole example leans on to stay offline: `go.mod`'s toolchain directive is not
an aspiration a reader has to satisfy by hand, it is a pin the `go` command
itself enforces, and on this host it resolves to the binary already
installed without a single byte crossing the network.

That is also the cleanest point of contrast with the two sibling toolchain
chapters. C++ has no equivalent single-file pin — Chapter 46 needed seven
named `CMakePresets.json` configurations plus a Conan lockfile to make
"which compiler, which build type" and "which exact package revision"
explicit, because C++ leaves compiler choice and dependency resolution as
separate decisions a project has to wire up itself. Rust's `edition` (in
`Cargo.toml`) and `rust-toolchain.toml`'s `channel` line are close cousins of
Go's `go`/`toolchain` split — a language-edition floor plus an exact
toolchain pin — but Rust's toolchain file additionally names `components`
(`rustfmt`, `clippy`) as part of the same pin. Go keeps the two directives to
exactly two lines inside the module a reader already has open, and — as the
rest of this chapter shows — treats every other tool (`gofmt`, `vet`,
`pprof`) as already living inside that one pinned binary rather than needing
a components list of its own.

## Deterministic build and digest

`toolbox report` asks the same falsifiable question `toolbox` (C++) did:
does a fixed input, hashed the same way, always produce the same output? The
type and the algorithm are both intentionally uninteresting:

```go
package main

// Digest wraps a single 64-bit FNV-1a hash value. Small on purpose: it
// exists so the delve chapter section (go/.dlvinit) has a concrete,
// non-trivial type to break on and print instead of a bare integer.
type Digest struct {
	FNV uint64
}

const (
	fnvOffsetBasis uint64 = 0xcbf29ce484222325
	fnvPrime       uint64 = 0x100000001b3
)

// kPayload is ch46's exact 16-byte payload ("The quick brown."), reused
// byte-for-byte so the Go and C++ toolbox chapters land on the identical
// FNV-1a digest (0x481984990deee5ff) -- a cross-appendix easter egg, not a
// coincidence. FNV-1a is language-independent: any conforming
// implementation over the same bytes produces the same 64-bit value.
var kPayload = [16]byte{
	0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b,
	0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e,
}
```

`fnv1a` is nine lines and touches nothing but integers:

```go
// fnv1a hashes data with the 64-bit FNV-1a algorithm. Integer/string-only
// output, no floats, addresses, timing, or map iteration -- the same input
// bytes produce the same digest bit-for-bit on every conforming Go
// toolchain, on every architecture, every run.
func fnv1a(data []byte) Digest {
	h := fnvOffsetBasis
	for _, b := range data {
		h ^= uint64(b)
		h *= fnvPrime
	}
	return Digest{FNV: h}
}
```

`cmdReport` prints only what `fnv1a` computed:

```go
func cmdReport() int {
	d := fnv1a(kPayload[:])
	fmt.Printf("toolbox report: payload_len=%d digest=0x%016x\n", len(kPayload), d.FNV)
	return 0
}
```

Run for real on this host:

```console
$ ./demo.sh go run report
toolbox report: payload_len=16 digest=0x481984990deee5ff
```

`digest=0x481984990deee5ff` is not a coincidence shared with Chapter 46 — it
is the same 16-byte payload, hashed by the same 64-bit FNV-1a algorithm,
computed by an entirely different compiler and runtime. FNV-1a has no
floating-point rounding, no pointer values, and no unordered-container
iteration to disagree about; two conforming implementations over identical
bytes are required to agree on every bit of the output, which is exactly why
Go's `toolbox report` and C++'s `toolbox report` land on the identical
digest even though nothing about their toolchains, calling conventions, or
runtimes has anything in common. Worth one sentence, not a whole section:
that agreement is a real cross-appendix check, not a repeated literal typed
twice by hand.

## `go generate` in depth

`go generate` is Go's answer to "keep a derived file accurate against its
source of truth" — it runs a directive comment, once, on demand, and never as part
of `go build`. `main.go`'s directive is one line at the very top of the
file:

```go
//go:generate go run ./internal/gen
```

`internal/gen/main.go` is the generator that directive names, and it is
deliberately self-contained: no third-party dependency, stdlib only, so
`go generate ./...` never touches the network any more than the rest of this
module does. Its source of truth is a hand-maintained map, listed
**out of alphabetical order on purpose**:

```go
// toolTable is the hand-maintained source of truth for `toolbox tools`.
// Keys are listed out of alphabetical order on purpose, to prove the
// generator -- not the author -- is what sorts them into generatedTools.
var toolTable = map[string]string{
	"staticcheck":   "advanced static analysis and unused-code detection",
	"gofmt":         "format Go source files",
	"go vet":        "report suspicious constructs the compiler allows",
	"delve":         "the Go debugger (dlv)",
	"go generate":   "run source-code generators referenced by directive",
	"gofumpt":       "a stricter superset of gofmt",
	"golangci-lint": "run many linters through one driver",
	"pprof":         "profile CPU, heap, and goroutine activity",
	"benchstat":     "compare statistical summaries of benchmark runs",
	"govulncheck":   "cross-reference dependencies against the Go vuln DB",
}
```

Go's built-in `map` type has no guaranteed iteration order — reading
`toolTable` twice in the same process can visit its keys in two different
sequences — so `render` sorts the keys itself before writing anything, and
passes the assembled source through `go/format` before it ever touches disk:

```go
func render() ([]byte, error) {
	names := make([]string, 0, len(toolTable))
	for name := range toolTable {
		names = append(names, name)
	}
	sort.Strings(names)

	var b strings.Builder
	b.WriteString(header)
	b.WriteString("package main\n\n")
	// ... struct + slice literal built from the sorted names ...
	return format.Source([]byte(b.String()))
}
```

`sort.Strings` plus `format.Source` together are what make re-running the
generator against unchanged input reproduce the *exact same bytes* every
time — no map-iteration nondeterminism, no formatting drift from one Go
release to the next. The committed output, `tools_generated.go`, opens with
the header every regeneration writes unchanged:

```go
// Code generated by internal/gen. DO NOT EDIT.

package main

// Tool describes one entry in the toolbox's tool table.
type Tool struct {
	Name string
	Role string
}

// generatedTools is sorted by Name; see internal/gen/main.go.
var generatedTools = []Tool{
	{Name: "benchstat", Role: "compare statistical summaries of benchmark runs"},
	{Name: "delve", Role: "the Go debugger (dlv)"},
```

`// Code generated by internal/gen. DO NOT EDIT.` is Go tooling's own
convention (`gofmt`, IDEs, and code-review bots all recognize this exact
comment shape) for marking a file as derived rather than authored — a
human editing `tools_generated.go` directly is fixing the wrong file, since
the next `go generate` run silently overwrites the edit. `cmdTools` reads
that slice and prints it:

```console
$ ./demo.sh go run tools
tools: count=10
tools: gofmt         -- format Go source files
```

That is a real, run transcript, not the full ten-line table — this chapter
only quotes the two lines `verify.lua` pins as literals (the count and one
sorted entry); the other eight rows are the same `%-13s -- %s` format
applied to the remaining names in `toolTable`, sorted.

The gate that matters is not "did the generator run", it is "did the
generator's output change anything" — the discipline a reader would wire
into CI as `go generate ./... && git diff --exit-code`. `verify.lua`
enforces the same property without assuming a git checkout to diff against,
by copying the committed file aside, regenerating, diffing the copy against
the fresh output, then restoring the original so the working tree is never
left dirty by the check itself:

```lua
local gen_check = [[
cd go
tmp=$(mktemp)
cp tools_generated.go "$tmp"
go generate ./... >/dev/null 2>&1
diff "$tmp" tools_generated.go
status=$?
cp "$tmp" tools_generated.go
rm -f "$tmp"
exit $status
]]
local gen = checks.run(gen_check)
checks.expect_exit(gen, 0,
  "go: go generate ./... regenerates tools_generated.go byte-identical to committed")
```

Run for real, both directions agree: the copy-aside diff is empty, and the
idiomatic form a reader would actually put in CI —
`go generate ./... && git diff --exit-code` — exits 0 on this checkout too.
Either way, the claim is the same one Chapter 46 made about GCC-vs-clang
parity, aimed at a different kind of determinism: not "two compilers agree,"
but "the generator, run twice against unchanged input, agrees with itself."

## `gofmt` vs `gofumpt`

`gofmt` is part of the `go` binary itself — there is no separate install, no
version to pin, and no config file: it applies one fixed layout and either
a file already matches it or `gofmt -l` prints the file's name. Silence is
success, which makes a clean run easy to overlook and a dirty one impossible
to miss. `go/testdata/gofmt/messy.go` exists specifically to be listed:

```go
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
```

`testdata/` is the load-bearing detail: Go's own tooling (`go build`,
`go vet`, `go test`) ignores any directory literally named `testdata` when
walking a module, so `messy.go` never becomes part of the compiled binary,
never gets vetted, and never affects `toolbox report`'s digest — it exists
purely as a target `gofmt` is pointed at *by name*. Run against both the
tracked sources and this fixture, the contrast is exact:

```console
$ gofmt -l $(find . -name '*.go' -not -path './testdata/*')

$ gofmt -l testdata/gofmt/messy.go
testdata/gofmt/messy.go
```

`gofumpt` is a stricter superset — it enforces every rule `gofmt` does, plus
a handful more that `gofmt` deliberately leaves alone, one of which is a
banned leading blank line at the start of a block. `testdata/gofumpt/gofmt_ok.go`
is built to sit exactly on that boundary: `gofmt`-clean, `gofumpt`-dirty.

```go
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
```

`gofumpt` is absent on this reference host, so its half of the contrast is
shown, not verified: `gofumpt -l testdata/gofumpt/gofmt_ok.go` is the
designed-for command, and `verify.lua`'s gate F would assert exactly that
file name back if the tool were installed. `gofmt -l` on the same file
prints nothing — that half of the pair *is* verified above, since `gofmt`
ships with `go` itself.

## `golangci-lint` config authoring

`golangci-lint` runs many linters, including `staticcheck` itself, behind
one driver and one config file, so authoring that file is most of the work
of adopting it. `go/.golangci.yml` is committed in full:

```yaml
# Reference golangci-lint config for ch47 (verify.lua gate G, if present).
# Absent on the reference host -- committed so a reader who installs
# golangci-lint gets a real, non-default lint pass, not just "add a config
# file" hand-waving. `unused` and `staticcheck` are on by default; `revive`
# and `unparam` are added explicitly, and testdata/ is excluded because those
# fixtures are gofmt/gofumpt targets, not lint targets.
version: "2"

linters:
  default: standard
  enable:
    - staticcheck
    - unused
    - revive
    - unparam

  exclusions:
    paths:
      - testdata

issues:
  max-issues-per-linter: 0
  max-same-issues: 0
```

`linters.default: standard` opts into golangci-lint v2's own curated
default set (which already includes `staticcheck` and `unused`); `enable`
lists them again explicitly alongside `revive` and `unparam` so the config
reads as a complete, intentional list rather than "whatever the default
happens to include this release." `exclusions.paths: [testdata]` is the
same isolation `gofmt`/`gofumpt` rely on, applied to linting: the fixtures
under `testdata/` are deliberately dirty for *other* tools and would be
noise here. `issues.max-issues-per-linter: 0` and `max-same-issues: 0`
both mean "no cap" — every finding is reported, not truncated after the
default handful, since this config exists to show every smell `smells.go`
was written to trigger.

`golangci-lint` is absent on this reference host, so its gate is shown, not
run: the designed-for command is `golangci-lint run ./...`, and `verify.lua`
gate G asserts the string `(unused)` appears in its output — golangci-lint's
own convention of suffixing each finding with the linter name that raised
it, in parentheses. `smells.go`'s three unreferenced functions (below) are
exactly what the `unused` linter — one of the two in `linters.default:
standard` this config keeps on — is designed to flag.

## `staticcheck`

`staticcheck` reads more of a program's data flow than `go vet` does, and
`go/smells.go` exists purely to give it (and `golangci-lint`'s bundled copy
of it) real targets — three functions, never called anywhere, each with its
own distinct smell:

```go
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
```

Three functions, three distinct designed-for findings, none of them
overlapping: `unusedHelper` (line 15) is simply dead code — **U1000**,
staticcheck's unused-declaration check. `deadStore` (line 24) is dead *and*
its first assignment (`x := 10`) is overwritten before ever being read —
**U1000** again for the whole function, plus **SA4006** for the specific
overwritten value, a pattern `go vet` does not catch because `x` genuinely
is read eventually, just not at its first value. `unused_smell_name`
(line 34) is dead a third time (a third **U1000**) and also violates Go's
`mixedCaps` naming convention on its own — **ST1003**, staticcheck's
`stylecheck` companion. None of the three is ever referenced from `main.go`
or anywhere else, so all three compile cleanly and none of them can ever
touch `toolbox report`'s digest, no matter how they are edited.

`staticcheck` is absent on this reference host, so these are the checks it
is designed to report, quoted from what the tool's own documentation names
them — not a captured run. `verify.lua` gate H asserts the literal tokens
`U1000` and `SA4006` would appear in `staticcheck ./...`'s output if the
tool were present; it does not assert `ST1003`, since that finding comes
from `staticcheck`'s separately-invoked `stylecheck` companion rather than
its default check set.

## `govulncheck`

`govulncheck` answers a question neither `gofmt`, `go vet`, nor
`staticcheck` can: not "is this code suspicious," but "does anything this
module actually calls sit inside a function the Go vulnerability database
has an advisory against." That is a fundamentally different kind of check —
it needs a reachable network (the vulnerability database lives at
`vuln.go.dev`) and it only has something to say about a *vulnerable
dependency*, which this module deliberately does not have: `go/go.mod` pulls
in nothing outside the standard library, so there is no `go.sum` and nothing
for `govulncheck` to cross-reference here at all.

The workflow a reader would actually use is `govulncheck ./...` (or
`govulncheck -mode=binary ./bin/toolbox` against an already-built binary),
run from inside the module whose dependencies are in question. Because this
module has no third-party dependency to demonstrate against, the practical way
to exercise the tool for real would be a small, deliberately isolated
sub-module — its own `go.mod`, pinning one dependency at a version with a
known advisory, calling exactly the vulnerable symbol — kept entirely
separate from `go/`'s own `go.mod` so a vulnerable dependency is never part
of this example's default build or committed to the repository. This
example does not ship that sub-module: doing so for real needs network
access to resolve the dependency and query the vulnerability database, both
excluded by this chapter's offline constraint, and committing a genuinely
vulnerable dependency into the repository is not a trade worth making for
one illustrative transcript.

What follows is the *shape* of a real advisory hit, not a captured run — it
was never executed on this host, there is no vulnerable dependency here to
find, and the identifier below is a placeholder, not a real advisory number:

```console
$ govulncheck ./...   # ILLUSTRATIVE -- not run on this host: no network, no vulnerable dependency committed
Scanning your code and P packages across M dependent modules for known vulnerabilities...

=== Symbol Results ===

Vulnerability #1: GO-XXXX-XXXXX
    <package>: <one-line summary of the advisory>
  More info: https://pkg.go.dev/vuln/GO-XXXX-XXXXX
  Module: <module path>
    Found in: <module path>@<version>
    Fixed in: <module path>@<fixed version>
    Example traces found:
      #1: <call chain from your code to the vulnerable symbol>
```

The two details worth carrying away from that shape: `govulncheck` reports a
stable `GO-xxxx-xxxxx` identifier per advisory (the same kind of durable
token `staticcheck`'s `U1000`/`SA4006` and `golangci-lint`'s `(unused)`
are — a name to grep for that survives wording changes across releases),
and it only ever flags a symbol your own code actually reaches (the
"Example traces found" call chain) — a vulnerable function sitting unused
in a dependency you happen to import produces no finding at all, since
nothing calls it. There is no `verify.lua` gate for `govulncheck` in this
example, on purpose: it is prose and workflow here, not a gated check.

## `delve` configs

Delve is Go's native debugger — DWARF-aware like `gdb`, but built with
goroutines as a first-class concept rather than bolted on. `go/.dlvinit` is
a committed, four-line headless-batch script, the same idea as Chapter 46's
`gdb` batch invocation, expressed in delve's own init-script format:

```
break main.go:33
continue
print d.FNV
quit
```

Line 33 of `main.go` is the `fmt.Printf` call inside `cmdReport`, one line
after `d := fnv1a(kPayload[:])` computes the digest:

```go
func cmdReport() int {
	d := fnv1a(kPayload[:])
	fmt.Printf("toolbox report: payload_len=%d digest=0x%016x\n", len(kPayload), d.FNV)
	return 0
}
```

Breaking on line 33 stops execution *after* `d` is assigned but *before*
its own line prints anything, which is exactly why `print d.FNV` has
something meaningful to show: delve renders `Digest`'s one field as a plain
64-bit unsigned decimal, `5195329438047200767` — the same bits as
`0x481984990deee5ff`, the identical value `toolbox report`'s own `%016x`
formatting and Chapter 46's `gdb` pretty-printer both already surfaced, this
time with no pretty-printer at all, because a single-field `uint64` needs no
help to read. The designed-for invocation a reader with `dlv` installed
would run:

```console
$ dlv exec ./bin/toolbox --init .dlvinit -- report
```

A debug binary built with optimizations and inlining turned off
(`go build -gcflags=all="-N -l" -o toolbox .`) is what `verify.lua` gate I
builds before invoking `dlv` this way — without `-N -l`, the compiler is
free to inline `fnv1a` into `cmdReport` and eliminate the local `d`
variable entirely, and there would be nothing left at line 33 for `break`
to stop on cleanly. `dlv` is absent on this reference host, so gate I is
shown, not verified: the designed-for assertion is that the batch session
above prints `5195329438047200767` and exits 0.

## How the code works

`main.go`'s whole dispatch is a three-way `switch` on `os.Args[1]`: `report`
computes and prints the FNV-1a digest above, `tools` prints the
`go generate`-produced table, and `defect divzero` seeds a deliberate
runtime panic — Go's analog of Chapter 46's UBSan-caught signed-integer
overflow, using the failure mode Go itself provides instead:

```go
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
```

`divisor` is a variable initialized to its zero value, not a literal `0` —
the same reason Chapter 46's `max_value` was declared `volatile`: a constant
division by a literal zero is a compile-time error in Go (`invalid
operation: division by zero`), so the defect has to survive as far as run
time to be the thing this section demonstrates, and a plain variable the
compiler cannot constant-fold does exactly that.

`digest.go` holds the `Digest` type and the `fnv1a` implementation quoted
above; `tools_generated.go` holds the generator's committed output;
`smells.go` and `vetdemo.go` hold the deliberate, never-called findings for
the static-analysis and `go vet` gates. Every one of those last three files
is isolated from the digest and from each other by a different mechanism —
`smells.go`'s functions are simply unreferenced, `vetdemo.go` sits behind a
`//go:build vetdemo` tag excluded from the default build entirely, and the
`testdata/` fixtures are ignored by `go build`/`go vet`/`go test` by
directory-name convention — so nothing about running `toolbox report` or
`toolbox tools` can ever depend on which of those designed-for smells has or
has not been "fixed."

{% include excalidraw.html
   file="47-go-tool-gates"
   alt="One Go module box splits into two gate lanes. The amber hard lane lists gofmt -l, go generate diff-clean, go vet -tags vetdemo naming a printf finding, go tool pprof -top naming hotLoop, and the FNV-1a digest -- all shipping inside the go binary and verifying offline. A dashed ghost lane beside it lists gofumpt, golangci-lint, staticcheck, and delve as gated-if-present, govulncheck marked network required, and benchstat, all shown as reference and not verified on this host."
   caption="Figure 47.2 — one module, two gate lanes: the go-native hard gates (gofmt, go generate, go vet, pprof, digest) that ship with the go binary and verify offline, beside the gated-if-present tools (gofumpt, golangci-lint, staticcheck, delve, govulncheck, benchstat) shown as reference" %}

## Errors, three ways

With one language and no compiler of its own to disagree with itself, "three
ways" here means three different *static* surfaces reading the same source
without ever running it — not three languages, and not a runtime trap,
since this chapter's one seeded panic (`defect divzero`) already got its own
section above. The first surface is the shallowest: `gofmt` (and, if
installed, `gofumpt`) notices only *shape* — whitespace, alignment, blank
lines — and says nothing about whether the code is correct, only whether it
matches one fixed, mechanical layout. `messy.go` above is not wrong in any
semantic sense; `gofmt -l` still lists it, because shape is the entire
question that tool answers.

The second surface is `go vet`, which ships with `go` itself and reads
*usage patterns the compiler allows but almost never means*. `vetdemo.go`,
excluded from the default build by its own build tag, is one deliberate
mismatch:

```go
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
```

The Go compiler accepts this without complaint — `Printf`'s format string
and its variadic arguments are only strings and `interface{}` values at the
type level, so there is nothing for the type checker itself to reject. `go
vet`'s printf analyzer is what actually reads the format verb against the
argument's real type and flags the mismatch, run for real on this host:

```console
$ go vet -tags vetdemo .
vetdemo.go:13:26: fmt.Printf format %d has arg "not-an-int" of wrong type string
```

The third surface goes deeper still: `staticcheck` and `golangci-lint`
(which bundles a copy of `staticcheck`) trace data flow and naming
convention rather than a single call's argument types — `smells.go`'s
`U1000`/`SA4006`/`ST1003` designed-for findings, above, are exactly that
class of thing, none of which `go vet` catches on its own. Three surfaces,
three different amounts of context each tool is willing to read before it
says anything: pure text shape, one function call's argument types, and a
whole package's data flow and naming — and each one earns its place by
catching something none of the others do.

## Concurrency lens

Chapter 44 spent a whole chapter on the GMP model: `GOMAXPROCS` P's are the
scarce resource that lets an M run Go code at all, goroutines are cheap, and
work-stealing moves half a queue at a time between P's that run dry. This
chapter's benchmark is the tooling side of that same picture — not *how*
the scheduler multiplexes work, but *how a reader measures whether it scaled
the way that model predicts*. `bench_test.go`'s `hotLoop` is a deliberately
dominant, purely CPU-bound function with no allocation and no function
calls inside its loop, so a sampling profiler has nowhere else to attribute
time to:

```go
func hotLoop(n int) uint64 {
	var x uint64 = 0x9e3779b97f4a7c15
	for i := 0; i < n; i++ {
		x ^= uint64(i)
		x *= 6364136223846793005
		x = x<<13 | x>>51
	}
	return x
}
```

`BenchmarkHotLoop` runs it serially; `BenchmarkHotLoopParallel` runs the
identical function through `b.RunParallel`, Go's built-in construct for
fanning a benchmark out across `GOMAXPROCS` goroutines at once:

```go
func BenchmarkHotLoopParallel(b *testing.B) {
	b.RunParallel(func(pb *testing.PB) {
		var local uint64
		for pb.Next() {
			local = hotLoop(hotLoopIters)
		}
		hotLoopSink = local
	})
}
```

`b.RunParallel` is not doing anything Chapter 44 didn't already name: each
of the goroutines it launches is a G, competing for one of `GOMAXPROCS` P's
the same way any other goroutines would, and `hotLoop`'s pure, allocation-free
loop is close to the best case for that model — no channel sends, no shared
mutable state, no lock contention, so there is nothing here for a P to block
on and nothing to steal from another P's queue. That is exactly why this
benchmark is the right one to compare `-cpu=1` against `-cpu=N` with: any
scaling loss that shows up would have to come from the scheduler's own
bookkeeping or from memory-hierarchy effects across cores, not from
goroutines waiting on each other — a workload engineered to isolate the
question `benchstat` is built to answer statistically rather than by eyeballing
one run's numbers.

## `pprof` and `benchstat`: the profiling workflow

`go tool pprof` is the go-native half of this pairing, and it is a hard,
verified gate here — no install required, because `runtime/pprof` and
`go tool pprof` both ship inside the same `go` binary as everything else in
this chapter's hard core. The workflow is three commands: build a CPU
profile from the benchmark, then read it with `-top`:

```console
$ go test -bench=. -run=^$ -cpuprofile=cpu.prof -o toolbox.test .
$ go tool pprof -top -nodecount=15 toolbox.test cpu.prof
      flat  flat%   sum%        cum   cum%
    18.57s 99.95% 99.95%     18.58s   100%  .../47-go-toolbox/go.hotLoop (inline)
         0     0% 99.95%      1.38s  7.43%  .../47-go-toolbox/go.BenchmarkHotLoop
         0     0% 99.95%     17.20s 92.57%  .../47-go-toolbox/go.BenchmarkHotLoopParallel.func1
```

`hotLoop` owns 99.95% of `flat` samples on its own, exactly as its design
intends, and the gate this chapter asserts is that name, `hotLoop`, appearing
in the top frames — never the timing next to it. Sample counts and elapsed
seconds vary with host load and iteration count from run to run; the
*identity* of the function actually spending the cycles does not, which is
why `verify.lua` gate E matches the string `hotLoop` and nothing else. That
same profile also shows `BenchmarkHotLoop` (7.43% cumulative) and
`BenchmarkHotLoopParallel.func1` (92.57% cumulative) as the two call paths
into `hotLoop` — both benchmarks ran during profiling, and `pprof` attributes
each sample to whichever one was actually on the stack at that instant.

`benchstat` is the tool that turns two sets of raw benchmark numbers into a
statistical comparison instead of an eyeballed diff — the workflow a reader
would use to ask "did this change actually help, or is that just noise":

```console
$ go test -bench=BenchmarkHotLoop -run=^$ -count=10 . > old.txt
$ go test -bench=BenchmarkHotLoop -run=^$ -count=10 . > new.txt
$ benchstat old.txt new.txt
```

`benchstat` is absent on this reference host, so that comparison is shown as
a workflow, not run. `verify.lua` gate K, if the tool were present, would
compare two runs of the *same* unmodified benchmark against each other and
assert only the table's **structure** — a `sec/op` column header and a row
naming `BenchmarkHotLoop` — deliberately not the numbers inside it, since two
runs of identical code will still differ in wall-clock time with host
scheduling noise, and this chapter's gates never assert a raw timing as
if it were a fixed literal. A reader comparing `-cpu=1` against `-cpu=N` for
`BenchmarkHotLoopParallel` would read the same table shape, with `benchstat`'s
own statistics (median, confidence interval) doing the job of saying whether
an observed difference is real or within noise — the same discipline Chapter
44's `GOGC`/`GOMEMLIMIT` comparisons used exact `NumGC` deltas to sidestep
entirely, here handled by a purpose-built statistics tool instead because
wall-clock benchmark timings have no exact-integer shortcut.

## Build, run, observe

```console
[host]$ cd examples/47-go-toolbox && ./demo.sh go build
```

That builds `bin/toolbox` from stdlib alone — no network, no `go.sum`.
Hand-running the hard-gated commands in sequence:

```console
[host]$ ./demo.sh go run report
toolbox report: payload_len=16 digest=0x481984990deee5ff
[host]$ cd go && go generate ./... && git diff --exit-code && cd ..
[host]$ gofmt -l $(find go -name '*.go' -not -path '*/testdata/*')
[host]$ cd go && go vet -tags vetdemo . && cd ..
vetdemo.go:13:26: fmt.Printf format %d has arg "not-an-int" of wrong type string
```

The full gate, exactly as run this session:

```console
[host]$ LSP_LANG=go REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
...
PASS 16 / FAIL 0
[host]$ python3 scripts/test-all-examples.py --only 47-go-toolbox
...
1 passed, 0 failed, 0 skipped
```

Sixteen real assertions, none of them a bare exit code: `go.mod`'s two
toolchain literals, the `report` digest against a fixed literal, the
copy-aside diff-clean check plus the generated table's count and one sorted
entry, `gofmt -l`'s empty-versus-flagged pair, `go vet`'s printf finding and
its file location, and the `pprof -top` name check — with `SKIP:` printed,
never a failure, for each of the five gated-if-present tools this host does
not have installed.

## Cross-check: the same digest, two languages apart

The clearest falsifiable claim in this chapter is not internal to Go at
all — it is the one this chapter shares with Chapter 46. `toolbox report`
here and `toolbox report` there hash a byte-for-byte identical 16-byte
payload with the byte-for-byte identical FNV-1a algorithm, and land on the
identical 64-bit value:

```console
[host]$ ./go/bin/toolbox report
toolbox report: payload_len=16 digest=0x481984990deee5ff
[host]$ ./cpp/build/release/toolbox report      # ch46, quoted there
toolbox report: label=[toolbox] report payload_len=16 digest=0x481984990deee5ff
```

`digest=0x481984990deee5ff` on both sides, produced by two toolchains that
share nothing else — a garbage-collected runtime versus a compiled binary
with no runtime of its own, a scheduler built around goroutines versus none
at all — because FNV-1a's definition constrains only integer arithmetic
over bytes, and integer arithmetic is one of the vanishingly few things
every conforming language implementation is required to agree on bit for
bit. The second cross-check is internal: the copy-aside `go generate` diff
and the idiomatic `git diff --exit-code` form check the identical property
two different ways — one without assuming a git checkout exists, one the
way a reader's own CI would actually gate it — and both report clean on
this tree, which is the same "two independent paths to the same answer"
discipline Chapter 46's GCC-vs-clang `diff` used, aimed here at a generator
agreeing with itself instead of two compilers agreeing with each other.

## Where this sits next to the book's other tooling chapters

Two earlier chapters already covered pieces of this ground, and this
chapter does not re-teach either:

- **Chapter 31** ("Per-Language Toolbelts") gave Go one column of a
  three-language *profiling breadth* comparison — `runtime/pprof`,
  `go tool trace`, and `dlv`, one paragraph each, next to C++'s `perf` and
  Rust's `cargo flamegraph`. This chapter is the opposite shape: it never
  touches `go tool trace` at all, and instead spends its depth budget on
  `dlv`'s **config authoring** (a committed `.dlvinit` headless-batch
  script, not just "delve exists") and the `pprof`-to-`benchstat`
  **workflow** — going from a raw CPU profile to a statistically
  defensible before/after comparison — ground Chapter 31 never had room for.
- **Chapter 44** ("The Go Runtime from a Systems Programmer's Seat") is
  where the GMP scheduler, the pacer, and the netpoller actually live —
  what `GOMAXPROCS`, work-stealing, and `SIGURG`-based preemption mean and
  why they behave as they do. This chapter assumes all of that and asks a
  narrower, tooling-shaped question on top of it: given that model, how does
  a reader *measure* whether a change to `hotLoop` actually changed
  anything, with `b.RunParallel` as the harness and `pprof`/`benchstat` as
  the instruments — the tooling that measures the runtime, not the runtime
  itself.

What is genuinely new here — the `go`/`toolchain` directive pair and
`GOTOOLCHAIN`'s resolution rule, `go generate` as a diff-clean CI discipline
with a self-contained generator, `gofmt`-clean-but-`gofumpt`-dirty as a
deliberately narrow fixture, `.golangci.yml`/`staticcheck` config authored
rather than merely invoked, and a committed `dlv` headless-batch script —
has no earlier chapter to point back to instead.

## What you learned

- **`go`/`toolchain` is two directives answering two different questions**:
  `go 1.26` pins the language-version floor every tool reads; `toolchain
  go1.26.5` pins the exact `go` binary release, resolved through
  `GOTOOLCHAIN=auto` with no network access needed when the installed
  binary already satisfies the pin — which it does on this host.
- **A generator's determinism has to be engineered, not assumed**:
  `internal/gen`'s `toolTable` is a Go map with no iteration-order
  guarantee, listed out of alphabetical order in source on purpose;
  `sort.Strings` plus `go/format`'s `format.Source` are what make
  `tools_generated.go` byte-identical across regenerations, which is the
  property `go generate ./... && git diff --exit-code` (and this chapter's
  copy-aside equivalent) actually gates.
- **`gofmt`, `gofumpt`, `go vet`, and `staticcheck`/`golangci-lint` read
  successively more context before saying anything**: pure text shape, one
  function call's argument types (`vetdemo.go:13`'s printf mismatch), and a
  whole package's data flow and naming (`smells.go`'s `U1000`/`SA4006`/
  `ST1003`) — three static surfaces, not three languages, and none of them
  ever executes the program.
- **`b.RunParallel` is Chapter 44's GMP model, applied as a measurement
  harness**: `hotLoop`'s allocation-free, lock-free loop is close to the
  best case for `GOMAXPROCS` P's to scale cleanly, which is exactly why it
  is the right benchmark to hand to `benchstat` for a statistically
  defensible `-cpu=1` vs `-cpu=N` comparison rather than an eyeballed one.
- **The same digest twice, in two languages that share nothing else**:
  `toolbox report`'s `0x481984990deee5ff` here matches Chapter 46's C++
  `toolbox report` byte for byte, because FNV-1a constrains only integer
  arithmetic over a fixed payload — one of the few things every conforming
  language implementation is required to agree on exactly.
- **Six real, committed tool configs, none of them run on this host**:
  `gofumpt`, `golangci-lint` (`.golangci.yml`), `staticcheck`, `govulncheck`,
  `dlv` (`.dlvinit`), and `benchstat` are all absent from this reference
  host; their sections above show real configs and designed-for findings,
  and the footer below marks exactly that gap and nothing more.

Part 13 continues with Rust's own toolchain, getting the same reference-depth
treatment this chapter and Chapter 46 gave C++ and Go.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.4-204.fc44, go1.26.5, offline, no
network access used):
<code>LSP_LANG=go REPO_ROOT=$(cd ../.. && pwd) lua verify.lua</code> reported
<code>PASS 16 / FAIL 0</code>, and
<code>python3 scripts/test-all-examples.py --only 47-go-toolbox</code>
reported <code>1 passed, 0 failed, 0 skipped</code>. Confirmed live and
folded into that PASS count, the go-native hard gates A-E: <code>go.mod</code>
pins <code>go 1.26</code> and <code>toolchain go1.26.5</code>;
<code>toolbox report</code> printed
<code>toolbox report: payload_len=16 digest=0x481984990deee5ff</code>,
byte-identical to Chapter 46's C++ digest; <code>go generate ./...</code>
regenerated <code>tools_generated.go</code> byte-for-byte against the
committed copy (copy-aside diff empty) and <code>toolbox tools</code>
printed the generated table's <code>count=10</code> plus its sorted
<code>gofmt</code> entry; <code>gofmt -l</code> was empty across every
tracked source and flagged <code>testdata/gofmt/messy.go</code>;
<code>go vet -tags vetdemo .</code> reported the printf mismatch at
<code>vetdemo.go:13:26</code>; and <code>go tool pprof -top
-nodecount=15</code> named <code>hotLoop</code> at 99.95% of flat samples
in a real <code>-cpuprofile</code> capture. Not exercised: <span
class="status status--unverified">unverified</span> — <code>gofumpt</code>,
<code>golangci-lint</code>, <code>staticcheck</code>, <code>govulncheck</code>,
<code>dlv</code>, and <code>benchstat</code> are all absent from this
reference host (confirmed by <code>command -v</code> for each), so gates
F, G, H, I, and K each printed an informational <code>SKIP:</code> rather
than running for real, and their sections above show committed configs
(<code>.golangci.yml</code>, <code>.dlvinit</code>) and designed-for
findings — the exact tokens (<code>(unused)</code>, <code>U1000</code>,
<code>SA4006</code>, the decimal <code>5195329438047200767</code>,
a <code>sec/op</code> table) a reader with those tools installed would see —
rather than a captured run. <code>govulncheck</code> is additionally
illustrative only: it needs network access to the Go vulnerability
database, this module has no vulnerable dependency to demonstrate against
by design, and no <code>govulncheck</code> gate exists in
<code>verify.lua</code> at all. <code>examples/manifest.yaml</code> marks
this example <code>mode: local</code> — no VM or LGTM path applies.</p>
