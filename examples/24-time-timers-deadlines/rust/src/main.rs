// chatterd v3 — heartbeats + deadlines (chapter 24: time, timers, deadlines).
//
//   chatterd clockprobe
//   chatterd listen  --name N --addr HOST:PORT     [opts]
//   chatterd connect --name N --peer NAME@HOST:PORT [opts]
//
// The Rust take on deadlines: a periodic timerfd(CLOCK_MONOTONIC) from nix
// drives the keepalive heartbeat, a std::time::Instant deadline drives peer
// liveness, and a signalfd doorbell drives clean shutdown. All three are fed
// to one poll(2) loop alongside the peer socket. Instant is a monotonic
// clock, so a peer is declared dead by silence, never by a wall-clock reading
// that a clock step could rewind. No tokio here — that is chapter 27.
//
// Wire format — this is THE canonical chatterd chat frame, unchanged since it
// was introduced in chapter 21 and extended only by adding TYPE values in
// later chapters; the header never changes. All integers big-endian:
//
//   byte 0   MAGIC0  = 0x43 ('C')
//   byte 1   MAGIC1  = 0x48 ('H')
//   byte 2   VERSION = 0x01
//   byte 3   TYPE    1=JOIN 2=MSG 3=DELIVER 4=WELCOME 5=PING
//   byte 4-5 LENGTH  uint16 payload length N
//   byte 6.. PAYLOAD N bytes: JOIN/MSG/WELCOME/PING are UTF-8 text (PING
//                             empty); DELIVER is name + 0x00 (NUL) + text.
//
// This v3 (chapter 24) peer only ever emits JOIN, MSG, and PING — DELIVER and
// WELCOME belong to the server-broadcast versions (ch22/ch27) and, since the
// header is shared, are simply treated as liveness traffic here if ever seen.
// PING is the v3 addition over v0/v1. Any received frame is "traffic" and
// resets the deadline; a peer with no traffic for --timeout-ms is dropped.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::fd::AsFd;
use std::time::{Duration, Instant};

use nix::errno::Errno;
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::signal::{SigSet, Signal};
use nix::sys::signalfd::{SfdFlags, SignalFd};
use nix::sys::time::{TimeSpec, TimeValLike};
use nix::sys::timerfd::{
    ClockId as TfdClockId, Expiration, TimerFd, TimerFlags, TimerSetTimeFlags,
};
use nix::time::{ClockId, ClockNanosleepFlags, clock_getres, clock_gettime, clock_nanosleep};

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------
const MAGIC0: u8 = b'C';
const MAGIC1: u8 = b'H';
const VERSION: u8 = 1;
const HEADER: usize = 6;
const MAX_PAYLOAD: usize = 65535;

const TYPE_JOIN: u8 = 1;
const TYPE_MSG: u8 = 2;
const TYPE_PING: u8 = 5;
// TYPE_DELIVER = 3 and TYPE_WELCOME = 4 belong to other chatterd versions
// (server broadcast / connect motd); v3 never emits them and treats them as
// generic liveness traffic if ever received.

fn encode(typ: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER + payload.len());
    let len = payload.len() as u16;
    out.extend_from_slice(&[MAGIC0, MAGIC1, VERSION, typ, (len >> 8) as u8, (len & 0xff) as u8]);
    out.extend_from_slice(payload);
    out
}

