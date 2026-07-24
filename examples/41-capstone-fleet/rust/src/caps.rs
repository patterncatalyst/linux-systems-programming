// caps.rs — capability-bounding-set drop, the cheap, always-available half of
// this chapter's sandboxing story (the other half, Landlock, lives in
// fwatch.rs). See go/caps.go for the full rationale.
//
// PR_CAPBSET_DROP removes a capability from the *bounding set*: even a future
// execve of a setuid-root binary can never regain it, for this process and
// every descendant (the bounding set is inherited across fork/exec and can
// only shrink). It is meaningful regardless of whether the calling process
// currently holds any capabilities at all — that is exactly pmon's case when
// deployed unprivileged on a lab guest.

// lastKnownCap is CAP_CHECKPOINT_RESTORE (40) on a 6.x+ kernel; probing past
// the kernel's actual last cap just returns EINVAL, which we ignore.
const LAST_KNOWN_CAP: libc::c_ulong = 40;

pub fn drop_bounding_set() {
    let mut dropped = 0;
    for c in 0..=LAST_KNOWN_CAP {
        // SAFETY: prctl(PR_CAPBSET_DROP, c, 0, 0, 0) has no memory-safety
        // implications; only its return value is inspected.
        let rc = unsafe { libc::prctl(libc::PR_CAPBSET_DROP, c, 0, 0, 0) };
        if rc == 0 {
            dropped += 1;
        }
    }
    // SAFETY: same as above.
    let rc = unsafe { libc::prctl(libc::PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) };
    if rc != 0 {
        eprintln!(
            "pmon: prctl(PR_SET_NO_NEW_PRIVS): {}",
            std::io::Error::last_os_error()
        );
        return;
    }
    eprintln!("pmon: capabilities bounding_set_dropped={dropped} no_new_privs=1");
}
