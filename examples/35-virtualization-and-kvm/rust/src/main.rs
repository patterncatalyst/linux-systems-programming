// overheadbench: three host-local microbenchmarks used to talk about
// virtualization/container overhead in chapter 35 —
//   (a) syscall latency: a tight getpid(2) loop, ns/call
//   (b) memory bandwidth: sequential read of a big buffer, GB/s
//   (c) small-file IO: create/write/fsync/unlink loop, ops/s
//
//   overheadbench [--bench syscall|mem|io|all] [--iters N]
//
// Prints one line per bench: "bench=<name> metric=<value> unit=<unit>".
// This binary always measures wherever it runs (host, VM, or container) —
// the chapter runs the *same* binary in all three places and tabulates the
// numbers; this example itself only asserts the host numbers are sane.

use std::fmt;
use std::fs::{self, OpenOptions};
use std::hint::black_box;
use std::io::Write as _;
use std::time::Instant;

const USAGE: &str = "usage: overheadbench [--bench syscall|mem|io|all] [--iters N]";

// Defaults are chosen so each bench runs for roughly tens to a few hundred
// milliseconds on a modern host; identical across all three languages.
const SYSCALL_DEFAULT_ITERS: u64 = 200_000;
const MEM_DEFAULT_PASSES: u64 = 16;
const MEM_BUF_BYTES: u64 = 128 * 1024 * 1024; // 128 MiB
const IO_DEFAULT_ITERS: u64 = 200;
const IO_FILE_BYTES: usize = 4096;

#[derive(Clone, Copy, PartialEq)]
enum Bench {
    Syscall,
    Mem,
    Io,
    All,
}

struct Config {
    bench: Bench,
    iters: Option<u64>,
}

#[derive(Debug)]
struct ArgError(String);

impl fmt::Display for ArgError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

fn parse_args(args: &[String]) -> Result<Config, ArgError> {
    let mut cfg = Config {
        bench: Bench::All,
        iters: None,
    };
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--bench" => {
                i += 1;
                let v = args
                    .get(i)
                    .ok_or_else(|| ArgError("--bench requires a value".into()))?;
                cfg.bench = match v.as_str() {
                    "syscall" => Bench::Syscall,
                    "mem" => Bench::Mem,
                    "io" => Bench::Io,
                    "all" => Bench::All,
                    other => return Err(ArgError(format!("unknown --bench value: {other}"))),
                };
            }
            "--iters" => {
                i += 1;
                let v = args
                    .get(i)
                    .ok_or_else(|| ArgError("--iters requires a value".into()))?;
                let n: u64 = v.parse().map_err(|_| {
                    ArgError(format!("--iters value must be a positive integer: {v}"))
                })?;
                if n == 0 {
                    return Err(ArgError("--iters value must be >= 1".into()));
                }
                cfg.iters = Some(n);
            }
            other => return Err(ArgError(format!("unknown argument: {other}"))),
        }
        i += 1;
    }
    Ok(cfg)
}

/// Tight getpid(2) loop, ns/call.
fn bench_syscall(iters: u64) -> f64 {
    let start = Instant::now();
    for _ in 0..iters {
        // rustix::process::getpid issues the raw getpid(2) syscall every
        // call — every iteration really crosses into the kernel, the
        // syscall-boundary cost this chapter contrasts against VM (vmexit
        // trap) and container (near-zero extra) overhead.
        black_box(rustix::process::getpid());
    }
    let mut ns = start.elapsed().as_nanos() as f64;
    if ns <= 0.0 {
        ns = 1.0;
    }
    ns / iters as f64
}