fn send_frame(mut w: impl Write, typ: u8, payload: &[u8]) -> std::io::Result<()> {
    w.write_all(&encode(typ, payload))
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
#[derive(Clone, Default)]
struct Opts {
    name: String,
    listen_addr: String,
    peer_name: String,
    peer_addr: String,
    heartbeat_ms: i64,
    timeout_ms: i64,
    backoff_ms: i64,
    max_backoff_ms: i64,
    message: Option<String>,
    seed: Option<u64>,
}

fn usage() -> ! {
    eprintln!("usage: chatterd clockprobe");
    eprintln!("       chatterd listen  --name N --addr HOST:PORT [opts]");
    eprintln!("       chatterd connect --name N --peer NAME@HOST:PORT [opts]");
    eprintln!(
        "opts:  --heartbeat-ms H --timeout-ms T --backoff-ms B \
         --max-backoff-ms M --message MSG --seed S"
    );
    std::process::exit(2);
}

fn parse_common(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts {
        heartbeat_ms: 1000,
        timeout_ms: 3000,
        backoff_ms: 200,
        max_backoff_ms: 5000,
        ..Default::default()
    };
    let mut i = 0;
    let need = |i: &mut usize, flag: &str| -> Result<String, String> {
        if *i + 1 >= args.len() {
            return Err(format!("{flag} needs a value"));
        }
        *i += 1;
        Ok(args[*i].clone())
    };
    let num = |s: &str, flag: &str, min: i64| -> Result<i64, String> {
        s.parse::<i64>().ok().filter(|&v| v >= min).ok_or_else(|| format!("bad value for {flag}: {s}"))
    };
    while i < args.len() {
        let a = args[i].clone();
        match a.as_str() {
            "--name" => o.name = need(&mut i, &a)?,
            "--addr" => o.listen_addr = need(&mut i, &a)?,
            "--peer" => {
                let v = need(&mut i, &a)?;
                let at = v.find('@').ok_or("--peer wants NAME@HOST:PORT")?;
                o.peer_name = v[..at].to_string();
                o.peer_addr = v[at + 1..].to_string();
            }
            "--heartbeat-ms" => o.heartbeat_ms = num(&need(&mut i, &a)?, &a, 1)?,
            "--timeout-ms" => o.timeout_ms = num(&need(&mut i, &a)?, &a, 1)?,
            "--backoff-ms" => o.backoff_ms = num(&need(&mut i, &a)?, &a, 1)?,
            "--max-backoff-ms" => o.max_backoff_ms = num(&need(&mut i, &a)?, &a, 1)?,
            "--message" => o.message = Some(need(&mut i, &a)?),
            "--seed" => o.seed = Some(num(&need(&mut i, &a)?, &a, 0)? as u64),
            other => return Err(format!("unknown flag {other}")),
        }
        i += 1;
    }
    if o.name.is_empty() {
        return Err("--name is required".to_string());
    }
    Ok(o)
}

// ---------------------------------------------------------------------------
// A tiny seeded PRNG (xorshift64*) so jitter is deterministic under --seed
// without pulling in an external crate.
// ---------------------------------------------------------------------------
struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Self {
        Rng(if seed == 0 { 0x9E37_79B9_7F4A_7C15 } else { seed })
    }
    fn next_u64(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    fn below(&mut self, n: i64) -> i64 {
        if n <= 0 { 0 } else { (self.next_u64() % n as u64) as i64 }
    }
}

// ---------------------------------------------------------------------------
// Shutdown doorbell + timerfd helpers
// ---------------------------------------------------------------------------
fn install_shutdown() -> SignalFd {
    let mut mask = SigSet::empty();
    mask.add(Signal::SIGINT);
    mask.add(Signal::SIGTERM);
    mask.thread_block().expect("block signals");
    SignalFd::with_flags(&mask, SfdFlags::SFD_CLOEXEC).expect("signalfd")
}

fn periodic_timerfd(period_ms: i64) -> TimerFd {
    let tfd = TimerFd::new(TfdClockId::CLOCK_MONOTONIC, TimerFlags::empty()).expect("timerfd");
    tfd.set(Expiration::Interval(TimeSpec::milliseconds(period_ms)), TimerSetTimeFlags::empty())
        .expect("timerfd_settime");
    tfd
}

fn poll_timeout_until(deadline: Instant) -> PollTimeout {
    let left = deadline.saturating_duration_since(Instant::now());
    if left.is_zero() {
        PollTimeout::ZERO
    } else {
        let ms = left.as_millis().saturating_add(1).min(u16::MAX as u128) as u16;
        PollTimeout::from(ms)
    }
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
#[derive(PartialEq)]
enum Outcome {
    TimedOut,
    Shutdown,
    PeerError,
}

struct SessionResult {
    outcome: Outcome,
    linked: bool,
    peer: String, // name learned from the peer's JOIN, if any
}

/// Drives one connection until the peer goes silent (TimedOut), we are asked
/// to quit (Shutdown), or the socket errors (PeerError).
fn run_session(o: &Opts, mut stream: TcpStream, sfd: &SignalFd, mut peer: String) -> SessionResult {
    let hb = periodic_timerfd(o.heartbeat_ms);
    let timeout = Duration::from_millis(o.timeout_ms as u64);

    let _ = send_frame(&stream, TYPE_JOIN, o.name.as_bytes());

    let mut buf: Vec<u8> = Vec::new();
    let mut linked = false;
    let mut sock_dead = false; // peer sent EOF; keep the deadline running
    let mut deadline = Instant::now() + timeout;

    loop {
        let (sd_ready, hb_ready, sock_ready) = {
            let mut pfds = vec![
                PollFd::new(sfd.as_fd(), PollFlags::POLLIN),
                PollFd::new(hb.as_fd(), PollFlags::POLLIN),
            ];
            if !sock_dead {
                pfds.push(PollFd::new(stream.as_fd(), PollFlags::POLLIN));
            }
            match poll(&mut pfds, poll_timeout_until(deadline)) {
                Ok(0) => return SessionResult { outcome: Outcome::TimedOut, linked, peer: peer.clone() },
                Ok(_) => {}
                Err(Errno::EINTR) => continue,
                Err(e) => panic!("poll: {e}"),
            }
            let ev = |idx: usize, flags: PollFlags| {
                pfds[idx].revents().map(|r| r.intersects(flags)).unwrap_or(false)
            };
            let sk = if sock_dead {
                false
            } else {
                ev(2, PollFlags::POLLIN | PollFlags::POLLHUP)
            };
            (ev(0, PollFlags::POLLIN), ev(1, PollFlags::POLLIN), sk)
        };

        if sd_ready {
            return SessionResult { outcome: Outcome::Shutdown, linked, peer: peer.clone() };
        }
        if hb_ready {
            let _ = hb.wait();
            if !sock_dead {
                let _ = send_frame(&stream, TYPE_PING, &[]); // a broken pipe is left to the deadline
            }
        }
        if sock_ready {
            let mut tmp = [0u8; 4096];
            match stream.read(&mut tmp) {
                Ok(0) => sock_dead = true, // EOF: let the deadline declare death
                Ok(n) => {
                    buf.extend_from_slice(&tmp[..n]);
                    deadline = Instant::now() + timeout; // traffic: extend life
                    loop {
                        // Capture the type byte before pop_frame drains the header.
                        let typ = if buf.len() >= HEADER { buf[3] } else { 0 };
                        match parse_next(&mut buf) {
                            Ok(Some(payload)) => {
                                if typ == TYPE_JOIN {
                                    peer = String::from_utf8_lossy(&payload).into_owned();
                                    if !linked {
                                        linked = true;
                                        println!("chatterd: {} linked with {}", o.name, peer);
                                        if let Some(m) = &o.message {
                                            let _ = send_frame(&stream, TYPE_MSG, m.as_bytes());
                                        }
                                    }
                                } else if typ == TYPE_MSG {
                                    println!(
                                        "chatterd: {} message from {}: {}",
                                        o.name,
                                        peer,
                                        String::from_utf8_lossy(&payload)
                                    );
                                }
                                // PING / DELIVER / WELCOME / reserved: liveness only, already counted.
                            }
                            Ok(None) => break,
                            Err(()) => return SessionResult { outcome: Outcome::PeerError, linked, peer: peer.clone() },
                        }
                    }
                    if buf.len() > HEADER + MAX_PAYLOAD {
                        return SessionResult { outcome: Outcome::PeerError, linked, peer: peer.clone() };
                    }
                }
                Err(_) => sock_dead = true,
            }
        }
    }
}

/// Pop one frame's payload from `buf` (header already validated by the caller
/// reading `buf[3]`). Returns the payload bytes so the type stays with the
/// caller. `Err(())` on bad magic/version.
fn parse_next(buf: &mut Vec<u8>) -> Result<Option<Vec<u8>>, ()> {
    if buf.len() < HEADER {
        return Ok(None);
    }
    if buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[2] != VERSION {
        return Err(());
    }
    let len = ((buf[4] as usize) << 8) | buf[5] as usize;
    if buf.len() < HEADER + len {
        return Ok(None);
    }
    let payload = buf[HEADER..HEADER + len].to_vec();
    buf.drain(..HEADER + len);
    Ok(Some(payload))
}

/// Wait out a backoff, but wake early for shutdown. `true` = shut down now.
fn sleep_or_shutdown(ms: i64, sfd: &SignalFd) -> bool {
    let clamp = ms.clamp(0, u16::MAX as i64) as u16;
    loop {
        let mut pfds = [PollFd::new(sfd.as_fd(), PollFlags::POLLIN)];
        match poll(&mut pfds, PollTimeout::from(clamp)) {
            Ok(0) => return false, // backoff elapsed
            Ok(_) => return true,  // a shutdown signal arrived
            Err(Errno::EINTR) => continue,
            Err(e) => panic!("poll: {e}"),
        }
    }
}

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------
fn run_listen(o: &Opts, sfd: &SignalFd) -> i32 {
    let listener = match TcpListener::bind(&o.listen_addr) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("chatterd: error: listen: {e}");
            return 1;
        }
    };
    let bound = listener.local_addr().expect("local_addr");
    println!("chatterd: {} listening on {}", o.name, bound);

    loop {
        let ready = {
            let mut pfds = [
                PollFd::new(sfd.as_fd(), PollFlags::POLLIN),
                PollFd::new(listener.as_fd(), PollFlags::POLLIN),
            ];
            match poll(&mut pfds, PollTimeout::NONE) {
                Ok(_) => {}
                Err(Errno::EINTR) => continue,
                Err(e) => panic!("poll: {e}"),
            }
            let ev = |idx: usize| pfds[idx].revents().map(|r| r.intersects(PollFlags::POLLIN)).unwrap_or(false);
            (ev(0), ev(1))
        };
        if ready.0 {
            println!("chatterd: {} shutting down", o.name);
            return 0;
        }
        if ready.1 {
            let stream = match listener.accept() {
                Ok((s, _)) => s,
                Err(_) => continue,
            };
            let res = run_session(o, stream, sfd, "peer".to_string());
            match res.outcome {
                Outcome::Shutdown => {
                    println!("chatterd: {} shutting down", o.name);
                    return 0;
                }
                Outcome::TimedOut if res.linked => {
                    println!("chatterd: peer {} timed out", res.peer);
                }
                _ => {}
            }
            // Loop back to accept: a reconnecting peer is welcomed again.
        }
    }
}

