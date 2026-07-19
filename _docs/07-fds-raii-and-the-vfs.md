---
title: "fds, RAII, and the VFS"
order: 7
part: "Files and I/O"
description: "The three-layer model behind every file descriptor — fd table, open file description, inode — proven with /proc and strace, plus openat/fstatat directory-relative walking and an owning fd wrapper in each language, as fwatch v0 takes its first snapshot."
duration: 45 minutes
---

This chapter starts the second of the book's long-running artifacts: `fwatch`,
the file watcher that will grow inotify, epoll, io_uring, and finally a
Landlock sandbox over the next several chapters. Version 0 has no events at
all — it is a deterministic snapshot/diff tool — because before you can watch
files change you need to be precise about what a file descriptor actually
*is*. The one new idea here is that an fd is not a file: it is the top of a
three-layer chain — a slot in your process's fd table, pointing at a
system-wide *open file description*, pointing at an *inode* — and every flag,
offset, and ownership question in this part of the book lands on exactly one
of those three layers. Along the way we build the owning fd wrapper each
language uses for the rest of the book, and we walk directory trees the safe
way: relative to a directory fd, never by re-resolving paths.

The code is in `examples/07-fds-raii-and-the-vfs/`. `./demo.sh` there builds
all three implementations and runs a self-contained snapshot → mutate → diff
demo; its `README.md` specifies the CLI, the snapshot format, and the exit
codes all three languages share.

{% include excalidraw.html
   file="07-fd-table-vfs"
   alt="Three stacked layers: the fwatch process fd table holding fds 0-2 and four dirfds 3-6 with cloexec on, each dirfd pointing down to its own kernel open file description holding flags 02700000 and a readdir position, each description pointing down to a VFS inode numbered 69926 through 69929; dashed openat arrows chain fd 3 to fd 4 to fd 5 to fd 6."
   caption="Figure 7.1 — one paused fwatch walk: four dirfds in the per-process fd table, four open file descriptions carrying flags and offset, four inodes; every number in this figure comes from the /proc inspection later in the chapter" %}

> **Tools used** — `strace`, `stat`, `ls`, `lsof`, `touch`, `cmp`, `python3`
> (host); `opensnoop`/`bpftrace` (lab VM, exercised in Part 8). Everything here
> is checked by `scripts/check-host.sh`, ships with Fedora, or is preinstalled
> in the lab VMs.

## One fd, three tables

When `openat(2)` returns `3`, three separate kernel structures were involved,
and conflating them is the root of most fd bugs:

1. **The fd table** is per-process: a plain array indexed by small integers,
   which is why fds are always the lowest free number. Each slot holds two
   things only — a pointer to an open file description, and the
   **close-on-exec bit**. That placement matters: `O_CLOEXEC` is a property
   of *your process's slot*, not of the underlying open file.
2. **The open file description** is system-wide: created by each `open`,
   shared by `dup(2)` and inherited across `fork(2)`. It owns the status
   flags (`O_RDONLY`, `O_NOFOLLOW`, …) and the file offset. Two processes
   that inherited the same description share one offset — the classic
   surprise where a parent and child interleave writes; two independent
   `open`s of the same path get two descriptions and two offsets.
3. **The inode** is the VFS object for the file itself — type, size, mtime,
   and the number `ls -li` prints. Many descriptions can point at one inode;
   an inode with zero directory entries *and* zero descriptions is when the
   data actually goes away (which is why you can unlink a running binary).

`fwatch snapshot` exercises the whole chain: it opens directories (fd table
entries), enumerates them through their descriptions' readdir offsets, and
records size and mtime — inode fields — for every regular file. The
`<relpath> <size-bytes> <mtime-unix-seconds>` lines it prints, sorted
bytewise by path, are inode metadata reached through fd-relative syscalls
without ever re-resolving a full path.

## Open flags, and `O_CLOEXEC` everywhere

Every directory open in all three implementations uses the same four flags —
this is the C++ spelling, from
`examples/07-fds-raii-and-the-vfs/cpp/src/main.cpp`:

```cpp
constexpr int kDirFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
```

`O_DIRECTORY` makes the open fail with `ENOTDIR` if the name stopped being a
directory between `readdir` and `openat` — classification races are real, and
we would rather get an error than enumerate a file. `O_NOFOLLOW` refuses to
traverse a symlink in the final component, so a walk of a tree you do not
fully control cannot be steered out of that tree by swapping a subdirectory
for a symlink to `/etc`.

