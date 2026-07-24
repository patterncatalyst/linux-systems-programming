---
title: "The capstone fleet: pmon supervises chatterd, sysagent, and fwatch across two capability-dropped, Landlock-sandboxed, OTLP-observed hosts"
order: 41
part: "Performance and Low Latency"
description: "41-capstone-fleet is one binary with four subcommands that assembles four of the book's six recurring programs into a single supervised system: pmon drops the capability bounding set and sets no_new_privs, then self-re-execs chatterd, sysagent, and fwatch as restartable children, forwarding SIGTERM/SIGINT and printing a pmon: health line every tick; chatterd (ch21-27) is bridged across two hosts so a message sent on one node is delivered on the other tagged from=NICK@node; sysagent (ch36) exports cpu%/mem%/load1 as OTel gauges over OTLP when OTEL_EXPORTER_OTLP_ENDPOINT is set; and fwatch (ch07-09, 33) watches its sandbox directory under an optional Landlock ruleset whose enforced ABI — 7 on the lab guest's 6.19.10 kernel, 9 on the Fedora 44 host — is itself a live cross-check of the kernel-version probing discipline Chapter 33 established. Verified on the systems-target/systems-peer two-VM lab, with OTLP reaching the host's Grafana LGTM stack over the libvirt gateway."
duration: "60 minutes"
---

Chapter 0's course map promised six programs that would grow chapter by
chapter into real systems software. By now four of them have grown enough to
stand on their own: `pmon` learned to fork, exec, wait, drop privilege, and
walk namespaces (Chapters 11–14, 18–19, 32, 34); `chatterd` learned TCP, then
epoll at scale, then UDP peer discovery, then async, then a pinned-core fast
path (21–27, 34, 38, 40); `sysagent` learned to read `/proc` and
`/sys/fs/cgroup` directly and then to export what it read (36–38); and
`fwatch` learned to watch a directory with polling, then inotify, then a
Landlock sandbox around itself (07–10, 33). This chapter does not teach a new
syscall. It assembles: one binary, four subcommands, where `pmon` is the
fleet's own init — it drops the capability bounding set for itself and every
descendant, then self-re-execs `chatterd`, `sysagent`, and `fwatch` as
children it restarts on unexpected exit, forwards its own signals to, and
reports on with a `pmon: health` line every tick.

Two copies of that fleet run in this chapter — one on loopback, for
`verify.lua`, and one for real, on the two-VM lab Chapter 2 built.
`chatterd` is bridged between `systems-target` and `systems-peer` so a
message sent on one host is delivered on the other; each guest's `sysagent`
exports gauges over OTLP, through the libvirt gateway, to the LGTM stack
Chapter 3 stood up on the host; and `fwatch` runs sandboxed under Landlock
inside each fleet, its enforced ABI itself differing between the host kernel
and the guest kernel — a real, verified number, not a claim. Nearly every
part of this book from Setting Up through Performance and Low Latency shows
up somewhere in the run below.

{% include excalidraw.html
   file="41-fleet-topology"
   alt="Two side-by-side VM boxes labeled systems-target (192.168.124.7) and systems-peer (192.168.124.95), each drawn as a large rounded rectangle representing one fleet. Inside each box, a top band labeled 'pmon: capability bounding set dropped, no_new_privs=1' encloses three child boxes side by side: chatterd, sysagent, and fwatch, each connected upward to a small pmon box with a supervise/restart arrow labeled 'fork+exec, restart on unexpected exit, forward SIGTERM'. The fwatch box additionally sits inside its own smaller nested box labeled 'Landlock ruleset: reads restricted to sandbox dir' with its enforced ABI number shown as a variable (7 on this guest's 6.19.10 kernel). A bidirectional arrow runs directly between the two chatterd boxes, labeled 'bridge: bridge@target / bridge@peer, DELIVER re-broadcast with from=NICK@node'. Below both VM boxes, a horizontal band represents the libvirt virbr0 bridge and its gateway address 192.168.124.1; two upward arrows from each guest's sysagent box pass through the gateway band labeled 'OTLP/HTTP :4318' and converge on a host-side box labeled 'LGTM stack (Grafana + Prometheus + Tempo), Chapter 3' sitting outside and above both VM boxes. A small caption near the gateway notes 'same route deploy-to-vm.sh forwards OTEL_EXPORTER_OTLP_ENDPOINT over.'"
   caption="Figure 41.1 — the two-VM fleet: pmon supervises chatterd/sysagent/fwatch on each of systems-target and systems-peer inside a capability-bounding-set-drop boundary, fwatch additionally sandboxed under Landlock, chatterd bridged directly between the two guests, and both guests' sysagent exporting OTLP over the libvirt gateway to the host's LGTM stack" %}

