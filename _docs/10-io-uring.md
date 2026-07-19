---
title: "io_uring: the completion model"
order: 10
part: "Files and I/O"
description: "fwatch grows a batched sync engine and meets the second of the two I/O mental models — completion. SQ and CQ rings shared with the kernel, one io_uring_enter carrying sixteen operations, real strace counts showing the syscall collapse, and real timings showing why fewer syscalls is not automatically faster."
duration: 60 minutes
---

Chapter 9 ended with `fwatch` blocked in an event loop, and with a promise:
epoll would not be the last word on Linux I/O. Epoll answers one question —
*which of these fds may I touch without blocking?* — and then leaves every
actual `read(2)` and `write(2)` for you to make, one syscall per operation.
This chapter adds `fwatch sync`, a small directory-copy subcommand with two
engines: `--engine rw`, the plain read/write loop you already know, and
`--engine uring`, which never issues a data `read(2)` or `write(2)` at all.
Instead it *describes* batches of operations in memory the kernel shares with
the process, and submits sixteen of them per syscall. That is the
**completion model**, and readiness-versus-completion is the mental-model
pair this whole Part has been building toward. The chapter also delivers two
deliberate surprises: our uring engine is measurably *slower* on this
workload, and one of our three languages refuses to play at all — both on
purpose, both worth understanding.

The code is in `examples/10-io-uring/`. `./demo.sh` there builds and runs all
three implementations; its `README.md` covers the `scan`/`watch`/`sync`
subcommands and the two-engine contract.

{% include excalidraw.html
   file="10-uring-rings"
   alt="Three columns: user-space fwatch code filling SQEs and popping CQEs, the mmap'd shared memory holding the SQ ring, SQE array, and CQ ring, and the kernel consuming submissions and posting completions, with io_uring_enter as the single syscall between them."
   caption="Figure 10.1 — the io_uring anatomy: two rings and an SQE array shared via mmap; io_uring_enter is the only syscall in the data path" %}

> **Tools used** — `strace`, `diff`, `cmp`, `grep`, `python3` (host);
> `bpftrace` (lab VM, exercised in Part 8). Everything here is checked by
> `scripts/check-host.sh`, ships with Fedora, or is preinstalled in the lab VMs.

## Readiness asks "may I?", completion says "it is done"

