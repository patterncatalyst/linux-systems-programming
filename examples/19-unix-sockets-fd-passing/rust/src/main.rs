// pmon v5 — a process supervisor with a UNIX-socket control plane.
//
// `pmon supervise` spawns a child command (its own process group,
// stdout+stderr appended to a log file), restarts it 300 ms after every
// exit, and serves a SOCK_STREAM control socket. `pmon pmctl` is the client
// side of the same binary:
//
//   status  -> "child=<pid> uptime=<s>s restarts=<n>"
//   stop    -> "stopping", then the supervisor tears down and exits 0
//   logfd   -> the supervisor passes its OPEN log file descriptor through an
//              SCM_RIGHTS control message; pmctl reads the last 3 lines
//              straight off the received fd ("via-fd: ..."), never touching
//              the path.
//
// The cmsg plumbing is nix::sys::socket sendmsg/recvmsg with
// ControlMessage::ScmRights — explicit, not a convenience crate — and the
// received fd is wrapped in an OwnedFd immediately so it closes on drop.

use std::fs::File;
use std::io::{IoSlice, IoSliceMut, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::fs::FileExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::process::{Child, Command, ExitCode, Stdio};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use nix::sys::signal::{Signal, kill};
use nix::sys::socket::sockopt::PeerCredentials;
use nix::sys::socket::{
    ControlMessage, ControlMessageOwned, MsgFlags, getsockopt, recvmsg, sendmsg,
};
use nix::unistd::Pid;

const RESTART_BACKOFF: Duration = Duration::from_millis(300);
const MAX_LOG_READ: u64 = 1 << 20; // pmctl reads at most 1 MiB of log

fn usage() -> ExitCode {
    eprintln!("usage: pmon supervise --ctl SOCK --log FILE -- CMD [ARG...]");
    eprintln!("       pmon pmctl --ctl SOCK <status|stop|logfd>");
    ExitCode::from(2)
}

// ---------------------------------------------------------------------------
// Child lifecycle
// ---------------------------------------------------------------------------

struct State {
    child: i32, // pid of the current child
    started: Instant,
    restarts: u32,
    stopping: bool,
    child_done: bool, // reaper has collected the final child
}

type Shared = Arc<(Mutex<State>, Condvar)>;

/// Spawn argv in its own process group with stdout+stderr appended to the
/// log file — the fd is inherited directly, no pipe thread.
fn spawn_child(argv: &[String], log: &File) -> Result<Child> {
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..])
        .stdout(Stdio::from(log.try_clone().context("dup log fd")?))
        .stderr(Stdio::from(log.try_clone().context("dup log fd")?))
        .process_group(0); // own process group, so stop can signal the whole tree
    cmd.spawn().with_context(|| format!("exec {}", argv[0]))
}

fn exit_code_of(status: std::process::ExitStatus) -> i32 {
    if let Some(code) = status.code() {
        code
    } else {
        128 + status.signal().unwrap_or(1)
    }
}

/// One write per line: O_APPEND makes it an atomic append.
fn log_line(mut log: &File, line: &str) {
    let _ = log.write_all(format!("{line}\n").as_bytes());
}

fn mark_done(shared: &Shared) {
    let (lock, cv) = &**shared;
    lock.lock().unwrap().child_done = true;
    cv.notify_all();
}

/// Reaper thread: wait for the current child; unless we are stopping, back
/// off 300 ms and respawn. Ends by reporting child_done through the condvar.
fn reaper(shared: Shared, mut child: Child, argv: Vec<String>, log: File) {
    let (lock, cv) = &*shared;
    loop {
        let pid = child.id() as i32;
        let status = match child.wait() {
            Ok(status) => status,
            Err(err) => {
                eprintln!("pmon: error: waitpid: {err}");
                mark_done(&shared);
                return;
            }
        };
        if lock.lock().unwrap().stopping {
            mark_done(&shared);
            return;
        }
        eprintln!(
            "pmon: child pid={} exited status={}",
            pid,
            exit_code_of(status)
        );
        {
            let guard = lock.lock().unwrap();
            let (guard, _timed_out) = cv
                .wait_timeout_while(guard, RESTART_BACKOFF, |s| !s.stopping)
                .unwrap();
            if guard.stopping {
                drop(guard);
                mark_done(&shared);
                return;
            }
        }
        let next = match spawn_child(&argv, &log) {
            Ok(next) => next,
            Err(err) => {
                eprintln!("pmon: error: {err:#}");
                mark_done(&shared);
                return;
            }
        };
        let npid = next.id() as i32;
        let mut restarts_now = 0;
        let stop_now = {
            let mut s = lock.lock().unwrap();
            if !s.stopping {
                s.child = npid;
                s.started = Instant::now();
                s.restarts += 1;
                restarts_now = s.restarts;
            }
            s.stopping
        };
        if stop_now {
            // stop raced with the respawn: undo it
            let mut next = next;
            let _ = kill(Pid::from_raw(-npid), Signal::SIGTERM);
            let _ = next.wait();
            mark_done(&shared);
            return;
        }
        eprintln!("pmon: restart {restarts_now} child pid={npid}");
        log_line(&log, &format!("pmon: start child pid={npid}"));
        child = next;
    }
}

