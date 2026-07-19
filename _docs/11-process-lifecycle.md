---
title: "The process lifecycle: fork, exec, wait"
order: 11
part: "Processes, Signals, Privilege"
description: "fork, exec, and wait walked end to end as pmon v0 spawns and reaps its first children: copy-on-write, a real zombie caught live in ps, orphans reparented to a subreaper, exit codes vs termination signals, rusage accounting — and the clone flavors os/exec and Command actually issue, read from strace."
duration: "45 minutes"
---

This chapter opens the book's third part with a new long-running artifact:
`pmon`, a process monitor that will grow signal forwarding, pidfd-based
supervision, and privilege dropping over the next three chapters. Version 0
does the irreducible job of every supervisor — spawn a command, wait for it,
report how it died and what it cost — because everything else in this part
builds on one idea: **a process's death is data, and someone must collect
it**. Between a child's last instruction and the parent's `wait`, the kernel
holds the corpse — exit status, resource usage, a pid that cannot be reused —
as a *zombie*, and in this chapter you will create one deliberately, watch it
in `ps`, read its `/proc` entry field by field, and then reap it. Along the
way we take apart what `fork(2)` really copies (almost nothing, thanks to
copy-on-write), why exec failure needs a back channel, and what Go's
`os/exec` and Rust's `Command` are actually doing under their tidy APIs — the
answer, straight from `strace`, involves three different flavors of `clone`.

The code is in `examples/11-process-lifecycle/`. `./demo.sh` there builds all
three implementations and walks the four child fates — clean exit, `exit 42`,
death by SIGTERM, exec failure — and its `README.md` pins the output contract
and exit-code mirroring all three languages share.

{% include excalidraw.html
   file="11-process-lifecycle-states"
   alt="Five process states drawn as boxes: fork/clone3 leads into Running R inside an alive band that also holds Sleeping S and Stopped T with SIGSTOP and SIGCONT arrows between them; an amber exit arrow crosses into an after-exit band holding Zombie Z, annotated defunct in ps f and exit status plus rusage only, with a final amber wait4/waitid arrow down to a dashed reaped box where the pid table slot is free."
   caption="Figure 11.1 — the lifecycle as ps STAT letters; the kernel keeps a zombie until someone reaps it, and every annotation in the zombie box is observed later in this chapter" %}

> **Tools used** — `strace`, `ps`, `kill`, `sh`, `sleep`, `cat` (host);
> `execsnoop`/`exitsnoop` (lab VM, exercised in Part 8). Everything here is
> checked by `scripts/check-host.sh`, ships with Fedora, or is preinstalled
> in the lab VMs.

## Born twice: fork, then exec

Unix splits "run a program" into two syscalls with nothing in between but
your own code. `fork(2)` duplicates the calling process — same program, same
open fds, a new pid — and *returns twice*: the child sees `0`, the parent
sees the child's pid. `execve(2)` then replaces the calling process's program
image with a new one, keeping the pid and the fd table. The gap between them
is the whole point: the child can rearrange fds, drop privileges, or change
directory *as ordinary code running in the child*, before the new program
exists. Every shell redirection you have ever typed is three lines of code in
that gap.

The apparent extravagance of fork — duplicate an entire address space just to
throw it away at exec — is paid for by **copy-on-write**. The kernel copies
page *tables*, not pages: parent and child share every physical page,
read-only, and only a write by either side triggers a fault that copies the
one page written. A fork of a 100 MB process touches kilobytes. You can see
the accounting in this chapter's own evidence: the zombie's
`/proc/<pid>/stat` below records `minflt` of 103 — one hundred and three
minor faults is the entire page-level cost of forking pmon and execing
`sleep`. The COW story has a sharper corollary for the Go and Rust runtimes,
visible in the cross-check: when the child is going to exec *immediately*,
even copying page tables is waste, so both runtimes ask for
`CLONE_VM|CLONE_VFORK` — share the address space outright and suspend the
parent until the exec — which is fork with the copy amortized to zero.

