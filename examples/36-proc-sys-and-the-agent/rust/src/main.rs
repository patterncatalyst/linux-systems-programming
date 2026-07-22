// sysagent v0 (chapter 36: /proc, /sys, and the agent) — a USE-method
// metrics collector reading /proc + /sys/fs/cgroup, no root required.
//
//   sysagent sample [--json] [--interval-ms N]
//   sysagent serve  --port P [--interval-ms N]
//
// `sample` takes one snapshot: CPU utilization from a /proc/stat delta
// spanning --interval-ms (default 200), run-queue + load from
// /proc/loadavg, memory from /proc/meminfo, per-disk I/O from
// /proc/diskstats, network rx/tx from /proc/net/dev, and cgroup PSI
// (falling back to system-wide /proc/pressure) if the kernel exposes it.
// `serve` exposes the identical snapshot as JSON over a hand-rolled
// HTTP/1.1 /metrics endpoint, one fresh sample per request.
//
// The field names in both the --json and /metrics output are the
// deterministic, cross-language schema documented in README.md — C++ and
// Go sysagent emit byte-for-byte the same keys.
mod httpd;
mod procfs;

use std::process::ExitCode;

fn usage() {
    eprintln!(
        "usage: sysagent sample [--json] [--interval-ms N] | serve --port P [--interval-ms N]"
    );
}

/// Minimal hand-rolled flag scan, kept in the same shape as the C++ and Go
/// argument handling so the three stay directly comparable.
struct Flags {
    json: bool,
    interval_ms: u64,
    port: i64,
}

fn parse_flags(args: &[String]) -> Option<Flags> {
    let mut f = Flags {
        json: false,
        interval_ms: 200,
        port: -1,
    };
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => f.json = true,
            "--interval-ms" => {
                i += 1;
                f.interval_ms = args.get(i)?.parse().ok()?;
            }
            "--port" => {
                i += 1;
                f.port = args.get(i)?.parse().ok()?;
            }
            _ => return None,
        }
        i += 1;
    }
    Some(f)
}

fn cmd_sample(args: &[String]) -> u8 {
    let Some(f) = parse_flags(args).filter(|f| f.interval_ms > 0) else {
        usage();
        return 2;
    };
    match procfs::take_snapshot(f.interval_ms) {
        Ok(snap) => {
            if f.json {
                println!("{}", procfs::to_json(&snap));
            } else {
                print!("{}", procfs::to_text(&snap));
            }
            0
        }
        Err(e) => {
            eprintln!("sysagent: error: {e}");
            1
        }
    }
}

fn cmd_serve(args: &[String]) -> u8 {
    let Some(f) = parse_flags(args).filter(|f| f.port > 0 && f.interval_ms > 0) else {
        usage();
        return 2;
    };
    httpd::serve(f.port as u16, f.interval_ms) as u8
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
        return ExitCode::from(2);
    }
    let code = match args[1].as_str() {
        "sample" => cmd_sample(&args[2..]),
        "serve" => cmd_serve(&args[2..]),
        _ => {
            usage();
            2
        }
    };
    ExitCode::from(code)
}
