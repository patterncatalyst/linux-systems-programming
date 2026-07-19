---
title: "The page cache and durability"
order: 8
part: "Files and I/O"
description: "What write(2) actually promises — and what durability costs: dirty pages and writeback, fsync vs fdatasync vs sync_file_range, O_DIRECT, and the atomic-rename pattern, all measured with one tri-language benchmark on real hardware."
duration: 45 minutes
---

Chapter 7 ended with a file descriptor safely wrapped in RAII and a byte
successfully written through the VFS. This chapter is about the uncomfortable
question that follows: written *where*? A successful `write(2)` puts your
bytes in kernel memory — the **page cache** — and nothing more; if the power
fails a second later, they are gone, and no error will ever tell you so. The
gap between "the syscall returned 0 errors" and "the bytes are on the device"
is where databases earn their reputations, and this chapter measures that gap
with `iobench`, a small write benchmark implemented identically in C++23, Go,
and Rust. On this host the same 64 MiB costs 21 ms to "write" and 138 ms to
write *durably* — and by the end you will have watched the difference happen
in `/proc/meminfo`, `/proc/<pid>/io`, and an `strace` syscall table.

The code is in `examples/08-page-cache-and-durability/`. `./demo.sh` there
builds and runs all three implementations; its `README.md` covers the CLI,
output format, and exit codes shared by all three.

{% include excalidraw.html
   file="08-write-path-page-cache"
   alt="Three columns over user space, kernel, and storage bands. In buffered mode, write(2) copies into the page cache and data reaches the NVMe device either by later asynchronous writeback or by an fdatasync at the end; in fsync-every mode an fdatasync every N blocks forces it down; in direct mode an O_DIRECT write bypasses the page cache and DMAs straight from the user buffer to the device."
   caption="Figure 8.1 — one write path, three flush points: where each iobench mode hands bytes to the device" %}

> **Tools used** — `strace`, `stat`, `grep`, `python3` (host);
> `cachestat`/`biolatency` (lab VM, exercised in Part 8). Everything here is
> checked by `scripts/check-host.sh`, ships with Fedora, or is preinstalled in
> the lab VMs.

## What write(2) actually promises

The contract of a successful buffered `write(2)` is precise and smaller than
intuition suggests: the kernel has copied your bytes into page-cache pages
belonging to the file, marked those pages **dirty**, and made the data
visible to every subsequent `read(2)` from any process. That is all.
Durability is explicitly not included — the manpage says so — and the return
value is really just confirming a memcpy into kernel memory. That is why
`iobench --mode buffered` reports 3,013 MiB/s on this host's NVMe drive: the
timed loop never touches the device. You are benchmarking `memcpy` plus
bookkeeping.

The cache earns its keep in both directions. On reads, the kernel notices
sequential access and issues **readahead** — it fetches pages you have not
asked for yet, so a sequential reader almost never blocks on the device
(`posix_fadvise(2)` lets you tune or defeat this; `iobench` is write-only, so
readahead sits out this chapter's measurements). On writes, dirty pages
decouple your process from device latency entirely — until something forces
the issue.

Dirty pages do not accumulate forever. Kernel **writeback** (flusher) threads
write them to the device in the background, driven by two kinds of pressure:
age (`vm.dirty_expire_centisecs`, 30 s by default — no page stays dirty
longer) and volume (`vm.dirty_background_ratio` starts background writeback;
at `vm.dirty_ratio` the kernel begins throttling the writers themselves, and
your "memcpy-speed" `write(2)` suddenly runs at device speed). The systemwide
total is one number: `Dirty:` in `/proc/meminfo`, which the cross-check
section watches balloon by a gigabyte and collapse again.

## Choosing your flush point: fsync, fdatasync, sync_file_range

When you need the bytes on the device *now*, you pick one of three calls, in
decreasing order of guarantee:

- **`fsync(2)`** — writes the file's dirty pages *and* its metadata (size,
  timestamps, block allocations) to the device, and asks the device to flush
  its own volatile write cache. When it returns, the data survives power
  loss.
- **`fdatasync(2)`** — the same, minus metadata that is not needed to read
  the data back. An mtime update does not gate your bytes, so skipping that
  metadata write can save a device round trip. A changed file *size* is
  data-critical metadata and is still flushed. `iobench` uses `fdatasync`
  throughout, and the append-heavy write pattern of a database WAL is
  exactly the case it was designed for.
- **`sync_file_range(2)`** — the expert-mode footgun: it can start or wait on
  writeback for a byte range, but it never writes metadata and never issues a
  device cache flush, so it is **not** a durability primitive. Its real use
  is pacing — nudging writeback along so a later `fdatasync` has less to do —
  and its manpage warning ("extremely dangerous" as a durability mechanism)
  is one of the better reads in section 2.

