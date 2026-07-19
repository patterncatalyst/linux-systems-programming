// chatterd v2 (Rust 2024) — UDP multicast peer discovery + a TCP chat
// exchange. Grows the same chatterd program introduced in chapter 21: the TCP
// frame is the canonical chatterd chat frame (2-byte magic "CH" + 1-byte
// version + 1-byte type + 2-byte big-endian length + UTF-8 payload) shared by
// every chatterd version (ch21-ch27). The UDP discovery beacon below is a
// completely separate wire object — a plain ASCII datagram, no relation to
// the chat frame's magic/version/type/length header.
//
//   chatterd discover --group 239.7.7.7 --port 51888 --name alice \
//       --tcp-port 9101 --iface 127.0.0.1 [--announce-ms 200] [--rounds 10]
//
// The multicast socket is built with the socket2 crate (IP_ADD_MEMBERSHIP /
// IP_MULTICAST_IF / IP_MULTICAST_LOOP), then converted into a std UdpSocket
// (an OwnedFd) shared across threads. Fallible paths use Result/?; the accept
// and receive loops run on std threads coordinated by an AtomicBool.

use std::collections::HashSet;
use std::error::Error;
use std::io::{self, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener, TcpStream, UdpSocket};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

use socket2::{Domain, Protocol, SockAddr, Socket, Type};

const BEACON_MAGIC: &str = "CHATTERD1"; // UDP beacon tag only

// --- canonical chatterd chat frame (TCP) -----------------------------------
// magic 'C' 'H', version 0x01, type (1 byte), length (2 bytes big-endian,
// 0..65535), payload (UTF-8, type-specific). This header is byte-identical
// across every chatterd version; ch23's post-discovery greeting uses only
// JOIN (announce the dialer's name) and DELIVER (name + NUL + text, the
// listener's reply) — the same types chapters 21/22/24/27 share.
const FRAME_MAGIC: [u8; 2] = [0x43, 0x48]; // "CH"
const FRAME_VERSION: u8 = 0x01;
const FRAME_JOIN: u8 = 1;
#[allow(dead_code)]
const FRAME_MSG: u8 = 2;
const FRAME_DELIVER: u8 = 3;
#[allow(dead_code)]
const FRAME_WELCOME: u8 = 4;
#[allow(dead_code)]
const FRAME_PING: u8 = 5;
const MAX_FRAME_PAYLOAD: usize = 0xFFFF;

type Fallible<T> = Result<T, Box<dyn Error + Send + Sync>>;

#[derive(Clone)]
struct Config {
    group: Ipv4Addr,
    port: u16,
    name: String,
    tcp_port: u16,
    iface: Ipv4Addr,
    announce_ms: u64,
    rounds: u32,
}

fn usage() {
    eprintln!(
        "usage: chatterd discover --group <ip> --port <n> --name <s> \
[--tcp-port <n>] [--iface <ip>] [--announce-ms <n>] [--rounds <n>]"
    );
}

fn parse_port(s: &str) -> Result<u16, i32> {
    s.parse::<u16>().ok().filter(|&n| n >= 1).ok_or_else(|| {
        usage();
        2
    })
}

