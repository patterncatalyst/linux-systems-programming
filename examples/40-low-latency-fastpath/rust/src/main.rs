// chatterd-fastpath — a stripped, pinned, allocation-free derivative of the
// chatterd echo/pub-sub hot path (chapter 21) built purely to answer one
// question: what does a low-latency network hot path actually cost, and what
// does removing that cost look like at the syscall level?
//
// Three subcommands, one 64-byte wire frame; observable behavior is identical
// to the C++ and Go ports (same usage text, same "app: ..." lines, same
// percentiles_ns format), because verify.lua asserts against all three.
//
//   app fastpath --port P --pin CPU [--busy-poll]
//   app naive --port P
//   app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]
//
// THE FASTPATH FRAME (fixed 64 bytes):
//
//     +-------+---------+------+-----------+-----------------+---------+
//     | magic | version | type | seq (u64) | send_ns (u64)   | pad     |
//     | 2B    | 1B      | 1B   | 8B BE     | 8B BE           | 44B     |
//     +-------+---------+------+-----------+-----------------+---------+
//
// WHY THIS PORT USES RAW FDS INSTEAD OF std::net:
//
// This is the port that follows the C++ original most literally, and it can:
// Rust's std makes real blocking syscalls (there is no M:N runtime parking
// tasks off the syscall the way Go's netpoller does), so the C++ technique — a
// signal handler installed WITHOUT SA_RESTART, so a blocking read(2)/poll(2)
// already in progress returns EINTR and the loop notices the stop flag — works
// verbatim here. The Go port had to substitute a close-the-fd wakeup for
// exactly this reason; Rust does not.
//
// The catch is that std's higher-level wrappers hide EINTR: TcpListener::accept
// retries it internally (cvt_r), and Read::read_exact treats
// ErrorKind::Interrupted as "try again". Both would swallow the very signal
// this example is about. So the servers are built on raw fds via nix, where
// every EINTR is handled explicitly — which also makes OwnedFd the natural
// equivalent of the C++ RAII Socket: it closes its descriptor on drop, is
// move-only, and needs no hand-written destructor.
//
// Pinning is simpler than in Go. sched_setaffinity(2) acts on a thread, and
// this program is single-threaded — the main thread's tid equals the pid — so
// one sched_setaffinity(0, ...) call both pins the hot loop and shows up in
// /proc/<pid>/status, where verify.lua reads Cpus_allowed_list to prove the
// pinning happened at the kernel level. No LockOSThread / GOMAXPROCS dance is
// needed because there is no runtime scheduler moving the work between CPUs.

use std::net::{Ipv4Addr, SocketAddrV4};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

use nix::errno::Errno;
use nix::fcntl::{FcntlArg, OFlag, fcntl};
use nix::libc;
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sched::{CpuSet, sched_setaffinity};
use nix::sys::signal::{SaFlags, SigAction, SigHandler, SigSet, Signal, sigaction};
use nix::sys::socket::{
    AddressFamily, Backlog, MsgFlags, SockFlag, SockType, SockaddrIn, accept4, bind, connect,
    listen, recv, setsockopt, socket, sockopt,
};
use nix::unistd::{Pid, read, write};

const FRAME_SIZE: usize = 64;
const MAGIC0: u8 = 0x43; // 'C'
const MAGIC1: u8 = 0x46; // 'F'
const WIRE_VERSION: u8 = 0x01;
const TYPE_ECHO: u8 = 0x01;

type Frame = [u8; FRAME_SIZE];

// A fallible setup step carrying an already-formatted, user-facing message —
// the Rust stand-in for the C++ port's std::expected<T, std::string>.
type StepResult<T> = Result<T, String>;

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

fn build_frame(seq: u64, send_ns: u64) -> Frame {
    let mut f = [0u8; FRAME_SIZE];
    f[0] = MAGIC0;
    f[1] = MAGIC1;
    f[2] = WIRE_VERSION;
    f[3] = TYPE_ECHO;
    f[4..12].copy_from_slice(&seq.to_be_bytes());
    f[12..20].copy_from_slice(&send_ns.to_be_bytes());
    // bytes 20..63 stay zero
    f
}

