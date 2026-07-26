---
title: "Rust macros for systems code: proc-macros, syn/quote, and what miri proves about unsafe"
order: 43
part: "Deep Dives"
description: "sysmacros is chapter 43's proc-macro crate, powering two Rust-only techniques in the app binary -- #[derive(SysError)], a miniature thiserror built with syn 2.0's AST and quote's token emission, and #[instrument], a compile-time tracing attribute that times a function with Instant -- plus a sound unsafe unaligned read that cargo +nightly miri validates clean and, under the deliberately-named --oob-read path, catches as undefined behaviour. The book's first single-language deep dive, verified on the Fedora 44 host with verify.lua PASS 8/FAIL 0, the exact six-line stable stdout, and miri's own out-of-bounds diagnostic reproduced byte for byte."
duration: "35 minutes"
---

Every chapter so far has mirrored the same behavior across C++23, Go, and
Rust, because the point of those chapters was a Linux mechanism — a
syscall, a signal, a namespace — that all three languages sit on top of
equally. This one breaks that pattern on purpose. Proc-macros are a Rust
compiler feature with no C++ or Go equivalent worth building a codetab
around, and `miri`, the interpreter that catches undefined behaviour a
normal build would run right past, only exists because Rust's `unsafe`
carries obligations the type system stops checking. Chapter 12 gave
`pmon` a `PmonError`-shaped hierarchy by hand, written out field by field;
this chapter asks how much of that boilerplate a macro can write for
you, and how you prove the `unsafe` block sitting a few lines below it is
still doing exactly what its comment claims.

`sysmacros` is a small cargo workspace: a `proc-macro = true` crate holding
two macros, and an `app` binary that uses them. `#[derive(SysError)]`
turns an errno-style enum into a `Display` + `std::error::Error` impl from
one `#[error("...")]` attribute per variant — a miniature `thiserror`,
built from scratch with `syn 2.0` and `quote`. `#[instrument]` wraps a
function to print its entry, its exit, and its wall-clock duration,
without a tracing runtime. And `read_record` is one `unsafe` unaligned
read of a `repr(C)` struct out of a byte buffer — sound on the path that
checks its bounds, and undefined behaviour on the `--oob-read` path that
skips the check, which is exactly the distinction `miri` exists to draw.

{% include excalidraw.html
   file="43-proc-macro-pipeline"
   alt="Two horizontal bands. Top band: the app crate, main.rs's annotated source (#[derive(SysError)] enum PmonError, #[instrument] fn hash_chunk) feeding rustc, which packages a TokenStream and hands it off; on the right, rustc splices tokens back into the AST, typechecks, and compiles the result into the app binary (generated Display + Error impls, the instrumented hash_chunk). Bottom band: sysmacros, a separate proc-macro crate compiled FIRST, containing two nodes -- syn::parse_macro_input! turning the TokenStream into a real DeriveInput/ItemFn AST, and quote! emitting a new TokenStream2 (the Display impl or the wrapped function). Arrows cross the band boundary labeled TokenStream (macro input) going down and TokenStream (macro output) coming back up. A note at the bottom reads: sysmacros never runs at app runtime -- it is pure tokens-in/tokens-out, executed only while rustc is compiling app."
   caption="Figure 43.1 — the compile-time-only pipeline: annotated source becomes a TokenStream, the separately-compiled sysmacros crate parses it with syn and re-emits new tokens with quote, and rustc splices the result into app" %}

> **Tools used** — `cargo` (host, build/run the pinned stable toolchain —
> `rustc 1.97.1` per `rust-toolchain.toml`, in `scripts/check-host.sh`),
> `cargo +nightly miri` / the `miri` component (host — a nightly toolchain
> plus `miri` installed via `rustup`, exercised throughout this chapter but
> **not** part of the stable CI gate, so it is intentionally not in
> `scripts/check-host.sh`), `lua` (host, drives `verify.lua` — in
> `scripts/check-host.sh`).

