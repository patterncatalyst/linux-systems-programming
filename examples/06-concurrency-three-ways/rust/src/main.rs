// parhash: parallel FNV-1a 64 checksummer — Rust flavor.
//
// Concurrency shape: 4 scoped threads share one mpsc Receiver behind a Mutex
// and pull relative paths from it; results flow back to the main thread on a
// second mpsc channel. SIGINT sets a shared AtomicBool (signal-hook's flag
// registry): the walker stops sending, each worker finishes the file it is on
// and refuses further work, and main prints whatever completed plus
// "parhash: interrupted" (exit 130).

use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex};

use anyhow::{Context, Result};

const WORKERS: usize = 4;
const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

/// Stream a file through FNV-1a 64 in 64 KiB chunks.
fn hash_file(path: &Path) -> Result<u64> {
    let mut file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let mut buf = vec![0u8; 64 * 1024];
    let mut hash = FNV_OFFSET;
    loop {
        let n = file
            .read(&mut buf)
            .with_context(|| format!("read {}", path.display()))?;
        if n == 0 {
            return Ok(hash);
        }
        for &byte in &buf[..n] {
            hash ^= u64::from(byte);
            hash = hash.wrapping_mul(FNV_PRIME);
        }
    }
}

/// Recursively send the relative path of every regular file under `dir`,
/// stopping early once `interrupted` is set. Unreadable subtrees are skipped.
fn walk(dir: &Path, rel: &Path, interrupted: &AtomicBool, tx: &mpsc::Sender<String>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        if interrupted.load(Ordering::Relaxed) {
            return; // interrupted: stop accepting work
        }
        let Ok(file_type) = entry.file_type() else {
            continue;
        };
        let child_rel = rel.join(entry.file_name());
        if file_type.is_dir() {
            walk(&entry.path(), &child_rel, interrupted, tx);
        } else if file_type.is_file() {
            let _ = tx.send(child_rel.to_string_lossy().into_owned());
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let [_, ref root] = args[..] else {
        eprintln!("usage: parhash DIR");
        return ExitCode::from(2);
    };
    let root = PathBuf::from(root);
    if !root.is_dir() {
        eprintln!("parhash: cannot walk {}", root.display());
        return ExitCode::from(1);
    }

    let interrupted = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal_hook::consts::SIGINT, Arc::clone(&interrupted))
        .expect("register SIGINT flag");

    let (path_tx, path_rx) = mpsc::channel::<String>();
    let (result_tx, result_rx) = mpsc::channel::<(String, u64)>();
    let path_rx = Arc::new(Mutex::new(path_rx));

    let mut results: Vec<(String, u64)> = Vec::new();
    std::thread::scope(|scope| {
        for _ in 0..WORKERS {
            let path_rx = Arc::clone(&path_rx);
            let result_tx = result_tx.clone();
            let interrupted = Arc::clone(&interrupted);
            let root = &root;
            scope.spawn(move || {
                loop {
                    if interrupted.load(Ordering::Relaxed) {
                        return; // refuse queued-but-unstarted work
                    }
                    // Lock only around recv so dequeues serialize but hashing
                    // runs in parallel across all four workers.
                    let Ok(rel) = path_rx.lock().expect("receiver lock").recv() else {
                        return; // walker done and queue drained
                    };
                    match hash_file(&root.join(&rel)) {
                        Ok(sum) => {
                            let _ = result_tx.send((rel, sum));
                        }
                        Err(_) => eprintln!("parhash: skipping {rel}"),
                    }
                }
            });
        }
        drop(result_tx); // main keeps no sender: result_rx ends when workers do

        walk(&root, Path::new(""), &interrupted, &path_tx);
        drop(path_tx); // walker done: idle workers see the channel close

        for entry in result_rx {
            results.push(entry); // drains until every in-flight hash lands
        }
    });

    results.sort_by(|a, b| a.0.cmp(&b.0));
    for (rel, sum) in &results {
        println!("{sum:016x}  {rel}");
    }
    if interrupted.load(Ordering::Relaxed) {
        eprintln!("parhash: interrupted");
        return ExitCode::from(130);
    }
    println!("parhash: {} files, {WORKERS} workers", results.len());
    ExitCode::SUCCESS
}
