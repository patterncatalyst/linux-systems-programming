# 20-shared-memory-coordination — shmkv v1

shmkv grows from a passive shared-memory key-value file (v0) into a live
coordination fabric: one process publishes updates, other processes learn
about them **without polling**, through a seqlock for consistency and a futex
for wakeups — the same pair of primitives glibc builds mutexes and condition
variables from. Implemented three times (C++23, Go 1.26, Rust 2024) with
identical observable behavior.

## The v1 file format (SHKV2)

```
offset  size  field
0       8     magic "SHKV2\0\0\0"
8       4     seqlock word    (u32; odd = writer in critical section)
12      4     futex word      (u32; low 32 bits of the update counter)
16      8     update counter  (u64; total updates published)
24      40    reserved
64      512   8 slots x 64 bytes: key[24] NUL-padded, value[40] NUL-padded
```

One page (4096 bytes), mapped `MAP_SHARED` by every participant. The slots
form a ring keyed by `(id-1) % 8`, so a watcher that sleeps through a wake
can back-fill up to 8 missed updates from the ring.

## Subcommands

```
shmkv serve FILE [--updates N] [--interval-ms T]
shmkv watch FILE [--events N]
shmkv bench FILE [--rounds N] [--channel futex|mq|poll]
```

- **serve** (writer): creates/initializes FILE, then publishes N updates
  (`k<i>=value-<i>`), each under the seqlock, bumping the u64 counter and the
  u32 futex word, then `FUTEX_WAKE`-ing all waiters. Prints one
  `published update <k>: <key>=<value>` line per update.
- **watch** (reader): validates the magic, then loops `FUTEX_WAIT` on the
  futex word; on each wake it takes a seqlock-consistent snapshot and prints
  `observed update <k>: <key>=<value>` for every new id, exiting 0 after N
  events (or 1 after a 30 s self-termination deadline).
- **bench**: producer/consumer threads over the same mapping measure one-way
  notification latency for three channels — `futex` (wake/wait), `mq` (a
  POSIX message queue carrying the update id), `poll` (a 1 ms sleep loop
  re-reading the counter; it sleeps *before* looking, so every observation
  costs at least one full nap). Rounds are strictly serialized by an ack word
  so queueing never masquerades as latency. Output is one row:
  `bench: channel=<c> rounds=<n> p50_us=<a> p99_us=<b>`. The message queue
  is unlinked on exit; nothing survives in `/dev/mqueue`.

Errors: usage problems exit 2; a missing FILE or a file without the SHKV2
magic exits 1 with a `shmkv:`-prefixed message on stderr.

## The seqlock protocol

```
write: seq += 1 (relaxed)   ; release fence ; mutate slots + counter ; seq += 1 (release)
read:  s1 = seq (acquire)   ; if s1 odd retry
       copy counter + slots ; acquire fence
       s2 = seq (relaxed)   ; consistent iff s1 == s2, else retry
```

The data copy races the writer on purpose; a torn copy is detected by the
seq re-check and discarded. That trade-off — writers never block, readers
retry — is exactly the kernel's seqlock. The retry loop is bounded (10^6
attempts) so a corrupt file cannot livelock a reader forever.

The futex is **shared** (no `FUTEX_PRIVATE_FLAG`): the kernel keys waiters
by the file's inode, which is what lets two unrelated processes rendezvous
through the same mapped page. `EAGAIN`, `ETIMEDOUT`, and `EINTR` from
`FUTEX_WAIT` are all normal — the waiter re-reads shared state and decides
again.

## Language notes

- **C++23**: RAII wrappers for the fd, the mapping, and the message queue;
  `std::atomic_ref` binds atomics onto the mapped words; raw
  `syscall(SYS_futex, ...)`; `std::jthread` for the bench consumer;
  `std::expected` carries setup errors to `main`.
- **Go**: `golang.org/x/sys/unix.Syscall6` for `SYS_FUTEX` and the four
  `mq_*` syscalls (the kernel takes the queue name *without* glibc's leading
  slash). **Futex caveat**: `FUTEX_WAIT` parks the calling OS thread in the
  kernel where the goroutine scheduler cannot reach it; sysmon hands the P to
  another thread so the program keeps running, but every concurrent waiter
  pins a kernel thread. Fine for one watcher per process; channels and the
  netpoller remain the scalable Go answer.
- **Rust (edition 2024)**: `rustix` `open` returns an `OwnedFd`; a small
  `Drop` type munmaps; `AtomicU32::from_ptr` overlays atomics on the mapping;
  the futex is the raw `libc::syscall(libc::SYS_futex, ...)` so the kernel
  contract stays visible; `std::thread::scope` runs the bench consumer.

## Run it

```sh
./demo.sh build                 # all three languages
./demo.sh cpp run serve /tmp/kv.shm --updates 8 --interval-ms 100 &
./demo.sh rust run watch /tmp/kv.shm --events 8      # cross-language: same file
./demo.sh go run bench /tmp/b.shm --rounds 200 --channel futex
```

Languages interoperate: a Go `serve` wakes a Rust `watch` on the same file,
because the contract is the page layout plus the futex, not the runtime.

## Verification (behavioral)

`verify.lua` asserts, per language:

1. usage → exit 2; missing file → exit 1; non-SHKV2 file → exit 1.
2. `serve` and `watch` run as two real processes (checked mid-run via
   `pgrep -f`); all 6 updates are observed with consistent `k<i>=value-<i>`
   pairs; both exit 0.
3. the file afterwards: SHKV2 magic, futex word = 6, u64 counter = 6, the
   final value string present in a slot, size exactly 4096.
4. `bench` emits a parseable row for each of futex/mq/poll; `p99 >= p50`;
   poll p50 ≥ 500 µs by construction; futex p50 < poll p50.
5. `/dev/mqueue` holds no `shmkv-bench-*` leftovers.
