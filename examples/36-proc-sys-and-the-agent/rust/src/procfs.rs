//! sysagent: /proc + cgroup PSI readers and the snapshot they compose into.

use std::fs;
use std::thread;
use std::time::Duration;

/// One line of /proc/diskstats reduced to the USE-method fields.
pub struct DiskStat {
    pub name: String,
    pub reads: i64,
    pub writes: i64,
    pub read_sectors: i64,
    pub write_sectors: i64,
}

/// One interface's counters from /proc/net/dev.
pub struct NetStat {
    pub iface: String,
    pub rx_bytes: i64,
    pub tx_bytes: i64,
}

/// One fully-formed sample. Field names here are the canonical, deterministic
/// names shared with the C++ and Go implementations (see README.md) — only
/// the encoding (Rust struct vs C++ struct vs Go struct) differs.
#[derive(Default)]
pub struct Snapshot {
    pub cpu_util_pct: f64,
    pub cpu_user_pct: f64,
    pub cpu_system_pct: f64,

    pub load1: f64,
    pub load5: f64,
    pub load15: f64,
    pub runnable: i64,
    pub total_threads: i64,

    pub mem_total_kb: i64,
    pub mem_available_kb: i64,
    pub mem_used_kb: i64,

    pub disks: Vec<DiskStat>,
    pub net: Vec<NetStat>,

    pub psi_available: bool,
    pub psi_cpu_some_avg10: f64,
    pub psi_mem_some_avg10: f64,
    pub psi_io_some_avg10: f64,
}

#[derive(Default, Clone, Copy)]
struct CpuTicks {
    user: i64,
    nice: i64,
    system: i64,
    idle: i64,
    iowait: i64,
    irq: i64,
    softirq: i64,
    steal: i64,
}

/// Parses the aggregate "cpu " line of /proc/stat.
fn read_cpu_ticks() -> Result<CpuTicks, String> {
    let text = fs::read_to_string("/proc/stat").map_err(|e| format!("read /proc/stat: {e}"))?;
    let line = text.lines().next().ok_or("/proc/stat: empty")?;
    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 9 || fields[0] != "cpu" {
        return Err(format!("/proc/stat: unexpected first line {line:?}"));
    }
    let n = |i: usize| -> Result<i64, String> {
        fields[i]
            .parse()
            .map_err(|e| format!("/proc/stat: parse field {i}: {e}"))
    };
    Ok(CpuTicks {
        user: n(1)?,
        nice: n(2)?,
        system: n(3)?,
        idle: n(4)?,
        iowait: n(5)?,
        irq: n(6)?,
        softirq: n(7)?,
        steal: n(8)?,
    })
}

struct LoadAvg {
    load1: f64,
    load5: f64,
    load15: f64,
    runnable: i64,
    total_threads: i64,
}

fn read_loadavg() -> Result<LoadAvg, String> {
    let text =
        fs::read_to_string("/proc/loadavg").map_err(|e| format!("read /proc/loadavg: {e}"))?;
    let fields: Vec<&str> = text.split_whitespace().collect();
    if fields.len() < 4 {
        return Err(format!("/proc/loadavg: unexpected shape {text:?}"));
    }
    let load1 = fields[0]
        .parse()
        .map_err(|e| format!("/proc/loadavg: load1: {e}"))?;
    let load5 = fields[1]
        .parse()
        .map_err(|e| format!("/proc/loadavg: load5: {e}"))?;
    let load15 = fields[2]
        .parse()
        .map_err(|e| format!("/proc/loadavg: load15: {e}"))?;
    let (run, total) = fields[3]
        .split_once('/')
        .ok_or_else(|| format!("/proc/loadavg: bad running/total field {:?}", fields[3]))?;
    let runnable = run
        .parse()
        .map_err(|e| format!("/proc/loadavg: runnable: {e}"))?;
    let total_threads = total
        .parse()
        .map_err(|e| format!("/proc/loadavg: total: {e}"))?;
    Ok(LoadAvg {
        load1,
        load5,
        load15,
        runnable,
        total_threads,
    })
}