`O_CLOEXEC` deserves its own paragraph because the book will put it on
*every* fd it ever opens. Without it, every fd you hold is inherited by every
child process you — or any library you link — ever `fork`/`exec`s. That is
how a watcher ends up keeping a deleted directory alive because some spawned
helper still holds fd 5, or how a privileged fd leaks into an unprivileged
child. The tempting fix, "open, then set `FD_CLOEXEC` with `fcntl`", has a
hole: in a threaded program another thread can `fork`+`exec` in the window
between the two calls, and the fd escapes anyway. `O_CLOEXEC` closes that
window by setting the bit *atomically at open time* — which is why it is
baked into `kDirFlags` rather than applied afterwards. Note which layer it
lives on: the per-process fd table slot. A `dup` of the fd starts with the
bit clear; the open file description neither knows nor cares.

## `openat`, `fstatat`, and walking without paths

The naive way to walk a tree is string arithmetic: `readdir`, concatenate
`dir + "/" + name`, `stat` the result, recurse. Every one of those `stat`s
re-resolves the whole path from `/`, so every step is a fresh race: if any
ancestor component is renamed — or replaced by a symlink — between two calls,
you are suddenly operating on a different file than the one `readdir` told
you about. The `*at` syscall family fixes this by making the *directory fd*
the anchor: `openat(dirfd, name, …)` and `fstatat(dirfd, name, …,
AT_SYMLINK_NOFOLLOW)` resolve exactly one component, relative to a directory
you already hold open. Once `fwatch` holds fd 3 on the root of the tree,
renaming that tree out from under it changes nothing — the fd pins the inode,
not the name. `AT_SYMLINK_NOFOLLOW` completes the discipline for the stat
side: classify the entry itself, never what a symlink points at. (`AT_FDCWD`,
which `scan` passes for the very first open, is the pseudo-dirfd meaning
"resolve relative to the current working directory" — the only path-based
resolution in the whole program.)

This is not only about races. Anchored walks also survive path-length limits,
cross into the capability style of `openat2(2)` and `RESOLVE_BENEATH` that
the Landlock chapter will want, and are *faster* — one component resolved per
call instead of the whole prefix every time.

## How the code works

Two data structures carry the whole program. A `Tree` maps relative path →
`(size, mtime)`: C++ uses `std::map<std::string, Info>` and Rust
`BTreeMap<String, Info>` because both iterate in sorted key order, which
makes snapshot output deterministic for free; Go uses a plain
`map[string]info` and sorts the keys explicitly at print time — same
observable behavior, sorting moved to the edge. `Info` is two `int64`s; two
snapshots differ exactly when a key set or an `Info` differs, which is all
`diff` needs.

The second structure is the owning fd wrapper, and it is the heart of the
chapter. The C++ one, verbatim:

```cpp
// Owns a file descriptor; closes it exactly once.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};
```

Copying is deleted because two owners means two `close`s — a double close is
worse than a leak, since between the two calls the number may already name
someone else's freshly opened file. Moving transfers ownership by
`std::exchange`-ing the source to `-1`, so the moved-from destructor is a
no-op. `release()` is the escape hatch for APIs that *take* ownership, and
the C++ code needs it immediately: `fdopendir(3)` adopts the fd into a
`DIR*`, after which `closedir(3)` is the one and only cleanup. The
`DirStream` class documents that transfer in code — `DirStream::adopt(Fd)`
calls `fd.release()` only after `fdopendir` succeeds, so on the failure path
the `Fd` destructor still closes it. Exactly one owner on every path,
including errors.

Rust gets the same semantics from the standard library: `rustix::fs::open`
and `openat` return `std::os::fd::OwnedFd`, whose `Drop` is the close and
which is move-only by construction — the borrow checker rejects what C++
merely deletes. Functions that need the fd without owning it take it as
`BorrowedFd` (via `AsFd`, which is why `walk` can pass `&fd` to `statat` and
`openat` and still drop `fd` at the end). Go has no destructors, so the walk
wraps each raw dirfd in `os.NewFile` and `defer f.Close()` owns it — one
close per directory, on every return path.

