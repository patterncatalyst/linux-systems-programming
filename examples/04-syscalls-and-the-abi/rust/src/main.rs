//! sysprobe: a labeled syscall specimen — openat+write+close of an unlinked
//! temp file, a 10 ms nanosleep, and 16 bytes of getrandom — built to be
//! watched under strace(1). Prints "step=<name> ok" per step, then a summary.

use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::process::ExitCode;

use anyhow::{Context, Result, bail};
use rustix::fs::{self, AtFlags, CWD, Mode, OFlags, Timespec};
use rustix::io::Errno;
use rustix::rand::{GetRandomFlags, getrandom};
use rustix::thread::{NanosleepRelativeResult, nanosleep};

const PAYLOAD: &[u8] = b"sysprobe scratch payload\n";

/// Step 1: openat(2). Prefer an anonymous O_TMPFILE inode (no name ever
/// appears in the directory); fall back to a named create+unlink on
/// filesystems that lack O_TMPFILE.
fn open_scratch(dir: &str) -> Result<OwnedFd> {
    let mode = Mode::RUSR | Mode::WUSR;
    match fs::openat(
        CWD,
        dir,
        OFlags::TMPFILE | OFlags::RDWR | OFlags::CLOEXEC,
        mode,
    ) {
        Ok(fd) => return Ok(fd),
        Err(Errno::OPNOTSUPP | Errno::ISDIR | Errno::INVAL) => {}
        Err(e) => return Err(e).with_context(|| format!("openat {dir} (O_TMPFILE)")),
    }
    let path = format!("{dir}/sysprobe.{}", std::process::id());
    let fd = fs::openat(
        CWD,
        &path,
        OFlags::CREATE | OFlags::EXCL | OFlags::RDWR | OFlags::CLOEXEC,
        mode,
    )
    .with_context(|| format!("openat {path}"))?;
    fs::unlinkat(CWD, &path, AtFlags::empty()).with_context(|| format!("unlink {path}"))?;
    Ok(fd)
}

/// Step 2: write(2), retrying on EINTR and short writes.
fn write_all(fd: BorrowedFd<'_>, mut buf: &[u8]) -> Result<()> {
    while !buf.is_empty() {
        match rustix::io::write(fd, buf) {
            Ok(n) => buf = &buf[n..],
            Err(Errno::INTR) => continue,
            Err(e) => return Err(e).context("write"),
        }
    }
    Ok(())
}

/// Step 3: nanosleep(2), resuming with the remaining time on EINTR.
fn sleep_ms(ms: i64) -> Result<()> {
    let mut req = Timespec {
        tv_sec: 0,
        tv_nsec: ms * 1_000_000,
    };
    loop {
        match nanosleep(&req) {
            NanosleepRelativeResult::Ok => return Ok(()),
            NanosleepRelativeResult::Interrupted(rem) => req = rem,
            NanosleepRelativeResult::Err(e) => bail!("nanosleep: {e}"),
        }
    }
}

/// Step 4: getrandom(2), looping over partial reads.
fn fill_random(mut buf: &mut [u8]) -> Result<()> {
    while !buf.is_empty() {
        match getrandom(&mut *buf, GetRandomFlags::empty()) {
            Ok(n) => buf = &mut buf[n..],
            Err(Errno::INTR) => continue,
            Err(e) => return Err(e).context("getrandom"),
        }
    }
    Ok(())
}

fn run() -> Result<()> {
    let dir = std::env::var("TMPDIR")
        .ok()
        .filter(|d| !d.is_empty())
        .unwrap_or_else(|| "/tmp".to_owned());

    let fd = open_scratch(&dir).context("open")?;
    println!("step=open ok");

    write_all(fd.as_fd(), PAYLOAD).context("write")?;
    println!("step=write ok");

    drop(fd); // OwnedFd drop issues close(2) here, before the sleep

    sleep_ms(10).context("sleep")?;
    println!("step=sleep ok");

    let mut entropy = [0u8; 16];
    fill_random(&mut entropy).context("random")?;
    println!("step=random ok");

    println!("sysprobe: 4 steps ok");
    Ok(())
}

fn main() -> ExitCode {
    if std::env::args_os().len() > 1 {
        eprintln!("usage: app");
        return ExitCode::from(2);
    }
    if let Err(e) = run() {
        eprintln!("sysprobe: {e:#}");
        return ExitCode::from(1);
    }
    ExitCode::SUCCESS
}
