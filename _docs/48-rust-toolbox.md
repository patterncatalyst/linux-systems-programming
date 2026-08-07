---
title: "The Rust toolbox: rust-toolchain.toml pins, rustfmt and clippy as toolchain components, and the cargo-test-to-nextest workflow"
order: 48
part: "Appendices: Tooling"
description: "rust-toolbox is one zero-dependency crate and the toolchain wrapped around it -- rust-toolchain.toml pinning channel and components so rustup resolves the same rustc, rustfmt, and clippy for everyone, an isolated misformatted fixture and a feature-gated clippy bait that make the format and lint gates assert real findings, and cargo test asserting the digest literal -- with committed deny.toml and clippy.toml for the cargo-nextest/cargo-deny/cargo-llvm-cov/cargo-audit tier shown as reference but absent on the host. Verified on the Fedora 44 host: verify.lua PASS 20/FAIL 0 fully offline, toolbox report digest=0x481984990deee5ff (byte-identical to ch46's C++ and ch47's Go digest), cargo fmt --check clean while rustfmt flags the fixture, cargo clippy -D warnings clean by default and naming needless_return under --features lintbait, and cargo test passing the digest unit test."
duration: "50 minutes"
---

Chapter 46 pointed every C++ tool it could find at one small binary; Chapter
47 did the same for Go and found the depth had moved — out of the build
system and into `go.mod`'s own toolchain directives. Rust closes the arc, and
it moves the line one step further in the same direction. Go pins its
compiler in the module file; Rust pins its compiler *and its formatter and
its linter*, in a file `rustup` reads before `cargo` has done anything at
all. That single fact reorganizes this whole chapter. In Chapter 47,
`gofumpt`, `golangci-lint`, and `staticcheck` had to be gated-if-present,
because they are separate downloads that a given host may simply not have. In
Rust, `rustfmt` and `clippy` are *components of the pinned channel* — if the
pin resolves, they are there, at exactly the version the pin names. So the
formatter and the linter graduate out of the "shown as reference" tier and
into the hard, offline, always-runs core. `toolbox` (the Rust one) is a
single zero-dependency crate engineered the same way its C++ and Go siblings
were: every tool pointed at it has something concrete to catch or confirm,
never just a clean exit.

{% include excalidraw.html
   file="48-rust-toolchain-pipeline"
   alt="A rust-toolchain.toml box listing channel 1.97.1, edition 2024, and the rustfmt and clippy components feeds into a rustup box that resolves the pinned toolchain and installs it if missing, which in turn drives a cargo/rustc 1.97.1 box. From cargo/rustc two arms fan out: a deterministic build arm running cargo run -- report, producing the zero-dependency toolbox binary and an FNV-1a digest over a fixed payload, landing in an amber digest gate asserting 0x481984990deee5ff and noting the same value appears in the C++ and Go chapters; and a cargo test arm running the #[cfg(test)] suite in digest.rs, landing in an amber cargo-test gate that asserts the same literal so any drift fails. Both gates are labeled rust-native and verified offline on this host."
   caption="Figure 48.1 — the Rust toolchain and pinning flow: rust-toolchain.toml's channel and components resolve through rustup into cargo/rustc, which drives the deterministic build (report digest) and the cargo test gate" %}

> **Tools used** — `rustup` (host; resolves `rust-toolchain.toml` before
> every `cargo` invocation, gated as a hard requirement by
> `scripts/check-host.sh`'s "rustc via rustup" check, which fails outright
> if `rustup` is missing and warns if the resolved version is not 1.97.1),
> `cargo` and `rustc` (host; `scripts/check-host.sh` gates both), `rustfmt`
> and `cargo fmt` (host; a component of the pinned channel, not separately
> gated by `check-host.sh` because pinning the channel pins it), `cargo
> clippy` (host; likewise a pinned component), `cargo test` (host; ships
> inside `cargo`). `cargo-nextest`, `cargo-deny`, `cargo-llvm-cov`, and
> `sccache` are absent on this reference host and are not installed;
> `cargo-audit`, `cargo-watch`, and `cargo-flamegraph` are present but not
> exercised as gates (no advisory database offline / interactive / needs
> `perf` privileges), so all seven of those sections below are shown as
> reference, not verified. No VM, no root, no LGTM stack —
> `examples/48-rust-toolbox` is `mode: local` in `examples/manifest.yaml`.

Because the Rust toolchain is itself the subject, `examples/48-rust-toolbox/`
ships `langs: [rust]` only, the same way Chapter 43's macro chapter, Chapter
46's C++ chapter, and Chapter 47's Go chapter each stayed inside one
language — a C++ or Go build has nothing to say about a
`rust-toolchain.toml` component list or a `clippy.toml` MSRV. Every code
block below is a plain fenced `rust`/`toml`/`console` block; there is no
`codetabs` include to reach for when only one language is in the room.

## The pin: `rust-toolchain.toml`

The file is nineteen lines, and only the last three do anything:

```toml
[toolchain]
channel = "1.97.1"
components = ["rustfmt", "clippy"]
```

`channel` is the part everybody knows: it names an exact release, so
`cargo build` in this directory compiles with 1.97.1 whatever the developer's
`rustup default` happens to be. What makes the file more than a version
string is `components`. `rustfmt` and `clippy` are not separate projects that
happen to work with Rust — they are artifacts `rustup` downloads *for a
specific toolchain*, versioned in lockstep with it. Listing them here says
"this repository does not merely need Rust 1.97.1, it needs 1.97.1's
formatter and 1.97.1's linter," and `rustup` will install them the first time
anything in this directory invokes `cargo`.

The resolution is not a convention this chapter is asserting; `rustup` will
tell you about it directly. Run inside `examples/48-rust-toolbox/rust`, on a
host whose global default is `stable` and which also has `nightly` and
`1.96.0` sitting around:

