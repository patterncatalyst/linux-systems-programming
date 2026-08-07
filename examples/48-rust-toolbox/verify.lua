-- Verify 48-rust-toolbox (Rust only): the Rust toolchain itself is the
-- subject -- rustup/rust-toolchain.toml pinning, rustfmt, clippy, cargo
-- test, and the skip-if-present nextest/deny/llvm-cov/audit accelerators.
-- Rust's analog of ch46's C++ toolbox and ch47's Go toolbox. Every
-- assertion below checks a real, tool-produced effect, never merely a
-- process exit code.
--
-- Hard-gated (A-D): shipped by the pinned toolchain itself, deterministic,
-- fully offline, so they run on any host where `rustup` has resolved
-- rust-toolchain.toml. This core is larger than ch47's Go equivalent for a
-- structural reason: rustfmt and clippy are rustup *components* of the
-- pinned channel, so pinning the channel pins the formatter and the linter
-- too. Go's gofumpt and golangci-lint are separate downloads and could
-- only ever be gated-if-present.
--   A. rust-toolchain.toml pins channel "1.97.1" and Cargo.toml sets
--      edition "2024"; the resolved `rustc --version` really is 1.97.1;
--      `toolbox report` (an integer/string-only FNV-1a digest) equals a
--      known literal (0x481984990deee5ff -- the same value as ch46's C++
--      and ch47's Go toolbox, since all three reuse the identical 16-byte
--      payload).
--   B. `cargo fmt --check` is clean across the crate's module tree, and
--      `rustfmt --edition 2024 --check fixtures/misformatted.rs` exits
--      nonzero and prints a diff for the deliberately misformatted fixture
--      (kept outside the module tree so it can never affect the build).
--   C. `cargo clippy --all-targets -- -D warnings` is clean on the default
--      build, and `cargo clippy --features lintbait --all-targets --
--      -D warnings` fails and names the stable `needless_return` lint from
--      src/lintbait.rs. The gate is the lint NAME, never a diagnostic count.
--   D. `cargo test` runs the named digest unit test and reports
--      "test result: ok". The test asserts the digest literal, so any drift
--      in the payload or the FNV-1a implementation fails here first.
--
-- Gated-if-present (E, F, G, H): this host has none of cargo-nextest,
-- cargo-deny, or cargo-llvm-cov installed, and cargo-audit is installed but
-- its RustSec advisory database is not fetched -- all four are excluded by
-- this example's offline constraint (no `cargo install`, no network). Each
-- gate degrades to an informational "SKIP: ..." print, never a
-- checks.skip() (which would abort the whole script) and never a hard FAIL
-- -- A-D must still run regardless. A reader who has installed these tools
-- exercises the real gate against the committed deny.toml/clippy.toml.
--
-- Not gated at all: cargo-watch (interactive/never terminates), cargo-
-- flamegraph (needs perf + elevated perf_event_paranoid), and sccache (a
-- cache-hit-rate accelerator, not a correctness property). The chapter
-- shows all three; none is asserted. miri is cross-referenced to ch43/ch29,
-- which own it.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "rust" then
  checks.skip("48-rust-toolbox is Rust-only (LSP_LANG=" .. tostring(lang) .. ")")
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

local b = checks.run("./demo.sh rust build")
checks.expect_exit(b, 0, "rust: cargo build --offline succeeds (zero-dep, no network)")

local pin = checks.run("cat rust/rust-toolchain.toml")
checks.expect_match(pin.out, 'channel = "1%.97%.1"', "rust-toolchain.toml: pins channel 1.97.1")
checks.expect_match(pin.out, 'components = %["rustfmt", "clippy"%]',
  "rust-toolchain.toml: pins the rustfmt + clippy components")

local manifest = checks.run("cat rust/Cargo.toml")
checks.expect_match(manifest.out, 'edition = "2024"', "Cargo.toml: sets edition 2024")

local rustc = checks.run("cd rust && rustc --version")
checks.expect_exit(rustc, 0, "rust: rustc --version runs through the pin")
checks.expect_match(rustc.out, "rustc 1%.97%.1",
  "rust: the pin really resolves -- rustc reports 1.97.1")

local EXPECTED_REPORT = "toolbox report: payload_len=16 digest=0x481984990deee5ff"

local report = checks.run("./demo.sh rust run report")
checks.expect_exit(report, 0, "rust: toolbox report runs")
checks.expect_match(report.out, lit(EXPECTED_REPORT),
  "rust: toolbox report equals the expected literal (matches ch46's C++ and ch47's Go digest)")

-- ── B. rustfmt (clean on the tree, dirty on the isolated fixture) ────────

local fmt_tracked = checks.run("cd rust && cargo fmt --check 2>&1")
checks.expect_exit(fmt_tracked, 0, "rust: cargo fmt --check exits 0 on the crate's module tree")
checks.expect_match(fmt_tracked.out, "^%s*$",
  "rust: cargo fmt --check prints no diff for tracked sources")

local fmt_fixture = checks.run(
  "cd rust && rustfmt --edition 2024 --check fixtures/misformatted.rs 2>&1")
if fmt_fixture.exit == 0 then
  checks.expect_exit(fmt_fixture, 1,
    "rust: rustfmt --check exits nonzero on the misformatted fixture")
else
  print("ok: rust: rustfmt --check exits nonzero on the misformatted fixture")
