// pmon v1 — a mini process supervisor (chapter 12: signals and safe handling).
//
//   pmon run -- CMD [ARGS...]
//   pmon supervise [--max-restarts N] [--backoff-ms B] -- CMD [ARGS...]
//
// The Rust take on async-signal-safe design: the classic self-pipe trick,
// with the unsafe parts delegated to the signal-hook crate. Each watched
// signal is registered twice — flag::register sets a per-signal AtomicBool,
// and low_level::pipe::register_raw writes one byte to a "doorbell" pipe
// (write(2) is async-signal-safe; the write end is non-blocking so a full
// pipe can never wedge the handler). The main loop is then ordinary code:
// poll(2) on the doorbell with the backoff as the timeout, drain it, and
// look at the flags plus waitpid(WNOHANG) to see what happened.

use std::io::{PipeWriter, Read};
use std::os::fd::{AsFd, OwnedFd};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use nix::errno::Errno;
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::signal::{Signal, kill};
use nix::sys::wait::{WaitPidFlag, WaitStatus, waitpid};
use nix::unistd::Pid;
use signal_hook::consts::{SIGCHLD, SIGHUP, SIGINT, SIGTERM};

const USAGE: &str = "usage: pmon run -- CMD [ARGS...]\n       pmon supervise \
                     [--max-restarts N] [--backoff-ms B] -- CMD [ARGS...]";

fn usage() -> ! {
    eprintln!("{USAGE}");
    std::process::exit(2);
}

fn die(err: anyhow::Error) -> ! {
    eprintln!("pmon: error: {err:#}");
    std::process::exit(1);
}

/// Decoded exit of a reaped child.
#[derive(Clone, Copy)]
struct ChildExit {
    status: i32,
    signo: i32,
    signaled: bool,
}

impl ChildExit {
    fn clean(self) -> bool {
        !self.signaled && self.status == 0
    }
}

fn report(pid: i32, ce: ChildExit) {
    if ce.signaled {
        println!("pmon: child {pid} killed signal={}", ce.signo);
    } else {
        println!("pmon: child {pid} exited status={}", ce.status);
    }
}

/// Spawn CMD with inherited stdio and print the started line.
fn spawn(argv: &[String]) -> Result<i32> {
    let child = Command::new(&argv[0])
        .args(&argv[1..])
        .spawn()
        .with_context(|| format!("start {}", argv[0]))?;
    let pid = child.id() as i32;
    println!("pmon: started pid {pid}");
    // The Child handle is dropped without wait(): pmon reaps via waitpid(2)
    // so one code path serves both subcommands. Drop does not reap.
    Ok(pid)
}

fn decode_wait(ws: WaitStatus) -> Option<ChildExit> {
    match ws {
        WaitStatus::Exited(_, code) => {
            Some(ChildExit { status: code, signo: 0, signaled: false })
        }
        WaitStatus::Signaled(_, sig, _) => {
            Some(ChildExit { status: 0, signo: sig as i32, signaled: true })
        }
        _ => None, // StillAlive, stopped, continued: not an exit
    }
}

/// Blocking reap tolerating EINTR (signal-hook handlers may interrupt it).
fn reap_blocking(pid: i32) -> Result<ChildExit> {
    loop {
        match waitpid(Pid::from_raw(pid), None) {
            Ok(ws) => {
                if let Some(ce) = decode_wait(ws) {
                    return Ok(ce);
                }
            }
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e).context("waitpid"),
        }
    }
}

fn run_once(argv: &[String]) -> Result<i32> {
    let pid = spawn(argv)?;
    let ce = reap_blocking(pid)?;
    report(pid, ce);
    Ok(if ce.signaled { 128 + ce.signo } else { ce.status })
}

struct SuperviseOpts {
    max_restarts: u32,
    backoff_ms: u64,
    argv: Vec<String>,
}

/// One registered signal: the flag half of the flag + doorbell pair. The
/// handler owns its dup of the pipe's write end as an OwnedFd; signal-hook
/// makes it non-blocking so the handler can never wedge on a full pipe.
fn flag_for(signal: i32, doorbell: &PipeWriter) -> Result<Arc<AtomicBool>> {
    let flag = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal, Arc::clone(&flag)).context("flag::register")?;
    let owned: OwnedFd = doorbell.as_fd().try_clone_to_owned().context("dup")?;
    signal_hook::low_level::pipe::register_raw(signal, owned)
        .context("pipe::register_raw")?;
    Ok(flag)
}