```console
$ rustup show
Default host: x86_64-unknown-linux-gnu
rustup home:  /home/rsedor/.rustup

installed toolchains
--------------------
stable-x86_64-unknown-linux-gnu (default)
nightly-x86_64-unknown-linux-gnu
1.96.0-x86_64-unknown-linux-gnu
1.97.1-x86_64-unknown-linux-gnu (active)

active toolchain
----------------
name: 1.97.1-x86_64-unknown-linux-gnu
active because: overridden by '/home/rsedor/Dev/linux-systems-programming/examples/48-rust-toolbox/rust/rust-toolchain.toml'
installed targets:
  x86_64-unknown-linux-gnu
```

`active because: overridden by '…/rust-toolchain.toml'` is the entire
mechanism, stated by the tool. Step one directory outside the crate and the
override evaporates:

```console
$ cd /tmp && rustup show active-toolchain
stable-x86_64-unknown-linux-gnu (default)
```

That is the property Chapter 47 got from `GOTOOLCHAIN` and Chapter 46 had to
build by hand out of seven `CMakePresets.json` configurations: the version
decision lives with the source, not with whoever checked it out. What Rust
adds on top is that the *tools* travel with it too. Every binary in the chain
reports the same release:

```console
$ cargo --version
cargo 1.97.1 (c980f4866 2026-06-30)
$ rustc --version
rustc 1.97.1 (8bab26f4f 2026-07-14)
$ cargo clippy --version
clippy 0.1.97 (8bab26f4f6 2026-07-14)
$ cargo fmt --version
rustfmt 1.9.0-stable (8bab26f4f6 2026-07-14)
```

Note the commit hashes: `rustc`, `clippy`, and `rustfmt` all report
`8bab26f4f6` — one build of one source tree. A clippy lint's exact wording,
a rustfmt rule's exact output, and the compiler that accepts the result are
all the same snapshot. That is why the gates below can assert on specific
diagnostic text without being fragile: the pin holds the diagnostic still.

### What is deliberately *not* pinned

`components` lists exactly two entries, and the omissions are as deliberate
as the inclusions. `llvm-tools` (needed by `cargo-llvm-cov`) and `rust-src`
(needed by miri and rust-analyzer) are left out, because `rustup` fetches any
missing component **over the network, on the first `cargo` invocation in this
directory**. Pinning a component a reader does not have turns `cargo build
--offline` into a download — which is exactly the failure this example exists
to avoid. The chapter's own draft hit it: an earlier `components` list
included `llvm-tools`, and the first build in a fresh checkout printed
`info: downloading component llvm-tools` before compiling anything. The file
documents the opt-in instead:

```toml
# llvm-tools (needed by cargo-llvm-cov) and rust-src (needed by miri and
# rust-analyzer) are deliberately NOT pinned here: rustup fetches any missing
# component over the network on the first cargo invocation in this directory,
# which would break the offline build on a fresh host. Add them explicitly
# when you opt into those tools:
#
#   rustup component add llvm-tools rust-src
```

That is the general rule for this file: pin what every build needs, and make
everything else a documented command a reader runs on purpose.

## Deterministic build and digest

`Cargo.toml` is short by design, and its most important section is the one
that is not there:

```toml
[package]
name = "toolbox"
version = "0.1.0"
edition = "2024"

[[bin]]
name = "toolbox"
path = "src/main.rs"

[features]
# Feature-gated clippy bait (src/lintbait.rs). Off by default so the
# default build and `cargo clippy --all-targets -- -D warnings` are clean;
# `cargo clippy --features lintbait -- -D warnings` compiles it and fires
# the stable `needless_return` lint on purpose (ch48's clippy gate).
lintbait = []
```

There is no `[dependencies]`. The crate is zero-dependency on purpose, and
`cargo tree` is the proof:

```console
$ cargo tree --offline
toolbox v0.1.0 (/home/rsedor/Dev/linux-systems-programming/examples/48-rust-toolbox/rust)
```

One node. `Cargo.lock` holds exactly one package — itself. That is not
minimalism for its own sake: it is what makes `--offline` a meaningful flag
rather than a hopeful one. `rust/demo.sh` passes it on every build:

```bash
# --offline on purpose: toolbox is a zero-dependency crate, so a correct
# build never needs the network. Passing --offline turns "cargo quietly
# reached crates.io" from an invisible event into a hard build failure.
build() {
  cargo build --offline --release
}
```

`edition = "2024"` is the other line worth stopping on. Editions are Rust's
mechanism for changing language semantics without breaking old code: a 2024
crate and a 2015 crate can link together in one binary, each compiled under
its own rules. Editions are per-crate, the channel is per-repository, and
they are set in different files — `Cargo.toml` and `rust-toolchain.toml`
respectively. Both of ch48's gates check both, because getting one right and
the other wrong is the common failure: a 2024 crate compiled by a toolchain
too old to know that edition fails with a confusing error about an unknown
edition, not an obviously-wrong one.

The payload and the hash are lifted byte-for-byte from Chapter 46's C++ and
Chapter 47's Go toolbox, which is the whole point:

```rust
/// ch46/ch47's exact 16-byte payload (`"The quick brown."`), reused
/// byte-for-byte so the C++, Go, and Rust toolbox chapters land on the
/// identical FNV-1a digest (`0x481984990deee5ff`) -- a cross-appendix
/// easter egg, not a coincidence. FNV-1a is language-independent: any
/// conforming implementation over the same bytes produces the same 64-bit
/// value.
pub const PAYLOAD: [u8; 16] = [
    0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f, 0x77, 0x6e, 0x2e,
];

/// FNV-1a over `data`. Integer-only, no floats, addresses, timing, or
/// hash-map iteration -- the same input bytes produce the same digest
/// bit-for-bit on every conforming Rust toolchain, every architecture,
/// every run.
pub fn fnv1a(data: &[u8]) -> Digest {
    let mut h = FNV_OFFSET_BASIS;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(FNV_PRIME);
    }
    Digest { fnv: h }
}
```

`wrapping_mul` is the one Rust-specific detail and it is load-bearing. Plain
`*` on `u64` panics on overflow in a debug build and wraps in a release
build — so the same source would compute the digest in release and abort in
`cargo test`, which builds with debug assertions on. `wrapping_mul` states
the intent the algorithm actually requires (FNV-1a is defined modulo 2⁶⁴) and
makes the two profiles agree. C++ got this behavior by using `uint64_t`,
where wrapping is simply what the language does; Go got it the same way. Rust
is the only one of the three where you must ask for it, and the only one
where forgetting is caught rather than silent.