`iobench` turns the choice into a flag. `--mode buffered` times the bare
`write(2)` loop, then pays for a single closing `fdatasync` on a separate
`fsync_ms=` line, so the price of durability is printed as its own number.
`--mode fsync-every` moves `fdatasync` *inside* the timed loop, every N
blocks — durable-as-you-go, the way a database commits.

## O_DIRECT, and getting renames to keep promises

The third mode opts out of the cache entirely. Opening with **`O_DIRECT`**
makes each `write(2)` DMA straight from your user buffer to the device —
no copy into the page cache, no dirty pages, no writeback. The price is a
contract: buffer address, file offset, and transfer length must all be
aligned to the device's logical block size (4,096 bytes covers current
hardware), and a filesystem may simply refuse the flag at `open(2)` with
`EINVAL` — which `iobench` treats as a documented outcome (exit 4), because
it genuinely varies by filesystem.

Why do databases insist on it? Because they already maintain their own buffer
pool, sized and evicted with knowledge of the workload that the kernel cannot
have; caching every page twice wastes half the RAM. And because a database
needs to *know* when bytes are on disk to order its writes (WAL before data
page), a cache that says "done" when nothing happened is a liability. Note
what `O_DIRECT` does **not** give you: the device's own write cache is still
in play, so even direct I/O needs `fdatasync` (or `O_DSYNC`) for real
durability.

The last durability tool is not a syscall but a pattern. To replace a config
file so that readers see either the old version or the new one — never a
torn half — you use **atomic rename**: write the new content to a temporary
file in the *same directory*, `fsync` the temp file, `rename(2)` it over the
target (rename within a filesystem is atomic), then `fsync` the *directory*
fd so the rename itself — a directory entry, i.e. metadata living in the
directory, as chapter 7's inode walk showed — is on disk too. Skip step 2 and
a crash can leave you a correctly named empty file; skip step 4 and the old
name can come back after reboot. `iobench` deliberately does not do this — a
benchmark should overwrite in place — but every "save settings" feature you
ship should.

## How the code works