> **Tools used** — `ssh`/`scp` (host, to stage the built binary onto both
> `systems-target` and `systems-peer` and drive `pmon` on each — the same
> `scripts/lab/deploy-to-vm.sh` transport every VM example uses), `curl`
> (host, querying Grafana's Prometheus proxy for the `sysagent_cpu_pct`
> gauge). The libvirt bridge's gateway address (`192.168.124.1`) is the
> route guest-side OTLP traffic takes to reach the host's LGTM stack — a
> network path, not a tool. Capability-bounding-set drop and Landlock are
> syscalls the program makes itself (`prctl(2)`, `landlock_create_ruleset(2)`
> and friends); seccomp was already proven alongside Landlock in Chapter 33
> and is not re-derived here. `ssh`/`scp`/`curl` ship in Fedora's base
> install and aren't separately checked by `scripts/check-host.sh`, the same
> way Chapter 36 notes for `curl` itself.

## One binary, four programs: pmon as fleet init

`app`'s four subcommands share one binary and one usage banner:

```
pmon [--node NAME] [--sandbox-dir DIR] [--peer HOST:PORT] [--peer-node NAME]
     [--chatterd-port P] [--health-interval-ms N]
chatterd serve [--host H] [--port P] [--node NAME] [--peer HOST:PORT] [--peer-node NAME]
chatterd send   --host H --port P --nick NICK --text TEXT [--timeout-ms T]
chatterd listen --host H --port P --nick NICK [--timeout-ms T]
sysagent [--node NAME] [--interval-ms N] [--once]
sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]
fwatch snapshot DIR
fwatch watch DIR [--sandbox] [--timeout-ms T]
```

`pmon` never execs a *different* binary for chatterd, sysagent, or fwatch —
it re-execs **itself**, with a different `argv[0]` command word, the same
self-re-exec technique Chapter 34's container entrypoint used. Concretely:
`mkdir` the sandbox directory, drop the capability bounding set once, resolve
the running binary's own path (`os.Executable()` in Go,
`readlink("/proc/self/exe")` in C++ and Rust), then fork+exec that same path
three times with three different argument lists — `chatterd serve …`,
`sysagent --node … --interval-ms 2000`, `fwatch watch DIR --sandbox`.

{% include excalidraw.html
   file="41-supervise-lifecycle"
   alt="A state-machine diagram for one supervised child. A box labeled starting has an arrow to a box labeled up, annotated 'fork+exec succeeded, pid recorded, pmon: started service=NAME pid=PID printed'. From up, a dashed arrow labeled 'blocking wait (waitpid/cmd.Wait/nix::waitpid)' loops back into a decision diamond: 'shutting_down flag set?'. The no-branch leads to a box labeled restarting, annotated 'pmon: restart service=NAME attempt=N reason=... printed, restarts counter incremented, 300ms backoff sleep', with an arrow looping back up to starting. The yes-branch leads to a box labeled down. Off to the side, a separate small loop shows a clock icon labeled 'health tick every --health-interval-ms' with an arrow into a box printing 'pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0'. A third flow at the bottom shows SIGTERM/SIGINT arriving at a box labeled 'signal thread (sigwait / signal.Notify channel / SigSet::wait)', with an arrow to 'shutting_down.store(true)', then a fan-out arrow to 'kill(child_pid, SIGTERM) for every recorded pid', ending in a box labeled 'pmon: shutdown, exit 0' once every per-child supervisor has returned."
   caption="Figure 41.2 — pmon's per-child supervise/restart lifecycle (starting → up → wait → restarting → up again on an unexpected exit), the health tick alongside it, and the SIGTERM → forward-to-children → shutdown path" %}

Before any of that forking happens, `pmon` drops every capability it might
hold from its own bounding set and sets `no_new_privs`. `PR_CAPBSET_DROP`
removes a capability from the *bounding set*, not the current effective set —
the guarantee it gives is that no future `execve` of a setuid-root binary,
by this process or any descendant, can ever regain that capability, because
the bounding set only shrinks across `fork`/`exec`. That guarantee holds
even when the calling process is already unprivileged, which is exactly
`pmon`'s situation on an unprivileged lab-guest account: a loopback run
prints `pmon: capabilities bounding_set_dropped=0 no_new_privs=1`, and `0` is
the *correct* number here — there was nothing in the bounding set to drop,
and the line still proves the `prctl` calls ran and `no_new_privs` is set.

{% include codetabs.html langs="C++|Go|Rust" %}

```go
func dropCapabilityBoundingSet() {
	dropped := 0
	for c := 0; c <= lastKnownCap; c++ {
		if err := unix.Prctl(unix.PR_CAPBSET_DROP, uintptr(c), 0, 0, 0); err == nil {
			dropped++
		}
	}
	if err := unix.Prctl(unix.PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); err != nil {
		fmt.Fprintf(os.Stderr, "pmon: prctl(PR_SET_NO_NEW_PRIVS): %v\n", err)
		return
	}
	fmt.Fprintf(os.Stderr, "pmon: capabilities bounding_set_dropped=%d no_new_privs=1\n", dropped)
}
```

