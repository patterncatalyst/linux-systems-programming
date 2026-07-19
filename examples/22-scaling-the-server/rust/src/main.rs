// chatterd v1 — a peer-to-peer chat daemon growing chapter by chapter.
//
// ch21 introduced the length-prefixed frame protocol and a threaded server
// (one thread per connection). ch22 — "scaling the server" — keeps that
// threaded engine working and adds a single-threaded epoll engine, built with
// mio: one thread, many connections, nonblocking accept plus a per-connection
// read/write state machine with an outbound queue and real backpressure
// (partial writes are carried across poll iterations, WRITABLE drives the flush).
//
// Wire format (identical across cpp/go/rust and every version), big-endian:
//
//   offset 0  2  magic   'C' 'H'  (0x43 0x48)
//   offset 2  1  version 0x01
//   offset 3  1  type
//   offset 4  2  length  u16 payload byte count
//   offset 6  N  payload
//
//   type 0x01 JOIN     c->s  payload = nickname (UTF-8)
//   type 0x02 MSG      c->s  payload = message text (UTF-8)
//   type 0x03 DELIVER  s->c  payload = nick + 0x00 (NUL) + text
//   type 0x04 WELCOME  s->c  payload = empty (sent on accept)
//
// The server delivers every MSG to every connected client, including the
// originator, so a client confirms acceptance by seeing its own line echoed.

use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::net::{Shutdown, TcpListener as StdTcpListener, TcpStream as StdTcpStream};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use anyhow::{Context, Result};
use mio::net::{TcpListener as MioTcpListener, TcpStream as MioTcpStream};
use mio::unix::SourceFd;
use mio::{Events, Interest, Poll, Token};
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::signal::{SigSet, Signal};
use nix::sys::signalfd::{SfdFlags, SignalFd};

const VERSION: u8 = 0x01;
const TYPE_JOIN: u8 = 0x01;
const TYPE_MSG: u8 = 0x02;
const TYPE_DELIVER: u8 = 0x03;
const TYPE_WELCOME: u8 = 0x04;

// ---------------------------------------------------------------------------
// Frame protocol
// ---------------------------------------------------------------------------

fn encode(typ: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(6 + payload.len());
    f.extend_from_slice(&[
        b'C',
        b'H',
        VERSION,
        typ,
        (payload.len() >> 8) as u8,
        payload.len() as u8,
    ]);
    f.extend_from_slice(payload);
    f
}

fn deliver_payload(nick: &str, text: &[u8]) -> Vec<u8> {
    let nb = nick.as_bytes();
    let mut p = Vec::with_capacity(nb.len() + 1 + text.len());
    p.extend_from_slice(nb);
    p.push(0);
    p.extend_from_slice(text);
    p
}

fn split_deliver(payload: &[u8]) -> (String, String) {
    match payload.iter().position(|&b| b == 0) {
        Some(nul) => (
            String::from_utf8_lossy(&payload[..nul]).into_owned(),
            String::from_utf8_lossy(&payload[nul + 1..]).into_owned(),
        ),
        None => ("?".into(), String::from_utf8_lossy(payload).into_owned()),
    }
}

// Pull one complete frame off the front of buf, consuming its bytes. Returns
// None when buf holds only a partial frame (need more bytes).
fn take_frame(buf: &mut Vec<u8>) -> Option<(u8, Vec<u8>)> {
    if buf.len() < 6 {
        return None;
    }
    if buf[0] != b'C' || buf[1] != b'H' || buf[2] != VERSION {
        buf.clear(); // desync on a foreign speaker; drop the stream
        return None;
    }
    let len = ((buf[4] as usize) << 8) | buf[5] as usize;
    if buf.len() < 6 + len {
        return None;
    }
    let typ = buf[3];
    let payload = buf[6..6 + len].to_vec();
    buf.drain(0..6 + len);
    Some((typ, payload))
}

// ---------------------------------------------------------------------------
// Signals — SIGINT/SIGTERM blocked process-wide, delivered as readable data.
// ---------------------------------------------------------------------------