struct MemInfo {
    total_kb: i64,
    available_kb: i64,
}

fn read_meminfo() -> Result<MemInfo, String> {
    let text =
        fs::read_to_string("/proc/meminfo").map_err(|e| format!("read /proc/meminfo: {e}"))?;
    let mut total_kb = None;
    let mut available_kb = None;
    for line in text.lines() {
        let mut it = line.split_whitespace();
        let (Some(key), Some(val)) = (it.next(), it.next()) else {
            continue;
        };
        let Ok(v) = val.parse::<i64>() else { continue };
        match key {
            "MemTotal:" => total_kb = Some(v),
            "MemAvailable:" => available_kb = Some(v),
            _ => {}
        }
        if total_kb.is_some() && available_kb.is_some() {
            break;
        }
    }
    match (total_kb, available_kb) {
        (Some(total_kb), Some(available_kb)) => Ok(MemInfo {
            total_kb,
            available_kb,
        }),
        _ => Err("/proc/meminfo: missing MemTotal/MemAvailable".into()),
    }
}

fn read_diskstats() -> Result<Vec<DiskStat>, String> {
    let text =
        fs::read_to_string("/proc/diskstats").map_err(|e| format!("read /proc/diskstats: {e}"))?;
    let mut out = Vec::new();
    for line in text.lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        // major minor name reads_completed reads_merged sectors_read
        // ms_reading writes_completed writes_merged sectors_written ...
        if fields.len() < 10 {
            continue;
        }
        let name = fields[2];
        if name.starts_with("loop") || name.starts_with("ram") {
            continue;
        }
        let p = |i: usize| fields[i].parse::<i64>().unwrap_or(0);
        out.push(DiskStat {
            name: name.to_string(),
            reads: p(3),
            writes: p(7),
            read_sectors: p(5),
            write_sectors: p(9),
        });
    }
    Ok(out)
}

fn read_netdev() -> Result<Vec<NetStat>, String> {
    let text =
        fs::read_to_string("/proc/net/dev").map_err(|e| format!("read /proc/net/dev: {e}"))?;
    let mut out = Vec::new();
    for line in text.lines().skip(2) {
        let Some((iface, rest)) = line.split_once(':') else {
            continue;
        };
        let fields: Vec<&str> = rest.split_whitespace().collect();
        if fields.len() < 9 {
            continue;
        }
        let rx_bytes = fields[0].parse().unwrap_or(0);
        let tx_bytes = fields[8].parse().unwrap_or(0);
        out.push(NetStat {
            iface: iface.trim().to_string(),
            rx_bytes,
            tx_bytes,
        });
    }
    Ok(out)
}

#[derive(Default)]
struct Psi {
    cpu_some_avg10: f64,
    mem_some_avg10: f64,
    io_some_avg10: f64,
}

/// Pulls "avg10=X.XX" out of a PSI line such as
/// "some avg10=0.00 avg60=0.00 avg300=0.00 total=12345".
fn parse_avg10(line: &str) -> f64 {
    for tok in line.split_whitespace() {
        if let Some(v) = tok.strip_prefix("avg10=")
            && let Ok(f) = v.parse()
        {
            return f;
        }
    }
    0.0
}

/// Reads one PSI file's first ("some") line and parses its avg10, if the
/// file exists and is readable.
fn read_one_pressure(path: &str) -> Option<f64> {
    let text = fs::read_to_string(path).ok()?;
    let line = text.lines().next()?;
    Some(parse_avg10(line))
}

fn read_psi_from_dir(dir: &str) -> Option<Psi> {
    let cpu = read_one_pressure(&format!("{dir}/cpu.pressure"));
    let mem = read_one_pressure(&format!("{dir}/memory.pressure"));
    let io = read_one_pressure(&format!("{dir}/io.pressure"));
    if cpu.is_none() && mem.is_none() && io.is_none() {
        return None;
    }
    Some(Psi {
        cpu_some_avg10: cpu.unwrap_or(0.0),
        mem_some_avg10: mem.unwrap_or(0.0),
        io_some_avg10: io.unwrap_or(0.0),
    })
}

