// chatterd.rs — the book's recurring peer-to-peer chat daemon (ch21-27),
// reduced to what the capstone needs: serve local clients, and (new in this
// chapter) bridge two chatterd instances across hosts so a message sent on
// one node is delivered to clients on the other. The wire frame (proto.rs)
// is unchanged from every prior chatterd chapter.
//
// Bridging design (identical to go/chatterd.go's and chatterd.cpp's): a
// bridge worker is just an ordinary client of the *other* node's server
// (nick "bridge@<local-node>"). Concretely, with --peer set on both sides:
//
//   target dials peer, joins as "bridge@target"   (this connection lives in
//                                                   peer's client registry)
//   peer   dials target, joins as "bridge@peer"   (lives in target's registry)
//
// A local MSG is broadcast as DELIVER to every registered connection
// (including a bridge connection dialed in from the other side) exactly like
// any other client — no protocol change. When a bridge *worker* itself
// receives a DELIVER over the connection it dialed, it re-broadcasts locally
// with include_bridges=false, so the message never goes back out over any
// bridge: no ping-pong between two nodes.
//
// Raw fds via `nix` rather than std::net, following ex40's fastpath and
// chatterd.cpp's own RAII-Fd approach: a poll(2)-with-timeout accept loop
// (so the process can check the shutdown flag between accepts) instead of
// closing a blocked accept() from another thread, which std::net cannot do
// safely. OwnedFd is the RAII socket — closes on drop, move-only, no
// hand-written destructor needed.
use std::net::{Ipv4Addr, SocketAddrV4};
use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use nix::errno::Errno;
use nix::fcntl::{FcntlArg, OFlag, fcntl};
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::socket::{
    AddressFamily, Backlog, SockFlag, SockType, SockaddrIn, accept4, bind, connect, getsockopt,
    listen as listen_syscall, setsockopt, socket, sockopt,
};
use nix::sys::time::{TimeVal, TimeValLike};

use crate::proto::{self, Frame};
use crate::telemetry::{self, Telemetry};
use crate::util;

// ---------------------------------------------------------------------------
// TCP helpers — IPv4-literal only, matching chatterd.cpp's inet_pton-based
// listen_tcp/connect_tcp (every caller in this example, verify.lua included,
// only ever passes literal addresses, never a hostname to resolve).
// ---------------------------------------------------------------------------

fn listen_tcp(host: &str, port: u16) -> Result<OwnedFd, String> {
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
    let _ = setsockopt(&sock, sockopt::ReuseAddr, &true);
    let addr = SockaddrIn::from(SocketAddrV4::new(ip, port));
    bind(sock.as_raw_fd(), &addr).map_err(|e| format!("bind: {e}"))?;
    listen_syscall(
        &sock,
        Backlog::new(16).map_err(|e| format!("backlog: {e}"))?,
    )
    .map_err(|e| format!("listen: {e}"))?;
    Ok(sock)
}

// Non-blocking connect + poll(2), the syscall-level equivalent of Go's
// net.DialTimeout.
fn connect_tcp(host: &str, port: u16, timeout_ms: i64) -> Result<OwnedFd, String> {
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

    let orig_flags =
        fcntl(sock.as_fd(), FcntlArg::F_GETFL).map_err(|e| format!("fcntl(F_GETFL): {e}"))?;
    let mut nb_flags = OFlag::from_bits_truncate(orig_flags);
    nb_flags.insert(OFlag::O_NONBLOCK);
    fcntl(sock.as_fd(), FcntlArg::F_SETFL(nb_flags)).map_err(|e| format!("fcntl(F_SETFL): {e}"))?;

    match connect(sock.as_raw_fd(), &addr) {
        Ok(()) => {}
        Err(Errno::EINPROGRESS) => {
            let mut fds = [PollFd::new(sock.as_fd(), PollFlags::POLLOUT)];
            let timeout =
                PollTimeout::try_from(timeout_ms.max(0) as u64).unwrap_or(PollTimeout::MAX);
            let n = poll(&mut fds, timeout).map_err(|e| format!("poll: {e}"))?;
            if n <= 0 {
                return Err("i/o timeout".to_string());
            }
            let soerr: i32 =
                getsockopt(&sock, sockopt::SocketError).map_err(|e| format!("getsockopt: {e}"))?;
            if soerr != 0 {
                return Err(std::io::Error::from_raw_os_error(soerr).to_string());
            }
        }
        Err(e) => return Err(format!("connect {host}:{port}: {e}")),
    }
    // Restore blocking mode.
    fcntl(
        sock.as_fd(),
        FcntlArg::F_SETFL(OFlag::from_bits_truncate(orig_flags)),
    )
    .map_err(|e| format!("fcntl(F_SETFL restore): {e}"))?;
    Ok(sock)
}