```console
$ ./demo.sh rust build
    Finished `release` profile [optimized] target(s) in 0.00s
$ ./demo.sh rust run report
toolbox report: payload_len=16 digest=0x481984990deee5ff
```

## `rustfmt`: a clean tree and one isolated dirty file

A format gate that only ever asserts "the tree is clean" is half a gate: it
passes just as happily when the formatter is broken, misconfigured, or
looking at nothing at all. The gate needs both directions — clean where it
should be clean, and *loud* where it should be loud — which means the example
has to carry a deliberately misformatted file without that file ever
affecting the build.

Rust makes the isolation easy in a way Go's `testdata/` convention and C++'s
`.clang-format` exclusions do not quite match: **a `.rs` file that no `mod`
declaration names is not part of the crate at all.** `cargo build`, `cargo
test`, `cargo clippy`, and `cargo fmt` all walk the module tree from the
crate roots, so `fixtures/misformatted.rs` — declared by nothing — is
invisible to every one of them:

```rust
// This file lives OUTSIDE the crate's module tree on purpose: nothing
// `mod`-declares it, so `cargo build`, `cargo test`, and `cargo clippy`
// never see it, and `cargo fmt --check` (which walks the module tree from
// the crate roots) leaves it alone. Only an explicit, per-file
// `rustfmt --edition 2024 --check fixtures/misformatted.rs` reads it -- and
// must exit nonzero, printing a diff.
//
// Do not "fix" the formatting below. The wrong spacing, the 4-into-2 indent,
// the missing trailing comma, and the crammed `if` are the assertion.

pub struct Sample{ pub id:u32, pub label:&'static str }

pub fn classify( n : i64 )->&'static str{
    if n<0 {"negative"} else if n==0 {"zero"}else{"positive"}
}
```

`rustfmt.toml` sits beside it and applies to both halves of the gate:

```toml
edition = "2024"
max_width = 100
newline_style = "Unix"
use_small_heuristics = "Default"
```

`edition = "2024"` appears here as well as in `Cargo.toml` because `rustfmt`
invoked directly — as the fixture half of the gate does — has no `Cargo.toml`
to read. `cargo fmt` passes the edition through from the manifest; bare
`rustfmt` does not, which is why the fixture command below spells
`--edition 2024` out explicitly. Two paths into the same formatter, two
different ways it learns which language version it is formatting.

The clean half is unglamorous and that is correct:

```console
$ cargo fmt --check ; echo $?
0
```

No output, exit 0. The loud half, run on the same host in the same session:

```console
$ rustfmt --edition 2024 --check fixtures/misformatted.rs
Diff in /home/rsedor/Dev/linux-systems-programming/examples/48-rust-toolbox/rust/fixtures/misformatted.rs:10:
 // Do not "fix" the formatting below. The wrong spacing, the 4-into-2 indent,
 // the missing trailing comma, and the crammed `if` are the assertion.

-pub struct Sample{ pub id:u32, pub label:&'static str }
+pub struct Sample {
+    pub id: u32,
+    pub label: &'static str,
+}

-pub fn classify( n : i64 )->&'static str{
-    if n<0 {"negative"} else if n==0 {"zero"}else{"positive"}
+pub fn classify(n: i64) -> &'static str {
+    if n < 0 {
+        "negative"
+    } else if n == 0 {
+        "zero"
+    } else {
+        "positive"
+    }
 }
```

Exit status 1, with a diff. `verify.lua` asserts three separate things about
that output — nonzero exit, a `Diff in …fixtures/misformatted.rs` line, and
the specific reformatted signature `pub fn classify(n: i64) -> &'static str
{` — because any one of them alone could pass for the wrong reason. A nonzero
exit could be a missing file; a `Diff in` line could name some other file;
only the reformatted signature proves `rustfmt` actually parsed this source
and produced this rewrite.

## `clippy`: clean by default, loud behind a feature

The linter gate has the same two-directional shape as the formatter's, but
the isolation problem is harder. A clippy bait *is* real code — it has to
compile — so it cannot simply be excluded the way an unreferenced file can.
Three approaches are available and two of them are wrong:

- `#[allow(clippy::needless_return)]` on the function. This defeats the
  entire gate: the lint no longer fires, so there is nothing to assert.
- Leave the bait in the default build, un-allowed. Then `cargo clippy
  --all-targets -- -D warnings` fails *always*, and the clean half of the
  gate is gone.
- Put the bait behind a Cargo feature, off by default. The default build and
  the default clippy run never compile it; `--features lintbait` does.

The third is what `src/lintbait.rs` does, and `main.rs` gates both the module
declaration and the subcommand that reaches it on the same feature:

```rust
mod digest;

#[cfg(feature = "lintbait")]
mod lintbait;
```

The bait itself is four lines of ordinary, compiling, deliberately
un-idiomatic Rust:

```rust
pub fn trip_needless_return(x: i64) -> i64 {
    let y = x * 2;
    return y;
}
```

`return y;` as the last statement of a function, where the bare expression
`y` would do. `rustc` accepts it without a murmur — it is valid Rust with
identical semantics either way. `clippy::needless_return` is a *style* lint,
and style lints are exactly the class of thing a compiler must not enforce
and a linter should. That division is the point of having clippy at all.

Clean first:

```console
$ cargo clippy --offline --all-targets -- -D warnings
    Checking toolbox v0.1.0 (/home/rsedor/Dev/linux-systems-programming/examples/48-rust-toolbox/rust)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.05s
```

`--all-targets` matters: without it, clippy checks the binary but skips the
`#[cfg(test)]` module, so lint problems in test code slip through silently.
`-D warnings` after the `--` promotes every clippy warning to an error, which
is what turns a linter into a gate — a tool that prints warnings nobody reads
is documentation, not enforcement.

Now the same command with the feature on:

```console
$ cargo clippy --offline --features lintbait --all-targets -- -D warnings
    Checking toolbox v0.1.0 (/home/rsedor/Dev/linux-systems-programming/examples/48-rust-toolbox/rust)
error: unneeded `return` statement
  --> src/lintbait.rs:13:5
   |
13 |     return y;
   |     ^^^^^^^^
   |
   = help: for further information visit https://rust-lang.github.io/rust-clippy/rust-1.97.0/index.html#needless_return
   = note: `-D clippy::needless-return` implied by `-D warnings`
   = help: to override `-D warnings` add `#[allow(clippy::needless_return)]`
help: remove `return`
   |
13 -     return y;
13 +     y
   |

error: could not compile `toolbox` (bin "toolbox") due to 1 previous error
```

Read the word `error` there: the same finding is a warning by default and an
error only because of `-D warnings`. The note says so outright — ``-D
clippy::needless-return` implied by `-D warnings``. The gate asserts the lint
*name* (`needless_return`), the diagnostic text (``unneeded `return`
statement``), and the file (`src/lintbait.rs`), never a count of findings.
Counts move whenever a clippy release adds a lint; a specific stable lint's
name and message do not, especially with the channel pinned.

`clippy.toml` sits beside `rustfmt.toml` and pins the thresholds that decide
what "clean" means:

```toml
msrv = "1.97.1"
too-many-arguments-threshold = 8
type-complexity-threshold = 250
```

`msrv` is the interesting one. It tells clippy the minimum Rust version this
crate supports, and clippy *suppresses* lints whose suggested fix would not
compile on that version — the linter refusing to recommend a language feature
you cannot use. With the channel pinned to the same value, MSRV and toolchain
agree by construction here, which is the easy case; in a real crate that
supports a range of versions, this line is what keeps clippy's advice
actionable.

## `cargo test`: the gate that catches drift first

The digest test lives in `src/digest.rs`, inside the file it tests, which is
Rust's default and a genuine difference from both siblings — C++ needed a
separate translation unit and Go a separate `_test.go` file:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fnv1a_matches_cross_language_digest() {
        let d = fnv1a(&PAYLOAD);
        assert_eq!(
            d,
            Digest {
                fnv: 0x481984990deee5ff
            },
            "digest must match the C++/Go toolbox chapters' literal"
        );
    }
}
```

`#[cfg(test)]` means the module and everything in it are compiled out of the
release binary entirely — the test costs nothing at run time, and `use
super::*` reaches the private items of its parent without any of them being
made `pub` for testing's sake.

```console
$ cargo test --offline
   Compiling toolbox v0.1.0 (/home/rsedor/Dev/linux-systems-programming/examples/48-rust-toolbox/rust)
    Finished `test` profile [unoptimized + debuginfo] target(s) in 0.10s
     Running unittests src/main.rs (target/debug/deps/toolbox-6b13b413a4fa8296)

running 1 test
test digest::tests::fnv1a_matches_cross_language_digest ... ok

test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.00s
```

The gate asserts the named test line and the `test result: ok. 1 passed; 0
failed` summary, not the process exit code — a `cargo test` run that compiled
zero tests also exits 0, and would sail past a weaker check.

It is worth seeing the gate fail on purpose, because a gate nobody has
watched fail is a gate nobody knows works. Flipping the payload's last byte
from `0x2e` (`.`) to `0x21` (`!`) in a scratch copy of the crate:

```console
$ cargo test --offline
running 1 test
test digest::tests::fnv1a_matches_cross_language_digest ... FAILED

failures:

---- digest::tests::fnv1a_matches_cross_language_digest stdout ----

thread 'digest::tests::fnv1a_matches_cross_language_digest' (480627) panicked at src/digest.rs:43:9:
assertion `left == right` failed: digest must match the C++/Go toolbox chapters' literal
  left: Digest { fnv: 5195334935605341822 }
 right: Digest { fnv: 5195329438047200767 }
note: run with `RUST_BACKTRACE=1` environment variable to display a backtrace


failures:
    digest::tests::fnv1a_matches_cross_language_digest

test result: FAILED. 0 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.00s
```

One byte, and `assert_eq!` prints both sides in decimal because that is how
`Digest`'s derived `Debug` formats a `u64`. `5195329438047200767` is
`0x481984990deee5ff` — the identical decimal Chapter 47's `delve` gate
printed when it read the Go struct's field out of a live process. Two
languages, two entirely different tools, one number.

This is also why the test gate is separate from the report gate rather than
redundant with it. Both assert the same literal, but they fail at different
times and say different things: `cargo test` fails at check time with both
values in front of you, while `toolbox report` fails only once someone runs
the binary and compares output. Drift gets caught in the cheaper place first.

## How the code works

`main.rs`'s dispatch is a `match` on the first argument, and the `lintbait`
arm is compiled in only when the feature is:

```rust
fn main() {
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("report") => cmd_report(),
        #[cfg(feature = "lintbait")]
        Some("lintbait") => {
            let doubled = lintbait::trip_needless_return(21);
            println!("toolbox lintbait: doubled={doubled}");
        }
        _ => {
            print_usage();
            std::process::exit(2);
        }
    }
}
```

`args.next().as_deref()` is the idiom worth naming: `std::env::args()` yields
owned `String`s, and matching a `String` against the literal `"report"`
requires a `&str`. `as_deref()` turns the `Option<String>` into an
`Option<&str>` without cloning, so the match arms can be plain string
literals. Attribute-on-a-match-arm is legal Rust and does exactly what it
looks like — with the feature off, that arm does not exist, and the
`lintbait` argument falls through to `print_usage()` and exit 2.

`cmd_report` is the deterministic half:

```rust
fn cmd_report() {
    let d = fnv1a(&PAYLOAD);
    println!(
        "toolbox report: payload_len={} digest=0x{:016x}",
        PAYLOAD.len(),
        d.fnv
    );
}
```

`{:016x}` — zero-padded to sixteen hex digits — is the formatting detail that
makes the output a stable string to assert on. Without the `016`, a digest
whose top nibble happened to be zero would print fifteen characters and the
literal comparison would fail for a reason that has nothing to do with the
hash being wrong.

