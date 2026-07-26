//! Chapter 43's app: three metaprogramming techniques a systems programmer
//! actually reaches for.
//!
//! - `#[derive(SysError)]` (from the `sysmacros` proc-macro crate) generates
//!   `Display` + `std::error::Error` for an errno-style error enum from
//!   `#[error("...")]` attributes — a miniature `thiserror`.
//! - `#[instrument]` wraps `hash_chunk` to log its entry, exit, and
//!   wall-clock duration — tracing without a runtime.
//! - `read_record` is a sound `unsafe` unaligned read of a `repr(C)` struct
//!   out of a byte buffer, validated with miri; `--oob-read` deliberately
//!   reads 4 bytes past the buffer so miri reports the undefined behaviour.

use sysmacros::{SysError, instrument};

#[derive(SysError, Debug)]
enum PmonError {
    #[error("open {path} failed: {errno}")]
    Open { path: String, errno: i32 },
    #[error("child {pid} exited with status {code}")]
    ChildExit { pid: u32, code: i32 },
    #[error("policy rejected: {0}")]
    PolicyRejected(String),
}

#[instrument]
fn hash_chunk(len: usize) -> u64 {
    // Deterministic FNV-1a over 0..len — the exact value doesn't matter, only
    // that it's reproducible across runs.
    let mut h = 0xcbf29ce484222325u64;
    for i in 0..len {
        h = (h ^ i as u64).wrapping_mul(0x100000001b3);
    }
    h
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Record {
    magic: u32,
    slots: u32,
}

// magic 0x4b565053 (LE bytes "SPVK"), slots 256 (LE).
const BUF: [u8; 8] = [0x53, 0x50, 0x56, 0x4B, 0x00, 0x01, 0x00, 0x00];

fn read_record(buf: &[u8], oob: bool) -> Record {
    let off = if oob { 4 } else { 0 };
    // oob: deliberately reads past the 8-byte buffer.
    // SOUND path checks the bounds; the --oob-read path skips the check (the
    // bug miri catches).
    if !oob {
        assert!(buf.len() >= off + core::mem::size_of::<Record>());
    }
    // SAFETY (sound path): buf has >= 8 bytes, Record is repr(C) plain-old-data,
    // read_unaligned tolerates any alignment. Under --oob-read this reads 4 bytes
    // past the end -> undefined behaviour, which is exactly what miri reports.
    unsafe { core::ptr::read_unaligned(buf.as_ptr().add(off) as *const Record) }
}

fn main() {
    let open_err = PmonError::Open {
        path: "/etc/shadow".to_string(),
        errno: 13,
    };
    println!("error: {open_err}");

    let child_err = PmonError::ChildExit { pid: 4242, code: 1 };
    println!("error: {child_err}");

    let policy_err = PmonError::PolicyRejected("sandbox blocked os.execute".to_string());
    println!("error: {policy_err}");

    hash_chunk(4096);

    let oob = std::env::args().any(|a| a == "--oob-read");
    let rec = read_record(&BUF, oob);
    println!("record: magic={:#x} slots={}", rec.magic, rec.slots);
}