## Two macros, one crate: `sysmacros`

`sysmacros/Cargo.toml` sets `proc-macro = true` in its `[lib]` section,
which is what makes it a fundamentally different kind of dependency:
rustc compiles it as a *compiler plugin* — a program that runs during
`app`'s own compilation, not inside `app` itself — before it ever touches
`main.rs`. That is why this is a two-crate cargo workspace rather than one
package: a proc-macro can only live in its own crate, compiled first, and
`app`'s `Cargo.toml` depends on it the ordinary way (`sysmacros = { path =
"sysmacros" }`). Nothing in `sysmacros/src/lib.rs` ever runs when you
execute the finished `app` binary; every line of it runs once, at build
time, and produces tokens that rustc splices into the program in its
place.

The macro `PmonError` actually uses is the plainer of the two ideas —
each variant just needs a message:

```rust
#[derive(SysError, Debug)]
enum PmonError {
    #[error("open {path} failed: {errno}")]
    Open { path: String, errno: i32 },
    #[error("child {pid} exited with status {code}")]
    ChildExit { pid: u32, code: i32 },
    #[error("policy rejected: {0}")]
    PolicyRejected(String),
}
```

`#[derive(SysError)]` reads that `#[error("...")]` string per variant and
writes the `Display` arm that formats it, plus a blanket `impl
std::error::Error`. The named-field variants (`Open`, `ChildExit`) format
their own fields by name; the tuple variant (`PolicyRejected`) formats its
one field positionally as `{0}`. `#[instrument]` sits on `hash_chunk`
below it the same way `#[error(...)]` sits on an enum variant: an
annotation the macro reads, not code that runs before the function does.
Both macros end at the same place — a `syn::Result<TokenStream2>` that is
either the new code or a `syn::Error` describing exactly what was wrong
with the input — which "Errors, three ways" returns to below.

## A sound unsafe read, and what miri proves

`read_record` is the one piece of this chapter that is not macro-generated,
and it earns its own section because "sound" is a specific claim, not a
synonym for "compiles":

```rust
fn read_record(buf: &[u8], oob: bool) -> Record {
    let off = if oob { 4 } else { 0 };
    // oob: deliberately reads past the 8-byte buffer.
    // SOUND path checks the bounds; the --oob-read path skips the check (the
    // bug miri catches).
    if !oob {
        assert!(buf.len() >= off + core::mem::size_of::<Record>());
    }
    // SAFETY (sound path): buf has >= 8 bytes, Record is repr(C) plain-old-data,
    // read_unaligned tolerates any alignment. Under --oob-read this reads 4 bytes
    // past the end -> undefined behaviour, which is exactly what miri reports.
    unsafe { core::ptr::read_unaligned(buf.as_ptr().add(off) as *const Record) }
}
```

An `unsafe` block is a promise to the compiler: "I checked something the
type system can't, and this operation is valid anyway." `read_unaligned`
needs three things to be true — the pointer is readable for
`size_of::<Record>()` bytes, those bytes form a valid `Record` (satisfied
trivially here, since `Record` is `repr(C)` and made only of integers),
and the read doesn't race another write. The `assert!` right above the
`unsafe` block is what makes the sound path's promise true: it checks the
buffer really does have 8 bytes before offset 0 is read from. `--oob-read`
sets `off = 4` and, by design, skips that `assert!` — the read now reaches
for offset `4..12` against an 8-byte `buf`, and nothing in a normal
`cargo build`/`cargo run` will tell you that happened. That silence is the
whole reason `miri` exists: it is not a linter looking at your source, it
is an interpreter that tracks every allocation's real bounds and aborts
the instant a read or write steps outside them.

