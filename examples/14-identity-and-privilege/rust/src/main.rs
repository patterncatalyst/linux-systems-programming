//! pmon v3 — identity & privilege (chapter 14).
//!
//! Subcommands:
//!   pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]
//!   pmon bindprobe [--port 80]
//!
//! `drop` runs as root and hands CMD to an unprivileged user, optionally
//! carrying exactly one capability (CAP_NET_BIND_SERVICE) across the identity
//! change and the following execvp(3) via the ambient-capability mechanism.
//! The ordering is the lesson (same as the C++ raw path):
//!
//! 1. prctl(PR_SET_KEEPCAPS, 1) before the uid change, so the permitted set
//!    survives setresuid(2).
//! 2. setgroups / setresgid / setresuid become the target user.
//! 3. capset(2) pins CAP_NET_BIND_SERVICE into permitted + inheritable
//!    (ambient requires both).
//! 4. prctl(PR_CAP_AMBIENT_RAISE) raises it into the ambient set — the only
//!    set a non-root, no-file-cap execve keeps.
//! 5. execvp(3): CMD runs as <name>, keeping :80 iff the cap was raised.
//!
//! Raw libc sequence, mirroring the C++ build; std OwnedFd gives the socket
//! RAII; errors flow through Result/?.

use std::ffi::{CStr, CString, c_char, c_void};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

// Not exported by the libc crate; both are stable kernel ABI constants.
const LINUX_CAPABILITY_VERSION_3: u32 = 0x2008_0522;
const CAP_NET_BIND_SERVICE: u32 = 10;

/// A syscall failure: what we attempted, plus the captured errno.
struct SysErr {
    what: String,
    errno: i32,
}

/// libc strerror(3), so the text matches the C++/Go builds byte-for-byte.
fn strerror(errno: i32) -> String {
    // SAFETY: strerror returns a valid NUL-terminated static string.
    unsafe { CStr::from_ptr(libc::strerror(errno)) }
        .to_string_lossy()
        .into_owned()
}

fn last_errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

/// Turn a negative syscall return into an error carrying errno.
fn check(rc: i64, what: &str) -> Result<(), SysErr> {
    if rc < 0 {
        Err(SysErr {
            what: what.to_string(),
            errno: last_errno(),
        })
    } else {
        Ok(())
    }
}

// ---- bindprobe -----------------------------------------------------------

fn cmd_bindprobe(args: &[String]) -> i32 {
    let mut port: u16 = 80;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("bindprobe: --port needs a value");
                    return 2;
                };
                match v.parse::<u16>() {
                    Ok(p) => port = p,
                    Err(_) => {
                        eprintln!("bindprobe: bad port: {v}");
                        return 2;
                    }
                }
                i += 1;
            }
            other => {
                eprintln!("bindprobe: unexpected argument: {other}");
                return 2;
            }
        }
        i += 1;
    }

    // SAFETY: raw socket then immediately owned; all pointers below are to
    // locals whose lifetime covers each call.
    let raw = unsafe { libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0) };
    if raw < 0 {
        eprintln!("bindprobe: socket: {}", strerror(last_errno()));
        return 1;
    }
    let sock = unsafe { OwnedFd::from_raw_fd(raw) };

    let one: i32 = 1;
    unsafe {
        libc::setsockopt(
            sock.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_REUSEADDR,
            &one as *const i32 as *const c_void,
            std::mem::size_of::<i32>() as libc::socklen_t,
        );
    }

    let mut addr: libc::sockaddr_in = unsafe { std::mem::zeroed() };
    addr.sin_family = libc::AF_INET as libc::sa_family_t;
    addr.sin_addr.s_addr = libc::INADDR_ANY.to_be();
    addr.sin_port = port.to_be();

    let rc = unsafe {
        libc::bind(
            sock.as_raw_fd(),
            &addr as *const libc::sockaddr_in as *const libc::sockaddr,
            std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t,
        )
    };
    if rc != 0 {
        eprintln!("bindprobe: bind :{port}: {}", strerror(last_errno()));
        return 3;
    }
    let uid = unsafe { libc::getuid() };
    println!("bindprobe: uid={uid} bound :{port}");
    0
}

// ---- drop ----------------------------------------------------------------