fn block_signals() -> Result<SignalFd> {
    let mut set = SigSet::empty();
    set.add(Signal::SIGINT);
    set.add(Signal::SIGTERM);
    set.thread_block().context("sigprocmask")?;
    SignalFd::with_flags(&set, SfdFlags::SFD_NONBLOCK | SfdFlags::SFD_CLOEXEC)
        .context("signalfd")
}

// ---------------------------------------------------------------------------
// Threaded engine — one std thread per connection, a shared broadcast registry.
// ---------------------------------------------------------------------------

struct ConnHandle {
    id: u64,
    w: Mutex<StdTcpStream>, // a clone used for writes + shutdown
    nick: Mutex<String>,
}

struct ConnRegistry {
    conns: Mutex<HashMap<u64, Arc<ConnHandle>>>,
    messages: AtomicU64,
    peak: AtomicUsize,
}

impl ConnRegistry {
    fn new() -> Self {
        Self {
            conns: Mutex::new(HashMap::new()),
            messages: AtomicU64::new(0),
            peak: AtomicUsize::new(0),
        }
    }
    fn add(&self, h: Arc<ConnHandle>) {
        let mut m = self.conns.lock().unwrap();
        m.insert(h.id, h);
        let n = m.len();
        if n > self.peak.load(Ordering::Relaxed) {
            self.peak.store(n, Ordering::Relaxed);
        }
    }
    fn remove(&self, id: u64) {
        self.conns.lock().unwrap().remove(&id);
    }
    fn broadcast(&self, frame: &[u8]) {
        let snap: Vec<Arc<ConnHandle>> = self.conns.lock().unwrap().values().cloned().collect();
        for h in snap {
            let mut s = h.w.lock().unwrap();
            let _ = s.write_all(frame); // a dead peer is reaped by its reader
        }
    }
    fn shutdown_all(&self) {
        for h in self.conns.lock().unwrap().values() {
            let s = h.w.lock().unwrap();
            let _ = s.shutdown(Shutdown::Both); // wake the reader blocked in read()
        }
    }
}

fn serve_conn(mut stream: StdTcpStream, handle: Arc<ConnHandle>, reg: Arc<ConnRegistry>) {
    {
        let mut w = handle.w.lock().unwrap();
        let _ = w.write_all(&encode(TYPE_WELCOME, &[]));
    }
    let mut inbuf: Vec<u8> = Vec::new();
    let mut buf = [0u8; 4096];
    while let Ok(n) = stream.read(&mut buf) {
        if n == 0 {
            break; // EOF or shutdown() at teardown
        }
        inbuf.extend_from_slice(&buf[..n]);
        while let Some((typ, payload)) = take_frame(&mut inbuf) {
            if typ == TYPE_JOIN {
                *handle.nick.lock().unwrap() = String::from_utf8_lossy(&payload).into_owned();
            } else if typ == TYPE_MSG {
                reg.messages.fetch_add(1, Ordering::Relaxed);
                let nick = handle.nick.lock().unwrap().clone();
                reg.broadcast(&encode(TYPE_DELIVER, &deliver_payload(&nick, &payload)));
            }
        }
    }
    reg.remove(handle.id);
}