// ---------------------------------------------------------------------------
// Hub — the registry of every connection (local client or bridge worker)
// currently attached to this server.
// ---------------------------------------------------------------------------

struct PeerConn {
    fd: OwnedFd,
    nick: String,
    is_bridge: bool,
    wmu: Mutex<()>,
}

struct Hub {
    clients: Mutex<Vec<Arc<PeerConn>>>,
}

impl Hub {
    fn new() -> Self {
        Hub {
            clients: Mutex::new(Vec::new()),
        }
    }

    fn add(&self, c: Arc<PeerConn>) {
        self.clients.lock().unwrap().push(c);
    }

    fn remove(&self, c: &Arc<PeerConn>) {
        let mut g = self.clients.lock().unwrap();
        g.retain(|x| !Arc::ptr_eq(x, c));
    }

    fn broadcast_deliver(
        &self,
        exclude: Option<&Arc<PeerConn>>,
        include_bridges: bool,
        nick: &str,
        text: &str,
    ) {
        let targets: Vec<Arc<PeerConn>> = {
            let g = self.clients.lock().unwrap();
            g.iter()
                .filter(|c| exclude.map(|e| !Arc::ptr_eq(c, e)).unwrap_or(true))
                .filter(|c| include_bridges || !c.is_bridge)
                .cloned()
                .collect()
        };
        let payload = proto::deliver_payload(nick, text);
        for c in targets {
            let _wg = c.wmu.lock().unwrap();
            let f = Frame {
                typ: proto::TYPE_DELIVER,
                payload: payload.clone(),
            };
            let _ = proto::write_frame(c.fd.as_raw_fd(), &f); // best-effort, matches Go's `_ = c.send(f)`
        }
    }
}

fn handle_client(fd: OwnedFd, hub: Arc<Hub>, node: String, tel: Arc<Telemetry>) {
    let raw: RawFd = fd.as_raw_fd();
    let first = match proto::read_frame(raw) {
        Ok(f) if f.typ == proto::TYPE_JOIN => f,
        _ => return, // fd (owned locally) closes on drop
    };
    let nick = String::from_utf8_lossy(&first.payload).into_owned();
    let is_bridge = nick.starts_with("bridge@");
    let pc = Arc::new(PeerConn {
        fd,
        nick,
        is_bridge,
        wmu: Mutex::new(()),
    });
    hub.add(pc.clone());

    while let Ok(f) = proto::read_frame(raw) {
        if f.typ != proto::TYPE_MSG {
            continue;
        }
        let text = String::from_utf8_lossy(&f.payload).into_owned();

        match telemetry::start_deliver_span(&tel, &pc.nick, text.len(), &node, pc.is_bridge) {
            Some((span, trace_id)) => {
                hub.broadcast_deliver(Some(&pc), true, &pc.nick, &text);
                span.end_ok();
                if tel.enabled {
                    println!("chatterd: trace_id={trace_id} node={node} from={}", pc.nick);
                }
            }
            None => hub.broadcast_deliver(Some(&pc), true, &pc.nick, &text),
        }
    }
    hub.remove(&pc);
}

fn bridge_worker(
    peer_addr: String,
    local_node: String,
    peer_node: String,
    hub: Arc<Hub>,
    sig: &'static AtomicBool,
) {
    let Some((host, port_str)) = peer_addr.rsplit_once(':') else {
        return;
    };
    let Ok(port) = port_str.parse::<u16>() else {
        return;
    };
    let backoff = Duration::from_millis(300);

    while !sig.load(Ordering::Relaxed) {
        let conn = match connect_tcp(host, port, 3000) {
            Ok(c) => c,
            Err(_) => {
                thread::sleep(backoff);
                continue;
            }
        };
        let raw = conn.as_raw_fd();
        let bridge_nick = format!("bridge@{local_node}");
        let joinf = Frame {
            typ: proto::TYPE_JOIN,
            payload: bridge_nick.clone().into_bytes(),
        };
        if proto::write_frame(raw, &joinf).is_err() {
            thread::sleep(backoff);
            continue;
        }
        eprintln!("chatterd: bridge connected peer={peer_addr} as={bridge_nick}");

        while let Ok(f) = proto::read_frame(raw) {
            if f.typ != proto::TYPE_DELIVER {
                continue;
            }
            let Some((mut nick, text)) = proto::split_deliver(&f.payload) else {
                continue;
            };
            if !nick.contains('@') {
                nick.push('@');
                nick.push_str(&peer_node);
            }
            hub.broadcast_deliver(None, false, &nick, &text);
        }
        eprintln!("chatterd: bridge disconnected peer={peer_addr} (retrying)");
        thread::sleep(backoff);
        // `conn` drops here, closing the fd, before the loop reconnects.
    }
}