// ---------------------------------------------------------------------------
// supervise
// ---------------------------------------------------------------------------

fn read_command(conn: &mut UnixStream) -> String {
    let mut cmd = Vec::new();
    let mut one = [0u8; 1];
    while cmd.len() < 64 {
        match conn.read(&mut one) {
            Ok(1) if one[0] != b'\n' => cmd.push(one[0]),
            _ => break,
        }
    }
    String::from_utf8_lossy(&cmd).into_owned()
}

fn send_log_fd(conn: &UnixStream, log: &File) {
    let payload = [IoSlice::new(b"ok\n")];
    let fds = [log.as_raw_fd()];
    let cmsg = [ControlMessage::ScmRights(&fds)];
    if let Err(err) = sendmsg::<()>(conn.as_raw_fd(), &payload, &cmsg, MsgFlags::empty(), None) {
        eprintln!("pmon: sendmsg: {err}");
    }
}

fn cmd_supervise(ctl: &str, log_path: &str, child_argv: Vec<String>) -> Result<()> {
    // read(true), too: the fd handed out via SCM_RIGHTS shares this open
    // file description, and pmctl must be able to read from it.
    let log = File::options()
        .create(true)
        .read(true)
        .append(true)
        .open(log_path)
        .with_context(|| format!("open {log_path}"))?;

    let _ = std::fs::remove_file(ctl); // stale socket from a previous run
    let listener = UnixListener::bind(ctl).with_context(|| format!("listen {ctl}"))?;
    eprintln!("pmon: listening on {ctl}");

    let first = match spawn_child(&child_argv, &log) {
        Ok(first) => first,
        Err(err) => {
            let _ = std::fs::remove_file(ctl);
            return Err(err);
        }
    };
    let first_pid = first.id() as i32;
    eprintln!("pmon: started child pid={first_pid}");
    log_line(&log, &format!("pmon: start child pid={first_pid}"));

    let shared: Shared = Arc::new((
        Mutex::new(State {
            child: first_pid,
            started: Instant::now(),
            restarts: 0,
            stopping: false,
            child_done: false,
        }),
        Condvar::new(),
    ));
    let reaper_handle = {
        let shared = Arc::clone(&shared);
        let argv = child_argv.clone();
        let log = log.try_clone().context("dup log fd")?;
        std::thread::spawn(move || reaper(shared, first, argv, log))
    };

    for conn in listener.incoming() {
        let mut conn = conn.context("accept")?;

        if let Ok(cred) = getsockopt(&conn, PeerCredentials) {
            eprintln!("pmon: ctl connect uid={} pid={}", cred.uid(), cred.pid());
        }

        let cmd = read_command(&mut conn);
        let (lock, cv) = &*shared;
        match cmd.as_str() {
            "status" => {
                let reply = {
                    let s = lock.lock().unwrap();
                    format!(
                        "child={} uptime={}s restarts={}\n",
                        s.child,
                        s.started.elapsed().as_secs(),
                        s.restarts
                    )
                };
                let _ = conn.write_all(reply.as_bytes());
            }
            "logfd" => send_log_fd(&conn, &log),
            "stop" => {
                let _ = conn.write_all(b"stopping\n");
                drop(conn); // deliver the reply before tearing down
                eprintln!("pmon: stopping");
                let child = {
                    let mut s = lock.lock().unwrap();
                    s.stopping = true;
                    s.child
                };
                cv.notify_all();
                if child > 0 {
                    // whole process group; ESRCH is fine
                    let _ = kill(Pid::from_raw(-child), Signal::SIGTERM);
                }
                {
                    let guard = lock.lock().unwrap();
                    let _guard = cv.wait_while(guard, |s| !s.child_done).unwrap();
                }
                let _ = reaper_handle.join();
                let _ = std::fs::remove_file(ctl);
                return Ok(());
            }
            _ => {
                let _ = conn.write_all(b"err unknown command\n");
            }
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// pmctl
// ---------------------------------------------------------------------------

fn recv_log_fd(conn: &UnixStream) -> Result<OwnedFd> {
    let mut payload = [0u8; 16];
    let mut iov = [IoSliceMut::new(&mut payload)];
    let mut cmsg_buf = nix::cmsg_space!(RawFd);
    let msg = recvmsg::<()>(
        conn.as_raw_fd(),
        &mut iov,
        Some(&mut cmsg_buf),
        MsgFlags::MSG_CMSG_CLOEXEC,
    )
    .context("recvmsg")?;
    for cmsg in msg.cmsgs().context("parse cmsg")? {
        if let ControlMessageOwned::ScmRights(fds) = cmsg
            && let Some(&fd) = fds.first()
        {
            // SAFETY: the kernel installed this fd for us via SCM_RIGHTS;
            // we are its only owner.
            return Ok(unsafe { OwnedFd::from_raw_fd(fd) });
        }
    }
    bail!("no SCM_RIGHTS control message in reply")
}

fn log_tail_via_fd(fd: OwnedFd, count: usize) -> Result<Vec<String>> {
    let file = File::from(fd);
    let mut data = Vec::new();
    let mut chunk = [0u8; 4096];
    let mut off: u64 = 0;
    while off < MAX_LOG_READ {
        let n = file.read_at(&mut chunk, off).context("pread")?;
        if n == 0 {
            break;
        }
        data.extend_from_slice(&chunk[..n]);
        off += n as u64;
    }
    let text = String::from_utf8_lossy(&data);
    let text = text.strip_suffix('\n').unwrap_or(&text);
    if text.is_empty() {
        return Ok(Vec::new());
    }
    let lines: Vec<String> = text.split('\n').map(str::to_owned).collect();
    let skip = lines.len().saturating_sub(count);
    Ok(lines[skip..].to_vec())
}

fn cmd_pmctl(ctl: &str, action: &str) -> Result<()> {
    let mut conn = UnixStream::connect(ctl).with_context(|| format!("connect {ctl}"))?;
    conn.write_all(format!("{action}\n").as_bytes())
        .with_context(|| format!("write {ctl}"))?;

    if action == "logfd" {
        let fd = recv_log_fd(&conn)?;
        for line in log_tail_via_fd(fd, 3)? {
            println!("via-fd: {line}");
        }
        return Ok(());
    }

    // status / stop: the supervisor replies with one line and closes.
    let mut reply = String::new();
    conn.read_to_string(&mut reply).context("read reply")?;
    let text = reply.trim_end_matches('\n');
    if text.is_empty() {
        bail!("empty reply from supervisor");
    }
    if let Some(err) = text.strip_prefix("err ") {
        return Err(anyhow!("{err}"));
    }
    println!("{text}");
    Ok(())
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(sub) = args.first() else {
        return usage();
    };

    match sub.as_str() {
        "supervise" => {
            let mut ctl = String::new();
            let mut log = String::new();
            let mut child: Vec<String> = Vec::new();
            let mut i = 1;
            while i < args.len() {
                if args[i] == "--ctl" && i + 1 < args.len() {
                    ctl = args[i + 1].clone();
                    i += 2;
                } else if args[i] == "--log" && i + 1 < args.len() {
                    log = args[i + 1].clone();
                    i += 2;
                } else if args[i] == "--" {
                    child = args[i + 1..].to_vec();
                    break;
                } else {
                    return usage();
                }
            }
            if ctl.is_empty() || log.is_empty() || child.is_empty() {
                return usage();
            }
            if let Err(err) = cmd_supervise(&ctl, &log, child) {
                eprintln!("pmon: error: {err:#}");
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        "pmctl" => {
            let mut ctl = String::new();
            let mut action = String::new();
            let mut i = 1;
            while i < args.len() {
                if args[i] == "--ctl" && i + 1 < args.len() {
                    ctl = args[i + 1].clone();
                    i += 2;
                } else if action.is_empty() {
                    action = args[i].clone();
                    i += 1;
                } else {
                    return usage();
                }
            }
            if ctl.is_empty() || !matches!(action.as_str(), "status" | "stop" | "logfd") {
                return usage();
            }
            if let Err(err) = cmd_pmctl(&ctl, &action) {
                eprintln!("pmctl: error: {err:#}");
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        _ => usage(),
    }
}
