// chatterd v4 (Rust) — a peer-to-peer chat daemon.
//
// The connection-handling engine is selectable:
//   * thread : one std thread per client (the ch21 baseline; std::net + a
//              signal-hook stop flag).
//   * async  : tokio — an async fn per connection on a tokio::net::TcpListener,
//              spawned as tasks, broadcasting over per-client mpsc channels.
//
// Both engines speak the SAME length-prefixed frame protocol and are
// observably identical: a broadcast reaches every other connected client.
// std's TcpStream / TcpListener are the OwnedFd-backed RAII socket handles; a
// dropped stream closes its descriptor.
//
// Wire format (canonical chatterd chat frame, v1..v4):
//   [ magic 0x43 0x48 ][ version 0x01 ][ type u8 ][ length u16 be ][ payload ]
//   type: JOIN=1, MSG=2, DELIVER=3, WELCOME=4, PING=5.
//   The client's first frame is JOIN (payload = its nick). Every later client
//   frame is MSG (payload = message text). For a MSG with text T from the
//   client whose nick is N, the server sends every OTHER client a DELIVER
//   frame whose payload is N + 0x00 (NUL) + T. This chapter's engines use
//   only JOIN/MSG/DELIVER — WELCOME and PING are other versions' additions to
//   the same frame and are simply never sent here.
//
// Usage:
//   app serve [--engine thread|async] [--host H] [--port P]
//   app loadtest [--host H] [--port P] [--clients N]

use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

// ---------------------------------------------------------------------------
// Frame helpers — canonical chatterd chat frame (see file header).
// ---------------------------------------------------------------------------

const HEADER_LEN: usize = 6;
const MAGIC: [u8; 2] = [0x43, 0x48];
const VERSION: u8 = 0x01;
const TYPE_JOIN: u8 = 1;
const TYPE_MSG: u8 = 2;
const TYPE_DELIVER: u8 = 3;

fn encode_frame(typ: u8, body: &str) -> Vec<u8> {
    let len = body.len() as u16;
    let mut out = Vec::with_capacity(HEADER_LEN + body.len());
    out.extend_from_slice(&MAGIC);
    out.push(VERSION);
    out.push(typ);
    out.extend_from_slice(&len.to_be_bytes());
    out.extend_from_slice(body.as_bytes());
    out
}

// Blocking frame read on a stream that has a read timeout set. Returns
// Ok((type, body)), or Err on EOF/timeout/error.
fn read_frame_conn(s: &mut TcpStream) -> io::Result<(u8, String)> {
    let mut hdr = [0u8; HEADER_LEN];
    s.read_exact(&mut hdr)?;
    let typ = hdr[3];
    let n = u16::from_be_bytes([hdr[4], hdr[5]]) as usize;
    let mut buf = vec![0u8; n];
    s.read_exact(&mut buf)?;
    Ok((typ, String::from_utf8_lossy(&buf).into_owned()))
}

// ==========================================================================
// thread engine — one std thread per client.
// ==========================================================================

type ThreadClients = Arc<Mutex<HashMap<u64, Arc<Mutex<TcpStream>>>>>;

enum Frame {
    Msg(u8, String),
    Closed,
}

// Read one whole frame, honouring the stop flag between short read timeouts.
fn read_frame_std(reader: &mut TcpStream, buf: &mut Vec<u8>, stop: &AtomicBool) -> Frame {
    loop {
        if buf.len() >= HEADER_LEN {
            let typ = buf[3];
            let n = u16::from_be_bytes([buf[4], buf[5]]) as usize;
            if buf.len() >= HEADER_LEN + n {
                let body = String::from_utf8_lossy(&buf[HEADER_LEN..HEADER_LEN + n]).into_owned();
                buf.drain(0..HEADER_LEN + n);
                return Frame::Msg(typ, body);
            }
        }
        if stop.load(Ordering::Relaxed) {
            return Frame::Closed;
        }
        let mut tmp = [0u8; 4096];
        match reader.read(&mut tmp) {
            Ok(0) => return Frame::Closed,
            Ok(k) => buf.extend_from_slice(&tmp[..k]),
            Err(e)
                if e.kind() == io::ErrorKind::WouldBlock
                    || e.kind() == io::ErrorKind::TimedOut
                    || e.kind() == io::ErrorKind::Interrupted =>
            {
                continue // re-check the stop flag
            }
            Err(_) => return Frame::Closed,
        }
    }
}

