# 43 — rust-macros-for-systems

The book's first **single-language** example (Rust only): the deep dives don't
mirror across three languages. It shows three metaprogramming techniques a
systems programmer actually reaches for, as a small cargo **workspace** — a
proc-macro crate plus the binary that uses it:

- **`#[derive(SysError)]`** — a derive macro that generates `Display` + `std::error::Error`
  from `#[error("...")]` attributes (a miniature `thiserror`), for errno-style
  error enums.
- **`#[instrument]`** — an attribute macro that wraps a function to log its
  entry (name + args), exit (return value), and wall-clock duration — tracing
  without a runtime.
- **A sound `unsafe` unaligned read**, validated with **miri** — plus a
  defect-named `--oob-read` path that miri flags as undefined behaviour.

## Layout

```
43-rust-macros-for-systems/
├── demo.sh            # dispatcher (rust only)
├── verify.lua         # rust-only behavioural check
└── rust/
    ├── Cargo.toml     # workspace root + `app` bin package
    ├── src/main.rs    # the app: uses the macros + the unsafe module
    └── sysmacros/     # the proc-macro crate (proc-macro = true)
        └── src/lib.rs # SysError derive + instrument attribute macro
```

## Run it

```bash
./demo.sh rust run          # the scenario on stable 1.97.1 (what CI verifies)
./demo.sh rust miri         # cargo +nightly miri run — the sound path, no UB
./demo.sh rust miri -- --oob-read   # miri catches the out-of-bounds read
```

## Verify

`verify.lua` (Rust only; skips other langs) asserts the generated `Display`
strings, the `#[instrument]` entry/exit/timing lines, and the parsed record —
behaviour, not exit codes. The gate runs on the **pinned stable** toolchain;
miri (nightly) is exercised by the chapter, not the gate.

```bash
LSP_LANG=rust lua verify.lua
```

Mode: `local`. Crates: `syn 2.0`, `quote 1.0`, `proc-macro2 1.0`.