/// Sequential read bandwidth over a big buffer, GB/s.
fn bench_mem(passes: u64) -> f64 {
    let words = (MEM_BUF_BYTES / 8) as usize;
    let mut buf = vec![0u64; words];
    for (i, w) in buf.iter_mut().enumerate() {
        *w = (i as u64).wrapping_mul(2654435761);
    }

    let mut sum: u64 = 0;
    let start = Instant::now();
    for _ in 0..passes {
        for &w in &buf {
            sum = sum.wrapping_add(w);
        }
    }
    let mut secs = start.elapsed().as_secs_f64();
    if secs <= 0.0 {
        secs = 1e-9;
    }

    // Consume `sum` after the clock stops so the compiler cannot prove the
    // whole scan is dead and elide it; this doesn't affect the measured time.
    black_box(sum);

    let bytes = MEM_BUF_BYTES as f64 * passes as f64;
    bytes / secs / 1e9
}

/// Create/write/fsync/unlink loop, ops/s.
fn bench_io(iters: u64) -> Result<f64, String> {
    // Relative to the CWD (the demo.sh working directory), not the system
    // temp dir: on many dev hosts that's tmpfs, where fsync is nearly free
    // and the number stops meaning anything as "disk IO overhead". The
    // example directory itself is normally on a real filesystem, which is
    // the point of comparing this number across host/VM/container.
    let dir = std::path::PathBuf::from(format!("overheadbench-io-{}", std::process::id()));
    fs::create_dir_all(&dir).map_err(|e| format!("mkdir: {e}"))?;
    let path = dir.join("probe");
    let payload = vec![b'x'; IO_FILE_BYTES];

    let cleanup = |p: &std::path::Path, d: &std::path::Path| {
        let _ = fs::remove_file(p);
        let _ = fs::remove_dir(d);
    };

    let start = Instant::now();
    for _ in 0..iters {
        let mut f = match OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&path)
        {
            Ok(f) => f,
            Err(e) => {
                cleanup(&path, &dir);
                return Err(format!("open: {e}"));
            }
        };
        if let Err(e) = f.write_all(&payload) {
            cleanup(&path, &dir);
            return Err(format!("write: {e}"));
        }
        if let Err(e) = f.sync_all() {
            cleanup(&path, &dir);
            return Err(format!("fsync: {e}"));
        }
        drop(f);
        if let Err(e) = fs::remove_file(&path) {
            cleanup(&path, &dir);
            return Err(format!("unlink: {e}"));
        }
    }
    let mut secs = start.elapsed().as_secs_f64();
    if secs <= 0.0 {
        secs = 1e-9;
    }
    let _ = fs::remove_dir(&dir);
    Ok(iters as f64 / secs)
}

struct Row {
    name: &'static str,
    unit: &'static str,
    value: f64,
}

fn run_benches(cfg: &Config) -> Result<Vec<Row>, String> {
    let want_syscall = cfg.bench == Bench::Syscall || cfg.bench == Bench::All;
    let want_mem = cfg.bench == Bench::Mem || cfg.bench == Bench::All;
    let want_io = cfg.bench == Bench::Io || cfg.bench == Bench::All;

    let mut rows = Vec::new();
    if want_syscall {
        let iters = cfg.iters.unwrap_or(SYSCALL_DEFAULT_ITERS);
        rows.push(Row {
            name: "syscall",
            unit: "ns/call",
            value: bench_syscall(iters),
        });
    }
    if want_mem {
        let passes = cfg.iters.unwrap_or(MEM_DEFAULT_PASSES);
        rows.push(Row {
            name: "mem",
            unit: "GB/s",
            value: bench_mem(passes),
        });
    }
    if want_io {
        let iters = cfg.iters.unwrap_or(IO_DEFAULT_ITERS);
        let value = bench_io(iters).map_err(|e| format!("io bench failed: {e}"))?;
        rows.push(Row {
            name: "io",
            unit: "ops/s",
            value,
        });
    }
    Ok(rows)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let cfg = match parse_args(&args) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("{e}");
            eprintln!("{USAGE}");
            std::process::exit(2);
        }
    };

    let rows = match run_benches(&cfg) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    };

    for row in &rows {
        println!(
            "bench={} metric={:.2} unit={}",
            row.name, row.value, row.unit
        );
    }
}
