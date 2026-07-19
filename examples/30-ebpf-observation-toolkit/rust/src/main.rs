//! app work --seconds N — a small, observable workload for the eBPF
//! observation toolkit (chapter 30). It does four things in a loop, each one
//! deliberate bait for a specific bcc-tools/bpftrace probe:
//!
//!   1. opens (create+write+close) a fixed file every iteration  -> opensnoop
//!   2. fork/execs a short-lived child ("true") every 4th iter    -> execsnoop
//!   3. calls the named hot function busy_hash() every iteration  -> funccount / uprobe
//!   4. sleeps most of the iteration, so the process is off-CPU    -> offcputime
//!
//! This file writes no kernel-side eBPF: it is the userspace *target* that
//! examples/30-ebpf-observation-toolkit/observe.sh (running as root on the
//! lab VM) points bcc-tools and bpftrace at.
//!
//! Rust idioms: every fallible call flows through Result and `?`; the bait
//! file's descriptor is a rustix OwnedFd, closed by its Drop impl the moment
//! it goes out of scope — no explicit close(2).

use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use rustix::fs::{Mode, OFlags, open};
use rustix::io::write;

const ITER_INTERVAL: Duration = Duration::from_millis(230);
const EXEC_EVERY_N_ITERS: u64 = 4;
const BUSY_ROUNDS: u64 = 300_000;
const BAIT_PATH: &str = "/tmp/lsp-ebpf-work-bait.txt";

/// The named hot function every uprobe/funccount/bpftrace command in this
/// chapter targets by name. `#[unsafe(no_mangle)]` keeps the symbol as the
/// plain string "busy_hash" (matching the C++ build); `#[inline(never)]`
/// keeps the optimizer from folding it into its caller and making the
/// uprobe's attach point disappear.
#[unsafe(no_mangle)]
#[inline(never)]
pub extern "C" fn busy_hash(mut x: u64) -> u64 {
    for _ in 0..BUSY_ROUNDS {
        x ^= x << 7;
        x ^= x >> 9;
    }
    x
}

/// opensnoop bait: create/truncate, write a line, close. Every call is a
/// fresh open(2) on the same path; the OwnedFd's Drop does the close(2).
fn open_bait(iter: u64) -> Result<()> {
    // 0666: the bait file may get created by a root-run observe.sh one time
    // and an unprivileged demo.sh run the next — world-writable avoids a
    // permission mismatch between those two ownership scenarios.
    let fd = open(
        BAIT_PATH,
        OFlags::CREATE | OFlags::WRONLY | OFlags::TRUNC,
        Mode::from_raw_mode(0o666),
    )
    .context("open bait")?;
    write(&fd, format!("iter {iter}\n").as_bytes()).context("write bait")?;
    Ok(())
    // `fd` (an OwnedFd) drops here, closing the descriptor.
}

/// execsnoop bait: spawn "true" and wait for it. std::process::Command does
/// the fork+exec; the child's PPID is this process either way.
fn spawn_true() -> Result<i32> {
    let status = std::process::Command::new("true")
        .status()
        .context("spawn true")?;
    Ok(status.code().unwrap_or(-1))
}

#[derive(Default)]
struct Counters {
    iters: u64,
    opens: u64,
    execs: u64,
    busy_calls: u64,
    // The accumulated busy_hash() result, printed in the summary line. LLVM
    // can prove a #[no_mangle]/#[inline(never)] function is still pure and
    // dead-code-eliminate a whole call chain whose final result is never
    // observed — keeping this value alive here is what guarantees every call
    // really executed, not just that the symbol exists in the binary.
    busy_hash: u64,
}

fn run_workload(seconds: u64) -> Counters {
    let deadline = Instant::now() + Duration::from_secs(seconds);
    let mut c = Counters::default();
    let mut acc: u64 = 1;
    while Instant::now() < deadline {
        match open_bait(c.iters) {
            Ok(()) => c.opens += 1,
            Err(e) => eprintln!("work: {e:#}"),
        }

        if c.iters % EXEC_EVERY_N_ITERS == 0 {
            match spawn_true() {
                Ok(status) => {
                    c.execs += 1;
                    println!("work: exec i={} status={}", c.iters, status);
                }
                Err(e) => eprintln!("work: {e:#}"),
            }
        }

        acc = busy_hash(acc);
        c.busy_calls += 1;

        std::thread::sleep(ITER_INTERVAL);
        c.iters += 1;
    }
    c.busy_hash = acc;
    c
}

fn cmd_work(seconds: u64) -> i32 {
    println!(
        "work: start seconds={} pid={} bait={}",
        seconds,
        std::process::id(),
        BAIT_PATH
    );
    let c = run_workload(seconds);
    println!(
        "work: done seconds={} iters={} opens={} execs={} busy_calls={} busy_hash={}",
        seconds, c.iters, c.opens, c.execs, c.busy_calls, c.busy_hash
    );
    0
}

fn usage() {
    eprintln!("usage: app work --seconds N");
}

fn main() -> std::process::ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 || args[1] != "work" {
        usage();
        return std::process::ExitCode::from(2);
    }
    let mut seconds: i64 = -1;
    let mut i = 2;
    while i < args.len() {
        if args[i] == "--seconds" && i + 1 < args.len() {
            match args[i + 1].parse::<i64>() {
                Ok(v) if v > 0 => seconds = v,
                _ => {
                    eprintln!("work: bad --seconds value: {}", args[i + 1]);
                    return std::process::ExitCode::from(2);
                }
            }
            i += 2;
        } else {
            usage();
            return std::process::ExitCode::from(2);
        }
    }
    if seconds <= 0 {
        usage();
        return std::process::ExitCode::from(2);
    }
    std::process::ExitCode::from(cmd_work(seconds as u64) as u8)
}
