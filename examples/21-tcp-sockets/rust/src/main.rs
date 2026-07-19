// chatterd v0 — a thread-per-connection TCP chat room.
//
// One binary, two subcommands:
//
//   chatterd serve   --port P [--host H]
//       Accept TCP connections; each client runs on its own std::thread.
//       Every chat frame received is broadcast to all OTHER clients; a join
//       notice is broadcast to ALL clients (including the newcomer). SIGINT
//       closes the listener and exits 0.
//
//   chatterd chatctl --port P --name N [--host H]
//       Connect, announce N with a join frame, then send one frame per line
//       of stdin. A reader thread prints every received frame as
//       "<name>: <text>".
//
// THE WIRE PROTOCOL (fixed for the whole book, ch21-ch27). Every message is
// the canonical chatterd CHAT FRAME:
//
//     +-------+---------+------+----------------+------------------+
//     | magic | version | type | length (u16BE) | payload (UTF-8)  |
//     | 2B    | 1B      | 1B   | 2B             | `length` bytes   |
//     +-------+---------+------+----------------+------------------+
//
// magic is the two bytes 0x43 0x48 ("CH"); version is 0x01. `length` counts
// the payload only (0..65535), never the 6-byte header. This chapter (v0)
// uses three of the five frame types:
//
//   JOIN    (1) payload = name              client -> server, once, at connect
//   MSG     (2) payload = text               client -> server, per stdin line
//   DELIVER (3) payload = name 0x00 text      server -> all clients, a broadcast
//
// WELCOME (4) and PING (5) are reserved for later chapters (ch22, ch24); this
// program never sends them, and a chatctl reader here ignores any frame type
// it doesn't recognize so it stays interoperable with newer servers. The
// server relays a client's MSG as a DELIVER and synthesises join/leave
// DELIVER notices from the reserved sender name "server".
//
// Rust 2024: no async runtime (this chapter is about blocking sockets and
// threads). The listener is built through nix so it can carry SO_REUSEADDR and
// arrive as an OwnedFd; a SIGINT handler flips an AtomicBool the non-blocking
// accept loop polls; every connection owns a std::thread and a TcpStream.

use std::collections::HashMap;
use std::io::{BufReader, Read, Write};
use std::net::{Shutdown, SocketAddrV4, TcpListener, TcpStream};
use std::os::fd::{AsRawFd, OwnedFd};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use nix::sys::signal::{SaFlags, SigAction, SigHandler, SigSet, Signal, sigaction};
use nix::sys::socket::sockopt::ReuseAddr;
use nix::sys::socket::{
    AddressFamily, Backlog, SockFlag, SockType, SockaddrIn, bind, listen, setsockopt, socket,
};

// The canonical chatterd chat frame header: magic "CH", version 1.
const MAGIC: [u8; 2] = [0x43, 0x48];
const WIRE_VERSION: u8 = 0x01;
const HEADER_SIZE: usize = 6; // magic(2) + version(1) + type(1) + length(2)

const FRAME_JOIN: u8 = 1;
const FRAME_MSG: u8 = 2;
const FRAME_DELIVER: u8 = 3;
#[allow(dead_code)] // WELCOME is reserved for ch22; not sent or read here
const FRAME_WELCOME: u8 = 4;
#[allow(dead_code)] // PING is reserved for ch24; not sent or read here
const FRAME_PING: u8 = 5;

const BROADCAST_ALL: usize = 0; // sentinel id: no real client ever holds it

fn usage() -> i32 {
    eprintln!("usage: chatterd serve --port PORT [--host HOST]");
    eprintln!("       chatterd chatctl --port PORT --name NAME [--host HOST]");
    2
}

// ---------------------------------------------------------------------------
// The canonical chat frame: magic + version + type + u16BE length + payload.
// ---------------------------------------------------------------------------

fn build_frame(frame_type: u8, payload: &[u8]) -> Vec<u8> {
    // payload.len() is always <= 65535 for the frames this program builds
    // (names/messages typed on a terminal); the length field is u16BE.
    let mut out = Vec::with_capacity(HEADER_SIZE + payload.len());
    out.extend_from_slice(&MAGIC);
    out.push(WIRE_VERSION);
    out.push(frame_type);
    out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    out.extend_from_slice(payload);
    out
}

fn build_join(name: &str) -> Vec<u8> {
    build_frame(FRAME_JOIN, name.as_bytes())
}

fn build_msg(text: &str) -> Vec<u8> {
    build_frame(FRAME_MSG, text.as_bytes())
}

fn build_deliver(name: &str, text: &str) -> Vec<u8> {
    let mut payload = Vec::with_capacity(name.len() + 1 + text.len());
    payload.extend_from_slice(name.as_bytes());
    payload.push(0);
    payload.extend_from_slice(text.as_bytes());
    build_frame(FRAME_DELIVER, &payload)
}

