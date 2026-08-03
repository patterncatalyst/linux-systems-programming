---
title: "r16 / ch47 — go-toolbox — plan (internal)"
published: false
---

# r16 ch47 — example `47-go-toolbox` + chapter + 2 diagrams (lgtm-relay Phase 1)

Go analog of ch46. Single-language `langs: [go]`, `mode: local`. Part 13 "Appendices: Tooling".
Same "every tool asserts a real effect, never exit-0" contract.

## CRITICAL host finding
Only `go` (go1.26.5) is present. **gofumpt, golangci-lint, staticcheck, govulncheck, dlv, benchstat
ALL ABSENT.** check-host.sh: go hard-gated; golangci-lint/staticcheck/dlv warn-only; gofumpt/govulncheck/
benchstat not gated. CI = base golang only. → Hard core must be go-native; marquee tools are
gated-if-present and Phase 2 must `go install` them (network) to capture real firing output, else footer
degrades to unverified. verify.lua ALWAYS skip-if-present so CI stays green.

## Gate tiers
**Hard (go-native, deterministic, always run):**
- A. toolchain/version + digest: go.mod `go 1.26` + `toolchain go1.26.5`; `toolbox report` = integer/string-only
  FNV-1a digest == fixed literal (reuse ch46 kPayload → same `0x481984990deee5ff`, cross-appendix easter egg).
- B. `go generate` diff-clean: `//go:generate go run ./internal/gen` regenerates committed `tools_generated.go`;
  gate = regenerate → byte-identical. verify uses copy-aside-diff (no git needed); chapter shows `go generate ./... && git diff --exit-code`.
- C. gofmt: `gofmt -l` empty on tracked, nonempty on `testdata/gofmt/messy.go`.
- D. go vet: `//go:build vetdemo` Printf-mismatch file; `go vet -tags vetdemo .` reports stable `printf`.
- E. pprof structural: `go test -bench -cpuprofile` → `go tool pprof -top` names hot fn `hotLoop` (gate NAME not timings).

**Gated-if-present (absent on host; Phase 2 go-installs for capture; skip-if-absent, never abort):**
- F. gofumpt — `-l` flags `testdata/gofumpt/gofmt_ok.go` (gofmt-clean/gofumpt-dirty).
- G. golangci-lint — committed `go/.golangci.yml`; assert one linter name token on a line.
- H. staticcheck — assert `SAxxxx`/`U1000` ID token on a line.
- I. delve — debug binary `-gcflags=all=-N -l`, headless batch via `go/.dlvinit`; assert printed digest decimal `5202111637775040511`.
- J. govulncheck — **network**. Isolated `go-vuln/` sub-module (own go.mod/go.sum) pinning a known-vulnerable dep
  (x/text ParseAcceptLanguage, GO-20xx-xxxx; Phase 2 pins exact ver + reads exact GO-ID) calling the vulnerable
  symbol; assert stable `GO-xxxx-xxxx` token. Main module = clean contrast. Double-guard: tool present AND vuln.go.dev
  reachable. Never in default build/CI.
- K. benchstat — compare two `go test -bench` runs; assert table STRUCTURE only; ns/op shown-not-gated.

**Shown-not-gated:** raw bench numbers, `go tool trace`, gctrace, vuln counts.

