// util.rs — small helpers shared across the fleet's subcommands, the Rust
// counterpart of util.go's installSignalFlag: chatterd/sysagent/fwatch each
// run their own poll loop already (accept-with-timeout, interval sleep,
// poll(2) with a timeout), so a level-triggered flag is all any of them
// needs from a signal — no EINTR-sensitive syscall unwinding required, unlike
// ex40's fastpath server. pmon (a supervisor, not a poller) does its own
// dedicated child-reaping loop in pmon.rs instead of using this.
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

use nix::sys::signal::{SaFlags, SigAction, SigHandler, SigSet, Signal, sigaction};

static SIGNAL_FLAG: AtomicBool = AtomicBool::new(false);
static INSTALLED: OnceLock<()> = OnceLock::new();

extern "C" fn on_signal(_: libc::c_int) {
    SIGNAL_FLAG.store(true, Ordering::Relaxed);
}

/// Installs a SIGINT/SIGTERM handler (once per process; safe to call from
/// multiple subcommands' code paths) that flips a process-wide flag to true
/// the first time either signal arrives. Returns a reference to that flag.
pub fn install_signal_flag() -> &'static AtomicBool {
    INSTALLED.get_or_init(|| {
        let sa = SigAction::new(
            SigHandler::Handler(on_signal),
            SaFlags::empty(),
            SigSet::empty(),
        );
        // SAFETY: on_signal only performs a relaxed atomic store, which is
        // async-signal-safe.
        unsafe {
            let _ = sigaction(Signal::SIGINT, &sa);
            let _ = sigaction(Signal::SIGTERM, &sa);
        }
    });
    &SIGNAL_FLAG
}