fn supervise(opts: &SuperviseOpts) -> Result<i32> {
    // The doorbell: handlers write a byte, the main loop polls the read end.
    let (mut reader, writer) = std::io::pipe().context("pipe")?;
    let got_term = flag_for(SIGTERM, &writer)?;
    let got_int = flag_for(SIGINT, &writer)?;
    let got_hup = flag_for(SIGHUP, &writer)?;
    // SIGCHLD needs no flag: waitpid(WNOHANG) is the source of truth.
    signal_hook::low_level::pipe::register_raw(SIGCHLD, writer.into())
        .context("pipe::register_raw")?;

    // SIGTERM the child and reap it. Used by shutdown and reload paths.
    let stop_child = |pid: i32| -> Result<()> {
        match kill(Pid::from_raw(pid), Signal::SIGTERM) {
            Ok(()) | Err(Errno::ESRCH) => {}
            Err(e) => return Err(e).context("kill"),
        }
        reap_blocking(pid).map(|_| ())
    };

    let mut child = spawn(&opts.argv)?;
    let mut running = true; // false: no child alive, waiting out a backoff
    let mut restarts: u32 = 0;
    let mut backoff = opts.backoff_ms;
    let mut deadline = Instant::now();

    loop {
        let timeout = if running {
            PollTimeout::NONE
        } else {
            let left = deadline.saturating_duration_since(Instant::now());
            let ms = left.as_millis().saturating_add(1).min(u16::MAX as u128);
            if left.is_zero() { PollTimeout::ZERO } else { PollTimeout::from(ms as u16) }
        };
        let nready = {
            let mut fds = [PollFd::new(reader.as_fd(), PollFlags::POLLIN)];
            match poll(&mut fds, timeout) {
                Ok(n) => n,
                Err(Errno::EINTR) => continue,
                Err(e) => return Err(e).context("poll"),
            }
        };
        if nready == 0 {
            if Instant::now() < deadline {
                continue; // clamped timeout; keep waiting
            }
            // Backoff elapsed: bring the child back.
            child = spawn(&opts.argv)?;
            running = true;
            continue;
        }

        // Drain the doorbell (poll said readable; coalesced bytes are fine).
        let mut buf = [0u8; 64];
        let _ = reader.read(&mut buf).context("read doorbell")?;

        let term = got_term.swap(false, Ordering::SeqCst);
        let intr = got_int.swap(false, Ordering::SeqCst);
        if term || intr {
            if running {
                stop_child(child)?;
            }
            println!(
                "pmon: shutting down ({})",
                if term { "SIGTERM" } else { "SIGINT" }
            );
            return Ok(0);
        }

        if got_hup.swap(false, Ordering::SeqCst) {
            println!("pmon: reload requested");
            if running {
                stop_child(child)?;
            }
            restarts = 0;
            backoff = opts.backoff_ms;
            child = spawn(&opts.argv)?;
            running = true;
            continue;
        }

        // SIGCHLD (or a stale doorbell byte): see whether our child exited.
        if !running {
            continue;
        }
        match waitpid(Pid::from_raw(child), Some(WaitPidFlag::WNOHANG)) {
            Ok(ws) => {
                let Some(ce) = decode_wait(ws) else { continue };
                report(child, ce);
                if ce.clean() {
                    return Ok(0);
                }
                if restarts >= opts.max_restarts {
                    println!("pmon: giving up after {} restarts", opts.max_restarts);
                    return Ok(1);
                }
                restarts += 1;
                println!("pmon: restart #{restarts} (backoff {backoff}ms)");
                deadline = Instant::now() + Duration::from_millis(backoff);
                backoff *= 2;
                running = false;
            }
            Err(Errno::EINTR) | Err(Errno::ECHILD) => continue,
            Err(e) => return Err(e).context("waitpid"),
        }
    }
}

fn parse_supervise(args: &[String]) -> Result<SuperviseOpts> {
    let mut opts =
        SuperviseOpts { max_restarts: 5, backoff_ms: 100, argv: Vec::new() };
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--" => {
                opts.argv = args[i + 1..].to_vec();
                if opts.argv.is_empty() {
                    bail!("no command after --");
                }
                return Ok(opts);
            }
            "--max-restarts" => {
                i += 1;
                let v = args.get(i).and_then(|s| s.parse::<u32>().ok());
                opts.max_restarts =
                    v.ok_or_else(|| anyhow::anyhow!("bad value for --max-restarts"))?;
            }
            "--backoff-ms" => {
                i += 1;
                let v = args
                    .get(i)
                    .and_then(|s| s.parse::<u64>().ok())
                    .filter(|&v| v >= 1);
                opts.backoff_ms =
                    v.ok_or_else(|| anyhow::anyhow!("bad value for --backoff-ms"))?;
            }
            other => bail!("unknown flag {other}"),
        }
        i += 1;
    }
    bail!("no command after --")
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    let (sub, rest) = (args[0].as_str(), &args[1..]);
    let code = match sub {
        "run" => {
            if rest.len() < 2 || rest[0] != "--" {
                usage();
            }
            run_once(&rest[1..]).unwrap_or_else(|e| die(e))
        }
        "supervise" => {
            let opts = parse_supervise(rest).unwrap_or_else(|e| {
                eprintln!("pmon: error: {e:#}");
                usage();
            });
            supervise(&opts).unwrap_or_else(|e| die(e))
        }
        _ => usage(),
    };
    std::process::exit(code);
}