fn frame_header_ok(f: &Frame) -> bool {
    f[0] == MAGIC0 && f[1] == MAGIC1 && f[2] == WIRE_VERSION && f[3] == TYPE_ECHO
}

fn seq_of(f: &Frame) -> u64 {
    u64::from_be_bytes(f[4..12].try_into().unwrap())
}

// ---------------------------------------------------------------------------
// Shutdown plumbing: a plain (non-SA_RESTART) signal handler so a blocking
// read(2)/accept-wait poll(2) in progress is interrupted with EINTR rather
// than transparently resumed — exactly the C++ port's technique.
// ---------------------------------------------------------------------------

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}

fn install_signal_handlers() {
    let sa = SigAction::new(
        SigHandler::Handler(on_signal),
        SaFlags::empty(), // deliberately NOT SA_RESTART
        SigSet::empty(),
    );
    // SAFETY: on_signal only performs a relaxed atomic store, which is
    // async-signal-safe.
    unsafe {
        let _ = sigaction(Signal::SIGINT, &sa);
        let _ = sigaction(Signal::SIGTERM, &sa);
    }
}

fn stop_requested() -> bool {
    STOP.load(Ordering::Relaxed)
}

// ---------------------------------------------------------------------------
// Pinning
// ---------------------------------------------------------------------------

