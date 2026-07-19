# Diagrams

Each diagram is a paired `name.svg` (committed, embedded via the `excalidraw.html`
include) + `name.excalidraw` (editable source). Generate both with
`scripts/generate_diagram.py` (see `references/diagram-engine.md`). Catalogue them
here as you add them:

| Diagram | Chapter | What it shows |
|---|---|---|
| `00-course-map` | 0 | The four journey stages, fourteen parts, and six recurring artifacts |
| `01-toolchain-landscape` | 1 | Three language toolchain stacks converging on the shared demo.sh contract |
| `02-lab-topology` | 2 | Host, libvirt NAT network, and the systems-target/systems-peer guests |
| `03-telemetry-pipeline` | 3 | Demos exporting OTLP into the lsp-lgtm container and out to Grafana |
| `07-fd-table-vfs` | 7 | fd table → open file description → inode, annotated with the four dirfds of a paused fwatch walk |
| `05-error-flow-three-ways` | 5 | One ENOSPC's journey from write(2) through errno into each language's error type, converging on identical output and exit 3 |
| `08-write-path-page-cache` | 8 | The write path user buffer → page cache → device, with each iobench mode's bypass and flush points |
| `04-user-kernel-path` | 4 | One getrandom call crossing the user/kernel boundary, and the vDSO fast path that never traps |
| `04-runtime-syscall-layers` | 4 | The C++/Go/Rust layer stacks converging on the one register-level kernel ABI, with real strace -c totals |
| `10-uring-rings` | 10 | The SQ ring, SQE array, and CQ ring shared via mmap, the acquire/release handoffs around them, and io_uring_enter as the only syscall |
| `10-readiness-vs-completion` | 10 | Readiness (epoll answers "may I?") beside completion (io_uring answers "it is done"), with the real per-engine syscall counts from fwatch |
| `09-event-loop-anatomy` | 9 | One epoll loop over three event sources — inotify, timerfd, signalfd — with the kernel eventpoll interest list and the readiness path back into epoll_wait |
| `06-three-concurrency-models` | 6 | parhash's producer/queue/workers/cancellation shape in all three vocabularies, with observed /proc thread counts |
| `06-cancellation-drain` | 6 | The SIGINT drain sequence — one flag flips, walker and workers wind down, in-flight hashes finish, exit 130 |
| `16-mapping-page-cache` | 16 | Two processes' MAP_SHARED mappings converging on one set of page-cache pages, msync vs writeback to disk, and a MAP_PRIVATE view forking its copy on first write |
| `16-shmkv-layout` | 16 | The shmkv v0 on-disk format byte for byte — magic, u32 LE slot_count, 256-byte slots split 64/192 — with the three languages reading the same bytes |
| `17-arena-vs-gc` | 17 | Three reclamation strategies side by side — free-one-by-one through glibc malloc, arena/pool batch release, and the Go GC's pacer — annotated with real allocbench peak_rss and gc_cycles numbers |
| `11-process-lifecycle-states` | 11 | Process states as ps STAT letters — running, sleeping, stopped, zombie — and the wait4/waitid reap that frees the pid, annotated with the chapter's live zombie window |
| `11-fork-exec-wait-seq` | 11 | pmon's fork → exec → wait sequence with the CLOEXEC self-pipe carrying the exec verdict, and the clone flavors Go and Rust use for the same dance |
| `19-uds-control-plane` | 19 | pmon v5's supervise/pmctl control plane over a filesystem UNIX stream socket — listener, accepted fd with SO_PEERCRED, and the connected socket-inode pair ss -xp reported |
| `19-scm-rights-transfer` | 19 | SCM_RIGHTS crossing the socket: sender slot 3 and receiver slot 4 pointing at one shared open file description (pos 47, inode 95934), with the real sendmsg/recvmsg cmsg values from the chapter run |
| `12-signal-delivery-paths` | 12 | Disposition, per-thread mask, and pending set in the kernel, feeding the three consumption paths pmon uses — signalfd reads, Go runtime channels, and the self-pipe doorbell |
| `12-supervisor-state-machine` | 12 | pmon supervise's two live states (child running, backoff wait) and the exits, with SIGTERM/SIGINT, SIGHUP reload, and the doubling backoff as transitions |
| `14-credential-sets-caps` | 14 | The three-uid state machine and the five capability sets across pmon's drop — KEEPCAPS → setresuid → capset → AMBIENT_RAISE → execve beside the negative control, annotated with the real /proc status values from systems-target |
| `15-address-space-map` | 15 | One x86-64 process address space top to bottom — [vsyscall], [stack], the mmap region with the 64 MiB anonymous allocation, [heap], data, text — with real addresses and growth arrows |
| `15-fault-flow` | 15 | One touched page's journey — MMU miss, trap, VMA lookup — fanning out to SIGSEGV, anonymous minor fault, page-cache minor fault with fault-around, and major fault, with measured counts |
| `20-futex-wait-wake` | 20 | One shmkv wake end to end — watcher parks in FUTEX_WAIT on the mapped word, writer publishes under the seqlock and calls FUTEX_WAKE, kernel wait queue keyed by (inode, offset) hands the watcher back — with the EAGAIN lost-wakeup guard |
| `20-ipc-matrix` | 20 | The coordination ladder as measured on this host — futex, eventfd, pipe, POSIX mq, UNIX socket, and the 1 ms poll loop, with bench p50/p99 and the voluntary context-switch evidence |
| `13-pid-reuse-race-pidfd` | 13 | The five-step pid-reuse race timeline — spawn, crash, reap, recycle, kill(2) hitting a stranger — above the four-step pidfd lane that pins identity to an fd, annotated with the real fdinfo Pid: and strace evidence from pmon v2 |
| `18-pipe-internals` | 18 | One live pipe as the kernel sees it — write end fd 1 and read end fd 5 around the 16-slot ring of pages, with the measured 65536 B capacity, the F_SETPIPE_SZ page rounding, and what PIPE_BUF atomicity does and does not promise |
| `18-log-topology` | 18 | pmon v4's full log pipeline — child stdio through two O_CLOEXEC pipes into the supervisor's poll loop and the framed log, then tail splicing file→FIFO to a detachable cat reader, annotated with the run's real fds, inodes, flags, and splice return values |
