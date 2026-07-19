// app — a tiny PID-1 container entrypoint (chapter 34: our programs in
// containers).
//
//   app serve   run as the container's PID 1: reap children, forward SIGTERM
//               to the supervised worker, report the REAL container limits.
//   app naive   the trap: no signal handling installed at all. A process
//               that is PID 1 of its own PID namespace (i.e. the container's
//               entrypoint) is special-cased by the kernel: any signal for
//               which it has installed no explicit handler is dropped
//               instead of running that signal's default action. Only
//               SIGKILL (and SIGSTOP) still work. `naive` never registers a
//               handler, so `podman stop` cannot end it gracefully -- the
//               engine has to wait out the stop timeout and fall back to
//               SIGKILL.
//   app worker  the long-running child `serve` supervises (self-reexec).
//   app job     a short-lived child `serve` spawns periodically (self-reexec).
//
// Reaping, the Rust way: like the Go version (and unlike the C++ one), there
// is no signalfd/SIGCHLD anywhere here. std::process::Child::wait() calls
// waitpid(2) for you; parking one std::thread per spawned child in wait() is
// the reap loop. signal_hook::iterator::Signals turns SIGTERM/SIGINT into a
// blocking iterator on the main thread -- ordinary code, nothing async-
// signal-unsafe runs in a handler.
//
// Container-aware resource detection: std::thread::available_parallelism()
// on Linux divides the cgroup's cpu.max quota/period when it is lower than
// the affinity mask count -- cgroup-aware, like Go's GOMAXPROCS. Compare
// this program's number against the C++ version's hardware_concurrency()
// (which reports the host's cpu count no matter what `--cpus` says).

use std::fs;
use std::io::Read;
use std::process::{Child, Command};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result};
use nix::sys::signal::{Signal, kill};
use nix::unistd::Pid;
use signal_hook::consts::{SIGINT, SIGTERM};
use signal_hook::iterator::Signals;

fn usage() -> ! {
    eprintln!("usage: app serve|naive|worker|job");
    std::process::exit(2);
}

fn die(err: anyhow::Error) -> ! {
    eprintln!("app: error: {err:#}");
    std::process::exit(1);
}

/// Read a whole file, trailing whitespace trimmed. Empty string (not an
/// error) if the file cannot be opened -- cgroup files may be absent on
/// unusual hosts, and callers fall back to "unknown".
fn read_trim(path: &str) -> String {
    let mut buf = String::new();
    match fs::File::open(path).and_then(|mut f| f.read_to_string(&mut buf)) {
        Ok(_) => buf.trim_end_matches(['\n', '\r', ' ']).to_string(),
        Err(_) => String::new(),
    }
}

/// The cgroup v2 unified path this process is itself in: /proc/self/cgroup
/// has exactly one "0::<path>" line. Falls back to root if unreadable (e.g.
/// a non-cgroup-v2 host).
fn own_cgroup_path() -> String {
    let content = read_trim("/proc/self/cgroup");
    for line in content.lines() {
        if let Some(path) = line.strip_prefix("0::") {
            return path.to_string();
        }
    }
    "/".to_string()
}

/// Read a cgroup v2 controller file for THIS process's own cgroup. Inside a
/// container the cgroup namespace already makes "/sys/fs/cgroup" the
/// container's own slice, so the root-relative fallback covers that case
/// too.
fn read_own_cgroup_file(name: &str) -> String {
    let base = format!("/sys/fs/cgroup{}", own_cgroup_path());
    let v = read_trim(&format!("{base}/{name}"));
    if !v.is_empty() {
        return v;
    }
    read_trim(&format!("/sys/fs/cgroup/{name}"))
}

/// "max" or "<quota>/<period>", derived from cpu.max's raw
/// "max 100000" / "200000 100000" content.
fn cpu_max_display() -> String {
    let raw = read_own_cgroup_file("cpu.max");
    if raw.is_empty() {
        return "unknown".to_string();
    }
    match raw.split_once(' ') {
        None => raw,
        Some(("max", _)) => "max".to_string(),
        Some((quota, period)) => format!("{quota}/{period}"),
    }
}

fn mem_max_display() -> String {
    let raw = read_own_cgroup_file("memory.max");
    if raw.is_empty() { "unknown".to_string() } else { raw }
}

