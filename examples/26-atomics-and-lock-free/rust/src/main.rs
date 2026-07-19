//! spscring: a single-producer/single-consumer lock-free ring buffer.
//!
//! ```text
//! spscring --capacity K --items N [--pad on|off]
//! ```
//!
//! A producer thread pushes N u64 values (0..N-1) through a bounded ring of K
//! slots; the consumer (main thread) pops and sums them. head and tail are
//! `AtomicU64` monotonic counters synchronised with explicit
//! `Ordering::{Acquire, Release}` (Lamport's SPSC queue) — no mutex, no lock.
//! The ring slots are themselves `AtomicU64` accessed `Relaxed`; the head/tail
//! acquire/release pair is what publishes each slot to the other thread, so the
//! data race is ruled out at the type level rather than by `unsafe`.
//!
//! `--pad` puts head and tail on separate cache lines to remove the false
//! sharing that otherwise ping-pongs one line between the two cores.
//!
//! Prints exactly one line:
//! `spscring: items=<N> sum=<s> throughput_mops=<m> pad=<on|off>`

use std::process::ExitCode;
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::Instant;

const USAGE: &str = "usage: spscring --capacity K --items N [--pad on|off]";

struct Args {
    capacity: u64,
    items: u64,
    pad: bool,
}

fn parse_args(argv: &[String]) -> Result<Args, ()> {
    let mut capacity: Option<u64> = None;
    let mut items: Option<u64> = None;
    let mut pad = false;

    let mut i = 0;
    while i < argv.len() {
        let flag = argv[i].as_str();
        let mut next = || -> Result<&String, ()> {
            i += 1;
            argv.get(i).ok_or(())
        };
        match flag {
            "--capacity" => {
                let n: u64 = next()?.parse().map_err(|_| ())?;
                if n == 0 {
                    return Err(());
                }
                capacity = Some(n);
            }
            "--items" => {
                let n: u64 = next()?.parse().map_err(|_| ())?;
                items = Some(n);
            }
            "--pad" => match next()?.as_str() {
                "on" => pad = true,
                "off" => pad = false,
                _ => return Err(()),
            },
            _ => return Err(()),
        }
        i += 1;
    }

    Ok(Args {
        capacity: capacity.ok_or(())?,
        items: items.ok_or(())?,
        pad,
    })
}

/// A cache-line-aligned atomic: when used for head and tail separately this
/// forces each onto its own 64-byte line, removing false sharing.
#[repr(align(64))]
struct CacheAligned(AtomicU64);

/// Padded layout: head and tail on separate cache lines.
struct CtrlPadded {
    head: CacheAligned,
    tail: CacheAligned,
}

/// Packed layout: head and tail adjacent (false sharing).
struct CtrlPacked {
    head: AtomicU64,
    tail: AtomicU64,
}

/// The SPSC benchmark, generic over the two index atomics. `head` is written by
/// the consumer and read by the producer; `tail` the reverse.
fn run(cap: u64, items: u64, head: &AtomicU64, tail: &AtomicU64) -> (u64, f64) {
    let buf: Vec<AtomicU64> = (0..cap).map(|_| AtomicU64::new(0)).collect();
    let buf = buf.as_slice();

    let start = Instant::now();

    let sum = thread::scope(|s| {
        // Producer: push 0..items-1.
        s.spawn(move || {
            let mut t: u64 = 0;
            for i in 0..items {
                while t - head.load(Ordering::Acquire) == cap {
                    std::hint::spin_loop();
                }
                buf[(t % cap) as usize].store(i, Ordering::Relaxed);
                t += 1;
                tail.store(t, Ordering::Release);
            }
        });

        // Consumer on this thread.
        let mut h: u64 = 0;
        let mut sum: u64 = 0;
        for _ in 0..items {
            while h == tail.load(Ordering::Acquire) {
                std::hint::spin_loop();
            }
            sum = sum.wrapping_add(buf[(h % cap) as usize].load(Ordering::Relaxed));
            h += 1;
            head.store(h, Ordering::Release);
        }
        sum
    });

    let mut secs = start.elapsed().as_secs_f64();
    if secs < 1e-9 {
        secs = 1e-9;
    }
    let mops = items as f64 / secs / 1e6;
    (sum, mops)
}

fn bench(cap: u64, items: u64, pad: bool) -> (u64, f64) {
    if pad {
        let ctrl = CtrlPadded {
            head: CacheAligned(AtomicU64::new(0)),
            tail: CacheAligned(AtomicU64::new(0)),
        };
        run(cap, items, &ctrl.head.0, &ctrl.tail.0)
    } else {
        let ctrl = CtrlPacked {
            head: AtomicU64::new(0),
            tail: AtomicU64::new(0),
        };
        run(cap, items, &ctrl.head, &ctrl.tail)
    }
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let args = match parse_args(&argv) {
        Ok(a) => a,
        Err(()) => {
            eprintln!("{USAGE}");
            return ExitCode::from(2);
        }
    };

    let (sum, mops) = bench(args.capacity, args.items, args.pad);
    let pad = if args.pad { "on" } else { "off" };
    println!(
        "spscring: items={} sum={} throughput_mops={:.2} pad={}",
        args.items, sum, mops, pad
    );
    ExitCode::SUCCESS
}
