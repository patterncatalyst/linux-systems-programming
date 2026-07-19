// iobench: page cache vs durability — buffered writes, periodic fdatasync(2),
// and O_DIRECT, with identical CLI and output across the three languages.
//
//   iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE
//
// Writes M MiB (default 64) in 64 KiB blocks.
//   buffered     plain write(2)s; reports write time, then a second line
//                "fsync_ms=<t>" for the closing fdatasync(2)
//   fsync-every  fdatasync(2) every N blocks (default 8), timed end to end
//   direct       O_DIRECT with 4096-aligned buffers; if open(2) fails with
//                EINVAL, prints "direct: unsupported on this filesystem"
//                and exits 4

use std::os::fd::OwnedFd;
use std::process::ExitCode;
use std::time::Instant;

use anyhow::{Context, Result};
use rustix::fs::{Mode, OFlags, fdatasync, open};
use rustix::io::Errno;

const BLOCK_SIZE: usize = 64 * 1024;
const ALIGNMENT: usize = 4096;
const MIB: u64 = 1024 * 1024;
const USAGE: &str =
    "usage: iobench --mode buffered|fsync-every|direct [--every N] [--size-mb M] FILE";

#[derive(Clone, Copy, PartialEq, Eq)]
enum SyncMode {
    Buffered,
    FsyncEvery,
    Direct,
}

impl SyncMode {
    fn name(self) -> &'static str {
        match self {
            SyncMode::Buffered => "buffered",
            SyncMode::FsyncEvery => "fsync-every",
            SyncMode::Direct => "direct",
        }
    }
}

struct Options {
    mode: SyncMode,
    every: u64,
    size_mb: u64,
    file: String,
}

fn parse_positive(text: &str) -> Option<u64> {
    match text.parse::<u64>() {
        Ok(n) if n > 0 => Some(n),
        _ => None,
    }
}

fn parse_args(args: &[String]) -> Option<Options> {
    let mut mode = None;
    let mut every = 8u64;
    let mut size_mb = 64u64;
    let mut file: Option<String> = None;

    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--mode" => {
                mode = Some(match it.next()?.as_str() {
                    "buffered" => SyncMode::Buffered,
                    "fsync-every" => SyncMode::FsyncEvery,
                    "direct" => SyncMode::Direct,
                    _ => return None,
                });
            }
            "--every" => every = parse_positive(it.next()?)?,
            "--size-mb" => size_mb = parse_positive(it.next()?)?,
            other if other.starts_with("--") => return None,
            other => {
                if file.is_some() {
                    return None;
                }
                file = Some(other.to_string());
            }
        }
    }
    Some(Options { mode: mode?, every, size_mb, file: file? })
}

/// One 64 KiB block whose base address is 4096-aligned (required by O_DIRECT,
/// harmless otherwise). The backing Vec never reallocates, so the aligned
/// range stays valid.
struct AlignedBlock {
    backing: Vec<u8>,
    offset: usize,
}

impl AlignedBlock {
    fn new() -> Self {
        let mut backing = vec![0u8; BLOCK_SIZE + ALIGNMENT];
        let addr = backing.as_ptr() as usize;
        let offset = (ALIGNMENT - addr % ALIGNMENT) % ALIGNMENT;
        for (i, b) in backing[offset..offset + BLOCK_SIZE].iter_mut().enumerate() {
            *b = i as u8;
        }
        Self { backing, offset }
    }

    fn as_slice(&self) -> &[u8] {
        &self.backing[self.offset..self.offset + BLOCK_SIZE]
    }
}

fn open_output(path: &str, direct: bool) -> rustix::io::Result<OwnedFd> {
    let mut flags = OFlags::WRONLY | OFlags::CREATE | OFlags::TRUNC;
    if direct {
        flags |= OFlags::DIRECT;
    }
    open(path, flags, Mode::from_raw_mode(0o644))
}

/// write(2) the whole block, retrying across short writes and EINTR.
fn write_all(fd: &OwnedFd, mut data: &[u8]) -> rustix::io::Result<()> {
    while !data.is_empty() {
        match rustix::io::write(fd, data) {
            Ok(n) => data = &data[n..],
            Err(Errno::INTR) => continue,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// Elapsed nanoseconds since `from`, clamped to >= 1 so the throughput
/// division is defined.
fn elapsed_ns(from: Instant) -> u128 {
    from.elapsed().as_nanos().max(1)
}

fn run(opt: &Options) -> Result<ExitCode> {
    let fd = match open_output(&opt.file, opt.mode == SyncMode::Direct) {
        Ok(fd) => fd,
        Err(Errno::INVAL) if opt.mode == SyncMode::Direct => {
            eprintln!("direct: unsupported on this filesystem");
            return Ok(ExitCode::from(4));
        }
        Err(e) => return Err(e).context(format!("open {}", opt.file)),
    };

    let block = AlignedBlock::new();
    let nblocks = opt.size_mb * (MIB / BLOCK_SIZE as u64);
    let bytes = opt.size_mb * MIB;

    let t0 = Instant::now();
    for i in 0..nblocks {
        write_all(&fd, block.as_slice()).with_context(|| format!("write {}", opt.file))?;
        if opt.mode == SyncMode::FsyncEvery && (i + 1) % opt.every == 0 {
            fdatasync(&fd).with_context(|| format!("fdatasync {}", opt.file))?;
        }
    }
    if opt.mode == SyncMode::FsyncEvery && nblocks % opt.every != 0 {
        fdatasync(&fd).with_context(|| format!("fdatasync {}", opt.file))?;
    }
    let ns = elapsed_ns(t0);

    let mib_per_s = (bytes as f64 / MIB as f64) / (ns as f64 / 1e9);
    println!(
        "mode={} bytes={} ms={} MiB/s={:.1}",
        opt.mode.name(),
        bytes,
        ns / 1_000_000,
        mib_per_s
    );

    if opt.mode == SyncMode::Buffered {
        let t2 = Instant::now();
        fdatasync(&fd).with_context(|| format!("fdatasync {}", opt.file))?;
        println!("fsync_ms={}", elapsed_ns(t2) / 1_000_000);
    }
    Ok(ExitCode::SUCCESS)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(opt) = parse_args(&args) else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };
    match run(&opt) {
        Ok(code) => code,
        Err(err) => {
            eprintln!("error: {err:#}");
            ExitCode::from(1)
        }
    }
}
