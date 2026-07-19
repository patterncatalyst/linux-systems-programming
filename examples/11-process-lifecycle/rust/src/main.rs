//! pmon run -- CMD [ARGS...] — minimal process monitor for the process
//! lifecycle chapter: spawn CMD, wait for it, report its fate and rusage.
//!
//! ```text
//! pmon: pid <p> exited status <s>              child exited normally
//! pmon: pid <p> killed by signal <n> (<NAME>)  child died on a signal
//! pmon: rusage maxrss=<kb>KB user=<s>.<ms>s sys=<s>.<ms>s wall=<ms>ms
//! ```
//!
//! pmon's own exit code mirrors the child: the exit status, or 128+signal.
//! If exec itself fails, stderr gets "pmon: exec <cmd>: <reason>" and pmon
//! exits 127 with no report lines.
//!
//! `std::process::Command` does the fork/exec (with the same CLOEXEC
//! status-pipe trick the C++ version spells out, which is why an exec failure
//! comes back as an `Err` from `spawn`). The wait side goes through wait4(2)
//! directly so one call yields both the wait status and the child's rusage;
//! nix decodes the raw status and renders errno in strerror(3) shape.

use std::env;
use std::io;
use std::process::Command;
use std::time::Instant;

use nix::errno::Errno;
use nix::libc;
use nix::sys::wait::WaitStatus;
use nix::unistd::Pid;

/// Index 1..31; identical to the C++ and Go tables. Not strsignal(3) prose.
const SIG_NAMES: [&str; 32] = [
    "", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV", "USR2",
    "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
];

fn sig_name(n: i32) -> String {
    match usize::try_from(n).ok().and_then(|i| SIG_NAMES.get(i)) {
        Some(name) if n >= 1 => (*name).to_string(),
        _ => format!("SIG{n}"),
    }
}

/// "<sec>.<ms>s" from a wait4 timeval, e.g. "0.004s".
fn format_cpu(tv: libc::timeval) -> String {
    format!("{}.{:03}s", tv.tv_sec, tv.tv_usec / 1000)
}

/// strerror(3)-shaped reason for a spawn failure; `Errno::desc` carries the
/// classic text ("No such file or directory") without io::Error's
/// "(os error 2)" suffix.
fn reason(err: &io::Error) -> String {
    match err.raw_os_error() {
        Some(code) => Errno::from_raw(code).desc().to_string(),
        None => err.to_string(),
    }
}

/// wait4(2): reap `pid`, yielding the decoded wait status and its rusage.
fn wait4(pid: i32) -> io::Result<(WaitStatus, libc::rusage)> {
    let mut raw_status = 0i32;
    // SAFETY: zero-filled rusage is a valid out-param for wait4(2).
    let mut rusage: libc::rusage = unsafe { std::mem::zeroed() };
    loop {
        // SAFETY: raw_status and rusage are exclusively borrowed, valid
        // out-pointers for the duration of the call.
        let rc = unsafe { libc::wait4(pid, &mut raw_status, 0, &mut rusage) };
        if rc == pid {
            let status = WaitStatus::from_raw(Pid::from_raw(pid), raw_status)
                .map_err(|errno| io::Error::from_raw_os_error(errno as i32))?;
            return Ok((status, rusage));
        }
        let err = io::Error::last_os_error();
        if err.raw_os_error() != Some(libc::EINTR) {
            return Err(err);
        }
    }
}

fn monitor(name: &str, args: &[String]) -> i32 {
    let start = Instant::now();
    let child = match Command::new(name).args(args).spawn() {
        Ok(child) => child,
        Err(err) => {
            eprintln!("pmon: exec {name}: {}", reason(&err));
            return 127;
        }
    };
    let pid = child.id() as i32;

    let (status, rusage) = match wait4(pid) {
        Ok(pair) => pair,
        Err(err) => {
            eprintln!("pmon: wait4: {err}");
            return 1;
        }
    };
    let wall = start.elapsed().as_millis();

    let code = match status {
        WaitStatus::Exited(_, code) => {
            println!("pmon: pid {pid} exited status {code}");
            code
        }
        WaitStatus::Signaled(_, signal, _) => {
            let n = signal as i32;
            println!("pmon: pid {pid} killed by signal {n} ({})", sig_name(n));
            128 + n
        }
        other => {
            eprintln!("pmon: unexpected wait status: {other:?}");
            return 1;
        }
    };

    println!(
        "pmon: rusage maxrss={}KB user={} sys={} wall={wall}ms",
        rusage.ru_maxrss,
        format_cpu(rusage.ru_utime),
        format_cpu(rusage.ru_stime),
    );
    code
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 4 || args[1] != "run" || args[2] != "--" {
        eprintln!("usage: pmon run -- CMD [ARGS...]");
        std::process::exit(2);
    }
    std::process::exit(monitor(&args[3], &args[4..]));
}
