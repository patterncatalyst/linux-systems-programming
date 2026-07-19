// pmon v2 — a tiny process supervisor grown chapter by chapter.
//
// v0 (`run`) spawns a command, waits, and mirrors its exit status.
// v1 (`supervise --engine sigchld`) restarts a crashing child, driven by
//     SIGCHLD delivered as readable data through a signalfd (nix).
// v2 (`supervise --engine pidfd`, the DEFAULT) drops the SIGCHLD dependence
//     entirely: rustix::process::pidfd_open turns the child into a pollable
//     OwnedFd, rustix::event::poll reports the exit as ordinary readability,
//     waitid(WaitId::PidFd) reaps exactly that child, and the stop path
//     signals through the fd with pidfd_send_signal — no pid-reuse race
//     anywhere on the path.

use std::os::fd::{AsFd, AsRawFd};
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::process::Command;
use std::time::Instant;

use anyhow::{Context, Result};
use nix::sys::signal::{kill, SigSet, Signal as NixSignal};
use nix::sys::signalfd::{SfdFlags, SignalFd};
use nix::sys::wait::{waitpid, WaitPidFlag, WaitStatus};
use nix::unistd::Pid as NixPid;
use rustix::event::{poll, PollFd, PollFlags, Timespec};
use rustix::process::{
    pidfd_open, pidfd_send_signal, waitid, Pid, PidfdFlags, Signal, WaitId, WaitIdOptions,
};

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

fn spawn(cmdline: &[String]) -> Result<std::process::Child> {
    let mut cmd = Command::new(&cmdline[0]);
    cmd.args(&cmdline[1..]);
    // The supervisor blocks signals for its signalfd and the child inherits
    // that mask across fork+exec (std does NOT reset it) — a supervised
    // `sleep` with SIGTERM blocked would never stop. Clear the mask in the
    // child; sigprocmask is async-signal-safe, so pre_exec may call it.
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
// v0: run — spawn, wait, mirror.
// ---------------------------------------------------------------------------

fn cmd_run(cmdline: &[String]) -> Result<i32> {
    let mut child = spawn(cmdline)?;
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
// v1/v2: supervise — two engines, one restart policy.
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

/// pidfd engine: the child's exit is just readability on a file descriptor.
fn wait_pidfd(pidfd: &impl AsFd, sigfd: &SignalFd, deadline: Instant) -> Result<Outcome> {
    loop {
        let mut fds = [
            PollFd::new(pidfd, PollFlags::IN),
            PollFd::new(sigfd, PollFlags::IN),
        ];
        let n = match poll(&mut fds, Some(&remaining(deadline))) {
            Err(rustix::io::Errno::INTR) => continue,
            other => other.context("poll")?,
        };
        if n == 0 {
            return Ok(Outcome::Stopped(Stop::Timeout));
        }
        if fds[1].revents().contains(PollFlags::IN) {
            let _ = sigfd.read_signal();
            return Ok(Outcome::Stopped(Stop::Signal));
        }
        if !fds[0].revents().is_empty() {
            return Ok(Outcome::Reaped(reap_pidfd(pidfd)?));
        }
    }
}

/// sigchld engine: SIGCHLD (and INT/TERM) arrive through the signalfd.
fn wait_sigchld(pid: NixPid, sigfd: &SignalFd, deadline: Instant) -> Result<Outcome> {
    loop {
        let mut fds = [PollFd::new(sigfd, PollFlags::IN)];
        let n = match poll(&mut fds, Some(&remaining(deadline))) {
            Err(rustix::io::Errno::INTR) => continue,
            other => other.context("poll")?,
        };
        if n == 0 {
            return Ok(Outcome::Stopped(Stop::Timeout));
        }
        let info = sigfd.read_signal().context("read signalfd")?;
        let signo = info.map(|si| si.ssi_signo as i32).unwrap_or(0);
        if signo != NixSignal::SIGCHLD as i32 {
            return Ok(Outcome::Stopped(Stop::Signal));
        }
        match waitpid(pid, Some(WaitPidFlag::WNOHANG)).context("waitpid")? {
            WaitStatus::Exited(_, code) => {
                return Ok(Outcome::Reaped(ChildStatus { exited: true, value: code }));
            }
            WaitStatus::Signaled(_, sig, _) => {
                return Ok(Outcome::Reaped(ChildStatus { exited: false, value: sig as i32 }));
            }
            _ => {} // coalesced or stale SIGCHLD: our child is still running
        }
    }
}

fn blocking_reap(pid: NixPid) -> Result<ChildStatus> {
    match waitpid(pid, None).context("waitpid")? {
        WaitStatus::Exited(_, code) => Ok(ChildStatus { exited: true, value: code }),
        WaitStatus::Signaled(_, sig, _) => Ok(ChildStatus { exited: false, value: sig as i32 }),
        other => anyhow::bail!("waitpid: unexpected status {other:?}"),
    }
}

fn cmd_supervise(
    engine: Engine,
    max_restarts: u32,
    timeout_ms: u64,
    cmdline: &[String],
) -> Result<i32> {
    let sigfd = match engine {
        Engine::Pidfd => make_signalfd(&[NixSignal::SIGINT, NixSignal::SIGTERM])?,
        Engine::Sigchld => {
            // The mask (and the signalfd over it) exist BEFORE the first
            // spawn, so an early exit's SIGCHLD is queued, never lost.
            make_signalfd(&[NixSignal::SIGCHLD, NixSignal::SIGINT, NixSignal::SIGTERM])?
        }
    };
    let deadline = Instant::now() + std::time::Duration::from_millis(timeout_ms);

    let mut restarts: u32 = 0;
    loop {
        let child = spawn(cmdline)?;
        let pid = child.id() as i32;

        let (outcome, pidfd) = match engine {
            Engine::Pidfd => {
                let pidfd = pidfd_open(Pid::from_raw(pid).unwrap(), PidfdFlags::empty())
                    .context("pidfd_open")?;
                eprintln!("pmon: engine=pidfd child={pid} pidfd={}", pidfd.as_raw_fd());
                (wait_pidfd(&pidfd, &sigfd, deadline)?, Some(pidfd))
            }
            Engine::Sigchld => {
                eprintln!("pmon: engine=sigchld child={pid}");
                (wait_sigchld(NixPid::from_raw(pid), &sigfd, deadline)?, None)
            }
        };

        match outcome {
            Outcome::Stopped(why) => {
                // Ask the child to terminate, reap it, report why we leave.
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
                let _ = report_exit(pid, st);
                let why = match why {
                    Stop::Timeout => "timeout",
                    Stop::Signal => "signal",
                };
                println!("pmon: exiting ({why})");
                return Ok(0);
            }
            Outcome::Reaped(st) => {
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
// CLI
// ---------------------------------------------------------------------------

fn usage() -> i32 {
    eprint!(
        "usage: pmon <command>\n\
         \x20 run -- CMD [ARGS...]                     spawn CMD, wait, mirror its exit\n\
         \x20 supervise [--engine pidfd|sigchld] [--max-restarts N] [--timeout-ms T]\n\
         \x20           -- CMD [ARGS...]               restart CMD on abnormal exit\n\
         \x20                                          (defaults: pidfd, N=3, T=10000)\n"
    );
    2
}

fn real_main() -> i32 {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        return usage();
    }
    let Some(sep) = args.iter().position(|a| a == "--") else {
        return usage(); // both subcommands need `-- CMD [ARGS...]`
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
                    _ => return usage(),
                }
            }
            cmd_supervise(engine, max_restarts, timeout_ms, cmdline)
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