/// Parse argv. On any error prints usage and returns Err(exit_code).
fn parse_args() -> Result<Config, i32> {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() < 2 || argv[1] != "discover" {
        usage();
        return Err(2);
    }
    let mut group: Option<Ipv4Addr> = None;
    let mut port: Option<u16> = None;
    let mut name: Option<String> = None;
    let mut tcp_port: u16 = 9101;
    let mut iface: Ipv4Addr = Ipv4Addr::LOCALHOST;
    let mut announce_ms: u64 = 200;
    let mut rounds: u32 = 10;

    let args = &argv[2..];
    let take = |i: &mut usize, flag: &str| -> Result<String, i32> {
        if *i + 1 >= args.len() {
            eprintln!("chatterd: {flag} needs a value");
            return Err(2);
        }
        *i += 1;
        Ok(args[*i].clone())
    };
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "--group" => {
                let v = take(&mut i, a)?;
                group = Some(v.parse().map_err(|_| {
                    usage();
                    2
                })?);
            }
            "--name" => name = Some(take(&mut i, a)?),
            "--iface" => {
                let v = take(&mut i, a)?;
                iface = v.parse().map_err(|_| {
                    usage();
                    2
                })?;
            }
            "--port" => {
                let v = take(&mut i, a)?;
                port = Some(parse_port(&v)?);
            }
            "--tcp-port" => {
                let v = take(&mut i, a)?;
                tcp_port = parse_port(&v)?;
            }
            "--announce-ms" => {
                let v = take(&mut i, a)?;
                announce_ms = v.parse::<u64>().ok().filter(|&n| n > 0).ok_or_else(|| {
                    usage();
                    2
                })?;
            }
            "--rounds" => {
                let v = take(&mut i, a)?;
                rounds = v.parse::<u32>().ok().filter(|&n| n > 0).ok_or_else(|| {
                    usage();
                    2
                })?;
            }
            _ => {
                eprintln!("chatterd: unknown argument: {a}");
                usage();
                return Err(2);
            }
        }
        i += 1;
    }

    match (group, port, name) {
        (Some(group), Some(port), Some(name)) => Ok(Config {
            group,
            port,
            name,
            tcp_port,
            iface,
            announce_ms,
            rounds,
        }),
        _ => {
            usage();
            Err(2)
        }
    }
}

// --- canonical chatterd chat frame I/O --------------------------------------

fn write_frame(w: &mut impl Write, typ: u8, payload: &[u8]) -> io::Result<()> {
    if payload.len() > MAX_FRAME_PAYLOAD {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("frame payload too large: {}", payload.len()),
        ));
    }
    let mut hdr = [0u8; 6];
    hdr[0] = FRAME_MAGIC[0];
    hdr[1] = FRAME_MAGIC[1];
    hdr[2] = FRAME_VERSION;
    hdr[3] = typ;
    hdr[4] = (payload.len() >> 8) as u8;
    hdr[5] = (payload.len() & 0xFF) as u8;
    w.write_all(&hdr)?;
    w.write_all(payload)?;
    Ok(())
}

fn read_frame(r: &mut impl Read) -> io::Result<(u8, Vec<u8>)> {
    let mut hdr = [0u8; 6];
    r.read_exact(&mut hdr)?;
    if hdr[0] != FRAME_MAGIC[0] || hdr[1] != FRAME_MAGIC[1] {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad frame magic"));
    }
    if hdr[2] != FRAME_VERSION {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("unsupported frame version: {}", hdr[2]),
        ));
    }
    let typ = hdr[3];
    let n = ((hdr[4] as usize) << 8) | hdr[5] as usize;
    let mut body = vec![0u8; n];
    r.read_exact(&mut body)?;
    Ok((typ, body))
}

/// Build the multicast UDP socket and hand back a std UdpSocket (an OwnedFd).
fn make_multicast(cfg: &Config) -> Fallible<UdpSocket> {
    let sock = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
    sock.set_reuse_address(true)?;
    let bind = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, cfg.port);
    sock.bind(&SockAddr::from(bind))?;
    sock.join_multicast_v4(&cfg.group, &cfg.iface)?;
    sock.set_multicast_if_v4(&cfg.iface)?;
    sock.set_multicast_loop_v4(true)?;
    sock.set_multicast_ttl_v4(1)?;
    sock.set_read_timeout(Some(Duration::from_millis(200)))?;
    Ok(sock.into())
}

fn dial(ip: Ipv4Addr, port: u16) -> Fallible<TcpStream> {
    let addr = SocketAddr::from((ip, port));
    // Retry: the peer's beacon may arrive a hair before its listener is ready.
    for _ in 0..40 {
        if let Ok(s) = TcpStream::connect_timeout(&addr, Duration::from_millis(500)) {
            return Ok(s);
        }
        thread::sleep(Duration::from_millis(50));
    }
    Err(format!("connect {addr}: giving up").into())
}

fn accept_loop(listener: TcpListener, deliver_payload: Vec<u8>, stop: Arc<AtomicBool>) {
    listener.set_nonblocking(true).ok();
    while !stop.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((mut conn, _)) => {
                // Read the dialer's JOIN frame, reply with our DELIVER frame
                // (our name + NUL + greeting text), close.
                if read_frame(&mut conn).is_ok() {
                    let _ = write_frame(&mut conn, FRAME_DELIVER, &deliver_payload);
                }
            }
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(50));
            }
            Err(_) => thread::sleep(Duration::from_millis(50)),
        }
    }
}