pub fn serve(
    host: &str,
    port: u16,
    node: &str,
    peer: &str,
    peer_node_in: &str,
    tel: Arc<Telemetry>,
) -> i32 {
    let lfd = match listen_tcp(host, port) {
        Ok(fd) => fd,
        Err(e) => {
            eprintln!("chatterd: listen {host}:{port}: {e}");
            return 1;
        }
    };
    eprintln!("chatterd: listening on {host}:{port} node={node}");

    let hub = Arc::new(Hub::new());
    let sig = util::install_signal_flag();

    if !peer.is_empty() {
        let peer_node = if peer_node_in.is_empty() {
            "remote".to_string()
        } else {
            peer_node_in.to_string()
        };
        let hub2 = hub.clone();
        let peer = peer.to_string();
        let node2 = node.to_string();
        thread::spawn(move || bridge_worker(peer, node2, peer_node, hub2, sig));
    }

    loop {
        if sig.load(Ordering::Relaxed) {
            eprintln!("chatterd: shutdown");
            return 0;
        }
        let mut fds = [PollFd::new(lfd.as_fd(), PollFlags::POLLIN)];
        match poll(&mut fds, PollTimeout::from(200u16)) {
            Ok(0) => continue,
            Ok(_) => {}
            Err(Errno::EINTR) => continue,
            Err(e) => {
                eprintln!("chatterd: poll: {e}");
                return 1;
            }
        }
        let ready = fds[0]
            .revents()
            .map(|r| r.contains(PollFlags::POLLIN))
            .unwrap_or(false);
        if !ready {
            continue;
        }
        let cfd = match accept4(lfd.as_raw_fd(), SockFlag::SOCK_CLOEXEC) {
            Ok(fd) => unsafe { OwnedFd::from_raw_fd(fd) },
            Err(_) => continue,
        };
        let hub2 = hub.clone();
        let node2 = node.to_string();
        let tel2 = tel.clone();
        thread::spawn(move || handle_client(cfd, hub2, node2, tel2));
    }
}

// ---------------------------------------------------------------------------
// send / listen — minimal test clients used by verify to prove cross-host
// delivery deterministically (this capstone trims ch27's fuller "loadtest").
// ---------------------------------------------------------------------------

pub fn send(host: &str, port: u16, nick: &str, text: &str, timeout_ms: i64) -> i32 {
    let conn = match connect_tcp(host, port, timeout_ms) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("chatterd: send: dial {host}:{port}: {e}");
            return 1;
        }
    };
    let raw = conn.as_raw_fd();
    let joinf = Frame {
        typ: proto::TYPE_JOIN,
        payload: nick.as_bytes().to_vec(),
    };
    if let Err(e) = proto::write_frame(raw, &joinf) {
        eprintln!("chatterd: send: join: {e}");
        return 1;
    }
    let msgf = Frame {
        typ: proto::TYPE_MSG,
        payload: text.as_bytes().to_vec(),
    };
    if let Err(e) = proto::write_frame(raw, &msgf) {
        eprintln!("chatterd: send: msg: {e}");
        return 1;
    }
    thread::sleep(Duration::from_millis(150)); // let the write actually flush before closing
    println!("chatterd: sent nick={nick} text={text}");
    0
}

pub fn listen(host: &str, port: u16, nick: &str, timeout_ms: i64) -> i32 {
    let conn = match connect_tcp(host, port, 3000) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("chatterd: listen: dial {host}:{port}: {e}");
            return 1;
        }
    };
    let raw = conn.as_raw_fd();
    let joinf = Frame {
        typ: proto::TYPE_JOIN,
        payload: nick.as_bytes().to_vec(),
    };
    if let Err(e) = proto::write_frame(raw, &joinf) {
        eprintln!("chatterd: listen: join: {e}");
        return 1;
    }
    let _ = setsockopt(
        &conn,
        sockopt::ReceiveTimeout,
        &TimeVal::milliseconds(timeout_ms.max(0)),
    );

    loop {
        let f = match proto::read_frame(raw) {
            Ok(f) => f,
            Err(_) => {
                eprintln!("chatterd: listen timeout");
                return 1;
            }
        };
        if f.typ != proto::TYPE_DELIVER {
            continue;
        }
        let Some((from_nick, text)) = proto::split_deliver(&f.payload) else {
            continue;
        };
        println!("chatterd: received from={from_nick} text={text}");
        return 0;
    }
}