All three implementations share one CLI, one output format, and one
structure: parse arguments, open with mode-appropriate flags, write M MiB in
64 KiB blocks with mode-appropriate syncing, report. The heart is the `run`
function, shown here verbatim from each language:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
[[nodiscard]] int run(const Options& opt) {
    auto fd = open_output(opt.file, opt.mode == Mode::direct);
    if (!fd) {
        if (opt.mode == Mode::direct && fd.error() == std::errc::invalid_argument) {
            std::println(stderr, "direct: unsupported on this filesystem");
            return 4;
        }
        std::println(stderr, "error: open {}: {}", opt.file, fd.error().message());
        return 1;
    }

    const auto block = make_block();
    if (!block) {
        std::println(stderr, "error: aligned_alloc failed");
        return 1;
    }
    const std::span<const std::byte> data{block.get(), kBlockSize};

    const std::size_t nblocks = opt.size_mb * (kMiB / kBlockSize);
    const auto bytes = static_cast<std::uint64_t>(opt.size_mb) * kMiB;

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < nblocks; ++i) {
        if (const auto w = write_all(*fd, data); !w) {
            std::println(stderr, "error: write {}: {}", opt.file, w.error().message());
            return 1;
        }
        if (opt.mode == Mode::fsync_every && (i + 1) % opt.every == 0) {
            if (const auto s = datasync(*fd); !s) {
                std::println(stderr, "error: fdatasync {}: {}", opt.file, s.error().message());
                return 1;
            }
        }
    }
    if (opt.mode == Mode::fsync_every && nblocks % opt.every != 0) {
        if (const auto s = datasync(*fd); !s) {
            std::println(stderr, "error: fdatasync {}: {}", opt.file, s.error().message());
            return 1;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    const std::int64_t ns = elapsed_ns(t0, t1);
    const double mib_per_s =
        (static_cast<double>(bytes) / static_cast<double>(kMiB)) / (static_cast<double>(ns) / 1e9);
    std::println("mode={} bytes={} ms={} MiB/s={:.1f}", mode_name(opt.mode), bytes,
                 ns / 1'000'000, mib_per_s);

    if (opt.mode == Mode::buffered) {
        const auto t2 = std::chrono::steady_clock::now();
        if (const auto s = datasync(*fd); !s) {
            std::println(stderr, "error: fdatasync {}: {}", opt.file, s.error().message());
            return 1;
        }
        const auto t3 = std::chrono::steady_clock::now();
        std::println("fsync_ms={}", elapsed_ns(t2, t3) / 1'000'000);
    }
    return 0;
}
```

```go
func run(opt options) (int, error) {
	flags := unix.O_WRONLY | unix.O_CREAT | unix.O_TRUNC
	if opt.mode == "direct" {
		flags |= unix.O_DIRECT
	}
	fd, err := unix.Open(opt.file, flags, 0o644)
	if err != nil {
		if opt.mode == "direct" && errors.Is(err, unix.EINVAL) {
			fmt.Fprintln(os.Stderr, "direct: unsupported on this filesystem")
			return 4, nil
		}
		return 1, fmt.Errorf("open %s: %w", opt.file, err)
	}
	defer unix.Close(fd)

	block := alignedBlock()
	nblocks := opt.sizeMB * (mib / blockSize)
	bytes := opt.sizeMB * mib

	t0 := time.Now()
	for i := uint64(0); i < nblocks; i++ {
		if err := writeAll(fd, block); err != nil {
			return 1, fmt.Errorf("%s: %w", opt.file, err)
		}
		if opt.mode == "fsync-every" && (i+1)%opt.every == 0 {
			if err := datasync(fd); err != nil {
				return 1, fmt.Errorf("%s: %w", opt.file, err)
			}
		}
	}
	if opt.mode == "fsync-every" && nblocks%opt.every != 0 {
		if err := datasync(fd); err != nil {
			return 1, fmt.Errorf("%s: %w", opt.file, err)
		}
	}
	ns := clampNS(time.Since(t0))

	mibPerS := (float64(bytes) / float64(mib)) / (float64(ns) / 1e9)
	fmt.Printf("mode=%s bytes=%d ms=%d MiB/s=%.1f\n", opt.mode, bytes, ns/1e6, mibPerS)

	if opt.mode == "buffered" {
		t2 := time.Now()
		if err := datasync(fd); err != nil {
			return 1, fmt.Errorf("%s: %w", opt.file, err)
		}
		fmt.Printf("fsync_ms=%d\n", clampNS(time.Since(t2))/1e6)
	}
	return 0, nil
}
```

```rust
fn run(opt: &Options) -> Result<ExitCode> {
    let fd = match open_output(&opt.file, opt.mode == SyncMode::Direct) {
        Ok(fd) => fd,
        Err(Errno::INVAL) if opt.mode == SyncMode::Direct => {
            eprintln!("direct: unsupported on this filesystem");
            return Ok(ExitCode::from(4));
        }
        Err(e) => return Err(e).context(format!("open {}", opt.file)),
    };

    let block = AlignedBlock::new();
    let nblocks = opt.size_mb * (MIB / BLOCK_SIZE as u64);
    let bytes = opt.size_mb * MIB;

    let t0 = Instant::now();
    for i in 0..nblocks {
        write_all(&fd, block.as_slice()).with_context(|| format!("write {}", opt.file))?;
        if opt.mode == SyncMode::FsyncEvery && (i + 1) % opt.every == 0 {
            fdatasync(&fd).with_context(|| format!("fdatasync {}", opt.file))?;
        }
    }
    if opt.mode == SyncMode::FsyncEvery && nblocks % opt.every != 0 {
        fdatasync(&fd).with_context(|| format!("fdatasync {}", opt.file))?;
    }
    let ns = elapsed_ns(t0);

    let mib_per_s = (bytes as f64 / MIB as f64) / (ns as f64 / 1e9);
    println!(
        "mode={} bytes={} ms={} MiB/s={:.1}",
        opt.mode.name(),
        bytes,
        ns / 1_000_000,
        mib_per_s
    );

    if opt.mode == SyncMode::Buffered {
        let t2 = Instant::now();
        fdatasync(&fd).with_context(|| format!("fdatasync {}", opt.file))?;
        println!("fsync_ms={}", elapsed_ns(t2) / 1_000_000);
    }
    Ok(ExitCode::SUCCESS)
}
```

Walk the shared skeleton. **The open** adds `O_DIRECT` to
`O_WRONLY | O_CREAT | O_TRUNC` only in direct mode, and only the direct-mode
`EINVAL` is treated as the "unsupported filesystem" outcome — an `EINVAL`
from a buffered open would be a real bug and falls through to the generic
error path. **The block** is built once and rewritten every iteration: 64 KiB
of `i & 0xFF` bytes at a 4,096-aligned address. C++ gets alignment from
`std::aligned_alloc` (freed via a `unique_ptr` with a `FreeDeleter`); Go and
Rust over-allocate by one alignment unit and slice to the first aligned
offset inside — Go's comment notes the heap does not move allocations, and
Rust keeps the backing `Vec` alive in a struct so the aligned range stays
valid. The alignment is mandatory for `O_DIRECT` and harmless otherwise,
which is why all three modes share one buffer.

**The write** is never a bare `write(2)`. Each language has a `write_all`
that loops: a short write advances the slice/span by the returned count, and
`EINTR` — the call interrupted by a signal before writing anything — retries.
A 64 KiB write nearly always completes in one call, but "nearly always" is
exactly the assumption this book keeps telling you not to compile in.
**The sync placement** is the whole experiment: fsync-every calls `fdatasync`
inside the timed region every `--every` blocks (plus a trailing sync when the
block count is not a multiple of N — drop that and up to N−1 blocks quietly
escape the durability guarantee being measured), while buffered mode stops
the clock, prints the report line, and only then times its single `fdatasync`
as `fsync_ms=`. Elapsed time is clamped to at least 1 ns before the
throughput division — on tmpfs a tiny run genuinely can round to 0 ms.

### Errors, three ways

Chapter 5's policy gets its first workout against a syscall that fails in
interesting ways. The key move in all three languages is *classify before
you generalize*: the open error is inspected for the one value with a
documented meaning (`EINVAL` in direct mode → message + exit 4) before being
wrapped into the general error path (exit 1). C++ compares the
`std::error_code` against `std::errc::invalid_argument`, keeping the
comparison portable rather than testing raw `errno`. Go wraps with `%w` and
tests with `errors.Is(err, unix.EINVAL)`, so classification still works
through layers of context. Rust matches `Err(Errno::INVAL)` *before*
converting into `anyhow::Error` — the guard sits in the `match` because after
`.context()` the typed errno is boxed away and would need a downcast. Usage
errors are a separate exit code (2) diagnosed entirely in `parse_args`,
before any file is created — `verify.lua` checks that a bad `--mode` leaves
nothing on disk.

### Concurrency lens

`iobench` is single-threaded on purpose, but the system it measures is not.
The moment the write loop starts dirtying pages, kernel flusher threads wake
and begin writing them back *concurrently with the loop* — in the cross-check
below, a 2 GiB write peaks at only ~1.1 GiB dirty because writeback was
draining the pool while the program filled it. The final `fdatasync` is
therefore a *rendezvous*, not the whole transfer: it waits for whatever
writeback has not already covered. Language runtimes add one more wrinkle
from chapter 6: a blocking `fdatasync` parks the calling OS thread, so in Go
the runtime hands the P to another thread and other goroutines keep running —
one reason "just fsync in a goroutine" feels free until you count threads.
And timing uses `steady_clock` / `time.Now` monotonic readings /
`Instant` — wall-clock time can step backwards; monotonic clocks are the only
thing you should ever subtract.

## Build, run, observe

```bash
[host]$ cd examples/08-page-cache-and-durability
[host]$ ./demo.sh cpp run --mode buffered    bench.bin
[host]$ ./demo.sh cpp run --mode fsync-every bench.bin
[host]$ ./demo.sh cpp run --mode direct      bench.bin
```

On this host (btrfs on NVMe, 64 MiB default, C++ build) the three modes
printed:

```
mode=buffered bytes=67108864 ms=21 MiB/s=3013.0
fsync_ms=17
mode=fsync-every bytes=67108864 ms=138 MiB/s=462.8
mode=direct bytes=67108864 ms=81 MiB/s=785.2
```

Read the buffered pair together: 21 ms to "write" 64 MiB, then 17 ms of
`fdatasync` — durability nearly doubles the end-to-end time, and the 3,013
MiB/s headline was never a device number. Syncing every 8 blocks (512 KiB)
drops throughput 6.5× to 462.8 MiB/s because the device's flush latency now
sits inside the loop 128 times. `O_DIRECT` lands between them at 785.2 MiB/s:
every byte really moved to the device inside the timed region, with no
copy and no cache. Go and Rust agree with the C++ numbers (buffered 3,283.9
and 3,483.6 MiB/s with `fsync_ms=17` and `14` on the same runs) — the page
cache does not care which language dirtied it. After any run,
`stat -c %s bench.bin` reports exactly 67108864: same bytes, very different
guarantees.

## Cross-check: watching the bytes move

The claims above are checkable from the outside. First, the syscall pattern:
`strace -c` on a 4 MiB fsync-every run with `--every 4` should show 64 data
writes (4 MiB / 64 KiB) and 16 `fdatasync`s (64 / 4). It does — the 65th
write is the report line to stdout:

```
[host]$ strace -c -e trace=write,fdatasync,openat ./cpp/build/release/app --mode fsync-every --every 4 --size-mb 4 bench.bin
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
 54.29    0.001589          99        16           fdatasync
 45.71    0.001338          20        65           write
  0.00    0.000000           0         6           openat
```

Second, the dirty-page story. Sampling `Dirty:` from `/proc/meminfo` before,
during, and after a 2 GiB buffered run on this host:

```
Dirty:              8684 kB     # before
Dirty:           1118476 kB     # mid-run, during the write loop
Dirty:             22520 kB     # after exit — fdatasync drained it
```

A gigabyte of "written" data sat in RAM mid-run (and only ~1.1 GiB of the 2
GiB was dirty at the sample — concurrent writeback was already draining), and
the closing `fdatasync` — 424 ms on that run, against 661 ms for the whole 2
GiB write loop — put the rest on the device. Third, the per-process view in
`/proc/<pid>/io` during that same 2 GiB run:

```
[host]$ grep -E '^(wchar|write_bytes)' /proc/<pid>/io    # mid write loop
wchar: 540213256
write_bytes: 540286976
[host]$ grep -E '^(wchar|write_bytes)' /proc/<pid>/io    # during the final fdatasync
wchar: 2147483656
write_bytes: 2147500032
```

`wchar` counts bytes handed to `write(2)`; `write_bytes` counts bytes the
task caused to be destined for the storage layer, charged at page-dirtying
time and in page-sized units (hence it running a few KiB ahead). The second
sample is the chapter's thesis in two lines: all 2 GiB of `write(2)` calls
had returned while the process was still alive *waiting inside `fdatasync`*.
Finally, the same binary on tmpfs (this book's scratchpad) shows why none of
the magnitudes are portable: buffered 3,547.2 MiB/s with `fsync_ms=0`,
fsync-every 5,113.2, direct 5,304.7 — with no device behind the file,
`fdatasync` is nearly free, `O_DIRECT` is accepted but bypasses nothing, and
the three modes converge. That is precisely why `verify.lua` asserts output
shape and exact file sizes, never throughput.

<p><span class="status status--unverified">unverified</span> — <strong>On the
lab VM:</strong> the bcc-tools pair <code>cachestat</code> (page-cache
hit/miss counts while iobench runs) and <code>biolatency</code> (the block
I/O latency histogram that separates fsync-every's many small flushes from
direct mode's steady stream) would show the kernel side of these runs.
They need root and a kernel with BPF tooling, so they are not run on this
host; chapter 30 (Debugging part) exercises them on the lab VM.</p>

## What you learned

- A successful `write(2)` means "copied into the page cache", nothing more;
  dirty pages reach the device via writeback (age and pressure driven) or
  when you say so — and on this host that "so" cost 17 ms atop a 21 ms
  64 MiB write.
- `fsync` flushes data + metadata, `fdatasync` skips metadata that does not
  gate reading the data back, and `sync_file_range` is writeback pacing, not
  durability.
- `O_DIRECT` trades the cache for alignment rules and an `EINVAL`-at-open
  probe — and databases take that trade to own their caching and write
  ordering; crash-safe file replacement is tmpfile → `fsync` → `rename` →
  `fsync(dir)`.
- `strace -c`, `/proc/meminfo`'s `Dirty:`, and `/proc/<pid>/io` let you
  audit any program's durability behavior from outside — 2 GiB of `wchar`
  with the process still parked in `fdatasync` is the promise gap made
  visible.

Next, **event loops**: `epoll` and `inotify`, where the question stops being
"is the byte durable" and becomes "which of ten thousand fds is ready".

---

<p><span class="status status--verified">verified</span> — all numbers above
are from runs performed on the reference host (btrfs on NVMe, Fedora 44,
kernel 7.1.3-200.fc44) while writing this chapter: the mode reports
(<code>ms=21 / fsync_ms=17</code>, <code>ms=138</code>, <code>ms=81</code> at
64 MiB; Go/Rust buffered parity), the <code>strace -c</code> table (16
<code>fdatasync</code>, 65 <code>write</code>), the <code>Dirty:</code>
sequence 8684 → 1118476 → 22520 kB around a 2 GiB buffered run
(<code>ms=661</code>, <code>fsync_ms=424</code>), the
<code>/proc/&lt;pid&gt;/io</code> samples, the tmpfs convergence numbers, and
the runner gate (<code>test-all-examples.py</code>: cpp PASS, go PASS, rust
PASS — 15 behavioral checks per language). The bcc-tools callout above is the
one unverified item, deferred to the lab VM.</p>
