// shmkv v0 — a tiny mmap-backed key/value store (Rust, edition 2024).
//
// On-disk format, byte-exact and shared with the C++ and Go implementations:
//
//   offset 0   6 bytes   magic "SHKV1\0"            (53 48 4b 56 31 00)
//   offset 6   4 bytes   u32 little-endian slot_count
//   offset 10  slot_count x 256-byte slots:
//              [  0.. 64)  key,   NUL-padded (max 63 bytes, key[0]==0 => empty)
//              [ 64..256)  value, NUL-padded (max 191 bytes)
//
// The mapping is memmap2::MmapMut over a MAP_SHARED mapping: writes are
// visible to every other process mapping the same file, and flush() (msync
// with MS_SYNC) pushes them to disk before we report success.

use std::fs::OpenOptions;
use std::process::ExitCode;

use memmap2::MmapMut;

const HEADER_SIZE: usize = 10;
const KEY_FIELD: usize = 64;
const VALUE_FIELD: usize = 192;
const SLOT_SIZE: usize = KEY_FIELD + VALUE_FIELD;
const KEY_MAX: usize = KEY_FIELD - 1; // 63: room for one NUL
const VALUE_MAX: usize = VALUE_FIELD - 1; // 191
const MAGIC: [u8; 6] = *b"SHKV1\0";

/// A command failure: fixed stderr line plus the process exit code.
/// An empty message means "print the usage line" (exit 2).
struct Fail {
    code: u8,
    msg: String,
}

impl Fail {
    fn new(code: u8, msg: impl Into<String>) -> Self {
        Self { code, msg: msg.into() }
    }
    fn usage() -> Self {
        Self { code: 2, msg: String::new() }
    }
}

type Result<T> = std::result::Result<T, Fail>;

/// An open store. `file` is a std::fs::File, which owns the descriptor as an
/// OwnedFd — dropping the Store unmaps (MmapMut's Drop runs munmap) and then
/// closes the fd, in that order (fields drop in declaration order).
struct Store {
    map: MmapMut,
    slots: u32,
    _file: std::fs::File,
}

/// Map `file` MAP_SHARED, read/write.
///
/// ---- unsafe boundary -------------------------------------------------
/// `MmapMut::map_mut` is `unsafe` because the compiler cannot see other
/// writers of the underlying file: another process truncating it would
/// turn our in-bounds loads into SIGBUS, and concurrent mutation would be
/// a data race the borrow checker never observed. Why the call is sound
/// here:
///   * every access goes through `&self.map[..]` slices bounded by the
///     length we mapped, never raw pointers;
///   * we size the mapping from ftruncate/fstat in the same process and
///     no shmkv command ever shrinks an existing store file;
///   * the demo runs shmkv commands sequentially, so cross-process writes
///     are ordered by process exit (and each writer msyncs before exit) —
///     the concurrent-writer race the type system cannot rule out is
///     excluded by the tool's usage model, not by the compiler.
/// Everything outside this function stays in safe Rust.
/// ----------------------------------------------------------------------
fn map_shared(file: &std::fs::File) -> Result<MmapMut> {
    unsafe { MmapMut::map_mut(file) }.map_err(|_| Fail::new(1, "shmkv: mmap failed"))
}

impl Store {
    fn create(path: &str, slots: u32) -> Result<Store> {
        // truncate(true) first so a reused path starts from zero bytes; the
        // following set_len (ftruncate) then extends with guaranteed-zero
        // pages, which makes every slot "empty" (key[0] == 0) for free.
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)
            .map_err(|_| Fail::new(1, format!("shmkv: cannot open {path}")))?;
        let len = HEADER_SIZE as u64 + u64::from(slots) * SLOT_SIZE as u64;
        file.set_len(len)
            .map_err(|_| Fail::new(1, format!("shmkv: ftruncate failed on {path}")))?;
        let mut map = map_shared(&file)?;
        map[..MAGIC.len()].copy_from_slice(&MAGIC);
        map[MAGIC.len()..HEADER_SIZE].copy_from_slice(&slots.to_le_bytes());
        map.flush().map_err(|_| Fail::new(1, "shmkv: msync failed"))?;
        Ok(Store { map, slots, _file: file })
    }

    fn open(path: &str) -> Result<Store> {
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .map_err(|_| Fail::new(1, format!("shmkv: cannot open {path}")))?;
        let len = file
            .metadata()
            .map_err(|_| Fail::new(1, format!("shmkv: cannot open {path}")))?
            .len() as usize;
        let bad = || Fail::new(1, format!("shmkv: {path}: not a shmkv v0 store"));
        if len < HEADER_SIZE {
            return Err(bad());
        }
        let map = map_shared(&file)?;
        if map[..MAGIC.len()] != MAGIC {
            return Err(bad());
        }
        let slots = u32::from_le_bytes(map[MAGIC.len()..HEADER_SIZE].try_into().unwrap());
        if len != HEADER_SIZE + slots as usize * SLOT_SIZE {
            return Err(bad());
        }
        Ok(Store { map, slots, _file: file })
    }

    fn slot(&self, i: u32) -> &[u8] {
        let off = HEADER_SIZE + i as usize * SLOT_SIZE;
        &self.map[off..off + SLOT_SIZE]
    }

    fn slot_key(&self, i: u32) -> &[u8] {
        field(&self.slot(i)[..KEY_FIELD])
    }

    fn slot_value(&self, i: u32) -> &[u8] {
        field(&self.slot(i)[KEY_FIELD..])
    }

    /// Linear probe: overwrite the slot holding `key`, else claim the first
    /// empty slot. Exit-5 failure when every slot is taken.
    fn set(&mut self, key: &[u8], value: &[u8]) -> Result<()> {
        let mut target = self.slots;
        let mut overwrite = false;
        let mut first_empty = self.slots;
        for i in 0..self.slots {
            let k = self.slot_key(i);
            if k == key {
                target = i;
                overwrite = true;
                break;
            }
            if k.is_empty() && first_empty == self.slots {
                first_empty = i;
            }
        }
        if !overwrite {
            target = first_empty;
        }
        if target == self.slots {
            return Err(Fail::new(5, format!("shmkv: store full ({} slots)", self.slots)));
        }
        let off = HEADER_SIZE + target as usize * SLOT_SIZE;
        let slot = &mut self.map[off..off + SLOT_SIZE];
        slot.fill(0); // clear any longer previous value
        slot[..key.len()].copy_from_slice(key);
        slot[KEY_FIELD..KEY_FIELD + value.len()].copy_from_slice(value);
        self.map.flush().map_err(|_| Fail::new(1, "shmkv: msync failed"))
    }
}

