// fwatch v2 — batched sync engine (chapter 10: io_uring).
//
// Subcommands:
//   fwatch scan DIR                                 (v0: polling snapshot)
//   fwatch watch DIR --timeout MS                   (v1: inotify events)
//   fwatch sync SRCDIR DSTDIR [--engine rw|uring]   (v2: batched copy)
//
// The uring engine uses the io-uring crate: each file is copied by batching
// linked READ->WRITE SQE pairs (one pair per 128 KiB chunk, up to 8 pairs per
// submit_and_wait) at explicit offsets.

use std::fs::{self, File};
use std::io::{Read, Write};
use std::os::fd::{AsFd, AsRawFd};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use io_uring::{IoUring, opcode, squeue, types};
use nix::errno::Errno;
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::inotify::{AddWatchFlags, InitFlags, Inotify};

const CHUNK: u64 = 128 * 1024;
const PAIRS: usize = 8; // READ->WRITE pairs batched per submit

fn usage() -> ExitCode {
    eprintln!("usage: fwatch <command>");
    eprintln!("  fwatch scan DIR");
    eprintln!("  fwatch watch DIR --timeout MS");
    eprintln!("  fwatch sync SRCDIR DSTDIR [--engine rw|uring]");
    ExitCode::from(2)
}

/// Recursively collect entries below `root` (the root itself excluded).
/// Symlinks and other special files are recorded as neither dir nor regular.
fn collect(root: &Path, dir: &Path, out: &mut Vec<(PathBuf, fs::Metadata)>) -> Result<()> {
    for entry in fs::read_dir(dir).with_context(|| format!("read_dir {}", dir.display()))? {
        let entry = entry.with_context(|| format!("read_dir {}", dir.display()))?;
        let path = entry.path();
        let meta = fs::symlink_metadata(&path)
            .with_context(|| format!("stat {}", path.display()))?;
        let is_dir = meta.is_dir();
        out.push((path.clone(), meta));
        if is_dir {
            collect(root, &path, out)?;
        }
    }
    Ok(())
}

fn cmd_scan(dir: &Path) -> Result<()> {
    let mut entries = Vec::new();
    collect(dir, dir, &mut entries)?;
    let (mut files, mut bytes) = (0u64, 0u64);
    for (_, meta) in &entries {
        if meta.is_file() {
            files += 1;
            bytes += meta.len();
        }
    }
    println!("scanned {files} files {bytes} bytes");
    Ok(())
}

/// Plain read/write loop copy.
fn copy_file_rw(src: &Path, dst: &Path) -> Result<u64> {
    let mut input = File::open(src).with_context(|| format!("open {}", src.display()))?;
    let mut output = File::create(dst).with_context(|| format!("create {}", dst.display()))?;
    let mut buf = vec![0u8; CHUNK as usize];
    let mut total = 0u64;
    loop {
        let n = input
            .read(&mut buf)
            .with_context(|| format!("read {}", src.display()))?;
        if n == 0 {
            break;
        }
        output
            .write_all(&buf[..n])
            .with_context(|| format!("write {}", dst.display()))?;
        total += n as u64;
    }
    Ok(total)
}

/// io_uring copy: batches of linked READ->WRITE pairs at explicit offsets.
fn copy_file_uring(ring: &mut IoUring, src: &Path, dst: &Path) -> Result<u64> {
    let input = File::open(src).with_context(|| format!("open {}", src.display()))?;
    let output = File::create(dst).with_context(|| format!("create {}", dst.display()))?;
    let size = input.metadata()?.len();

    let mut bufs = vec![vec![0u8; CHUNK as usize]; PAIRS];
    let mut off = 0u64;
    while off < size {
        let mut lens = [0u32; PAIRS];
        let mut pairs = 0usize;
        {
            let mut sq = ring.submission();
            while pairs < PAIRS && off < size {
                let len = CHUNK.min(size - off) as u32;
                lens[pairs] = len;
                let buf = bufs[pairs].as_mut_ptr();
                // READ this chunk into the pair's buffer, then — only if it
                // fully succeeded (IO_LINK) — WRITE it at the same offset.
                let read = opcode::Read::new(types::Fd(input.as_raw_fd()), buf, len)
                    .offset(off)
                    .build()
                    .flags(squeue::Flags::IO_LINK)
                    .user_data((pairs as u64) << 1);
                let write = opcode::Write::new(types::Fd(output.as_raw_fd()), buf, len)
                    .offset(off)
                    .build()
                    .user_data(((pairs as u64) << 1) | 1);
                // SAFETY: the buffers and fds outlive the submissions; we wait
                // for all completions before the next loop iteration.
                unsafe {
                    sq.push(&read).map_err(|_| anyhow!("submission queue full"))?;
                    sq.push(&write).map_err(|_| anyhow!("submission queue full"))?;
                }
                off += u64::from(len);
                pairs += 1;
            }
        } // drop the SQ view: syncs the tail before entering the kernel
        ring.submit_and_wait(2 * pairs)
            .context("io_uring_enter")?;
        let mut seen = 0usize;
        while seen < 2 * pairs {
            for cqe in ring.completion() {
                let slot = (cqe.user_data() >> 1) as usize;
                if cqe.result() < 0 || cqe.result() as u32 != lens[slot] {
                    bail!(
                        "io_uring short op on {}: got {} want {}",
                        src.display(),
                        cqe.result(),
                        lens[slot]
                    );
                }
                seen += 1;
            }
            if seen < 2 * pairs {
                ring.submit_and_wait(2 * pairs - seen).context("io_uring_enter")?;
            }
        }
    }
    Ok(size)
}