fn pin_to_cpu(cpu: i32) -> StepResult<()> {
    // sysconf(_SC_NPROCESSORS_ONLN) reports CPUs online SYSTEM-WIDE and ignores
    // this process's affinity mask — the same call the C++ port uses. That
    // matters under the bench driver's `taskset -c 1`: a mask-aware count
    // (like Go's runtime.NumCPU) would report 1 and wrongly reject a pin to
    // CPU 1. sysconf sidesteps that.
    let nproc = unsafe { libc::sysconf(libc::_SC_NPROCESSORS_ONLN) };
    if cpu < 0 || (nproc > 0 && i64::from(cpu) >= nproc) {
        let last = if nproc > 0 { nproc - 1 } else { 0 };
        return Err(format!("cpu {cpu} out of range (0..{last})"));
    }
    let mut set = CpuSet::new();
    set.set(cpu as usize)
        .map_err(|e| format!("sched_setaffinity({cpu}): {e}"))?;
    // Pid 0 == the calling thread, which is the main (and only) thread, whose
    // tid == pid — so this is also what /proc/<pid>/status reports.
    sched_setaffinity(Pid::from_raw(0), &set)
        .map_err(|e| format!("sched_setaffinity({cpu}): {e}"))?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Sockets — OwnedFd is the RAII Socket: closes on drop, move-only.
// ---------------------------------------------------------------------------

fn listen_tcp(port: u16) -> StepResult<OwnedFd> {
    let sock = socket(
        AddressFamily::Inet,
        SockType::Stream,
        SockFlag::SOCK_CLOEXEC,
        None,
    )
    .map_err(|e| format!("socket: {e}"))?;
    let _ = setsockopt(&sock, sockopt::ReuseAddr, &true);

    let addr = SockaddrIn::new(0, 0, 0, 0, port); // 0.0.0.0:port
    bind(sock.as_raw_fd(), &addr).map_err(|e| format!("bind 0.0.0.0:{port}: {e}"))?;
    listen(&sock, Backlog::new(16).unwrap()).map_err(|e| format!("listen: {e}"))?;
    Ok(sock)
}

fn connect_tcp(host: &str, port: u16) -> StepResult<OwnedFd> {
    // Deliberately no DNS: the C++ port uses inet_pton(3), so a non-literal
    // host is an error here too rather than a lookup.
    let ip: Ipv4Addr = host
        .parse()
        .map_err(|_| format!("{host}: not an IPv4 address"))?;
    let sock = socket(
        AddressFamily::Inet,
        SockType::Stream,
        SockFlag::SOCK_CLOEXEC,
        None,
    )
    .map_err(|e| format!("socket: {e}"))?;
    let addr = SockaddrIn::from(SocketAddrV4::new(ip, port));
    connect(sock.as_raw_fd(), &addr).map_err(|e| format!("connect {host}:{port}: {e}"))?;
    Ok(sock)
}

// Blocking read of exactly n bytes; false on EOF, hard error, or a signal
// landing (EINTR) — the caller decides whether that means "stop".
fn read_full_blocking(fd: RawFd, buf: &mut [u8]) -> bool {
    let bfd = unsafe { BorrowedFd::borrow_raw(fd) };
    let mut off = 0;
    while off < buf.len() {
        match read(bfd, &mut buf[off..]) {
            Ok(0) => return false, // peer closed
            Ok(n) => off += n,
            Err(_) => return false,
        }
    }
    true
}

// Non-blocking spin read: the socket must already be O_NONBLOCK. Spins on
// EAGAIN with no syscall other than recv(2) itself — no sched_yield, no
// nanosleep — which is the entire point of "busy-poll".
fn read_full_busy(fd: RawFd, buf: &mut [u8]) -> bool {
    let mut off = 0;
    while off < buf.len() {
        match recv(fd, &mut buf[off..], MsgFlags::empty()) {
            Ok(0) => return false, // peer closed
            Ok(n) => off += n,
            Err(Errno::EAGAIN) => {
                if stop_requested() {
                    return false;
                }
                // spin
            }
            Err(_) => return false,
        }
    }
    true
}

fn write_all(fd: RawFd, buf: &[u8]) {
    let bfd = unsafe { BorrowedFd::borrow_raw(fd) };
    let mut off = 0;
    while off < buf.len() {
        match write(bfd, &buf[off..]) {
            Ok(n) => off += n,
            Err(Errno::EINTR) => {} // retry
            Err(_) => return,       // peer gone; nothing sensible to do
        }
    }
}

// Poll the listener with a 200ms timeout, matching the C++ poll(2) wait, so
// the stop flag is checked periodically even absent a signal. Returns
// Some(conn) on a ready connection, None to loop again (timeout / EINTR).
fn accept_one(lfd: RawFd) -> Result<Option<OwnedFd>, String> {
    let borrowed = unsafe { BorrowedFd::borrow_raw(lfd) };
    let mut fds = [PollFd::new(borrowed, PollFlags::POLLIN)];
    match poll(&mut fds, PollTimeout::from(200u16)) {
        Ok(0) => return Ok(None),
        Ok(_) => {}
        Err(Errno::EINTR) => return Ok(None),
        Err(e) => return Err(format!("poll: {e}")),
    }
    if !fds[0]
        .revents()
        .map(|r| r.contains(PollFlags::POLLIN))
        .unwrap_or(false)
    {
        return Ok(None);
    }
    match accept4(lfd, SockFlag::SOCK_CLOEXEC) {
        Ok(cfd) => Ok(Some(unsafe { OwnedFd::from_raw_fd(cfd) })),
        Err(Errno::EINTR) | Err(Errno::EAGAIN) => Ok(None),
        Err(e) => Err(format!("accept: {e}")),
    }
}

// ---------------------------------------------------------------------------
// fastpath / naive servers
// ---------------------------------------------------------------------------

fn cmd_fastpath(port: u16, pin_cpu: i32, busy_poll: bool) -> i32 {
    if let Err(e) = pin_to_cpu(pin_cpu) {
        eprintln!("app: error: {e}");
        return 1;
    }
    let lsock = match listen_tcp(port) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("app: error: {e}");
            return 1;
        }
    };
    install_signal_handlers();
    eprintln!(
        "app: fastpath listening on 0.0.0.0:{port} pinned-cpu={pin_cpu} busy-poll={}",
        if busy_poll { "on" } else { "off" }
    );

    // The ONE buffer for the whole life of the server: no allocation anywhere
    // in the loops below.
    let mut buf: Frame = [0u8; FRAME_SIZE];

    while !stop_requested() {
        let conn = match accept_one(lsock.as_raw_fd()) {
            Ok(Some(c)) => c,
            Ok(None) => continue,
            Err(e) => {
                eprintln!("app: error: {e}");
                return 1;
            }
        };
        let _ = setsockopt(&conn, sockopt::TcpNoDelay, &true);
        if busy_poll {
            let flags = fcntl(conn.as_fd(), FcntlArg::F_GETFL).unwrap_or(0);
            let mut oflag = OFlag::from_bits_truncate(flags);
            oflag.insert(OFlag::O_NONBLOCK);
            let _ = fcntl(conn.as_fd(), FcntlArg::F_SETFL(oflag));
        }
        let cfd = conn.as_raw_fd();
        loop {
            let ok = if busy_poll {
                read_full_busy(cfd, &mut buf)
            } else {
                read_full_blocking(cfd, &mut buf)
            };
            if !ok {
                break;
            }
            write_all(cfd, &buf);
        }
    }
    eprintln!("app: fastpath shutting down");
    0
}