fn serve_threaded(port: u16) -> Result<()> {
    let listener = StdTcpListener::bind(("127.0.0.1", port)).context("listen")?;
    let bound = listener.local_addr().context("getsockname")?.port();
    let sigfd = block_signals()?;
    let sig_raw = sigfd.as_raw_fd();
    eprintln!("chatterd: serving engine=threaded on 127.0.0.1:{bound}");

    let reg = Arc::new(ConnRegistry::new());
    let mut workers: Vec<JoinHandle<()>> = Vec::new();
    let mut next_id: u64 = 0;

    'accept: loop {
        let sig_borrow = unsafe { BorrowedFd::borrow_raw(sig_raw) };
        let mut fds = [
            PollFd::new(listener.as_fd(), PollFlags::POLLIN),
            PollFd::new(sig_borrow, PollFlags::POLLIN),
        ];
        match poll(&mut fds, PollTimeout::NONE) {
            Ok(_) => {}
            Err(nix::errno::Errno::EINTR) => continue,
            Err(e) => return Err(e).context("poll"),
        }
        let sig_ready = fds[1].revents().is_some_and(|r| r.contains(PollFlags::POLLIN));
        let listen_ready = fds[0].revents().is_some_and(|r| r.contains(PollFlags::POLLIN));
        if sig_ready {
            break 'accept; // SIGINT/SIGTERM
        }
        if listen_ready {
            let (stream, _) = match listener.accept() {
                Ok(pair) => pair,
                Err(_) => continue,
            };
            let wclone = stream.try_clone().context("try_clone")?;
            let id = next_id;
            next_id += 1;
            let handle = Arc::new(ConnHandle {
                id,
                w: Mutex::new(wclone),
                nick: Mutex::new("?".into()),
            });
            reg.add(handle.clone());
            let reg2 = reg.clone();
            workers.push(thread::spawn(move || serve_conn(stream, handle, reg2)));
        }
    }

    reg.shutdown_all();
    for w in workers {
        let _ = w.join();
    }
    eprintln!(
        "chatterd: stopped engine=threaded messages={} peak_conns={}",
        reg.messages.load(Ordering::Relaxed),
        reg.peak.load(Ordering::Relaxed)
    );
    Ok(())
}

// ---------------------------------------------------------------------------
// Epoll engine — one thread, mio-driven, per-connection state machines.
// ---------------------------------------------------------------------------

struct EConn {
    stream: MioTcpStream,
    nick: String,
    inbuf: Vec<u8>,
    outbuf: Vec<u8>, // pending outbound bytes (the backpressure queue)
    want_write: bool,
    dead: bool,
}

// Append to a connection's outbound queue and try to drain it now.
fn queue(reg: &mio::Registry, conns: &mut HashMap<usize, EConn>, tok: usize, frame: &[u8]) {
    if let Some(c) = conns.get_mut(&tok) {
        c.outbuf.extend_from_slice(frame);
    }
    flush(reg, conns, tok);
}

