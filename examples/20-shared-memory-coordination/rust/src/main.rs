// shmkv v1 — cross-process coordination over a shared-memory key-value file.
//
// Same SHKV2 layout as the C++ and Go versions:
//
//   offset  size  field
//   0       8     magic "SHKV2\0\0\0"
//   8       4     seqlock word   (u32; odd = writer in critical section)
//   12      4     futex word     (u32; low 32 bits of the update counter)
//   16      8     update counter (u64; total updates published)
//   24      40    reserved
//   64      512   8 slots x 64 bytes: key[24] NUL-padded, value[40] NUL-padded
//
// The file is opened through rustix (OwnedFd — the descriptor closes on
// drop), the mapping is a small RAII type that munmaps on drop, and the
// futex is the raw syscall via libc::syscall — no wrapper crate, so the
// FUTEX_WAIT/FUTEX_WAKE contract stays visible. The header words are
// accessed as &AtomicU32/&AtomicU64 references into the mapping; the slot
// bytes are copied non-atomically under the seqlock, and a torn copy is
// detected by the seq re-check and retried — the standard seqlock trade-off.

use std::os::fd::{AsFd, OwnedFd};
use std::process::ExitCode;
use std::ptr;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering, fence};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use rustix::fs::{Mode, OFlags, fstat, ftruncate, open};
use rustix::mm::{MapFlags, ProtFlags, mmap, munmap};

const MAGIC: [u8; 8] = *b"SHKV2\0\0\0";
const FILE_SIZE: usize = 4096;
const OFF_SEQ: usize = 8;
const OFF_FUTEX: usize = 12;
const OFF_COUNTER: usize = 16;
const OFF_SLOTS: usize = 64;
const SLOT_COUNT: u64 = 8;
const KEY_SIZE: usize = 24;
const VAL_SIZE: usize = 40;
const SLOT_SIZE: usize = 64;
const SLOTS_BYTES: usize = SLOT_COUNT as usize * SLOT_SIZE;

// --- the shared file -------------------------------------------------------

struct Shm {
    _fd: OwnedFd, // keeps the descriptor alive; closed on drop
    base: *mut u8,
}

// The raw pointer targets a MAP_SHARED mapping that lives as long as self.
unsafe impl Send for Shm {}
unsafe impl Sync for Shm {}

impl Drop for Shm {
    fn drop(&mut self) {
        unsafe {
            let _ = munmap(self.base.cast(), FILE_SIZE);
        }
    }
}

impl Shm {
    fn seq(&self) -> &AtomicU32 {
        unsafe { AtomicU32::from_ptr(self.base.add(OFF_SEQ).cast()) }
    }
    fn futex_word(&self) -> &AtomicU32 {
        unsafe { AtomicU32::from_ptr(self.base.add(OFF_FUTEX).cast()) }
    }
    fn counter(&self) -> &AtomicU64 {
        unsafe { AtomicU64::from_ptr(self.base.add(OFF_COUNTER).cast()) }
    }
    fn slots(&self) -> *mut u8 {
        unsafe { self.base.add(OFF_SLOTS) }
    }
}

fn open_shm(path: &str, create: bool) -> Result<Shm> {
    let mut flags = OFlags::RDWR | OFlags::CLOEXEC;
    if create {
        flags |= OFlags::CREATE;
    }
    let fd = open(path, flags, Mode::from_raw_mode(0o644))
        .map_err(std::io::Error::from)
        .with_context(|| format!("open {path}"))?;
    if create {
        ftruncate(fd.as_fd(), FILE_SIZE as u64)
            .map_err(std::io::Error::from)
            .with_context(|| format!("ftruncate {path}"))?;
    } else {
        let st = fstat(fd.as_fd())
            .map_err(std::io::Error::from)
            .with_context(|| format!("fstat {path}"))?;
        if (st.st_size as u64) < FILE_SIZE as u64 {
            bail!("{path}: bad magic (want SHKV2)");
        }
    }
    let base = unsafe {
        mmap(
            ptr::null_mut(),
            FILE_SIZE,
            ProtFlags::READ | ProtFlags::WRITE,
            MapFlags::SHARED,
            fd.as_fd(),
            0,
        )
        .map_err(std::io::Error::from)
        .with_context(|| format!("mmap {path}"))?
    }
    .cast::<u8>();
    let shm = Shm { _fd: fd, base };
    if create {
        unsafe {
            ptr::write_bytes(shm.base, 0, FILE_SIZE);
            ptr::copy_nonoverlapping(MAGIC.as_ptr(), shm.base, MAGIC.len());
        }
    } else {
        let head = unsafe { std::slice::from_raw_parts(shm.base, MAGIC.len()) };
        if head != MAGIC {
            bail!("{path}: bad magic (want SHKV2)");
        }
    }
    Ok(shm)
}