Every I/O interface you have met so far is synchronous at the moment of data
movement: `read(2)` in chapter 7, `write(2)` through the page cache in
chapter 8, and epoll in chapter 9 — which, for all its scalability, only
*schedules* those same calls. Readiness is a hint about the future ("fd 7
will not block right now"), and it can even be wrong: a readable socket can
still return `EAGAIN` by the time you get there.

io_uring inverts the contract. You write a **submission queue entry** (SQE) —
a 64-byte struct naming an opcode (`IORING_OP_READ`), an fd, a buffer
address, a length, and a file offset — into an array the kernel mapped into
your address space. You publish it by advancing the tail index of the
**submission queue** (SQ) ring. Some time later the kernel leaves a
**completion queue entry** (CQE) in the CQ ring: `res` is the operation's
result (bytes transferred, or a negative errno), and `user_data` is an opaque
value you attached to the SQE so you can match the completion back to the
request. The syscall `io_uring_enter(2)` exists only to say "I have queued N
things; wake me when M of them are done" — and with `IORING_ENTER_GETEVENTS`
it does both halves in one crossing. One syscall can therefore carry any
number of operations, which is the entire economic argument: the fixed cost
of entering the kernel is amortized across a batch. There is even a mode that
eliminates the syscall entirely — `IORING_SETUP_SQPOLL` starts a kernel
thread that polls the SQ ring so a busy application never traps at all. We
mention it and move on: our engine does not use it, and the manpage itself
warns that it "may sound immediately appealing as an automatic go-faster
flag" but must be evaluated case by case. Hold that thought; our numbers
below make the same point without SQPOLL.

{% include excalidraw.html
   file="10-readiness-vs-completion"
   alt="Two columns. Left, readiness: epoll_wait asks which fds are ready, the kernel reports readiness events, the application still performs read and write itself, and the operation can still fail with EAGAIN. Right, completion: the application queues SQEs describing the I/O, one io_uring_enter submits the batch, the kernel performs the I/O inline or via io-wq workers, and CQEs carry the finished results."
   caption="Figure 10.2 — the mental-model pair: readiness (epoll) hints per fd; completion (io_uring) reports finished results per operation" %}

## Where uring wins — and why containers say no

The batch economics pay off where per-operation syscall overhead is a real
fraction of the work: thousands of small operations in flight against
storage that benefits from deep queues (`O_DIRECT` on NVMe), or servers
juggling enormous socket counts, where features like registered buffers and
multishot operations remove per-op setup cost too. They pay off *least* when
each operation is already almost free — and a buffered read of a page-cache-hot
file is a memcpy. Small trees of warm files are exactly that, which is why
the timings below look the way they do.

The same power has a security price. io_uring is a large, fast-moving kernel
attack surface, and it lets a process express nearly every syscall family
through one opcode-dispatched interface — which quietly bypasses syscall
filters that were written in terms of `read`/`write`/`connect`. Container
runtimes responded plainly: on this host, podman's default seccomp profile
(`/usr/share/containers/seccomp.json`) sets `"defaultAction": "SCMP_ACT_ERRNO"`
and its allowlist contains **zero** mentions of `io_uring_setup`,
`io_uring_enter`, or `io_uring_register` — deny by default. Run the uring
engine in an unmodified container and `io_uring_setup` fails with `EPERM`.
Chapter 34, when our programs move into containers, deals with that head-on.

## Go's deliberate absence

`fwatch sync --engine uring` in Go prints
`engine=uring: unsupported in Go (see chapter 10)` and exits 64. That is not
laziness — it is the chapter's design lesson. Go already solved "many I/O
operations, few OS threads" once, inside the runtime: every goroutine writes
ordinary blocking code, and the **netpoller** multiplexes those goroutines
over epoll, parking them until readiness arrives. The concurrency model *is*
the I/O model. Grafting a completion engine underneath would mean teaching
the scheduler that some I/O finishes on rings instead of unparking on
readiness, deciding who owns the buffers while the kernel holds them, and
doing it all without breaking the promise that `f.Read(buf)` just works. The
tracking issue the source comment cites — golang/go#31908, open since 2019 —
remains on hold for exactly these reasons. Rust, at the other extreme, ships
io_uring as a library (`io-uring`, the crate our Rust engine uses) and lets
runtimes compete; C++ gives you the raw syscalls and a free hand. Three
languages, three answers to the same question: *who owns the event loop?*

> **Research note — io_uring on this kernel, from primary sources.** Claims
> about io_uring age fast, so this chapter cites only what this host shows.
> Kernel `7.1.3-200.fc44` reported 18 `IORING_FEAT_*` flags in the
> `io_uring_setup` params during our traced run — from `SINGLE_MMAP` and
> `NODROP` through `RECVSEND_BUNDLE`, `MIN_TIMEOUT`, `RW_ATTR`, and
> `NO_IOWAIT` — and the shipped `io_uring(7)`, `io_uring_setup(2)`, and
> `io_uring_enter(2)` manpages document the setup/enter contract used here.
> Feature lists in older articles (and newer marketing) routinely disagree
> with both; when in doubt, trust the flags your kernel returns.

## How the code works

`fwatch` v2 carries the earlier chapters' machinery forward — the v0
polling walk survives as `scan` (now a one-line summary; the per-file
`snapshot`/`diff` pair stays with v1), v1's `watch` keeps its name — and
adds `cmd_sync`: walk the source tree, recreate directories, copy every
regular file, print `synced <N> files <B> bytes engine=<e> ms=<t>`. The
engines differ only in how one file's bytes move.

The C++ engine is the pedagogical core, because the host has no
`liburing-devel` and we turned that into a feature: a self-contained `Ring`
class built on raw `syscall(2)` wrappers, so every moving part is visible.
`Ring::create` calls `io_uring_setup(2)` with 64 entries, then performs the
three canonical mmaps of the returned fd — the SQ ring (head, tail, mask,
and an index array), the CQ ring (head, tail, and the CQE array), and the
SQE array itself — honoring `IORING_FEAT_SINGLE_MMAP`, which on modern
kernels folds the two rings into one mapping (our trace below shows exactly
that: one 2368-byte mapping at offset 0, one SQE mapping at
`IORING_OFF_SQES`). The interesting discipline is in the hot-path methods,
from `examples/10-io-uring/cpp/src/main.cpp`:

```cpp
    // Claim the next SQE slot (zeroed); nullptr when the SQ ring is full.
    [[nodiscard]] io_uring_sqe* try_get_sqe() {
        const unsigned head = std::atomic_ref(*sq_head_).load(std::memory_order_acquire);
        if (local_tail_ - head > sq_mask_) {
            return nullptr;
        }
        const unsigned idx = local_tail_ & sq_mask_;
        io_uring_sqe* sqe = &sqes_[idx];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array_[idx] = idx;
        ++local_tail_;
        ++to_submit_;
        return sqe;
    }

    // Publish queued SQEs and wait for wait_nr completions in one syscall.
    [[nodiscard]] expected<void, std::error_code> submit_and_wait(unsigned wait_nr) {
        std::atomic_ref(*sq_tail_).store(local_tail_, std::memory_order_release);
        return enter(std::exchange(to_submit_, 0), wait_nr);
    }
```

`try_get_sqe` load-acquires the SQ *head* — the kernel's consumption cursor —
to learn how much room remains; the ring is full when our private
`local_tail_` has run a whole ring-size ahead of it. Filled entries stay
private until `submit_and_wait` store-releases the shared *tail*, which is
the publish: everything written to the SQEs before that store is guaranteed
visible to the kernel after it. The mask trick (`local_tail_ & sq_mask_`)
works because ring sizes are powers of two, so a free-running 32-bit counter
maps onto a slot without ever being reset — the same idiom you will meet
again in the lock-free chapter.

On top of the ring, each engine copies one file the same way — here is the
same function three times:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
[[nodiscard]] expected<uint64_t, std::error_code> copy_file_uring(Ring& ring, const fs::path& src,
                                                                  const fs::path& dst) {
    auto in = Fd::open(src, O_RDONLY | O_CLOEXEC);
    if (!in) {
        return unexpected(in.error());
    }
    auto out = Fd::open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (!out) {
        return unexpected(out.error());
    }
    struct stat st{};
    if (::fstat(in->get(), &st) != 0) {
        return unexpected(errno_ec());
    }
    const auto size = static_cast<uint64_t>(st.st_size);

    std::array<std::vector<char>, kPairs> bufs;
    for (auto& b : bufs) {
        b.resize(kChunk);
    }

    uint64_t off = 0;
    while (off < size) {
        std::array<unsigned, kPairs> lens{};
        unsigned pairs = 0;
        while (pairs < kPairs && off < size) {
            const auto len = static_cast<unsigned>(std::min<uint64_t>(kChunk, size - off));
            lens[pairs] = len;
            io_uring_sqe* rd = ring.try_get_sqe();
            io_uring_sqe* wr = rd != nullptr ? ring.try_get_sqe() : nullptr;
            if (rd == nullptr || wr == nullptr) {
                return unexpected(std::make_error_code(std::errc::no_buffer_space));
            }
            // READ this chunk into the pair's buffer...
            rd->opcode = IORING_OP_READ;
            rd->fd = in->get();
            rd->addr = reinterpret_cast<uint64_t>(bufs[pairs].data());
            rd->len = len;
            rd->off = off;
            rd->flags = IOSQE_IO_LINK;  // ...then, only if it fully succeeded,
            rd->user_data = uint64_t{pairs} << 1U;
            // ...WRITE the same buffer at the same offset in dst.
            wr->opcode = IORING_OP_WRITE;
            wr->fd = out->get();
            wr->addr = reinterpret_cast<uint64_t>(bufs[pairs].data());
            wr->len = len;
            wr->off = off;
            wr->user_data = (uint64_t{pairs} << 1U) | 1U;
            off += len;
            ++pairs;
        }
        if (auto s = ring.submit_and_wait(2 * pairs); !s) {
            return unexpected(s.error());
        }
        bool short_io = false;
        auto d = ring.drain(2 * pairs, [&](const io_uring_cqe& cqe) {
            const auto slot = static_cast<unsigned>(cqe.user_data >> 1U);
            if (cqe.res < 0 || static_cast<unsigned>(cqe.res) != lens[slot]) {
                short_io = true;
            }
        });
        if (!d) {
            return unexpected(d.error());
        }
        if (short_io) {
            return unexpected(std::make_error_code(std::errc::io_error));
        }
    }
    return size;
}
```

```go
func cmdSync(src, dst, engine string) int {
	if engine == "uring" {
		fmt.Fprintln(os.Stderr, "engine=uring: unsupported in Go (see chapter 10)")
		return 64
	}
	start := time.Now()
	if err := os.MkdirAll(dst, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %v\n", err)
		return 1
	}
	var files, bytes uint64
	err := walk(src, func(path string, d fs.DirEntry) error {
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return fmt.Errorf("rel %s: %w", path, err)
		}
		target := filepath.Join(dst, rel)
		switch {
		case d.IsDir():
			return os.MkdirAll(target, 0o755)
		case d.Type().IsRegular():
			n, err := copyFileRW(path, target)
			if err != nil {
				return err
			}
			files++
			bytes += n
		}
		return nil
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "fwatch: %v\n", err)
		return 1
	}
	ms := time.Since(start).Milliseconds()
	fmt.Printf("synced %d files %d bytes engine=%s ms=%d\n", files, bytes, engine, ms)
	return 0
}
```

```rust
/// io_uring copy: batches of linked READ->WRITE pairs at explicit offsets.
fn copy_file_uring(ring: &mut IoUring, src: &Path, dst: &Path) -> Result<u64> {
    let input = File::open(src).with_context(|| format!("open {}", src.display()))?;
    let output = File::create(dst).with_context(|| format!("create {}", dst.display()))?;
    let size = input.metadata()?.len();

    let mut bufs = vec![vec![0u8; CHUNK as usize]; PAIRS];
    let mut off = 0u64;
    while off < size {
        let mut lens = [0u32; PAIRS];
        let mut pairs = 0usize;
        {
            let mut sq = ring.submission();
            while pairs < PAIRS && off < size {
                let len = CHUNK.min(size - off) as u32;
                lens[pairs] = len;
                let buf = bufs[pairs].as_mut_ptr();
                // READ this chunk into the pair's buffer, then — only if it
                // fully succeeded (IO_LINK) — WRITE it at the same offset.
                let read = opcode::Read::new(types::Fd(input.as_raw_fd()), buf, len)
                    .offset(off)
                    .build()
                    .flags(squeue::Flags::IO_LINK)
                    .user_data((pairs as u64) << 1);
                let write = opcode::Write::new(types::Fd(output.as_raw_fd()), buf, len)
                    .offset(off)
                    .build()
                    .user_data(((pairs as u64) << 1) | 1);
                // SAFETY: the buffers and fds outlive the submissions; we wait
                // for all completions before the next loop iteration.
                unsafe {
                    sq.push(&read).map_err(|_| anyhow!("submission queue full"))?;
                    sq.push(&write).map_err(|_| anyhow!("submission queue full"))?;
                }
                off += u64::from(len);
                pairs += 1;
            }
        } // drop the SQ view: syncs the tail before entering the kernel
        ring.submit_and_wait(2 * pairs)
            .context("io_uring_enter")?;
        let mut seen = 0usize;
        while seen < 2 * pairs {
            for cqe in ring.completion() {
                let slot = (cqe.user_data() >> 1) as usize;
                if cqe.result() < 0 || cqe.result() as u32 != lens[slot] {
                    bail!(
                        "io_uring short op on {}: got {} want {}",
                        src.display(),
                        cqe.result(),
                        lens[slot]
                    );
                }
                seen += 1;
            }
            if seen < 2 * pairs {
                ring.submit_and_wait(2 * pairs - seen).context("io_uring_enter")?;
            }
        }
    }
    Ok(size)
}
```

The design is identical in C++ and Rust: for each 128 KiB chunk, queue a
READ into a per-slot buffer and a WRITE of that same buffer at the same
destination offset, and chain them with `IOSQE_IO_LINK` — the kernel starts
the WRITE only after its READ fully succeeded, so ordering *within a pair*
needs no round-trip to userspace. Up to eight pairs (sixteen SQEs, which is
why the ring is sized 64) go into one `submit_and_wait(2 * pairs)`. Every
operation carries an explicit `off` — completions can arrive in any order,
so nothing may depend on a shared file position; `user_data` encodes the
slot (and read/write bit) so each CQE can be checked against the length that
was requested. `drain` then insists on *exactly* `2 * pairs` CQEs,
re-entering the kernel if a wait returned early — a wait can be satisfied
before every linked pair has retired. Rust's version is the same shape on
the `io-uring` crate, which owns the setup and barriers (dropping the
`submission()` view is the tail-publish); the `unsafe` blocks mark the one
contract the compiler cannot check — the kernel holds raw pointers into
`bufs` until we have drained the completions. The fragile bits are
deliberate and visible: `kChunk`/`kPairs` are hardcoded tuning knobs, the
batch waits for *all* its completions before starting the next (no
cross-file pipelining), and the buffers are plain allocations, not
registered buffers — all three are exactly where a production engine would
diverge, and the numbers below show the cost.

### Errors, three ways

The completion model moves error handling: failures are no longer a `-1` and
an ambient `errno` at the call site, but a negative errno *value* inside
each CQE, delivered whenever the kernel got around to that operation. The
C++ engine folds every CQE through one check — `cqe.res < 0 ||
cqe.res != lens[slot]` — treating short transfers as errors, and returns
`std::errc::io_error` through the same `expected<…, error_code>` channel as
`open` failures; a full SQ ring is `std::errc::no_buffer_space`, and only
`io_uring_enter` itself can still fail the old way (its `EINTR` is retried
in `Ring::enter`). Rust does the same per-CQE audit but `bail!`s with the
got/want lengths, so the error message carries the evidence. And a linked
pair fails as a unit: if a READ comes up short, the kernel cancels its
linked WRITE with `-ECANCELED` — one more CQE, one more `res < 0`, caught by
the same check. Go's error story here is the bluntest and the most
interesting: `exit 64`, refusing at the front door, because the runtime's
contract cannot express the engine at all.

### Concurrency lens

Strip away the I/O and Figure 10.1 is a pair of single-producer,
single-consumer lock-free ring buffers — the kernel is simply *the other
thread*. That is why `try_get_sqe`/`submit_and_wait` are written in
acquire/release vocabulary: the store-release of the SQ tail publishes the
filled SQEs, the kernel's load-acquire observes them, and the mirror-image
pair guards the CQ ring in the other direction (`pop` load-acquires the CQ
tail before reading a CQE). `std::atomic_ref` is doing real work here — the
ring indices are plain `unsigned` fields in shared memory laid out by the
kernel, and `atomic_ref` lets us perform atomic operations on memory we do
not get to declare `std::atomic`. Meanwhile the kernel side is concurrent
too: `IORING_FEAT_NATIVE_WORKERS` in our trace is the io-wq worker-thread
pool, which executes any submitted operation that cannot complete
immediately — our buffered file writes among them, a fact that returns in
the timings. Chapter 26 rebuilds this exact structure in user space, barrier
by barrier.

## Build, run, observe

```bash
[host]$ cd examples/10-io-uring && ./demo.sh build
[host]$ ./demo.sh cpp run sync <srcdir> <dstdir> --engine uring
```

For real numbers, this session generated a scratch tree — 403 files,
19,507,360 bytes: 400 small files of 4–64 KiB across eight directories,
plus three larger files (about 1 MiB, 3 MiB, and 1.1 MiB) sized to straddle
the 128 KiB chunk and the 8-pair batch. Three runs per engine, warm page
cache, `ms=` as printed by the tool:

| engine | C++ | Rust | Go |
|---|---|---|---|
| `rw` | 11, 12, 11 ms | 13, 12, 12 ms | 21 ms |
| `uring` | 129, 137, 130 ms | 105, 107, 107 ms | exit 64 (by design) |

Read that plainly: on this workload our uring engine is roughly **ten times
slower**. Nothing is broken — the copies are byte-identical (checked below)
— the model just costs more than it saves here. Every file is page-cache
hot, so each `read(2)` in the rw loop is a short memcpy; the syscalls we so
cleverly eliminated were never the bottleneck. The uring path, meanwhile,
pays for what buffered file I/O cannot do asynchronously inline — the
kernel punts those operations to io-wq worker threads, buying context
switches and wakeups per batch — and our engine waits for each batch to
fully retire before building the next, with unregistered buffers reallocated
per file. This is the batch-size lesson the chapter owes you: completion
wins when queues are deep, operations are genuinely asynchronous, and fixed
costs get amortized across real waiting — not when the "I/O" is a memcpy
that was already cheaper than the bookkeeping. On the small verify fixture
(5 files, 1,348,634 bytes) the gap shrinks to noise: `rw` printed `ms=0`,
uring `ms=2`–`3`.

## Cross-check: the syscall collapse, counted

Timing said uring lost; `strace -c` proves it did so while *keeping its
promise* — the promise was fewer syscalls, not faster memcpys. Same tree,
C++ binary, both engines:

```bash
[host]$ strace -c ./cpp/build/release/app sync <tree>/src <tree>/dst --engine rw
```

The rw run: **850 `read`**, **444 `write`**, 820 `openat` — 3,467 calls
total. The uring run: **407 `io_uring_enter`**, 1 `io_uring_setup`, and the
only remaining `read`/`write` calls (4/1) belong to the dynamic loader. The
arithmetic closes exactly: 400 single-batch small files, plus 2 + 3 + 2
batches for the three large files (9, 24, and 10 chunks at 8 pairs per
batch) = 407 enters carrying all 19.5 MB. On the 5-file fixture the same
count is 5 enters — one per small file, two for the 9-chunk `d.bin`, zero
for the empty file. The uring table also shows 3,216 `brk` calls — the
allocator churn of building eight 128 KiB buffers per file — a bonus lesson
in how `strace -c` catches *your* overhead, not just the kernel's.

The setup trace shows the anatomy of Figure 10.1 verbatim
(`strace -e trace=io_uring_setup,io_uring_enter,mmap`, trimmed):

```
io_uring_setup(64, {flags=0, sq_entries=64, cq_entries=128,
    features=IORING_FEAT_SINGLE_MMAP|IORING_FEAT_NODROP|...|IORING_FEAT_NO_IOWAIT,
    sq_off={head=0, tail=4, ...}, cq_off={head=8, tail=12, ...}}) = 3