fn serve_client(
    id: u64,
    mut reader: TcpStream,
    write_handle: Arc<Mutex<TcpStream>>,
    clients: ThreadClients,
    stop: Arc<AtomicBool>,
) {
    reader
        .set_read_timeout(Some(Duration::from_millis(200)))
        .ok();
    let mut buf = Vec::new();

    // First frame is JOIN: its payload is the nick.
    let nick = match read_frame_std(&mut reader, &mut buf, &stop) {
        Frame::Msg(_typ, n) => n,
        Frame::Closed => return,
    };
    clients.lock().unwrap().insert(id, write_handle);

    loop {
        match read_frame_std(&mut reader, &mut buf, &stop) {
            // Every later frame is MSG: its payload is the message text.
            Frame::Msg(_typ, body) => {
                let frame = encode_frame(TYPE_DELIVER, &format!("{nick}\0{body}"));
                let map = clients.lock().unwrap();
                for (cid, w) in map.iter() {
                    if *cid != id {
                        if let Ok(mut s) = w.lock() {
                            let _ = s.write_all(&frame);
                        }
                    }
                }
            }
            Frame::Closed => break,
        }
    }
    clients.lock().unwrap().remove(&id);
}

fn run_thread(host: &str, port: u16) -> io::Result<()> {
    let stop = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal_hook::consts::SIGINT, Arc::clone(&stop))?;
    signal_hook::flag::register(signal_hook::consts::SIGTERM, Arc::clone(&stop))?;

    let listener = TcpListener::bind((host, port))?;
    listener.set_nonblocking(true)?;
    eprintln!("chatterd: listening on {host}:{port} engine=thread");

    let clients: ThreadClients = Arc::new(Mutex::new(HashMap::new()));
    let mut workers = Vec::new();
    let mut next_id = 0u64;

    while !stop.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, _)) => {
                let reader = stream.try_clone()?;
                let write_handle = Arc::new(Mutex::new(stream));
                let id = next_id;
                next_id += 1;
                let clients = Arc::clone(&clients);
                let stop = Arc::clone(&stop);
                workers.push(thread::spawn(move || {
                    serve_client(id, reader, write_handle, clients, stop);
                }));
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(50));
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(_) => break,
        }
    }

    for w in workers {
        let _ = w.join();
    }
    eprintln!("chatterd: shutdown");
    Ok(())
}

// ==========================================================================
// async engine — tokio.
// ==========================================================================

mod async_engine {
    use super::{encode_frame, HEADER_LEN, TYPE_DELIVER};
    use std::collections::HashMap;
    use std::io;
    use std::sync::{Arc, Mutex};
    use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt, BufReader};
    use tokio::net::TcpListener;
    use tokio::net::TcpStream;
    use tokio::signal::unix::{signal, SignalKind};
    use tokio::sync::{broadcast, mpsc};

    type Clients = Arc<Mutex<HashMap<u64, mpsc::UnboundedSender<Vec<u8>>>>>;

    // Returns (type, payload).
    async fn read_frame<R: AsyncRead + Unpin>(r: &mut R) -> io::Result<(u8, String)> {
        let mut hdr = [0u8; HEADER_LEN];
        r.read_exact(&mut hdr).await?;
        let typ = hdr[3];
        let n = u16::from_be_bytes([hdr[4], hdr[5]]) as usize;
        let mut buf = vec![0u8; n];
        r.read_exact(&mut buf).await?;
        Ok((typ, String::from_utf8_lossy(&buf).into_owned()))
    }

    async fn handle_conn(
        id: u64,
        sock: TcpStream,
        clients: Clients,
        mut shutdown: broadcast::Receiver<()>,
    ) {
        let (rd, mut wr) = sock.into_split();
        let mut rd = BufReader::new(rd);

        // First frame is JOIN: its payload is the nick.
        let nick = match read_frame(&mut rd).await {
            Ok((_typ, n)) => n,
            Err(_) => return,
        };

        let (tx, mut rx) = mpsc::unbounded_channel::<Vec<u8>>();
        clients.lock().unwrap().insert(id, tx);

        // Writer task: drain this client's queue onto the socket.
        let writer = tokio::spawn(async move {
            while let Some(frame) = rx.recv().await {
                if wr.write_all(&frame).await.is_err() {
                    break;
                }
            }
        });

        loop {
            tokio::select! {
                // Every later frame is MSG: its payload is the message text.
                r = read_frame(&mut rd) => match r {
                    Ok((_typ, body)) => {
                        let frame = encode_frame(TYPE_DELIVER, &format!("{nick}\0{body}"));
                        let map = clients.lock().unwrap();
                        for (cid, tx) in map.iter() {
                            if *cid != id {
                                let _ = tx.send(frame.clone());
                            }
                        }
                    }
                    Err(_) => break,
                },
                _ = shutdown.recv() => break,
            }
        }

        clients.lock().unwrap().remove(&id);
        writer.abort();
    }

    async fn serve(host: String, port: u16) -> io::Result<()> {
        let listener = TcpListener::bind((host.as_str(), port)).await?;
        eprintln!("chatterd: listening on {host}:{port} engine=async");

        let clients: Clients = Arc::new(Mutex::new(HashMap::new()));
        let (shutdown_tx, _) = broadcast::channel::<()>(1);
        let mut sigint = signal(SignalKind::interrupt())?;
        let mut sigterm = signal(SignalKind::terminate())?;
        let mut next_id = 0u64;

        loop {
            tokio::select! {
                accepted = listener.accept() => {
                    let (sock, _) = accepted?;
                    let id = next_id;
                    next_id += 1;
                    tokio::spawn(handle_conn(
                        id, sock, Arc::clone(&clients), shutdown_tx.subscribe(),
                    ));
                }
                _ = sigint.recv() => break,
                _ = sigterm.recv() => break,
            }
        }

        let _ = shutdown_tx.send(());
        eprintln!("chatterd: shutdown");
        Ok(())
    }

    pub fn run(host: &str, port: u16) -> io::Result<()> {
        let rt = tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()?;
        rt.block_on(serve(host.to_string(), port))
    }
}