{% include excalidraw.html
   file="43-miri-bounds-check"
   alt="An 8-byte buffer BUF: [u8; 8] drawn as eight numbered byte boxes 0 through 7, labeled above as magic: u32 (bytes 0..4) and slots: u32 (bytes 4..8), followed by four dashed ghost boxes numbered 8 through 11 marked past the end (never allocated). Below the real bytes, a blue SOUND window box spans bytes 0 through 7, labeled: read_record(buf, oob=false), read_unaligned::<Record>(ptr.add(0)), window [0..8) fully inside the 8-byte alloc, cargo +nightly miri run: exit 0, no UB. Below and offset right, an amber window box spans bytes 4 through 11 (crossing from the real buffer into the ghost bytes), labeled: --oob-read: read_record(buf, oob=true), read_unaligned::<Record>(ptr.add(4)), window [4..12) -- 4 bytes past the 8-byte alloc, miri: only 4 bytes from the end of the allocation (main.rs:57). Dashed amber boundary markers point from the OOB window up into the byte row at the offset-4 and offset-12 boundaries."
   caption="Figure 43.2 — the same 8-byte Record read from two offsets: the SOUND window [0..8) miri accepts, and the --oob-read window [4..12) that runs 4 bytes past the allocation, exactly what miri's diagnostic names" %}

## How the code works

Both macros are one function each, taking real syntax in and giving real
syntax back, and both follow the same three-step shape: `parse_macro_input!`
turns the raw `TokenStream` into a typed `syn` AST, an `expand_*` function
walks that AST and builds a `quote!` block, and the top-level macro
function unwraps the `Result`, turning `Ok` into the new tokens and `Err`
into a `compile_error!`. `expand_sys_error` is the derive side of that
shape:

```rust
fn expand_sys_error(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let name = &input.ident;

    let data_enum = match &input.data {
        Data::Enum(data_enum) => data_enum,
        _ => {
            return Err(syn::Error::new_spanned(
                input,
                "SysError can only be derived for enums",
            ))
        }
    };

    let mut arms = Vec::with_capacity(data_enum.variants.len());
    for variant in &data_enum.variants {
        let vident = &variant.ident;

        let error_attr = variant
            .attrs
            .iter()
            .find(|attr| attr.path().is_ident("error"))
            .ok_or_else(|| {
                syn::Error::new_spanned(
                    variant,
                    format!("variant `{vident}` is missing #[error(\"...\")]"),
                )
            })?;
        let message: LitStr = error_attr.parse_args()?;

        let arm = match &variant.fields {
            Fields::Named(fields) => {
                let idents: Vec<_> = fields
                    .named
                    .iter()
                    .map(|f| f.ident.clone().expect("named field always has an ident"))
                    .collect();
                // Edition-2024 inline format-arg capture: `path` and `errno`
                // in the literal resolve directly to the bindings destructured
                // in this match arm's pattern.
                quote! {
                    Self::#vident { #(#idents),* } => write!(f, #message),
                }
            }
            Fields::Unnamed(fields) => {
                let idents: Vec<_> = (0..fields.unnamed.len())
                    .map(|i| format_ident!("f{}", i))
                    .collect();
                quote! {
                    Self::#vident(#(#idents),*) => write!(f, #message, #(#idents),*),
                }
            }
            Fields::Unit => {
                quote! {
                    Self::#vident => write!(f, #message),
                }
            }
        };
        arms.push(arm);
    }

    Ok(quote! {
        impl ::std::fmt::Display for #name {
            fn fmt(&self, f: &mut ::std::fmt::Formatter<'_>) -> ::std::fmt::Result {
                match self {
                    #(#arms)*
                }
            }
        }

        impl ::std::error::Error for #name {}
    })
}
```

