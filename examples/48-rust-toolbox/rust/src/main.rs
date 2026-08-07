//! toolbox -- the reference Rust program for ch48 (rustup/rust-toolchain.toml
//! pinning, rustfmt, clippy, cargo test, and the skip-if-present
//! nextest/deny/llvm-cov/audit accelerators).
//!
//! Subcommands:
//!   toolbox report  -- deterministic FNV-1a digest over a fixed embedded
//!                       payload, integer/string output only (no floats,
//!                       addresses, timing, or hash-map iteration), so the
//!                       output is bit-for-bit reproducible across runs.
#![warn(clippy::all)]

mod digest;

#[cfg(feature = "lintbait")]
mod lintbait;

use digest::{PAYLOAD, fnv1a};

fn print_usage() {
    eprintln!("usage: toolbox <report>");
}

fn cmd_report() {
    let d = fnv1a(&PAYLOAD);
    println!(
        "toolbox report: payload_len={} digest=0x{:016x}",
        PAYLOAD.len(),
        d.fnv
    );
}

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
