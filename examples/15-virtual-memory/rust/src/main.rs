// memmap: self-inspecting virtual-memory tool — perform one kind of
// allocation, touch its pages, then report what the kernel now shows for
// this very process: the /proc/self/maps region backing the allocation,
// the VmRSS growth, and the getrusage(2) page-fault deltas.
//
//   memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|fault-walk [--mb N]
//
// Output contract (identical across C++, Go, Rust):
//   memmap: mode=<m> bytes=<b> pages=<p>
//   [fault-walk only] memmap: walk file=<path> steps=8
//   [fault-walk only] memmap: step=<i>/8 pages=<p> minor=<d> major=<d>
//   memmap: maps excerpt
//   memmap:   <raw /proc/self/maps line>   <-- target (mode=<m>)
//   memmap: vmrss_before=<kb>KB vmrss_after=<kb>KB
//   memmap: faults minor=<n> major=<n>

use std::ffi::c_void;
use std::fs::File;
use std::hint::black_box;
use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, anyhow, bail};
use rustix::mm::{MapFlags, ProtFlags, mmap, mmap_anonymous, munmap};

const MIB: usize = 1 << 20;
const STACK_CAP_MB: usize = 4; // stays well inside the 8 MiB rlimit
const WALK_STEPS: usize = 8;

// ---------------------------------------------------------------------------
// RAII wrappers
// ---------------------------------------------------------------------------

/// Owned mmap region; unmapped on drop.
struct Mapping {
    ptr: *mut c_void,
    len: usize,
}

impl Mapping {
    fn anon_rw(len: usize) -> Result<Self> {
        // SAFETY: NULL hint, fresh private mapping; kernel picks the address.
        let ptr = unsafe {
            mmap_anonymous(
                std::ptr::null_mut(),
                len,
                ProtFlags::READ | ProtFlags::WRITE,
                MapFlags::PRIVATE,
            )
        }
        .context("mmap")?;
        Ok(Self { ptr, len })
    }

    fn file_ro(file: &File, len: usize) -> Result<Self> {
        // SAFETY: NULL hint, read-only private file mapping.
        let ptr = unsafe {
            mmap(
                std::ptr::null_mut(),
                len,
                ProtFlags::READ,
                MapFlags::PRIVATE,
                file,
                0,
            )
        }
        .context("mmap")?;
        Ok(Self { ptr, len })
    }

    fn addr(&self) -> usize {
        self.ptr as usize
    }

    fn as_slice(&self) -> &[u8] {
        // SAFETY: the mapping is live for the lifetime of &self.
        unsafe { std::slice::from_raw_parts(self.ptr as *const u8, self.len) }
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        // SAFETY: the mapping is live and writable (only built via anon_rw).
        unsafe { std::slice::from_raw_parts_mut(self.ptr as *mut u8, self.len) }
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        // SAFETY: ptr/len came from a successful mmap and are unmapped once.
        unsafe {
            let _ = munmap(self.ptr, self.len);
        }
    }
}

/// Unlinks its path on drop.
struct TempFile {
    path: PathBuf,
}