fn cmd_naive(port: u16) -> i32 {
    let lsock = match listen_tcp(port) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("app: error: {e}");
            return 1;
        }
    };
    install_signal_handlers();
    eprintln!("app: naive listening on 0.0.0.0:{port}");

    while !stop_requested() {
        let conn = match accept_one(lsock.as_raw_fd()) {
            Ok(Some(c)) => c,
            Ok(None) => continue,
            Err(e) => {
                eprintln!("app: error: {e}");
                return 1;
            }
        };
        let cfd = conn.as_raw_fd();
        loop {
            // The "naive" part: a fresh heap buffer, every single message.
            let mut heap_buf = vec![0u8; FRAME_SIZE];
            if !read_full_blocking(cfd, &mut heap_buf) {
                break;
            }
            write_all(cfd, &heap_buf);
            // heap_buf frees here, every iteration.
        }
    }
    eprintln!("app: naive shutting down");
    0
}

// ---------------------------------------------------------------------------
// measure
// ---------------------------------------------------------------------------

fn percentile_ns(sorted: &[u64], p: f64) -> u64 {
    if sorted.is_empty() {
        return 0;
    }
    let n = sorted.len();
    let mut idx = (p / 100.0 * n as f64).ceil() as usize;
    if idx == 0 {
        idx = 1;
    }
    if idx > n {
        idx = n;
    }
    sorted[idx - 1]
}

fn cmd_measure(host: &str, port: u16, n: u64, warmup: u64, tag: &str) -> i32 {
    let conn = match connect_tcp(host, port) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("app: error: {e}");
            return 1;
        }
    };
    let _ = setsockopt(&conn, sockopt::TcpNoDelay, &true);
    let cfd = conn.as_raw_fd();

    println!("app: measure target={host}:{port} n={n} warmup={warmup}");

    let mut samples: Vec<u64> = Vec::with_capacity(n as usize);
    let total = n + warmup;
    for i in 0..total {
        let out = build_frame(i, 0);
        let t_send = Instant::now();
        write_all(cfd, &out);
        let mut inbuf: Frame = [0u8; FRAME_SIZE];
        if !read_full_blocking(cfd, &mut inbuf) {
            eprintln!("app: error: measure: connection closed at iteration {i}");
            return 1;
        }
        let dt = t_send.elapsed().as_nanos() as u64;
        if !frame_header_ok(&inbuf) || seq_of(&inbuf) != i {
            eprintln!("app: error: measure: malformed/mismatched echo at iteration {i}");
            return 1;
        }
        if i >= warmup {
            samples.push(dt);
        }
    }

    samples.sort_unstable();
    let p50 = percentile_ns(&samples, 50.0);
    let p90 = percentile_ns(&samples, 90.0);
    let p99 = percentile_ns(&samples, 99.0);
    let p999 = percentile_ns(&samples, 99.9);
    let mn = *samples.first().unwrap();
    let mx = *samples.last().unwrap();
    let sum: f64 = samples.iter().map(|&v| v as f64).sum();
    let mean = sum / samples.len() as f64;

    let tag = if tag.is_empty() { "-" } else { tag };
    println!(
        "app: percentiles_ns tag={tag} p50={p50} p90={p90} p99={p99} p99.9={p999} \
         min={mn} max={mx} mean={mean:.2} n={}",
        samples.len()
    );
    println!("app: table");
    println!("  p50    {p50} ns");
    println!("  p90    {p90} ns");
    println!("  p99    {p99} ns");
    println!("  p99.9  {p999} ns");
    println!("  min    {mn} ns");
    println!("  max    {mx} ns");
    println!("  mean   {mean:.2} ns");
    0
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn usage() -> i32 {
    eprintln!("usage: app fastpath --port P --pin CPU [--busy-poll]");
    eprintln!("       app naive --port P");
    eprintln!("       app measure --target HOST:PORT --n N [--warmup W] [--tag TAG]");
    2
}

