-- Verify 47-go-toolbox (Go only): the Go toolchain itself is the subject --
-- go/toolchain version pinning, go generate, gofmt/gofumpt, go vet, pprof,
-- golangci-lint/staticcheck config authoring, delve, benchstat. Go's analog
-- of ch46's C++ toolbox. Every assertion below checks a real, tool-produced
-- effect, never merely a process exit code.
--
-- Hard-gated (A-E): go-native, deterministic, offline, always run on any
-- host with just `go` installed.
--   A. go.mod pins "go 1.26" + "toolchain go1.26.5"; `toolbox report` (an
--      integer/string-only FNV-1a digest) equals a known literal
--      (0x481984990deee5ff -- the same value as ch46's C++ toolbox, since
--      both reuse the identical 16-byte payload).
--   B. `//go:generate go run ./internal/gen` regenerates tools_generated.go
--      byte-identical to the committed copy. Checked without git: the
--      committed file is copied aside, regenerated, diffed against the
--      saved copy (must be empty), then restored so the working tree is
--      never left dirty by this check. `toolbox tools` output (the
--      generated table) is also asserted against a literal.
--   C. `gofmt -l` is empty across every tracked .go file, and non-empty on
--      the deliberately misformatted testdata/gofmt/messy.go fixture.
--   D. `go vet -tags vetdemo .` reports the stable "printf" finding from
--      vetdemo.go's deliberate %d/string mismatch (a build-tagged file
--      excluded from the default build/test).
--   E. `go test -bench -cpuprofile` + `go tool pprof -top` names the
--      deliberately dominant hotLoop function in the top CPU-profile
--      frames. The gate is the NAME, never a timing.
--
-- Gated-if-present (F, G, H, I, K): this host has none of gofumpt,
-- golangci-lint, staticcheck, delve, or benchstat installed (offline
-- constraint -- no `go install`, no network). Each gate degrades to an
-- informational "SKIP: <tool> absent" print, never a checks.skip() (which
-- would abort the whole script) and never a hard FAIL -- A-E must still run
-- regardless. A reader who has these tools installed exercises the real
-- gate. No govulncheck gate: it needs network access to the Go vulnerability
-- database and a deliberately vulnerable dependency, both excluded by this
-- example's offline, stdlib-only, no-committed-vulnerability constraints;
-- the chapter covers govulncheck as illustrative prose only.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "go" then
  checks.skip("47-go-toolbox is Go-only (LSP_LANG=" .. tostring(lang) .. ")")
end