fn field(bytes: &[u8]) -> &[u8] {
    match bytes.iter().position(|&b| b == 0) {
        Some(n) => &bytes[..n],
        None => bytes,
    }
}

/// Digits only, in range [1, u32 max] — identical rules in all three
/// languages so the CLIs reject exactly the same inputs.
fn parse_slots(text: &str) -> Result<u32> {
    if text.is_empty() || text.len() > 10 || !text.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Fail::usage());
    }
    let v: u64 = text.bytes().fold(0, |acc, b| acc * 10 + u64::from(b - b'0'));
    if v == 0 || v > u64::from(u32::MAX) {
        return Err(Fail::usage());
    }
    Ok(v as u32)
}

fn cmd_create(file: &str, slots_text: &str) -> Result<()> {
    let slots = parse_slots(slots_text)?;
    let _store = Store::create(file, slots)?;
    let bytes = HEADER_SIZE as u64 + u64::from(slots) * SLOT_SIZE as u64;
    println!("created {file}: {slots} slots, {bytes} bytes");
    Ok(())
}

fn cmd_set(file: &str, key: &str, value: &str) -> Result<()> {
    if key.is_empty() {
        return Err(Fail::new(2, "shmkv: empty key"));
    }
    if key.len() > KEY_MAX {
        return Err(Fail::new(2, "shmkv: key too long (max 63 bytes)"));
    }
    if value.len() > VALUE_MAX {
        return Err(Fail::new(2, "shmkv: value too long (max 191 bytes)"));
    }
    let mut store = Store::open(file)?;
    store.set(key.as_bytes(), value.as_bytes())?;
    println!("set {key}");
    Ok(())
}

fn cmd_get(file: &str, key: &str) -> Result<()> {
    let store = Store::open(file)?;
    for i in 0..store.slots {
        if store.slot_key(i) == key.as_bytes() {
            println!("{}", String::from_utf8_lossy(store.slot_value(i)));
            return Ok(());
        }
    }
    Err(Fail::new(4, "shmkv: key not found"))
}

fn cmd_dump(file: &str) -> Result<()> {
    let store = Store::open(file)?;
    let mut pairs: Vec<(Vec<u8>, Vec<u8>)> = (0..store.slots)
        .filter(|&i| !store.slot_key(i).is_empty())
        .map(|i| (store.slot_key(i).to_vec(), store.slot_value(i).to_vec()))
        .collect();
    pairs.sort(); // bytewise by key, matching C++/Go
    for (k, v) in &pairs {
        println!("{}={}", String::from_utf8_lossy(k), String::from_utf8_lossy(v));
    }
    eprintln!("shmkv: {}/{} slots used", pairs.len(), store.slots);
    Ok(())
}

fn usage() -> ExitCode {
    eprintln!("usage: shmkv create FILE --slots N | set FILE KEY VALUE | get FILE KEY | dump FILE");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let strs: Vec<&str> = args.iter().map(String::as_str).collect();
    let result = match strs.as_slice() {
        ["create", file, "--slots", n] => cmd_create(file, n),
        ["set", file, key, value] => cmd_set(file, key, value),
        ["get", file, key] => cmd_get(file, key),
        ["dump", file] => cmd_dump(file),
        _ => return usage(),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(f) if f.msg.is_empty() => usage(),
        Err(f) => {
            eprintln!("{}", f.msg);
            ExitCode::from(f.code)
        }
    }
}