// --- seqlock ---------------------------------------------------------------

fn seqlock_write(shm: &Shm, mutate: impl FnOnce()) {
    shm.seq().fetch_add(1, Ordering::Relaxed); // odd: writer active
    fence(Ordering::Release);
    mutate();
    shm.seq().fetch_add(1, Ordering::Release); // even: consistent again
}

struct Snapshot {
    counter: u64,
    slots: [u8; SLOTS_BYTES],
}

fn seqlock_read(shm: &Shm) -> Result<Snapshot> {
    for _ in 0..1_000_000 {
        let s1 = shm.seq().load(Ordering::Acquire);
        if s1 & 1 != 0 {
            std::thread::yield_now(); // writer mid-update: retry
            continue;
        }
        let mut snap = Snapshot { counter: shm.counter().load(Ordering::Relaxed), slots: [0; SLOTS_BYTES] };
        unsafe {
            ptr::copy_nonoverlapping(shm.slots(), snap.slots.as_mut_ptr(), SLOTS_BYTES);
        }
        fence(Ordering::Acquire);
        if shm.seq().load(Ordering::Relaxed) == s1 {
            return Ok(snap);
        }
    }
    bail!("seqlock read livelocked")
}

// --- futex -----------------------------------------------------------------

// Shared (non-PRIVATE) futex: the kernel keys on the file's inode, so waiters
// and wakers in different processes rendezvous through the same mapping.
fn futex_wait(word: &AtomicU32, expected: u32, timeout: Duration) {
    let ts = libc::timespec {
        tv_sec: timeout.as_secs() as libc::time_t,
        tv_nsec: timeout.subsec_nanos() as libc::c_long,
    };
    // EAGAIN (word already moved on), ETIMEDOUT and EINTR are all normal
    // here: the caller re-checks shared state before waiting again.
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            word.as_ptr(),
            libc::FUTEX_WAIT,
            expected,
            &ts as *const libc::timespec,
            0usize,
            0u32,
        );
    }
}

fn futex_wake_all(word: &AtomicU32) {
    unsafe {
        libc::syscall(
            libc::SYS_futex,
            word.as_ptr(),
            libc::FUTEX_WAKE,
            i32::MAX,
            0usize,
            0usize,
            0u32,
        );
    }
}

// --- POSIX message queue (libc wrappers) -----------------------------------

/// mq_close + mq_unlink on drop — no queue name survives a bench run.
struct MessageQueue {
    mqd: libc::mqd_t,
    name: std::ffi::CString,
}

impl MessageQueue {
    fn create(name: &str) -> Result<Self> {
        let cname = std::ffi::CString::new(name)?;
        let mut attr: libc::mq_attr = unsafe { std::mem::zeroed() };
        attr.mq_maxmsg = 8;
        attr.mq_msgsize = 8;
        let mqd = unsafe {
            libc::mq_open(
                cname.as_ptr(),
                libc::O_CREAT | libc::O_EXCL | libc::O_RDWR,
                0o600 as libc::c_uint,
                &mut attr as *mut libc::mq_attr,
            )
        };
        if mqd == -1 {
            return Err(std::io::Error::last_os_error()).with_context(|| format!("mq_open {name}"));
        }
        Ok(Self { mqd, name: cname })
    }
}

impl Drop for MessageQueue {
    fn drop(&mut self) {
        unsafe {
            libc::mq_close(self.mqd);
            libc::mq_unlink(self.name.as_ptr());
        }
    }
}

fn mq_deadline() -> libc::timespec {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_REALTIME, &mut ts) };
    ts.tv_sec += 10;
    ts
}

fn mq_send_id(mq: &MessageQueue, id: u64) -> Result<()> {
    let buf = id.to_le_bytes();
    let ts = mq_deadline();
    let rc = unsafe {
        libc::mq_timedsend(mq.mqd, buf.as_ptr().cast(), buf.len(), 0, &ts)
    };
    if rc != 0 {
        return Err(std::io::Error::last_os_error()).context("mq_timedsend");
    }
    Ok(())
}

