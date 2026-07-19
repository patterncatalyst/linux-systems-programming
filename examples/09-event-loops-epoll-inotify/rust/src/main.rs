// fwatch v1 — a file watcher growing chapter by chapter.
//
// v0 subcommands (snapshot/diff) still work; v1 adds `watch`, built exactly
// like the C++ version: ONE epoll loop over three kernel file descriptors on
// a single thread —
//
//   inotify fd   — filesystem events for the watched directory
//   timerfd      — the debounce (100 ms per path) AND the overall timeout
//   signalfd     — SIGINT/SIGTERM delivered as ordinary readable data
//
// nix wraps each fd in an owning type (OwnedFd underneath), so every resource
// closes on drop; the readiness model stays explicit.

use std::collections::{BTreeMap, BTreeSet};
use std::os::linux::fs::MetadataExt;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use nix::errno::Errno;
use nix::sys::epoll::{Epoll, EpollCreateFlags, EpollEvent, EpollFlags, EpollTimeout};
use nix::sys::inotify::{AddWatchFlags, InitFlags, Inotify};
use nix::sys::signal::{SigSet, Signal};
use nix::sys::signalfd::{SfdFlags, SignalFd};
use nix::sys::time::TimeSpec;
use nix::sys::timerfd::{ClockId, Expiration, TimerFd, TimerFlags, TimerSetTimeFlags};

const DEBOUNCE: Duration = Duration::from_millis(100);

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

fn cmd_watch(dir: &str, timeout_ms: u64) -> Result<()> {
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

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn usage() -> ExitCode {
    eprintln!("usage: fwatch <command>");
    eprintln!("  snapshot DIR                  one line per regular file: name<TAB>size<TAB>mtime_ns");
    eprintln!("  diff OLD NEW                  compare two snapshots: created|modified|deleted <name>");
    eprintln!("  watch DIR [--timeout-ms T]    watch DIR (default 2000 ms) until timeout or SIGINT/SIGTERM");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (cmd, result) = match args.iter().map(String::as_str).collect::<Vec<_>>()[..] {
        ["snapshot", dir] => ("snapshot", cmd_snapshot(dir)),
        ["diff", old, new] => ("diff", cmd_diff(old, new)),
        ["watch", dir] => ("watch", cmd_watch(dir, 2000)),
        ["watch", dir, "--timeout-ms", t] => match t.parse::<u64>() {
            Ok(ms) if ms > 0 => ("watch", cmd_watch(dir, ms)),
            _ => return usage(),
        },
        _ => return usage(),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("fwatch: {cmd}: {err:#}");
            ExitCode::from(1)
        }
    }
}
