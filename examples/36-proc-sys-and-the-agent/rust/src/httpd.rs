//! sysagent: a hand-rolled, single-endpoint HTTP/1.1 server. No framework —
//! same philosophy as the raw-socket chatterd examples earlier in the book,
//! and the same poll(2) + signalfd(2) shutdown doorbell as their Rust code.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::fd::AsFd;

use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::signal::{SigSet, Signal};
use nix::sys::signalfd::{SfdFlags, SignalFd};

use crate::procfs::{take_snapshot, to_json};

fn install_shutdown() -> SignalFd {
    let mut mask = SigSet::empty();
    mask.add(Signal::SIGINT);
    mask.add(Signal::SIGTERM);
    mask.thread_block().expect("block signals");
    SignalFd::with_flags(&mask, SfdFlags::SFD_CLOEXEC).expect("signalfd")
}

fn http_response(status: u16, status_text: &str, content_type: &str, body: &str) -> String {
    format!(
        "HTTP/1.1 {status} {status_text}\r\nContent-Type: {content_type}\r\n\
         Content-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    )
}

fn handle_client(mut stream: TcpStream, interval_ms: u64) {
    let mut buf = [0u8; 4096];
    let n = match stream.read(&mut buf) {
        Ok(n) if n > 0 => n,
        _ => return,
    };
    let req = String::from_utf8_lossy(&buf[..n]);
    let line = req.lines().next().unwrap_or("");
    let mut parts = line.split_whitespace();
    let method = parts.next().unwrap_or("");
    let path = parts.next().unwrap_or("");

    let resp = if method == "GET" && path == "/metrics" {
        match take_snapshot(interval_ms) {
            Ok(snap) => http_response(200, "OK", "application/json", &to_json(&snap)),
            Err(e) => http_response(500, "Internal Server Error", "text/plain", &e),
        }
    } else {
        http_response(404, "Not Found", "text/plain", "not found\n")
    };
    let _ = stream.write_all(resp.as_bytes());
}

/// Serves GET /metrics (JSON snapshot, see procfs.rs) on 0.0.0.0:port until
/// SIGINT/SIGTERM. Each request takes a fresh snapshot with the given
/// interval_ms. Returns the process exit code (0 on a clean signal shutdown).
pub fn serve(port: u16, interval_ms: u64) -> i32 {
    let listener = match TcpListener::bind(("0.0.0.0", port)) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("sysagent: bind: {e}");
            return 1;
        }
    };
    if let Err(e) = listener.set_nonblocking(true) {
        eprintln!("sysagent: set_nonblocking: {e}");
        return 1;
    }

    let sfd = install_shutdown();
    println!("sysagent: listening on 0.0.0.0:{port}");

    loop {
        let mut pfds = [
            PollFd::new(sfd.as_fd(), PollFlags::POLLIN),
            PollFd::new(listener.as_fd(), PollFlags::POLLIN),
        ];
        match poll(&mut pfds, PollTimeout::NONE) {
            Ok(_) => {}
            Err(nix::errno::Errno::EINTR) => continue,
            Err(e) => {
                eprintln!("sysagent: poll: {e}");
                return 1;
            }
        }
        let sig_ready = pfds[0]
            .revents()
            .unwrap_or(PollFlags::empty())
            .contains(PollFlags::POLLIN);
        let conn_ready = pfds[1]
            .revents()
            .unwrap_or(PollFlags::empty())
            .contains(PollFlags::POLLIN);

        if sig_ready {
            println!("sysagent: shutting down");
            return 0;
        }
        if conn_ready {
            match listener.accept() {
                Ok((stream, _)) => {
                    let _ = stream.set_nonblocking(false);
                    handle_client(stream, interval_ms);
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(_) => {}
            }
        }
    }
}
