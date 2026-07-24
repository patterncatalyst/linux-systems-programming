// pmon.rs — the book's recurring process supervisor (ch11-14, 18-19, 32,
// 34), grown here into the capstone's fleet init: it drops the capability
// bounding set once (caps.rs), then fork/execs (self-re-exec via
// /proc/self/exe, the same technique ch34's container entrypoint uses)
// chatterd, sysagent, and fwatch as children, restarting any of them that
// exits unexpectedly, forwarding SIGTERM/SIGINT to all three on its own
// shutdown, and printing a health line every tick so an operator (or
// verify.lua, over ssh) can poll fleet state without touching /proc.
//
// One thread per supervised service (matching pmon.cpp's std::thread pool),
// each doing its own fork/waitpid retry loop; a dedicated thread blocks
// SIGTERM/SIGINT and consumes them via sigwait(2) (nix's `SigSet::wait`),
// exactly pmon.cpp's rationale for using sigwait over a signal handler: it
// avoids doing anything async-signal-unsafe on delivery. Every forked child
// clears its own inherited copy of that mask right after fork, before
// execve, so a SIGTERM forwarded to it can actually reach its own handling
// (the C++ port documents the same fix, carried over from ch34's container
// entrypoint).
use std::collections::HashMap;
use std::ffi::CString;
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use nix::sys::signal::{SigSet, Signal, kill};
use nix::sys::wait::{WaitStatus, waitpid};
use nix::unistd::{ForkResult, Pid, execv, fork};

use crate::caps;

struct ChildSpec {
    name: &'static str,
    args: Vec<String>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Starting,
    Up,
    Restarting,
    Down,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            State::Starting => "starting",
            State::Up => "up",
            State::Restarting => "restarting",
            State::Down => "down",
        }
    }
}

struct ChildState {
    state: Mutex<State>,
    restarts: AtomicI64,
    pid: AtomicI32,
}

impl ChildState {
    fn new() -> Self {
        ChildState {
            state: Mutex::new(State::Starting),
            restarts: AtomicI64::new(0),
            pid: AtomicI32::new(0),
        }
    }
    fn set_state(&self, s: State) {
        *self.state.lock().unwrap() = s;
    }
    fn get_state(&self) -> State {
        *self.state.lock().unwrap()
    }
}

fn describe_wait_status(status: nix::Result<WaitStatus>) -> String {
    match status {
        Ok(WaitStatus::Exited(_, code)) => format!("exit status {code}"),
        Ok(WaitStatus::Signaled(_, sig, _)) => format!("signal: {sig}"),
        Ok(other) => format!("{other:?}"),
        Err(e) => format!("waitpid: {e}"),
    }
}

// run_child_supervisor: fork+exec `self_exe args...` in a loop, restarting on
// unexpected exit — the per-service goroutine from go/pmon.go, one OS thread
// per service here too.
fn run_child_supervisor(
    self_exe: &CString,
    args: &[String],
    cs: &ChildState,
    name: &str,
    shutting_down: &AtomicBool,
) {
    // Every CString is built once, before the first fork, so the only work
    // the child performs between fork() and execv() is clearing its signal
    // mask and calling execv() itself.
    let mut argv: Vec<CString> = Vec::with_capacity(args.len() + 1);
    argv.push(self_exe.clone());
    for a in args {
        argv.push(CString::new(a.as_str()).expect("child arg contains NUL"));
    }

    loop {
        // SAFETY: this process is multi-threaded (one supervisor thread per
        // service plus the sigwait thread below); the child branch performs
        // only the signal-mask reset and execv() before either replacing
        // itself or exiting, matching pmon.cpp's fork-then-immediately-exec
        // discipline.
        match unsafe { fork() } {
            Ok(ForkResult::Child) => {
                // Unblock what pmon::run blocked in the parent (see below):
                // a forwarded SIGTERM must reach this child's own handling,
                // not sit blocked forever.
                let empty = SigSet::empty();
                let _ = empty.thread_set_mask();
                match execv(self_exe, &argv) {
                    Ok(_) => unreachable!(),
                    Err(_) => unsafe { libc::_exit(127) }, // execv failed
                }
            }
            Ok(ForkResult::Parent { child }) => {
                cs.pid.store(child.as_raw(), Ordering::Relaxed);
                cs.set_state(State::Up);
                println!("pmon: started service={name} pid={}", child.as_raw());

                let status = waitpid(child, None);
                if shutting_down.load(Ordering::Relaxed) {
                    cs.set_state(State::Down);
                    return;
                }
                let n = cs.restarts.fetch_add(1, Ordering::Relaxed) + 1;
                cs.set_state(State::Restarting);
                let reason = describe_wait_status(status);
                println!("pmon: restart service={name} attempt={n} reason={reason}");
                thread::sleep(Duration::from_millis(300));
            }
            Err(e) => {
                eprintln!("pmon: start service={name}: {e}");
                thread::sleep(Duration::from_millis(500));
            }
        }
    }
}