`attr.path().is_ident("error")` is syn 2.x's API for "is this attribute
named `error`" — `path()` is a method, not a field, one of the surface
changes from syn 1.x. `#[proc_macro_derive(SysError, attributes(error))]`
(not shown above) declares `error` as an *inert helper attribute*: it
tells rustc that `#[error("...")]` on a variant is this derive's business,
not an unknown-attribute error. `error_attr.parse_args::<LitStr>()`
(spelled `parse_args()?` above, with the type driven by the `LitStr`
annotation on `message`) parses whatever is inside the parentheses as a
single string literal — exactly the `"open {path} failed: {errno}"` from
`PmonError::Open`. The three `Fields` arms are why `PolicyRejected`'s tuple
variant formats differently from the two named-field variants: for
`Fields::Named`, the destructured pattern `Self::#vident { #(#idents),* }`
binds `path` and `errno` as local names, so `write!(f, #message)` alone is
enough — edition 2024's inline format-arg capture resolves `{path}` and
`{errno}` in the literal directly against those bindings, with no
positional arguments at all. For `Fields::Unnamed`, there is no name to
capture, so the arm binds synthetic identifiers (`f0`, `f1`, ...) and
passes them to `write!` explicitly as `#(#idents),*` after the message.
The whole function returns one `quote!` block containing both the
`Display` impl and the `Error` impl, and that block — not `PmonError`'s
own source — is what rustc actually type-checks and compiles into `app`.

`expand_instrument` takes the same shape and applies it to a function
instead of an enum:

```rust
fn expand_instrument(func: ItemFn) -> syn::Result<TokenStream2> {
    let ItemFn {
        attrs,
        vis,
        sig,
        block,
    } = func;

    let fn_name = sig.ident.to_string();

    let mut arg_idents = Vec::with_capacity(sig.inputs.len());
    for input in &sig.inputs {
        match input {
            syn::FnArg::Typed(pat_type) => match &*pat_type.pat {
                syn::Pat::Ident(pat_ident) => arg_idents.push(pat_ident.ident.clone()),
                other => {
                    return Err(syn::Error::new_spanned(
                        other,
                        "#[instrument] requires simple identifier parameters",
                    ))
                }
            },
            syn::FnArg::Receiver(receiver) => {
                return Err(syn::Error::new_spanned(
                    receiver,
                    "#[instrument] does not support methods with `self`",
                ))
            }
        }
    }

    let entry_fmt = {
        let args_fmt = arg_idents
            .iter()
            .map(|id| format!("{id}={{}}"))
            .collect::<Vec<_>>()
            .join(", ");
        format!("-> {fn_name}({args_fmt})")
    };
    let exit_fmt = format!("<- {fn_name} = {{}} ({{}}us)");

    Ok(quote! {
        #(#attrs)*
        #vis #sig {
            println!(#entry_fmt, #(#arg_idents),*);
            let __instrument_start = ::std::time::Instant::now();
            let __instrument_result = (move || #block)();
            let __instrument_elapsed = __instrument_start.elapsed();
            println!(#exit_fmt, __instrument_result, __instrument_elapsed.as_micros());
            __instrument_result
        }
    })
}
```

`parse_macro_input!(item as ItemFn)` (in the caller, not shown) gives
`expand_instrument` a full parsed function — signature, visibility,
attributes, and body — as one `syn::ItemFn`. The loop over `sig.inputs`
rejects anything that isn't a plain identifier parameter (`self` receivers,
destructuring patterns) with a `syn::Error` rather than silently mangling
it. The generated function keeps the original `#(#attrs)*`, `#vis`, and
`#sig` untouched — same name, same signature, same visibility — and
replaces only `#block`, the body, with one that prints an entry line,
runs the original body inside a closure so `#block`'s own `return`s and
`?` still work as written, times it with `Instant`, prints the exit line
with the return value and the elapsed microseconds, and returns the
result. `hash_chunk(len: usize) -> u64` goes in as ordinary Rust and comes
out as a function with the identical signature that also happens to log
itself — the caller never sees a difference except the printed lines.

## Errors, three ways