end
checks.expect_match(fmt_fixture.out, "Diff in .*fixtures/misformatted%.rs",
  "rust: rustfmt --check prints a diff naming the fixture")
checks.expect_match(fmt_fixture.out, "pub fn classify%(n: i64%) %-> &'static str %{",
  "rust: the diff shows the reformatted signature rustfmt would write")

-- ── C. clippy (clean by default, fires the named lint on the bait) ───────

local clippy_clean = checks.run(
  "cd rust && cargo clippy --offline --all-targets -- -D warnings 2>&1")
checks.expect_exit(clippy_clean, 0,
  "rust: cargo clippy --all-targets -- -D warnings is clean on the default build")
checks.expect_match(clippy_clean.out, "Finished",
  "rust: the clean clippy run really checked the crate (cargo reports Finished)")

local clippy_bait = checks.run(
  "cd rust && cargo clippy --offline --features lintbait --all-targets -- -D warnings 2>&1")
if clippy_bait.exit == 0 then
  checks.expect_exit(clippy_bait, 101,
    "rust: clippy --features lintbait fails under -D warnings")
else
  print("ok: rust: clippy --features lintbait fails under -D warnings")
end
checks.expect_match(clippy_bait.out, "needless_return",
  "rust: clippy names the stable needless_return lint")
checks.expect_match(clippy_bait.out, "unneeded `return` statement",
  "rust: clippy emits the needless_return diagnostic text")
checks.expect_match(clippy_bait.out, "src/lintbait%.rs:%d+",
  "rust: the finding is located in src/lintbait.rs")

-- ── D. cargo test (the digest unit test) ─────────────────────────────────

local test = checks.run("cd rust && cargo test --offline 2>&1")
checks.expect_exit(test, 0, "rust: cargo test succeeds")
checks.expect_match(test.out, "test digest::tests::fnv1a_matches_cross_language_digest %.%.%. ok",
  "rust: the named digest test runs and passes")
checks.expect_match(test.out, "test result: ok%. 1 passed; 0 failed",
  "rust: cargo test reports the summary line, not just exit 0")

-- ── E. cargo-nextest (skip-if-absent) ────────────────────────────────────

if tool_present("cargo-nextest") then
  local nt = checks.run("cd rust && cargo nextest run --offline 2>&1")
  checks.expect_exit(nt, 0, "rust: cargo nextest run succeeds")
  checks.expect_match(nt.out, "fnv1a_matches_cross_language_digest",
    "rust: nextest names the digest test in its per-test output")
  checks.expect_match(nt.out, "1 test run: 1 passed",
    "rust: nextest reports its own summary line")
else
  print("SKIP: cargo-nextest not found on PATH -- gate E not asserted")
end

-- ── F. cargo-deny (skip-if-absent; offline-answerable subsets only) ──────

if tool_present("cargo-deny") then
  local dn = checks.run("cd rust && cargo deny check bans licenses sources 2>&1")
  checks.expect_exit(dn, 0,
    "rust: cargo deny check bans licenses sources passes against the committed deny.toml")
  checks.expect_match(dn.out, "bans ok", "rust: cargo deny reports the bans check as ok")
  checks.expect_match(dn.out, "licenses ok", "rust: cargo deny reports the licenses check as ok")
else
  print("SKIP: cargo-deny not found on PATH -- gate F not asserted")
end

-- ── G. cargo-llvm-cov (skip-if-absent; assert a FILENAME, never a %) ─────

if tool_present("cargo-llvm-cov") then
  local cov = checks.run("cd rust && cargo llvm-cov --offline --summary-only 2>&1")
  checks.expect_exit(cov, 0, "rust: cargo llvm-cov --summary-only runs cleanly")
  checks.expect_match(cov.out, "digest%.rs",
    "rust: the coverage summary names digest.rs as a covered file")
  -- Deliberately no percentage assertion: coverage numbers move with every
  -- line added to the crate, and gating on one would make the example fail
  -- for a reason that has nothing to do with the toolchain.
else
  print("SKIP: cargo-llvm-cov not found on PATH -- gate G not asserted")
end

-- ── H. cargo-audit (skip-if-absent AND skip-if-advisory-db-absent) ───────

if not tool_present("cargo-audit") then
  print("SKIP: cargo-audit not found on PATH -- gate H not asserted")
elseif checks.run("test -d \"$HOME/.cargo/advisory-db\"").exit ~= 0 then
  -- cargo-audit is installed here, but its first run would clone the
  -- RustSec advisory database over the network. Offline, that is a fetch
  -- this example refuses to make, so the gate stays unasserted rather than
  -- turning a network outage into a test failure.
  print("SKIP: ~/.cargo/advisory-db absent (offline; cargo-audit would fetch it)" ..
    " -- gate H not asserted")
else
  local audit = checks.run("cd rust && cargo audit --offline 2>&1")
  checks.expect_exit(audit, 0, "rust: cargo audit reports no vulnerabilities for the zero-dep crate")
  checks.expect_match(audit.out, "Loaded %d+ security advisories",
    "rust: cargo audit really loaded the advisory database")
end

-- ── shown-not-gated: cargo-watch, cargo-flamegraph, sccache, miri ────────

print("info: cargo-watch, cargo-flamegraph and sccache are shown in the chapter but not gated" ..
  " (interactive / needs perf privileges / cache-hit-rate is not a correctness property)")
print("info: miri is not re-taught here -- ch29 and ch43 own it")

checks.finish()