The rest of the crate is the gate scaffolding, and each piece is isolated
from the digest by a *different* mechanism, on purpose: `src/lintbait.rs` by
a Cargo feature (not compiled unless asked), `fixtures/misformatted.rs` by
module-tree exclusion (not part of the crate at all), and `digest.rs`'s test
module by `#[cfg(test)]` (not in the release binary). Nothing about running
`toolbox report` can depend on whether any designed-for finding has or has
not been "fixed."

{% include excalidraw.html
   file="48-rust-tool-gates"
   alt="One Rust crate box splits into two gate lanes. The amber hard lane lists the rustup pin at channel 1.97.1 and edition 2024, the FNV-1a digest gate at 0x481984990deee5ff, cargo fmt --check clean on tracked sources and nonzero on the fixture, cargo clippy -D warnings with --features lintbait naming needless_return, and cargo test asserting the digest literal -- all running offline from the pinned toolchain alone because rustfmt and clippy ship as toolchain components. A dashed ghost lane beside it lists cargo-nextest, cargo-deny with its deny.toml, cargo-llvm-cov asserting a filename, and cargo-audit marked advisory-db slash network, plus cargo-watch, cargo-flamegraph, and sccache marked not gated, all shown as reference and not verified on this host."
   caption="Figure 48.2 — one crate, two gate lanes: the pinned-toolchain hard gates (rustup pin, digest, cargo fmt, cargo clippy, cargo test) that need no cargo install, beside the gated-if-present tools (cargo-nextest, cargo-deny, cargo-llvm-cov, cargo-audit) and the shown-not-gated accelerators (cargo-watch, cargo-flamegraph, sccache)" %}

## Errors, three ways

With one language and no second compiler to disagree with itself, "three
ways" here means three surfaces reading the same source, each willing to
read a different amount of context before it says anything — and, in Rust's
case, each one *promoting a different severity* into a failure.

The first surface is `rustfmt`, and it notices only shape. `misformatted.rs`
is not wrong in any semantic sense; `classify` returns the correct string for
every input. `rustfmt` still exits 1, because shape is the entire question it
answers, and it answers it by rewriting rather than by judging. There is no
severity to promote: a formatter's finding is binary.

The second surface is `clippy`, which reads idiom — patterns the compiler
accepts and a Rust programmer would not write. `return y;` is the whole
example, and the interesting part is that clippy's default verdict is
*warning*, not error. The severity is a policy decision the invoker makes, and
`-D warnings` is where that decision gets made:

```console
   = note: `-D clippy::needless-return` implied by `-D warnings`
```

This is a genuinely different model from the other two chapters. `go vet`'s
printf finding is reported at one fixed severity; a C++ compiler warning
becomes an error only via `-Werror` at build-configuration time. Rust splits
the linter from the compiler and then lets the *caller* set the threshold per
invocation — so a developer's inner loop can run clippy at warning level and
CI can run the identical command with `-D warnings` and get a hard failure,
with no second configuration to keep in sync.

The third surface is `cargo test`, and it is the only one that runs the code.
`assert_eq!` does not read shape or idiom; it reads *behavior*, and the
failure it produces is a panic with both values printed. Nothing static could
have caught a wrong payload byte — the code is well-formatted, idiomatic, and
type-correct with `0x21` in it. Only executing `fnv1a` and comparing the
result finds it.

Rust also draws a line the other two chapters do not have to: `Result` versus
panic. `toolbox` has no `Result` in it, which is itself the design decision —
the report path has nothing that can fail (a fixed array, an infallible hash,
a `println!`), so introducing a `Result` would be inventing an error case to
have one. The panics that do appear are all in test code, where a panic *is*
the reporting mechanism. Chapter 5 — the book's own "Errors, three ways" —
and Chapter 43 cover `Result` propagation and `?` at length for code that has
real fallible I/O; a tooling chapter's
contribution is the opposite observation — that a deterministic gate should
have no error paths to test, because every branch it does not have is a
branch that cannot drift.

## Concurrency lens

The concurrency in this chapter belongs to the *tools*, not to `toolbox`,
which is strictly single-threaded. Three layers of it are worth naming, and
Rust's tooling makes a choice at each one that differs from Go's.

The first is the build graph. `cargo` parallelizes across crates by default —
`-j` defaults to the CPU count, 16 on this host — which is why a
zero-dependency crate builds in well under a second and a crate with a deep
dependency tree does not. That default is also why a lockfile matters for
reproducibility but not for speed: the graph's *shape* is fixed by
`Cargo.lock`, and cargo walks it as wide as the machine allows. Inside a
single crate, `rustc` parallelizes differently again, by splitting the crate
into codegen units (16 in the release profile by default, 1 under
`lto = "fat"`), which trades compile time against optimization quality — the
same trade `mold` and `ccache` addressed from the outside in Chapter 46, made
from the inside here.

The second is the test harness. `cargo test` builds one test binary and
libtest runs the `#[test]` functions **in threads, inside that one process**,
as wide as the CPU count unless told otherwise:

```console
$ cargo test --offline -- --test-threads=1
running 1 test
test digest::tests::fnv1a_matches_cross_language_digest ... ok

test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.00s
```

`--test-threads=1` exists because that thread-per-test model has real
consequences for systems code specifically. Tests sharing a process share
`errno`-adjacent global state, the signal-disposition table, the file
descriptor table, the current working directory, and the process's own
`rlimit`s — so a test that installs a signal handler, `chdir`s, or exhausts
FDs can change the result of a test running concurrently beside it. Every one
of those is something the earlier parts of this book taught you to reach for.
`cargo test -- --test-threads=1` is the blunt fix, at the cost of all
parallelism.

`cargo-nextest` is the third layer, and it is the reason it appears in this
chapter's tool list at all: it runs **each test in its own process**. That
turns process-global state from a shared hazard into per-test isolation for
free, and it means a test that segfaults or `abort()`s takes down only itself
rather than the whole run — which for a book full of `mmap`, signal, and
`rlimit` examples is not a marginal benefit. The cost is process-spawn
overhead per test, which matters for thousands of microsecond-scale unit
tests and does not matter here. Go made the opposite structural choice —
one process per *package*, tests within a package sharing it — and Chapter
47's `b.RunParallel` benchmark lived entirely inside that shared process. Same
problem, two different places to put the isolation boundary.

