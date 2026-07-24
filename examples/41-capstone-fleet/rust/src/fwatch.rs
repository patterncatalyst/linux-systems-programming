// fwatch.rs — the book's recurring file watcher (ch07 polling -> ch09
// inotify/epoll -> ch33 Landlock+seccomp), reduced here to what the fleet
// needs: watch a directory, print one line per raw create/write/delete
// inotify event (no debounce/merge — go/fwatch.go prints every event as it
// arrives, and this port matches that exactly, unlike ex09/ex33's fuller
// debounced watcher), and optionally run under a Landlock ruleset that
// restricts reads to that directory. Landlock is wired exactly as ex33's
// fwatch does (the `landlock` crate for the ruleset, a raw syscall(2) ABI
// probe since landlock_create_ruleset has no libc/crate wrapper for the
// version-query form); the inotify fd comes from ex09's `nix::sys::inotify`
// usage. Full seccomp allow-listing was proven once in ex33 and is not
// re-derived here — capability-bounding-set drop (caps.rs) plus Landlock are
// this capstone's sandboxing layers, applied by pmon before it execs fwatch.
use std::fs;
use std::os::fd::AsFd;
use std::path::Path;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use landlock::{
    AccessFs, PathBeneath, PathFd, Ruleset, RulesetAttr, RulesetCreatedAttr, RulesetStatus,
};
use nix::errno::Errno;
use nix::poll::{PollFd, PollFlags, PollTimeout, poll};
use nix::sys::inotify::{AddWatchFlags, InitFlags, Inotify};

use crate::util;

// ---------------------------------------------------------------------------
// Landlock — raw syscall for the ABI probe (no libc/crate wrapper exists for
// the version-query form of landlock_create_ruleset); the ruleset itself
// goes through the `landlock` crate, exactly ex33's technique.
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
        libc::syscall(
            SYS_LANDLOCK_CREATE_RULESET,
            std::ptr::null::<u8>(),
            0usize,
            LANDLOCK_CREATE_RULESET_VERSION,
        )
    };
    if v < 0 { 0 } else { v as i32 }
}

/// Restricts filesystem reads to `dir` (ReadFile + ReadDir, both available
/// since ABI v1) and confirms the kernel actually enforced it (not merely
/// "best-effort, kernel too old").
fn apply_landlock(dir: &str) -> Result<(), String> {
    let access = AccessFs::ReadFile | AccessFs::ReadDir;
    let status = Ruleset::default()
        .handle_access(access)
        .map_err(|e| format!("landlock: handle_access: {e}"))?
        .create()
        .map_err(|e| format!("landlock: create ruleset: {e}"))?
        .add_rule(PathBeneath::new(
            PathFd::new(dir).map_err(|e| format!("landlock: open {dir}: {e}"))?,
            access,
        ))
        .map_err(|e| format!("landlock: add_rule: {e}"))?
        .restrict_self()
        .map_err(|e| format!("landlock: restrict_self: {e}"))?;
    if status.ruleset != RulesetStatus::FullyEnforced {
        return Err(format!(
            "landlock ruleset not fully enforced by this kernel ({:?})",
            status.ruleset
        ));
    }
    Ok(())
}

// classify mirrors go/fwatch.go's eventName: create, then delete, then
// modify (CLOSE_WRITE or MODIFY), else "other" — checked in that priority
// order because a single inotify event's mask can carry more than one bit.
fn classify(mask: AddWatchFlags) -> &'static str {
    if mask.contains(AddWatchFlags::IN_CREATE) {
        "create"
    } else if mask.contains(AddWatchFlags::IN_DELETE) {
        "delete"
    } else if mask.intersects(AddWatchFlags::IN_CLOSE_WRITE | AddWatchFlags::IN_MODIFY) {
        "modify"
    } else {
        "other"
    }
}

pub fn snapshot(dir: &str) -> i32 {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("fwatch: readdir {dir}: {e}");
            return 1;
        }
    };
    // os.ReadDir (the Go reference) returns entries sorted by filename;
    // std::fs::read_dir makes no such guarantee, so the names are collected
    // and sorted explicitly to keep this deterministic across languages.
    let mut names: Vec<String> = Vec::new();
    for entry in entries.flatten() {
        if entry.file_type().map(|ft| ft.is_file()).unwrap_or(false) {
            names.push(entry.file_name().to_string_lossy().into_owned());
        }
    }
    names.sort();
    for name in names {
        println!("{name}");
    }
    0
}

pub fn watch(dir: &str, sandbox: bool, timeout_ms: i64) -> i32 {
    if sandbox {
        let abi = landlock_abi();
        if abi == 0 {
            eprintln!("fwatch: Landlock not supported by this kernel");
            return 1;
        }
        if let Err(e) = apply_landlock(dir) {
            eprintln!("fwatch: landlock: {e}");
            return 1;
        }
        eprintln!("fwatch: landlock ABI={abi} enforced dir={dir}");
    }

    let inotify = match Inotify::init(InitFlags::IN_CLOEXEC | InitFlags::IN_NONBLOCK) {
        Ok(i) => i,
        Err(e) => {
            eprintln!("fwatch: inotify_init1: {e}");
            return 1;
        }
    };
    let mask = AddWatchFlags::IN_CREATE
        | AddWatchFlags::IN_MODIFY
        | AddWatchFlags::IN_DELETE
        | AddWatchFlags::IN_CLOSE_WRITE;
    if let Err(e) = inotify.add_watch(dir, mask) {
        eprintln!("fwatch: inotify_add_watch {dir}: {e}");
        return 1;
    }

    eprintln!("fwatch: watching {dir} (sandbox={sandbox})");

    let deadline =
        (timeout_ms > 0).then(|| Instant::now() + Duration::from_millis(timeout_ms as u64));
    let sig = util::install_signal_flag();

    loop {
        if sig.load(Ordering::Relaxed) {
            println!("(signal)");
            return 0;
        }
        if let Some(d) = deadline
            && Instant::now() > d
        {
            println!("(timeout)");
            return 0;
        }

        let mut fds = [PollFd::new(inotify.as_fd(), PollFlags::POLLIN)];
        match poll(&mut fds, PollTimeout::from(200u16)) {
            Ok(0) => continue,
            Ok(_) => {}
            Err(Errno::EINTR) => continue,
            Err(e) => {
                eprintln!("fwatch: poll: {e}");
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

        loop {
            match inotify.read_events() {
                Ok(batch) => {
                    for ev in batch {
                        let Some(name) = ev.name else {
                            continue; // event on the directory itself
                        };
                        let kind = classify(ev.mask);
                        let path = Path::new(dir).join(name.to_string_lossy().into_owned());
                        println!("event: {kind} {}", path.display());
                    }
                }
                Err(Errno::EAGAIN) => break, // queue drained
                Err(e) => {
                    eprintln!("fwatch: read: {e}");
                    return 1;
                }
            }
        }
    }
}