/// Read one frame; `Ok(None)` marks a clean EOF, `Err` a protocol fault.
fn read_frame<R: Read>(r: &mut R) -> Result<Option<(u8, Vec<u8>)>> {
    let mut hdr = [0u8; HEADER_SIZE];
    match r.read_exact(&mut hdr) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(e) => return Err(e.into()),
    }
    if hdr[0..2] != MAGIC || hdr[2] != WIRE_VERSION {
        bail!("not a chatterd frame (bad magic/version)");
    }
    let frame_type = hdr[3];
    let len = u16::from_be_bytes([hdr[4], hdr[5]]);
    let mut payload = vec![0u8; len as usize];
    if len > 0 {
        match r.read_exact(&mut payload) {
            Ok(()) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
            Err(e) => return Err(e.into()),
        }
    }
    Ok(Some((frame_type, payload)))
}

/// Split a DELIVER payload's `name\0text` into its two parts.
fn split_deliver(payload: &[u8]) -> Option<(String, String)> {
    let sep = payload.iter().position(|&b| b == 0)?;
    let name = String::from_utf8_lossy(&payload[..sep]).into_owned();
    let text = String::from_utf8_lossy(&payload[sep + 1..]).into_owned();
    Some((name, text))
}

// ---------------------------------------------------------------------------
// The hub: every connected client, keyed by id.
// ---------------------------------------------------------------------------

struct Client {
    name: String,
    writer: TcpStream, // a clone of the accepted stream, for broadcasts
}

#[derive(Default)]
struct Hub {
    clients: Mutex<HashMap<usize, Client>>,
}

impl Hub {
    fn add(&self, id: usize, writer: TcpStream) {
        self.clients
            .lock()
            .unwrap()
            .insert(id, Client { name: String::new(), writer });
    }

    fn set_name(&self, id: usize, name: &str) {
        if let Some(c) = self.clients.lock().unwrap().get_mut(&id) {
            c.name = name.to_owned();
        }
    }

    /// Remove a client and return the name it last carried.
    fn remove(&self, id: usize) -> Option<String> {
        self.clients.lock().unwrap().remove(&id).map(|c| c.name)
    }

    /// Write `frame` to every client; `except == BROADCAST_ALL` skips nobody.
    fn broadcast(&self, frame: &[u8], except: usize) {
        let mut guard = self.clients.lock().unwrap();
        for (&id, c) in guard.iter_mut() {
            if id != except {
                let _ = c.writer.write_all(frame);
            }
        }
    }

    /// Unblock every connection's blocking read so its thread can wind down.
    fn shutdown_all(&self) {
        for c in self.clients.lock().unwrap().values() {
            let _ = c.writer.shutdown(Shutdown::Both);
        }
    }
}

/// One connection's lifetime: read frames, drive the hub, announce the leave.
fn serve_client(hub: Arc<Hub>, stream: TcpStream, id: usize) {
    let writer = match stream.try_clone() {
        Ok(w) => w,
        Err(_) => return,
    };
    hub.add(id, writer);

    let mut r = BufReader::new(stream);
    let mut my_name = String::new();
    loop {
        match read_frame(&mut r) {
            Ok(Some((FRAME_JOIN, payload))) => {
                if payload.is_empty() {
                    break; // malformed: JOIN carries no name
                }
                my_name = String::from_utf8_lossy(&payload).into_owned();
                hub.set_name(id, &my_name);
                eprintln!("chatterd: {my_name} joined");
                hub.broadcast(&build_deliver("server", &format!("{my_name} joined")), BROADCAST_ALL);
            }
            Ok(Some((FRAME_MSG, payload))) => {
                if my_name.is_empty() {
                    continue; // MSG before JOIN: nothing sensible to attribute it to
                }
                let text = String::from_utf8_lossy(&payload).into_owned();
                hub.broadcast(&build_deliver(&my_name, &text), id);
            }
            Ok(Some(_)) => continue, // DELIVER/WELCOME/PING not sent by a client; ignore
            _ => break,              // EOF or a protocol fault ends the connection
        }
    }

    if let Some(name) = hub.remove(id)
        && !name.is_empty()
    {
        eprintln!("chatterd: {name} left");
        hub.broadcast(&build_deliver("server", &format!("{name} left")), BROADCAST_ALL);
    }
}

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_sigint(_: i32) {
    STOP.store(true, Ordering::Relaxed);
}

/// Build a listening socket through nix so it carries SO_REUSEADDR and arrives
/// as an OwnedFd we hand straight to the std TcpListener.
fn listen_tcp(host: &str, port: u16) -> Result<TcpListener> {
    let addr: SocketAddrV4 = format!("{host}:{port}")
        .parse()
        .with_context(|| format!("{host}: not an IPv4 address"))?;
    let fd: OwnedFd = socket(
        AddressFamily::Inet,
        SockType::Stream,
        SockFlag::SOCK_CLOEXEC,
        None,
    )
    .context("socket")?;
    setsockopt(&fd, ReuseAddr, &true).context("SO_REUSEADDR")?;
    bind(fd.as_raw_fd(), &SockaddrIn::from(addr)).with_context(|| format!("bind {host}:{port}"))?;
    listen(&fd, Backlog::new(128).unwrap()).context("listen")?;
    Ok(TcpListener::from(fd))
}