fn print_container_line() {
    // Cgroup-aware, unlike the C++ version's hardware_concurrency().
    let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0);
    println!(
        "container: cpu.max={} effective_parallelism={} mem.max={}",
        cpu_max_display(),
        n,
        mem_max_display()
    );
}

fn worker_run() -> i32 {
    let mut tick = 1u64;
    loop {
        thread::sleep(Duration::from_secs(2));
        println!("app: worker pid={} tick={tick}", std::process::id());
        tick += 1;
    }
}

fn job_run(seq: &str) -> i32 {
    println!("app: job pid={} seq={seq} done", std::process::id());
    0
}

/// Resolve the running binary's own path so serve can re-exec it as
/// "worker"/"job" -- like Go, Rust avoids fork(2)-without-exec (the
/// allocator and runtime are not fork-safe once threads exist), so this is
/// the idiomatic way to spawn "more of this program" as a real child
/// process.
fn self_path() -> Result<std::path::PathBuf> {
    std::env::current_exe().context("resolve self path")
}

fn serve() -> Result<i32> {
    print_container_line();
    println!("app: pid={} ppid={}", std::process::id(), nix::unistd::getppid());

    let mut signals = Signals::new([SIGTERM, SIGINT]).context("Signals::new")?;
    let self_exe = self_path()?;

    let mut worker: Child = Command::new(&self_exe)
        .arg("worker")
        .spawn()
        .context("start worker")?;
    let worker_pid = worker.id();
    println!("app: worker started pid={worker_pid}");

    // The job spawner: a background thread that periodically re-execs
    // "app job <seq>" and, in its own thread per child, waits for it --
    // that wait() call IS the reap. `stop` lets shutdown cancel the loop
    // cooperatively instead of detaching a thread that outlives serve().
    let stop = Arc::new(AtomicBool::new(false));
    let job_spawner = {
        let stop = Arc::clone(&stop);
        let self_exe = self_exe.clone();
        thread::spawn(move || {
            let mut seq = 0u64;
            while !stop.load(Ordering::SeqCst) {
                for _ in 0..10 {
                    if stop.load(Ordering::SeqCst) {
                        return;
                    }
                    thread::sleep(Duration::from_millis(100));
                }
                seq += 1;
                match Command::new(&self_exe).args(["job", &seq.to_string()]).spawn() {
                    Ok(mut child) => {
                        thread::spawn(move || {
                            let pid = child.id();
                            match child.wait() {
                                Ok(status) => println!(
                                    "app: reaped pid={pid} status={}",
                                    status.code().unwrap_or(-1)
                                ),
                                Err(e) => println!("app: reap failed pid={pid}: {e}"),
                            }
                        });
                    }
                    Err(e) => println!("app: job spawn failed: {e}"),
                }
            }
        })
    };

    // Block on the signal iterator: this is the pid-1 duty naive skips.
    let sig = signals.forever().next().expect("signal stream ended");
    stop.store(true, Ordering::SeqCst);
    let _ = job_spawner.join();
    if kill(Pid::from_raw(worker_pid as i32), Signal::SIGTERM).is_ok() {
        let _ = worker.wait();
    }
    println!(
        "app: shutting down ({})",
        if sig == SIGTERM { "SIGTERM" } else { "SIGINT" }
    );
    Ok(0)
}

// THE OTHER HALF OF THE TRAP: naive installs no signal handling whatsoever.
// As this container's PID 1, the kernel will not run SIGTERM's default
// action (terminate) for it -- there is no handler, so the signal is simply
// dropped. `podman stop` has no way to ask it to leave; only SIGKILL ends it.
fn naive_run() -> i32 {
    print_container_line();
    println!("app: pid={} ppid={}", std::process::id(), nix::unistd::getppid());
    let mut tick = 1u64;
    loop {
        thread::sleep(Duration::from_secs(1));
        println!("app: naive heartbeat tick={tick}");
        tick += 1;
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    let code = match args[0].as_str() {
        "serve" => serve().unwrap_or_else(|e| die(e)),
        "naive" => naive_run(),
        "worker" => worker_run(),
        "job" => {
            if args.len() < 2 {
                usage();
            }
            job_run(&args[1])
        }
        _ => usage(),
    };
    std::process::exit(code);
}
