// fwatch v0 — snapshot/diff a directory tree with dirfd-relative syscalls.
//
//   fwatch snapshot DIR              one "<relpath> <size> <mtime>" line per
//                                    regular file, sorted by path
//   fwatch diff DIR SNAPSHOT_FILE    re-scan and print +/-/~ lines plus a
//                                    "fwatch: A added, R removed, M modified"
//                                    summary
//
// The walk is deliberately explicit: rustix::fs::openat relative to the
// parent directory fd (an OwnedFd — closed exactly once, on drop),
// rustix::fs::Dir to enumerate, and rustix::fs::statat with
// SYMLINK_NOFOLLOW to classify entries.

use std::collections::BTreeMap;
use std::os::fd::OwnedFd;
use std::process::ExitCode;

use anyhow::{Result, anyhow, bail};
use rustix::fs::{AtFlags, Dir, FileType, Mode, OFlags, openat, statat};

#[derive(Clone, Copy, PartialEq, Eq)]
struct Info {
    size: i64,
    mtime: i64,
}

/// Sorted by path, bytewise — BTreeMap keeps snapshot output deterministic.
type Tree = BTreeMap<String, Info>;

const DIR_FLAGS: OFlags = OFlags::RDONLY
    .union(OFlags::DIRECTORY)
    .union(OFlags::CLOEXEC)
    .union(OFlags::NOFOLLOW);

fn fail(what: &str, path: &str, errno: rustix::io::Errno) -> anyhow::Error {
    anyhow!("cannot {what} '{path}' (errno {})", errno.raw_os_error())
}

fn join(base: &str, name: &str) -> String {
    if base.is_empty() { name.to_string() } else { format!("{base}/{name}") }
}

/// Walk the directory owned by `fd`, recording every regular file into
/// `tree`. `display` is the path for error messages, `rel` the DIR-relative
/// prefix. `fd` is dropped (closed) when the walk of this level finishes.
fn walk(fd: OwnedFd, display: &str, rel: &str, tree: &mut Tree) -> Result<()> {
    // Dir::read_from dups the fd for its own cursor; `fd` stays usable as the
    // anchor for the *at() calls below.
    let dir = Dir::read_from(&fd).map_err(|e| fail("open directory", display, e))?;

    for entry in dir {
        let entry = entry.map_err(|e| fail("read directory", display, e))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if name == "." || name == ".." {
            continue;
        }

        let st = match statat(&fd, &name, AtFlags::SYMLINK_NOFOLLOW) {
            Ok(st) => st,
            Err(rustix::io::Errno::NOENT) => continue, // vanished mid-walk
            Err(e) => return Err(fail("stat", &join(display, &name), e)),
        };

        match FileType::from_raw_mode(st.st_mode) {
            FileType::RegularFile => {
                tree.insert(
                    join(rel, &name),
                    Info { size: st.st_size as i64, mtime: st.st_mtime as i64 },
                );
            }
            FileType::Directory => {
                let sub = match openat(&fd, &name, DIR_FLAGS, Mode::empty()) {
                    Ok(sub) => sub,
                    Err(rustix::io::Errno::NOENT) => continue,
                    Err(e) => return Err(fail("open directory", &join(display, &name), e)),
                };
                walk(sub, &join(display, &name), &join(rel, &name), tree)?;
            }
            // Symlinks, sockets, pipes, devices: not part of the v0 snapshot.
            _ => {}
        }
    }
    Ok(())
}

fn scan(dir: &str) -> Result<Tree> {
    let fd = rustix::fs::open(dir, DIR_FLAGS, Mode::empty())
        .map_err(|e| fail("open directory", dir, e))?;
    let mut tree = Tree::new();
    walk(fd, dir, "", &mut tree)?;
    Ok(tree)
}

/// Parse "path size mtime" lines. The path may contain spaces: size and
/// mtime are the last two space-separated fields.
fn parse_snapshot(text: &str) -> Result<Tree> {
    let mut tree = Tree::new();
    for (i, line) in text.split('\n').enumerate() {
        if line.is_empty() {
            continue;
        }
        let malformed = || anyhow!("malformed snapshot line {}", i + 1);

        let mut fields = line.rsplitn(3, ' ');
        let mtime_s = fields.next().ok_or_else(malformed)?;
        let size_s = fields.next().ok_or_else(malformed)?;
        let path = fields.next().ok_or_else(malformed)?;
        if path.is_empty() {
            bail!(malformed());
        }

        let size: i64 = size_s.parse().map_err(|_| malformed())?;
        let mtime: i64 = mtime_s.parse().map_err(|_| malformed())?;
        tree.insert(path.to_string(), Info { size, mtime });
    }
    Ok(tree)
}

fn cmd_snapshot(dir: &str) -> Result<()> {
    for (path, info) in scan(dir)? {
        println!("{} {} {}", path, info.size, info.mtime);
    }
    Ok(())
}

fn cmd_diff(dir: &str, snapshot_path: &str) -> Result<()> {
    let text = std::fs::read_to_string(snapshot_path).map_err(|e| {
        anyhow!(
            "cannot read snapshot '{snapshot_path}' (errno {})",
            e.raw_os_error().unwrap_or(0)
        )
    })?;
    let before = parse_snapshot(&text)?;
    let after = scan(dir)?;

    // Sorted union of both key sets.
    let mut paths: Vec<&String> = before.keys().chain(after.keys()).collect();
    paths.sort();
    paths.dedup();

    let (mut added, mut removed, mut modified) = (0, 0, 0);
    for path in paths {
        match (before.get(path), after.get(path)) {
            (None, Some(_)) => {
                println!("+ {path}");
                added += 1;
            }
            (Some(_), None) => {
                println!("- {path}");
                removed += 1;
            }
            (Some(old), Some(now)) if old != now => {
                println!("~ {path}");
                modified += 1;
            }
            _ => {}
        }
    }
    println!("fwatch: {added} added, {removed} removed, {modified} modified");
    Ok(())
}

fn usage() -> ExitCode {
    eprintln!("usage: fwatch snapshot DIR");
    eprintln!("       fwatch diff DIR SNAPSHOT_FILE");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.iter().map(String::as_str).collect::<Vec<_>>()[..] {
        ["snapshot", dir] => cmd_snapshot(dir),
        ["diff", dir, snap] => cmd_diff(dir, snap),
        _ => return usage(),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("fwatch: error: {e}");
            ExitCode::from(1)
        }
    }
}