fn recv_loop(udp: Arc<UdpSocket>, cfg: Config, stop: Arc<AtomicBool>) {
    let mut seen: HashSet<String> = HashSet::new();
    let mut buf = [0u8; 2048];
    while !stop.load(Ordering::Relaxed) {
        let n = match udp.recv_from(&mut buf) {
            Ok((n, _)) => n,
            Err(ref e)
                if e.kind() == io::ErrorKind::WouldBlock
                    || e.kind() == io::ErrorKind::TimedOut =>
            {
                continue;
            }
            Err(_) => continue,
        };
        let msg = String::from_utf8_lossy(&buf[..n]);
        let tok: Vec<&str> = msg.split_whitespace().collect();
        if tok.len() != 4 || tok[0] != BEACON_MAGIC {
            continue;
        }
        let pname = tok[1];
        if pname == cfg.name || seen.contains(pname) {
            continue; // our own beacon or already known
        }
        let (pport, pip) = match (tok[2].parse::<u16>(), tok[3].parse::<Ipv4Addr>()) {
            (Ok(p), Ok(ip)) if p >= 1 => (p, ip),
            _ => continue,
        };
        seen.insert(pname.to_string());

        println!("discovered peer {pname} at {pip}:{pport}");
        let mut conn = match dial(pip, pport) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("chatterd: error: dial {pname}: {e}");
                continue;
            }
        };
        // JOIN (announce ourselves), then read the peer's DELIVER reply.
        if let Err(e) = write_frame(&mut conn, FRAME_JOIN, cfg.name.as_bytes()) {
            eprintln!("chatterd: error: {e}");
            continue;
        }
        let (_typ, payload) = match read_frame(&mut conn) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("chatterd: error: {e}");
                continue;
            }
        };
        let reply = String::from_utf8_lossy(&payload);
        let (rname, rtext) = reply.split_once('\0').unwrap_or((reply.as_ref(), ""));
        println!("peer {rname} says: {rtext}");
    }
}

fn discover(cfg: &Config) -> Fallible<()> {
    let listener = TcpListener::bind((cfg.iface, cfg.tcp_port))
        .map_err(|e| format!("listen tcp {}:{}: {e}", cfg.iface, cfg.tcp_port))?;
    let udp = Arc::new(make_multicast(cfg)?);

    eprintln!(
        "chatterd: announcing as {} on {}:{} (tcp {}:{})",
        cfg.name, cfg.group, cfg.port, cfg.iface, cfg.tcp_port
    );

    let deliver_payload = format!("{}\0hello from {}", cfg.name, cfg.name).into_bytes();
    let stop = Arc::new(AtomicBool::new(false));

    let acc = {
        let (p, s) = (deliver_payload.clone(), stop.clone());
        thread::spawn(move || accept_loop(listener, p, s))
    };
    let rcv = {
        let (u, c, s) = (udp.clone(), cfg.clone(), stop.clone());
        thread::spawn(move || recv_loop(u, c, s))
    };

    let beacon = format!("{} {} {} {}", BEACON_MAGIC, cfg.name, cfg.tcp_port, cfg.iface);
    let dst = SocketAddr::from((cfg.group, cfg.port));
    for _ in 0..cfg.rounds {
        udp.send_to(beacon.as_bytes(), dst)?;
        thread::sleep(Duration::from_millis(cfg.announce_ms));
    }
    // Grace window so final exchanges complete, then stop the loops.
    thread::sleep(Duration::from_millis(800));
    stop.store(true, Ordering::Relaxed);
    let _ = acc.join();
    let _ = rcv.join();
    Ok(())
}

fn main() {
    let code = match parse_args() {
        Ok(cfg) => match discover(&cfg) {
            Ok(()) => 0,
            Err(e) => {
                eprintln!("chatterd: error: {e}");
                1
            }
        },
        Err(code) => code,
    };
    std::process::exit(code);
}