fn cmd_sync(src: &Path, dst: &Path, engine: &str) -> Result<()> {
    let start = Instant::now();
    if !src.is_dir() {
        bail!("{}: not a directory", src.display());
    }
    fs::create_dir_all(dst).with_context(|| format!("mkdir {}", dst.display()))?;

    let mut ring = if engine == "uring" {
        Some(IoUring::new(64).context("io_uring_setup")?)
    } else {
        None
    };

    let mut entries = Vec::new();
    collect(src, src, &mut entries)?;
    let (mut files, mut bytes) = (0u64, 0u64);
    for (path, meta) in &entries {
        let target = dst.join(path.strip_prefix(src)?);
        if meta.is_dir() {
            fs::create_dir_all(&target)
                .with_context(|| format!("mkdir {}", target.display()))?;
        } else if meta.is_file() {
            let copied = match ring.as_mut() {
                Some(ring) => copy_file_uring(ring, path, &target)?,
                None => copy_file_rw(path, &target)?,
            };
            files += 1;
            bytes += copied;
        }
    }

    let ms = start.elapsed().as_millis();
    println!("synced {files} files {bytes} bytes engine={engine} ms={ms}");
    Ok(())
}

fn cmd_watch(dir: &Path, timeout_ms: u64) -> Result<()> {
    let inotify = Inotify::init(InitFlags::IN_NONBLOCK | InitFlags::IN_CLOEXEC)
        .context("inotify_init1")?;
    inotify
        .add_watch(
            dir,
            AddWatchFlags::IN_CREATE | AddWatchFlags::IN_MODIFY | AddWatchFlags::IN_DELETE,
        )
        .with_context(|| format!("watch {}", dir.display()))?;

    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
    let mut count = 0u64;
    loop {
        let now = Instant::now();
        if now >= deadline {
            break;
        }
        let remaining = (deadline - now).as_millis().min(i32::MAX as u128) as i32;
        let mut fds = [PollFd::new(inotify.as_fd(), PollFlags::POLLIN)];
        match poll(
            &mut fds,
            PollTimeout::try_from(remaining).unwrap_or(PollTimeout::MAX),
        ) {
            Ok(0) => continue, // deadline check at the loop top ends the watch
            Ok(_) => {}
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e).context("poll"),
        }
        match inotify.read_events() {
            Ok(events) => {
                for ev in events {
                    let kind = if ev.mask.contains(AddWatchFlags::IN_CREATE) {
                        "CREATE"
                    } else if ev.mask.contains(AddWatchFlags::IN_MODIFY) {
                        "MODIFY"
                    } else if ev.mask.contains(AddWatchFlags::IN_DELETE) {
                        "DELETE"
                    } else {
                        continue;
                    };
                    if let Some(name) = ev.name {
                        println!("event {} {}", kind, name.to_string_lossy());
                        count += 1;
                    }
                }
            }
            Err(Errno::EAGAIN) => continue,
            Err(e) => return Err(e).context("inotify read"),
        }
    }
    println!("watched {count} events");
    Ok(())
}

fn dispatch(args: &[String]) -> Option<Result<()>> {
    match args.first().map(String::as_str) {
        Some("scan") if args.len() == 2 => Some(cmd_scan(Path::new(&args[1]))),
        Some("watch") if args.len() == 4 && args[2] == "--timeout" => {
            let timeout_ms: u64 = args[3].parse().ok()?;
            Some(cmd_watch(Path::new(&args[1]), timeout_ms))
        }
        Some("sync") if args.len() == 3 || args.len() == 5 => {
            let engine = if args.len() == 5 {
                if args[3] != "--engine" {
                    return None;
                }
                args[4].as_str()
            } else {
                "rw"
            };
            if engine != "rw" && engine != "uring" {
                return None;
            }
            Some(cmd_sync(Path::new(&args[1]), Path::new(&args[2]), engine))
        }
        _ => None,
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match dispatch(&args) {
        None => usage(),
        Some(Ok(())) => ExitCode::SUCCESS,
        Some(Err(err)) => {
            eprintln!("fwatch: {err:#}");
            ExitCode::FAILURE
        }
    }
}
