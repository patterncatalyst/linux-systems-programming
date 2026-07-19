//! bugfarm — chapter 29: valgrind, sanitizers, and miri.
//!
//!   app <leak|uaf|uninit|overflow|race>
//!
//! Safe Rust's compile-time guarantees rule out four of the five seeded
//! defects outright: the borrow checker won't let a use-after-free or a
//! data race compile, and there's no way to name an uninitialised value or
//! index past a slice's bound without either a panic (bounds check) or an
//! explicit `unsafe` block. So `uaf`, `uninit`, `overflow`, and `race` each
//! print a fixed note and exit 64 -- the compile-time rejection *is* the
//! finding, reproduced here as a documented outcome rather than a crash.
//!
//! What safe Rust can still do is leak memory on purpose: `Box::leak`
//! consumes a `Box` and hands back a `'static` reference, deliberately
//! opting the allocation out of Rust's normal drop-based reclamation.
//! Nothing frees it -- valgrind's memcheck reports the same "definitely
//! lost" block it would for an equivalent C/C++ leak.

use std::env;
use std::hint::black_box;
use std::process::ExitCode;

const USAGE: &str = "usage: app <leak|uaf|uninit|overflow|race>";
const PREVENTED: &str = "rust: prevented at compile time — see chapter";

#[derive(Clone, Copy)]
enum Bug {
    Leak,
    Uaf,
    Uninit,
    Overflow,
    Race,
}

fn parse_bug(arg: &str) -> Result<Bug, String> {
    match arg {
        "leak" => Ok(Bug::Leak),
        "uaf" => Ok(Bug::Uaf),
        "uninit" => Ok(Bug::Uninit),
        "overflow" => Ok(Bug::Overflow),
        "race" => Ok(Bug::Race),
        other => Err(format!("unknown bug: {other}")),
    }
}

// leak: Box::leak is a safe API whose entire purpose is to opt an
// allocation out of automatic reclamation -- the program exits normally,
// but the 64 KiB Vec (and the Box that briefly held it) are never freed.
//
// black_box on the pointer is load-bearing, not decoration: without it,
// LLVM can see that nothing observable depends on the buffer's *contents*
// (only its statically-known length gets printed) and optimizes the
// entire allocation away, leak and all -- confirmed on this host by
// valgrind reporting zero bytes lost until the barrier was added.
fn leak() {
    let leaked: &'static mut Vec<u8> = Box::leak(Box::new(vec![0xAAu8; 65536]));
    leaked[0] = 0xAA; // touch it so the write isn't provably dead
    black_box(leaked.as_ptr());
    println!(
        "bugfarm: leak: leaked {} bytes via Box::leak, never reclaimed (intentional)",
        leaked.len()
    );
}

/// Returns `Ok(())` on the expressible bug (leak), or `Err(64)` for a bug
/// safe Rust prevents outright, or `Err(2)` for a usage error.
fn run(args: &[String]) -> Result<(), i32> {
    if args.len() != 1 {
        return Err(2);
    }
    let bug = parse_bug(&args[0]).map_err(|e| {
        eprintln!("app: {e}");
        2
    })?;
    match bug {
        Bug::Leak => {
            leak();
            Ok(())
        }
        Bug::Uaf | Bug::Uninit | Bug::Overflow | Bug::Race => {
            eprintln!("{PREVENTED}");
            Err(64)
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(2) => {
            eprintln!("{USAGE}");
            ExitCode::from(2)
        }
        Err(code) => ExitCode::from(code as u8),
    }
}
