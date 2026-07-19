# 19-unix-sockets-fd-passing — pmon v5

pmon grows a control plane. `pmon supervise` runs a child command under
supervision — own process group, stdout+stderr appended to a log file,
automatic restart 300 ms after every exit — and listens on a UNIX stream
socket. `pmon pmctl` (same binary, different subcommand) talks to it:

```
$ ./demo.sh cpp run supervise --ctl /tmp/p/ctl.sock --log /tmp/p/pmon.log -- \
      /bin/sh -c 'echo alpha; echo beta; echo gamma; exec sleep 30' &
pmon: listening on /tmp/p/ctl.sock
pmon: started child pid=2601772

$ ./demo.sh cpp run pmctl --ctl /tmp/p/ctl.sock status
child=2601772 uptime=4s restarts=0
# supervisor stderr, from SO_PEERCRED on the accepted connection:
pmon: ctl connect uid=1000 pid=2601779

$ ./demo.sh cpp run pmctl --ctl /tmp/p/ctl.sock logfd
via-fd: alpha
via-fd: beta
via-fd: gamma

$ ./demo.sh cpp run pmctl --ctl /tmp/p/ctl.sock stop
stopping
```

## What the chapter is really about

`logfd` is the headline. The supervisor does not send the log *path* — it
sends its **open log file descriptor** through the socket in an `SCM_RIGHTS`
control message, and pmctl reads the last 3 lines directly off the received
fd (positional reads from offset 0, so the supervisor's `O_APPEND` writer is
undisturbed). The kernel installs a new descriptor in the receiving process
that shares the sender's open file description: an fd is a capability, and
UNIX sockets can hand capabilities between processes. `verify.lua` proves it
by diffing the `via-fd:` output against `tail -3` of the file, byte for byte.

Two supporting ideas:

- **SO_PEERCRED** — before trusting a single command byte, the supervisor
  asks the kernel who is on the other end of the connection and logs
  `pmon: ctl connect uid=<u> pid=<p>`. Credentials from the kernel, not from
  the client's own claims.
- **The log is opened O_RDWR|O_APPEND** — a write-only fd would arrive
  write-only in pmctl (the *file description* crosses the socket, flags and
  all), and the read side would fail with EBADF.

## The three implementations

Identical CLI, stdout/stderr shapes, and exit codes (0 ok / 1 runtime / 2
usage). Same wire protocol: one command line in, one reply line (or a reply
plus cmsg) out.

| | control messages | supervision concurrency |
|---|---|---|
| **C++23** | `sendmsg(2)`/`recvmsg(2)` with `CMSG_FIRSTHDR`/`CMSG_LEN`/`CMSG_DATA` built by hand | reaper on a `std::jthread`, state under mutex + condvar, `std::expected` for every fallible step, RAII `Fd` owns each descriptor |
| **Go 1.26** | `unix.UnixRights` / `unix.ParseSocketControlMessage` / `unix.ParseUnixRights` over `net.UnixConn.WriteMsgUnix`/`ReadMsgUnix` | reaper goroutine; restart backoff vs. shutdown is a `select` on a timer and a stop channel; wrapped errors end at `pmon: error:` |
| **Rust (2024)** | `nix::sys::socket` `sendmsg`/`recvmsg` with `ControlMessage::ScmRights` and `cmsg_space!` | reaper thread, `Arc<(Mutex<State>, Condvar)>`; the received fd goes straight into an `OwnedFd` so it closes on drop |

## Gotchas worth remembering

- **`sun_path` is 108 bytes.** Bind/connect fail with `EINVAL`-flavored
  errors when the socket path is longer (deep CI workspace paths hit this
  immediately). Keep control sockets under short directories.
- Passing an fd duplicates the *open file description*, so offset and status
  flags travel with it — hence O_RDWR above, and hence pmctl using `pread`
  instead of seeking, which would move the shared offset.
- The child gets its own process group (`setpgid`), so `stop` can SIGTERM
  the whole tree, including anything the child forked.
- Log lines are written with one `write(2)` each; `O_APPEND` makes every
  append atomic even though supervisor and child share the file.

## Files

```
demo.sh        # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
verify.lua     # behavioral checks (status fields, creds line, fd-passing tail, teardown)
cpp/ go/ rust/ # one pmon each, binary always ./…/app, demo.sh contract identical
```

## Verification

`python3 scripts/test-all-examples.py --only 19-unix-sockets-fd-passing`
builds all three and runs `verify.lua` per language. The checks are
behavioral: status field shapes, a live `/proc/<child>` running the
supervised command, the SO_PEERCRED uid equal to `id -u`, `via-fd:` output
identical to `tail -3` of the log, restart counting consistent between
status, stderr, and the log, and a stop that leaves no socket file and no
child process behind.
