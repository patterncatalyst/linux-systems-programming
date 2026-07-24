// proto.rs — the canonical chatterd chat frame (introduced ch21, unchanged
// since): magic "CH", version 1, one byte type, big-endian u16 length, then
// payload. This capstone speaks only JOIN/MSG/DELIVER, the same three types
// every prior chatterd chapter uses.
use std::os::fd::{BorrowedFd, RawFd};

use nix::errno::Errno;
use nix::unistd::{read, write};

pub const MAGIC0: u8 = b'C';
pub const MAGIC1: u8 = b'H';
pub const VERSION: u8 = 1;

pub const TYPE_JOIN: u8 = 1;
pub const TYPE_MSG: u8 = 2;
pub const TYPE_DELIVER: u8 = 3;

pub struct Frame {
    pub typ: u8,
    pub payload: Vec<u8>,
}

fn write_all(fd: RawFd, buf: &[u8]) -> std::io::Result<()> {
    let bfd = unsafe { BorrowedFd::borrow_raw(fd) };
    let mut off = 0;
    while off < buf.len() {
        match write(bfd, &buf[off..]) {
            Ok(0) => return Err(std::io::Error::from(std::io::ErrorKind::WriteZero)),
            Ok(n) => off += n,
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e.into()),
        }
    }
    Ok(())
}

fn read_all(fd: RawFd, buf: &mut [u8]) -> std::io::Result<()> {
    let bfd = unsafe { BorrowedFd::borrow_raw(fd) };
    let mut off = 0;
    while off < buf.len() {
        match read(bfd, &mut buf[off..]) {
            Ok(0) => return Err(std::io::Error::from(std::io::ErrorKind::UnexpectedEof)),
            Ok(n) => off += n,
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e.into()),
        }
    }
    Ok(())
}

/// Writes the 6-byte header + payload to fd. Errors on a short/failed write
/// or an oversized payload.
pub fn write_frame(fd: RawFd, f: &Frame) -> std::io::Result<()> {
    if f.payload.len() > 0xFFFF {
        return Err(std::io::Error::other(format!(
            "payload too large: {} bytes",
            f.payload.len()
        )));
    }
    let mut hdr = [0u8; 6];
    hdr[0] = MAGIC0;
    hdr[1] = MAGIC1;
    hdr[2] = VERSION;
    hdr[3] = f.typ;
    hdr[4..6].copy_from_slice(&(f.payload.len() as u16).to_be_bytes());
    write_all(fd, &hdr)?;
    if !f.payload.is_empty() {
        write_all(fd, &f.payload)?;
    }
    Ok(())
}

/// Reads one frame from fd (blocking on a normal, non-nonblocking socket).
/// Errors on EOF/short-read/bad magic.
pub fn read_frame(fd: RawFd) -> std::io::Result<Frame> {
    let mut hdr = [0u8; 6];
    read_all(fd, &mut hdr)?;
    if hdr[0] != MAGIC0 || hdr[1] != MAGIC1 || hdr[2] != VERSION {
        return Err(std::io::Error::other("bad frame magic/version"));
    }
    let len = u16::from_be_bytes([hdr[4], hdr[5]]) as usize;
    let mut payload = vec![0u8; len];
    if len > 0 {
        read_all(fd, &mut payload)?;
    }
    Ok(Frame {
        typ: hdr[3],
        payload,
    })
}

/// Builds the ch21 "nick NUL text" DELIVER payload.
pub fn deliver_payload(nick: &str, text: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(nick.len() + 1 + text.len());
    out.extend_from_slice(nick.as_bytes());
    out.push(0);
    out.extend_from_slice(text.as_bytes());
    out
}

/// Reverses deliver_payload; None if there is no NUL.
pub fn split_deliver(payload: &[u8]) -> Option<(String, String)> {
    let pos = payload.iter().position(|&b| b == 0)?;
    let nick = String::from_utf8_lossy(&payload[..pos]).into_owned();
    let text = String::from_utf8_lossy(&payload[pos + 1..]).into_owned();
    Some((nick, text))
}
