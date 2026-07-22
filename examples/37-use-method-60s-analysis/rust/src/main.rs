// loadmix (chapter 37): a saturation generator + guided-analysis harness for
// the classic Brendan Gregg 60-second USE-method checklist.
//
//   app --resource cpu|mem|io|net --seconds N   saturate that resource
//   app analyze [--seconds N]                    run the checklist and name
//                                                 the saturated resource
//
// `analyze` shells out to the real system tools (uptime, dmesg, vmstat,
// mpstat, pidstat, iostat, free, sar, ss, top) exactly as a human running the
// checklist would, parses their plain-text tables generically (by column
// *name*, read from each tool's own header row, never a hardcoded position),
// and for each of the four resources reports which USE signals -- Utilization
// and Saturation -- fired against a fixed threshold. Errors is reported too,
// fixed false: this chapter induces load, not kernel-logged faults, so a
// real checklist run finds none.
//
// Rust idioms used throughout, in place of the C++ reference's jthread pools,
// std::atomic counters, and std::expected: std::thread::scope (the jthread
// analogue -- worker threads are RAII-joined when the scope block ends),
// AtomicU64 counters shared across them by reference, std::process::Command
// for every subprocess capture and spawn, and OpenOptionsExt::custom_flags
// plus FileExt::write_at for the O_DSYNC pwrite path. Every observable line
// below is a byte-for-byte match of the C++ std::println output (field
// names, order, and spacing); only floating-point rounding may differ, which
// is why verify.lua checks shapes (%d+%.%d%d) rather than exact decimals.

use std::fs::{self, File, OpenOptions};
use std::io;
use std::net::UdpSocket;
use std::os::unix::fs::{FileExt, OpenOptionsExt};
use std::path::Path;
use std::process::{Child, Command};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

// ------------------------------------------------------------- utilities --

fn nproc() -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
}

fn trim(s: &str) -> String {
    s.trim_matches(|c: char| c == ' ' || c == '\t' || c == '\r' || c == '\n')
        .to_string()
}

fn split_ws(line: &str) -> Vec<String> {
    line.split_whitespace().map(String::from).collect()
}

// Mirrors std::getline over an istringstream: split on '\n', and drop the
// final empty element produced when the text ends with a trailing newline
// (getline never yields that last empty read).
fn split_lines(text: &str) -> Vec<String> {
    if text.is_empty() {
        return Vec::new();
    }
    let mut parts: Vec<String> = text.split('\n').map(String::from).collect();
    if parts.last().is_some_and(|s| s.is_empty()) {
        parts.pop();
    }
    parts
}

// A parsed table row: column names (from a tool's own header line) zipped
// with this row's values, so every parser below reads fields *by name*
// rather than by a hardcoded position -- robust to sysstat/procps column
// reordering across versions.
struct Row {
    names: Vec<String>,
    values: Vec<String>,
}

impl Row {
    fn get(&self, name: &str) -> Option<f64> {
        let n = self.names.len().min(self.values.len());
        for i in 0..n {
            if self.names[i] == name {
                return self.values[i].parse::<f64>().ok();
            }
        }
        None
    }
}

fn make_row(
    header_tokens: &[String],
    header_skip: usize,
    data_tokens: &[String],
    data_skip: usize,
) -> Row {
    let names = header_tokens.get(header_skip..).unwrap_or(&[]).to_vec();
    let values = data_tokens.get(data_skip..).unwrap_or(&[]).to_vec();
    Row { names, values }
}