fn mq_receive_id(mq: &MessageQueue) -> Result<u64> {
    let mut buf = [0u8; 8];
    loop {
        let ts = mq_deadline();
        let n = unsafe {
            libc::mq_timedreceive(mq.mqd, buf.as_mut_ptr().cast(), buf.len(), ptr::null_mut(), &ts)
        };
        if n == 8 {
            return Ok(u64::from_le_bytes(buf));
        }
        let err = std::io::Error::last_os_error();
        if n < 0 && err.raw_os_error() == Some(libc::EINTR) {
            continue;
        }
        return Err(err).context("mq_timedreceive");
    }
}

// --- helpers ---------------------------------------------------------------

fn cstr(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

fn write_slot(shm: &Shm, id: u64, key: &str, val: &str) {
    let off = ((id - 1) % SLOT_COUNT) as usize * SLOT_SIZE;
    unsafe {
        let slot = shm.slots().add(off);
        ptr::write_bytes(slot, 0, SLOT_SIZE);
        let klen = key.len().min(KEY_SIZE - 1);
        ptr::copy_nonoverlapping(key.as_ptr(), slot, klen);
        let vlen = val.len().min(VAL_SIZE - 1);
        ptr::copy_nonoverlapping(val.as_ptr(), slot.add(KEY_SIZE), vlen);
    }
}

fn read_slot(snap: &Snapshot, id: u64) -> (String, String) {
    let off = ((id - 1) % SLOT_COUNT) as usize * SLOT_SIZE;
    let slot = &snap.slots[off..off + SLOT_SIZE];
    (cstr(&slot[..KEY_SIZE]), cstr(&slot[KEY_SIZE..]))
}

// --- serve -----------------------------------------------------------------

fn cmd_serve(path: &str, updates: u64, interval_ms: u64) -> Result<()> {
    let shm = open_shm(path, true)?;
    println!("serve: file={path} updates={updates} interval_ms={interval_ms}");
    for k in 1..=updates {
        std::thread::sleep(Duration::from_millis(interval_ms));
        let key = format!("k{k}");
        let val = format!("value-{k}");
        seqlock_write(&shm, || {
            write_slot(&shm, k, &key, &val);
            shm.counter().store(k, Ordering::Relaxed);
        });
        shm.futex_word().store(k as u32, Ordering::Release);
        futex_wake_all(shm.futex_word());
        println!("published update {k}: {key}={val}");
    }
    println!("serve: complete updates={updates}");
    Ok(())
}

// --- watch -----------------------------------------------------------------

fn cmd_watch(path: &str, events: u64) -> Result<()> {
    let shm = open_shm(path, false)?;
    println!("watch: file={path} events={events}");
    let deadline = Instant::now() + Duration::from_secs(30);
    let mut last = 0u64;
    let mut printed = 0u64;
    while printed < events {
        if Instant::now() > deadline {
            bail!("watch: timed out waiting for updates");
        }
        let snap = seqlock_read(&shm)?;
        if snap.counter > last {
            // The slot ring holds the last SLOT_COUNT updates, so a watcher
            // that slept through a wake can still back-fill missed ids.
            let mut id = last + 1;
            while id <= snap.counter && printed < events {
                let (key, val) = read_slot(&snap, id);
                println!("observed update {id}: {key}={val}");
                printed += 1;
                id += 1;
            }
            last = snap.counter;
            continue;
        }
        let w = shm.futex_word().load(Ordering::Acquire);
        if w != last as u32 {
            continue; // moved on while we looked: re-read now
        }
        futex_wait(shm.futex_word(), w, Duration::from_secs(2));
    }
    println!("watch: complete events={events}");
    Ok(())
}

// --- bench -----------------------------------------------------------------

fn cmd_bench(path: &str, rounds: u64, channel: &str) -> Result<()> {
    let shm = open_shm(path, true)?;
    let mq = if channel == "mq" {
        Some(MessageQueue::create(&format!(
            "/shmkv-bench-{}",
            std::process::id()
        ))?)
    } else {
        None
    };

    let ack = AtomicU64::new(0);
    let n = rounds as usize;
    let mut send_ts = vec![Instant::now(); n];

    let recv_ts = std::thread::scope(|scope| -> Result<Vec<Instant>> {
        let consumer = scope.spawn(|| -> Result<Vec<Instant>> {
            let mut recv = vec![Instant::now(); n];
            for i in 1..=rounds {
                match channel {
                    "futex" => loop {
                        let w = shm.futex_word().load(Ordering::Acquire);
                        if w >= i as u32 {
                            break;
                        }
                        futex_wait(shm.futex_word(), w, Duration::from_millis(200));
                    },
                    "poll" => loop {
                        // Sleep FIRST, then look: by construction every
                        // observation costs at least one full 1 ms nap.
                        std::thread::sleep(Duration::from_millis(1));
                        if shm.counter().load(Ordering::Acquire) >= i {
                            break;
                        }
                    },
                    _ => {
                        mq_receive_id(mq.as_ref().unwrap())?;
                    }
                }
                recv[i as usize - 1] = Instant::now();
                ack.store(i, Ordering::Release);
            }
            Ok(recv)
        });

        let spin_deadline = Instant::now() + Duration::from_secs(30);
        for i in 1..=rounds {
            // Spin until the consumer acknowledged round i-1 so rounds never
            // overlap (overlap would measure queueing, not wakeup latency).
            while ack.load(Ordering::Acquire) != i - 1 {
                if Instant::now() > spin_deadline {
                    bail!("bench: consumer stalled");
                }
                std::hint::spin_loop();
            }
            send_ts[i as usize - 1] = Instant::now();
            if channel == "mq" {
                mq_send_id(mq.as_ref().unwrap(), i)?;
            } else {
                seqlock_write(&shm, || shm.counter().store(i, Ordering::Relaxed));
                shm.futex_word().store(i as u32, Ordering::Release);
                if channel == "futex" {
                    futex_wake_all(shm.futex_word());
                }
            }
        }
        consumer.join().expect("consumer thread panicked")
    })?;

    let mut us: Vec<u64> = (0..n)
        .map(|i| {
            recv_ts[i]
                .checked_duration_since(send_ts[i])
                .unwrap_or_default()
                .as_micros() as u64
        })
        .collect();
    us.sort_unstable();
    let pct = |p: u64| {
        let idx = (rounds * p / 100).min(rounds - 1) as usize;
        us[idx]
    };
    println!(
        "bench: channel={channel} rounds={rounds} p50_us={} p99_us={}",
        pct(50),
        pct(99)
    );
    Ok(())
}

// --- CLI -------------------------------------------------------------------

fn usage() -> ExitCode {
    eprintln!("usage: shmkv serve FILE [--updates N] [--interval-ms T]");
    eprintln!("       shmkv watch FILE [--events N]");
    eprintln!("       shmkv bench FILE [--rounds N] [--channel futex|mq|poll]");
    ExitCode::from(2)
}

fn parse_u64(s: &str) -> Option<u64> {
    match s.parse::<u64>() {
        Ok(v) if v >= 1 && v <= 1_000_000 => Some(v),
        _ => None,
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 {
        return usage();
    }
    let (cmd, file) = (args[0].as_str(), args[1].as_str());

    let (mut updates, mut interval_ms, mut events, mut rounds) = (8u64, 100u64, 4u64, 100u64);
    let mut channel = String::from("futex");
    let mut i = 2;
    while i < args.len() {
        if i + 1 >= args.len() {
            return usage();
        }
        let (flag, val) = (args[i].as_str(), args[i + 1].as_str());
        match (flag, cmd) {
            ("--channel", "bench") => channel = val.to_string(),
            ("--updates", "serve") | ("--interval-ms", "serve") | ("--events", "watch")
            | ("--rounds", "bench") => {
                let Some(v) = parse_u64(val) else {
                    return usage();
                };
                match flag {
                    "--updates" => updates = v,
                    "--interval-ms" => interval_ms = v,
                    "--events" => events = v,
                    _ => rounds = v,
                }
            }
            _ => return usage(),
        }
        i += 2;
    }

    let result = match cmd {
        "serve" => cmd_serve(file, updates, interval_ms),
        "watch" => cmd_watch(file, events),
        "bench" => {
            if !matches!(channel.as_str(), "futex" | "mq" | "poll") {
                return usage();
            }
            cmd_bench(file, rounds, &channel)
        }
        _ => return usage(),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("shmkv: {e:#}");
            ExitCode::FAILURE
        }
    }
}
