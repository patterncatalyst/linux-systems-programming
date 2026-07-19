---
title: "Errors, three ways"
order: 5
part: "Foundations"
description: "One error's journey from a failing write(2) into std::expected, a wrapped Go error, and a Rust Result — copyx establishes the book's error taxonomy, the strerror(3) message contract, and the EINTR retry policy every later chapter reuses."
duration: "45 minutes"
---

Chapter 4 walked a syscall down to the boundary and back, and ended on the
detail this chapter is built from: when a syscall fails, the kernel does not
throw, log, or explain — it puts a small negative integer in a register and
returns. Everything your language calls an "error" is constructed, in user
space, from that one number. This chapter takes a single program — `copyx`, a
file copier — and writes it three times so you can watch the *same* kernel
failure become a `std::expected`, a wrapped Go `error`, and a Rust `Result`,
and still come out the other side as byte-identical output and the same exit
code. Along the way it fixes the policies the rest of the book never
renegotiates: what EINTR means, what a short write demands, and which side of
a failure owns the exit code.

The code is in `examples/05-errors-three-ways/`. `./demo.sh` there builds all
three implementations and runs a walkthrough of the taxonomy; its `README.md`
states the behavior contract each one must satisfy.

{% include excalidraw.html
   file="05-error-flow-three-ways"
   alt="One failure's journey: write(2) on /dev/full returns -ENOSPC in the kernel; in user space that becomes errno 28, which fans out into three columns — C++23's std::expected with std::error_code mapping into a Failure struct, Go's unix.Errno wrapped into *opError with Unwrap for errors.Is/As, and Rust's Result with Errno mapped via map_err into a thiserror CopyError variant — all three converging on one outcome box: exit 3 and the line copyx: write /dev/full: No space left on device."
   caption="Figure 5.1 — one failure, three spellings, one contract: ENOSPC from write(2) to the user, in each type system" %}

> **Tools used** — `strace`, `head`, `cmp`, `ls`, `python3` (host); `bpftrace`
> (lab VM, exercised in Part 8). Everything here is checked by
> `scripts/check-host.sh`, ships with Fedora, or is preinstalled in the lab VMs.

## errno is an ABI, not a variable

On x86-64 Linux, a failing syscall returns a value between -1 and -4095 in
`rax`; the number is the negated error code. `ENOSPC` is 28 — not "usually
28", but 28 by kernel ABI, stable for decades, the same for every language
and libc on the platform. What differs is the unwrapping. glibc's syscall
stubs check the return, store the positive code into the thread-local
`errno`, and hand C a `-1`. Go's runtime makes the syscall itself and returns
the code as an ordinary value — `unix.Errno` is just `uintptr` with an
`Error()` method; there is no global to race on. Rust's `rustix` does the
same trick: `Errno` is a wrapper around the raw code, delivered in the `Err`
arm of a `Result`, never through ambient state.

That shared integer is why a cross-language error *policy* is even possible.
Whatever type each language dresses the failure in, the bottom of the stack
is the same number with the same `strerror(3)` text — and `copyx` treats that
text as part of its interface. The behavior contract, identical in all three
implementations:

| Outcome | Output | Exit |
| --- | --- | --- |
| Success | stdout: `copied <N> bytes` | 0 |
| Bad usage | stderr: `usage: copyx SRC DST` | 2 |
| Source-side failure (open/read SRC) | stderr: `copyx: <reason>` | 2 |
| Destination-side failure (open/write/close DST) | stderr: `copyx: <reason>` | 3 |

The split is deliberate: a caller (or a shell script) can distinguish "your
input was bad" from "the place you told me to write is broken" without
parsing prose. Exit codes are the oldest structured error channel on Unix,
and `copyx` uses them as one.

Two syscall-level policies are also part of the contract, because they will
be true of every program in this book. First, **EINTR is retried**. A
`read(2)` or `write(2)` interrupted by a signal before transferring anything
fails with `EINTR`; nothing happened, so the only correct response is to
reissue the call — treating it as an error turns every signal into data
corruption at a distance. Second, **short writes are resumed**. `write(2)` is
allowed to accept fewer bytes than you offered; the loop must advance past
what the kernel took and continue. (The third sibling, `EAGAIN`, means "would
block" on a non-blocking descriptor — not an error either, but a scheduling
fact; it becomes central when epoll arrives in the Files and I/O part.) One
more subtlety the contract encodes: the write side must *observe*
`close(2)`, because deferred I/O errors can surface there; the read side's
close result carries nothing actionable and is released by RAII or `defer`.