impl Drop for TempFile {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

// ---------------------------------------------------------------------------
// /proc and getrusage probes
// ---------------------------------------------------------------------------

struct Baseline {
    rss_kb: i64,
    minor: i64,
    major: i64,
}

fn vmrss_kb() -> Result<i64> {
    let status = std::fs::read_to_string("/proc/self/status")
        .context("open /proc/self/status")?;
    for line in status.lines() {
        if let Some(rest) = line.strip_prefix("VmRSS:") {
            let kb = rest
                .split_whitespace()
                .next()
                .ok_or_else(|| anyhow!("cannot parse VmRSS"))?
                .parse::<i64>()
                .context("cannot parse VmRSS")?;
            return Ok(kb);
        }
    }
    bail!("VmRSS not found in /proc/self/status")
}

fn fault_counts() -> Result<(i64, i64)> {
    // SAFETY: rusage is plain data; getrusage fills it or fails.
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    let rc = unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    if rc != 0 {
        bail!("getrusage: {}", std::io::Error::last_os_error());
    }
    Ok((ru.ru_minflt, ru.ru_majflt))
}

fn take_baseline() -> Result<Baseline> {
    let rss_kb = vmrss_kb()?;
    let (minor, major) = fault_counts()?;
    Ok(Baseline { rss_kb, minor, major })
}

/// Print every /proc/self/maps line whose range overlaps [addr, addr+len).
fn print_maps_excerpt(addr: usize, len: usize, mode: &str) -> Result<()> {
    let maps = std::fs::read_to_string("/proc/self/maps")
        .context("open /proc/self/maps")?;
    println!("memmap: maps excerpt");
    let (lo, hi) = (addr, addr + len);
    for line in maps.lines() {
        let Some((range, _)) = line.split_once(' ') else {
            continue;
        };
        let Some((start_s, end_s)) = range.split_once('-') else {
            continue;
        };
        let (Ok(start), Ok(end)) = (
            usize::from_str_radix(start_s, 16),
            usize::from_str_radix(end_s, 16),
        ) else {
            continue;
        };
        if start < hi && end > lo {
            println!("memmap:   {line}   <-- target (mode={mode})");
        }
    }
    Ok(())
}

/// Common tail: maps excerpt, RSS growth, fault deltas.
fn report(addr: usize, len: usize, mode: &str, base: &Baseline) -> Result<()> {
    print_maps_excerpt(addr, len, mode)?;
    let rss_after = vmrss_kb()?;
    let (minor, major) = fault_counts()?;
    println!(
        "memmap: vmrss_before={}KB vmrss_after={}KB",
        base.rss_kb, rss_after
    );
    println!(
        "memmap: faults minor={} major={}",
        minor - base.minor,
        major - base.major
    );
    Ok(())
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

fn touch_writable(mem: &mut [u8], page: usize) {
    let mut i = 0;
    while i < mem.len() {
        mem[i] = 1;
        i += page;
    }
    black_box(mem);
}

fn touch_readable(mem: &[u8], page: usize) {
    let mut sum = 0u64;
    let mut i = 0;
    while i < mem.len() {
        sum += u64::from(mem[i]);
        i += page;
    }
    black_box(sum);
}

// Baseline is captured by the caller: the compiler's stack probing touches
// every page of this 4 MiB frame at function entry, so faults counted from
// inside the frame would miss the allocation itself.
#[inline(never)]
fn stack_worker(bytes: usize, page: usize, base: &Baseline) -> Result<()> {
    let mut buf = [0u8; STACK_CAP_MB * MIB];
    touch_writable(&mut buf[..bytes], page);
    report(buf.as_ptr() as usize, bytes, "stack", base)
}

fn run_stack(bytes: usize, page: usize) -> Result<()> {
    let base = take_baseline()?;
    stack_worker(bytes, page, &base)
}

fn run_heap(bytes: usize, page: usize) -> Result<()> {
    let base = take_baseline()?;
    let mut buf = vec![0u8; bytes];
    touch_writable(&mut buf, page);
    report(buf.as_ptr() as usize, bytes, "heap", &base)
}

fn run_mmap_anon(bytes: usize, page: usize) -> Result<()> {
    let base = take_baseline()?;
    let mut map = Mapping::anon_rw(bytes)?;
    touch_writable(map.as_mut_slice(), page);
    report(map.addr(), map.len, "mmap-anon", &base)
}

fn open_for_map(path: &Path) -> Result<(File, usize)> {
    let file =
        File::open(path).with_context(|| path.display().to_string())?;
    let len = file
        .metadata()
        .with_context(|| format!("{}: stat", path.display()))?
        .len() as usize;
    if len == 0 {
        bail!("{}: file is empty", path.display());
    }
    Ok((file, len))
}

fn run_mmap_file(path: &Path, page: usize) -> Result<()> {
    let (file, len) = open_for_map(path)?;
    println!(
        "memmap: mode=mmap-file bytes={} pages={}",
        len,
        len.div_ceil(page)
    );
    let base = take_baseline()?;
    let map = Mapping::file_ro(&file, len)?;
    touch_readable(map.as_slice(), page);
    report(map.addr(), map.len, "mmap-file", &base)
}

fn write_walk_file(bytes: usize) -> Result<PathBuf> {
    let path = std::env::temp_dir()
        .join(format!("memmap-walk-{}.bin", std::process::id()));
    let mut file =
        File::create(&path).with_context(|| path.display().to_string())?;
    let chunk = vec![0xA5u8; MIB];
    let mut left = bytes;
    while left > 0 {
        let n = left.min(chunk.len());
        file.write_all(&chunk[..n])
            .with_context(|| format!("{}: write", path.display()))?;
        left -= n;
    }
    Ok(path)
}

fn run_fault_walk(bytes: usize, page: usize) -> Result<()> {
    let tmp = TempFile {
        path: write_walk_file(bytes)?,
    };
    let (file, len) = open_for_map(&tmp.path)?;
    let base = take_baseline()?;
    let map = Mapping::file_ro(&file, len)?;

    println!("memmap: walk file={} steps={}", tmp.path.display(), WALK_STEPS);
    let pages = len / page;
    let (mut prev_minor, mut prev_major) = (base.minor, base.major);
    let mut done = 0usize;
    for step in 1..=WALK_STEPS {
        let quota = if step == WALK_STEPS {
            pages - done
        } else {
            pages / WALK_STEPS
        };
        touch_readable(
            &map.as_slice()[done * page..(done + quota) * page],
            page,
        );
        let (minor, major) = fault_counts()?;
        println!(
            "memmap: step={}/{} pages={} minor={} major={}",
            step,
            WALK_STEPS,
            quota,
            minor - prev_minor,
            major - prev_major
        );
        (prev_minor, prev_major) = (minor, major);
        done += quota;
    }
    report(map.addr(), map.len, "fault-walk", &base)
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

enum Mode {
    Stack,
    Heap,
    MmapAnon,
    MmapFile,
    FaultWalk,
}

struct Options {
    mode: Mode,
    mb: usize,
    file: Option<PathBuf>,
}

fn usage() {
    eprintln!(
        "usage: memmap --mode stack|heap|mmap-anon|mmap-file <FILE>|fault-walk [--mb N]"
    );
}

fn parse_args(args: &[String]) -> Result<Options> {
    let mut mode = None;
    let mut mb = 64usize;
    let mut file: Option<PathBuf> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--mode" => {
                i += 1;
                let m = args.get(i).ok_or_else(|| anyhow!("--mode needs a value"))?;
                mode = Some(match m.as_str() {
                    "stack" => Mode::Stack,
                    "heap" => Mode::Heap,
                    "mmap-anon" => Mode::MmapAnon,
                    "mmap-file" => Mode::MmapFile,
                    "fault-walk" => Mode::FaultWalk,
                    other => bail!("unknown mode: {other}"),
                });
            }
            "--mb" => {
                i += 1;
                let v = args.get(i).ok_or_else(|| anyhow!("--mb needs a value"))?;
                mb = match v.parse::<usize>() {
                    Ok(n) if (1..=1024).contains(&n) => n,
                    _ => bail!("--mb must be 1..1024"),
                };
            }
            a if !a.starts_with("--") && file.is_none() => {
                file = Some(PathBuf::from(a));
            }
            a => bail!("unexpected argument: {a}"),
        }
        i += 1;
    }
    let mode = mode.ok_or_else(|| anyhow!("--mode is required"))?;
    if matches!(mode, Mode::MmapFile) && file.is_none() {
        bail!("mmap-file needs a FILE argument");
    }
    if !matches!(mode, Mode::MmapFile) && file.is_some() {
        bail!("only mmap-file takes a FILE argument");
    }
    Ok(Options { mode, mb, file })
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Ok(opts) = parse_args(&args) else {
        usage();
        std::process::exit(2);
    };
    let page = rustix::param::page_size();

    let result = match opts.mode {
        Mode::Stack => {
            let bytes = opts.mb.min(STACK_CAP_MB) * MIB;
            println!("memmap: mode=stack bytes={} pages={}", bytes, bytes / page);
            run_stack(bytes, page)
        }
        Mode::Heap => {
            let bytes = opts.mb * MIB;
            println!("memmap: mode=heap bytes={} pages={}", bytes, bytes / page);
            run_heap(bytes, page)
        }
        Mode::MmapAnon => {
            let bytes = opts.mb * MIB;
            println!(
                "memmap: mode=mmap-anon bytes={} pages={}",
                bytes,
                bytes / page
            );
            run_mmap_anon(bytes, page)
        }
        // Prints its own header line (size comes from the file).
        Mode::MmapFile => run_mmap_file(opts.file.as_deref().unwrap(), page),
        Mode::FaultWalk => {
            let bytes = opts.mb * MIB;
            println!(
                "memmap: mode=fault-walk bytes={} pages={}",
                bytes,
                bytes / page
            );
            run_fault_walk(bytes, page)
        }
    };
    if let Err(err) = result {
        eprintln!("memmap: error: {err:#}");
        std::process::exit(1);
    }
}