fn parse_port(s: &str) -> Option<u16> {
    match s.parse::<u64>() {
        Ok(v) if (1..=65535).contains(&v) => Some(v as u16),
        _ => None,
    }
}

fn parse_u64(s: &str) -> Option<u64> {
    s.parse::<u64>().ok()
}

fn run(args: &[String]) -> i32 {
    if args.is_empty() {
        return usage();
    }

    match args[0].as_str() {
        "fastpath" => {
            let mut port_str = String::new();
            let mut pin_cpu: i32 = -1;
            let mut busy_poll = false;
            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--port" if i + 1 < args.len() => {
                        i += 1;
                        port_str = args[i].clone();
                    }
                    "--pin" if i + 1 < args.len() => {
                        i += 1;
                        match parse_u64(&args[i]) {
                            Some(v) => pin_cpu = v as i32,
                            None => return usage(),
                        }
                    }
                    "--busy-poll" => busy_poll = true,
                    _ => return usage(),
                }
                i += 1;
            }
            let port = match parse_port(&port_str) {
                Some(p) => p,
                None => return usage(),
            };
            if pin_cpu < 0 {
                return usage();
            }
            cmd_fastpath(port, pin_cpu, busy_poll)
        }

        "naive" => {
            let mut port_str = String::new();
            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--port" if i + 1 < args.len() => {
                        i += 1;
                        port_str = args[i].clone();
                    }
                    _ => return usage(),
                }
                i += 1;
            }
            let port = match parse_port(&port_str) {
                Some(p) => p,
                None => return usage(),
            };
            cmd_naive(port)
        }

        "measure" => {
            let mut target = String::new();
            let mut n: u64 = 0;
            let mut warmup: u64 = 200;
            let mut tag = String::new();
            let mut have_n = false;
            let mut i = 1;
            while i < args.len() {
                match args[i].as_str() {
                    "--target" if i + 1 < args.len() => {
                        i += 1;
                        target = args[i].clone();
                    }
                    "--n" if i + 1 < args.len() => {
                        i += 1;
                        match parse_u64(&args[i]) {
                            Some(v) => {
                                n = v;
                                have_n = true;
                            }
                            None => return usage(),
                        }
                    }
                    "--warmup" if i + 1 < args.len() => {
                        i += 1;
                        match parse_u64(&args[i]) {
                            Some(v) => warmup = v,
                            None => return usage(),
                        }
                    }
                    "--tag" if i + 1 < args.len() => {
                        i += 1;
                        tag = args[i].clone();
                    }
                    _ => return usage(),
                }
                i += 1;
            }
            let colon = match target.rfind(':') {
                Some(c) => c,
                None => return usage(),
            };
            if !have_n || n == 0 {
                return usage();
            }
            let port = match parse_port(&target[colon + 1..]) {
                Some(p) => p,
                None => return usage(),
            };
            cmd_measure(&target[..colon], port, n, warmup, &tag)
        }

        _ => usage(),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    std::process::exit(run(&args));
}
