# 47 — go-toolbox

A **single-language** example (Go only): the Go toolchain itself is the
subject, mirroring ch46's C++ toolbox. Every tool the chapter names asserts a
real, tool-produced effect against this module — never a bare exit code.

- **`toolbox report`** — a deterministic FNV-1a digest over a fixed embedded
  16-byte payload, integer/string output only (no floats, addresses, timing,
  or map iteration). The payload is byte-for-byte ch46's C++ payload, so both
  chapters land on the identical digest `0x481984990deee5ff` — a
  cross-appendix easter egg, not a coincidence.
- **`toolbox tools`** — prints `tools_generated.go`'s sorted table, proof
  that `go generate` produced real, checkable output.
- **`toolbox defect divzero`** — deliberately panics with a runtime integer
  divide-by-zero (narrative parity with ch46's UBSan-caught overflow).
- **`go generate`** (`internal/gen`, stdlib-only, `go/format`-clean) —
  regenerates `tools_generated.go` byte-identical to the committed copy.
- **`gofmt`** vs **`gofumpt`** — `testdata/gofmt/messy.go` is gofmt-dirty;
  `testdata/gofumpt/gofmt_ok.go` is gofmt-clean but gofumpt-dirty (a leading
  blank line inside a block).
- **`go vet -tags vetdemo .`** — `vetdemo.go` (excluded from the default
  build by its build tag) has a deliberate `Printf` format-verb mismatch.
- **`go tool pprof`** — `bench_test.go`'s `hotLoop` is a deliberately
  dominant hot function; the gate asserts pprof names it, never a timing.
- **`golangci-lint`** / **`staticcheck`** — `smells.go` has unused-function
  (`U1000`), dead-store (`SA4006`), and naming (`ST1003`) smells, never
  called at runtime so they cannot affect the digest.
- **`delve`** — `go/.dlvinit` is a committed headless-batch script: break,
  continue, print the digest field, quit.
- **`benchstat`** — `bench_test.go` also has a `b.RunParallel` benchmark for
  a `-cpu=1` vs `-cpu=N` comparison (concurrency lens).

## Host reality: hard core only, offline

This example was built and verified on a host with **only `go` (1.26.5)
present** — `gofumpt`, `golangci-lint`, `staticcheck`, `govulncheck`, `dlv`,
and `benchstat` are all absent, and per this iteration's decision, none are
installed (no network, no `go install`). The main module is **stdlib-only**
(no `go.sum`) and builds fully offline.

Gates A-E below are go-native and always run. Gates F, G, H, I, K
(gofumpt/golangci-lint/staticcheck/delve/benchstat) degrade to an
informational `SKIP: <tool> absent` on this host; a reader with the tools
installed exercises them for real. There is no `govulncheck` gate at all: it
needs network access to the Go vulnerability database and a deliberately
vulnerable dependency, both excluded here — the chapter covers it as
illustrative prose only, never gated, never committed.

## Run it

```bash
./demo.sh go build              # stdlib only, no network
./demo.sh go run report          # digest=0x481984990deee5ff
./demo.sh go run tools           # the go-generate-produced table
./demo.sh go run defect divzero  # panics on purpose (exit 2)

go generate ./... && git diff --exit-code   # idiomatic diff-clean check
gofmt -l $(find go -name '*.go' -not -path '*/testdata/*')  # empty
go vet -tags vetdemo ./go/...    # the seeded printf finding
```

## Verify

`verify.lua` (Go only; skips other langs) asserts gates A-E for real and
prints an informational `SKIP:` for any of F, G, H, I, K whose tool is
absent from `PATH` — it never hard-fails or aborts on a missing optional
tool.

```bash
LSP_LANG=go REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

Mode: `local`. Stdlib only — no external modules, no `go.sum`.