{% include excalidraw.html
   file="11-fork-exec-wait-seq"
   alt="Two lanes, parent pmon and child. Parent creates pipe2 O_CLOEXEC, calls fork drawn as clone SIGCHLD with a COW note, and an arrow labeled returns 0 in the child crosses to the child lane where execvp runs; a dashed arrow back carries EOF on success or errno then exit 127. The child box CMD runs then exits or is killed leads by an amber arrow labeled zombie until reaped, SIGCHLD to the parent's waitpid/wait4 box, then down to report and mirror the fate, status s exits s and signal n exits 128 plus n."
   caption="Figure 11.2 — pmon's spine: fork → exec → wait, with the CLOEXEC self-pipe carrying the exec verdict back to the parent" %}

One hole remains in the two-step model: if `execvp` itself fails — the
command does not exist — the failure happens *in the child*, after fork, and
the parent has no direct way to distinguish "exec failed" from "the program
ran and exited". pmon closes the hole with a **self-pipe**, and chapter 7's
`O_CLOEXEC` discipline is what makes it work. Both pipe ends are opened
close-on-exec, from `examples/11-process-lifecycle/cpp/src/main.cpp`:

```cpp
// Self-pipe for exec-failure reporting: both ends O_CLOEXEC, so a successful
// exec closes the child's copy and the parent reads EOF.
struct ExecPipe {
    Fd read_end;
    Fd write_end;
};

[[nodiscard]] std::expected<ExecPipe, std::error_code> make_exec_pipe() {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        return std::unexpected(last_error());
    }
    return ExecPipe{Fd{fds[0]}, Fd{fds[1]}};
}
```

If exec succeeds, the kernel closes the child's write end atomically at exec
time and the parent's `read` returns 0 — EOF, no child code required. If exec
fails, the child writes the raw `errno` into the pipe and `_exit(127)`s. The
parent reads either nothing or four bytes that name the exact failure. This
trick is not pedagogy: the cross-check shows Go and Rust doing precisely the
same dance inside their standard libraries.

## Zombies, orphans, and the duty to reap

When a process exits, the kernel cannot simply delete it: the parent may not
have asked yet how it died, and the pid must not be recycled while an answer
is owed. So the address space, fds, and threads are freed immediately, but
the task record — pid, exit status, rusage — stays as a **zombie** (`Z` in
`ps`, rendered `<defunct>`) until the parent calls `wait4(2)`/`waitid(2)`.
Reaping *is* that call: the kernel copies the status and rusage out and only
then frees the pid. A parent that spawns children and never waits leaks pids
— a real resource, bounded by `pid_max` — which is why every supervisor loop
in this book pairs each spawn with exactly one reap.

Orphans travel the other direction: if the *parent* dies first, the child is
not left parentless — the kernel reparents it up the tree to the nearest
**subreaper** (`PR_SET_CHILD_SUBREAPER`, a Part 3 recurring guest) or to pid
1. On this Fedora host the reparent target is not init itself: orphaning a
`sleep` by letting its shell parent exit immediately re-homed it to the user's
`systemd --user` instance, pid 6145 — session managers register as subreapers
precisely so they, not pid 1, inherit the duty to reap. Reparenting is also
why zombies are curable by killing the negligent parent: its zombies get
reparented, and subreapers wait promptly.

## Exit codes vs termination signals

"How did it die" is one `int` with two disjoint answers. A process that
called `exit(s)` has `WIFEXITED(status)` true and `WEXITSTATUS(status)`
yields the low 8 bits of `s`. A process killed by a signal never reached
`exit` at all: `WIFSIGNALED(status)` is true and `WTERMSIG(status)` names the
signal. They are different fields of the wait status, and conflating them
loses information — "exited 15" and "killed by SIGTERM" are entirely
different deaths. The shell flattens the pair into one byte with the `128+n`
convention, and pmon mirrors it exactly so it composes with `&&`, `make`, and
CI: status `s` becomes exit `s`, signal `n` becomes exit `128+n` (SIGTERM →
143, SIGKILL → 137), exec failure is the shell's classic `127`, and usage
errors are `2`. Those numbers are asserted, not aspirational: `verify.lua`
checks all of them against live runs, 27 checks per language.

