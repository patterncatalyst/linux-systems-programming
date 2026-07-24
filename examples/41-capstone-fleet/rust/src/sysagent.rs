// sysagent.rs — /proc/stat, /proc/meminfo, /proc/loadavg readers and the
// sample loop, ported field-for-field from go/sysagent.go so the printed
// cpu_pct/mem_pct/load1 numbers are computed by the identical formula (not
// just "close enough"): idle is specifically the 4th space-separated value
// on /proc/stat's "cpu " line, and total is the sum of every value on that
// line including guest/guest_nice, exactly as the Go reference sums them.
use std::fs;
use std::sync::atomic::Ordering;
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crate::telemetry::{self, Telemetry};
use crate::util;

#[derive(Default, Clone, Copy)]
struct CpuTimes {
    idle: u64,
    total: u64,
}

fn read_cpu_times() -> Result<CpuTimes, String> {
    let text = fs::read_to_string("/proc/stat").map_err(|e| format!("open /proc/stat: {e}"))?;
    let line = text.lines().next().ok_or("empty /proc/stat")?;
    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 5 || fields[0] != "cpu" {
        return Err(format!("unexpected /proc/stat format: {line:?}"));
    }
    let mut total: u64 = 0;
    let mut idle: u64 = 0;
    for (i, s) in fields[1..].iter().enumerate() {
        let Ok(v) = s.parse::<u64>() else { continue };
        total += v;
        if i == 3 {
            // idle is the 4th value after "cpu"
            idle = v;
        }
    }
    Ok(CpuTimes { idle, total })
}

fn cpu_pct(prev: &CpuTimes, cur: &CpuTimes) -> f64 {
    let d_total = cur.total as f64 - prev.total as f64;
    let d_idle = cur.idle as f64 - prev.idle as f64;
    if d_total <= 0.0 {
        return 0.0;
    }
    (1.0 - d_idle / d_total) * 100.0
}

fn read_mem_pct() -> Result<f64, String> {
    let text =
        fs::read_to_string("/proc/meminfo").map_err(|e| format!("open /proc/meminfo: {e}"))?;
    let mut total = 0.0_f64;
    let mut avail = 0.0_f64;
    for line in text.lines() {
        let mut it = line.split_whitespace();
        let (Some(key), Some(val)) = (it.next(), it.next()) else {
            continue;
        };
        let Ok(v) = val.parse::<f64>() else { continue };
        match key.trim_end_matches(':') {
            "MemTotal" => total = v,
            "MemAvailable" => avail = v,
            _ => {}
        }
    }
    if total <= 0.0 {
        return Err("MemTotal not found".to_string());
    }
    Ok((1.0 - avail / total) * 100.0)
}

fn read_load1() -> Result<f64, String> {
    let text =
        fs::read_to_string("/proc/loadavg").map_err(|e| format!("open /proc/loadavg: {e}"))?;
    let field = text
        .split_whitespace()
        .next()
        .ok_or("empty /proc/loadavg")?;
    field
        .parse::<f64>()
        .map_err(|e| format!("parse load1: {e}"))
}

#[derive(Default)]
struct Sample {
    cpu_pct: f64,
    mem_pct: f64,
    load1: f64,
}

fn take_sample(prev: &mut CpuTimes) -> Result<Sample, String> {
    let cur = read_cpu_times()?;
    let mut s = Sample::default();
    if prev.total > 0 {
        s.cpu_pct = cpu_pct(prev, &cur);
    }
    *prev = cur;
    s.mem_pct = read_mem_pct()?;
    s.load1 = read_load1()?;
    Ok(s)
}

pub fn run(node: &str, interval_ms: u64, once: bool, tel: &Telemetry) -> i32 {
    let gauges = telemetry::register_sysagent_gauges(tel, node);
    let sig = util::install_signal_flag();

    // Prime the CPU-time baseline so the first printed sample isn't a bogus
    // 0 (matches Go: read once, sleep 200ms, THEN start the sample loop).
    let mut prev = read_cpu_times().unwrap_or_default();
    thread::sleep(Duration::from_millis(200));

    loop {
        let s = match take_sample(&mut prev) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("sysagent: sample: {e}");
                return 1;
            }
        };
        if tel.enabled {
            gauges.update(telemetry::GaugeSample {
                cpu_pct: s.cpu_pct,
                mem_pct: s.mem_pct,
                load1: s.load1,
            });
        }
        let ts = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        println!(
            "sysagent: node={node} cpu_pct={:.2} mem_pct={:.2} load1={:.2} ts={ts}",
            s.cpu_pct, s.mem_pct, s.load1
        );

        if once {
            break;
        }
        if sig.load(Ordering::Relaxed) {
            break;
        }
        thread::sleep(Duration::from_millis(interval_ms));
        if sig.load(Ordering::Relaxed) {
            break;
        }
    }
    if tel.enabled {
        telemetry::print_export_errors("sysagent");
    }
    0
}

// saturate — chaos helper: busy-spin threads (cpu) or hold allocated,
// touched memory (mem) for --seconds, so a concurrently-running sysagent's
// cpu_pct/mem_pct visibly rises (this fleet's USE-method callback).
pub fn saturate(resource: &str, seconds: i64, workers: i64, mb: i64) -> i32 {
    let seconds = seconds.max(0) as u64;
    match resource {
        "cpu" => {
            let workers = if workers <= 0 {
                std::thread::available_parallelism()
                    .map(|n| n.get() as i64)
                    .unwrap_or(1)
            } else {
                workers
            };
            println!("sysagent: saturate resource=cpu seconds={seconds} workers={workers} started");
            let stop = Instant::now() + Duration::from_secs(seconds);
            let mut handles = Vec::with_capacity(workers as usize);
            for _ in 0..workers {
                handles.push(thread::spawn(move || {
                    let mut x = 0.0001_f64;
                    while Instant::now() < stop {
                        x = x * 1.0000001 + 1.0;
                    }
                    if x == 0.0 {
                        // never true; keeps the compiler from eliding the loop
                        println!("{x}");
                    }
                }));
            }
            for h in handles {
                let _ = h.join();
            }
            println!("sysagent: saturate done");
            0
        }
        "mem" => {
            let mb = if mb <= 0 { 256 } else { mb };
            println!("sysagent: saturate resource=mem seconds={seconds} mb={mb} started");
            let n = mb as usize * 1024 * 1024;
            let mut buf = vec![0u8; n];
            for (i, b) in buf.iter_mut().enumerate() {
                *b = i as u8; // touch every page so it's resident, not just reserved
            }
            thread::sleep(Duration::from_secs(seconds));
            let _ = buf[0];
            println!("sysagent: saturate done");
            0
        }
        _ => {
            eprintln!(
                "usage: sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]"
            );
            2
        }
    }
}