fn print_health(order: &[&str], states: &HashMap<&str, Arc<ChildState>>) {
    let mut line = String::from("pmon: health ");
    for (i, name) in order.iter().enumerate() {
        if i > 0 {
            line.push(' ');
        }
        line.push_str(name);
        line.push('=');
        line.push_str(states[name].get_state().name());
    }
    line.push_str(" restarts=");
    for (i, name) in order.iter().enumerate() {
        if i > 0 {
            line.push(',');
        }
        line.push_str(name);
        line.push(':');
        line.push_str(&states[name].restarts.load(Ordering::Relaxed).to_string());
    }
    println!("{line}");
}

pub fn run(
    node: &str,
    sandbox_dir: &str,
    peer: &str,
    peer_node: &str,
    chatterd_port: u16,
    health_interval_ms: u64,
) -> i32 {
    if let Err(e) = fs::create_dir_all(sandbox_dir) {
        eprintln!("pmon: mkdir {sandbox_dir}: {e}");
        return 1;
    }

    caps::drop_bounding_set();

    let self_exe_path = match fs::read_link("/proc/self/exe") {
        Ok(p) => p,
        Err(e) => {
            eprintln!("pmon: readlink /proc/self/exe: {e}");
            return 1;
        }
    };
    let self_exe = match CString::new(self_exe_path.as_os_str().as_bytes()) {
        Ok(c) => c,
        Err(_) => {
            eprintln!("pmon: self exe path contains NUL");
            return 1;
        }
    };

    let mut chatterd_args = vec![
        "chatterd".to_string(),
        "serve".to_string(),
        "--host".to_string(),
        "0.0.0.0".to_string(),
        "--port".to_string(),
        chatterd_port.to_string(),
        "--node".to_string(),
        node.to_string(),
    ];
    if !peer.is_empty() {
        chatterd_args.push("--peer".to_string());
        chatterd_args.push(peer.to_string());
        chatterd_args.push("--peer-node".to_string());
        chatterd_args.push(peer_node.to_string());
    }

    let specs = vec![
        ChildSpec {
            name: "chatterd",
            args: chatterd_args,
        },
        ChildSpec {
            name: "sysagent",
            args: vec![
                "sysagent".to_string(),
                "--node".to_string(),
                node.to_string(),
                "--interval-ms".to_string(),
                "2000".to_string(),
            ],
        },
        ChildSpec {
            name: "fwatch",
            args: vec![
                "fwatch".to_string(),
                "watch".to_string(),
                sandbox_dir.to_string(),
                "--sandbox".to_string(),
            ],
        },
    ];

    let order: [&'static str; 3] = ["chatterd", "sysagent", "fwatch"];
    let mut states: HashMap<&'static str, Arc<ChildState>> = HashMap::new();
    for name in order {
        states.insert(name, Arc::new(ChildState::new()));
    }

    let shutting_down = Arc::new(AtomicBool::new(false));

    // Block SIGTERM/SIGINT in THIS thread before spawning anything: the
    // dedicated sigwait thread below is the only one that ever consumes
    // them; every forked child unblocks its own inherited copy of the mask
    // before execve (see run_child_supervisor).
    let mut mask = SigSet::empty();
    mask.add(Signal::SIGTERM);
    mask.add(Signal::SIGINT);
    mask.thread_block().expect("pthread_sigmask");

    {
        let shutting_down = shutting_down.clone();
        let states_for_signal: Vec<Arc<ChildState>> =
            order.iter().map(|n| states[n].clone()).collect();
        thread::spawn(move || {
            if mask.wait().is_ok() {
                shutting_down.store(true, Ordering::Relaxed);
                for cs in &states_for_signal {
                    let pid = cs.pid.load(Ordering::Relaxed);
                    if pid > 0 {
                        let _ = kill(Pid::from_raw(pid), Signal::SIGTERM);
                    }
                }
            }
        });
    }

    let mut worker_handles = Vec::with_capacity(specs.len());
    for spec in specs {
        let cs = states[spec.name].clone();
        let self_exe = self_exe.clone();
        let shutting_down = shutting_down.clone();
        worker_handles.push(thread::spawn(move || {
            run_child_supervisor(&self_exe, &spec.args, &cs, spec.name, &shutting_down);
        }));
    }

    loop {
        thread::sleep(Duration::from_millis(health_interval_ms));
        if worker_handles.iter().all(|h| h.is_finished()) {
            break;
        }
        print_health(&order, &states);
    }

    println!("pmon: shutdown");
    for h in worker_handles {
        let _ = h.join();
    }
    0
}
