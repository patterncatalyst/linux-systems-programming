// fwatch v3 — the ch09 watcher, now sandboxed.
//
// `watch --sandbox DIR` applies two independent kernel access-control layers
// before the epoll loop ever runs:
//
//   Landlock  — a ruleset (the `landlock` crate) restricting filesystem READS
//               to DIR. Unprivileged, self-imposed, enforced by the kernel
//               against this process and every descendant.
//   seccomp   — a syscall allowlist (the `seccompiler` crate — Firecracker's
//               BPF compiler) covering only what the watch loop needs.
//               Everything else returns EPERM.
//
// Landlock's own syscall numbers have no libc wrapper; probing the running
// kernel's ABI (`landlock_abi()` below) goes through the raw syscall(2)
// numbers directly, mirroring the C++ and Go versions of this example. Those
// numbers are part of the kernel's frozen syscall table (once assigned they
// never change, unlike a struct field offset), so hardcoding them per
// architecture is the same thing glibc's own <bits/syscall.h> does — but the
// ABI *version* returned by the call is always probed at runtime, never
// assumed.

use std::collections::{BTreeMap, BTreeSet};
use std::os::linux::fs::MetadataExt;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use landlock::{AccessFs, PathBeneath, PathFd, Ruleset, RulesetAttr, RulesetCreatedAttr, RulesetStatus};
use nix::errno::Errno;
use nix::sys::epoll::{Epoll, EpollCreateFlags, EpollEvent, EpollFlags, EpollTimeout};
use nix::sys::inotify::{AddWatchFlags, InitFlags, Inotify};
use nix::sys::signal::{SigSet, Signal};
use nix::sys::signalfd::{SfdFlags, SignalFd};
use nix::sys::time::TimeSpec;
use nix::sys::timerfd::{ClockId, Expiration, TimerFd, TimerFlags, TimerSetTimeFlags};
use seccompiler::{BpfProgram, SeccompAction, SeccompFilter, TargetArch};

const DEBOUNCE: Duration = Duration::from_millis(100);

// ---------------------------------------------------------------------------
// Landlock ABI probe — raw syscall, no libc/crate wrapper exists for this.
// ---------------------------------------------------------------------------

#[cfg(target_arch = "x86_64")]
const SYS_LANDLOCK_CREATE_RULESET: libc::c_long = 444;
const LANDLOCK_CREATE_RULESET_VERSION: libc::c_int = 1 << 0;

/// Probes the running kernel's supported Landlock ABI version (0 if
/// Landlock isn't built in / enabled at boot). Never hardcoded.
fn landlock_abi() -> i32 {
    // SAFETY: landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)
    // is documented to only ever query the ABI version; it touches no memory.
    let v = unsafe {
        libc::syscall(SYS_LANDLOCK_CREATE_RULESET, std::ptr::null::<u8>(), 0usize,
                      LANDLOCK_CREATE_RULESET_VERSION)
    };
    if v < 0 { 0 } else { v as i32 }
}

