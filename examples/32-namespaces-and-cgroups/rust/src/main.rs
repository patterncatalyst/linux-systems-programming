//! pmon v6 — namespaces & cgroups (chapter 32).
//!
//! Subcommand:
//!   pmon containerize [--hostname NAME] [--mem-max BYTES|max]
//!                     [--cpu-max "QUOTA PERIOD"|max] [--cgroup NAME]
//!                     -- CMD [ARGS...]
//!
//! Same two primitives as the C++ build, through `nix` instead of raw libc
//! calls:
//!
//!   1. NAMESPACES — unshare(2) with CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET|
//!      CLONE_NEWPID puts the caller into a fresh mount, UTS and network
//!      namespace immediately, and arranges for the next fork(2) child to
//!      become PID 1 of a fresh PID namespace (unshare(2) never moves the
//!      caller itself into a new PID namespace — only its future children).
//!   2. CGROUP v2 LIMITS — a dedicated cgroup gets memory.max and cpu.max
//!      written before the child is ever created, so the limits are already
//!      live the instant CMD execs.
//!
//! `fork(2)` (not clone3) here: nix's `fork` gives the same "no code runs
//! between clone and exec except what we ourselves write" shape as the
//! C++ build's `clone3(flags=0)`. Its safety contract is the same as libc
//! fork's — the child may only call async-signal-safe operations until it
//! execs — which is exactly what the child below does: read its own pid,
//! read the hostname, print, exec.

use std::ffi::CString;
use std::fs;
use std::io::Write;

use nix::mount::{MsFlags, mount};
use nix::sched::{CloneFlags, unshare};
use nix::sys::wait::{WaitStatus, waitpid};
use nix::unistd::{ForkResult, Uid, execvp, fork, gethostname, getpid, sethostname};

/// Names as printed in "killed signal=<n> (<NAME>)". A hand-rolled table
/// (identical across all three implementations and to pmon's earlier
/// chapters) rather than strsignal(3) or `Signal`'s own text, both of which
/// are library/locale-dependent prose.
const SIGNAL_NAMES: [&str; 32] = [
    "", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV", "USR2",
    "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS",
];

fn signal_name(sig: i32) -> String {
    if sig >= 1 && (sig as usize) < SIGNAL_NAMES.len() {
        SIGNAL_NAMES[sig as usize].to_string()
    } else {
        format!("SIG{sig}")
    }
}

// ---- cgroup helpers --------------------------------------------------

/// Whether `tokens` (a whitespace-separated list, as cgroupfs writes it)
/// contains `word` as a whole token — "cpu" must not match inside "cpuset".
fn has_token(tokens: &str, word: &str) -> bool {
    tokens.split_whitespace().any(|t| t == word)
}

/// The root cgroup is exempt from the "no internal process" constraint, so
/// controllers can be enabled there even though it holds every process on
/// the box — this is how a fresh sibling cgroup gets memory/cpu control
/// without first relocating ourselves into a leaf.
fn ensure_root_controllers() -> Result<(), String> {
    let have = fs::read_to_string("/sys/fs/cgroup/cgroup.subtree_control")
        .map_err(|e| format!("read cgroup.subtree_control: {e}"))?;
    let mut add = String::new();
    if !has_token(&have, "memory") {
        add.push_str("+memory ");
    }
    if !has_token(&have, "cpu") {
        add.push_str("+cpu ");
    }
    if add.is_empty() {
        return Ok(());
    }
    fs::write("/sys/fs/cgroup/cgroup.subtree_control", add)
        .map_err(|e| format!("enable controllers: {e}"))
}

/// Creates (or reuses) `/sys/fs/cgroup/<name>`, applies the limits, and
/// moves the calling process into it. Every subsequent fork inherits
/// cgroup membership automatically, so CMD is under the limits from its
/// first instruction.
fn setup_cgroup(name: &str, mem_max: &str, cpu_max: &str) -> Result<String, String> {
    ensure_root_controllers()?;
    let path = format!("/sys/fs/cgroup/{name}");
    if let Err(e) = fs::create_dir(&path)
        && e.kind() != std::io::ErrorKind::AlreadyExists
    {
        return Err(format!("mkdir {path}: {e}"));
    }
    fs::write(format!("{path}/memory.max"), mem_max)
        .map_err(|e| format!("write memory.max: {e}"))?;
    fs::write(format!("{path}/cpu.max"), cpu_max).map_err(|e| format!("write cpu.max: {e}"))?;
    // Best-effort: no swap headroom, so a breach of memory.max is a real
    // OOM kill instead of a silent slowdown via swap. Not every kernel
    // exposes swap accounting the same way, so a failure here isn't fatal.
    let _ = fs::write(format!("{path}/memory.swap.max"), "0");
    fs::write(format!("{path}/cgroup.procs"), getpid().to_string())
        .map_err(|e| format!("write cgroup.procs: {e}"))?;
    Ok(path)
}

/// Extracts X from "some avg10=X avg60=Y avg300=Z total=T".
fn parse_psi_some_avg10(psi: &str) -> String {
    let Some(some_pos) = psi.find("some ") else {
        return "?".into();
    };
    let rest = &psi[some_pos..];
    let Some(a) = rest.find("avg10=") else {
        return "?".into();
    };
    let rest = &rest[a + "avg10=".len()..];
    match rest.find(' ') {
        Some(sp) => rest[..sp].to_string(),
        None => rest.to_string(),
    }
}

