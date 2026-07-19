//! app -- bugfarm scenario 1: a crashing pmon-like supervisor
//! (chapter 28: gdb and remote debugging).
//!
//!   app run    -- normal operation: reap a child, report its exit, exit 0
//!   app crash  -- the SAME call path, fed a child record whose exit-status
//!                 slot was never populated (as if handle_child ran before
//!                 the reaper filled it in) -> Option::unwrap() on a None
//!                 three frames down:
//!
//!                   main -> run_supervisor -> handle_child -> deref
//!
//! This binary is deliberately dual-use: a normal program in "run" mode, a
//! debugging TARGET in "crash" mode.
//!
//! Rust's answer to the C++ null-pointer bug class: there is no raw pointer
//! to leave dangling, so the equivalent defect is an `Option` that should
//! have been filled in by now and was not. `.unwrap()` on that `None` panics
//! rather than segfaulting -- with `panic = "unwind"` (this crate's default)
//! gdb sees the unwinder's machinery, not a clean SIGSEGV, so
//! `RUST_BACKTRACE=1` is the idiomatic tool here, printing every frame below
//! by name. Each frame function below is `#[inline(never)]` so release
//! optimizations cannot fold the call chain away.

#[derive(Clone, Copy)]
struct ChildRecord {
    pid: u32,
    exit_slot: Option<i32>,
}

#[inline(never)]
fn deref(child: &ChildRecord) {
    // BUG: unwrap() with no check. In "crash" mode exit_slot is None because
    // handle_child ran before the reaper recorded the exit status -- exactly
    // the kind of race a real supervisor can hit.
    println!(
        "pmon: child {} exited status={}",
        child.pid,
        child.exit_slot.unwrap()
    );
}

#[inline(never)]
fn handle_child(child: &ChildRecord) {
    deref(child);
    // A no-op after the call: without it, `deref` is the last thing this
    // function does and the optimizer turns the call into a tail jump,
    // reusing this frame and erasing `handle_child` from the backtrace.
    std::hint::black_box(());
}

#[inline(never)]
fn run_supervisor(inject_bug: bool) -> i32 {
    println!("pmon: supervisor starting");
    let child = ChildRecord {
        pid: 4242,
        exit_slot: if inject_bug { None } else { Some(0) },
    };
    if inject_bug {
        handle_child(&child); // crash path: exit_slot stays None
        return 0; // unreachable
    }
    handle_child(&child);
    println!("pmon: supervisor exiting cleanly");
    0
}

enum Mode {
    Run,
    Crash,
}

fn parse_mode(s: &str) -> Result<Mode, String> {
    match s {
        "run" => Ok(Mode::Run),
        "crash" => Ok(Mode::Crash),
        other => Err(format!("unknown subcommand: {other}")),
    }
}

fn usage() -> ! {
    eprintln!("usage: app run");
    eprintln!("       app crash");
    std::process::exit(2);
}

/// Parses argv down to a `Mode` using `?`; any usage error is handled by the
/// caller, which prints the same two-line usage block as the other two
/// languages regardless of which parse step failed.
fn try_parse(args: &[String]) -> Result<Mode, String> {
    if args.len() != 2 {
        return Err("wrong number of arguments".to_string());
    }
    let mode = parse_mode(&args[1])?;
    Ok(mode)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let Ok(mode) = try_parse(&args) else {
        usage();
    };
    let code = run_supervisor(matches!(mode, Mode::Crash));
    std::process::exit(code);
}
