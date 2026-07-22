// benchlab (Rust) — proper benchmarking of the chatterd frame codec (ch21).
//
// Usage:
//   app --op encode|decode|roundtrip --iters N [--warmup W]   # the real thing
//   app --lie [--op encode|decode|roundtrip]                  # the anti-pattern
//
// The "real thing" times each iteration individually with a monotonic clock,
// discards a warmup phase, and reports min/median/p99/max plus a
// coordinated-omission-corrected p99 (see co_correct below). `--lie` times a
// single unwarmed call with wall-clock and reports one number — exactly the
// benchmark the rest of this example exists to discredit.
//
// Wire format (canonical chatterd chat frame, introduced ch21):
//   [ magic 0x43 0x48 ][ version 0x01 ][ type u8 ][ length u16 be ][ payload ]
// This file re-implements only encode_frame/decode_frame — the codec under
// test — not the daemon; see ch21/22/27 for the networked version.

use std::process::ExitCode;
use std::time::Instant;

// ---------------------------------------------------------------------------
// Frame codec under test — canonical chatterd chat frame (see file header).
// ---------------------------------------------------------------------------
const HEADER_LEN: usize = 6;
const MAGIC0: u8 = 0x43;
const MAGIC1: u8 = 0x48;
const VERSION: u8 = 0x01;
const TYPE_DELIVER: u8 = 3;

fn encode_frame(typ: u8, body: &[u8]) -> Vec<u8> {
    let len = body.len() as u16;
    let mut out = Vec::with_capacity(HEADER_LEN + body.len());
    out.push(MAGIC0);
    out.push(MAGIC1);
    out.push(VERSION);
    out.push(typ);
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(body);
    out
}

// Decodes exactly one frame that fills `buf` completely — no partial-read
// reassembly here, that lives in the networked chatterd (ch21/22/27); this
// harness only measures the codec's per-call cost.
fn decode_frame(buf: &[u8]) -> Result<(u8, Vec<u8>), String> {
    if buf.len() < HEADER_LEN {
        return Err("frame shorter than header".to_string());
    }
    if buf[0] != MAGIC0 || buf[1] != MAGIC1 {
        return Err("bad magic".to_string());
    }
    if buf[2] != VERSION {
        return Err("bad version".to_string());
    }
    let typ = buf[3];
    let len = u16::from_be_bytes([buf[4], buf[5]]) as usize;
    if buf.len() != HEADER_LEN + len {
        return Err("length mismatch".to_string());
    }
    Ok((typ, buf[HEADER_LEN..].to_vec()))
}

// ---------------------------------------------------------------------------
// The fixed workload: a DELIVER frame carrying a realistic chat line. Every
// language in this example encodes/decodes the identical bytes.
// ---------------------------------------------------------------------------
const NICK: &str = "alice";
const TEXT: &str = "the quick brown fox jumps over the lazy dog, three times, for benchlab";

fn delivery_body() -> Vec<u8> {
    let mut body = Vec::with_capacity(NICK.len() + 1 + TEXT.len());
    body.extend_from_slice(NICK.as_bytes());
    body.push(0);
    body.extend_from_slice(TEXT.as_bytes());
    body
}

#[derive(Clone, Copy)]
enum Op {
    Encode,
    Decode,
    Roundtrip,
}

impl Op {
    fn parse(s: &str) -> Result<Op, String> {
        match s {
            "encode" => Ok(Op::Encode),
            "decode" => Ok(Op::Decode),
            "roundtrip" => Ok(Op::Roundtrip),
            other => Err(format!("unknown --op '{other}'")),
        }
    }

    fn name(self) -> &'static str {
        match self {
            Op::Encode => "encode",
            Op::Decode => "decode",
            Op::Roundtrip => "roundtrip",
        }
    }
}

// One iteration of the requested op against the fixed workload. Returns a
// small scalar derived from the result so the caller can fold it into a
// checksum — this is what keeps the optimizer from proving the call's result
// is unused and deleting it. Propagates a codec bug (should never trigger on
// this fixed workload) via `?` up to main.
fn run_once(op: Op, body: &[u8], prebuilt_frame: &[u8]) -> Result<u64, String> {
    match op {
        Op::Encode => {
            let frame = encode_frame(TYPE_DELIVER, body);
            Ok(*frame.last().unwrap_or(&0) as u64)
        }
        Op::Decode => {
            let (_typ, payload) = decode_frame(prebuilt_frame)?;
            Ok(payload.len() as u64)
        }
        Op::Roundtrip => {
            let frame = encode_frame(TYPE_DELIVER, body);
            let (_typ, payload) = decode_frame(&frame)?;
            Ok(payload.len() as u64)
        }
    }
}

fn percentile(sorted: &[i64], p: f64) -> i64 {
    if sorted.is_empty() {
        return 0;
    }
    let idx = (p * (sorted.len() - 1) as f64) as usize;
    sorted[idx]
}

