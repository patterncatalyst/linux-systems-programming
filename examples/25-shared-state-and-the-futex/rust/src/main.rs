// workq — a bounded MPMC job queue and worker pool (Rust 2024).
//
// P producers push N items total into a bounded blocking queue built from a
// Mutex<VecDeque> plus two Condvars — the futex-backed pair this chapter is
// about. C consumers pop items and fold each payload into a checksum.
//
// The correct path gives every consumer its own accumulator and combines them
// after the scoped threads join; nothing shared is written without the lock.
// --buggy is the deliberate counterexample: safe Rust will NOT let two threads
// write one plain counter, so the buggy path reaches for `unsafe` and a raw
// pointer to force the data race — and then prints the corrupted result that
// the borrow checker exists to prevent. (No sanitizer needed to see it fail;
// run it a few times and consumed/checksum wander.)
//
// Payloads are a pure function of index folded with XOR, so the correct
// checksum is deterministic for a given (seed, N) regardless of P and C — and
// byte-identical to the C++ and Go builds.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Condvar, Mutex};
use std::thread;
use std::time::Instant;

const GOLDEN: u64 = 0x9E3779B97F4A7C15;
const MIX1: u64 = 0xBF58476D1CE4E5B9;
const MIX2: u64 = 0x94D049BB133111EB;
const SEED_DEFAULT: u64 = 0x0123456789ABCDEF;
const USAGE: &str =
    "usage: workq --producers P --consumers C --items N [--buggy] [--seed S] [--cap K]";

/// splitmix64 finalizer over seed + (i+1)*golden, wrapping u64 throughout.
fn payload(seed: u64, i: u64) -> u64 {
    let mut x = seed.wrapping_add(i.wrapping_add(1).wrapping_mul(GOLDEN));
    x = (x ^ (x >> 30)).wrapping_mul(MIX1);
    x = (x ^ (x >> 27)).wrapping_mul(MIX2);
    x ^ (x >> 31)
}

struct Config {
    producers: i64,
    consumers: i64,
    items: i64,
    cap: usize,
    seed: u64,
    buggy: bool,
}

fn parse_seed(s: &str) -> Option<u64> {
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).ok()
    } else {
        s.parse::<u64>().ok()
    }
}

/// Ok(cfg) or Err(msg): Err(None) => usage only, Err(Some(m)) => "workq: m" + usage.
fn parse(args: &[String]) -> Result<Config, Option<String>> {
    let (mut producers, mut consumers, mut items) = (None, None, None);
    let mut cap: i64 = 256;
    let mut seed = SEED_DEFAULT;
    let mut buggy = false;

    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--buggy" => buggy = true,
            "--producers" | "--consumers" | "--items" | "--cap" => {
                let v = it.next().ok_or_else(|| Some(format!("{a} needs a value")))?;
                let n: i64 = v
                    .parse()
                    .map_err(|_| Some(format!("not an integer: {v}")))?;
                match a.as_str() {
                    "--producers" => producers = Some(n),
                    "--consumers" => consumers = Some(n),
                    "--items" => items = Some(n),
                    _ => cap = n,
                }
            }
            "--seed" => {
                let v = it.next().ok_or_else(|| Some(format!("{a} needs a value")))?;
                seed = parse_seed(v).ok_or_else(|| Some(format!("not an integer: {v}")))?;
            }
            other => return Err(Some(format!("unknown flag: {other}"))),
        }
    }

    let (producers, consumers, items) = match (producers, consumers, items) {
        (Some(p), Some(c), Some(n)) => (p, c, n),
        _ => return Err(None),
    };
    if producers < 1 || consumers < 1 {
        return Err(Some("--producers and --consumers must be >= 1".to_string()));
    }
    if items < 0 {
        return Err(Some("--items must be >= 0".to_string()));
    }
    if cap < 1 {
        return Err(Some("--cap must be >= 1".to_string()));
    }
    Ok(Config {
        producers,
        consumers,
        items,
        cap: cap as usize,
        seed,
        buggy,
    })
}

struct Inner {
    q: VecDeque<u64>,
    closed: bool,
}

/// A bounded blocking MPMC queue: one Mutex, two Condvars. Producers block on
/// `not_full`, consumers block on `not_empty`; `close` wakes drained consumers.
struct BoundedQueue {
    inner: Mutex<Inner>,
    not_full: Condvar,
    not_empty: Condvar,
    cap: usize,
}