```cpp
void drop_bounding_set() {
    int dropped = 0;
    for (int c = 0; c <= kLastKnownCap; ++c) {
        if (::prctl(PR_CAPBSET_DROP, c, 0, 0, 0) == 0) {
            ++dropped;
        }
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        std::fprintf(stderr, "pmon: prctl(PR_SET_NO_NEW_PRIVS) failed\n");
        return;
    }
    std::fprintf(stderr, "pmon: capabilities bounding_set_dropped=%d no_new_privs=1\n", dropped);
}
```

```rust
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
```

All three loop `prctl(PR_CAPBSET_DROP, c, …)` from `0` through
`CAP_CHECKPOINT_RESTORE` (40, current as of a 6.x kernel) and ignore `EINVAL`
from probing past whatever the running kernel's actual last capability is —
the loop is future-proof by construction rather than by a version check.
Once the bounding set is shrunk, every child `pmon` forks inherits the
narrower set automatically; nothing in `chatterd`, `sysagent`, or `fwatch`
has to know this happened.

With the bounding set dropped, `pmon` spawns and supervises the three
children — one goroutine per service in Go, one `std::thread` per service in
C++, one `std::thread` per service in Rust, each running the same
restart-on-unexpected-exit loop Figure 41.2 diagrams:

{% include codetabs.html langs="C++|Go|Rust" %}

```go
		go func() {
			defer wg.Done()
			for {
				cmd := exec.Command(selfExe, spec.args...)
				cmd.Stdout = os.Stdout
				cmd.Stderr = os.Stderr
				cmd.Env = os.Environ()
				if err := cmd.Start(); err != nil {
					fmt.Fprintf(os.Stderr, "pmon: start service=%s: %v\n", spec.name, err)
					time.Sleep(500 * time.Millisecond)
					continue
				}
				cs.pid.Store(int64(cmd.Process.Pid))
				cs.state.Store("up")
				fmt.Printf("pmon: started service=%s pid=%d\n", spec.name, cmd.Process.Pid)

				waitErr := cmd.Wait()
				if shuttingDown.Load() {
					cs.state.Store("down")
					return
				}
				n := cs.restarts.Add(1)
				cs.state.Store("restarting")
				fmt.Printf("pmon: restart service=%s attempt=%d reason=%v\n", spec.name, n, waitErr)
				time.Sleep(300 * time.Millisecond)
			}
		}()
```

```cpp
void run_child_supervisor(const std::string& self_exe, const ChildSpec& spec, ChildState& cs,
                           std::atomic<bool>& shutting_down) {
    for (;;) {
        const pid_t pid = ::fork();
        if (pid < 0) {
            std::println(stderr, "pmon: start service={}: {}", spec.name, std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (pid == 0) {
            // Unblock what pmon::run blocked in the parent (see pmon::run):
            // a forwarded SIGTERM must reach this child's own handler, not
            // sit blocked forever.
            sigset_t empty;
            sigemptyset(&empty);
            ::pthread_sigmask(SIG_SETMASK, &empty, nullptr);

            std::vector<char*> argv;
            argv.reserve(spec.args.size() + 2);
            argv.push_back(const_cast<char*>(self_exe.c_str()));
            for (const auto& a : spec.args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            ::execv(self_exe.c_str(), argv.data());
            ::_exit(127); // execv failed
        }

        cs.pid.store(pid, std::memory_order_relaxed);
        cs.state.store(State::Up, std::memory_order_relaxed);
        std::println("pmon: started service={} pid={}", spec.name, pid);
        std::fflush(stdout);

        int status = 0;
        ::waitpid(pid, &status, 0);

        if (shutting_down.load(std::memory_order_relaxed)) {
            cs.state.store(State::Down, std::memory_order_relaxed);
            return;
        }

        const long n = cs.restarts.fetch_add(1, std::memory_order_relaxed) + 1;
        cs.state.store(State::Restarting, std::memory_order_relaxed);
        std::string reason;
        if (WIFEXITED(status)) {
            reason = "exit status " + std::to_string(WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            reason = std::string("signal: ") + ::strsignal(WTERMSIG(status));
        } else {
            reason = "unknown";
        }
        std::println("pmon: restart service={} attempt={} reason={}", spec.name, n, reason);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}
```

```rust
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
```

Go delegates the fork+exec mechanics to `os/exec.Command`, which is why its
version is shorter — `cmd.Start()`/`cmd.Wait()` hide the raw syscalls C++ and
Rust make directly (`fork()`, then `execv()` in the child, then `waitpid()`
in the parent). But the *shape* is identical across all three: start, record
the pid, print `pmon: started`, block until the child exits, check whether
this exit was requested (`shutting_down`) or not, and if not, count a
restart, print `pmon: restart … reason=…`, back off 300 ms, and loop. A
child that fails to even start (`cmd.Start()` erroring, `fork()` returning
`< 0`) is not fatal to `pmon` — it logs, waits 500 ms, and retries, because a
supervisor's job is to keep trying, not to give up on the first bad attempt.

