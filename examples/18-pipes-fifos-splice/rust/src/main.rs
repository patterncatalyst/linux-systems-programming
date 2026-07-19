// pmon v4 — the process supervisor becomes a log pipeline.
//
// v0 (`run`) spawns a command, waits, and mirrors its exit status.
// v1 (`supervise --engine sigchld`) restarts a crashing child via SIGCHLD
//     delivered as readable data through a signalfd (nix).
// v2 (`supervise --engine pidfd`, the DEFAULT) supervises through
//     rustix::process::pidfd_open + poll + waitid(WaitId::PidFd) — no
//     pid-reuse race.
// v4 (this chapter) keeps both engines and adds pipes:
//     * supervise captures the child's stdout AND stderr through two pipes
//       into a log file as "[out] ..."/"[err] ..." lines — the two pipe read
//       ends simply join the fd set each engine already polls;
//     * `tail --log F --fifo PATH` creates a FIFO and relays log bytes into
//       whatever reader attaches — rustix::pipe::splice fast path
//       (file -> pipe, kernel-side), read/write fallback; SIGPIPE ignored,
//       EPIPE reported as "pmon: tail reader detached" and survived,
//       nothing lost.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::os::unix::fs::FileTypeExt;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::process::{ChildStderr, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use nix::errno::Errno;
use nix::sys::signal::{
    SaFlags, SigAction, SigHandler, SigSet, Signal as NixSignal, kill, sigaction,
};
use nix::sys::signalfd::{SfdFlags, SignalFd};
use nix::sys::stat::Mode as NixMode;
use nix::sys::wait::{WaitPidFlag, WaitStatus, waitpid};
use nix::unistd::{Pid as NixPid, mkfifo};
use rustix::event::{PollFd, PollFlags, Timespec, poll};
use rustix::fs::{OFlags, fcntl_setfl};
use rustix::pipe::{SpliceFlags, splice};
use rustix::process::{
    Pid, PidfdFlags, Signal, WaitId, WaitIdOptions, pidfd_open, pidfd_send_signal, waitid,
};

const CHUNK: usize = 64 * 1024;
const POLL_TICK: Duration = Duration::from_millis(50);

// ---------------------------------------------------------------------------
// Status plumbing shared by every subcommand.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct ChildStatus {
    exited: bool, // true: value is the exit status; false: value is the signal
    value: i32,
}

/// Prints the exit-observation line and returns the mirrored exit code.
fn report_exit(pid: i32, st: ChildStatus) -> i32 {
    if st.exited {
        println!("pmon: child={} exited status={}", pid, st.value);
        st.value
    } else {
        println!("pmon: child={} killed signal={}", pid, st.value);
        128 + st.value
    }
}

/// Spawn with an empty signal mask in the child — the supervisor blocks
/// signals for its signalfd and std does NOT reset the mask across
/// fork+exec (a supervised `sleep` with SIGTERM blocked would never stop).
/// sigprocmask is async-signal-safe, so pre_exec may call it.
fn spawn(cmdline: &[String], piped: bool) -> Result<std::process::Child> {
    let mut cmd = Command::new(&cmdline[0]);
    cmd.args(&cmdline[1..]);
    if piped {
        cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
    }
    unsafe {
        cmd.pre_exec(|| {
            SigSet::empty()
                .thread_set_mask()
                .map_err(|e| std::io::Error::from_raw_os_error(e as i32))
        });
    }
    cmd.spawn().with_context(|| format!("spawn: {}", cmdline[0]))
}

// ---------------------------------------------------------------------------
// v0: run — spawn, wait, mirror (stdio inherited, no capture).
// ---------------------------------------------------------------------------

fn cmd_run(cmdline: &[String]) -> Result<i32> {
    let mut child = spawn(cmdline, false)?;
    let pid = child.id() as i32;
    eprintln!("pmon: run child={pid}");
    let status = child.wait().context("wait")?;
    let st = match status.signal() {
        Some(sig) => ChildStatus { exited: false, value: sig },
        None => ChildStatus { exited: true, value: status.code().unwrap_or(0) },
    };
    Ok(report_exit(pid, st))
}

