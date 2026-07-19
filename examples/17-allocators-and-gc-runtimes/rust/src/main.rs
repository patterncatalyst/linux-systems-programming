// allocbench — allocation-heavy index build/query (chapter 17: allocators
// and GC runtimes).
//
//   allocbench [--allocs N] [--variant default|arena]
//
// Both variants build the same string->string index (1000 distinct keys,
// overwritten round-robin) from N iterations of short-lived intermediate
// strings, then query every distinct key back. What differs is where the
// intermediates live:
//
//   default: every key/frag/value is a plain String — each a trip to the
//            global allocator, each freed individually when its batch
//            scratch Vec is dropped.
//   arena:   a bumpalo::Bump arena. Per batch of 1000 iterations the
//            intermediates are bumpalo::collections::String values carved
//            out of the bump by pointer arithmetic; nothing is freed
//            per-object. After each batch, `bump.reset()` reclaims the
//            whole region in O(1) and the next batch reuses the same
//            chunks. Only the final key/value pairs are copied out into
//            the long-lived index.
//
// Reports: "allocbench: variant=<v> allocs=<n> peak_rss=<kb>KB ms=<t>"
// (peak_rss is getrusage(2) ru_maxrss — the VmHWM high-water mark — in KB).

use std::collections::HashMap;
use std::fmt::Write as _;
use std::process::ExitCode;
use std::time::Instant;

use anyhow::{Context, Result, bail};
use bumpalo::Bump;
use bumpalo::collections::String as BumpString;
use bumpalo::collections::Vec as BumpVec;

const DEFAULT_ALLOCS: usize = 200_000;
const KEYSPACE: usize = 1_000; // distinct keys; the rest is churn
const BATCH: usize = 1_000; // arena reset granularity (iterations)
const REPEAT: usize = 4; // value = frag repeated REPEAT times
const SLAB_BYTES: usize = 512 * 1024; // initial bump capacity

struct Config {
    allocs: usize,
    arena: bool,
}

fn usage() {
    eprintln!("usage: allocbench [--allocs N] [--variant default|arena]");
}

fn parse_args() -> Result<Config, String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut cfg = Config {
        allocs: DEFAULT_ALLOCS,
        arena: false,
    };
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--allocs" if i + 1 < args.len() => {
                i += 1;
                cfg.allocs = match args[i].parse::<usize>() {
                    Ok(n) if n > 0 => n,
                    _ => return Err(format!("not a positive integer: {}", args[i])),
                };
            }
            "--variant" if i + 1 < args.len() => {
                i += 1;
                match args[i].as_str() {
                    "default" => cfg.arena = false,
                    "arena" => cfg.arena = true,
                    other => return Err(format!("unknown variant: {other}")),
                }
            }
            other => return Err(format!("unknown argument: {other}")),
        }
        i += 1;
    }
    Ok(cfg)
}

/// default variant: plain heap strings, freed one by one at batch end.
fn build_default(allocs: usize) -> HashMap<String, String> {
    let mut index = HashMap::with_capacity(KEYSPACE);
    let mut start = 0;
    while start < allocs {
        let end = (start + BATCH).min(allocs);
        let mut scratch: Vec<String> = Vec::with_capacity(end - start);
        for i in start..end {
            let idx = i % KEYSPACE;
            let key = format!("key-{idx}");
            let frag = format!("value-{idx}-{}/", i % 97);
            let mut value = String::new();
            for _ in 0..REPEAT {
                value.push_str(&frag);
            }
            index.insert(key, value);
            scratch.push(frag); // keeps the churn alive per batch
        }
        start = end;
    } // drop(scratch): one deallocation per surviving string
    index
}

/// arena variant: same shape, but the intermediates are bump-allocated and
/// the whole batch is reclaimed at once by `bump.reset()`.
fn build_arena(allocs: usize) -> HashMap<String, String> {
    let mut bump = Bump::with_capacity(SLAB_BYTES);
    let mut index = HashMap::with_capacity(KEYSPACE);
    let mut start = 0;
    while start < allocs {
        let end = (start + BATCH).min(allocs);
        {
            let mut scratch: BumpVec<BumpString> = BumpVec::with_capacity_in(end - start, &bump);
            for i in start..end {
                let idx = i % KEYSPACE;
                let mut key = BumpString::new_in(&bump);
                write!(key, "key-{idx}").expect("bump string write cannot fail");
                let mut frag = BumpString::new_in(&bump);
                write!(frag, "value-{idx}-{}/", i % 97).expect("bump string write cannot fail");
                let mut value = BumpString::new_in(&bump);
                for _ in 0..REPEAT {
                    value.push_str(frag.as_str());
                }
                index.insert(key.as_str().to_owned(), value.as_str().to_owned());
                scratch.push(frag); // keeps the churn alive per batch
            }
        } // borrows on the bump end here...
        bump.reset(); // ...so the whole batch vanishes in O(1)
        start = end;
    }
    index
}

fn query(index: &HashMap<String, String>, allocs: usize) -> Result<u64> {
    let distinct = allocs.min(KEYSPACE);
    let mut total: u64 = 0;
    for idx in 0..distinct {
        let key = format!("key-{idx}");
        let value = index
            .get(&key)
            .with_context(|| format!("missing key: {key}"))?;
        total += value.len() as u64;
    }
    if total == 0 {
        bail!("query summed zero bytes");
    }
    Ok(total)
}

fn peak_rss_kb() -> Result<i64> {
    let mut ru = std::mem::MaybeUninit::<libc::rusage>::zeroed();
    // SAFETY: RUSAGE_SELF with a properly sized, writable rusage out-param.
    let rc = unsafe { libc::getrusage(libc::RUSAGE_SELF, ru.as_mut_ptr()) };
    if rc != 0 {
        return Err(std::io::Error::last_os_error()).context("getrusage");
    }
    // SAFETY: getrusage returned 0, so the struct is fully initialized.
    let ru = unsafe { ru.assume_init() };
    Ok(ru.ru_maxrss) // kilobytes on Linux
}

fn run(cfg: &Config) -> Result<()> {
    let t0 = Instant::now();
    let index = if cfg.arena {
        build_arena(cfg.allocs)
    } else {
        build_default(cfg.allocs)
    };
    query(&index, cfg.allocs)?;
    let ms = t0.elapsed().as_millis();

    let peak = peak_rss_kb()?;
    let variant = if cfg.arena { "arena" } else { "default" };
    println!(
        "allocbench: variant={variant} allocs={} peak_rss={peak}KB ms={ms}",
        cfg.allocs
    );
    Ok(())
}

fn main() -> ExitCode {
    let cfg = match parse_args() {
        Ok(cfg) => cfg,
        Err(err) => {
            eprintln!("allocbench: {err}");
            usage();
            return ExitCode::from(2);
        }
    };
    if let Err(err) = run(&cfg) {
        eprintln!("allocbench: {err:#}");
        return ExitCode::from(1);
    }
    ExitCode::SUCCESS
}