impl BoundedQueue {
    fn new(cap: usize) -> Self {
        BoundedQueue {
            inner: Mutex::new(Inner {
                q: VecDeque::new(),
                closed: false,
            }),
            not_full: Condvar::new(),
            not_empty: Condvar::new(),
            cap,
        }
    }

    fn push(&self, v: u64) {
        let mut g = self.inner.lock().unwrap();
        while g.q.len() >= self.cap {
            g = self.not_full.wait(g).unwrap();
        }
        g.q.push_back(v);
        drop(g);
        self.not_empty.notify_one();
    }

    /// Pop one item; returns None once the queue is closed and drained.
    fn pop(&self) -> Option<u64> {
        let mut g = self.inner.lock().unwrap();
        loop {
            if let Some(v) = g.q.pop_front() {
                drop(g);
                self.not_full.notify_one();
                return Some(v);
            }
            if g.closed {
                return None;
            }
            g = self.not_empty.wait(g).unwrap();
        }
    }

    fn close(&self) {
        let mut g = self.inner.lock().unwrap();
        g.closed = true;
        drop(g);
        self.not_empty.notify_all();
    }
}

/// A raw pointer the buggy path deliberately shares across threads. Safe Rust
/// forbids this; the `unsafe impl`s are what it takes to opt out of the very
/// protection this chapter demonstrates.
#[derive(Clone, Copy)]
struct Racy<T>(*mut T);
unsafe impl<T> Send for Racy<T> {}
unsafe impl<T> Sync for Racy<T> {}

fn produce(queue: &BoundedQueue, produced: &AtomicI64, cfg: &Config, p: i64) {
    let mut i = p;
    while i < cfg.items {
        queue.push(payload(cfg.seed, i as u64));
        produced.fetch_add(1, Ordering::Relaxed);
        i += cfg.producers;
    }
}

fn run(cfg: &Config) {
    let queue = BoundedQueue::new(cfg.cap);
    let produced = AtomicI64::new(0);
    let mut shared_consumed: i64 = 0;
    let mut shared_checksum: u64 = 0;
    let start = Instant::now();

    let queue = &queue;
    let produced = &produced;

    let (consumed, checksum) = if cfg.buggy {
        let cptr = Racy(&mut shared_consumed as *mut i64);
        let kptr = Racy(&mut shared_checksum as *mut u64);
        thread::scope(|s| {
            let mut prod = Vec::new();
            for p in 0..cfg.producers {
                prod.push(s.spawn(move || produce(queue, produced, cfg, p)));
            }
            let mut cons = Vec::new();
            for _ in 0..cfg.consumers {
                cons.push(s.spawn(move || {
                    // Bind the whole wrappers so the closure captures the
                    // `Send` `Racy`, not the bare `*mut` field (2024 captures
                    // disjoint fields otherwise).
                    let (cptr, kptr) = (cptr, kptr);
                    while let Some(v) = queue.pop() {
                        // DATA RACE: two consumers write these with no lock.
                        unsafe {
                            *cptr.0 += 1;
                            *kptr.0 ^= v;
                        }
                    }
                }));
            }
            for h in prod {
                h.join().unwrap();
            }
            queue.close();
            for h in cons {
                h.join().unwrap();
            }
        });
        (shared_consumed, shared_checksum)
    } else {
        thread::scope(|s| {
            let mut prod = Vec::new();
            for p in 0..cfg.producers {
                prod.push(s.spawn(move || produce(queue, produced, cfg, p)));
            }
            let mut cons = Vec::new();
            for _ in 0..cfg.consumers {
                cons.push(s.spawn(move || {
                    let mut count: i64 = 0;
                    let mut sum: u64 = 0;
                    while let Some(v) = queue.pop() {
                        count += 1;
                        sum ^= v;
                    }
                    (count, sum)
                }));
            }
            for h in prod {
                h.join().unwrap();
            }
            queue.close();
            let mut consumed = 0i64;
            let mut checksum = 0u64;
            for h in cons {
                let (c, k) = h.join().unwrap();
                consumed += c;
                checksum ^= k;
            }
            (consumed, checksum)
        })
    };

    let ms = start.elapsed().as_millis();
    println!(
        "workq: produced={} consumed={} checksum={:016x} ms={}",
        produced.load(Ordering::Relaxed),
        consumed,
        checksum,
        ms
    );
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match parse(&args) {
        Ok(cfg) => run(&cfg),
        Err(msg) => {
            if let Some(m) = msg {
                eprintln!("workq: {m}");
            }
            eprintln!("{USAGE}");
            std::process::exit(2);
        }
    }
}