// ---------------------------------------------------------------------------
// v4 capture: two pipes, line-buffered into the log with [out]/[err] tags.
// ---------------------------------------------------------------------------

/// Accumulates one stream's bytes and appends complete lines to the log as
/// "[pfx] line\n". Line-buffered: each completed line is written immediately.
struct LineRelay {
    prefix: &'static str,
    buf: Vec<u8>,
}

impl LineRelay {
    fn new(prefix: &'static str) -> Self {
        Self { prefix, buf: Vec::new() }
    }

    fn feed(&mut self, chunk: &[u8], log: &mut File) -> Result<()> {
        self.buf.extend_from_slice(chunk);
        while let Some(nl) = self.buf.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = self.buf.drain(..=nl).collect();
            self.emit(&line[..line.len() - 1], log)?;
        }
        if self.buf.len() > CHUNK {
            // bound the partial-line buffer
            let line = std::mem::take(&mut self.buf);
            self.emit(&line, log)?;
        }
        Ok(())
    }

    /// Stream closed: a trailing partial line still gets logged, with '\n'.
    fn flush(&mut self, log: &mut File) -> Result<()> {
        if self.buf.is_empty() {
            return Ok(());
        }
        let line = std::mem::take(&mut self.buf);
        self.emit(&line, log)
    }

    fn emit(&self, line: &[u8], log: &mut File) -> Result<()> {
        log.write_all(format!("[{}] ", self.prefix).as_bytes())
            .and_then(|_| log.write_all(line))
            .and_then(|_| log.write_all(b"\n"))
            .context("write log")
    }
}

/// One child's capture pipes and their relays.
struct Capture {
    out_pipe: ChildStdout,
    err_pipe: ChildStderr,
    out_relay: LineRelay,
    err_relay: LineRelay,
    out_open: bool,
    err_open: bool,
}

impl Capture {
    fn new(child: &mut std::process::Child) -> Result<Self> {
        Ok(Self {
            out_pipe: child.stdout.take().context("no stdout pipe")?,
            err_pipe: child.stderr.take().context("no stderr pipe")?,
            out_relay: LineRelay::new("out"),
            err_relay: LineRelay::new("err"),
            out_open: true,
            err_open: true,
        })
    }

    /// One readiness-driven read on the stdout pipe; EOF closes and flushes.
    fn drain_out(&mut self, log: &mut File) -> Result<()> {
        let mut buf = vec![0u8; CHUNK];
        match self.out_pipe.read(&mut buf) {
            Ok(0) => {
                self.out_open = false;
                self.out_relay.flush(log)
            }
            Ok(n) => self.out_relay.feed(&buf[..n], log),
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => Ok(()),
            Err(e) => Err(e).context("read stdout pipe"),
        }
    }

    fn drain_err(&mut self, log: &mut File) -> Result<()> {
        let mut buf = vec![0u8; CHUNK];
        match self.err_pipe.read(&mut buf) {
            Ok(0) => {
                self.err_open = false;
                self.err_relay.flush(log)
            }
            Ok(n) => self.err_relay.feed(&buf[..n], log),
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => Ok(()),
            Err(e) => Err(e).context("read stderr pipe"),
        }
    }