// ---- containerize ------------------------------------------------------

fn cmd_containerize(args: &[String]) -> i32 {
    let mut hostname = "pmon-containerized".to_string();
    let mut mem_max = "max".to_string();
    let mut cpu_max = "max 100000".to_string();
    let mut cgroup_name = String::new();
    let mut cmd: Vec<String> = Vec::new();

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--hostname" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("containerize: --hostname needs a value");
                    return 2;
                };
                hostname = v.clone();
                i += 1;
            }
            "--mem-max" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("containerize: --mem-max needs a value");
                    return 2;
                };
                mem_max = v.clone();
                i += 1;
            }
            "--cpu-max" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("containerize: --cpu-max needs a value");
                    return 2;
                };
                cpu_max = v.clone();
                i += 1;
            }
            "--cgroup" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("containerize: --cgroup needs a value");
                    return 2;
                };
                cgroup_name = v.clone();
                i += 1;
            }
            "--" => {
                cmd = args[i + 1..].to_vec();
                break;
            }
            other => {
                eprintln!("containerize: unexpected argument: {other}");
                return 2;
            }
        }
        i += 1;
    }
    if cmd.is_empty() {
        eprintln!("containerize: missing -- CMD");
        return 2;
    }
    if !Uid::effective().is_root() {
        eprintln!("containerize: must run as root");
        return 1;
    }
    if cgroup_name.is_empty() {
        cgroup_name = format!("pmon-{}", getpid());
    }

    let cgroup_path = match setup_cgroup(&cgroup_name, &mem_max, &cpu_max) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("containerize: {e}");
            return 1;
        }
    };

    // Mount, UTS and network namespaces take effect on THIS process right
    // now. The PID namespace is deferred: the next fork(2) child becomes
    // PID 1 of a fresh one; we (the caller) stay in our original PID
    // namespace so we can waitpid(2) the child the ordinary way.
    if let Err(e) = unshare(
        CloneFlags::CLONE_NEWNS
            | CloneFlags::CLONE_NEWUTS
            | CloneFlags::CLONE_NEWNET
            | CloneFlags::CLONE_NEWPID,
    ) {
        eprintln!("containerize: unshare: {e}");
        return 1;
    }

    // Detach mount propagation recursively so nothing we (or CMD) do in
    // this namespace leaks a mount event back to the host's mount table.
    if let Err(e) = mount(
        Some("none"),
        "/",
        None::<&str>,
        MsFlags::MS_REC | MsFlags::MS_PRIVATE,
        None::<&str>,
    ) {
        eprintln!("containerize: mount MS_PRIVATE: {e}");
        return 1;
    }

    // sethostname(2) now lands in the fresh UTS namespace, not the host's.
    if let Err(e) = sethostname(&hostname) {
        eprintln!("containerize: sethostname: {e}");
        return 1;
    }

    // SAFETY: the child performs only async-signal-safe operations
    // (getpid, gethostname, writes to already-open stdio fds, execvp)
    // before it either execs or _exits — it never returns up through this
    // stack frame into arbitrary Rust code, satisfying fork(2)'s contract.
    match unsafe { fork() } {
        Ok(ForkResult::Child) => {
            if getpid().as_raw() == 1 {
                println!("pmon: child sees pid 1");
            }
            if let Ok(hn) = gethostname() {
                println!("pmon: hostname={}", hn.to_string_lossy());
            }
            let _ = std::io::stdout().flush();

            let prog = CString::new(cmd[0].as_str()).unwrap_or_default();
            let cargs: Vec<CString> = cmd
                .iter()
                .map(|s| CString::new(s.as_str()).unwrap_or_default())
                .collect();
            // execvp never returns on success.
            let err = execvp(&prog, &cargs).unwrap_err();
            eprintln!("pmon: exec {}: {err}", cmd[0]);
            std::process::exit(127);
        }
        Ok(ForkResult::Parent { child }) => {
            let status = waitpid(child, None);

            if let Ok(psi) = fs::read_to_string(format!("{cgroup_path}/memory.pressure")) {
                println!(
                    "pmon: cgroup mem.pressure some={}",
                    parse_psi_some_avg10(&psi)
                );
            }

            let exit_code = match status {
                Ok(WaitStatus::Exited(_, code)) => {
                    println!("pmon: child exited status={code}");
                    code
                }
                Ok(WaitStatus::Signaled(_, sig, _)) => {
                    let n = sig as i32;
                    println!("pmon: child killed signal={n} ({})", signal_name(n));
                    128 + n
                }
                Ok(_) => 0,
                Err(e) => {
                    eprintln!("containerize: waitpid: {e}");
                    1
                }
            };

            let _ = fs::remove_dir(&cgroup_path); // best-effort cleanup

            exit_code
        }
        Err(e) => {
            eprintln!("containerize: fork: {e}");
            1
        }
    }
}

fn usage() {
    eprintln!("usage:");
    eprintln!("  pmon containerize [--hostname NAME] [--mem-max BYTES|max]");
    eprintln!("                    [--cpu-max \"QUOTA PERIOD\"|max] [--cgroup NAME]");
    eprintln!("                    -- CMD [ARGS...]");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
        std::process::exit(2);
    }
    let code = match args[1].as_str() {
        "containerize" => cmd_containerize(&args[2..]),
        _ => {
            usage();
            2
        }
    };
    std::process::exit(code);
}
