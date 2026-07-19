//! toolbelt — three focused profiling targets, one binary per language, each
//! exercised by that language's native profiler (chapter 31: per-language
//! toolbelts).
//!
//!   app <hot|alloc> [--n N]
//!
//! hot:   counts primes in [2, n) by trial division. `spin_hot` is the only
//!        function that does real work — it dominates a CPU profile (perf
//!        record/report, or `cargo flamegraph` where installable).
//! alloc: builds a 1000-key string index by round-robin overwrite from n
//!        iterations of freshly allocated strings. `alloc_churn` is the
//!        only allocation site — it dominates an allocation profile.
//!
//! Output: "app: mode=<hot|alloc> n=<n> result=<r> ms=<t>" on success.

use std::collections::HashMap;
use std::process::ExitCode;
use std::time::Instant;

use anyhow::{Result, bail};

const DEFAULT_HOT_N: u64 = 3_000_000;
const DEFAULT_ALLOC_N: u64 = 200_000;
const KEYSPACE: u64 = 1_000; // distinct keys; the rest is churn

struct Config {
    mode: &'static str,
    n: u64,
}

fn usage() {
    eprintln!("usage: app <hot|alloc> [--n N]");
}

fn parse_args() -> Result<Config, String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        return Err("missing mode".to_string());
    }
    let mode: &'static str = match args[0].as_str() {
        "hot" => "hot",
        "alloc" => "alloc",
        other => return Err(format!("unknown mode: {other}")),
    };
    let mut cfg = Config {
        mode,
        n: if mode == "hot" {
            DEFAULT_HOT_N
        } else {
            DEFAULT_ALLOC_N
        },
    };

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--n" if i + 1 < args.len() => {
                i += 1;
                cfg.n = match args[i].parse::<u64>() {
                    Ok(v) if v > 0 => v,
                    _ => return Err(format!("not a positive integer: {}", args[i])),
                };
            }
            other => return Err(format!("unknown argument: {other}")),
        }
        i += 1;
    }
    Ok(cfg)
}

/// The CPU-bound target: trial division is deliberately unoptimized (checks
/// every candidate divisor up to sqrt(i)) so a couple of seconds of runtime
/// buys a profile with a single, unmistakable hot frame. `#[inline(never)]`
/// keeps the compiler from folding it into `run` and hiding it from the
/// sampler.
#[inline(never)]
fn spin_hot(n: u64) -> u64 {
    let mut count = 0u64;
    for i in 2..n {
        let mut prime = true;
        let mut d = 2u64;
        while d * d <= i {
            if i % d == 0 {
                prime = false;
                break;
            }
            d += 1;
        }
        if prime {
            count += 1;
        }
    }
    count
}

/// The allocation-heavy target: n iterations of churn, each a fresh key and
/// value string, round-robin overwriting a 1000-entry index. Every
/// allocation in the program happens inside this one function.
#[inline(never)]
fn alloc_churn(n: u64) -> Result<u64> {
    let mut index: HashMap<String, String> = HashMap::with_capacity(KEYSPACE as usize);
    for i in 0..n {
        let idx = i % KEYSPACE;
        let key = format!("k{idx}");
        let value = format!("{i:x}");
        index.insert(key, value);
    }

    let want = n.min(KEYSPACE) as usize;
    if index.len() != want {
        bail!("index has {} entries, want {want}", index.len());
    }
    let total: u64 = index.values().map(|v| v.len() as u64).sum();
    if total == 0 {
        bail!("summed zero bytes");
    }
    Ok(total)
}

fn run(cfg: &Config) -> Result<u64> {
    match cfg.mode {
        "hot" => Ok(spin_hot(cfg.n)),
        "alloc" => alloc_churn(cfg.n),
        _ => unreachable!("parse_args only produces hot|alloc"),
    }
}

fn main() -> ExitCode {
    let cfg = match parse_args() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("app: {e}");
            usage();
            return ExitCode::from(2);
        }
    };

    let t0 = Instant::now();
    let result = run(&cfg);
    let ms = t0.elapsed().as_millis();

    match result {
        Ok(r) => {
            println!("app: mode={} n={} result={r} ms={ms}", cfg.mode, cfg.n);
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("app: {e:#}");
            ExitCode::from(1)
        }
    }
}
