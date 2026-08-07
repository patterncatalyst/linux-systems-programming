# 48 — rust-toolbox

A **single-language** example (Rust only): the Rust toolchain itself is the
subject, closing the three-chapter arc that ch46 (C++) and ch47 (Go) opened.
Every tool the chapter names asserts a real, tool-produced effect against
this crate — never a bare exit code.

- **`toolbox report`** — a deterministic FNV-1a digest over a fixed embedded
  16-byte payload, integer/string output only (no floats, addresses, timing,
  or hash-map iteration). The payload is byte-for-byte ch46's C++ and ch47's
  Go payload, so all three chapters land on the identical digest
  `0x481984990deee5ff` — a cross-appendix easter egg, not a coincidence.
- **`rust-toolchain.toml`** — pins `channel = "1.97.1"` plus the `rustfmt`
  and `clippy` components. The pin travels with the repository: `cargo` in
  this directory resolves to 1.97.1 whatever `rustup default` says globally.
- **`cargo fmt --check`** — clean across the crate's module tree;
  `fixtures/misformatted.rs` lives *outside* that tree (nothing `mod`-declares
  it) so only an explicit per-file `rustfmt --edition 2024 --check` reads it,
  and that must exit nonzero with a diff.
- **`cargo clippy --all-targets -- -D warnings`** — clean on the default
  build; `--features lintbait` compiles `src/lintbait.rs`'s deliberate
  `return y;` and fires the stable `needless_return` lint. The bait is *not*
  `#[allow]`-ed — allowing it would defeat the gate.
- **`cargo test`** — `src/digest.rs`'s `fnv1a_matches_cross_language_digest`
  asserts the digest literal, so drift in the payload or the hash fails here
  before it reaches the report gate.
- **`cargo-nextest`** / **`cargo-deny`** / **`cargo-llvm-cov`** /
  **`cargo-audit`** — gated *if present* against the committed `deny.toml`
  and `clippy.toml`.
- **`cargo-watch`**, **`cargo-flamegraph`**, **`sccache`** — shown in the
  chapter, never gated (interactive / needs `perf` privileges / a cache-hit
  rate is not a correctness property). **miri** is cross-referenced to ch29
  and ch43, which own it.

## Host reality: a bigger hard core than ch47, offline

This example was built and verified on a host with **only the pinned Rust
toolchain present** — `cargo-nextest`, `cargo-deny`, `cargo-llvm-cov` and
`sccache` are absent, `cargo-audit` is installed but its RustSec advisory
database is not fetched, and per this iteration's decision none of that is
installed or fetched (no network, no `cargo install`). The crate is
**zero-dependency**, so `cargo build --offline` really is offline.

The interesting asymmetry with ch47: **rustfmt and clippy are rustup
components of the pinned channel**, so pinning the channel pins the formatter
and the linter too. Go's `gofumpt` and `golangci-lint` are separate downloads
and could only ever be gated-if-present. That is why ch48's hard core (A–D)
covers formatting *and* linting offline, where ch47's could not.

Gates A–D always run. Gates E, F, G, H
(nextest/deny/llvm-cov/audit) degrade to an informational `SKIP: …` on this
host; a reader who has installed the tools exercises them for real. The
cargo-audit gate is guarded twice — on the binary *and* on
`~/.cargo/advisory-db` — so a missing database is a skip, not a failure.

## Run it

```bash
./demo.sh rust build             # zero-dep, --offline, no network
./demo.sh rust run report        # digest=0x481984990deee5ff

cd rust
cargo fmt --check                                        # clean, exits 0
rustfmt --edition 2024 --check fixtures/misformatted.rs  # diff, exits 1
cargo clippy --offline --all-targets -- -D warnings      # clean, exits 0
cargo clippy --offline --features lintbait --all-targets -- -D warnings
cargo test --offline                                     # the digest test
```

## Verify

`verify.lua` (Rust only; skips other langs) asserts gates A–D for real and
prints an informational `SKIP:` for any of E, F, G, H whose tool — or, for
cargo-audit, whose advisory database — is absent. It never hard-fails or
aborts on a missing optional tool.

```bash
LSP_LANG=rust REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

Mode: `local`. Zero-dependency — the `Cargo.lock` contains exactly one
package, `toolbox` itself.