The walk itself is the same function three times:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Walk the directory opened as `fd`, recording every regular file into `tree`.
// `display` is the path for error messages, `rel` the DIR-relative prefix.
[[nodiscard]] std::expected<void, Failure> walk(Fd fd, const std::string& display,
                                                const std::string& rel, Tree& tree) {
    auto stream = DirStream::adopt(std::move(fd));
    if (!stream) {
        return std::unexpected(fail("open directory", display, stream.error()));
    }

    while (const dirent* entry = ::readdir(stream->get())) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const auto st = stat_at(stream->fd(), name.c_str());
        if (!st) {
            if (st.error().value() == ENOENT) continue; // vanished mid-walk
            return std::unexpected(fail("stat", join(display, name), st.error()));
        }

        if (S_ISREG(st->st_mode)) {
            tree[join(rel, name)] = Info{static_cast<std::int64_t>(st->st_size),
                                         static_cast<std::int64_t>(st->st_mtim.tv_sec)};
        } else if (S_ISDIR(st->st_mode)) {
            auto sub = open_dir_at(stream->fd(), name.c_str());
            if (!sub) {
                if (sub.error().value() == ENOENT) continue;
                return std::unexpected(
                    fail("open directory", join(display, name), sub.error()));
            }
            if (auto walked =
                    walk(std::move(*sub), join(display, name), join(rel, name), tree);
                !walked) {
                return walked;
            }
        }
        // Symlinks, sockets, pipes, devices: not part of the v0 snapshot.
    }
    return {};
}
```

```go
// walk enumerates the directory owned by f, recording every regular file
// into out. display is the path for error messages, rel the DIR-relative
// prefix. f is closed before walk returns.
func walk(f *os.File, display, rel string, out tree) error {
	defer f.Close()

	names, err := f.Readdirnames(-1)
	if err != nil {
		return fmt.Errorf("reading '%s': %w", display, err)
	}

	dirfd := int(f.Fd())
	for _, name := range names {
		var st unix.Stat_t
		if err := unix.Fstatat(dirfd, name, &st, unix.AT_SYMLINK_NOFOLLOW); err != nil {
			if errors.Is(err, unix.ENOENT) {
				continue // vanished mid-walk
			}
			return failf("stat", join(display, name), err)
		}

		switch st.Mode & unix.S_IFMT {
		case unix.S_IFREG:
			out[join(rel, name)] = info{size: st.Size, mtime: st.Mtim.Sec}
		case unix.S_IFDIR:
			childFd, err := unix.Openat(dirfd, name, dirFlags, 0)
			if err != nil {
				if errors.Is(err, unix.ENOENT) {
					continue
				}
				return failf("open directory", join(display, name), err)
			}
			child := os.NewFile(uintptr(childFd), join(display, name))
			if err := walk(child, join(display, name), join(rel, name), out); err != nil {
				return err
			}
		}
		// Symlinks, sockets, pipes, devices: not part of the v0 snapshot.
	}
	return nil
}
```

```rust
/// Walk the directory owned by `fd`, recording every regular file into
/// `tree`. `display` is the path for error messages, `rel` the DIR-relative
/// prefix. `fd` is dropped (closed) when the walk of this level finishes.
fn walk(fd: OwnedFd, display: &str, rel: &str, tree: &mut Tree) -> Result<()> {
    // Dir::read_from dups the fd for its own cursor; `fd` stays usable as the
    // anchor for the *at() calls below.
    let dir = Dir::read_from(&fd).map_err(|e| fail("open directory", display, e))?;

    for entry in dir {
        let entry = entry.map_err(|e| fail("read directory", display, e))?;
        let name = entry.file_name().to_string_lossy().into_owned();
        if name == "." || name == ".." {
            continue;
        }

        let st = match statat(&fd, &name, AtFlags::SYMLINK_NOFOLLOW) {
            Ok(st) => st,
            Err(rustix::io::Errno::NOENT) => continue, // vanished mid-walk
            Err(e) => return Err(fail("stat", &join(display, &name), e)),
        };

        match FileType::from_raw_mode(st.st_mode) {
            FileType::RegularFile => {
                tree.insert(
                    join(rel, &name),
                    Info { size: st.st_size as i64, mtime: st.st_mtime as i64 },
                );
            }
            FileType::Directory => {
                let sub = match openat(&fd, &name, DIR_FLAGS, Mode::empty()) {
                    Ok(sub) => sub,
                    Err(rustix::io::Errno::NOENT) => continue,
                    Err(e) => return Err(fail("open directory", &join(display, &name), e)),
                };
                walk(sub, &join(display, &name), &join(rel, &name), tree)?;
            }
            // Symlinks, sockets, pipes, devices: not part of the v0 snapshot.
            _ => {}
        }
    }
    Ok(())
}
```

Structure first: each `walk` takes *ownership* of the directory fd (by-value
`Fd`, `*os.File` with `defer Close`, by-value `OwnedFd`), enumerates
entries, `fstatat`s each one relative to that fd, records regular files under
`join(rel, name)`, and recurses into subdirectories by `openat`-ing a child
fd and handing ownership down. `display` and `rel` are deliberately separate
strings — errors should say `cannot stat 'tree/sub/b.txt'` with the
user-supplied prefix, while snapshot keys must be root-relative so a snapshot
taken as `fwatch snapshot ./tree` compares cleanly against a rescan of the
same tree by any other spelling of its path.

Three per-language details are worth dwelling on. In C++, enumeration goes
through `DirStream` because `readdir` needs a `DIR*`, but classification and
descent use `stream->fd()` — `dirfd(3)` recovers the underlying fd from the
stream, so the anchor for the `*at` calls is the same open file description
the stream reads. In Go, `dirfd := int(f.Fd())` extracts the raw integer,
which is safe here *only because* `defer f.Close()` keeps `f` reachable for
the whole function — more on that in the concurrency lens. In Rust,
`Dir::read_from(&fd)` borrows the fd and takes its own handle for the
enumeration cursor (you can see it in the strace below as an extra
`openat(fd, ".")`), leaving `fd` free to serve as the `statat`/`openat`
anchor; entry order comes from the kernel unsorted in all three languages,
and the sorted map makes the output deterministic anyway.

The wiring is thin by design. `scan` opens the root (`AT_FDCWD`-relative —
the only full-path resolution), seeds the recursion, and returns the `Tree`.
`cmd_snapshot` prints it. `cmd_diff` re-reads a saved snapshot with an
ordinary buffered file read — parsing lines *from the right*, so the last two
space-separated fields are size and mtime and paths containing spaces
round-trip — then walks the sorted union of old and new key sets emitting
`+`/`-`/`~` lines and the final `fwatch: A added, R removed, M modified`
summary, which prints even when all counts are zero. Fragile bits, stated
plainly: mtime has one-second granularity here (`st_mtim.tv_sec`), so a
same-second same-size rewrite is invisible to `diff` — the durability chapter
sharpens this; symlinks, sockets, pipes, and devices are skipped entirely in
v0; and the whole tree lives in memory, which is fine for the trees `fwatch`
will watch and wrong for a backup tool.

## Errors, three ways

All three implementations must produce byte-identical diagnostics — the
contract is `fwatch: error: cannot open directory 'X' (errno N)` on stderr
and exit 1 — so each language has to recover the *raw errno* from its own
error machinery. C++ converts `errno` to `std::error_code` at the syscall
wrapper and formats `Failure` messages with `ec.value()`; fallible functions
return `std::expected`, and `main` never touches `errno` directly. Go wraps
with `%w` (`failf` → `fmt.Errorf`), keeps the chain queryable, and digs the
number back out with `errors.As(err, &errno)` — while `errors.Is(err,
unix.ENOENT)` handles the one *recoverable* error: an entry that `readdir`
reported but that vanished before `fstatat`, which every implementation
silently skips rather than failing the walk. Rust matches
`rustix::io::Errno::NOENT` structurally for the same skip and formats
`errno.raw_os_error()` into an `anyhow` error otherwise. Same policy, three
mechanisms: races you expect are handled at the call site; everything else
carries the path and the errno up to one printer.

## Concurrency lens

`fwatch` v0 is deliberately single-threaded in all three languages — the walk
is a depth-first recursion with one live fd chain — but two concurrency
points are already load-bearing. First, `O_CLOEXEC`: the fd table is shared
by every thread in the process, and the fork+exec window it closes is a
*threading* hazard — even "single-threaded" Go code runs on a multithreaded
runtime that may spawn processes on your behalf. Second, Go's `os.File` has a
cleanup registered with the runtime: if a `File` becomes unreachable, the
garbage collector closes its fd for you. That sounds like RAII but is not —
it is nondeterministic, and it means `int(f.Fd())` is a loan against `f`'s
liveness: hold the raw integer past the last reference to `f` and the
collector can close the fd while you are still using the number, or worse,
after the kernel has reassigned it. The `defer f.Close()` in `walk` is
therefore doing double duty — deterministic cleanup *and* a liveness anchor
for `dirfd`. Rust makes the same mistake unrepresentable (`BorrowedFd`'s
lifetime is tied to the `OwnedFd`), and C++ trusts you to keep the `Fd`
alive, which is why the book's wrapper never hands out an owning raw int
except through the explicitly named `release()`.

## Build, run, observe

```bash
[host]$ cd examples/07-fds-raii-and-the-vfs && ./demo.sh
```

Each language builds, snapshots a fixture tree into a temp file, mutates the
tree, and diffs. To drive it by hand the way the rest of this chapter does,
make a small tree with pinned mtimes and snapshot it:

```bash
[host]$ mkdir -p tree/sub/deep
[host]$ printf hello > tree/a.txt && printf bye > tree/gone.txt
[host]$ printf worldwide > tree/sub/b.txt && printf abc > tree/sub/deep/c.bin
[host]$ touch -d @1000000000 tree/a.txt && touch -d @1000000300 tree/gone.txt
[host]$ touch -d @1000000100 tree/sub/b.txt && touch -d @1000000200 tree/sub/deep/c.bin
[host]$ ./cpp/build/release/app snapshot tree
a.txt 5 1000000000
gone.txt 3 1000000300
sub/b.txt 9 1000000100
sub/deep/c.bin 3 1000000200
```

On this host the Go and Rust binaries print those same four lines, and
`cmp` between the three captured outputs confirms they are byte-identical —
that identical observable behavior is what `verify.lua` asserts, and the
runner (`python3 scripts/test-all-examples.py --only 07-fds-raii-and-the-vfs`)
reports `PASS PASS PASS` for cpp, go, and rust.

## Cross-check, three ways

**Snapshot fields against `ls -li` and `stat`.** The snapshot claims sizes
5, 3, 9, 3 and four specific mtimes. Independent tools that read the same
inodes agree, field for field:

```bash
[host]$ ls -li tree
68626 -rw-r--r-- 1 rsedor rsedor  5 Sep  8  2001 a.txt
68627 -rw-r--r-- 1 rsedor rsedor  3 Sep  8  2001 gone.txt
68624 drwxr-xr-x 3 rsedor rsedor 80 Jul 18 23:02 sub
[host]$ stat --format='%i %s %Y %n' tree/a.txt tree/sub/b.txt
68626 5 1000000000 tree/a.txt
68628 9 1000000100 tree/sub/b.txt
```

`ls` renders `mtime` 1000000000 as `Sep 8 2001` — the same inode field,
formatted; the sizes match the snapshot's second column exactly.

**The syscalls, under `strace`.** Filtering the C++ binary to the calls the
source claims it makes (on x86_64 glibc, `fstatat` arrives in the kernel as
`newfstatat`; nothing in this program uses `statx`, and tracing it confirms
zero calls):

```bash
[host]$ strace -e trace=openat,newfstatat,statx ./cpp/build/release/app snapshot tree
openat(AT_FDCWD, "tree", O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY) = 3
newfstatat(3, "gone.txt", {st_mode=S_IFREG|0644, st_size=3, ...}, AT_SYMLINK_NOFOLLOW) = 0
newfstatat(3, "a.txt", {st_mode=S_IFREG|0644, st_size=5, ...}, AT_SYMLINK_NOFOLLOW) = 0
newfstatat(3, "sub", {st_mode=S_IFDIR|0755, st_size=80, ...}, AT_SYMLINK_NOFOLLOW) = 0
openat(3, "sub", O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY) = 4
newfstatat(4, "b.txt", {st_mode=S_IFREG|0644, st_size=9, ...}, AT_SYMLINK_NOFOLLOW) = 0
newfstatat(4, "deep", {st_mode=S_IFDIR|0755, st_size=60, ...}, AT_SYMLINK_NOFOLLOW) = 0
openat(4, "deep", O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY) = 5
newfstatat(5, "c.bin", {st_mode=S_IFREG|0644, st_size=3, ...}, AT_SYMLINK_NOFOLLOW) = 0
+++ exited with 0 +++
```

(Loader lines for `ld.so.cache` and the shared libraries trimmed.) Read the
first arguments: only the root open uses `AT_FDCWD`; every later `openat` and
`newfstatat` anchors on fd 3, 4, or 5 — the dirfd chain, exactly as Figure
7.1 draws it, with every open carrying `O_CLOEXEC|O_NOFOLLOW|O_DIRECTORY`.
The kernel also reports unsorted entry order (`gone.txt` before `a.txt`);
the sorted output really does come from the map. The Rust binary shows one
extra `openat(3, ".", …) = 4` per level — `Dir::read_from` acquiring its own
cursor handle, just as its comment says.

**The fd table of a live walk, in `/proc`.** `snapshot` finishes its walk
before printing, so to catch the dirfds open you have to slow the walk down.
`strace` can inject delays; against a tree `big/l1/l2/l3/` holding 3000
files, delaying each stat by 20 ms leaves minutes to inspect:

```bash
[host]$ strace -o /dev/null -e trace=newfstatat -e inject=newfstatat:delay_enter=20000 \
        ./cpp/build/release/app snapshot big > /dev/null &