## The gated-if-present tier: `cargo-nextest`, `cargo-deny`, `cargo-llvm-cov`, `cargo-audit`

None of these four ships with the toolchain; each is a `cargo install`. On
this reference host, `cargo-nextest`, `cargo-deny`, and `cargo-llvm-cov` are
absent and `cargo-audit` is present but has no advisory database, and this
iteration's decision was to install nothing and fetch nothing. So all four
are **shown as reference, not verified** — `verify.lua` prints an
informational `SKIP:` for each rather than failing. What *is* committed is
the configuration, because that is the durable artifact: a reader who runs
`cargo install cargo-deny` gets a working gate with no further authoring.

`cargo nextest run` would name the test and print its own summary
(`1 test run: 1 passed`); the gate asserts those tokens, not an exit code.

`deny.toml` is the substantial one. Its offline-answerable checks — `bans`,
`licenses`, `sources` — are answerable from `Cargo.lock` alone, which is why
those three are the subsets the gate would run:

```toml
[licenses]
# toolbox is zero-dependency, so this list constrains exactly one crate:
# toolbox itself. That is the point -- the policy is in place before the
# first dependency arrives, not bolted on after.
allow = [
    "MIT",
    "Apache-2.0",
    "Apache-2.0 WITH LLVM-exception",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "ISC",
    "Unicode-3.0",
]
confidence-threshold = 0.93

[bans]
multiple-versions = "deny"
wildcards = "deny"
highlight = "all-duplicates"
```

`multiple-versions = "deny"` is the line that earns its place in a systems
crate: Cargo will happily link two semver-incompatible versions of the same
crate into one binary, which for a crate wrapping a C library or holding a
global registry means two independent copies of state that both think they
are the only one. `wildcards = "deny"` forbids `version = "*"` dependencies,
which is how a lockfile-clean build turns into a different build next week.
`cargo deny check advisories` is deliberately *not* in the gated subset:
it needs the RustSec database cloned into `~/.cargo/advisory-db`, a network
fetch this example refuses to make.

`cargo llvm-cov --summary-only` is the coverage tool, and the gate that would
run it asserts a **filename** — that `digest.rs` appears in the summary —
never a percentage. A percentage moves every time a line is added to the
crate, so gating on one produces failures that have nothing to do with the
toolchain. It also needs `rustup component add llvm-tools`, per the pin
discussion above.

`cargo audit` is the network-shaped one, and it gets a *double* guard in
`verify.lua` — on the binary and on the database:

```lua
if not tool_present("cargo-audit") then
  print("SKIP: cargo-audit not found on PATH -- gate H not asserted")
elseif checks.run("test -d \"$HOME/.cargo/advisory-db\"").exit ~= 0 then
```

Without the second check this host would *pass* the tool test and then have
`cargo audit` clone the RustSec database mid-verification — turning a
network outage into a test failure, and an offline example into an online
one. What a real finding looks like, shown here as clearly-labeled
**illustrative** output rather than a captured run, since this crate has no
vulnerable dependency by design and none will be committed:

```
Crate:     some-crate
Version:   0.4.1
Title:     Use-after-free in some-crate
Date:      2026-03-11
ID:        RUSTSEC-2026-0031
URL:       https://rustsec.org/advisories/RUSTSEC-2026-0031
Severity:  7.5 (high)
Solution:  Upgrade to >=0.4.3
```

The durable token there is the `RUSTSEC-YYYY-NNNN` identifier — stable,
citable, and the thing a policy actually references. Chapter 47 made the same
call for `govulncheck` and for the same two reasons: network, and no
committed vulnerability.

## Shown, not gated: `cargo-watch`, `cargo-flamegraph`, `sccache`

Three tools round out a working Rust setup and none of them is asserted,
each for a different reason worth stating rather than hiding.

`cargo watch -x 'clippy --all-targets -- -D warnings'` re-runs the lint gate
on every save. It is the inner-loop counterpart to the CI gate — the same
command, run continuously — and it is not gated because it never terminates.
A check that never returns is not a check.

`cargo flamegraph --bin toolbox -- report` produces an SVG of where CPU time
went. It needs `perf`, which needs `kernel.perf_event_paranoid` lowered or
root, and it produces output that differs run to run by construction. Chapter
31 already ran `cargo-flamegraph` for real against a Rust binary and taught
how to read the result, and Chapter 26 hit the `perf_event_paranoid=2`
privilege wall head-on; the reason it is not gated *here* is the same reason
Chapter 46 did not gate `ccache` — sampling output is not a deterministic
assertion.

`sccache` caches compilation artifacts across builds
(`RUSTC_WRAPPER=sccache`). It is a pure accelerator: a correct build and a
correct cached build produce identical output, so there is nothing about
correctness to assert. Its own metric — cache hit rate — depends on what you
compiled five minutes ago, which is exactly the kind of number this book
refuses to gate on. `ccache` and `mold` got the same treatment in Chapter 46.

**miri** deserves a pointer rather than a section: it interprets Rust at the
MIR level and catches undefined behavior in `unsafe` code that no amount of
formatting, linting, or testing will. It is not re-taught here because
Chapters 29 and 43 already own it, and `toolbox` has no `unsafe` block for it
to find anything in — which is the correct reason to omit a tool, as opposed
to omitting it because it is inconvenient.

## Build, run, observe

Everything below runs on the host, offline, with no `cargo install` and no
VM:

```bash
[host]$ cd examples/48-rust-toolbox
[host]$ ./demo.sh rust build              # zero-dep, --offline, no network
[host]$ ./demo.sh rust run report         # digest=0x481984990deee5ff

[host]$ cd rust
[host]$ rustup show                       # "active because: overridden by ...rust-toolchain.toml"
[host]$ cargo tree --offline              # exactly one node
[host]$ cargo fmt --check                 # silent, exit 0
[host]$ rustfmt --edition 2024 --check fixtures/misformatted.rs   # diff, exit 1
[host]$ cargo clippy --offline --all-targets -- -D warnings       # clean
[host]$ cargo clippy --offline --features lintbait --all-targets -- -D warnings
[host]$ cargo test --offline              # the named digest test
```