## How the code works

The C++ version spells the lifecycle out syscall by syscall; Go and Rust
lean on their runtimes for the spawn and reclaim the low-level facts on the
wait side. Here is the spine, three ways — C++ from `fork` to the mirrored
exit, and the equivalent `monitor` function in Go and Rust:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
    const auto start = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::println(stderr, "pmon: fork: {}", last_error().message());
        return 1;
    }

    if (pid == 0) {
        // Child. On exec success the O_CLOEXEC pipe closes itself; on failure
        // ship errno to the parent and die with the shell's 127 convention.
        pipe->read_end.reset();
        ::execvp(child_argv[0], child_argv);
        const int exec_errno = errno;
        (void)::write(pipe->write_end.get(), &exec_errno, sizeof exec_errno);
        _exit(127);
    }

    // Parent: drop our write end so read() returns the moment exec resolves.
    pipe->write_end.reset();
    int exec_errno = 0;
    ssize_t nread = 0;
    do {
        nread = ::read(pipe->read_end.get(), &exec_errno, sizeof exec_errno);
    } while (nread < 0 && errno == EINTR);

    const auto status = wait_child(pid);
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!status) {
        std::println(stderr, "pmon: waitpid: {}", status.error().message());
        return 1;
    }

    if (nread == static_cast<ssize_t>(sizeof exec_errno)) {
        const std::error_code ec{exec_errno, std::system_category()};
        std::println(stderr, "pmon: exec {}: {}", child_argv[0], ec.message());
        return 127;
    }

    const auto ru = children_rusage();
    if (!ru) {
        std::println(stderr, "pmon: getrusage: {}", ru.error().message());
        return 1;
    }

    int code = 1;
    if (WIFEXITED(*status)) {
        code = WEXITSTATUS(*status);
        std::println("pmon: pid {} exited status {}", pid, code);
    } else if (WIFSIGNALED(*status)) {
        const int sig = WTERMSIG(*status);
        code = 128 + sig;
        std::println("pmon: pid {} killed by signal {} ({})", pid, sig, signal_name(sig));
    } else {
        std::println(stderr, "pmon: unexpected wait status {:#x}", *status);
        return 1;
    }

    std::println("pmon: rusage maxrss={}KB user={} sys={} wall={}ms", ru->ru_maxrss,
                 format_cpu(ru->ru_utime), format_cpu(ru->ru_stime), wall.count());
    return code;
}
```

```go
func monitor(name string, args []string) int {
	cmd := exec.Command(name, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr

	start := time.Now()
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: exec %s: %s\n", name, reason(err))
		return 127
	}
	state, err := waitChild(cmd)
	wall := time.Since(start)
	if err != nil {
		fmt.Fprintf(os.Stderr, "pmon: %v\n", err)
		return 1
	}

	ws, ok := state.Sys().(syscall.WaitStatus)
	if !ok {
		fmt.Fprintln(os.Stderr, "pmon: no wait status for child")
		return 1
	}
	ru, ok := state.SysUsage().(*syscall.Rusage)
	if !ok {
		fmt.Fprintln(os.Stderr, "pmon: no rusage for child")
		return 1
	}

	var code int
	switch {
	case ws.Exited():
		code = ws.ExitStatus()
		fmt.Printf("pmon: pid %d exited status %d\n", state.Pid(), code)
	case ws.Signaled():
		sig := int(ws.Signal())
		code = 128 + sig
		fmt.Printf("pmon: pid %d killed by signal %d (%s)\n", state.Pid(), sig, sigName(sig))
	default:
		fmt.Fprintf(os.Stderr, "pmon: unexpected wait status %#x\n", uint32(ws))
		return 1
	}

	fmt.Printf("pmon: rusage maxrss=%dKB user=%s sys=%s wall=%dms\n",
		ru.Maxrss, formatCPU(ru.Utime), formatCPU(ru.Stime), wall.Milliseconds())
	return code
}
```

```rust
fn monitor(name: &str, args: &[String]) -> i32 {
    let start = Instant::now();
    let child = match Command::new(name).args(args).spawn() {
        Ok(child) => child,
        Err(err) => {
            eprintln!("pmon: exec {name}: {}", reason(&err));
            return 127;
        }
    };
    let pid = child.id() as i32;

    let (status, rusage) = match wait4(pid) {
        Ok(pair) => pair,
        Err(err) => {
            eprintln!("pmon: wait4: {err}");
            return 1;
        }
    };
    let wall = start.elapsed().as_millis();

    let code = match status {
        WaitStatus::Exited(_, code) => {
            println!("pmon: pid {pid} exited status {code}");
            code
        }
        WaitStatus::Signaled(_, signal, _) => {
            let n = signal as i32;
            println!("pmon: pid {pid} killed by signal {n} ({})", sig_name(n));
            128 + n
        }
        other => {
            eprintln!("pmon: unexpected wait status: {other:?}");
            return 1;
        }
    };

    println!(
        "pmon: rusage maxrss={}KB user={} sys={} wall={wall}ms",
        rusage.ru_maxrss,
        format_cpu(rusage.ru_utime),
        format_cpu(rusage.ru_stime),
    );
    code
}
```

Walk the C++ tab first, because everything else is a compressed version of
it. `steady_clock::now()` is taken *before* the fork so wall time covers the
child's whole life, and steady (not system) time so an NTP step cannot make a
child appear to run backwards. `fork` splits the world; the child branch
resets its copy of the read end (chapter 7's move-only `Fd` makes "close
exactly once" a type property, and `ExecPipe` is just two of them), calls
`execvp` — the `p` variant walks `PATH` like a shell — and, only if exec
*returned*, ships `errno` up the pipe and dies with `_exit(127)`. `_exit`,
not `exit`: the child still shares the parent's stdio buffers, and running
the parent's atexit handlers or flushing its buffered output from the child
would emit duplicate bytes. The parent drops its write end — otherwise its
own open fd would hold the pipe open and `read` would block forever, a
self-deadlock — then reads the verdict, retrying on `EINTR`. `wait_child`
loops `waitpid` under the same `EINTR` discipline; the reap must survive a
stray signal. Only then does the status get decoded through the
`WIFEXITED`/`WIFSIGNALED` pair, and the rusage line printed from
`getrusage(RUSAGE_CHILDREN)` — the accumulated cost of all waited-for
children, which equals this child's cost because there is exactly one.

The Go tab hides the fork behind `cmd.Start`, but nothing else changes
shape. `cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, ...` inherits stdio so
the child's output interleaves naturally — the pid-identity check in
`verify.lua` depends on it, the child echoing `$$` through the shared
descriptor. A `Start` error *is* the self-pipe verdict, surfaced as a value.
`waitChild` needs one subtlety: `cmd.Wait` returns an error for a child that
ran and failed, but an `exec.ExitError` still carries the `ProcessState` we
want — a child exiting 42 is pmon's *success path* — so only non-`ExitError`
failures propagate. The wait-side facts come back out of the portable
wrapper by type assertion: `state.Sys().(syscall.WaitStatus)` is the raw
status int with the same `Exited()`/`Signaled()` split, and
`state.SysUsage().(*syscall.Rusage)` is the per-child rusage the kernel
filled in at reap time.

Rust's `Command::spawn` similarly returns `Err` on exec failure (same
status-pipe trick internally), but the wait side goes straight to the
syscall, because `wait4(2)` answers both questions in one call — status
*and* rusage for exactly this child:

```rust
/// wait4(2): reap `pid`, yielding the decoded wait status and its rusage.
fn wait4(pid: i32) -> io::Result<(WaitStatus, libc::rusage)> {
    let mut raw_status = 0i32;
    // SAFETY: zero-filled rusage is a valid out-param for wait4(2).
    let mut rusage: libc::rusage = unsafe { std::mem::zeroed() };
    loop {
        // SAFETY: raw_status and rusage are exclusively borrowed, valid
        // out-pointers for the duration of the call.
        let rc = unsafe { libc::wait4(pid, &mut raw_status, 0, &mut rusage) };
        if rc == pid {
            let status = WaitStatus::from_raw(Pid::from_raw(pid), raw_status)
                .map_err(|errno| io::Error::from_raw_os_error(errno as i32))?;
            return Ok((status, rusage));
        }
        let err = io::Error::last_os_error();
        if err.raw_os_error() != Some(libc::EINTR) {
            return Err(err);
        }
    }
}
```

The two `SAFETY` comments are the whole cost of dropping below
`std::process`: a zeroed `rusage` is a valid out-parameter, and the two
`&mut`s are exclusive for the duration of the call. `nix` decodes the raw
status into a typed `WaitStatus`, giving the `match` in `monitor` the same
two-armed shape as `WIFEXITED`/`WIFSIGNALED`. Note the deliberate asymmetry
with C++: `RUSAGE_CHILDREN` aggregates every reaped child, `wait4` meters
one — identical numbers here, different interfaces, and the distinction will
matter the moment pmon supervises more than one process.

Two shared fragile bits, stated plainly. The signal names in
`killed by signal 15 (TERM)` come from a hand-rolled 32-entry table,
identical in all three sources — `strsignal(3)` returns locale-dependent
prose ("Terminated") and Go and Rust each have their own spellings, so the
table is the only way three runtimes print one string. And the rusage line's
`%03d` milliseconds formatting is part of the output contract:
`user=0.000s`, not `user=0.0s`, so `verify.lua` can match the shape exactly.

## Errors, three ways

The contract for a failed spawn is byte-identical output — `pmon: exec
<cmd>: No such file or directory` on stderr, exit 127, and *no* fate or
rusage lines, because no child ran. Each language must dig the errno out of
different wreckage. C++ owns the errno directly: the child ships the raw
`int` over the pipe and the parent formats it through
`std::error_code::message()`. Rust gets an `io::Error` from `spawn` and
recovers `raw_os_error()`, rendering it with `nix`'s `Errno::desc` — the
classic `strerror(3)` text without `io::Error`'s "(os error 2)" suffix. Go
is the interesting one, from `examples/11-process-lifecycle/go/main.go`:

```go
// reason renders a spawn failure in strerror(3) shape. Go's errno strings are
// lowercase ("no such file or directory"), so the first letter is capitalized
// to match what the C++ and Rust versions print; a PATH miss (exec.ErrNotFound)
// is what execvp(3) reports as ENOENT.
func reason(err error) string {
	var errno syscall.Errno
	if errors.As(err, &errno) {
		return capitalize(errno.Error())
	}
	if errors.Is(err, exec.ErrNotFound) {
		return "No such file or directory"
	}
	return capitalize(err.Error())
}
```

`errors.As` unwraps to the underlying `syscall.Errno` when there is one, but
Go renders errno strings lowercase, so the first rune is capitalized to
match the C and Rust spellings; and a `PATH` miss surfaces as
`exec.ErrNotFound` rather than an errno at all, which `reason` maps back to
the `ENOENT` text `execvp(3)` reports in the other two. The parity is real:
all three binaries, given `nonexistent-cmd-xyz`, printed the identical line
and exited 127 in this session's runs.

## Concurrency lens

`fork` and threads are famously hostile: the child inherits *only the
calling thread*, so any lock another thread held at fork time stays locked
forever in the child — which is why the only safe thing a multithreaded
process can do between fork and exec is call async-signal-safe functions.
pmon's C++ version can fork casually only because it is single-threaded. Go
cannot make that promise — the runtime is multithreaded before `main` runs —
so `os/exec` confines the child to a tiny fixed path of raw syscalls, and
the strace shows the runtime spawning its scheduler threads
(`CLONE_THREAD` clones) before the one process-creating
`clone(CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD)`. That `CLONE_PIDFD` flag
is this part's chapter 13 arriving early: Go receives a *pidfd* for the
child and waits via `waitid(P_PIDFD, ...)`, immune to pid-reuse races — it
identifies the child by fd, not by number. Rust's `CLONE_CLEAR_SIGHAND`
(via `posix_spawn`) solves a different race: the child starts with default
signal dispositions instead of whatever handlers the parent had installed,
exactly the class of inherited-state bug the vfork'd shared address space
would otherwise make lethal. One more accounting subtlety:
`getrusage(RUSAGE_CHILDREN)` is process-wide state — if concurrent code
reaped other children between two calls, the aggregate moves — while
`wait4`'s per-child rusage is race-free by construction. pmon v0 is
single-threaded in all three languages; these flags are the runtimes
defending themselves.

## Build, run, observe

```bash
[host]$ cd examples/11-process-lifecycle && ./demo.sh
```

Each language builds and walks the four fates. By hand, the runs that
produced this chapter's numbers:

```bash
[host]$ cpp/build/release/app run -- sh -c 'exit 42'
pmon: pid 2600288 exited status 42
pmon: rusage maxrss=3936KB user=0.000s sys=0.001s wall=1ms
[host]$ echo $?
42
[host]$ go/bin/app run -- sh -c 'kill -TERM $$'
pmon: pid 2600305 killed by signal 15 (TERM)
pmon: rusage maxrss=3816KB user=0.000s sys=0.001s wall=1ms
[host]$ echo $?
143
[host]$ rust/target/release/app run -- sh -c 'kill -KILL $$'
pmon: pid 2613705 killed by signal 9 (KILL)
pmon: rusage maxrss=3576KB user=0.000s sys=0.000s wall=0ms
[host]$ echo $?
137
[host]$ cpp/build/release/app run -- ./nonexistent
pmon: exec ./nonexistent: No such file or directory
[host]$ echo $?
127
```

Two more observations tie the report lines to reality. A 200 ms sleep
yields `wall=200ms` on the nose (`sleep 0.2` → `maxrss=2120KB user=0.000s
sys=0.000s wall=200ms`), and the pid in the fate line is the child's own:
`run -- sh -c 'echo "child: my pid is $$"'` printed `child: my pid is
2607401` through the inherited stdout, followed by pmon's
`pid 2607401 exited status 0` — same number, one from inside, one from the
wait. The runner
(`python3 scripts/test-all-examples.py --only 11-process-lifecycle`)
reports `PASS PASS PASS` for cpp, go, and rust — 3 passed, 0 failed, 0
skipped — with `verify.lua` asserting 27 behavioral checks per language.

## Cross-check, three ways

**The clone flavors, under `strace -f`.** The claim was that three runtimes
spell "make a child" three ways. Tracing each binary running `/bin/true`
with `strace -f -e trace=clone,clone3,vfork,execve,wait4,waitid`, the
process-creating call and the reap, verbatim per runtime — C++ (glibc
`fork`):

```
[host]$ strace -f -e trace=clone,clone3,vfork,execve,wait4,waitid cpp/build/release/app run -- /bin/true
2600465 clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, child_tidptr=0x7fe6aa38ba50) = 2600466
2600466 execve("/bin/true", ["/bin/true"], 0x7ffd64262d60 /* 96 vars */ <unfinished ...>
2600465 wait4(2600466 <unfinished ...>
2600465 <... wait4 resumed>, [{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, NULL) = 2600466
```

A full copy-on-write fork — `child_stack=NULL` means "same stack, COW" —
then `wait4` with a null rusage pointer (pmon's C++ uses `getrusage`
instead). Go issues:

```
2600690 clone(child_stack=NULL, flags=CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD <unfinished ...>
2600696 execve("/bin/true", ["/bin/true"], 0x2bac423f2388 /* 96 vars */ <unfinished ...>
2600690 <... clone resumed>, parent_tid=[6]) = 2600696
2600690 waitid(P_PIDFD, 6 <unfinished ...>
2600690 <... waitid resumed>, {si_signo=SIGCHLD, si_code=CLD_EXITED, si_pid=2600696, si_uid=1000, si_status=0, si_utime=0, si_stime=0}, WEXITED, {ru_utime={tv_sec=0, tv_usec=0}, ru_stime={tv_sec=0, tv_usec=1058}, ...}) = 0
```

vfork semantics (`CLONE_VM|CLONE_VFORK`: shared address space, parent
suspended until exec), *plus* `CLONE_PIDFD` — the reap is
`waitid(P_PIDFD, 6, ...)`, identifying the child by the returned pidfd, and
the trace even shows Go probing pidfd support first — a
`waitid(P_PIDFD, 4, ...) = -1 ECHILD` self-check, then a throwaway
`CLONE_PIDFD` clone waited on fd 5 — before the real spawn. Rust is the
third flavor:

```
2600701 clone3({flags=CLONE_VM|CLONE_VFORK|CLONE_CLEAR_SIGHAND, exit_signal=SIGCHLD, stack=0x7f7f7c13a000, stack_size=0x9000}, 88 <unfinished ...>
2600702 execve("/bin/true", ["/bin/true"], 0x7ffec54d8a00 /* 96 vars */ <unfinished ...>
2600701 <... clone3 resumed>)           = 2600702
2600701 wait4(2600702 <unfinished ...>
2600701 <... wait4 resumed>, [{WIFEXITED(s) && WEXITSTATUS(s) == 0}], 0, {ru_utime={tv_sec=0, tv_usec=0}, ru_stime={tv_sec=0, tv_usec=656}, ...}) = 2600702
```

`clone3(2)` — the modern extensible spelling, here via glibc's
`posix_spawn` — with the same vfork optimization plus
`CLONE_CLEAR_SIGHAND`, and then pmon's own explicit `wait4` with a real
rusage out-parameter, exactly as `wait4()` in `rust/src/main.rs` claims.
Three clone flavors, one lifecycle.

**A zombie, live in `ps f` and `/proc`.** pmon reaps promptly, so to catch
the zombie window you stop the *parent* mid-wait: start
`app run -- sleep 3`, `kill -STOP` pmon, and let the child die while its
reaper is frozen. Four seconds later, on this host:

```bash
[host]$ ps f -o pid,ppid,stat,cmd -p 2601639,2601641
    PID    PPID STAT CMD
2601639 2601637 TN   cpp/build/release/app run -- sleep 3
2601641 2601639 ZN    \_ [sleep] <defunct>
[host]$ head -3 /proc/2601641/status
Name:	sleep
State:	Z (zombie)
Tgid:	2601641
[host]$ cat /proc/2601641/stat
2601641 (sleep) Z 2601639 2601637 2601637 0 -1 4227084 103 0 0 0 0 0 0 0 25 5 1 0 64173304 0 0 ...
```

Walk the `stat` fields (the format is `pid (comm) state ...` — parse from
the right of the closing parenthesis, since `comm` may contain spaces):
field 3 is `Z`, the zombie state `ps` renders as `<defunct>`; field 4 is the
ppid `2601639` — stopped pmon, which `ps f`'s tree draws as the parent of
its own corpse; field 10, `minflt=103`, is the copy-on-write bill discussed
earlier; field 19 is nice `5` (the `N` in both STAT columns — this session
ran niced); and fields 23–24, `vsize` and `rss`, are both `0` — the address
space is already gone, which is why killing a zombie is meaningless: there
is nothing left to kill, only data waiting to be read. `kill -CONT 2601639`
ended the window: pmon's `wait_child` returned at last and it printed
`pmon: pid 2601641 exited status 0` with `wall=4343ms` — wall time measures
until the *reap*, not the death, and the stopped window is in the bill.

**An orphan, reparented.** The inverse experiment — parent dies first —
takes one line: `sh -c 'sleep 7 >/dev/null 2>&1 & echo $!'` prints the
background child's pid and exits, orphaning it. Half a second later:

```bash
[host]$ ps -o pid,ppid,stat,cmd -p 2606723
    PID    PPID STAT CMD
2606723    6145 S    sleep 7
[host]$ ps -o pid,cmd -p 6145
    PID CMD
   6145 /usr/lib/systemd/systemd --user --deserialize=10
```

The dead shell's pid is gone from the pair; the child's `PPid` is now 6145,
the user's `systemd --user` — a registered subreaper, which will `waitid`
the sleep the moment it exits. No zombie ever forms on this path.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the fleet-wide view of this chapter is `execsnoop` and `exitsnoop` from
> bcc-tools: every exec and every exit on the box, with the same
> status/signal split pmon decodes, no per-process strace needed. bcc-tools
> need root and a kernel with BTF, so they run on the `systems-target` VM,
> and Part 8's eBPF observation chapter exercises them there against these
> exact binaries.

## What you learned

- A process is born in two steps — `fork` (COW: page tables now, pages only
  on write; 103 minor faults for a whole spawn) and `exec` (new image, same
  pid and fds) — and the gap between them is where fd plumbing and privilege
  dropping live; a CLOEXEC self-pipe is how exec failure reports back.
- Death is data: the kernel holds exit status and rusage as a zombie until
  `wait4`/`waitid` reaps it, `ps` state `Z`/`<defunct>` with `rss=0`; orphans
  are reparented to a subreaper (`systemd --user` here), never abandoned.
- One wait status, two disjoint deaths: `WIFEXITED`+`WEXITSTATUS` vs
  `WIFSIGNALED`+`WTERMSIG`, flattened by the shell convention pmon mirrors —
  `s`, `128+n`, or 127 when exec itself failed.
- The tidy APIs are the same machinery: glibc `fork` is
  `clone(SIGCHLD)`, Go spawns with `clone(CLONE_VM|CLONE_PIDFD|CLONE_VFORK)`
  and reaps by pidfd via `waitid(P_PIDFD)`, Rust's `Command` rides
  `posix_spawn`'s `clone3(CLONE_VM|CLONE_VFORK|CLONE_CLEAR_SIGHAND)` —
  three flavors of one lifecycle, all read from `strace -f`.

Next, **safe signals**: what a handler is allowed to do (almost nothing),
why `pmon` will route SIGTERM through a signalfd instead, and how to forward
signals to a child without racing its death.

---

<p><span class="status status--verified">verified</span> — every number and
output line above was produced on the Fedora 44 reference host this session:
the runner printed <code>11-process-lifecycle  PASS  PASS  PASS</code> (3
passed, 0 failed, 0 skipped; 27 verify.lua checks per language); the fate
runs (42/143/137/127, PATH-miss parity, <code>wall=200ms</code> for a 200 ms
sleep, pid 2607401 echoed by the child and reported by the wait) are real
transcripts; the strace excerpts showing <code>clone(SIGCHLD)</code>,
<code>clone(CLONE_VM|CLONE_PIDFD|CLONE_VFORK|SIGCHLD)</code> +
<code>waitid(P_PIDFD)</code>, and
<code>clone3(CLONE_VM|CLONE_VFORK|CLONE_CLEAR_SIGHAND)</code> are trimmed
real output; and the zombie window (<code>ZN [sleep] &lt;defunct&gt;</code>,
<code>State: Z (zombie)</code>, stat fields walked, reap after
<code>kill -CONT</code> with <code>wall=4343ms</code>) and the orphan
reparented to <code>systemd --user</code> pid 6145 were captured live. The
"On the lab VM" execsnoop/exitsnoop callout is unverified as marked, deferred
to Part 8.</p>