fn run_connect(o: &Opts, sfd: &SignalFd) -> i32 {
    println!("chatterd: {} connecting to {} at {}", o.name, o.peer_name, o.peer_addr);
    let seed = o.seed.unwrap_or_else(|| {
        clock_gettime(ClockId::CLOCK_MONOTONIC).map(|t| t.tv_nsec() as u64).unwrap_or(1)
    });
    let mut rng = Rng::new(seed);
    let mut backoff = o.backoff_ms;

    loop {
        if let Ok(stream) = TcpStream::connect(&o.peer_addr) {
            backoff = o.backoff_ms; // a fresh TCP connection resets backoff
            let res = run_session(o, stream, sfd, o.peer_name.clone());
            match res.outcome {
                Outcome::Shutdown => {
                    println!("chatterd: {} shutting down", o.name);
                    return 0;
                }
                Outcome::TimedOut => {
                    println!("chatterd: peer {} timed out", o.peer_name);
                }
                Outcome::PeerError => {}
            }
        }
        // Jittered exponential backoff (equal jitter: half fixed, half random).
        let half = backoff / 2;
        let delay = half + rng.below(half + 1);
        println!("chatterd: reconnecting to {} in {}ms", o.peer_name, delay);
        if sleep_or_shutdown(delay, sfd) {
            println!("chatterd: {} shutting down", o.name);
            return 0;
        }
        backoff = (backoff * 2).min(o.max_backoff_ms);
    }
}