## chatterd, bridged across two hosts

`chatterd` (Chapters 21–27) speaks the same wire frame it always has —
magic `"CH"`, version 1, a type byte, a big-endian `u16` length, then payload
(`proto.go`/`.hpp`/`.rs`, unchanged) — and this chapter adds exactly one new
idea on top: with `--peer HOST:PORT` set on both sides, a message sent on
one node is delivered to a listener on the other. The mechanism needs no
protocol change at all. A **bridge worker** is just an ordinary client of
the *other* node's server, joined under the nick `bridge@<local-node>`:

```
target dials peer, joins as "bridge@target"   (lives in peer's client registry)
peer   dials target, joins as "bridge@peer"   (lives in target's client registry)
```

A local `MSG` is broadcast as `DELIVER` to every registered connection —
including a bridge connection dialed in from the other side — exactly like
any other client. When a bridge *worker* itself receives a `DELIVER` over the
connection it dialed out, it re-broadcasts locally with
`includeBridges=false`, so the message never goes back out over any bridge:
no ping-pong between two nodes forwarding the same line back and forth
forever. The one piece of new logic lives in the bridge worker's receive
loop — tag the nick with the *sending* node's name, if it isn't tagged
already, before re-broadcasting locally:

{% include codetabs.html langs="C++|Go|Rust" %}

```go
func bridgeWorker(peerAddr, localNode, peerNode string, h *hub, sig interface{ Load() bool }) {
	backoff := 300 * time.Millisecond
	for !sig.Load() {
		conn, err := net.DialTimeout("tcp", peerAddr, 3*time.Second)
		if err != nil {
			time.Sleep(backoff)
			continue
		}
		bridgeNick := "bridge@" + localNode
		if err := writeFrame(conn, frame{typ: typeJoin, payload: []byte(bridgeNick)}); err != nil {
			conn.Close()
			time.Sleep(backoff)
			continue
		}
		fmt.Fprintf(os.Stderr, "chatterd: bridge connected peer=%s as=%s\n", peerAddr, bridgeNick)

		for {
			f, err := readFrame(conn)
			if err != nil {
				break
			}
			if f.typ != typeDeliver {
				continue
			}
			nick, text, ok := splitDeliver(f.payload)
			if !ok {
				continue
			}
			if !strings.Contains(nick, "@") {
				nick = nick + "@" + peerNode
			}
			h.broadcastDeliver(nil, false, nick, text)
		}
		conn.Close()
		fmt.Fprintf(os.Stderr, "chatterd: bridge disconnected peer=%s (retrying)\n", peerAddr)
		time.Sleep(backoff)
	}
}
```

```cpp
void bridge_worker(std::string peer_addr, std::string local_node, std::string peer_node, Hub& hub,
                    std::atomic<bool>& sig) {
    const auto colon = peer_addr.rfind(':');
    if (colon == std::string::npos) {
        return;
    }
    const std::string host = peer_addr.substr(0, colon);
    const int port = std::atoi(peer_addr.substr(colon + 1).c_str());
    constexpr auto kBackoff = std::chrono::milliseconds(300);

    while (!sig.load(std::memory_order_relaxed)) {
        auto conn = connect_tcp(host, port, 3000);
        if (!conn) {
            std::this_thread::sleep_for(kBackoff);
            continue;
        }
        const std::string bridge_nick = "bridge@" + local_node;
        proto::Frame joinf{proto::kJoin, {bridge_nick.begin(), bridge_nick.end()}};
        if (auto r = proto::write_frame(conn->get(), joinf); !r) {
            std::this_thread::sleep_for(kBackoff);
            continue;
        }
        std::println(stderr, "chatterd: bridge connected peer={} as={}", peer_addr, bridge_nick);
        std::fflush(stderr);

        for (;;) {
            auto f = proto::read_frame(conn->get());
            if (!f) {
                break;
            }
            if (f->type != proto::kDeliver) {
                continue;
            }
            std::string nick;
            std::string text;
            if (!proto::split_deliver(f->payload, nick, text)) {
                continue;
            }
            if (nick.find('@') == std::string::npos) {
                nick += "@" + peer_node;
            }
            hub.broadcast_deliver(nullptr, false, nick, text);
        }
        std::println(stderr, "chatterd: bridge disconnected peer={} (retrying)", peer_addr);
        std::fflush(stderr);
        std::this_thread::sleep_for(kBackoff);
    }
}
```