fn flush(reg: &mio::Registry, conns: &mut HashMap<usize, EConn>, tok: usize) {
    let c = match conns.get_mut(&tok) {
        Some(c) => c,
        None => return,
    };
    while !c.outbuf.is_empty() {
        match c.stream.write(&c.outbuf) {
            Ok(0) => {
                c.dead = true;
                return;
            }
            Ok(n) => {
                c.outbuf.drain(0..n);
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => break, // buffer full: wait for WRITABLE
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(_) => {
                c.dead = true;
                return;
            }
        }
    }
    let need = !c.outbuf.is_empty();
    if need != c.want_write {
        c.want_write = need;
        let interest = if need {
            Interest::READABLE | Interest::WRITABLE
        } else {
            Interest::READABLE
        };
        let _ = reg.reregister(&mut c.stream, Token(tok), interest);
    }
}

fn broadcast(reg: &mio::Registry, conns: &mut HashMap<usize, EConn>, frame: &[u8]) {
    let toks: Vec<usize> = conns.keys().copied().collect();
    for tok in toks {
        queue(reg, conns, tok, frame);
    }
}

fn on_readable(
    reg: &mio::Registry,
    conns: &mut HashMap<usize, EConn>,
    tok: usize,
    messages: &mut u64,
) {
    let mut tmp = [0u8; 4096];
    loop {
        let res = match conns.get_mut(&tok) {
            Some(c) => c.stream.read(&mut tmp),
            None => return,
        };
        match res {
            Ok(0) => {
                conns.get_mut(&tok).unwrap().dead = true;
                break;
            }
            Ok(n) => conns.get_mut(&tok).unwrap().inbuf.extend_from_slice(&tmp[..n]),
            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == ErrorKind::Interrupted => continue,
            Err(_) => {
                conns.get_mut(&tok).unwrap().dead = true;
                break;
            }
        }
    }
    loop {
        let frame = conns.get_mut(&tok).and_then(|c| take_frame(&mut c.inbuf));
        match frame {
            None => break,
            Some((typ, payload)) => {
                if typ == TYPE_JOIN {
                    if let Some(c) = conns.get_mut(&tok) {
                        c.nick = String::from_utf8_lossy(&payload).into_owned();
                    }
                } else if typ == TYPE_MSG {
                    *messages += 1;
                    let nick = conns.get(&tok).map(|c| c.nick.clone()).unwrap_or_else(|| "?".into());
                    let frame = encode(TYPE_DELIVER, &deliver_payload(&nick, &payload));
                    broadcast(reg, conns, &frame);
                }
            }
        }
    }
}

fn accept_ready(
    reg: &mio::Registry,
    conns: &mut HashMap<usize, EConn>,
    listener: &mut MioTcpListener,
    next_tok: &mut usize,
    peak: &mut usize,
) {
    loop {
        match listener.accept() {
            Ok((mut stream, _)) => {
                let tok = *next_tok;
                *next_tok += 1;
                if reg.register(&mut stream, Token(tok), Interest::READABLE).is_err() {
                    continue;
                }
                conns.insert(
                    tok,
                    EConn {
                        stream,
                        nick: "?".into(),
                        inbuf: Vec::new(),
                        outbuf: Vec::new(),
                        want_write: false,
                        dead: false,
                    },
                );
                if conns.len() > *peak {
                    *peak = conns.len();
                }
                queue(reg, conns, tok, &encode(TYPE_WELCOME, &[]));
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
            Err(_) => break,
        }
    }
}

fn reap(reg: &mio::Registry, conns: &mut HashMap<usize, EConn>) {
    let dead: Vec<usize> = conns.iter().filter(|(_, c)| c.dead).map(|(t, _)| *t).collect();
    for tok in dead {
        if let Some(mut c) = conns.remove(&tok) {
            let _ = reg.deregister(&mut c.stream);
        }
    }
}

fn serve_epoll(port: u16) -> Result<()> {
    let mut poll_inst = Poll::new().context("epoll_create1")?;
    let reg = poll_inst.registry().try_clone().context("dup epoll")?;

    let addr = format!("127.0.0.1:{port}").parse().expect("valid loopback addr");
    let mut listener = MioTcpListener::bind(addr).context("listen")?;
    let bound = listener.local_addr().context("getsockname")?.port();
    reg.register(&mut listener, Token(0), Interest::READABLE).context("epoll_ctl")?;

    let sigfd = block_signals()?;
    let sig_raw = sigfd.as_raw_fd();
    reg.register(&mut SourceFd(&sig_raw), Token(1), Interest::READABLE).context("epoll_ctl")?;

    eprintln!("chatterd: serving engine=epoll on 127.0.0.1:{bound}");

    let mut conns: HashMap<usize, EConn> = HashMap::new();
    let mut next_tok: usize = 2;
    let mut peak: usize = 0;
    let mut messages: u64 = 0;
    let mut events = Events::with_capacity(1024);

    loop {
        if let Err(e) = poll_inst.poll(&mut events, None) {
            if e.kind() == ErrorKind::Interrupted {
                continue;
            }
            return Err(e).context("epoll_wait");
        }
        for event in events.iter() {
            match event.token().0 {
                0 => accept_ready(&reg, &mut conns, &mut listener, &mut next_tok, &mut peak),
                1 => {
                    reap(&reg, &mut conns);
                    eprintln!(
                        "chatterd: stopped engine=epoll messages={messages} peak_conns={peak}"
                    );
                    return Ok(());
                }
                tok => {
                    if !conns.contains_key(&tok) {
                        continue; // reaped earlier in this batch
                    }
                    if (event.is_read_closed() || event.is_error())
                        && let Some(c) = conns.get_mut(&tok)
                    {
                        c.dead = true;
                    }
                    if event.is_readable() {
                        on_readable(&reg, &mut conns, tok, &mut messages);
                    }
                    if conns.contains_key(&tok) && event.is_writable() {
                        flush(&reg, &mut conns, tok);
                    }
                }
            }
        }
        reap(&reg, &mut conns);
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

fn connect_to(port: u16) -> Result<StdTcpStream> {
    StdTcpStream::connect(("127.0.0.1", port))
        .with_context(|| format!("cannot connect to 127.0.0.1:{port}"))
}

// Blocking frame reader over a std stream, buffering partial reads.
struct FrameStream {
    stream: StdTcpStream,
    buf: Vec<u8>,
    tmp: [u8; 4096],
}

impl FrameStream {
    fn new(stream: StdTcpStream) -> Self {
        Self { stream, buf: Vec::new(), tmp: [0u8; 4096] }
    }
    // Ok(Some(frame)); Ok(None) on EOF or SO_RCVTIMEO timeout.
    fn next(&mut self) -> std::io::Result<Option<(u8, Vec<u8>)>> {
        loop {
            if let Some(f) = take_frame(&mut self.buf) {
                return Ok(Some(f));
            }
            match self.stream.read(&mut self.tmp) {
                Ok(0) => return Ok(None),
                Ok(n) => self.buf.extend_from_slice(&self.tmp[..n]),
                Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {
                    return Ok(None);
                }
                Err(e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            }
        }
    }
}

fn cmd_send(port: u16, nick: &str, text: &str) -> Result<ExitCode> {
    let stream = connect_to(port)?;
    stream.set_nodelay(true).ok();
    let wstream = stream.try_clone().context("try_clone")?;
    let mut w = wstream;
    w.write_all(&encode(TYPE_JOIN, nick.as_bytes())).context("send")?;
    w.write_all(&encode(TYPE_MSG, text.as_bytes())).context("send")?;
    let mut fs = FrameStream::new(stream);
    while let Some((typ, payload)) = fs.next().context("recv")? {
        if typ != TYPE_DELIVER {
            continue; // skip WELCOME
        }
        let (dn, dt) = split_deliver(&payload);
        println!("{dn}: {dt}");
        if dn == nick && dt == text {
            return Ok(ExitCode::SUCCESS); // saw our own message echoed back
        }
    }
    Ok(ExitCode::SUCCESS)
}

fn cmd_listen(port: u16, nick: &str, count: usize) -> Result<ExitCode> {
    let stream = connect_to(port)?;
    stream.set_read_timeout(Some(Duration::from_secs(10))).ok();
    let mut w = stream.try_clone().context("try_clone")?;
    w.write_all(&encode(TYPE_JOIN, nick.as_bytes())).context("send")?;
    let mut fs = FrameStream::new(stream);
    let mut got = 0usize;
    while got < count {
        match fs.next().context("recv")? {
            None => {
                eprintln!("chatctl: timed out after {got} of {count} messages");
                return Ok(ExitCode::from(1));
            }
            Some((typ, payload)) => {
                if typ != TYPE_DELIVER {
                    continue;
                }
                let (dn, dt) = split_deliver(&payload);
                println!("{dn}: {dt}");
                got += 1;
            }
        }
    }
    Ok(ExitCode::SUCCESS)
}

fn cmd_flood(port: u16, n: usize, text: &str) -> Result<ExitCode> {
    let joined = Arc::new(AtomicUsize::new(0));
    let delivered = Arc::new(AtomicUsize::new(0));
    let go = Arc::new(AtomicBool::new(false));
    let mut handles: Vec<JoinHandle<()>> = Vec::with_capacity(n);

    for k in 0..n {
        let joined = joined.clone();
        let delivered = delivered.clone();
        let go = go.clone();
        let text = text.to_string();
        handles.push(thread::spawn(move || {
            let stream = match connect_to(port) {
                Ok(s) => s,
                Err(_) => return,
            };
            stream.set_read_timeout(Some(Duration::from_secs(10))).ok();
            let mut w = match stream.try_clone() {
                Ok(s) => s,
                Err(_) => return,
            };
            if w.write_all(&encode(TYPE_JOIN, format!("flooder{k}").as_bytes())).is_err() {
                return;
            }
            let mut fs = FrameStream::new(stream);
            // Wait for the WELCOME: proof the server has us in its fan-out set.
            loop {
                match fs.next() {
                    Ok(Some((typ, _))) if typ == TYPE_WELCOME => break,
                    Ok(Some(_)) => continue,
                    _ => return,
                }
            }
            joined.fetch_add(1, Ordering::Relaxed);
            while !go.load(Ordering::Acquire) {
                thread::sleep(Duration::from_millis(1));
            }
            if k == 0 {
                let _ = w.write_all(&encode(TYPE_MSG, text.as_bytes()));
            }
            // Exactly one broadcast is expected — conn0's message reaches all.
            loop {
                match fs.next() {
                    Ok(Some((typ, _))) if typ == TYPE_DELIVER => {
                        delivered.fetch_add(1, Ordering::Relaxed);
                        return;
                    }
                    Ok(Some(_)) => continue,
                    _ => return,
                }
            }
        }));
    }

    // Wait for every connection to be welcomed before releasing the sender.
    for _ in 0..5000 {
        if joined.load(Ordering::Relaxed) >= n {
            break;
        }
        thread::sleep(Duration::from_millis(1));
    }
    go.store(true, Ordering::Release);
    for h in handles {
        let _ = h.join();
    }

    let j = joined.load(Ordering::Relaxed);
    let d = delivered.load(Ordering::Relaxed);
    println!("flood: connected {j}");
    println!("flood: delivered {d}");
    if j == n && d == n {
        Ok(ExitCode::SUCCESS)
    } else {
        Ok(ExitCode::from(1))
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn usage() -> ExitCode {
    eprintln!("usage: chatterd <command>");
    eprintln!("  serve --engine threaded|epoll --port P   run the chat daemon");
    eprintln!("  send  --port P NICK TEXT                 join, broadcast, print echoes");
    eprintln!("  listen --port P NICK --count N           join, print N delivered lines");
    eprintln!("  flood --port P N [--text TEXT]           open N conns, broadcast, count");
    ExitCode::from(2)
}

fn flag(args: &[String], name: &str) -> Option<String> {
    args.windows(2).find(|w| w[0] == name).map(|w| w[1].clone())
}

fn parse_port(s: &str) -> Option<u16> {
    s.parse::<u32>().ok().filter(|p| *p <= 65535).map(|p| p as u16)
}

fn run() -> Result<ExitCode> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        return Ok(usage());
    }
    match args[0].as_str() {
        "serve" => {
            let engine = flag(&args, "--engine");
            let port = flag(&args, "--port").as_deref().and_then(parse_port);
            let (engine, port) = match (engine, port) {
                (Some(e), Some(p)) => (e, p),
                _ => return Ok(usage()),
            };
            let result = match engine.as_str() {
                "threaded" => serve_threaded(port),
                "epoll" => serve_epoll(port),
                _ => return Ok(usage()),
            };
            match result {
                Ok(()) => Ok(ExitCode::SUCCESS),
                Err(e) => {
                    eprintln!("chatterd: {e:#}");
                    Ok(ExitCode::from(1))
                }
            }
        }
        "send" if args.len() == 5 && args[1] == "--port" => {
            let port = match parse_port(&args[2]) {
                Some(p) => p,
                None => return Ok(usage()),
            };
            cmd_send(port, &args[3], &args[4])
        }
        "listen" if args.len() == 6 && args[1] == "--port" && args[4] == "--count" => {
            let port = match parse_port(&args[2]) {
                Some(p) => p,
                None => return Ok(usage()),
            };
            match args[5].parse::<usize>() {
                Ok(c) if c > 0 => cmd_listen(port, &args[3], c),
                _ => Ok(usage()),
            }
        }
        "flood" if args.len() >= 4 && args[1] == "--port" => {
            let port = match parse_port(&args[2]) {
                Some(p) => p,
                None => return Ok(usage()),
            };
            let text = flag(&args, "--text").unwrap_or_else(|| "flood".into());
            match args[3].parse::<usize>() {
                Ok(n) if n > 0 => cmd_flood(port, n, &text),
                _ => Ok(usage()),
            }
        }
        _ => Ok(usage()),
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => code,
        Err(e) => {
            eprintln!("chatctl: {e:#}");
            ExitCode::from(1)
        }
    }
}