/// Prefers this process's own cgroup (v2 unified hierarchy: a single
/// "0::/path" line in /proc/self/cgroup) and falls back to the system-wide
/// /proc/pressure/* files. Returns None if PSI is not exposed anywhere on
/// this kernel/host.
fn read_psi() -> Option<Psi> {
    if let Ok(text) = fs::read_to_string("/proc/self/cgroup") {
        for line in text.lines() {
            if let Some(rel) = line.strip_prefix("0::") {
                if let Some(psi) = read_psi_from_dir(&format!("/sys/fs/cgroup{rel}")) {
                    return Some(psi);
                }
                break;
            }
        }
    }
    read_psi_from_dir("/proc/pressure")
}

/// Takes two /proc/stat readings `interval_ms` apart and fills in every other
/// field concurrently in between, via scoped threads — the Rust analogue of
/// the C++ jthread pool and the Go goroutines fanned in over a channel.
pub fn take_snapshot(interval_ms: u64) -> Result<Snapshot, String> {
    let t0 = read_cpu_ticks()?;

    let mut snap = Snapshot::default();
    let mut first_err: Option<String> = None;

    thread::scope(|s| {
        let load_h = s.spawn(read_loadavg);
        let mem_h = s.spawn(read_meminfo);
        let disk_h = s.spawn(read_diskstats);
        let net_h = s.spawn(read_netdev);
        let psi_h = s.spawn(read_psi);

        thread::sleep(Duration::from_millis(interval_ms));

        match load_h.join().unwrap() {
            Ok(la) => {
                snap.load1 = la.load1;
                snap.load5 = la.load5;
                snap.load15 = la.load15;
                snap.runnable = la.runnable;
                snap.total_threads = la.total_threads;
            }
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        match mem_h.join().unwrap() {
            Ok(mi) => {
                snap.mem_total_kb = mi.total_kb;
                snap.mem_available_kb = mi.available_kb;
                snap.mem_used_kb = mi.total_kb - mi.available_kb;
            }
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        match disk_h.join().unwrap() {
            Ok(disks) => snap.disks = disks,
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        match net_h.join().unwrap() {
            Ok(net) => snap.net = net,
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
        if let Some(psi) = psi_h.join().unwrap() {
            snap.psi_available = true;
            snap.psi_cpu_some_avg10 = psi.cpu_some_avg10;
            snap.psi_mem_some_avg10 = psi.mem_some_avg10;
            snap.psi_io_some_avg10 = psi.io_some_avg10;
        }
    });

    if let Some(e) = first_err {
        return Err(e);
    }

    let t1 = read_cpu_ticks()?;

    let busy0 = t0.user + t0.nice + t0.system;
    let busy1 = t1.user + t1.nice + t1.system;
    let idle0 = t0.idle + t0.iowait;
    let idle1 = t1.idle + t1.iowait;
    let total0 = busy0 + idle0 + t0.irq + t0.softirq + t0.steal;
    let total1 = busy1 + idle1 + t1.irq + t1.softirq + t1.steal;

    let d_total = (total1 - total0) as f64;
    let d_busy =
        ((busy1 - busy0) + (t1.irq - t0.irq) + (t1.softirq - t0.softirq) + (t1.steal - t0.steal))
            as f64;
    let d_user = (t1.user - t0.user) as f64;
    let d_system = (t1.system - t0.system) as f64;

    if d_total > 0.0 {
        snap.cpu_util_pct = 100.0 * d_busy / d_total;
        snap.cpu_user_pct = 100.0 * d_user / d_total;
        snap.cpu_system_pct = 100.0 * d_system / d_total;
    }

    Ok(snap)
}

/// Renders the deterministic single-line JSON object (see README.md).
pub fn to_json(s: &Snapshot) -> String {
    let mut o = String::new();
    o.push('{');
    o.push_str(&format!("\"cpu_util_pct\":{:.2},", s.cpu_util_pct));
    o.push_str(&format!("\"cpu_user_pct\":{:.2},", s.cpu_user_pct));
    o.push_str(&format!("\"cpu_system_pct\":{:.2},", s.cpu_system_pct));
    o.push_str(&format!("\"load1\":{:.2},", s.load1));
    o.push_str(&format!("\"load5\":{:.2},", s.load5));
    o.push_str(&format!("\"load15\":{:.2},", s.load15));
    o.push_str(&format!("\"runnable\":{},", s.runnable));
    o.push_str(&format!("\"total_threads\":{},", s.total_threads));
    o.push_str(&format!("\"mem_total_kb\":{},", s.mem_total_kb));
    o.push_str(&format!("\"mem_available_kb\":{},", s.mem_available_kb));
    o.push_str(&format!("\"mem_used_kb\":{},", s.mem_used_kb));
    o.push_str("\"disks\":[");
    for (i, d) in s.disks.iter().enumerate() {
        if i > 0 {
            o.push(',');
        }
        o.push_str(&format!(
            "{{\"name\":\"{}\",\"reads\":{},\"writes\":{},\"read_sectors\":{},\"write_sectors\":{}}}",
            d.name, d.reads, d.writes, d.read_sectors, d.write_sectors
        ));
    }
    o.push_str("],\"net\":[");
    for (i, n) in s.net.iter().enumerate() {
        if i > 0 {
            o.push(',');
        }
        o.push_str(&format!(
            "{{\"iface\":\"{}\",\"rx_bytes\":{},\"tx_bytes\":{}}}",
            n.iface, n.rx_bytes, n.tx_bytes
        ));
    }
    o.push_str("],");
    o.push_str(&format!("\"psi_available\":{},", s.psi_available));
    o.push_str(&format!(
        "\"psi_cpu_some_avg10\":{:.2},",
        s.psi_cpu_some_avg10
    ));
    o.push_str(&format!(
        "\"psi_mem_some_avg10\":{:.2},",
        s.psi_mem_some_avg10
    ));
    o.push_str(&format!("\"psi_io_some_avg10\":{:.2}", s.psi_io_some_avg10));
    o.push('}');
    o
}

/// Renders human-readable key=value lines with the same field names.
pub fn to_text(s: &Snapshot) -> String {
    let mut o = String::new();
    o.push_str(&format!(
        "cpu_util_pct={:.2} cpu_user_pct={:.2} cpu_system_pct={:.2}\n",
        s.cpu_util_pct, s.cpu_user_pct, s.cpu_system_pct
    ));
    o.push_str(&format!(
        "load1={:.2} load5={:.2} load15={:.2} runnable={} total_threads={}\n",
        s.load1, s.load5, s.load15, s.runnable, s.total_threads
    ));
    o.push_str(&format!(
        "mem_total_kb={} mem_available_kb={} mem_used_kb={}\n",
        s.mem_total_kb, s.mem_available_kb, s.mem_used_kb
    ));
    for d in &s.disks {
        o.push_str(&format!(
            "disk name={} reads={} writes={} read_sectors={} write_sectors={}\n",
            d.name, d.reads, d.writes, d.read_sectors, d.write_sectors
        ));
    }
    for n in &s.net {
        o.push_str(&format!(
            "net iface={} rx_bytes={} tx_bytes={}\n",
            n.iface, n.rx_bytes, n.tx_bytes
        ));
    }
    o.push_str(&format!(
        "psi_available={} psi_cpu_some_avg10={:.2} psi_mem_some_avg10={:.2} psi_io_some_avg10={:.2}\n",
        s.psi_available, s.psi_cpu_some_avg10, s.psi_mem_some_avg10, s.psi_io_some_avg10
    ));
    o
}