And the gate itself:

```console
$ cd examples/48-rust-toolbox
$ LSP_LANG=rust REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
ok: rust: cargo build --offline succeeds (zero-dep, no network)
ok: rust-toolchain.toml: pins channel 1.97.1
ok: rust-toolchain.toml: pins the rustfmt + clippy components
ok: Cargo.toml: sets edition 2024
ok: rust: rustc --version runs through the pin
ok: rust: the pin really resolves -- rustc reports 1.97.1
ok: rust: toolbox report runs
ok: rust: toolbox report equals the expected literal (matches ch46's C++ and ch47's Go digest)
ok: rust: cargo fmt --check exits 0 on the crate's module tree
ok: rust: cargo fmt --check prints no diff for tracked sources
ok: rust: rustfmt --check exits nonzero on the misformatted fixture
ok: rust: rustfmt --check prints a diff naming the fixture
ok: rust: the diff shows the reformatted signature rustfmt would write
ok: rust: cargo clippy --all-targets -- -D warnings is clean on the default build
ok: rust: the clean clippy run really checked the crate (cargo reports Finished)
ok: rust: clippy --features lintbait fails under -D warnings
ok: rust: clippy names the stable needless_return lint
ok: rust: clippy emits the needless_return diagnostic text
ok: rust: the finding is located in src/lintbait.rs
ok: rust: cargo test succeeds
ok: rust: the named digest test runs and passes
ok: rust: cargo test reports the summary line, not just exit 0
SKIP: cargo-nextest not found on PATH -- gate E not asserted
SKIP: cargo-deny not found on PATH -- gate F not asserted
SKIP: cargo-llvm-cov not found on PATH -- gate G not asserted
SKIP: ~/.cargo/advisory-db absent (offline; cargo-audit would fetch it) -- gate H not asserted
info: cargo-watch, cargo-flamegraph and sccache are shown in the chapter but not gated (interactive / needs perf privileges / cache-hit-rate is not a correctness property)
info: miri is not re-taught here -- ch29 and ch43 own it
PASS 20 / FAIL 0
```

Four `SKIP:` lines, no failures. That is the shape a gated-if-present tier is
supposed to have: the absent tools are named, in the output, where nobody can
mistake "not run" for "passed."

## Cross-check: the same digest, three languages apart

Three chapters, three languages, three completely unrelated toolchains, one
16-byte payload:

```console
$ examples/46-cpp-toolbox/cpp/build/release/toolbox report
toolbox report: label=[toolbox] report payload_len=16 digest=0x481984990deee5ff
$ examples/47-go-toolbox/go/bin/toolbox report
toolbox report: payload_len=16 digest=0x481984990deee5ff
$ examples/48-rust-toolbox/rust/target/release/toolbox report
toolbox report: payload_len=16 digest=0x481984990deee5ff
```

(The C++ line carries an extra `label=[toolbox]` field from Chapter 46's
argument-echo demo; the `payload_len` and `digest` fields are identical
across all three.)

FNV-1a is language-independent — a shift, an xor, and a multiply modulo
2⁶⁴ — so nothing about that agreement is surprising *in principle*. What
makes it worth printing is that agreeing in practice required each chapter to
have gotten a different language-specific detail right: C++ needed
`uint64_t`'s defined wrapping and a `-fsanitize=undefined` build that did not
trip on it, Go needed the untyped-constant rules not to promote the FNV prime
into something wider, and Rust needed `wrapping_mul` so a debug build would
not panic where release wraps. Three different ways to get modular arithmetic
wrong, three different tools that would have caught it, one number that says
none of them did.

## Where this sits next to the book's other tooling chapters

Part 13 covers tooling in four chapters and this is the last of them, so the
arc is worth naming explicitly. Chapter 45 handled the *analysis suites* —
tools that observe a running system from outside it. Chapters 46, 47, and 48
each took one language and pointed its whole toolchain at one small binary,
and the interesting result is how differently the three languages distribute
the same responsibilities:

- **C++ (ch46)** puts almost nothing in the language distribution. CMake,
  Conan, clang-tidy, clang-format, the sanitizers, gdb, ccache, and mold are
  eight separate projects with eight separate versions, and `CMakePresets.json`
  exists to make "which of these, configured how" an explicit, committed
  choice. The chapter's parity gate — GCC and clang producing byte-identical
  output — is a check the other two languages have no reason to run.
- **Go (ch47)** puts the compiler, the formatter, the vetter, the test
  runner, the profiler, and the generator inside one `go` binary, and pins
  that binary's version in `go.mod`. What it leaves outside — gofumpt,
  golangci-lint, staticcheck, delve, benchstat — is precisely what Chapter 47
  had to gate as if-present.
- **Rust (ch48)** pins a *toolchain* rather than a binary, and that toolchain
  includes the formatter and the linter as components. The consequence is
  this chapter's structure: the hard, offline core covers formatting and
  linting, which Chapter 47's could not, while the if-present tier is a
  different set of tools entirely (nextest, deny, llvm-cov, audit) — every
  one of them a `cargo install` rather than a component.

Chapter 31 sits underneath all three: it ran each language's *profiling*
toolbelt — `perf` for C++, the `pprof` ecosystem for Go, `cargo-flamegraph`
and clippy for Rust — against one shared workload, which is why these three
chapters could concentrate on the build-and-check side instead of re-teaching
how to read a flamegraph.

The dependency and supply-chain story splits the same way. C++ needed Conan
lockfiles to have a dependency graph worth checking at all; Go's module
system makes the graph free and `govulncheck` the tool that reads it; Rust
gets the graph free too and splits the reading in two — `cargo-deny` for
policy answerable from the lockfile, `cargo-audit` for the RustSec advisory
database over the network. Chapters 47 and 48 made the identical call about
that network half: show it, name the durable identifier, never gate it.

## What you learned