// Coordinated-omission correction (HdrHistogram's recordValueWithExpectedInterval):
// this harness is a closed loop — iteration N+1 never starts until N returns —
// so a stall (a page fault, a context switch, a scheduling hiccup) is
// recorded as a single slow sample. A fixed-rate caller would instead have
// had many requests queue up behind that stall, each experiencing a similar
// multiple-of-the-target-interval delay. This backfills those missing
// virtual samples so the tail reflects what a real caller would have seen,
// not just what one lucky/unlucky iteration measured.
fn co_correct(raw: &[i64], expected_interval_ns: i64, cap: usize) -> Vec<i64> {
    let mut out = Vec::with_capacity(raw.len());
    'outer: for &v in raw {
        out.push(v);
        if expected_interval_ns <= 0 || v <= expected_interval_ns {
            continue;
        }
        let mut missing = v - expected_interval_ns;
        while missing >= expected_interval_ns {
            if out.len() >= cap {
                break 'outer;
            }
            out.push(missing);
            missing -= expected_interval_ns;
        }
    }
    out
}

fn usage() -> ! {
    eprintln!("usage: benchlab --op encode|decode|roundtrip --iters N [--warmup W]");
    eprintln!("       benchlab --lie [--op encode|decode|roundtrip]");
    std::process::exit(2);
}

fn parse_u64(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse::<i64>().ok()
}

struct Args {
    lie: bool,
    op: Option<Op>,
    iters: i64,
    warmup: i64,
}

fn parse_args() -> Args {
    let argv: Vec<String> = std::env::args().collect();
    let mut lie = false;
    let mut op_str: Option<String> = None;
    let mut iters: i64 = -1;
    let mut warmup: i64 = 1000;

    let mut i = 1;
    while i < argv.len() {
        match argv[i].as_str() {
            "--lie" => lie = true,
            "--op" if i + 1 < argv.len() => {
                op_str = Some(argv[i + 1].clone());
                i += 1;
            }
            "--iters" if i + 1 < argv.len() => {
                iters = parse_u64(&argv[i + 1]).unwrap_or_else(|| usage());
                i += 1;
            }
            "--warmup" if i + 1 < argv.len() => {
                warmup = parse_u64(&argv[i + 1]).unwrap_or_else(|| usage());
                i += 1;
            }
            _ => usage(),
        }
        i += 1;
    }

    if !lie && op_str.is_none() {
        usage();
    }
    let op = op_str.map(|s| Op::parse(&s).unwrap_or_else(|_| usage()));

    Args { lie, op, iters, warmup }
}

fn main() -> ExitCode {
    let args = parse_args();
    let op = args.op.unwrap_or(Op::Roundtrip);
    let body = delivery_body();
    let prebuilt = encode_frame(TYPE_DELIVER, &body);

    if args.lie {
        // The anti-pattern: no warmup, one call, wall-clock, done. This is
        // exactly the benchmark this example's README argues never to trust.
        let t0 = Instant::now();
        let sink = match run_once(op, &body, &prebuilt) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("benchlab: codec bug: {e}");
                return ExitCode::from(1);
            }
        };
        let elapsed_ns = t0.elapsed().as_nanos();
        println!(
            "benchlab: lie op={} (no warmup, single wall-clock sample, ignores variance)",
            op.name()
        );
        println!("benchlab: lie elapsed_ns={elapsed_ns} sink={sink}");
        return ExitCode::SUCCESS;
    }

    if args.iters < 2 || args.warmup < 0 || args.iters <= args.warmup {
        usage();
    }
    let iters = args.iters;
    let warmup = args.warmup;

    // Warmup: run the op without timing it, letting allocators/caches/branch
    // predictors reach steady state before any sample is recorded.
    let mut checksum: u64 = 0;
    for _ in 0..warmup {
        match run_once(op, &body, &prebuilt) {
            Ok(v) => checksum = checksum.wrapping_add(v),
            Err(e) => {
                eprintln!("benchlab: codec bug: {e}");
                return ExitCode::from(1);
            }
        }
    }

    let mut raw: Vec<i64> = Vec::with_capacity(iters as usize);
    for _ in 0..iters {
        let t0 = Instant::now();
        let r = run_once(op, &body, &prebuilt);
        let elapsed_ns = t0.elapsed().as_nanos() as i64;
        match r {
            Ok(v) => checksum = checksum.wrapping_add(v),
            Err(e) => {
                eprintln!("benchlab: codec bug: {e}");
                return ExitCode::from(1);
            }
        }
        raw.push(elapsed_ns);
    }

    let mut sorted = raw.clone();
    sorted.sort_unstable();
    let min_ns = sorted[0];
    let median_ns = percentile(&sorted, 0.5);
    let p99_ns = percentile(&sorted, 0.99);
    let max_ns = *sorted.last().unwrap();

    let expected_interval_ns = if median_ns > 0 { median_ns } else { 1 };
    const CO_CAP: usize = 5_000_000;
    let mut corrected = co_correct(&raw, expected_interval_ns, CO_CAP);
    corrected.sort_unstable();
    let co_p99_ns = percentile(&corrected, 0.99);

    println!("benchlab: op={} iters={} warmup={}", op.name(), iters, warmup);
    println!(
        "benchlab: n={} min_ns={} median_ns={} p99_ns={} max_ns={}",
        sorted.len(),
        min_ns,
        median_ns,
        p99_ns,
        max_ns
    );
    println!(
        "benchlab: co_p99_ns={} expected_interval_ns={} co_n={}",
        co_p99_ns,
        expected_interval_ns,
        corrected.len()
    );
    println!("benchlab: checksum={checksum:016x}");
    ExitCode::SUCCESS
}