// Run a short command to completion and capture combined stdout+stderr.
// Forced to the C locale so decimal points/dates are unambiguous regardless
// of the guest's configured locale. The one fallible operation in this file
// that returns a Result, per book convention (the C++ reference uses
// std::expected here).
fn run_capture(cmd: &str) -> io::Result<String> {
    // Grouped in a subshell so a single trailing "2>&1" captures every stage
    // of a pipeline (e.g. "dmesg | tail -n 5") -- without the parens, 2>&1
    // binds only to the pipeline's last stage and an earlier stage's stderr
    // (e.g. dmesg's own permission-denied message) leaks past the capture.
    let full = format!("( env LC_ALL=C LANG=C {cmd} ) 2>&1");
    let output = Command::new("sh").arg("-c").arg(full).output()?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

// Start `env LC_ALL=C LANG=C <argv...>` in the background, stdout+stderr
// redirected to outfile; returns the child (or None on spawn failure). Used
// for the five interval samplers so they run *concurrently* -- a 12s analyze
// window costs ~12s wall-clock, not 5x that from running them one at a time.
fn spawn_to_file(argv: &[&str], outfile: &Path) -> Option<Child> {
    let out = File::create(outfile).ok()?;
    let err = out.try_clone().ok()?;
    Command::new("env")
        .arg("LC_ALL=C")
        .arg("LANG=C")
        .args(argv)
        .stdout(out)
        .stderr(err)
        .spawn()
        .ok()
}

fn wait_for(child: Option<Child>) {
    if let Some(mut c) = child {
        let _ = c.wait();
    }
}

fn read_all(path: &str) -> String {
    fs::read_to_string(path).unwrap_or_default()
}

// ------------------------------------------------------- checklist parsers --
// Each parser reads a header line's column names from the tool's own output
// and zips subsequent data lines against that name list -- see Row::get
// above.

#[derive(Default)]
struct VmstatResult {
    run_queue_max: f64,
    swap_io_max: f64, // si+so, KB/s
}

// vmstat has no aggregate "Average:" line (unlike mpstat/pidstat), and its
// *first* sample is a since-boot average, not a live interval -- both quirks
// are handled here: we scan every data row after the header, skip row 0, and
// take the max across the rest (the steady-state saturation signal, not
// diluted by an average across the whole window).
fn parse_vmstat(text: &str) -> VmstatResult {
    let mut res = VmstatResult::default();
    let mut header: Vec<String> = Vec::new();
    let mut data_rows: Vec<Vec<String>> = Vec::new();
    for line in split_lines(text) {
        let toks = split_ws(&line);
        if toks.is_empty() {
            continue;
        }
        if header.is_empty() {
            if toks.iter().any(|t| t == "swpd") {
                header = toks;
            }
            continue;
        }
        if toks.len() + 2 >= header.len() {
            data_rows.push(toks);
        }
    }
    for row in data_rows.iter().skip(1) {
        // skip row 0 (since-boot average)
        let r = make_row(&header, 0, row, 0);
        res.run_queue_max = res.run_queue_max.max(r.get("r").unwrap_or(0.0));
        let si = r.get("si").unwrap_or(0.0);
        let so = r.get("so").unwrap_or(0.0);
        res.swap_io_max = res.swap_io_max.max(si + so);
    }
    res
}

#[derive(Default)]
struct MpstatResult {
    busy_pct: f64,
    iowait_pct: f64,
}

// mpstat -P ALL's own "Average:" row over the whole window (excludes the
// since-boot first report automatically -- sysstat's job, not ours).
fn parse_mpstat(text: &str) -> MpstatResult {
    let mut res = MpstatResult::default();
    let mut header: Vec<String> = Vec::new();
    let mut data: Vec<String> = Vec::new();
    for line in split_lines(text) {
        let toks = split_ws(&line);
        if toks.len() < 2 {
            continue;
        }
        if toks[0] == "Average:" && toks[1] == "CPU" {
            header = toks;
        } else if toks[0] == "Average:" && toks[1] == "all" {
            data = toks;
        }
    }
    if header.is_empty() || data.is_empty() {
        return res;
    }
    let r = make_row(&header, 2, &data, 2);
    let idle = r.get("%idle").unwrap_or(100.0);
    res.iowait_pct = r.get("%iowait").unwrap_or(0.0);
    res.busy_pct = 100.0 - idle;
    res
}

fn parse_free_used_pct(text: &str) -> f64 {
    let mut header: Vec<String> = Vec::new();
    let mut data: Vec<String> = Vec::new();
    for line in split_lines(text) {
        let toks = split_ws(&line);
        if toks.is_empty() {
            continue;
        }
        if toks.iter().any(|t| t == "available") {
            header = toks;
        } else if toks[0] == "Mem:" {
            data = toks;
        }
    }
    if header.is_empty() || data.is_empty() {
        return 0.0;
    }
    let r = make_row(&header, 0, &data, 1);
    let total = r.get("total").unwrap_or(0.0);
    let avail = r.get("available").unwrap_or(0.0);
    if total <= 0.0 {
        return 0.0;
    }
    (total - avail) / total * 100.0
}

#[derive(Default)]
struct IostatResult {
    util_max: f64,
    await_max: f64, // w_await, ms
}

// iostat -xz has no Average line either, AND -z hides idle devices entirely
// (they just don't appear in a quiet interval) -- so we scan every per-second
// block, skip block 0 (since-boot), skip loopback/removable/compressed
// pseudo-devices, and take the max over the guest's real block device.
fn parse_iostat(text: &str) -> IostatResult {
    let mut res = IostatResult::default();
    const EXCL: [&str; 4] = ["zram", "sr", "loop", "dm-"];
    let mut block_idx: i32 = -1;
    let mut header: Vec<String> = Vec::new();
    for line in split_lines(text) {
        let toks = split_ws(&line);
        if toks.is_empty() {
            continue;
        }
        if toks[0] == "avg-cpu:" {
            block_idx += 1;
            continue;
        }
        if toks[0] == "Device" {
            header = toks;
            continue;
        }
        if header.is_empty() || block_idx < 1 {
            continue; // no header yet, or since-boot block
        }
        let dev = &toks[0];
        if EXCL.iter().any(|&p| dev.starts_with(p)) {
            continue;
        }
        let r = make_row(&header, 1, &toks, 1);
        res.util_max = res.util_max.max(r.get("%util").unwrap_or(0.0));
        res.await_max = res.await_max.max(r.get("w_await").unwrap_or(0.0));
    }
    res
}

fn parse_sar_pkts(text: &str, iface: &str) -> f64 {
    let mut header: Vec<String> = Vec::new();
    let mut data: Vec<String> = Vec::new();
    for line in split_lines(text) {
        let toks = split_ws(&line);
        if toks.len() < 2 {
            continue;
        }
        if toks[0] == "Average:" && toks[1] == "IFACE" {
            header = toks;
        } else if toks[0] == "Average:" && toks[1] == iface {
            data = toks;
        }
    }
    if header.is_empty() || data.is_empty() {
        return 0.0;
    }
    let r = make_row(&header, 2, &data, 2);
    r.get("rxpck/s").unwrap_or(0.0) + r.get("txpck/s").unwrap_or(0.0)
}

// ---------------------------------------------------------------- saturate --

fn sat_cpu(seconds: u64, workers: usize) -> (u64, u64) {
    let deadline = Instant::now() + Duration::from_secs(seconds);
    let total_iters = AtomicU64::new(0);
    let checksum = AtomicU64::new(0);
    std::thread::scope(|s| {
        for w in 0..workers {
            let total_iters = &total_iters;
            let checksum = &checksum;
            s.spawn(move || {
                let mut x: u64 = 0x9E3779B97F4A7C15u64 ^ (w as u64 + 1);
                let mut iters: u64 = 0;
                loop {
                    x ^= x << 7;
                    x ^= x >> 9;
                    iters += 1;
                    if iters & 0xFFFFF == 0 && Instant::now() >= deadline {
                        break;
                    }
                }
                total_iters.fetch_add(iters, Ordering::Relaxed);
                checksum.fetch_add(x, Ordering::Relaxed); // keeps x provably live (no DCE)
            });
        }
    }); // worker threads join here -- thread::scope is the jthread analogue
    (
        total_iters.load(Ordering::Relaxed),
        checksum.load(Ordering::Relaxed),
    )
}

fn sat_mem(seconds: u64, workers: usize) -> (u64, u64) {
    let mut mem_kb: u64 = 0;
    if let Ok(text) = fs::read_to_string("/proc/meminfo") {
        for line in text.lines() {
            if let Some(rest) = line.strip_prefix("MemTotal:") {
                if let Some(tok) = rest.split_whitespace().next() {
                    mem_kb = tok.parse().unwrap_or(0);
                }
                break;
            }
        }
    }
    let target_bytes: u64 = if mem_kb > 0 {
        (mem_kb as f64 * 1024.0 * 1.35) as u64
    } else {
        2u64 << 30 // 2 GiB fallback if /proc/meminfo is unreadable
    };
    let chunk: u64 = (target_bytes / workers as u64).max(4096 * 16);

    let deadline = Instant::now() + Duration::from_secs(seconds);
    let total_touches = AtomicU64::new(0);
    std::thread::scope(|s| {
        for _ in 0..workers {
            let total_touches = &total_touches;
            s.spawn(move || {
                let mut buf = vec![0u8; chunk as usize];
                let mut touches: u64 = 0;
                while Instant::now() < deadline {
                    let mut off = 0usize;
                    while off < buf.len() {
                        buf[off] = buf[off].wrapping_add(1);
                        touches += 1;
                        off += 4096;
                    }
                }
                total_touches.fetch_add(touches, Ordering::Relaxed);
            });
        }
    });
    (total_touches.load(Ordering::Relaxed), target_bytes)
}

fn sat_io(seconds: u64, workers: usize) -> (u64, u64) {
    let dir = format!("/var/tmp/loadmix-io-{}", std::process::id());
    let _ = fs::create_dir(&dir);

    let deadline = Instant::now() + Duration::from_secs(seconds);
    let total_writes = AtomicU64::new(0);
    let total_bytes = AtomicU64::new(0);
    std::thread::scope(|s| {
        for w in 0..workers {
            let path = format!("{dir}/w{w}.dat");
            let total_writes = &total_writes;
            let total_bytes = &total_bytes;
            s.spawn(move || {
                let file = match OpenOptions::new()
                    .write(true)
                    .create(true)
                    .truncate(true)
                    .mode(0o600)
                    .custom_flags(libc::O_DSYNC)
                    .open(&path)
                {
                    Ok(f) => f,
                    Err(_) => return,
                };
                const K_BUF: usize = 16384;
                let mut buf = [0u8; K_BUF];
                // xorshift64*: cheap per-thread PRNG so the payload isn't
                // trivially compressible (this guest's /var is btrfs with
                // zstd:1 compression, which would otherwise let a real disk
                // write shrink to nearly nothing and mute the iowait signal).
                let mut seed: u64 = 0x2545F4914F6CDD1Du64 ^ (w as u64 + 1);
                let mut writes: u64 = 0;
                let mut bytes: u64 = 0;
                while Instant::now() < deadline {
                    let mut i = 0usize;
                    while i < K_BUF {
                        seed ^= seed << 13;
                        seed ^= seed >> 7;
                        seed ^= seed << 17;
                        buf[i..i + 8].copy_from_slice(&seed.to_le_bytes());
                        i += 8;
                    }
                    if let Ok(n) = file.write_at(&buf, 0) {
                        // same 16 KiB region: bounded file size
                        if n > 0 {
                            writes += 1;
                            bytes += n as u64;
                        }
                    }
                }
                drop(file);
                let _ = fs::remove_file(&path);
                total_writes.fetch_add(writes, Ordering::Relaxed);
                total_bytes.fetch_add(bytes, Ordering::Relaxed);
            });
        }
    });
    let _ = fs::remove_dir(&dir);
    (
        total_writes.load(Ordering::Relaxed),
        total_bytes.load(Ordering::Relaxed),
    )
}

fn sat_net(seconds: u64, workers: usize) -> (u64, u64) {
    let rsock = match UdpSocket::bind("127.0.0.1:0") {
        Ok(s) => s,
        Err(_) => return (0, 0),
    };
    let _ = rsock.set_read_timeout(Some(Duration::from_millis(200))); // 200ms so the receiver notices `stop` promptly
    let port = match rsock.local_addr() {
        Ok(a) => a.port(),
        Err(_) => return (0, 0),
    };

    let stop = AtomicBool::new(false);
    let total_pkts = AtomicU64::new(0);
    let total_bytes = AtomicU64::new(0);
    std::thread::scope(|s| {
        let stop_ref = &stop;
        let rsock_ref = &rsock;
        s.spawn(move || {
            let mut buf = [0u8; 256];
            while !stop_ref.load(Ordering::Relaxed) {
                let _ = rsock_ref.recv(&mut buf);
            }
        });
        let deadline = Instant::now() + Duration::from_secs(seconds);
        std::thread::scope(|s2| {
            for _ in 0..workers {
                let total_pkts = &total_pkts;
                let total_bytes = &total_bytes;
                s2.spawn(move || {
                    let sock = match UdpSocket::bind("0.0.0.0:0") {
                        Ok(s) => s,
                        Err(_) => return,
                    };
                    if sock.connect(("127.0.0.1", port)).is_err() {
                        return;
                    }
                    let payload = [0u8; 64];
                    let mut pkts: u64 = 0;
                    let mut bytes: u64 = 0;
                    while Instant::now() < deadline {
                        if let Ok(n) = sock.send(&payload)
                            && n > 0
                        {
                            pkts += 1;
                            bytes += n as u64;
                        }
                    }
                    total_pkts.fetch_add(pkts, Ordering::Relaxed);
                    total_bytes.fetch_add(bytes, Ordering::Relaxed);
                });
            }
        }); // sender pool joins here
        stop.store(true, Ordering::Relaxed);
    }); // receiver joins here (notices `stop` within ~200ms)
    (
        total_pkts.load(Ordering::Relaxed),
        total_bytes.load(Ordering::Relaxed),
    )
}

fn cmd_saturate(resource: &str, seconds: i32) -> i32 {
    let cpus = nproc();
    let workers = if resource == "mem" {
        cpus.max(2)
    } else if resource == "cpu" {
        (cpus * 3).max(4)
    } else {
        (cpus * 4).max(4)
    };

    println!("loadmix: start resource={resource} seconds={seconds} workers={workers}");
    let (ops, bytes) = match resource {
        "cpu" => sat_cpu(seconds as u64, workers),
        "mem" => sat_mem(seconds as u64, workers),
        "io" => sat_io(seconds as u64, workers),
        _ => sat_net(seconds as u64, workers),
    };
    println!(
        "loadmix: done resource={resource} seconds={seconds} workers={workers} ops={ops} bytes={bytes}"
    );
    0
}

// ------------------------------------------------------------------ analyze --

fn cmd_analyze(seconds: i32) -> i32 {
    let cpus = nproc();
    println!("analyze: start seconds={seconds} cpus={cpus}");

    let uptime_raw = match run_capture("uptime") {
        Ok(s) => trim(&s),
        Err(_) => "unavailable".to_string(),
    };
    println!("analyze: tool uptime raw=\"{uptime_raw}\"");

    let dm = run_capture("dmesg | tail -n 5");
    let dm_status = match &dm {
        Ok(s) if !s.contains("Operation not permitted") => "ok",
        _ => "denied",
    };
    println!("analyze: tool dmesg status={dm_status}");

    let top_out = run_capture("top -b -n1");
    let mut cpu_line = String::new();
    let mut tasks_line = String::new();
    if let Ok(text) = &top_out {
        for l in split_lines(text) {
            if l.starts_with("%Cpu") {
                cpu_line = trim(&l);
            }
            if l.starts_with("Tasks:") {
                tasks_line = trim(&l);
            }
        }
    }
    println!("analyze: tool top cpu=\"{cpu_line}\" tasks=\"{tasks_line}\"");

    let ss_out = run_capture("ss -s");
    let mut ss_line = String::new();
    if let Ok(text) = &ss_out
        && let Some(first) = split_lines(text).first()
    {
        ss_line = trim(first);
    }
    println!("analyze: tool ss raw=\"{ss_line}\"");

    // The five interval samplers run CONCURRENTLY (each redirected to its own
    // temp file) so an N-second checklist costs ~N seconds, not 5N.
    let dir = format!("/tmp/loadmix-analyze-{}", std::process::id());
    let _ = fs::create_dir(&dir);
    let vfile = format!("{dir}/vmstat.out");
    let mfile = format!("{dir}/mpstat.out");
    let ifile = format!("{dir}/iostat.out");
    let sfile = format!("{dir}/sar.out");
    let pfile = format!("{dir}/pidstat.out");
    let n = seconds.to_string();
    let pv = spawn_to_file(&["vmstat", "1", &n], Path::new(&vfile));
    let pm = spawn_to_file(&["mpstat", "-P", "ALL", "1", &n], Path::new(&mfile));
    let pi = spawn_to_file(&["iostat", "-xz", "1", &n], Path::new(&ifile));
    let ps = spawn_to_file(&["sar", "-n", "DEV", "1", &n], Path::new(&sfile));
    let pp = spawn_to_file(&["pidstat", "1", &n], Path::new(&pfile));
    wait_for(pv);
    wait_for(pm);
    wait_for(pi);
    wait_for(ps);
    wait_for(pp);
    let vtext = read_all(&vfile);
    let mtext = read_all(&mfile);
    let itext = read_all(&ifile);
    let stext = read_all(&sfile);
    let ptext = read_all(&pfile);
    let _ = fs::remove_file(&vfile);
    let _ = fs::remove_file(&mfile);
    let _ = fs::remove_file(&ifile);
    let _ = fs::remove_file(&sfile);
    let _ = fs::remove_file(&pfile);
    let _ = fs::remove_dir(&dir);

    let mut active = 0;
    for l in split_lines(&ptext) {
        let t = split_ws(&l);
        if t.len() >= 2 && t[0] == "Average:" && t[1] != "UID" {
            active += 1;
        }
    }
    println!("analyze: tool pidstat active_processes={active}");

    let mem_used_pct = match run_capture("free -m") {
        Ok(s) => parse_free_used_pct(&s),
        Err(_) => 0.0,
    };

    let vm = parse_vmstat(&vtext);
    let mp = parse_mpstat(&mtext);
    let io = parse_iostat(&itext);
    let net_pkts = parse_sar_pkts(&stext, "lo");

    println!(
        "analyze: metric resource=cpu name=busy_pct value={:.2} unit=pct",
        mp.busy_pct
    );
    println!(
        "analyze: metric resource=cpu name=run_queue value={:.2} unit=procs",
        vm.run_queue_max
    );
    println!("analyze: metric resource=mem name=used_pct value={mem_used_pct:.2} unit=pct");
    println!(
        "analyze: metric resource=mem name=swap_io value={:.2} unit=kbps",
        vm.swap_io_max
    );
    println!(
        "analyze: metric resource=io name=util_pct value={:.2} unit=pct",
        io.util_max
    );
    println!(
        "analyze: metric resource=io name=iowait_pct value={:.2} unit=pct",
        mp.iowait_pct
    );
    println!(
        "analyze: metric resource=io name=await_ms value={:.2} unit=ms",
        io.await_max
    );
    println!("analyze: metric resource=net name=pkts value={net_pkts:.2} unit=per_s");

    // signal thresholds: Utilization/Saturation pair per resource. cpu's
    // saturation threshold scales with this guest's own core count -- a run
    // queue longer than (cpus + headroom) means threads are waiting for a
    // CPU, the textbook USE definition of saturation.
    struct Candidate {
        resource: &'static str,
        u_val: f64,
        u_thr: f64,
        s_val: f64,
        s_thr: f64,
    }
    let table = [
        Candidate {
            resource: "cpu",
            u_val: mp.busy_pct,
            u_thr: 60.0,
            s_val: vm.run_queue_max,
            s_thr: cpus as f64 + 2.0,
        },
        Candidate {
            resource: "mem",
            u_val: mem_used_pct,
            u_thr: 75.0,
            s_val: vm.swap_io_max,
            s_thr: 300.0,
        },
        Candidate {
            resource: "io",
            u_val: io.util_max,
            u_thr: 40.0,
            s_val: mp.iowait_pct,
            s_thr: 8.0,
        },
        Candidate {
            resource: "net",
            u_val: net_pkts,
            u_thr: 2000.0,
            s_val: net_pkts,
            s_thr: 8000.0,
        },
    ];

    let mut verdict = String::new();
    let mut best_ratio = -1.0f64;
    for c in &table {
        let ufired = c.u_val >= c.u_thr;
        let sfired = c.s_val >= c.s_thr;
        println!(
            "analyze: signal resource={} type=Utilization fired={} value={:.2} threshold={:.2}",
            c.resource, ufired, c.u_val, c.u_thr
        );
        println!(
            "analyze: signal resource={} type=Saturation fired={} value={:.2} threshold={:.2}",
            c.resource, sfired, c.s_val, c.s_thr
        );
        println!(
            "analyze: signal resource={} type=Errors fired=false",
            c.resource
        );
        let ratio = c.s_val / c.s_thr;
        if ratio > best_ratio {
            best_ratio = ratio;
            verdict = c.resource.to_string();
        }
    }
    println!("analyze: verdict resource={verdict} ratio={best_ratio:.2}");
    println!("analyze: done");
    0
}

fn usage() {
    eprintln!("usage: app --resource cpu|mem|io|net --seconds N");
    eprintln!("       app analyze [--seconds N]");
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    if args.first().map(String::as_str) == Some("analyze") {
        let mut seconds: i32 = 60;
        let mut i = 1;
        while i < args.len() {
            if args[i] == "--seconds" && i + 1 < args.len() {
                match args[i + 1].parse::<i32>() {
                    Ok(v) => seconds = v,
                    Err(_) => {
                        usage();
                        std::process::exit(2);
                    }
                }
                i += 2;
            } else {
                usage();
                std::process::exit(2);
            }
        }
        if seconds <= 0 {
            usage();
            std::process::exit(2);
        }
        std::process::exit(cmd_analyze(seconds));
    }

    let mut resource = String::new();
    let mut seconds: i32 = -1;
    let mut i = 0;
    while i < args.len() {
        if args[i] == "--resource" && i + 1 < args.len() {
            resource = args[i + 1].clone();
            i += 2;
        } else if args[i] == "--seconds" && i + 1 < args.len() {
            match args[i + 1].parse::<i32>() {
                Ok(v) => seconds = v,
                Err(_) => {
                    usage();
                    std::process::exit(2);
                }
            }
            i += 2;
        } else {
            usage();
            std::process::exit(2);
        }
    }
    if !["cpu", "mem", "io", "net"].contains(&resource.as_str()) {
        usage();
        std::process::exit(2);
    }
    if seconds <= 0 {
        usage();
        std::process::exit(2);
    }
    std::process::exit(cmd_saturate(&resource, seconds));
}