#[repr(C)]
struct CapHeader {
    version: u32,
    pid: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CapData {
    effective: u32,
    permitted: u32,
    inheritable: u32,
}

/// Drop to `uid`/`gid`, optionally retaining CAP_NET_BIND_SERVICE, then exec.
/// Returns only on failure; on success control never comes back.
fn arrange_and_exec(
    uid: libc::uid_t,
    gid: libc::gid_t,
    keep_cap: bool,
    cmd: &[String],
) -> Result<(), SysErr> {
    // (1) Retain the permitted set across the uid change (must precede it).
    if keep_cap {
        check(
            unsafe { libc::prctl(libc::PR_SET_KEEPCAPS, 1, 0, 0, 0) } as i64,
            "PR_SET_KEEPCAPS",
        )?;
    }

    // (2) Become the target user: groups, then gid, then uid.
    check(unsafe { libc::setgroups(1, &gid) } as i64, "setgroups")?;
    check(
        unsafe { libc::setresgid(gid, gid, gid) } as i64,
        "setresgid",
    )?;
    check(
        unsafe { libc::setresuid(uid, uid, uid) } as i64,
        "setresuid",
    )?;

    // (3) Pin exactly CAP_NET_BIND_SERVICE into permitted+inheritable.
    if keep_cap {
        let hdr = CapHeader {
            version: LINUX_CAPABILITY_VERSION_3,
            pid: 0,
        };
        let bit = 1u32 << CAP_NET_BIND_SERVICE; // cap 10, word 0
        let data = [
            CapData {
                effective: bit,
                permitted: bit,
                inheritable: bit,
            },
            CapData {
                effective: 0,
                permitted: 0,
                inheritable: 0,
            },
        ];
        check(
            unsafe { libc::syscall(libc::SYS_capset, &hdr as *const CapHeader, data.as_ptr()) },
            "capset",
        )?;

        // (4) Raise it into the ambient set.
        check(
            unsafe {
                libc::prctl(
                    libc::PR_CAP_AMBIENT,
                    libc::PR_CAP_AMBIENT_RAISE as libc::c_ulong,
                    CAP_NET_BIND_SERVICE as libc::c_ulong,
                    0,
                    0,
                )
            } as i64,
            "PR_CAP_AMBIENT_RAISE",
        )?;
    }

    // (5) Hand off to CMD.
    let prog = CString::new(cmd[0].as_str()).map_err(|_| SysErr {
        what: "bad CMD".into(),
        errno: libc::EINVAL,
    })?;
    let cargs: Vec<CString> = cmd
        .iter()
        .map(|s| CString::new(s.as_str()).unwrap_or_default())
        .collect();
    let mut ptrs: Vec<*const c_char> = cargs.iter().map(|c| c.as_ptr()).collect();
    ptrs.push(std::ptr::null());
    unsafe { libc::execvp(prog.as_ptr(), ptrs.as_ptr()) };
    Err(SysErr {
        what: format!("execvp {}", cmd[0]),
        errno: last_errno(),
    })
}

fn cmd_drop(args: &[String]) -> i32 {
    let mut user: Option<&str> = None;
    let mut keep_cap = false;
    let mut cmd: Vec<String> = Vec::new();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--user" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("drop: --user needs a value");
                    return 2;
                };
                user = Some(v);
                i += 1;
            }
            "--keep-cap" => {
                let Some(v) = args.get(i + 1) else {
                    eprintln!("drop: --keep-cap needs a value");
                    return 2;
                };
                if v != "net_bind_service" {
                    eprintln!("drop: unsupported --keep-cap: {v}");
                    return 2;
                }
                keep_cap = true;
                i += 1;
            }
            "--" => {
                cmd = args[i + 1..].to_vec();
                break;
            }
            other => {
                eprintln!("drop: unexpected argument: {other}");
                return 2;
            }
        }
        i += 1;
    }

    let Some(user) = user else {
        eprintln!("drop: --user <name> is required");
        return 2;
    };
    if cmd.is_empty() {
        eprintln!("drop: missing -- CMD");
        return 2;
    }
    if unsafe { libc::getuid() } != 0 {
        eprintln!("drop: must run as root");
        return 1;
    }

    let cname = match CString::new(user) {
        Ok(c) => c,
        Err(_) => {
            eprintln!("drop: unknown user: {user}");
            return 1;
        }
    };
    // SAFETY: getpwnam returns a pointer into a static buffer valid until the
    // next call; we read it immediately.
    let pw = unsafe { libc::getpwnam(cname.as_ptr()) };
    if pw.is_null() {
        eprintln!("drop: unknown user: {user}");
        return 1;
    }
    let (uid, gid) = unsafe { ((*pw).pw_uid, (*pw).pw_gid) };

    match arrange_and_exec(uid, gid, keep_cap, &cmd) {
        Ok(()) => 0, // unreachable: exec never returns on success
        Err(e) => {
            eprintln!("drop: {}: {}", e.what, strerror(e.errno));
            1
        }
    }
}

fn usage() {
    eprintln!("usage:");
    eprintln!("  pmon drop --user <name> [--keep-cap net_bind_service] -- CMD [args...]");
    eprintln!("  pmon bindprobe [--port 80]");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
        std::process::exit(2);
    }
    let code = match args[1].as_str() {
        "drop" => cmd_drop(&args[2..]),
        "bindprobe" => cmd_bindprobe(&args[2..]),
        _ => {
            usage();
            2
        }
    };
    std::process::exit(code);
}