## How the code works

Each implementation has the same skeleton: a phase-tagged error type, a
retrying `read_some`, a resuming `write_all`, and a `copy_file` that maps
each failure onto the taxonomy. The error types come first, because they are
where the three type systems diverge most visibly. C++
(`examples/05-errors-three-ways/cpp/src/main.cpp`) uses a plain struct —

```cpp
struct Failure {
    std::string message; // printed after the "copyx: " prefix
    int exit_code;
};
```

— because by the time `copy_file` fails, classification is *done*: the
message is rendered and the exit code chosen. Go
(`examples/05-errors-three-ways/go/main.go`) cannot stop there, since Go
errors are expected to stay inspectable:

```go
type opError struct {
	op   string // "read", "write", "close"; "" means open(2)
	path string
	err  unix.Errno // reachable via errors.Is / errors.As through Unwrap
	exit int
}
```

`opError` keeps the raw errno and implements `Unwrap() error`, so
`errors.Is(err, unix.ENOENT)` still answers truthfully through the wrapper.
Rust (`examples/05-errors-three-ways/rust/src/main.rs`) puts the phase into
the *type* itself, one enum variant per failure site, with `thiserror`
generating the `Display` impl from the `#[error("...")]` attributes:

```rust
#[derive(Debug, Error)]
enum CopyError {
    #[error("{path}: {reason}")]
    SrcOpen { path: String, reason: String },
    #[error("read {path}: {reason}")]
    Read { path: String, reason: String },
    #[error("{path}: {reason}")]
    DstOpen { path: String, reason: String },
    #[error("write {path}: {reason}")]
    Write { path: String, reason: String },
    #[error("close {path}: {reason}")]
    Close { path: String, reason: String },
}
```