```rust
fn bridge_worker(
    peer_addr: String,
    local_node: String,
    peer_node: String,
    hub: Arc<Hub>,
    sig: &'static AtomicBool,
) {
    let Some((host, port_str)) = peer_addr.rsplit_once(':') else {
        return;
    };
    let Ok(port) = port_str.parse::<u16>() else {
        return;
    };
    let backoff = Duration::from_millis(300);

    while !sig.load(Ordering::Relaxed) {
        let conn = match connect_tcp(host, port, 3000) {
            Ok(c) => c,
            Err(_) => {
                thread::sleep(backoff);
                continue;
            }
        };
        let raw = conn.as_raw_fd();
        let bridge_nick = format!("bridge@{local_node}");
        let joinf = Frame {
            typ: proto::TYPE_JOIN,
            payload: bridge_nick.clone().into_bytes(),
        };
        if proto::write_frame(raw, &joinf).is_err() {
            thread::sleep(backoff);
            continue;
        }
        eprintln!("chatterd: bridge connected peer={peer_addr} as={bridge_nick}");

        while let Ok(f) = proto::read_frame(raw) {
            if f.typ != proto::TYPE_DELIVER {
                continue;
            }
            let Some((mut nick, text)) = proto::split_deliver(&f.payload) else {
                continue;
            };
            if !nick.contains('@') {
                nick.push('@');
                nick.push_str(&peer_node);
            }
            hub.broadcast_deliver(None, false, &nick, &text);
        }
        eprintln!("chatterd: bridge disconnected peer={peer_addr} (retrying)");
        thread::sleep(backoff);
        // `conn` drops here, closing the fd, before the loop reconnects.
    }
}
```

`peer_node` here is the *name of the node this worker dialed into* — so when
`dave` sends a message locally on `target`, `target`'s hub broadcasts
`DELIVER` to every local client including the `bridge@peer` connection that
`peer`'s own bridge worker dialed in with; that worker reads the `DELIVER`
back over the connection it opened, sees `dave` has no `@` in it yet, tags it
`dave@target`, and re-broadcasts locally on `peer` with
`includeBridges=false`. A listener on `peer` sees
`chatterd: received from=dave@target text=…` — the `@target` suffix is the
proof the message actually crossed the wire, not that it was echoed
somewhere local. The reconnect loop (dial, backoff, retry) means either side
can start first, or the other side can restart under `pmon`, without the
bridge ever needing a fixed startup order.

## sysagent and fwatch: what's observed, what's sandboxed

`sysagent` under the fleet is the same `/proc`-reading collector Chapter 36
built, trimmed to the three signals a fleet operator watches continuously:
`cpu_pct` (a `/proc/stat` tick delta), `mem_pct` (`/proc/meminfo`), and
`load1` (`/proc/loadavg`), printed every `--interval-ms` as
`sysagent: node=… cpu_pct=… mem_pct=… load1=… ts=…`. New in this chapter:
when `OTEL_EXPORTER_OTLP_ENDPOINT` is set, those same three numbers are also
registered as OTel **observable gauges** (`sysagent.cpu.pct`,
`sysagent.mem.pct`, `sysagent.load1`) with a callback that reads the latest
sample under a mutex and reports it tagged with a `node` attribute, exported
over OTLP/HTTP on a periodic reader. When the endpoint is unset, `sysagent`
prints `sysagent: otel disabled (OTEL_EXPORTER_OTLP_ENDPOINT not set)` and
runs exactly the same otherwise — telemetry here is additive, never a
precondition for the service coming up. `sysagent saturate --resource
cpu|mem --seconds N` is the chaos half: it spins worker threads doing
nothing but arithmetic, or holds a touched, resident memory allocation, so a
concurrently-running `sysagent`'s own `cpu_pct`/`mem_pct` visibly moves — the
USE-method callback this fleet can point at itself.

`fwatch` is Chapter 07–10's file watcher — `inotify_init1`, one
non-recursive watch on a directory, `create`/`modify`/`delete` lines — with
Chapter 33's Landlock ruleset available as `--sandbox`. The ruleset
restricts filesystem *reads* to the watched directory: `pmon` passes fwatch
`watch SANDBOX_DIR --sandbox`, and before the inotify loop starts, `fwatch`
probes the running kernel's Landlock ABI with
`landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`, builds a
ruleset scoped to that one directory, and calls `landlock_restrict_self`
after setting `no_new_privs` — the identical sequence Chapter 33 proved,
reused rather than re-derived. `fwatch: landlock ABI=N enforced dir=…` is
printed only once the kernel has actually accepted the restriction, and — as
the cross-check below shows — `N` is not a constant baked into the program;
it is whatever the kernel underneath happens to support. Capability-bounding-
set drop (applied once, by `pmon`, before any child exists) and Landlock
(applied per-process, by `fwatch` itself) are this capstone's two sandbox
layers; seccomp, Chapter 33's third layer, was proven there and is not
reapplied here.

## How the code works