fn install_sigint() -> Result<()> {
    let action = SigAction::new(
        SigHandler::Handler(on_sigint),
        SaFlags::empty(),
        SigSet::empty(),
    );
    // SAFETY: on_sigint only stores into a static AtomicBool, which is
    // async-signal-safe.
    unsafe { sigaction(Signal::SIGINT, &action) }.context("sigaction")?;
    Ok(())
}

fn cmd_serve(host: &str, port: u16) -> Result<()> {
    let listener = listen_tcp(host, port)?;
    listener.set_nonblocking(true).context("set_nonblocking")?;
    install_sigint()?;
    eprintln!("chatterd: listening on {host}:{port}");

    let hub = Arc::new(Hub::default());
    let mut handles: Vec<JoinHandle<()>> = Vec::new();
    let mut next_id: usize = BROADCAST_ALL; // first real id is 1

    while !STOP.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((conn, _)) => {
                conn.set_nonblocking(false).context("client set_blocking")?;
                next_id += 1;
                let id = next_id;
                let hub = Arc::clone(&hub);
                handles.push(thread::spawn(move || serve_client(hub, conn, id)));
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(20));
            }
            Err(e) => return Err(anyhow!("accept: {e}")),
        }
    }

    eprintln!("chatterd: shutting down");
    hub.shutdown_all(); // unblock every reader
    for h in handles {
        let _ = h.join();
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// chatctl
// ---------------------------------------------------------------------------

fn cmd_chatctl(host: &str, port: u16, name: &str) -> Result<()> {
    let mut stream =
        TcpStream::connect((host, port)).with_context(|| format!("connect {host}:{port}"))?;

    // Announce ourselves: a JOIN frame's payload is just our name.
    stream
        .write_all(&build_join(name))
        .with_context(|| format!("write {host}:{port}"))?;

    let reader_stream = stream.try_clone().context("clone stream")?;
    let reader = thread::spawn(move || {
        let mut r = BufReader::new(reader_stream);
        while let Ok(Some((frame_type, payload))) = read_frame(&mut r) {
            if frame_type != FRAME_DELIVER {
                continue; // WELCOME/PING/etc. are for later chapters; ignore them
            }
            if let Some((n, t)) = split_deliver(&payload) {
                println!("{n}: {t}");
            }
        }
    });

    let stdin = std::io::stdin();
    let mut line = String::new();
    loop {
        line.clear();
        match stdin.read_line(&mut line) {
            Ok(0) => break, // EOF
            Ok(_) => {
                let msg = line.trim_end_matches('\n');
                if msg.is_empty() {
                    continue; // never emit an empty-text frame; that reads as a join
                }
                if stream.write_all(&build_msg(msg)).is_err() {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    // stdin EOF: close the socket so the reader sees EOF, then join it so every
    // received line is flushed before we exit.
    let _ = stream.shutdown(Shutdown::Both);
    let _ = reader.join();
    Ok(())
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn main() -> std::process::ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(sub) = args.first().cloned() else {
        return std::process::ExitCode::from(usage() as u8);
    };
    if sub != "serve" && sub != "chatctl" {
        return std::process::ExitCode::from(usage() as u8);
    }

    let mut host = String::from("127.0.0.1");
    let mut port_str = String::new();
    let mut name = String::new();
    let mut i = 1;
    while i < args.len() {
        if args[i] == "--port" && i + 1 < args.len() {
            port_str = args[i + 1].clone();
            i += 2;
        } else if args[i] == "--host" && i + 1 < args.len() {
            host = args[i + 1].clone();
            i += 2;
        } else if args[i] == "--name" && i + 1 < args.len() {
            name = args[i + 1].clone();
            i += 2;
        } else {
            return std::process::ExitCode::from(usage() as u8);
        }
    }
    let Ok(port) = port_str.parse::<u16>() else {
        return std::process::ExitCode::from(usage() as u8);
    };
    if port == 0 {
        return std::process::ExitCode::from(usage() as u8);
    }

    if sub == "serve" {
        if let Err(err) = cmd_serve(&host, port) {
            eprintln!("chatterd: error: {err:#}");
            return std::process::ExitCode::from(1);
        }
    } else {
        if name.trim().is_empty() {
            return std::process::ExitCode::from(usage() as u8);
        }
        if let Err(err) = cmd_chatctl(&host, port, &name) {
            eprintln!("chatctl: error: {err:#}");
            return std::process::ExitCode::from(1);
        }
    }
    std::process::ExitCode::SUCCESS
}