Every earlier chapter's "errors, three ways" contrasted three languages;
this one has only Rust, so the three ways are the three moments an error
in this program can actually surface. **Compile time**: a malformed macro
input — `#[derive(SysError)]` on a struct, or a variant missing
`#[error("...")]` — becomes a `syn::Error`, and `derive_sys_error` turns
that into real tokens with `err.to_compile_error().into()`, so a bad
macro invocation is a compiler error at the call site, never a panic
inside `sysmacros` itself. **Run time**: once compiled, `PmonError`'s
three variants are ordinary values — `println!("error: {open_err}")`
drives the generated `Display` impl the same way any hand-written error
type would, producing `error: open /etc/shadow failed: 13` and the two
lines after it. **A checked run**: `read_record`'s `--oob-read` path
produces no error at all under a normal `cargo run` — the read succeeds,
reads whatever bytes happen to sit past the allocation, and the program
keeps going, which is precisely what makes undefined behaviour dangerous
rather than merely broken. `cargo +nightly miri run` is what turns that
silence into a real, located diagnostic: not a `Result`, not a panic, but
an interpreter-level Undefined Behavior report naming the exact
instruction and the exact number of bytes it stepped past the end of the
allocation.

## Concurrency lens

There is a genuine concurrency angle here, and it is not the hot loop —
there isn't one. `expand_sys_error` and `expand_instrument` run inside
rustc's own build process, once per macro invocation, as ordinary
synchronous function calls; nothing in `sysmacros` spawns a thread or
shares mutable state across invocations, because a proc-macro never needs
to — it is handed one `TokenStream`, returns one `TokenStream`, and is
done. `#[instrument]`'s `Instant::now()`/`.elapsed()` pair is just as
thread-agnostic at the *other* end of the pipeline: the generated wrapper
times whatever thread calls the instrumented function, on that thread's
own clock, with no global state to synchronize. Call `hash_chunk` from ten
threads and you get ten independent entry/exit pairs, interleaved on
stdout by nothing more than `println!`'s own internal lock. Contrast a
real tracing framework like `tracing`: `#[tracing::instrument]` has to
enter a span on a thread- or task-local subscriber stack, so concurrent
spans compose into a tree a `Registry` maintains. This chapter's
`#[instrument]` composes with concurrency by having nothing to compose —
it wraps whatever the function does and gets out of the way, at the cost
of never answering "which call is this exit line for" once two threads
print at once.

## Build, run, observe

```bash
[host]$ cd examples/43-rust-macros-for-systems && ./demo.sh build
```

`./demo.sh rust run` builds (if needed) and runs the release binary on the
pinned stable toolchain — the exact path `verify.lua` exercises:

```console
[host]$ ./demo.sh rust run
error: open /etc/shadow failed: 13
error: child 4242 exited with status 1
error: policy rejected: sandbox blocked os.execute
-> hash_chunk(len=4096)
<- hash_chunk = 10911624183873311525 (4us)
record: magic=0x4b565053 slots=256
```

Six lines, three techniques: lines 1–3 are `#[derive(SysError)]`'s
generated `Display`, one line per variant shape (named-field, multi-named-field,
tuple); lines 4–5 are `#[instrument]` around `hash_chunk(len=4096)` — the
hash itself (`10911624183873311525`) is a deterministic FNV-1a over
`0..4096`, so it is identical on every run, and only the microsecond
figure (`4us` here) varies; line 6 is the sound `read_record` call
printing the `Record` it read.

`./demo.sh rust miri` runs the identical scenario under
`cargo +nightly miri run --quiet` — the interpreter, not the compiled
binary. It prints the same six lines and exits 0 (no UB reported), with
one visible difference: the instrument line reads
`<- hash_chunk = 10911624183873311525 (491785us)` instead of `(4us)`. The
hash value does not change — determinism holds whether the code runs
natively or under an interpreter — but the wall-clock duration jumps from
4 microseconds to roughly 491,785 microseconds, a slowdown of about five
orders of magnitude. That is not a regression to worry about: `miri` is a
UB checker, not a profiler, and it re-executes every memory operation
through its own bookkeeping instead of the CPU directly. The lesson is in
the contrast, not the absolute number — a wall-clock measurement taken
under `miri` tells you nothing about the compiled program's real
performance, only about whether its memory operations are sound.