**Fixture isolation (go compiles every .go in a dir):** `testdata/` (ignored by build/vet/test; tools target explicitly)
for gofmt/gofumpt fixtures; `//go:build vetdemo` tag for vet; unused unexported funcs in `smells.go` (compile-clean,
never called → don't touch digest) for staticcheck/golangci.

## Chapter spine (ch44/ch46 precedent, plain fenced go/yaml/console, no codetabs)
hook → Fig47.1 → Tools-used box → go toolchain/version mgmt (go/toolchain directives, GOTOOLCHAIN) → deterministic
build+digest → go generate in depth (self-contained generator, DO-NOT-EDIT, diff-clean) → gofmt vs gofumpt →
golangci-lint config authoring → staticcheck → govulncheck (isolated vuln module, GO-ID, network decision stated) →
delve configs (.dlvinit headless batch) → How the code works → Errors N ways (gofmt/vet/staticcheck/golangci = 4 static
surfaces) → Concurrency lens (benchstat -cpu=1 vs N of a RunParallel bench; cross-ref ch44 GMP) → pprof/benchstat
workflow → build/run/observe → cross-check → what you learned → status footer.

Differentiate (cross-ref, don't re-teach): ch31 (3-lang profiling breadth — pprof/trace/delve one-para each) →
ch47 deep on delve config authoring + pprof→benchstat workflow. ch44 (GMP/GC/netpoller internals) → ch47 = the
tooling that measures it. New: go/toolchain version mgmt, go-generate discipline, gofumpt-vs-gofmt, .golangci.yml/
staticcheck authoring, govulncheck advisory workflow.

Rejected: hard-gate absent tools (skip-if-present + Phase2 install); stringer for generate (keep gate hard w/ go run);
loose bad .go fixtures (testdata/+tags); vuln dep in main module (isolate in go-vuln/); assert message text/counts
(assert stable IDs); gate bench timings (gate pprof name + benchstat structure only).

## Steps
1. Scaffold+strip (new-example.sh, delete cpp/rust, single-lang demo.sh). BLOCKING.
2. Module sources+configs+fixtures under go/: main.go, digest.go, internal/gen/main.go + tools_generated.go(committed),
   bench_test.go(hotLoop+RunParallel), smells.go, vetdemo.go, testdata/gofmt/messy.go, testdata/gofumpt/gofmt_ok.go,
   .golangci.yml, .dlvinit, go/demo.sh, go.mod; isolated go-vuln/{go.mod,go.sum,main.go}; README.md. Depends 1.
3. verify.lua (A-E hard; F-K if-present via tool_present + informational SKIP, govulncheck network-guarded; copy-aside
   diff for generate) + demo.sh contract, LSP_LANG=go guard. Depends 2.
4. Local build+verify+capture: Phase 2 go-installs gofumpt/staticcheck/govulncheck/dlv/benchstat + golangci-lint
   (network; dangerouslyDisableSandbox for install/govulncheck only), run every gate REAL, pin digest literal + exact
   GO-xxxx/SAxxxx/linter tokens + line numbers + dlv decimal, confirm generate diff-clean + pprof hot-fn name. Gates prose.
5. Chapter _docs/47-go-toolbox.md. Depends 4. Parallel with 6.
6. 2 diagrams: 47-go-toolchain-pipeline (Fig47.1), 47-go-tool-gates (Fig47.2) + README rows. Depends 2. Parallel with 5.
7. manifest.yaml (langs:[go], mode:local, timeout 480) + catalogue. After 1. (manifest + diagrams/README = only shared files.)

## Acceptance criteria
1. no cpp/rust dir; manifest 47-go-toolbox langs:[go] mode:local no requires.
2. ./demo.sh go build exits 0 NO network (stdlib-only main module).
3. go.mod has `go 1.26` + `toolchain go1.26.5`; report == asserted digest literal.
4. go generate → byte-identical committed (git diff --exit-code clean; verify copy-aside diff clean).
5. verify.lua PASS N/FAIL 0; A-E each real effect (digest, generate-diff, gofmt list, vet printf, pprof name) — no bare exit-0.
6. F-K each fire (Phase2 installed → pinned token) OR informational SKIP — never abort/FAIL; A-E still run.
7. govulncheck (if run) → pinned GO-xxxx on go-vuln/, clean on main; go-vuln/ never in default build/CI.
8. front matter part=="Appendices: Tooling"; full spine.
9. every chapter go/yaml/console block = verbatim substring of source (or real captured transcript).
10. validate.py OK; two Figure 47.x includes; both diagrams catalogued.
11. banned-words clean; Tools-used box == tools exercised.
12. test-all-examples --only 47-go-toolbox PASS; footer status--verified w/ real toolchain + per-gate evidence, explicit
    status--unverified for any tool Phase 2 couldn't install.

## Risks
tools absent (Phase2 install + skip-if-present); govulncheck DB/network (isolated module + double guard); bench/pprof
non-determinism (gate name/structure only); ID drift (assert stable tokens, re-pin step4); generate non-determinism
(sorted keys + go/format); fixture leakage (testdata/+tags+unused funcs); dlv DWARF (-gcflags=all=-N -l); sandbox network
for go install (dangerouslyDisableSandbox install/govulncheck only).

## Verification outlook
PARTIALLY CLEAN. Hard core A-E fully CI-verifiable + offline-deterministic (go binary only). Marquee tools F-K absent on
host → depth depends on Phase 2 go-installing them (govulncheck needs network). If installed → footer like ch46 (all
firing, only benchstat timings shown). If not → those 6 sections legitimately status--unverified while hard core passes.
Merge safe either way; footer reflects exactly which optional tools Phase 2 installed.

## DECISION (user, gate): HARD-CORE ONLY (offline) + AUTONOMOUS merge.
- NO network, NO `go install`, NO committing a vulnerable dep, NO `go-vuln/` sub-module.
- VERIFIED on this host = go-native hard gates A-E only.
- F-K (gofumpt/golangci-lint/staticcheck/govulncheck/dlv/benchstat): ship real committed config
  artifacts (.golangci.yml, .dlvinit) + skip-if-present gates in verify.lua (a reader WITH the tools
  hits them; on this host they informational-SKIP). Sections written + shown, marked status--unverified.
- govulncheck: chapter prose/workflow + clearly-labeled ILLUSTRATIVE advisory output (not "from a run"),
  NOT gated, NO vulnerable dep in repo.
- verify.lua must PASS on A-E, informational-SKIP F-K, FAIL 0.
## Status
- [ ] S1 scaffold - [ ] S2 sources - [ ] S3 verify - [ ] S4 build+install+capture
- [ ] S5 chapter - [ ] S6 diagrams - [ ] S7 manifest - [ ] Phase3 validate - [ ] gate/PR