mmap(NULL, 2368, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, 3, 0) = 0x7f508be1b000
mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, 3, 0x10000000) = 0x7f508be1a000
```

One mapping at offset 0 holds both rings (`SINGLE_MMAP`); the second, at
`IORING_OFF_SQES` (0x10000000), is the SQE array. Correctness got its own
independent check: `diff -r` on the uring-copied tree reported no
differences, and `cmp` on the copied `d.bin` (1,048,581 bytes, the
batch-boundary-straddling file) was byte-identical. Finally, the security
claim above is a one-liner you can run yourself: `grep -c io_uring
/usr/share/containers/seccomp.json` prints `0` on this host, against a
`defaultAction` of `SCMP_ACT_ERRNO`.

> **On the lab VM** — <span class="status status--unverified">unverified</span>:
> the natural next probe is watching io-wq from the inside —
> `bpftrace -e 'tracepoint:io_uring:io_uring_submit_req { @[comm] = count(); }'`
> while the two engines run. bcc/bpftrace are not part of this host's
> toolchain by design; chapter 30 (Debugging part) builds that eBPF
> observation kit on the lab VM and exercises exactly this.

## What you learned

- Readiness and completion are the two I/O mental models: epoll hints that
  an fd *may* be usable; io_uring reports that an operation *finished*, with
  its result in the CQE — and the SQ/SQE/CQ structures live in memory shared
  via three well-known mmaps of the ring fd.
- One `io_uring_enter` can submit and reap a whole batch — 407 enters
  replaced 1,294 data syscalls on our 403-file tree — and `IOSQE_IO_LINK`
  expresses intra-pair ordering without a userspace round-trip.
- Fewer syscalls is not faster by itself: against a warm page cache our
  uring engine ran ~10× slower than the rw loop (129 vs 11 ms), because
  buffered-file completions ride io-wq worker threads and our batches are
  small and fully synchronous — completion earns its keep at depth, on real
  waiting.
- io_uring's power is why container seccomp profiles deny it by default
  (podman's allowlist has no io_uring entries on this host — chapter 34),
  and why Go declines to expose it at all: the netpoller already owns the
  event loop (golang/go#31908 stays on hold).

Next, Part 3 opens with the **process lifecycle** — `fork`, `exec`, and
`wait` — and the first cut of `pmon`, the process supervisor that stays with
us to the capstone.

---

<p><span class="status status--verified">verified</span> — all numbers above are from this session's runs on the Fedora 44 host (kernel 7.1.3-200.fc44): the runner table passed 3/3 (cpp, go, rust); the 403-file/19,507,360-byte tree synced with <code>engine=rw</code> in 11–12 ms (C++) and 12–13 ms (Rust) versus 129–137 ms and 105–107 ms with <code>engine=uring</code>; <code>strace -c</code> counted 850 read + 444 write (rw) against 407 io_uring_enter + 1 io_uring_setup (uring, loader-only read/write 4/1); the fixture uring sync used 5 enters and printed <code>synced 5 files 1348634 bytes engine=uring ms=3</code>; the io_uring_setup/mmap trace, the <code>diff -r</code>/<code>cmp</code> byte-equality, the Go refusal (<code>engine=uring: unsupported in Go (see chapter 10)</code>, exit 64), and the empty <code>grep -c io_uring</code> against podman's seccomp profile were all reproduced as quoted. Unverified: the bpftrace probe in the lab-VM callout, as marked.</p>