-- Escape a literal string for use inside a Lua pattern (checks.expect_match
-- takes a pattern, not a plain substring).
local function lit(s)
  return (s:gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1"))
end

local function tool_present(cmd)
  return checks.run("command -v " .. cmd .. " >/dev/null 2>&1").exit == 0
end

-- ── A. toolchain pin + deterministic digest ──────────────────────────────

local b = checks.run("./demo.sh go build")
checks.expect_exit(b, 0, "go: builds (stdlib only, no network)")

local gomod = checks.run("cat go/go.mod")
checks.expect_match(gomod.out, "\ngo 1%.26\n", "go.mod: pins \"go 1.26\"")
checks.expect_match(gomod.out, "toolchain go1%.26%.5", "go.mod: pins \"toolchain go1.26.5\"")

local EXPECTED_REPORT = "toolbox report: payload_len=16 digest=0x481984990deee5ff"

local report = checks.run("./demo.sh go run report")
checks.expect_exit(report, 0, "go: toolbox report runs")
checks.expect_match(report.out, lit(EXPECTED_REPORT),
  "go: toolbox report equals the expected literal (matches ch46's C++ digest)")

-- ── B. go generate diff-clean + generated-table literal ──────────────────

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

local EXPECTED_TOOLS_HEAD = "tools: count=10"
local EXPECTED_TOOLS_LINE = "tools: gofmt         -- format Go source files"

local tools = checks.run("./demo.sh go run tools")
checks.expect_exit(tools, 0, "go: toolbox tools runs")
checks.expect_match(tools.out, lit(EXPECTED_TOOLS_HEAD),
  "go: toolbox tools reports the generated table's count")
checks.expect_match(tools.out, lit(EXPECTED_TOOLS_LINE),
  "go: toolbox tools includes a generated, sorted table entry")

-- ── C. gofmt ──────────────────────────────────────────────────────────────

local fmt_tracked = checks.run(
  "cd go && gofmt -l $(find . -name '*.go' -not -path './testdata/*')")
checks.expect_exit(fmt_tracked, 0, "go: gofmt -l exits 0 on tracked sources")
checks.expect_match(fmt_tracked.out, "^%s*$",
  "go: gofmt -l prints nothing for tracked sources")

local fmt_fixture = checks.run("cd go && gofmt -l testdata/gofmt/messy.go")
checks.expect_match(fmt_fixture.out, "testdata/gofmt/messy%.go",
  "go: gofmt -l flags the deliberately misformatted fixture")

-- ── D. go vet (build-tagged printf mismatch) ─────────────────────────────

local vet = checks.run("cd go && go vet -tags vetdemo . 2>&1")
checks.expect_match(vet.out, "Printf format %%d has arg .* of wrong type string",
  "go: go vet -tags vetdemo reports a printf finding")
checks.expect_match(vet.out, "vetdemo%.go:%d+", "go: the printf finding is located in vetdemo.go")

-- ── E. pprof (structural: the hot function's NAME, not a timing) ────────

local pprof_check = [[
cd go
tmpd=$(mktemp -d)
go test -bench=. -run=^$ -cpuprofile="$tmpd/cpu.prof" -o "$tmpd/toolbox.test" . >/dev/null
go tool pprof -top -nodecount=15 "$tmpd/toolbox.test" "$tmpd/cpu.prof"
status=$?
rm -rf "$tmpd"
exit $status
]]
local pprof = checks.run(pprof_check)
checks.expect_exit(pprof, 0, "go: go test -bench -cpuprofile + go tool pprof -top run cleanly")
checks.expect_match(pprof.out, "hotLoop", "go: pprof -top names hotLoop in the CPU-profile top frames")

-- ── F. gofumpt (skip-if-absent) ──────────────────────────────────────────

if tool_present("gofumpt") then
  local fumpt = checks.run("cd go && gofumpt -l testdata/gofumpt/gofmt_ok.go")
  checks.expect_match(fumpt.out, "testdata/gofumpt/gofmt_ok%.go",
    "go: gofumpt -l flags the gofmt-clean/gofumpt-dirty fixture")
else
  print("SKIP: gofumpt not found on PATH -- gate F not asserted")
end

-- ── G. golangci-lint (skip-if-absent) ────────────────────────────────────

if tool_present("golangci-lint") then
  local lintr = checks.run("cd go && golangci-lint run ./... 2>&1")
  checks.expect_match(lintr.out, "%(unused%)",
    "go: golangci-lint flags smells.go's unused functions via the \"unused\" linter")
else
  print("SKIP: golangci-lint not found on PATH -- gate G not asserted")
end

-- ── H. staticcheck (skip-if-absent) ──────────────────────────────────────

if tool_present("staticcheck") then
  local sc = checks.run("cd go && staticcheck ./... 2>&1")
  checks.expect_match(sc.out, "U1000", "go: staticcheck flags smells.go's unused functions (U1000)")
  checks.expect_match(sc.out, "SA4006", "go: staticcheck flags deadStore's dead store (SA4006)")
else
  print("SKIP: staticcheck not found on PATH -- gate H not asserted")
end

-- ── I. delve headless batch via .dlvinit (skip-if-absent) ────────────────

if tool_present("dlv") then
  local dlv_check = [[
cd go
tmp=$(mktemp)
go build -gcflags=all="-N -l" -o "$tmp" .
dlv exec "$tmp" --init .dlvinit -- report
status=$?
rm -f "$tmp"
exit $status
]]
  local dlv = checks.run(dlv_check)
  checks.expect_exit(dlv, 0, "go: dlv headless batch via .dlvinit runs cleanly")
  checks.expect_match(dlv.out, "5195329438047200767",
    "go: dlv prints the digest as a plain decimal (== 0x481984990deee5ff)")
else
  print("SKIP: dlv (delve) not found on PATH -- gate I not asserted")
end

-- ── K. benchstat (skip-if-absent; table STRUCTURE only, timings shown-not-gated) ──

if tool_present("benchstat") then
  local bench_check = [[
cd go
a=$(mktemp)
c=$(mktemp)
go test -bench=BenchmarkHotLoop -run=^$ -count=5 . > "$a"
go test -bench=BenchmarkHotLoop -run=^$ -count=5 . > "$c"
benchstat "$a" "$c"
status=$?
rm -f "$a" "$c"
exit $status
]]
  local bs = checks.run(bench_check)
  checks.expect_exit(bs, 0, "go: benchstat compares two BenchmarkHotLoop runs cleanly")
  checks.expect_match(bs.out, "sec/op", "go: benchstat emits a sec/op comparison table")
  checks.expect_match(bs.out, "HotLoop", "go: benchstat's table names BenchmarkHotLoop")
else
  print("SKIP: benchstat not found on PATH -- gate K not asserted")
end

-- ── govulncheck: no gate (network + vuln dep both excluded; prose only) ──

print("info: govulncheck not gated -- offline/no-committed-vulnerability constraints" ..
  " (see chapter for the illustrative advisory workflow)")

checks.finish()
