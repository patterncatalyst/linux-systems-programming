---
title: "r17 / ch48 — rust-toolbox — plan (internal)"
published: false
---

# r17 ch48 — example `48-rust-toolbox` + chapter + 2 diagrams (lgtm-relay Phase 1)

Rust analog of ch46/ch47. Single-language `langs: [rust]`, `mode: local`. Part 13 "Appendices: Tooling" (FINAL ch of Part 13).
Same "every gate asserts a real effect, never exit-0" contract.

## Host audit
PRESENT: rustup, cargo/rustc 1.97.1, clippy 0.1.97, rustfmt 1.9.0, cargo-audit 0.20.0, cargo-watch, cargo-flamegraph.
ABSENT: cargo-nextest, cargo-deny, cargo-llvm-cov, sccache, miri. `~/.cargo/advisory-db` ABSENT (audit/deny need network).
KEY ASYMMETRY vs ch47: **clippy + rustfmt are pinned rust-toolchain.toml components → HARD-gateable** (present, like gofmt ships with go).

## Approach
One zero-dep binary crate `toolbox` (edition 2024, channel 1.97.1 via committed rust-toolchain.toml), offline default build.
`report` subcommand → FNV-1a digest over the SAME ch46/ch47 16-byte payload → `digest=0x481984990deee5ff` (cross-appendix
easter egg across all 3 languages). Also a #[cfg(test)] unit test asserting the literal.

### Gate tiers
- **A HARD** rustup/pin + digest: `rustc --version` has 1.97.1; Cargo.toml edition="2024"; report == literal.
- **B HARD** rustfmt: `cargo fmt --check` 0 on tracked; `rustfmt --edition 2024 --check fixtures/misformatted.rs` nonzero.
- **C HARD** clippy: `cargo clippy --all-targets -- -D warnings` 0 clean; `cargo clippy --features lintbait -- -D warnings`
  nonzero + names stable token `needless_return`.
- **D HARD** cargo test: named digest test → `fnv1a` test + `test result: ok` (test asserts digest literal → drift fails).
- **E gated-if-present** cargo-nextest: `cargo nextest run` names test + PASS (absent → SKIP).
- **F gated-if-present** cargo-deny: committed deny.toml; `cargo deny check bans licenses sources` (offline subsets) token.
- **G gated-if-present** cargo-llvm-cov: `--summary-only` → assert covered FILENAME (digest.rs), never a %.
- **H gated-if-present + network-guarded** cargo-audit: present but advisory-db absent → run only if db present/fetched;
  assert `0 vulnerabilities`/`Loaded N advisories` token on clean crate; else SKIP + illustrative prose.
- cargo-watch / cargo-flamegraph / sccache: SHOWN-NOT-GATED (interactive / needs perf+root / non-deterministic).
- miri: CROSS-REF ONLY (ch43/ch29 own it; do not re-teach).

verify.lua HARD-gates A-D only; E-H tool_present() skip-if-present informational SKIP (never checks.skip/abort/FAIL).

### cargo-audit network decision (like ch47 govulncheck)
Main crate zero-dep (clean contrast). Real advisory hit = isolated non-member `rust/audit-vuln/` sub-crate pinning a
RUSTSEC-YYYY-NNNN dep, built ONLY on online opt-in (never default/CI). Offline: audit-vuln/ NOT committed, gate SKIPs,
chapter shows clearly-labeled ILLUSTRATIVE RUSTSEC- advisory (durable token = the RUSTSEC- id).