    /// The child is reaped: blocking-read both pipes to EOF so every logged
    /// byte lands before the exit is reported.
    fn drain_to_eof(&mut self, log: &mut File) -> Result<()> {
        while self.out_open {
            self.drain_out(log)?;
        }
        while self.err_open {
            self.drain_err(log)?;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// v1/v2 engines with the v4 pipes joined into the same poll set.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq)]
enum Engine {
    Pidfd,
    Sigchld,
}

#[derive(Clone, Copy)]
enum Stop {
    Signal,
    Timeout,
}

enum Outcome {
    Reaped(ChildStatus),
    Stopped(Stop),
}

fn remaining(deadline: Instant) -> Timespec {
    let left = deadline.saturating_duration_since(Instant::now());
    Timespec {
        tv_sec: left.as_secs() as i64,
        tv_nsec: i64::from(left.subsec_nanos()),
    }
}

/// Block `sigs` process-wide and return a signalfd delivering them as data.
fn make_signalfd(sigs: &[NixSignal]) -> Result<SignalFd> {
    let mut mask = SigSet::empty();
    for s in sigs {
        mask.add(*s);
    }
    mask.thread_block().context("sigprocmask")?;
    SignalFd::with_flags(&mask, SfdFlags::SFD_CLOEXEC).context("signalfd")
}

fn status_from_waitid(w: rustix::process::WaitIdStatus) -> ChildStatus {
    if let Some(sig) = w.terminating_signal() {
        ChildStatus { exited: false, value: sig as i32 }
    } else {
        ChildStatus { exited: true, value: w.exit_status().unwrap_or(0) }
    }
}

/// waitid(P_PIDFD) reaps the process the fd refers to — never a recycled pid.
fn reap_pidfd(pidfd: &impl AsFd) -> Result<ChildStatus> {
    let status = waitid(WaitId::PidFd(pidfd.as_fd()), WaitIdOptions::EXITED)
        .context("waitid(P_PIDFD)")?
        .context("waitid(P_PIDFD): no status")?;
    Ok(status_from_waitid(status))
}

fn blocking_reap(pid: NixPid) -> Result<ChildStatus> {
    match waitpid(pid, None).context("waitpid")? {
        WaitStatus::Exited(_, code) => Ok(ChildStatus { exited: true, value: code }),
        WaitStatus::Signaled(_, sig, _) => Ok(ChildStatus { exited: false, value: sig as i32 }),
        other => bail!("waitpid: unexpected status {other:?}"),
    }
}

/// One poll loop for both engines: the capture pipes, the signalfd, and
/// (pidfd engine only) the pidfd. Closed pipes drop out of the poll set.
fn wait_child(
    pid: NixPid,
    pidfd: Option<&OwnedFd>,
    sigfd: &SignalFd,
    deadline: Instant,
    cap: &mut Capture,
    log: &mut File,
) -> Result<Outcome> {
    // Fixed slot order; a closed pipe polls a harmless duplicate of sigfd
    // with no requested events instead.
    loop {
        let mut fds = [
            PollFd::new(sigfd, PollFlags::IN),
            PollFd::new(
                &cap.out_pipe,
                if cap.out_open { PollFlags::IN } else { PollFlags::empty() },
            ),
            PollFd::new(
                &cap.err_pipe,
                if cap.err_open { PollFlags::IN } else { PollFlags::empty() },
            ),
            match pidfd {
                Some(fd) => PollFd::new(fd, PollFlags::IN),
                None => PollFd::new(sigfd, PollFlags::empty()),
            },
        ];
        let n = match poll(&mut fds, Some(&remaining(deadline))) {
            Err(rustix::io::Errno::INTR) => continue,
            other => other.context("poll")?,
        };
        if n == 0 {
            return Ok(Outcome::Stopped(Stop::Timeout));
        }
        let revents: Vec<PollFlags> = fds.iter().map(|f| f.revents()).collect();
        if cap.out_open && !revents[1].is_empty() {
            cap.drain_out(log)?;
        }
        if cap.err_open && !revents[2].is_empty() {
            cap.drain_err(log)?;
        }
        if revents[0].contains(PollFlags::IN) {
            let info = sigfd.read_signal().context("read signalfd")?;
            let signo = info.map(|si| si.ssi_signo as i32).unwrap_or(0);
            if signo != NixSignal::SIGCHLD as i32 {
                return Ok(Outcome::Stopped(Stop::Signal));
            }
            // sigchld engine: WNOHANG-reap; coalesced SIGCHLD is benign.
            match waitpid(pid, Some(WaitPidFlag::WNOHANG)).context("waitpid")? {
                WaitStatus::Exited(_, code) => {
                    return Ok(Outcome::Reaped(ChildStatus { exited: true, value: code }));
                }
                WaitStatus::Signaled(_, sig, _) => {
                    return Ok(Outcome::Reaped(ChildStatus {
                        exited: false,
                        value: sig as i32,
                    }));
                }
                _ => {} // our child is still running
            }
        }
        if let Some(fd) = pidfd
            && !revents[3].is_empty()
        {
            return Ok(Outcome::Reaped(reap_pidfd(fd)?));
        }
    }
}

fn cmd_supervise(
    engine: Engine,
    max_restarts: u32,
    timeout_ms: u64,
    log_path: &str,
    cmdline: &[String],
) -> Result<i32> {
    let mut log = File::options()
        .create(true)
        .append(true)
        .open(log_path)
        .with_context(|| format!("open {log_path}"))?;
    let sigfd = match engine {
        Engine::Pidfd => make_signalfd(&[NixSignal::SIGINT, NixSignal::SIGTERM])?,
        Engine::Sigchld => {
            // The mask (and the signalfd over it) exist BEFORE the first
            // spawn, so an early exit's SIGCHLD is queued, never lost.
            make_signalfd(&[NixSignal::SIGCHLD, NixSignal::SIGINT, NixSignal::SIGTERM])?
        }
    };
    let deadline = Instant::now() + Duration::from_millis(timeout_ms);

    let mut restarts: u32 = 0;
    loop {
        let mut child = spawn(cmdline, true)?;
        let pid = child.id() as i32;
        let mut cap = Capture::new(&mut child)?;

        let pidfd = match engine {
            Engine::Pidfd => {
                let pidfd = pidfd_open(Pid::from_raw(pid).unwrap(), PidfdFlags::empty())
                    .context("pidfd_open")?;
                eprintln!("pmon: engine=pidfd child={pid} pidfd={}", pidfd.as_raw_fd());
                Some(pidfd)
            }
            Engine::Sigchld => {
                eprintln!("pmon: engine=sigchld child={pid}");
                None
            }
        };

        let outcome = wait_child(
            NixPid::from_raw(pid),
            pidfd.as_ref(),
            &sigfd,
            deadline,
            &mut cap,
            &mut log,
        )?;

        match outcome {
            Outcome::Stopped(why) => {
                // Ask the child to terminate, reap it, finish the log,
                // report why we leave.
                let st = match &pidfd {
                    Some(fd) => {
                        let _ = pidfd_send_signal(fd, Signal::TERM);
                        reap_pidfd(fd)?
                    }
                    None => {
                        let _ = kill(NixPid::from_raw(pid), NixSignal::SIGTERM);
                        blocking_reap(NixPid::from_raw(pid))?
                    }
                };
                cap.drain_to_eof(&mut log)?;
                let _ = report_exit(pid, st);
                let why = match why {
                    Stop::Timeout => "timeout",
                    Stop::Signal => "signal",
                };
                println!("pmon: exiting ({why})");
                return Ok(0);
            }
            Outcome::Reaped(st) => {
                // Finish the log before reporting, then apply the policy.
                cap.drain_to_eof(&mut log)?;
                let _ = report_exit(pid, st);
                if st.exited && st.value == 0 {
                    return Ok(0);
                }
                if restarts >= max_restarts {
                    println!("pmon: giving up after {max_restarts} restarts");
                    return Ok(1);
                }
                restarts += 1;
                println!("pmon: restart {restarts}/{max_restarts}");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// v4: tail — follow the log, relay into a FIFO, survive reader churn.
// ---------------------------------------------------------------------------

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" fn on_stop(_sig: i32) {
    STOP.store(true, Ordering::SeqCst);
}

fn stop_requested() -> bool {
    STOP.load(Ordering::SeqCst)
}

/// Open the FIFO for writing without blocking forever: O_NONBLOCK turns
/// "no reader yet" into ENXIO, which we poll. Once a reader is attached the
/// fd goes back to blocking mode for the relay.
fn open_writer(fifo: &str) -> Result<Option<OwnedFd>> {
    while !stop_requested() {
        match rustix::fs::open(
            fifo,
            OFlags::WRONLY | OFlags::NONBLOCK | OFlags::CLOEXEC,
            rustix::fs::Mode::empty(),
        ) {
            Ok(fd) => {
                fcntl_setfl(&fd, OFlags::empty())
                    .with_context(|| format!("clear O_NONBLOCK on {fifo}"))?;
                return Ok(Some(fd));
            }
            Err(rustix::io::Errno::NXIO) | Err(rustix::io::Errno::INTR) => {
                std::thread::sleep(POLL_TICK);
            }
            Err(e) => return Err(e).with_context(|| format!("open {fifo}")),
        }
    }
    Ok(None) // stop requested while waiting for a reader
}

enum Relay {
    Moved,
    Idle,
    Detached,
    NoSplice,
}

/// splice(2) fast path: move log bytes kernel-side into the pipe. The file
/// offset only advances on success, so a detached reader (EPIPE) loses
/// nothing — the same bytes are respliced for the next reader.
fn relay_splice(log: &File, w: &OwnedFd) -> Result<Relay> {
    match splice(log, None, w, None, CHUNK, SpliceFlags::empty()) {
        Ok(0) => Ok(Relay::Idle),
        Ok(_) => Ok(Relay::Moved),
        Err(rustix::io::Errno::PIPE) => Ok(Relay::Detached),
        Err(rustix::io::Errno::INTR) | Err(rustix::io::Errno::AGAIN) => Ok(Relay::Idle),
        Err(rustix::io::Errno::INVAL) | Err(rustix::io::Errno::NOSYS) => Ok(Relay::NoSplice),
        Err(e) => Err(e).context("splice"),
    }
}

/// read/write fallback with the same no-loss contract: on EPIPE the file
/// offset is rewound to the first unwritten byte.
fn relay_rw(log: &mut File, w: &OwnedFd) -> Result<Relay> {
    let before = log.stream_position().context("stream_position")?;
    let mut buf = vec![0u8; CHUNK];
    let n = match log.read(&mut buf) {
        Ok(0) => return Ok(Relay::Idle),
        Ok(n) => n,
        Err(e) if e.kind() == std::io::ErrorKind::Interrupted => return Ok(Relay::Idle),
        Err(e) => return Err(e).context("read log"),
    };
    let mut written = 0usize;
    while written < n {
        match rustix::io::write(w, &buf[written..n]) {
            Ok(k) => written += k,
            Err(rustix::io::Errno::INTR) => continue,
            Err(e) => {
                log.seek(SeekFrom::Start(before + written as u64)).context("seek log")?;
                if e == rustix::io::Errno::PIPE {
                    return Ok(Relay::Detached);
                }
                return Err(e).context("write fifo");
            }
        }
    }
    Ok(Relay::Moved)
}

fn cmd_tail(log_path: &str, fifo: &str) -> Result<i32> {
    unsafe {
        // SIGPIPE must stay an error value (EPIPE), never kill the relay.
        sigaction(
            NixSignal::SIGPIPE,
            &SigAction::new(SigHandler::SigIgn, SaFlags::empty(), SigSet::empty()),
        )
        .context("sigaction SIGPIPE")?;
        // No SA_RESTART: blocking calls return EINTR so the loops see STOP.
        let stop =
            SigAction::new(SigHandler::Handler(on_stop), SaFlags::empty(), SigSet::empty());
        sigaction(NixSignal::SIGINT, &stop).context("sigaction SIGINT")?;
        sigaction(NixSignal::SIGTERM, &stop).context("sigaction SIGTERM")?;
    }

    match mkfifo(fifo, NixMode::from_bits_truncate(0o644)) {
        Ok(()) => {}
        Err(Errno::EEXIST) => {
            let meta = std::fs::metadata(fifo).with_context(|| format!("stat {fifo}"))?;
            if !meta.file_type().is_fifo() {
                bail!("{fifo} exists and is not a FIFO");
            }
        }
        Err(e) => return Err(e).with_context(|| format!("mkfifo {fifo}")),
    }
    let mut log = File::open(log_path).with_context(|| format!("open {log_path}"))?;
    eprintln!("pmon: tail ready (fifo {fifo})");

    let mut use_splice = true;
    while !stop_requested() {
        let Some(w) = open_writer(fifo)? else {
            break; // stop requested while waiting for a reader
        };
        let mut attached = true;
        while !stop_requested() && attached {
            let step =
                if use_splice { relay_splice(&log, &w)? } else { relay_rw(&mut log, &w)? };
            match step {
                Relay::Moved => {}
                Relay::Idle => std::thread::sleep(POLL_TICK),
                Relay::NoSplice => use_splice = false,
                Relay::Detached => {
                    println!("pmon: tail reader detached");
                    attached = false;
                }
            }
        }
    }
    println!("pmon: exiting (signal)");
    Ok(0)
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn usage() -> i32 {
    eprint!(
        "usage: pmon <command>\n\
         \x20 run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit\n\
         \x20 supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]\n\
         \x20           [--log FILE] -- CMD [ARGS...]  restart CMD on abnormal exit;\n\
         \x20                                          capture stdout/stderr into FILE\n\
         \x20                                          (defaults: pidfd, N=3, T=10000,\n\
         \x20                                          FILE=pmon.log)\n\
         \x20 tail --log FILE --fifo PATH              relay appended log lines into a\n\
         \x20                                          FIFO created at PATH\n"
    );
    2
}

fn real_main() -> i32 {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        return usage();
    }

    if args[0] == "tail" {
        let mut log_path = String::new();
        let mut fifo = String::new();
        let mut i = 1;
        while i + 1 < args.len() {
            match args[i].as_str() {
                "--log" => log_path = args[i + 1].clone(),
                "--fifo" => fifo = args[i + 1].clone(),
                _ => return usage(),
            }
            i += 2;
        }
        if i != args.len() || log_path.is_empty() || fifo.is_empty() {
            return usage();
        }
        return match cmd_tail(&log_path, &fifo) {
            Ok(code) => code,
            Err(e) => {
                eprintln!("pmon: {e:#}");
                1
            }
        };
    }

    let Some(sep) = args.iter().position(|a| a == "--") else {
        return usage(); // run and supervise need `-- CMD [ARGS...]`
    };
    if sep + 1 >= args.len() {
        return usage();
    }
    let flags = &args[1..sep];
    let cmdline = &args[sep + 1..];

    let result = match args[0].as_str() {
        "run" => {
            if !flags.is_empty() {
                return usage();
            }
            cmd_run(cmdline)
        }
        "supervise" => {
            let mut engine = Engine::Pidfd;
            let mut max_restarts: u32 = 3;
            let mut timeout_ms: u64 = 10_000;
            let mut log_path = String::from("pmon.log");
            let mut it = flags.iter();
            loop {
                let Some(flag) = it.next() else { break };
                let Some(value) = it.next() else {
                    return usage();
                };
                match flag.as_str() {
                    "--engine" => match value.as_str() {
                        "pidfd" => engine = Engine::Pidfd,
                        "sigchld" => engine = Engine::Sigchld,
                        _ => return usage(),
                    },
                    "--max-restarts" => match value.parse::<u32>() {
                        Ok(n) => max_restarts = n,
                        Err(_) => return usage(),
                    },
                    "--timeout-ms" => match value.parse::<u64>() {
                        Ok(t) if t > 0 => timeout_ms = t,
                        _ => return usage(),
                    },
                    "--log" => log_path = value.clone(),
                    _ => return usage(),
                }
            }
            cmd_supervise(engine, max_restarts, timeout_ms, &log_path, cmdline)
        }
        _ => return usage(),
    };

    match result {
        Ok(code) => code,
        Err(e) => {
            eprintln!("pmon: {e:#}");
            1
        }
    }
}

fn main() {
    std::process::exit(real_main());
}