Now the core loops. The three excerpts below are the real `write_all` and
`copy_file` from each implementation — read one, then diff it mentally
against the next:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// write(2) until the whole span is written: EINTR restarts the call, a short
// write resumes from where the kernel stopped.
[[nodiscard]] std::expected<void, std::error_code>
write_all(const Fd& fd, std::span<const char> buf) {
    while (!buf.empty()) {
        const ssize_t n = ::write(fd.get(), buf.data(), buf.size());
        if (n > 0) {
            buf = buf.subspan(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue; // interrupted: reissue the same span
        }
        return std::unexpected(last_error());
    }
    return {};
}

struct Failure {
    std::string message; // printed after the "copyx: " prefix
    int exit_code;
};

[[nodiscard]] std::expected<std::uint64_t, Failure>
copy_file(const char* src_path, const char* dst_path) {
    auto src = open_fd(src_path, O_RDONLY | O_CLOEXEC);
    if (!src) {
        return std::unexpected(
            Failure{std::format("{}: {}", src_path, src.error().message()), 2});
    }

    auto dst = open_fd(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (!dst) {
        return std::unexpected(
            Failure{std::format("{}: {}", dst_path, dst.error().message()), 3});
    }

    std::uint64_t total = 0;
    std::array<char, 64 * 1024> buf{};
    for (;;) {
        const auto n = read_some(*src, buf);
        if (!n) {
            return std::unexpected(
                Failure{std::format("read {}: {}", src_path, n.error().message()), 2});
        }
        if (*n == 0) {
            break; // EOF
        }
        if (auto w = write_all(*dst, std::span<const char>{buf}.first(*n)); !w) {
            return std::unexpected(
                Failure{std::format("write {}: {}", dst_path, w.error().message()), 3});
        }
        total += *n;
    }

    // The write side must observe close(2): deferred IO errors land here.
    if (auto c = dst->close(); !c) {
        return std::unexpected(
            Failure{std::format("close {}: {}", dst_path, c.error().message()), 3});
    }
    return total;
}
```

```go
// writeAll pushes the whole buffer through write(2): EINTR restarts the
// call, a short write resumes from where the kernel stopped.
func writeAll(fd int, buf []byte) error {
	for len(buf) > 0 {
		n, err := unix.Write(fd, buf)
		if errors.Is(err, unix.EINTR) {
			continue // interrupted: reissue the same span
		}
		if err != nil {
			return err
		}
		buf = buf[n:]
	}
	return nil
}

func copyFile(srcPath, dstPath string) (int64, error) {
	src, err := unix.Open(srcPath, unix.O_RDONLY|unix.O_CLOEXEC, 0)
	if err != nil {
		return 0, newOpError("", srcPath, fmt.Errorf("open %s: %w", srcPath, err), 2)
	}
	defer unix.Close(src) // read side: nothing actionable in the close result

	dst, err := unix.Open(dstPath, unix.O_WRONLY|unix.O_CREAT|unix.O_TRUNC|unix.O_CLOEXEC, 0o644)
	if err != nil {
		return 0, newOpError("", dstPath, fmt.Errorf("open %s: %w", dstPath, err), 3)
	}

	var total int64
	buf := make([]byte, 64*1024)
	for {
		n, err := readSome(src, buf)
		if err != nil {
			unix.Close(dst)
			return 0, newOpError("read", srcPath, err, 2)
		}
		if n == 0 {
			break // EOF
		}
		if err := writeAll(dst, buf[:n]); err != nil {
			unix.Close(dst)
			return 0, newOpError("write", dstPath, err, 3)
		}
		total += int64(n)
	}

	// The write side must observe close(2): deferred IO errors land here.
	if err := unix.Close(dst); err != nil {
		return 0, newOpError("close", dstPath, err, 3)
	}
	return total, nil
}
```

```rust
/// write(2) until the whole slice is written: EINTR restarts the call, a
/// short write resumes from where the kernel stopped.
fn write_all(fd: impl AsFd, mut buf: &[u8]) -> Result<(), Errno> {
    while !buf.is_empty() {
        match write(&fd, buf) {
            Ok(n) => buf = &buf[n..],
            Err(Errno::INTR) => continue, // interrupted: reissue the same span
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// close(2) with the result observed: on the write side this is where
/// deferred IO errors surface. `into_raw_fd` transfers ownership out of the
/// `OwnedFd`, so the fd is closed exactly once.
fn close_checked(fd: OwnedFd) -> Result<(), Errno> {
    unsafe { try_close(fd.into_raw_fd()) }
}

fn copy_file(src_path: &str, dst_path: &str) -> Result<u64, CopyError> {
    let src = open(src_path, OFlags::RDONLY | OFlags::CLOEXEC, Mode::empty()).map_err(|e| {
        CopyError::SrcOpen { path: src_path.to_owned(), reason: strerror(e) }
    })?; // OwnedFd: dropped (and closed) on every path below

    let dst = open(
        dst_path,
        OFlags::WRONLY | OFlags::CREATE | OFlags::TRUNC | OFlags::CLOEXEC,
        Mode::from_bits_truncate(0o644),
    )
    .map_err(|e| CopyError::DstOpen { path: dst_path.to_owned(), reason: strerror(e) })?;

    let mut total: u64 = 0;
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = read_some(&src, &mut buf).map_err(|e| CopyError::Read {
            path: src_path.to_owned(),
            reason: strerror(e),
        })?;
        if n == 0 {
            break; // EOF
        }
        write_all(&dst, &buf[..n]).map_err(|e| CopyError::Write {
            path: dst_path.to_owned(),
            reason: strerror(e),
        })?;
        total += n as u64;
    }

    close_checked(dst).map_err(|e| CopyError::Close {
        path: dst_path.to_owned(),
        reason: strerror(e),
    })?;
    Ok(total)
}
```

The loops are structurally identical — a 64 KiB buffer, read until a zero
return means EOF, `write_all` per chunk, then a checked close — but each
language spends its effort differently. **C++** builds the discipline out of
`std::expected`: every fallible helper returns
`std::expected<T, std::error_code>`, `last_error()` snapshots `errno` into
`std::generic_category()` (the category for POSIX errno values; Chapter 1's
`uname` wrapper used `system_category` — for Linux errno they render the
same, and the book standardizes on `generic_category` from here on), and the
move-only RAII `Fd` guarantees no descriptor outlives its owner. The
asymmetry between `dst->close()` — explicit, `[[nodiscard]]`, result mapped
to exit 3 — and `src` being closed silently by the destructor *is* the close
policy from the contract, written in types. **Go** threads one `error` value
through everything: `readSome` detects EINTR with
`errors.Is(err, unix.EINTR)` rather than `err == unix.EINTR`, so the check
survives any future wrapping; `newOpError` recovers the raw `unix.Errno`
from whatever it is handed using `errors.As`, and `main` gets the exit code
back the same way. Note also what Go must do that the others get free: its
errno strings are lowercase ("no such file or directory"), so `strerror`
capitalizes the first rune to match glibc. **Rust** lets `?` do the
propagation and `map_err` do the classification — each call site names its
variant, so forgetting to classify a failure is a compile error, and
`exit_code()` is an exhaustive `match` the compiler re-checks whenever a
variant is added. Its `strerror` has the inverse problem to Go's: `Errno`
displays through `std::io::Error`, which appends " (os error N)", so the
suffix is stripped to keep the three outputs byte-identical.

### Errors, three ways

`copyx` is the policy statement for the thread that runs through every
chapter, so state the doctrine plainly. In **C++**, errors that are part of
normal operation — a missing file, a full disk — travel as values
(`std::expected`), and exceptions are reserved for *subsystem boundaries*:
the places where a whole component (a parser, a plugin, a request handler)
fails as a unit and someone several frames up owns the recovery. Inside
syscall-adjacent code, exceptions obscure control flow exactly where you need
to see it. In **Go**, the equivalent line is drawn between `error` and
`panic`: an `error` is a result, a `panic` is a bug (index out of range, nil
map write) — `copyx` returns errors for everything the kernel can say and
would panic only on programmer mistakes. In **Rust**, the same line separates
`Result` from `panic!`; the book's examples are written to be correct under
`panic = "abort"`, which means a panic is never part of the error path — it
is a crash you fix. On the library-versus-application split: `thiserror`
(used here) is for code whose *callers* match on error variants; `anyhow` is
for binaries that only report. `copyx` is a binary, but its exit codes make
error *identity* part of the interface, which is exactly the case where the
typed enum earns its boilerplate.

### Concurrency lens

`copyx` is deliberately single-threaded — one loop, one buffer, no shared
state — but the error machinery is already concurrency-load-bearing. C's
`errno` is thread-local precisely so two threads failing simultaneously do
not corrupt each other's error state, and `last_error()` copies it into a
value *immediately* because the very next libc call on the same thread may
overwrite it. Go and Rust sidestep the global entirely: the errno arrives as
a return value, so there is nothing thread-local to protect. And the EINTR
policy is really a concurrency policy in disguise — signals are asynchronous
interruptions, and a program whose I/O loops retry EINTR is one whose
correctness does not depend on when a signal lands. That property gets
load-tested in the Processes, Signals, Privilege part, when `pmon` starts
handling real signals around these same loops.

## Build, run, observe

```bash
[host]$ cd examples/05-errors-three-ways && ./demo.sh
```

The walkthrough runs the taxonomy per language. Reproduce it by hand against
any of the three binaries — shown here with a 200 000-byte source (sized so
the final read comes up short of the 64 KiB buffer):

```bash
[host]$ head -c 200000 /dev/urandom > src.bin
[host]$ ./cpp/build/release/app src.bin dst.bin; echo exit=$?
copied 200000 bytes
exit=0
[host]$ ./cpp/build/release/app nope.bin out.bin; echo exit=$?
copyx: nope.bin: No such file or directory
exit=2
[host]$ ./cpp/build/release/app src.bin /dev/full; echo exit=$?
copyx: write /dev/full: No space left on device
exit=3
```

Then run the same three cases with `./go/bin/app` and
`./rust/target/release/app`: on this host all three produced the identical
three lines and the identical exit codes 0, 2, 3, and `cmp src.bin dst.bin`
exited 0 for each copy — byte-identical output from byte-identical input,
which is the whole claim of the chapter. `/dev/full` is the tool that makes
the ENOSPC case repeatable: `ls -li /dev/full` shows a character device with
major:minor `1, 7`, a kernel-provided device whose write handler always
returns `-ENOSPC` — a full disk on tap, no root required. The book's runner
(`python3 scripts/test-all-examples.py --only 05-errors-three-ways`) drives
eleven behavioral checks per language through `verify.lua` — including the
empty-source edge case (`copied 0 bytes`, exit 0) — and reported
`PASS PASS PASS` on this host.

## Cross-check: watch the error being born

Everything above shows the error *after* each runtime dressed it up.
`strace -e trace=write` shows the moment it exists — the failing `write(2)`
itself. On the C++ binary:

```bash
[host]$ strace -e trace=write ./cpp/build/release/app src.bin /dev/full
write(4, "\343\275\2436'\325\377\3074N0\262\2248O\303U}\267R\327|\241?1\205M\265\303\320\f\2"..., 65536) = -1 ENOSPC (No space left on device)
write(2, "copyx: write /dev/full: No space"..., 47copyx: write /dev/full: No space left on device) = 47
write(2, "\n", 1
)                       = 1
+++ exited with 3 +++
```

Read it bottom-up from the failure: one 65 536-byte `write` to descriptor 4
(the destination; 3 is the source) fails with `ENOSPC`, and the *next* writes
are to descriptor 2 — the program printing its own error message. The entire
journey in Figure 5.1 happened between those two lines, in user space,
invisible to the kernel. The Rust binary (`strace -e trace=write
./rust/target/release/app src.bin /dev/full`) fails identically but emits the
message as six separate little writes (`"copyx: "`, `"write "`,
`"/dev/full"`, …) — `eprintln!` writing each format fragment straight to
unbuffered stderr. The Go binary (add `-f`; the runtime has threads) shows
the failing 65 536-byte write on descriptor **5**, not 4 — this trace shows
why: the runtime is already holding
`/sys/fs/cgroup/.../cpu.max` open on descriptor 3 for its container-aware
scheduling, so the source and destination land one higher. Three runtimes,
three descriptor layouts and stderr habits — and one identical failing
syscall, one identical message, one identical `+++ exited with 3 +++`.

> **On the lab VM** — <span class="status status--unverified">unverified</span>:
> the aggregate view of this chapter — *which* errnos your whole system's
> syscalls are failing with, live — belongs to eBPF tooling in the class of
> bpftrace one-liners counting negative syscall returns by errno. That is not
> runnable on this host without privileges; Chapter 30 (Debugging part)
> exercises it in the `systems-target` VM against these same binaries.

## What you learned

- Error codes are kernel ABI: a failing syscall returns `-errno` in a
  register, `ENOSPC` is 28 everywhere on the platform, and glibc's
  thread-local `errno`, Go's `unix.Errno` value, and rustix's `Errno` are
  three deliveries of the same integer.
- `copyx`'s taxonomy — exit 2 for the source side, 3 for the destination
  side, reasons in `strerror(3)` form — is expressible in all three type
  systems: `std::expected` mapped into a `Failure`, an `*opError` with
  `Unwrap` keeping `errors.Is`/`As` truthful, and a `thiserror` enum whose
  `exit_code()` the compiler keeps exhaustive.
- The book's retry policy: EINTR means nothing transferred, so reissue;
  short writes resume from where the kernel stopped; the write side observes
  `close(2)`; EAGAIN is a scheduling fact deferred to the epoll chapters.
- Exceptions (C++) and panics (Go, Rust) are for bugs and subsystem
  boundaries, never for outcomes the kernel can report; and `strace` can
  catch the exact syscall where an error is born, before any runtime
  touches it.

Next, the second thread that never stops: **concurrency, three ways** — the
same worker pool as `std::jthread`s and atomics, goroutines and channels,
and scoped threads under `Send`/`Sync`.

---

<p><span class="status status--verified">verified</span> — all evidence in this chapter was produced on the Fedora 44 host this session: each of the three binaries printed <code>copied 200000 bytes</code> (exit 0) with <code>cmp</code> exiting 0, <code>copyx: nope.bin: No such file or directory</code> (exit 2), and <code>copyx: write /dev/full: No space left on device</code> (exit 3); the strace excerpt is real output showing <code>write(4, …, 65536) = -1 ENOSPC</code> before the exit-3; the Go trace showed the failing write on descriptor 5 with <code>cpu.max</code> held on 3, and the Rust trace showed the fragmented stderr writes; <code>ls -li /dev/full</code> showed the <code>1, 7</code> character device; and <code>scripts/test-all-examples.py --only 05-errors-three-ways</code> reported PASS for cpp, go, and rust (3 passed, 0 failed, 0 skipped). The lab-VM eBPF callout above is the one unverified item, deferred to Chapter 30.</p>