- **`rust-toolchain.toml` pins more than a compiler.** `channel` fixes the
  release; `components` fixes which of that release's artifacts get
  installed. Because `rustfmt` and `clippy` are components, pinning the
  channel pins the formatter and the linter at the same commit as `rustc` —
  which is why this chapter's format and lint gates are hard and offline
  where Chapter 47's equivalents could not be.
- **`rustup show` states the override.** `active because: overridden by
  '…/rust-toolchain.toml'` is the mechanism reported by the tool, and it
  disappears one directory outside the crate.
- **Do not pin components you do not need.** `rustup` fetches missing
  components over the network on the first `cargo` invocation, so an
  aspirational `llvm-tools` in `components` silently breaks
  `cargo build --offline`. Document `rustup component add` instead.
- **A format gate needs both directions.** Clean on the tree *and* loud on a
  known-dirty file. In Rust the dirty file isolates for free: a `.rs` file no
  `mod` declares is not part of the crate, so nothing but an explicit
  per-file `rustfmt` invocation ever reads it.
- **A lint gate needs both directions too, and a Cargo feature is the clean
  way to get them.** `#[allow]` on the bait destroys the gate; leaving the
  bait in the default build destroys the clean half. `#[cfg(feature =
  "lintbait")]` keeps both.
- **`-D warnings` is where severity is decided, per invocation.** Clippy's
  findings are warnings by default; the caller promotes them. The inner loop
  and CI can run the identical command at different thresholds with no second
  config to keep in sync.
- **Assert the lint name and the diagnostic text, never a finding count.**
  Counts move with every clippy release; a stable lint's name does not,
  especially with the channel pinned.
- **`wrapping_mul`, not `*`.** Rust panics on overflow in debug and wraps in
  release, so an algorithm defined modulo 2⁶⁴ has to say so — otherwise
  `cargo test` and the release binary disagree.
- **Gate on the summary line, not the exit code.** `cargo test` with zero
  tests compiled also exits 0. `test result: ok. 1 passed; 0 failed` does not.
- **`cargo test` runs tests in threads inside one process; `cargo-nextest`
  runs each in its own.** For systems code that touches signal dispositions,
  the FD table, the CWD, or `rlimit`s, that difference is the whole reason to
  reach for nextest — or for `--test-threads=1`.
- **Guard a network-shaped gate on the data, not just the binary.**
  `cargo-audit` present with no `~/.cargo/advisory-db` would silently clone
  the RustSec database mid-verification. Two guards, one skip.
- **Say which tools did not run, in the output.** Four `SKIP:` lines beside
  `PASS 20 / FAIL 0` is the shape that keeps "absent" from reading as
  "passed."

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.5-201.fc44, rustc/cargo 1.97.1 via
rustup, clippy 0.1.97, rustfmt 1.9.0-stable, offline, no network access
used): <code>LSP_LANG=rust REPO_ROOT=$(cd ../.. && pwd) lua verify.lua</code>
reported <code>PASS 20 / FAIL 0</code>, and
<code>python3 scripts/test-all-examples.py --only 48-rust-toolbox</code>
reported <code>1 passed, 0 failed, 0 skipped</code>. Confirmed live and
folded into that PASS count, the pinned-toolchain hard gates A-D:
<code>rustup show</code> reported <code>active because: overridden by
'…/examples/48-rust-toolbox/rust/rust-toolchain.toml'</code> while
<code>rustup show active-toolchain</code> outside the crate reported
<code>stable-x86_64-unknown-linux-gnu (default)</code>;
<code>rust-toolchain.toml</code> pins <code>channel = "1.97.1"</code> with
<code>components = ["rustfmt", "clippy"]</code> and <code>Cargo.toml</code>
sets <code>edition = "2024"</code>; <code>rustc --version</code> resolved
through the pin to <code>rustc 1.97.1 (8bab26f4f 2026-07-14)</code>;
<code>cargo tree --offline</code> printed exactly one node;
<code>./demo.sh rust run report</code> printed <code>toolbox report:
payload_len=16 digest=0x481984990deee5ff</code>, byte-identical to Chapter
46's C++ and Chapter 47's Go digest; <code>cargo fmt --check</code> exited 0
with no output while <code>rustfmt --edition 2024 --check
fixtures/misformatted.rs</code> exited 1 with the diff quoted above;
<code>cargo clippy --offline --all-targets -- -D warnings</code> exited 0
clean and <code>--features lintbait</code> failed with <code>unneeded
`return` statement</code> at <code>src/lintbait.rs:13:5</code> naming
<code>needless_return</code>; and <code>cargo test --offline</code> reported
<code>test result: ok. 1 passed; 0 failed</code>. The drift transcript in the
<code>cargo test</code> section is also a real run, taken on a scratch copy
of the crate with the payload's last byte changed from <code>0x2e</code> to
<code>0x21</code>. Not exercised: <span class="status
status--unverified">unverified</span> — <code>cargo-nextest</code>,
<code>cargo-deny</code>, <code>cargo-llvm-cov</code>, and <code>sccache</code>
are absent from this reference host (confirmed by <code>command -v</code> for
each) and were not installed, and <code>cargo-audit</code> is present but
<code>~/.cargo/advisory-db</code> is absent, so gates E, F, G, and H each
printed an informational <code>SKIP:</code> rather than running; their
sections above show the committed configs (<code>deny.toml</code>,
<code>clippy.toml</code>) and the exact tokens (<code>1 test run: 1
passed</code>, <code>bans ok</code>, a <code>digest.rs</code> coverage row, a
<code>RUSTSEC-YYYY-NNNN</code> identifier) a reader with those tools installed
would see, rather than a captured run. The <code>cargo audit</code> advisory
block is explicitly labeled illustrative: this crate is zero-dependency by
design and no vulnerable dependency will be committed to it.
<code>cargo-watch</code> and <code>cargo-flamegraph</code> are installed on
this host but deliberately not gated (never terminates / needs
<code>perf</code> privileges and produces non-deterministic output), and
<code>sccache</code>'s cache-hit rate is not a correctness property; miri is
cross-referenced to Chapters 29 and 43 and has no <code>unsafe</code> block
here to examine. <code>examples/manifest.yaml</code> marks this example
<code>mode: local</code> — no VM or LGTM path applies.</p>
