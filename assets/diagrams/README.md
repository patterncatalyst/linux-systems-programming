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