`./demo.sh rust miri -- --oob-read` runs the same binary with the
deliberately out-of-bounds path enabled, and this time `miri` stops
before printing the sixth line at all:

```console
[host]$ cargo +nightly miri run --quiet -- --oob-read
error: Undefined Behavior: memory access failed: attempting to access 8 bytes, but got alloc25+0x4 which is only 4 bytes from the end of the allocation
  --> src/main.rs:57:14
   |
57 |     unsafe { core::ptr::read_unaligned(buf.as_ptr().add(off) as *const Record) }
   |              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ Undefined Behavior occurred here
   |
   = help: this indicates a bug in the program: it performed an invalid operation, and caused Undefined Behavior
   = help: see https://doc.rust-lang.org/nightly/reference/behavior-considered-undefined.html for further information
   = note: stack backtrace: 0: read_record (src/main.rs:57) 1: main (src/main.rs:76)
```

`alloc25` is `miri`'s own name for the specific allocation `BUF` occupies
in its interpreted heap; `+0x4` is the offset the failing read started at
(`off = 4` on the `--oob-read` path); "only 4 bytes from the end of the
allocation" is `miri` doing arithmetic a native run never does — `BUF` is
8 bytes, the read needs 8 more bytes starting at offset 4, and offset 4
plus 8 is 12, four bytes past where the allocation actually ends. The
line and column (`src/main.rs:57:14`) point at the exact `unsafe` block
quoted above. On stable Rust this same `--oob-read` invocation runs to
completion, silently reading whatever garbage bytes happen to sit past
`BUF` in memory — no error, no crash, no signal that anything was wrong.

The gate the CI-equivalent runner checks is the stable path only:

```console
[host]$ python3 scripts/test-all-examples.py --only 43-rust-macros-for-systems
...
1 passed, 0 failed, 0 skipped
```

```console
[host]$ cd examples/43-rust-macros-for-systems && LSP_LANG=rust lua verify.lua
...
PASS 8 / FAIL 0
```

## Cross-check: the generated code is exactly what a systems error type needs, and the unsafe read only ever touches in-bounds bytes

Two independent claims, two independent proofs. The first is that
`#[derive(SysError)]`'s generated `Display` impl is not an approximation
of a hand-written systems error type — it *is* one: `verify.lua` asserts
the exact three lines `PmonError`'s variants must produce
(`open /etc/shadow failed: 13`, `child 4242 exited with status 1`,
`policy rejected: sandbox blocked os.execute`), plus the `#[instrument]`
entry/exit shape and the parsed `Record` fields, for `PASS 8 / FAIL 0` —
eight assertions against behaviour, not "the binary exited 0." The second
is that the sound `read_record` path really is sound and the
`--oob-read` path really is a bug, and that claim is checked by an
interpreter rather than taken on faith: the identical scenario run under
`cargo +nightly miri run` exits 0 with no UB reported on the default path,
and reports the precise out-of-bounds access — 4 bytes past an 8-byte
allocation, at `src/main.rs:57` — the moment `--oob-read` is passed. The
491785us-versus-4us timing gap is the same proof from a different angle:
it shows `miri` is genuinely re-executing every memory access through its
own interpreter rather than fast-pathing to the native instruction, which
is exactly the mechanism that lets it catch a bounds violation a native
`cargo run` sails straight through.

## What you learned

- **A proc-macro crate is a second, separate compilation**: `sysmacros`
  sets `proc-macro = true` and is compiled by rustc as a compiler plugin
  before `main.rs` is ever touched — every proc-macro is `TokenStream` in,
  `TokenStream` out, and none of that code exists at `app`'s runtime.
