//! copyx SRC DST — file copier that establishes the book's error taxonomy.
//!
//!   exit 0 — success; "copied <N> bytes" on stdout
//!   exit 2 — source-side failure (open/read SRC); "copyx: <reason>" on stderr
//!   exit 3 — destination-side failure (open/write/close DST); same shape
//!
//! EINTR on read(2)/write(2) is retried (the syscall was interrupted before
//! completing; reissuing it is the only correct policy), and short writes are
//! resumed until the whole buffer is on its way.

use std::os::fd::{AsFd, IntoRawFd, OwnedFd};
use std::process::ExitCode;

use rustix::fs::{Mode, OFlags, open};
use rustix::io::{Errno, read, try_close, write};
use thiserror::Error;

/// Each variant pins a failure to a syscall phase so `main` can map it onto
/// the taxonomy: source-side failures exit 2, destination-side failures 3.
#[derive(Debug, Error)]
enum CopyError {
    #[error("{path}: {reason}")]
    SrcOpen { path: String, reason: String },
    #[error("read {path}: {reason}")]
    Read { path: String, reason: String },
    #[error("{path}: {reason}")]
    DstOpen { path: String, reason: String },
    #[error("write {path}: {reason}")]
    Write { path: String, reason: String },
    #[error("close {path}: {reason}")]
    Close { path: String, reason: String },
}

impl CopyError {
    fn exit_code(&self) -> u8 {
        match self {
            CopyError::SrcOpen { .. } | CopyError::Read { .. } => 2,
            CopyError::DstOpen { .. } | CopyError::Write { .. } | CopyError::Close { .. } => 3,
        }
    }
}

/// Render an errno the way glibc's strerror(3) does, so all three
/// implementations print identical reason text. `Errno`'s own Display goes
/// through `std::io::Error`, which appends " (os error N)" — strip it.
fn strerror(errno: Errno) -> String {
    let s = std::io::Error::from(errno).to_string();
    match s.rfind(" (os error ") {
        Some(idx) => s[..idx].to_owned(),
        None => s,
    }
}

/// read(2) once, retrying EINTR: interrupted means nothing was consumed.
fn read_some(fd: impl AsFd, buf: &mut [u8]) -> Result<usize, Errno> {
    loop {
        match read(&fd, &mut *buf) {
            Err(Errno::INTR) => continue, // interrupted before transferring anything: retry
            other => return other,
        }
    }
}

/// write(2) until the whole slice is written: EINTR restarts the call, a
/// short write resumes from where the kernel stopped.
fn write_all(fd: impl AsFd, mut buf: &[u8]) -> Result<(), Errno> {
    while !buf.is_empty() {
        match write(&fd, buf) {
            Ok(n) => buf = &buf[n..],
            Err(Errno::INTR) => continue, // interrupted: reissue the same span
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// close(2) with the result observed: on the write side this is where
/// deferred IO errors surface. `into_raw_fd` transfers ownership out of the
/// `OwnedFd`, so the fd is closed exactly once.
fn close_checked(fd: OwnedFd) -> Result<(), Errno> {
    unsafe { try_close(fd.into_raw_fd()) }
}

fn copy_file(src_path: &str, dst_path: &str) -> Result<u64, CopyError> {
    let src = open(src_path, OFlags::RDONLY | OFlags::CLOEXEC, Mode::empty()).map_err(|e| {
        CopyError::SrcOpen { path: src_path.to_owned(), reason: strerror(e) }
    })?; // OwnedFd: dropped (and closed) on every path below

    let dst = open(
        dst_path,
        OFlags::WRONLY | OFlags::CREATE | OFlags::TRUNC | OFlags::CLOEXEC,
        Mode::from_bits_truncate(0o644),
    )
    .map_err(|e| CopyError::DstOpen { path: dst_path.to_owned(), reason: strerror(e) })?;

    let mut total: u64 = 0;
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = read_some(&src, &mut buf).map_err(|e| CopyError::Read {
            path: src_path.to_owned(),
            reason: strerror(e),
        })?;
        if n == 0 {
            break; // EOF
        }
        write_all(&dst, &buf[..n]).map_err(|e| CopyError::Write {
            path: dst_path.to_owned(),
            reason: strerror(e),
        })?;
        total += n as u64;
    }

    close_checked(dst).map_err(|e| CopyError::Close {
        path: dst_path.to_owned(),
        reason: strerror(e),
    })?;
    Ok(total)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: copyx SRC DST");
        return ExitCode::from(2);
    }
    match copy_file(&args[1], &args[2]) {
        Ok(n) => {
            println!("copied {n} bytes");
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("copyx: {e}");
            ExitCode::from(e.exit_code())
        }
    }
}