fn run_clockprobe() -> i32 {
    let res = clock_getres(ClockId::CLOCK_MONOTONIC).expect("clock_getres");
    let res_ns = res.tv_sec() as i64 * 1_000_000_000 + res.tv_nsec() as i64;

    let t0 = Instant::now();
    let req = TimeSpec::nanoseconds(1_000_000); // 1 ms
    while clock_nanosleep(ClockId::CLOCK_MONOTONIC, ClockNanosleepFlags::empty(), &req)
        == Err(Errno::EINTR)
    {}
    let sleep_us = t0.elapsed().as_micros();

    let tfd = TimerFd::new(TfdClockId::CLOCK_MONOTONIC, TimerFlags::empty()).expect("timerfd");
    tfd.set(Expiration::OneShot(TimeSpec::nanoseconds(1_000_000)), TimerSetTimeFlags::empty())
        .expect("timerfd_settime");
    let t1 = Instant::now();
    tfd.wait().expect("timerfd wait");
    let tfd_us = t1.elapsed().as_micros();

    println!(
        "clockprobe: CLOCK_MONOTONIC res={res_ns}ns nanosleep(1ms) actual={sleep_us}us \
         timerfd(1ms) actual={tfd_us}us"
    );
    0
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    let sub = args[0].as_str();
    let rest = &args[1..];

    if sub == "clockprobe" {
        std::process::exit(run_clockprobe());
    }
    if sub != "listen" && sub != "connect" {
        usage();
    }

    let o = parse_common(rest).unwrap_or_else(|e| {
        eprintln!("chatterd: error: {e}");
        usage();
    });

    // Block SIGINT/SIGTERM and consume them via a signalfd doorbell.
    let sfd = install_shutdown();

    let code = if sub == "listen" {
        if o.listen_addr.is_empty() {
            eprintln!("chatterd: error: listen needs --addr");
            usage();
        }
        run_listen(&o, &sfd)
    } else {
        if o.peer_addr.is_empty() {
            eprintln!("chatterd: error: connect needs --peer");
            usage();
        }
        run_connect(&o, &sfd)
    };
    std::process::exit(code);
}