// ==========================================================================
// loadtest client — drives the broadcast assertion.
// ==========================================================================

fn run_loadtest(host: &str, port: u16, nclients: usize) -> io::Result<i32> {
    let addr = format!("{host}:{port}");
    let mut receivers: Vec<TcpStream> = Vec::with_capacity(nclients);

    for i in 0..nclients {
        let mut s = TcpStream::connect(&addr)?;
        s.write_all(&encode_frame(TYPE_JOIN, &format!("r{i}")))?;
        s.set_read_timeout(Some(Duration::from_secs(5)))?;
        receivers.push(s);
    }

    let mut sender = TcpStream::connect(&addr)?;
    sender.write_all(&encode_frame(TYPE_JOIN, "sender"))?; // JOIN

    // Let the server register every client before the broadcast goes out.
    thread::sleep(Duration::from_millis(250));
    sender.write_all(&encode_frame(TYPE_MSG, "hello world"))?;

    let want_nick = "sender";
    let want_text = "hello world";
    let mut delivered = 0;
    for (i, s) in receivers.iter_mut().enumerate() {
        let ok = match read_frame_conn(s) {
            Ok((typ, body)) if typ == TYPE_DELIVER => match body.split_once('\0') {
                Some((n, t)) => n == want_nick && t == want_text,
                None => false,
            },
            _ => false,
        };
        if ok {
            delivered += 1;
        } else {
            eprintln!("loadtest: client r{i} missed the broadcast");
        }
    }
    println!("loadtest: delivered {delivered}/{nclients}");
    Ok(if delivered == nclients { 0 } else { 1 })
}

// ---------------------------------------------------------------------------
// CLI.
// ---------------------------------------------------------------------------

fn usage() {
    eprintln!("usage:");
    eprintln!("  app serve [--engine thread|async] [--host H] [--port P]");
    eprintln!("  app loadtest [--host H] [--port P] [--clients N]");
}

struct Opts {
    engine: String,
    host: String,
    port: u16,
    clients: usize,
}

// Returns None on malformed input.
fn parse_opts(args: &[String]) -> Option<Opts> {
    let mut o = Opts {
        engine: "async".to_string(),
        host: "127.0.0.1".to_string(),
        port: 47100,
        clients: 20,
    };
    let mut i = 0;
    while i < args.len() {
        let key = &args[i];
        let val = args.get(i + 1)?;
        match key.as_str() {
            "--engine" => o.engine = val.clone(),
            "--host" => o.host = val.clone(),
            "--port" => o.port = val.parse().ok()?,
            "--clients" => o.clients = val.parse().ok()?,
            _ => return None,
        }
        i += 2;
    }
    Some(o)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
        return ExitCode::from(2);
    }
    let cmd = args[1].as_str();
    let opts = match parse_opts(&args[2..]) {
        Some(o) => o,
        None => {
            usage();
            return ExitCode::from(2);
        }
    };

    match cmd {
        "serve" => {
            let r = match opts.engine.as_str() {
                "thread" => run_thread(&opts.host, opts.port),
                "async" => async_engine::run(&opts.host, opts.port),
                other => {
                    eprintln!("chatterd: unknown engine '{other}' (want thread|async)");
                    return ExitCode::from(2);
                }
            };
            if let Err(e) = r {
                eprintln!("chatterd: {e}");
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        "loadtest" => {
            if opts.clients < 1 || opts.clients > 65535 {
                eprintln!("loadtest: --clients out of range");
                return ExitCode::from(2);
            }
            match run_loadtest(&opts.host, opts.port, opts.clients) {
                Ok(code) => ExitCode::from(code as u8),
                Err(e) => {
                    eprintln!("loadtest: {e}");
                    ExitCode::from(1)
                }
            }
        }
        _ => {
            usage();
            ExitCode::from(2)
        }
    }
}