- **`#[derive(SysError)]` earns the name "miniature `thiserror`"**: one
  `syn::Result<TokenStream2>` function parses each variant's
  `#[error("...")]` string with `syn 2.0`, builds a `Display` match arm
  per `Fields` shape (named, tuple, unit) with `quote!`, and edition 2024's
  inline format-arg capture resolves `{path}`/`{errno}` straight against
  the destructured match bindings — no positional arguments needed.
- **Malformed macro input is a `compile_error!`, never a panic**: both
  macros convert a `syn::Error` into real tokens via
  `err.to_compile_error()`, so a missing `#[error(...)]` attribute or a
  non-enum target fails the build with a located diagnostic, the same
  discipline a hand-written derive would need to earn.
- **`unsafe` is a promise the type system stopped checking, and `miri`
  checks it anyway**: the sound `read_record` path's `assert!` makes its
  `SAFETY` comment true; the `--oob-read` path skips that assert and reads
  4 bytes past an 8-byte allocation — silent on stable, a named Undefined
  Behavior report under `cargo +nightly miri run`.
- **`miri` is a correctness checker, not a profiler**: the identical
  scenario took 4 microseconds natively and roughly 491,785 microseconds
  under `miri` — a real, expected, five-order-of-magnitude interpreter
  slowdown that says nothing about `app`'s real performance and everything
  about whether its memory operations are sound.
- **`#[instrument]` is thread-agnostic because it holds no state at all**:
  it wraps whatever function it's given in a `println!`/`Instant`/`println!`
  sandwich with no shared subscriber or span stack, unlike a runtime
  tracing crate that has to maintain one — composable with any threading
  model precisely because it has nothing to synchronize.

Two deep dives remain in this part: this chapter's Rust-only look at
compile-time metaprogramming, and the next — the Go runtime itself, from
a systems programmer's seat.

---

<p><span class="status status--verified">verified</span> — on the Fedora
44 reference host this session: <code>python3 scripts/test-all-examples.py
--only 43-rust-macros-for-systems</code> reported <code>1 passed, 0
failed</code> (rust column only; this is the book's first single-language
example), and <code>LSP_LANG=rust lua verify.lua</code> reported
<code>PASS 8 / FAIL 0</code>. <code>./demo.sh rust run</code> on the pinned
stable <code>rustc 1.97.1</code> toolchain produced the exact six-line
transcript quoted above: the three <code>#[derive(SysError)]</code>
<code>Display</code> lines, the <code>#[instrument]</code> entry/exit pair
around <code>hash_chunk(len=4096)</code> (hash
<code>10911624183873311525</code>, deterministic; <code>4us</code> measured
this run), and the parsed <code>record: magic=0x4b565053 slots=256</code>
line. <code>cargo +nightly miri run --quiet</code> (the sound path) exited 0
with no Undefined Behavior reported, printing the same six lines with the
instrument line's duration at <code>491785us</code> instead of
<code>4us</code> — a real ~5-order-of-magnitude interpreter slowdown, not a
regression. <code>cargo +nightly miri run --quiet -- --oob-read</code>
reproduced the exact Undefined Behavior diagnostic quoted above: memory
access failed at <code>alloc25+0x4</code>, "only 4 bytes from the end of
the allocation," located at <code>src/main.rs:57:14</code>, the
<code>read_unaligned</code> call inside <code>read_record</code>. Crate
pins: <code>syn 2.0</code> (features <code>full</code>), <code>quote
1.0</code>, <code>proc-macro2 1.0</code>, on stable <code>rustc
1.97.1</code> (<code>rust-toolchain.toml</code>) with a separately
installed nightly toolchain plus the <code>miri</code> component via
<code>rustup</code>. Not exercised: this example is <code>mode: local</code>
per <code>examples/manifest.yaml</code> — there is no VM or LGTM path for
it; and the <code>--oob-read</code> path is only ever run under
<code>miri</code> in this book, never asserted on stable Rust, where the
same call is undefined behaviour rather than a checkable outcome.</p>