/// Restricts filesystem reads to `dir` (ReadFile + ReadDir, both available
/// since ABI v1) via the `landlock` crate, and confirms the kernel actually
/// enforced it (not merely "best-effort, kernel too old").
fn apply_landlock(dir: &str) -> Result<()> {
    let access = AccessFs::ReadFile | AccessFs::ReadDir;
    let status = Ruleset::default()
        .handle_access(access)
        .context("landlock: handle_access")?
        .create()
        .context("landlock: create ruleset")?
        .add_rule(PathBeneath::new(PathFd::new(dir).context("landlock: open dir")?, access))
        .context("landlock: add_rule")?
        .restrict_self()
        .context("landlock: restrict_self")?;
    if status.ruleset != RulesetStatus::FullyEnforced {
        bail!("landlock ruleset not fully enforced by this kernel ({:?})", status.ruleset);
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// seccomp — a `seccompiler` allowlist covering exactly what the watch loop
// needs (empirically confirmed with `strace -f` across a full watch
// session: create/modify/delete + debounce flush + timeout/signal exit).
// Anything else returns EPERM once the filter loads.
// ---------------------------------------------------------------------------

fn watch_syscalls() -> Vec<i64> {
    vec![
        libc::SYS_read, libc::SYS_write, libc::SYS_close,
        libc::SYS_epoll_create1, libc::SYS_epoll_ctl, libc::SYS_epoll_wait,
        libc::SYS_inotify_init1, libc::SYS_inotify_add_watch,
        libc::SYS_timerfd_create, libc::SYS_timerfd_settime, libc::SYS_signalfd4,
        libc::SYS_rt_sigprocmask, libc::SYS_rt_sigreturn,
        libc::SYS_mmap, libc::SYS_munmap, libc::SYS_mremap, libc::SYS_brk,
        libc::SYS_exit, libc::SYS_exit_group,
        libc::SYS_fstat, libc::SYS_newfstatat, libc::SYS_lseek,
        libc::SYS_clock_gettime, libc::SYS_clock_nanosleep, libc::SYS_nanosleep,
        libc::SYS_getrandom,
    ]
}

/// Installs an allowlist of `allowed` syscalls; everything else returns
/// errno `deny_errno`. Returns the number of syscalls admitted.
fn install_seccomp(allowed: &[i64], deny_errno: i32) -> Result<usize> {
    let rules = allowed.iter().map(|&nr| (nr, vec![])).collect();
    let filter = SeccompFilter::new(
        rules,
        SeccompAction::Errno(deny_errno as u32),
        SeccompAction::Allow,
        TargetArch::x86_64,
    )
    .context("seccomp: build filter")?;
    let program: BpfProgram = filter.try_into().context("seccomp: compile filter")?;
    seccompiler::apply_filter(&program).context("seccomp: install filter")?;
    Ok(allowed.len())
}

// ---------------------------------------------------------------------------
// v0: snapshot + diff
// ---------------------------------------------------------------------------

fn cmd_snapshot(dir: &str) -> Result<()> {
    let mut files: Vec<(String, u64, i64)> = Vec::new();
    let entries = std::fs::read_dir(dir).with_context(|| dir.to_string())?;
    for entry in entries {
        let entry = entry.with_context(|| dir.to_string())?;
        let Ok(meta) = entry.metadata() else {
            continue; // raced with a concurrent delete
        };
        if !meta.file_type().is_file() {
            continue;
        }
        let mtime_ns = meta.st_mtime() * 1_000_000_000 + meta.st_mtime_nsec();
        files.push((entry.file_name().to_string_lossy().into_owned(), meta.st_size(), mtime_ns));
    }
    files.sort();
    for (name, size, mtime_ns) in files {
        println!("{name}\t{size}\t{mtime_ns}");
    }
    Ok(())
}

/// Parse "name<TAB>size<TAB>mtime_ns" lines into name -> "size<TAB>mtime_ns";
/// malformed lines are skipped.
fn load_snapshot(path: &str) -> Result<BTreeMap<String, String>> {
    let text = std::fs::read_to_string(path).with_context(|| path.to_string())?;
    let mut out = BTreeMap::new();
    for line in text.lines() {
        let Some((rest, mtime)) = line.rsplit_once('\t') else {
            continue;
        };
        let Some((name, size)) = rest.rsplit_once('\t') else {
            continue;
        };
        if name.is_empty() {
            continue;
        }
        out.insert(name.to_string(), format!("{size}\t{mtime}"));
    }
    Ok(out)
}

fn cmd_diff(old_path: &str, new_path: &str) -> Result<()> {
    let old_snap = load_snapshot(old_path)?;
    let new_snap = load_snapshot(new_path)?;
    let names: BTreeSet<&String> = old_snap.keys().chain(new_snap.keys()).collect();
    for name in names {
        match (old_snap.get(name), new_snap.get(name)) {
            (None, Some(_)) => println!("created {name}"),
            (Some(_), None) => println!("deleted {name}"),
            (Some(o), Some(n)) if o != n => println!("modified {name}"),
            _ => {}
        }
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// v1: watch — one epoll loop, three fds, zero threads beyond main.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Created,
    Modified,
    Deleted,
}

impl Kind {
    fn name(self) -> &'static str {
        match self {
            Kind::Created => "created",
            Kind::Modified => "modified",
            Kind::Deleted => "deleted",
        }
    }
}

fn classify(mask: AddWatchFlags) -> Option<Kind> {
    if mask.intersects(AddWatchFlags::IN_CREATE | AddWatchFlags::IN_MOVED_TO) {
        Some(Kind::Created)
    } else if mask.intersects(AddWatchFlags::IN_DELETE | AddWatchFlags::IN_MOVED_FROM) {
        Some(Kind::Deleted)
    } else if mask.intersects(AddWatchFlags::IN_MODIFY | AddWatchFlags::IN_ATTRIB) {
        Some(Kind::Modified)
    } else {
        None
    }
}

/// Coalescing rule shared by all three implementations: within one debounce
/// window, delete wins, a fresh creation stays "created" through later
/// writes, and a delete+recreate pair reads as "modified".
fn merge(old: Option<Kind>, new: Kind) -> Kind {
    match (old, new) {
        (None, k) => k,
        (_, Kind::Deleted) => Kind::Deleted,
        (Some(Kind::Created), _) => Kind::Created,
        _ => Kind::Modified,
    }
}

struct Pending {
    kind: Kind,
    due: Instant,
}

const TOK_INOTIFY: u64 = 0;
const TOK_TIMER: u64 = 1;
const TOK_SIGNAL: u64 = 2;

fn arm_timer(timer: &TimerFd, rel: Duration) -> Result<()> {
    // A zero TimeSpec would disarm the timer; fire "immediately" instead.
    let rel = rel.max(Duration::from_nanos(1));
    timer
        .set(Expiration::OneShot(TimeSpec::from_duration(rel)), TimerSetTimeFlags::empty())
        .context("timerfd_settime")?;
    Ok(())
}

fn flush(pending: &mut BTreeMap<String, Pending>, now: Instant, all: bool) {
    // BTreeMap iterates in name order, so a batch flush is deterministic.
    let due: Vec<String> = pending
        .iter()
        .filter(|(_, p)| all || p.due <= now)
        .map(|(name, _)| name.clone())
        .collect();
    for name in due {
        let p = pending.remove(&name).expect("collected from this map");
        println!("event: {} {}", p.kind.name(), name);
    }
}

/// The watch loop itself. Whatever sandboxing the caller applied (Landlock +
/// seccomp) has already happened before this runs; the loop's own logic
/// never needs to know.
fn run_watch_loop(dir: &str, timeout_ms: u64) -> Result<()> {
    let inotify =
        Inotify::init(InitFlags::IN_NONBLOCK | InitFlags::IN_CLOEXEC).context("inotify_init1")?;
    let watch_mask = AddWatchFlags::IN_CREATE
        | AddWatchFlags::IN_MODIFY
        | AddWatchFlags::IN_ATTRIB
        | AddWatchFlags::IN_DELETE
        | AddWatchFlags::IN_MOVED_FROM
        | AddWatchFlags::IN_MOVED_TO;
    inotify.add_watch(dir, watch_mask).with_context(|| dir.to_string())?;

    let timer = TimerFd::new(
        ClockId::CLOCK_MONOTONIC,
        TimerFlags::TFD_NONBLOCK | TimerFlags::TFD_CLOEXEC,
    )
    .context("timerfd_create")?;

    let mut sigset = SigSet::empty();
    sigset.add(Signal::SIGINT);
    sigset.add(Signal::SIGTERM);
    sigset.thread_block().context("sigprocmask")?;
    let sigfd = SignalFd::with_flags(&sigset, SfdFlags::SFD_NONBLOCK | SfdFlags::SFD_CLOEXEC)
        .context("signalfd")?;

    let epoll = Epoll::new(EpollCreateFlags::EPOLL_CLOEXEC).context("epoll_create1")?;
    epoll.add(&inotify, EpollEvent::new(EpollFlags::EPOLLIN, TOK_INOTIFY)).context("epoll_ctl")?;
    epoll.add(&timer, EpollEvent::new(EpollFlags::EPOLLIN, TOK_TIMER)).context("epoll_ctl")?;
    epoll.add(&sigfd, EpollEvent::new(EpollFlags::EPOLLIN, TOK_SIGNAL)).context("epoll_ctl")?;

    eprintln!("fwatch: watching {dir}");
    let deadline = Instant::now() + Duration::from_millis(timeout_ms);
    let mut pending: BTreeMap<String, Pending> = BTreeMap::new();

    loop {
        // One timer covers both jobs: it is always armed to whichever comes
        // first — the overall timeout or the earliest per-path debounce.
        let next = pending.values().map(|p| p.due).min().map_or(deadline, |d| d.min(deadline));
        arm_timer(&timer, next.saturating_duration_since(Instant::now()))?;

        let mut events = [EpollEvent::empty(); 8];
        let n = match epoll.wait(&mut events, EpollTimeout::NONE) {
            Ok(n) => n,
            Err(Errno::EINTR) => continue,
            Err(e) => return Err(e).context("epoll_wait"),
        };

        for event in &events[..n] {
            match event.data() {
                TOK_INOTIFY => loop {
                    let batch = match inotify.read_events() {
                        Ok(batch) => batch,
                        Err(Errno::EAGAIN) => break, // queue drained
                        Err(e) => return Err(e).context("inotify read"),
                    };
                    let now = Instant::now();
                    for ev in batch {
                        let Some(name) = ev.name else {
                            continue; // event on the directory itself
                        };
                        let Some(kind) = classify(ev.mask) else {
                            continue;
                        };
                        let name = name.to_string_lossy().into_owned();
                        let old = pending.get(&name).map(|p| p.kind);
                        pending
                            .insert(name, Pending { kind: merge(old, kind), due: now + DEBOUNCE });
                    }
                },
                TOK_TIMER => {
                    let mut buf = [0u8; 8];
                    let _ = nix::unistd::read(&timer, &mut buf);
                    let now = Instant::now();
                    if now >= deadline {
                        flush(&mut pending, now, true);
                        println!("fwatch: exiting (timeout)");
                        return Ok(());
                    }
                    flush(&mut pending, now, false);
                }
                TOK_SIGNAL => {
                    let _ = sigfd.read_signal();
                    flush(&mut pending, Instant::now(), true);
                    println!("fwatch: exiting (signal)");
                    return Ok(());
                }
                _ => {}
            }
        }
    }
}

fn cmd_watch(dir: &str, timeout_ms: u64, sandbox: bool) -> Result<()> {
    if sandbox {
        let abi = landlock_abi();
        if abi <= 0 {
            bail!("Landlock not supported by this kernel");
        }
        apply_landlock(dir)?;
        eprintln!("fwatch: landlock ABI={abi} enforced");

        let n = install_seccomp(&watch_syscalls(), libc::EPERM)?;
        eprintln!("fwatch: seccomp filter installed ({n} syscalls allowed)");
    }
    run_watch_loop(dir, timeout_ms)
}

// ---------------------------------------------------------------------------
// probes — negative controls, one Landlock, one seccomp, exercised alone.
// ---------------------------------------------------------------------------

// Exit codes distinct from the ordinary 0/1/2 contract: a verifier can tell
// "confirmed denied" (the PASSING case for a negative control) apart from
// "some other error" or "was NOT denied" (a sandbox bug).
const PROBE_DENIED: u8 = 20;
const PROBE_NOT_DENIED: u8 = 21;

fn cmd_probe_outside(sandbox_dir: &str, outside_path: &str) -> ExitCode {
    let abi = landlock_abi();
    if abi <= 0 {
        eprintln!("fwatch: probe: Landlock not supported by this kernel");
        return ExitCode::from(1);
    }
    if let Err(e) = apply_landlock(sandbox_dir) {
        eprintln!("fwatch: probe: landlock: {e:#}");
        return ExitCode::from(1);
    }
    eprintln!("fwatch: landlock ABI={abi} enforced");

    match std::fs::File::open(outside_path) {
        Ok(f) => {
            drop(f);
            println!("fwatch: probe outside {outside_path}: opened (landlock did NOT block this)");
            ExitCode::from(PROBE_NOT_DENIED)
        }
        Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => {
            println!("fwatch: probe outside {outside_path}: EACCES ({e})");
            ExitCode::from(PROBE_DENIED)
        }
        Err(e) => {
            println!("fwatch: probe outside {outside_path}: unexpected error: {e}");
            ExitCode::from(1)
        }
    }
}

fn cmd_probe_forbidden_syscall() -> ExitCode {
    // socket(2) is deliberately absent from the allowlist.
    let n = match install_seccomp(&watch_syscalls(), libc::EPERM) {
        Ok(n) => n,
        Err(e) => {
            eprintln!("fwatch: probe: seccomp: {e:#}");
            return ExitCode::from(1);
        }
    };
    eprintln!("fwatch: seccomp filter installed ({n} syscalls allowed)");

    // SAFETY: socket(2) with these arguments has no memory-safety
    // implications; we only inspect its return value.
    let s = unsafe { libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0) };
    if s >= 0 {
        unsafe { libc::close(s) };
        println!("fwatch: probe forbidden-syscall: socket() unexpectedly succeeded");
        return ExitCode::from(PROBE_NOT_DENIED);
    }
    let err = std::io::Error::last_os_error();
    if err.raw_os_error() == Some(libc::EPERM) {
        println!("fwatch: probe forbidden-syscall: EPERM ({err})");
        ExitCode::from(PROBE_DENIED)
    } else {
        println!("fwatch: probe forbidden-syscall: unexpected error: {err}");
        ExitCode::from(1)
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn usage() -> ExitCode {
    eprintln!("usage: fwatch <command>");
    eprintln!("  snapshot DIR                              one line per regular file");
    eprintln!("  diff OLD NEW                              compare two snapshots");
    eprintln!("  watch DIR [--timeout-ms T]                unsandboxed watch");
    eprintln!("  watch --sandbox DIR [--timeout-ms T]      Landlock+seccomp sandboxed watch");
    eprintln!("  probe --sandbox DIR --outside PATH        negative control: open PATH (outside DIR) under Landlock");
    eprintln!("  probe --forbidden-syscall                 negative control: socket(2) under a seccomp allowlist that omits it");
    ExitCode::from(2)
}

fn parse_watch_args(args: &[&str]) -> Option<(String, u64, bool)> {
    let mut sandbox = false;
    let mut dir: Option<String> = None;
    let mut timeout_ms = 2000u64;
    let mut i = 0;
    while i < args.len() {
        match args[i] {
            "--sandbox" if i + 1 < args.len() => {
                sandbox = true;
                dir = Some(args[i + 1].to_string());
                i += 2;
            }
            "--timeout-ms" if i + 1 < args.len() => {
                timeout_ms = args[i + 1].parse().ok().filter(|v| *v > 0)?;
                i += 2;
            }
            other if dir.is_none() => {
                dir = Some(other.to_string());
                i += 1;
            }
            _ => return None,
        }
    }
    Some((dir?, timeout_ms, sandbox))
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let argv: Vec<&str> = args.iter().map(String::as_str).collect();
    if argv.is_empty() {
        return usage();
    }

    match argv[0] {
        "snapshot" if argv.len() == 2 => match cmd_snapshot(argv[1]) {
            Ok(()) => ExitCode::SUCCESS,
            Err(e) => {
                eprintln!("fwatch: snapshot: {e:#}");
                ExitCode::from(1)
            }
        },
        "diff" if argv.len() == 3 => match cmd_diff(argv[1], argv[2]) {
            Ok(()) => ExitCode::SUCCESS,
            Err(e) => {
                eprintln!("fwatch: diff: {e:#}");
                ExitCode::from(1)
            }
        },
        "watch" => {
            let Some((dir, timeout_ms, sandbox)) = parse_watch_args(&argv[1..]) else {
                return usage();
            };
            match cmd_watch(&dir, timeout_ms, sandbox) {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("fwatch: watch: {e:#}");
                    ExitCode::from(1)
                }
            }
        }
        "probe" => {
            let mut sandbox_dir: Option<&str> = None;
            let mut outside: Option<&str> = None;
            let mut forbidden_syscall = false;
            let rest = &argv[1..];
            let mut i = 0;
            while i < rest.len() {
                match rest[i] {
                    "--sandbox" if i + 1 < rest.len() => {
                        sandbox_dir = Some(rest[i + 1]);
                        i += 2;
                    }
                    "--outside" if i + 1 < rest.len() => {
                        outside = Some(rest[i + 1]);
                        i += 2;
                    }
                    "--forbidden-syscall" => {
                        forbidden_syscall = true;
                        i += 1;
                    }
                    _ => return usage(),
                }
            }
            match (sandbox_dir, outside, forbidden_syscall) {
                (None, None, true) => cmd_probe_forbidden_syscall(),
                (Some(d), Some(p), false) => cmd_probe_outside(d, p),
                _ => usage(),
            }
        }
        _ => usage(),
    }
}