### Fixture isolation (Rust)
- rustfmt fixture → `rust/fixtures/misformatted.rs` OUTSIDE module tree (rustfmt per-file, never compiled → can't break build).
- clippy bait → `#[cfg(feature="lintbait")]` module (default build/clippy clean; `--features lintbait` fires needless_return).

Rejected: real dep (breaks offline); #[allow] the bait (loses clean -D warnings pass); hard-gate absent tools; commit vuln
dep in main crate (isolate); gate coverage % (assert filename); re-teach miri (cross-ref).

## Steps
S1 scaffold+strip (new-example.sh, delete cpp/go, single-lang demo.sh). BLOCKING.
S2 sources+configs+fixtures under rust/: Cargo.toml(zero-dep, edition 2024, [features] lintbait), Cargo.lock,
   rust-toolchain.toml(channel 1.97.1, components rustfmt/clippy/llvm-tools/rust-src), src/main.rs, src/digest.rs
   (Digest+kPayload+fnv1a+#[cfg(test)] test), src/lintbait.rs, rustfmt.toml, clippy.toml, deny.toml, fixtures/misformatted.rs,
   rust/demo.sh, README.md. (online-only: rust/audit-vuln/ non-member, NOT committed offline.) Depends S1.
S3 verify.lua (A-D hard; E-H skip-if-present; audit db-guarded) + demo contract, LSP_LANG=rust. Depends S2.
S4 build+capture: offline → A-D PASS, E-H SKIP, FAIL 0; pin digest/needless_return/fmt/test tokens. (install-the-4 path only
   if user opts in: cargo install nextest/deny/llvm-cov/sccache + fetch advisory-db + build audit-vuln, re-pin tokens.) Depends S3.
S5 chapter _docs/48-rust-toolbox.md. Depends S4. Parallel S6.
S6 2 diagrams (48-rust-toolchain-pipeline Fig48.1, 48-rust-tool-gates Fig48.2) + README rows. Depends S2. Parallel S5.
S7 manifest (langs:[rust], mode:local, timeout 600). After S1.
Collisions: manifest.yaml (S7) + diagrams/README.md (S6) only.

## Acceptance criteria
1. no cpp/go dir; manifest 48-rust-toolbox langs:[rust] mode:local no requires.
2. ./demo.sh rust build exits 0 NO network (zero-dep).
3. Cargo.toml edition="2024"; rust-toolchain.toml channel="1.97.1"; rustc --version has 1.97.1.
4. `toolbox report` == `...digest=0x481984990deee5ff` (== ch46/ch47); digest.rs unit test asserts same literal.
5. cargo fmt --check 0 on tracked; rustfmt --check fixtures/misformatted.rs nonzero.
6. cargo clippy --all-targets -- -D warnings 0 clean; --features lintbait nonzero + names needless_return.
7. verify.lua PASS N/FAIL 0; A-D real effects (no bare exit-0); E-H fire-with-token OR informational SKIP, never abort/FAIL.
8. cargo-audit gate runs only if tool+db present; audit-vuln/ (if exists) never in default build/CI.
9. front matter part=="Appendices: Tooling"; full spine (Tools-used, Errors-3-ways, Concurrency lens, cross-check, footer).
10. every chapter rust/toml/console block = verbatim substring of source or real transcript.
11. validate.py OK; two Figure 48.x includes; both diagrams catalogued.
12. banned-words clean; Tools-used box == tools exercised; test-all-examples --only 48-rust-toolbox PASS; footer status--verified
    reflects A-D (+ any installed tool), explicit status--unverified for tools not run.

## Risks
toolchain fetch on build if CI lacks 1.97.1 (same pin as ch43, established); audit/deny advisory-db network (db-guard, SKIP
offline); coverage%/flamegraph/sccache non-determinism (filename/prose only); host-missing tools (skip-if-present); clippy
lint/version drift (pin channel, assert lint NAME); fixture leakage (fmt outside tree, bait feature-gated); flamegraph root/perf
(shown-not-gated).

## Verification outlook (depth gate — 2 scenarios)
Offline baseline STRONGER than ch47: clippy+rustfmt pinned components → present → hard core A-D = pin+digest, rustfmt clean/dirty,
clippy clean+lint-name, cargo test = 5 real gates, NO network. Install-decision affects only E-H + accelerators.
- Scenario 1 OFFLINE HARD-CORE (ch47's chosen path): committed configs + skip-if-present; PASS A-D, SKIP E-H, FAIL 0;
  audit illustrative; no audit-vuln committed. Footer: A-D verified; nextest/deny/llvm-cov/audit/sccache/flamegraph/watch unverified.
- Scenario 2 INSTALL-THE-4 (opt-in, network): cargo install nextest/deny/llvm-cov/sccache + advisory-db + build audit-vuln;
  E-H fire (nextest PASS, deny token, llvm-cov filename, RUSTSEC-id). Footer near ch46 fullness; only flamegraph/watch shown-not-gated.
Merge-safe either way; verify.lua identical; footer states which optional tools ran.

## DECISION (user, gate): OFFLINE HARD-CORE + AUTONOMOUS merge.
- NO network, NO cargo install, NO audit-vuln/ sub-crate, NO vulnerable dep committed.
- VERIFIED = A-D hard gates (pin+digest, rustfmt clean/dirty, clippy clean + needless_return, cargo test).
- E-H (nextest/deny/llvm-cov/audit): ship committed configs (deny.toml/clippy.toml/rustfmt.toml) + skip-if-present
  gates (absent/db-absent → informational SKIP). Sections shown-as-reference, status--unverified.
- cargo-audit: present on host but advisory-db absent → SKIP offline; chapter shows ILLUSTRATIVE RUSTSEC- advisory.
- verify.lua PASS A-D, SKIP E-H, FAIL 0.
## Status
- [x] S1 - [x] S2 - [x] S3 - [x] S4 - [x] S5 - [x] S6 - [x] S7 - [ ] Phase3 - [ ] gate/PR

## Execution deltas from the plan
- **`rust-toolchain.toml` components trimmed to `["rustfmt", "clippy"]`.** The planned
  `llvm-tools`/`rust-src` entries made rustup fetch a missing component over the network on the
  first `cargo` invocation (observed: `info: downloading component llvm-tools`), which breaks the
  OFFLINE HARD-CORE decision's `cargo build --offline` claim on a fresh host. Both are now
  documented as an explicit `rustup component add` opt-in in the file's comment and in the chapter's
  "What is deliberately *not* pinned" subsection. Diagram `48-rust-toolchain-pipeline` (SVG +
  .excalidraw) relabelled `rustfmt · clippy components` to match.
- Gate A widened to also assert `rustc --version` == 1.97.1 through the pin (plan had it; recorded
  here because it is what proves the pin *resolves*, not merely that the file says so).
- No `defect` subcommand (ch46 UBSan / ch47 divzero analog): "Errors, three ways" is three *static/
  behavioral* surfaces (rustfmt shape → clippy idiom + `-D warnings` severity promotion → `cargo
  test` behavior), plus the deliberate no-`Result` design note. A seeded runtime panic would have
  duplicated ch47 without adding a Rust-specific observation.
- Concurrency lens = tooling concurrency: cargo build-graph `-j` + release codegen-units, libtest's
  thread-per-test model (`--test-threads=1`, captured), and nextest's process-per-test isolation.

## Gate matrix (host run, 2026-08-04 → 08-07, Fedora 44, kernel 7.1.5-201.fc44, offline)
| gate | result |
| --- | --- |
| `lua verify.lua` (LSP_LANG=rust) | **PASS 20 / FAIL 0**, 4 informational SKIP (E-H) |
| `test-all-examples.py --only 48-rust-toolbox` | 1 passed, 0 failed, 0 skipped |
| `validate.py` | OK |
| chapter source blocks verbatim | 15/15 (16th is the `[host]$` command list) |
| banned words (honest/lie) | clean |
| cross-language digest | cpp/go/rust all `digest=0x481984990deee5ff` (all three binaries re-run) |
| A pin+digest / B rustfmt / C clippy / D cargo test | all fire real effects, both directions |
| E nextest / F deny / G llvm-cov / H audit | SKIP (absent / advisory-db absent) — never FAIL |