`main` in all three languages dispatches on `argv[1]` — `pmon`, `chatterd`,
`sysagent`, or `fwatch` — through the same hand-rolled `argFlag`/`hasFlag`/
`firstPositional` scan the rest of the book's argument parsing uses, funnel-
ing every malformed invocation through one `usage()` call. `chatterd serve`
and `sysagent` (without `saturate`) each call `initTelemetry`, which is
additive by design: it reads `OTEL_EXPORTER_OTLP_ENDPOINT`, and if it's
unset, returns a no-op `telemetry` struct backed by `otel.Tracer`/
`otel.Meter` no-op implementations rather than failing the subcommand — a
service the fleet depends on for chatting and sampling `/proc` should never
refuse to start because an observability backend is unreachable or
unconfigured. When the endpoint *is* set, `chatterd` wraps every delivered
message in a `chatterd.deliver` span tagged with the sender's nick, text
length, node, and whether it arrived over a bridge, and prints
`chatterd: trace_id=… node=… from=…` for each one; `sysagent` registers its
three gauges and lets a periodic OTLP/HTTP exporter push them on its own
schedule. Self-re-exec is the one place the three languages genuinely
diverge in mechanism: Go's `os.Executable()` plus `filepath.Abs` resolves
the running binary through `/proc/self/exe` internally, while C++ and Rust
call `readlink("/proc/self/exe", …)` directly — same symlink, three ways to
read it, all landing on the same absolute path that every forked child then
`execv`s.

## Errors, three ways

`app` has two usage-error surfaces and one runtime-error surface. The **top-
level usage error** — no command, an unrecognized subcommand, `chatterd`/
`fwatch` with no sub-subcommand, `chatterd send` missing `--nick`/`--text` —
prints the shared `usage: app <command>` banner and exits **2**.
`sysagent saturate` has its **own**, narrower usage error: a missing
`--resource` prints `usage: sysagent saturate --resource cpu|mem --seconds N
[--workers K|--mb M]` — a second banner, not the top-level one, because
`saturate` is itself a small program with its own contract. **Runtime
errors** are exit **1** with a `service: error message` line: `chatterd:
listen H:P: …` on a bind failure, `fwatch: Landlock not supported by this
kernel` when the probe returns ABI 0, `pmon: mkdir DIR: …` if the sandbox
directory can't be created. What `pmon` does *not* treat as a runtime error
is a single failed fork or exec attempt for one of its children — that's
logged (`pmon: start service=…: …`) and retried after a backoff, never
surfaced as a fatal error, because a process supervisor exists precisely to
absorb transient start failures rather than propagate them. A clean `pmon`
shutdown, a successful one-shot `sysagent --once`, or a `fwatch watch` that
runs to its `--timeout-ms` deadline or a delivered signal all exit **0**.

## Concurrency lens

Every piece of this book's other running thread — which concurrency model
each language reaches for — shows up in `pmon`'s two jobs: consuming exactly
one `SIGTERM`/`SIGINT` no matter how many children are running, and
supervising each child without one child's restart blocking another's.

Go's version is the one most readers will recognize: `signal.Notify` hands a
buffered channel to the runtime's own signal machinery, and a dedicated
goroutine blocks on `<-sigCh`, sets `shuttingDown`, and kills every recorded
child pid. Each of the three services gets its own goroutine running the
restart loop shown above, all three joined with a `sync.WaitGroup`; the main
goroutine waits on a `select` between the `WaitGroup`'s completion (via a
`done` channel) and a health-interval `time.Ticker`, printing
`pmon: health …` each time the ticker fires and returning once every child
has actually stopped.