[host]$ ls -l /proc/<pid>/fd
lr-x------ 1 rsedor rsedor 64 Jul 18 23:03 3 -> …/fixture/big
lr-x------ 1 rsedor rsedor 64 Jul 18 23:03 4 -> …/fixture/big/l1
lr-x------ 1 rsedor rsedor 64 Jul 18 23:03 5 -> …/fixture/big/l1/l2
lr-x------ 1 rsedor rsedor 64 Jul 18 23:03 6 -> …/fixture/big/l1/l2/l3
[host]$ cat /proc/<pid>/fdinfo/6
pos:	1980
flags:	02700000
mnt_id:	59
ino:	69929
```

(fds 0–2 trimmed.) Four dirfds, one per recursion level — the walk holds
`O(depth)` fds, not `O(files)`. The `fdinfo` file reads out the *description*
layer: `pos: 1980` is the readdir cursor partway through l3's 3000 entries,
and `flags: 02700000` decodes as `O_CLOEXEC` (02000000) + `O_NOFOLLOW`
(0400000) + `O_DIRECTORY` (0200000) + `O_LARGEFILE` (0100000, added by the
64-bit ABI) — the close-on-exec bit, verified on a live process. The `ino:`
lines completed the loop on this host: fds 3–6 reported inodes 69926–69929,
and `ls -lid big big/l1 big/l1/l2 big/l1/l2/l3` printed 69926, 69927, 69928,
and 69929 for the same directories, while
`lsof -a -p <pid> -d 0-20` listed the four fds as type `DIR` with the same
`NODE` numbers. fd table → description → inode: each layer observed with a
different tool, all agreeing.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the eBPF view of this chapter is watching `openat` flags fleet-wide with
> `opensnoop`/`bpftrace` instead of per-process `strace`; bcc-tools are not
> runnable on this host without privileges, and chapter 30 (Debugging part)
> exercises exactly that on the `systems-target` VM.

## What you learned

- A file descriptor is three layers: a per-process fd-table slot (home of the
  close-on-exec bit), a shared open file description (flags and offset —
  `/proc/<pid>/fdinfo` reads it out), and the inode (`ls -li`'s numbers,
  `fwatch`'s size and mtime).
- `O_CLOEXEC` at open time is the only race-free spelling, and
  `O_DIRECTORY|O_NOFOLLOW` turn classification races into clean errors;
  `openat`/`fstatat` anchored on a dirfd resolve one component per call and
  are immune to ancestor renames.
- Ownership is the same idea three ways: a move-only `Fd` with an explicit
  `release()` for `fdopendir`'s ownership transfer, `os.File` with a deferred
  `Close` that also keeps the GC's nondeterministic closer at bay, and
  `OwnedFd`/`BorrowedFd` where the compiler enforces the discipline.
- Expected races are policy: `ENOENT` on a vanished entry is a skip at the
  call site in all three languages; everything else carries path + errno to
  one printer that formats identical text.

Next, **page cache and durability**: what the kernel does with the bytes you
write, why `fwatch`'s one-second mtimes can lie, and what `fsync` actually
promises.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host this
session: the runner printed <code>07-fds-raii-and-the-vfs  PASS  PASS
PASS</code> (3 passed, 0 failed, 0 skipped); all three binaries emitted the
identical four snapshot lines shown (confirmed byte-identical with
<code>cmp</code>); the strace excerpt is real trimmed output showing the
<code>AT_FDCWD</code> root open and the fd 3→4→5 dirfd chain with
<code>O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_DIRECTORY</code> on every open; and the
delay-injection run yielded the live <code>/proc/&lt;pid&gt;/fd</code>
listing (dirfds 3–6), <code>fdinfo</code> flags <code>02700000</code> and
inodes 69926–69929 matching <code>ls -lid</code> and <code>lsof</code>. The
"On the lab VM" eBPF callout is unverified as marked, as is the exact readdir
<code>pos</code> value on filesystems other than tmpfs.</p>