C++ and Rust reach for the same alternative to a signal *handler*: a
dedicated **`sigwait`-consuming thread**. Both block `SIGTERM` and `SIGINT`
in the main thread with `pthread_sigmask`/`SigSet::thread_block()` *before*
spawning any other thread — a blocked mask is inherited by every thread
created afterward, so only the one thread that later calls
`sigwait`/`SigSet::wait()` (nix's wrapper over the same syscall) ever
consumes the signal; no async-signal-unsafe work happens in a handler at
all, because there is no handler. Each of the three services then gets its
own `std::thread`, and — unlike Go's `exec.Command`-based children — that
thread performs the `fork()`/`execv()` **itself**, directly: `pmon`'s C++
and Rust ports are "fork+threads" in the sense the task names them, one OS
thread owning one child's entire fork/wait/restart lifecycle. Every forked
child clears its own inherited copy of the blocked signal mask
(`pthread_sigmask(SIG_SETMASK, &empty, …)` in C++, `SigSet::empty().
thread_set_mask()` in Rust) immediately after `fork()` and before `execv()`
— without this, the child would inherit a *blocked* `SIGTERM`/`SIGINT` and
its own shutdown-signal handling (the same `install_signal_flag`
mechanism `chatterd`/`sysagent`/`fwatch` use standalone) could never fire,
the exact bug Chapter 34's container entrypoint first ran into and
documented. Where C++ and Rust part ways is the health-tick wait: C++'s main
thread holds a `std::condition_variable` and calls `wait_for(lock, interval,
predicate)`, waking early the moment every worker thread's `active` counter
reaches zero and otherwise printing health once per timeout; Rust's main
thread has no condvar at all — it just `thread::sleep`s the health interval
and polls `JoinHandle::is_finished()` on every worker, a coarser but simpler
wake pattern that trades a small amount of shutdown latency (up to one
health interval) for not needing a shared mutex/condvar pair at all.

## Build, run, observe

```bash
[host]$ cd examples/41-capstone-fleet && ./demo.sh build
```

`verify.lua` follows the convention Chapter 23 established for `vm-peer`
examples: the assertion itself runs entirely on loopback — the fleet, the
cross-node bridge across two `127.0.0.1` ports, and telemetry to a local
LGTM stack — while the real two-host run is the chapter's demo. A loopback
fleet, driven by hand the same way, shows every piece in one place:

```console
[host]$ ./go/bin/app pmon --node p1 --sandbox-dir /tmp/fwatch-sandbox
pmon: capabilities bounding_set_dropped=0 no_new_privs=1
pmon: started service=chatterd pid=<PID>
chatterd: listening on 0.0.0.0:47100 node=p1
pmon: started service=sysagent pid=<PID>
sysagent: node=p1 cpu_pct=<CPU> mem_pct=<MEM> load1=<LOAD> ts=<TS>
pmon: started service=fwatch pid=<PID>
fwatch: landlock ABI=9 enforced dir=/tmp/fwatch-sandbox
pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0
^C
pmon: shutdown
```

(`ABI=9` here is the reference host's own Landlock version — see the
cross-check below for the guest's different number.) A local listener
receiving a message sent by another client on the same node, and the
cross-node bridge with two nodes on loopback, are exactly the shapes
`verify.lua` asserts and that produced this session's evidence:

```console
chatterd: received from=alice text=hello fleet
chatterd: received from=dave@A text=cross node
```

```console
[host]$ python3 scripts/test-all-examples.py --only 41-capstone-fleet --mode vm
verifying...
  verify 41-capstone-fleet [cpp]: PASS
  verify 41-capstone-fleet [go]: PASS
  verify 41-capstone-fleet [rust]: PASS

example              cpp   go    rust
41-capstone-fleet     PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

Each language's `verify.lua` run reported `PASS 31 / FAIL 0`.

The real payoff is the two-host run this chapter has been building toward:
`pmon` staged and started on both `systems-target` (192.168.124.7) and
`systems-peer` (192.168.124.95), bridged to each other, both exporting OTLP
through the libvirt gateway (`192.168.124.1`) to the host's LGTM stack:

```console
[host]$ scp app fedora@192.168.124.7:~/app
[host]$ scp app fedora@192.168.124.95:~/app
[vm]$   OTEL_EXPORTER_OTLP_ENDPOINT=http://192.168.124.1:4318 \
          ./app pmon --node target --peer 192.168.124.95:47100 --peer-node peer
[peer]$ OTEL_EXPORTER_OTLP_ENDPOINT=http://192.168.124.1:4318 \
          ./app pmon --node peer --peer 192.168.124.7:47100 --peer-node target
```

Both guests reached full health:

```console
[vm]$   pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0
[peer]$ pmon: health chatterd=up sysagent=up fwatch=up restarts=chatterd:0,sysagent:0,fwatch:0
```

And the cross-host bridge delivered a message sent on `target` to a listener
on `peer`:

```console
[vm]$   ./app chatterd send --host 127.0.0.1 --port 47100 --nick dave --text "hello across hosts"
[peer]$ ./app chatterd listen --host 127.0.0.1 --port 47100 --nick carol
chatterd: received from=dave@target text=hello across hosts
```

## Cross-check: an ABI the kernel decides, and a gauge that crosses the gateway

Two independent proofs back this chapter's two biggest claims, and neither
comes from a line the program prints about its own intentions.

The first is Landlock's enforced ABI. `fwatch --sandbox` never hardcodes a
version — it asks the running kernel with
`landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`, the
exact probe Chapter 33 established, and prints whatever comes back. On the
Fedora 44 reference host (kernel `7.1.3-200.fc44`), where this chapter's
loopback `verify.lua` runs, that probe returns **ABI 9**. On the
`systems-target` lab guest (kernel `6.19.10-300.fc44`) this session, the
identical code, the identical syscall, returned **ABI 7**. That is not a
bug and not a discrepancy to reconcile — it is the accurate, verified
consequence of two different kernel versions supporting two different sets
of Landlock features, and it is the same lesson Chapter 33 taught about
attach targets and struct offsets, reached through a sandbox ABI instead: a
program that hardcodes "ABI 9" would be *wrong* on the guest, and `fwatch`'s
own probe is what keeps it right on both.

The second is the OTLP path itself. `sysagent: otel enabled endpoint=…`
proves the exporter was configured; it does not prove a single byte reached
anywhere. The real proof is on the other end of the gateway: after the
two-host bridge run above, `sysagent_cpu_pct{node="peer"}` — queried through
Grafana's Prometheus proxy — was present with value **1.75**, a gauge that
started life inside an unprivileged process on `systems-peer`, was
exported over OTLP/HTTP to `http://192.168.124.1:4318`, crossed the libvirt
bridge's gateway, and landed in the host's own Prometheus, labeled by the
node that produced it. That is the guest→host telemetry path Chapter 3's
LGTM stack exists to receive, closed end to end.

## What you learned

- **`pmon` is the fleet's own init**: it drops the capability bounding set
  and sets `no_new_privs` once, then self-re-execs its own binary three
  times with three different `argv[0]` commands, restarting whichever child
  exits unexpectedly and forwarding its own `SIGTERM`/`SIGINT` to every
  recorded child pid on shutdown.
- **`PR_CAPBSET_DROP` is meaningful even on an already-unprivileged
  process.** `bounding_set_dropped=0` on the lab guest's unprivileged
  account is the *correct* number — the guarantee (no future `execve` of a
  setuid binary can regain a dropped capability) holds regardless of what
  was there to begin with.
- **A bridge is just a client with a special nick.** `chatterd`'s cross-host
  bridging needed no protocol change: `bridge@<node>` is an ordinary
  connection in the peer's client registry, `includeBridges=false` on
  re-broadcast is what stops a two-node ping-pong, and the `@node` suffix on
  a delivered nick is the observable proof a message actually crossed.
- **A sandbox's enforced feature set is a kernel fact, not a program's
  assumption.** The identical Landlock probe returned ABI 9 on the Fedora
  44 host and ABI 7 on the lab guest's 6.19.10 kernel this session — the
  same class of version drift Chapter 33 taught, reached through a
  capability-negotiation syscall instead of an attach point.
- **Telemetry is additive, and the guest→host OTLP path is real, not
  assumed.** Every service ran identically with `OTEL_EXPORTER_OTLP_ENDPOINT`
  unset; with it set, `sysagent_cpu_pct{node="peer"}` — 1.75 on this
  session's run — reached the host's Prometheus over the libvirt gateway,
  the same route `deploy-to-vm.sh` forwards for every OTLP-emitting VM demo.
- **Identical supervision, three concurrency shapes.** Go leans on
  `os/exec.Command`, goroutines, a `sync.WaitGroup`, and a `signal.Notify`
  channel; C++ and Rust both fork and `execv` directly inside a dedicated
  `std::thread` per child and consume signals on a separate `sigwait`/
  `SigSet::wait` thread rather than a handler — and even between those two,
  C++'s health tick wakes early off a condition variable while Rust's just
  polls `is_finished()` on a sleep — the same restart/health/shutdown
  contract, four genuinely different mechanisms underneath.

This closes the six-program arc that began in Chapter 7: four of the six —
`pmon`, `chatterd`, `sysagent`, `fwatch` — now run together, supervised,
sandboxed, and observed, across two real hosts. The book continues into
three deep dives — embedding Lua, Rust macros for systems code, the Go
runtime — but every syscall, sandbox layer, and signal-handling discipline
those chapters lean on was proven here, running for real.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host (kernel <code>7.1.3-200.fc44</code>) and the two lab guests
<code>systems-target</code> (192.168.124.7) / <code>systems-peer</code>
(192.168.124.95, kernel <code>6.19.10-300.fc44</code>) this session:
<code>python3 scripts/test-all-examples.py --only 41-capstone-fleet --mode
vm</code> printed <code>PASS PASS PASS</code> (3 passed, 0 failed;
<code>verify.lua</code> reported <code>PASS 31 / FAIL 0</code> for each of
cpp/go/rust, exercised entirely on loopback per the Chapter 23 vm-peer
convention). On the real two-host run, <code>pmon</code> on both
<code>systems-target</code> and <code>systems-peer</code> reached
<code>pmon: health chatterd=up sysagent=up fwatch=up</code>; a message sent
on <code>target</code> (nick <code>dave</code>, text "hello across hosts")
was delivered to a listener on <code>peer</code> as <code>chatterd: received
from=dave@target text=hello across hosts</code>, proving the cross-host
bridge; <code>fwatch --sandbox</code> on the guest printed <code>fwatch:
landlock ABI=7 enforced</code>, against <code>ABI=9</code> on the host's
loopback run — the same probe, two different kernels, a real and verified
difference; and with <code>OTEL_EXPORTER_OTLP_ENDPOINT=http://192.168.124.1:
4318</code> set on both guests, <code>sysagent_cpu_pct{node="peer"}</code>
reached the host's Prometheus (queried through Grafana's proxy) with value
<code>1.75</code>, confirming the guest→host OTLP path over the libvirt
gateway. Not exercised: seccomp filtering of any fleet child (Chapter 33
proved seccomp alongside Landlock; this capstone reuses only the
capability-bounding-set-drop and Landlock layers, not seccomp) and a run
where the LGTM stack was unreachable (that path reports SKIP for the
telemetry assertions, not a failing example, per the manifest's
<code>requires: [lgtm]</code>).</p>
