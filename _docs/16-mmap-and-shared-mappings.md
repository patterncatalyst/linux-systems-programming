---
title: "mmap and shared mappings"
order: 16
part: "Memory"
description: "File-backed vs anonymous and MAP_SHARED vs MAP_PRIVATE copy-on-write, msync guarantees against page-cache writeback, the ftruncate+mmap lifecycle, and memfd sealing — proven by shmkv v0, one byte-exact store format that the C++, Go, and Rust binaries create, write, and read interchangeably."
duration: 50 minutes
---

The previous chapter took the address space apart — VMAs, page tables, faults —
without ever asking where a mapping's bytes come from. This chapter answers
that with the book's next artifact: `shmkv`, a key/value store whose entire
database is one `mmap`'d file. The one new idea is that `MAP_SHARED` collapses
three things you have treated as separate — your memory, the page cache, and
the file on disk — into a single set of physical pages. A store instruction in
one process is a load result in another and a dirty page the kernel will
eventually write out, with no `read`, no `write`, and no copy in between.
Version 0 leans on that hard: three binaries in three languages agree on every
byte of one on-disk format, so the Rust binary can create a store, the Go
binary can write into it, and the C++ binary reads the value back.

The code is in `examples/16-mmap-and-shared-mappings/`. `./demo.sh` there
builds all three implementations; its `README.md` specifies the byte-exact
format, the CLI, and the exit codes all three languages share.

{% include excalidraw.html
   file="16-mapping-page-cache"
   alt="Two user-space process bands above a kernel band and a disk band. Process A holds an mmap MAP_SHARED node marked rw-s and a dashed MAP_PRIVATE node; process B holds its own MAP_SHARED node. Amber and grey arrows from both mappings converge on one page-cache node for store.kv in the kernel band; a dashed arrow back to the private node is labeled first write copies the page; an amber msync MS_SYNC arrow and a dashed writeback-eventually arrow lead down to the store.kv-on-disk node."
   caption="Figure 16.1 — one file, one set of page-cache pages, many mappings: A's store and B's load meet in the same physical memory, msync forces those pages to disk, and a MAP_PRIVATE view forks its own copy on first write" %}

> **Tools used** — `strace`, `od`, `cmp`, `stat`, `python3` (host);
> `bpftrace` (lab VM, exercised in Part 8). Everything here is checked by
> `scripts/check-host.sh`, ships with Fedora, or is preinstalled in the lab
> VMs.

## The mapping quadrant

Every `mmap` call picks one cell of a 2×2. The first axis is **what backs the
pages**: a *file-backed* mapping names an fd and an offset, so its pages are
literally page-cache pages of that file; an *anonymous* mapping
(`MAP_ANONYMOUS`) is backed by nothing but zero-fill — this is what your
allocator asks for when it wants a fresh arena, which is chapter 17's whole
subject. The second axis is **who sees your writes**. `MAP_SHARED` means
writes go to the backing object itself: for a file, straight into the shared
page cache, visible immediately to every other process mapping the same range.
`MAP_PRIVATE` means copy-on-write: your mapping starts out referencing the
very same page-cache pages — reads are free and shared — but the page-table
entries are write-protected, and the first store to a page faults, at which
point the kernel copies that page and points only *your* mapping at the copy.
Your writes never reach the file, and after the copy, other processes' writes
to that page stop reaching you. That is exactly how the loader maps your
binary: code and read-only data `MAP_PRIVATE` from the executable, shared by
every running instance until someone (a debugger planting a breakpoint) dirties
a page.

Copy-on-write is observable in one screenful. With a store file present, map
it privately and deface the magic:

```python
import mmap, os
fd = os.open("demo.kv", os.O_RDWR)
size = os.fstat(fd).st_size
priv = mmap.mmap(fd, size, flags=mmap.MAP_PRIVATE)
priv[0:5] = b"XXXXX"                      # scribble over the magic... privately
print("private view :", bytes(priv[0:5]))
print("file on disk :", open("demo.kv", "rb").read(5))
```

```console
[host]$ python3 private_cow.py
private view : b'XXXXX'
file on disk : b'SHKV1'
```

The write landed in a COW copy; the file — and every `MAP_SHARED` mapper —
still sees `SHKV1`. `shmkv` therefore uses `MAP_SHARED` everywhere: the page
cache *is* the database.

## What `msync` promises — and what writeback already does

Two guarantees get conflated here, and they have different owners.
**Visibility** between processes needs no syscall at all: two `MAP_SHARED`
mappings of the same file range resolve to the same physical pages, so a store
in one is a load in the other at ordinary memory speed. **Durability** is a
separate question: your store dirtied a page-cache page, and the kernel's
writeback machinery will get it to disk *eventually* — on Fedora's defaults,
dirty pages start aging out after 30 seconds. If the machine loses power
before that, the store never happened. `msync(addr, len, MS_SYNC)` closes the
gap: it blocks until the dirty pages in that range are written to the backing
file, the mapped-memory analogue of `fsync`'s promise from the durability
chapter (`MS_ASYNC`, by contrast, is close to a no-op on modern Linux — it
just nudges writeback). `shmkv` calls `msync(MS_SYNC)` at the end of every
`create` and `set`, so "`set alpha`" on stdout means the bytes are on disk,
not merely in the cache. The trade-off is the same one `fsync` bought: each
write now costs a device round-trip.

## The `ftruncate` + `mmap` lifecycle

A file-backed mapping does not size itself. `create` performs a fixed ritual:
`open(O_RDWR|O_CREAT|O_TRUNC)` so a reused path starts at zero bytes, then
`ftruncate(fd, 10 + slots*256)` to extend the file, then `mmap` for exactly
that length. Order matters twice. First, the mapping's length is fixed at map
time — mapping a still-empty file and truncating afterwards would leave pages
you can touch but a file too short to back them, and touching a page past EOF
delivers `SIGBUS`, not an error return. Second, `ftruncate` extension is
guaranteed zero-fill, and the format defines `key[0] == 0` as "empty slot" —
so a fresh store needs no initialization loop at all; every slot is born empty
because the kernel says extended bytes are zero. The header write and one
`msync` finish the job. `open` for an existing store is the mirror image:
`fstat` for the size, refuse anything smaller than a header, map exactly
`st_size` bytes, then validate magic and the size equation before trusting a
single slot.

### Aside: `memfd_create` and sealing, live

Sometimes you want the mapping semantics without any filesystem name.
`memfd_create(2)` returns an fd to an anonymous tmpfs file — mappable,
`ftruncate`-able, passable over a Unix socket — and, uniquely, **sealable**:

```python
fd = os.memfd_create("shmkv-demo", os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
os.ftruncate(fd, 4096)
print(os.readlink(f"/proc/self/fd/{fd}"))          # an anonymous "file"
m = mmap.mmap(fd, 4096, flags=mmap.MAP_SHARED)
m[:6] = b"SHKV1\0"
fcntl.fcntl(fd, fcntl.F_ADD_SEALS,
            fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_FUTURE_WRITE)
```

```console
[host]$ python3 memfd_seal.py
/memfd:shmkv-demo (deleted)
seals: 22
new writable map: [Errno 1] Operation not permitted
```

Run this session: the fd's `/proc` link shows `/memfd:shmkv-demo (deleted)` —
a file with no name anywhere; `F_GET_SEALS` reads back 22
(`F_SEAL_SHRINK`=2 + `F_SEAL_GROW`=4 + `F_SEAL_FUTURE_WRITE`=16); the existing
mapping keeps writing, but a *new* writable `MAP_SHARED` attempt fails with
`EPERM`. Sealing is what makes shared memory safe to accept from an untrusted
peer — the `SIGBUS`-via-truncation attack below becomes impossible once
`F_SEAL_SHRINK` is set. The IPC part passes exactly such fds between
processes.

## One layout, three languages

Interop works because the format is defined in bytes, not in types: magic
`"SHKV1\0"` at offset 0, a **u32 little-endian** slot count at offset 6, then
256-byte slots — key NUL-padded in `[0..64)`, value in `[64..256)`. Note that
offset 6 is deliberately *misaligned* for a 4-byte integer, and none of the
three implementations care, because none of them ever casts a pointer into the
mapping: C++ assembles the u32 from four bytes with shifts, Go uses
`binary.LittleEndian.PutUint32`, Rust `u32::to_le_bytes` — byte moves have no
alignment requirements and no host-endianness dependence.

The tempting alternative — declare a struct and cast the mapping to it —
is where every language would break differently. A C++
`struct { char magic[6]; uint32_t slots; }` inserts two padding bytes to align
`slots`, silently making the header 12 bytes (and the unaligned cast would be
undefined behavior besides); `repr(Rust)` makes no layout promise at all —
Rust is free to reorder fields — so only `#[repr(C, packed)]` would be
defensible; Go specifies no struct padding contract and has no packed
annotation, which is precisely why `encoding/binary` exists. And all three
would still inherit the CPU's byte order. Stating "u32 LE at offset 6" in the
format and copying bytes is boring, and boring is why `cmp` can prove two
stores identical.

{% include excalidraw.html
   file="16-shmkv-layout"
   alt="A byte-layout band shows store.kv as 1034 bytes: a dark magic node with bytes 53 48 4b 56 31 00, an amber u32 little-endian slot_count node with bytes 04 00 00 00, and four 256-byte slot boxes. A zoom band below splits one slot into a key field covering bytes 0 to 64 and a value field covering 64 to 256, both NUL-padded. Three language nodes — C++ writes it, Go reads it, Rust reads it — point up into the layout."
   caption="Figure 16.2 — the shmkv v0 format, byte for byte; the header bytes shown are the real od output from this chapter's run, and cmp proves a Rust-built and a C++-built store file identical" %}

## How the code works

Each implementation is one `Store` abstraction over two resources — the fd and
the mapping — plus four commands. The mapping is where the languages differ
most, so that is the excerpt:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// RAII shared mapping: mmap on construction, munmap in the destructor,
// msync(MS_SYNC) on demand. MAP_SHARED means stores are visible to every
// other process mapping the same file, and reach the page cache (and disk,
// after msync) rather than staying private to this address space.
class Mapping {
  public:
    static Result<Mapping> map_shared(const Fd& fd, std::size_t len) {
        void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (p == MAP_FAILED) {
            return std::unexpected(fail(1, "shmkv: mmap failed"));
        }
        return Mapping{static_cast<unsigned char*>(p), len};
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), len_(std::exchange(other.len_, 0)) {}
    Mapping& operator=(Mapping&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = std::exchange(other.data_, nullptr);
            len_ = std::exchange(other.len_, 0);
        }
        return *this;
    }
    ~Mapping() { reset(); }

    [[nodiscard]] unsigned char* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }

    [[nodiscard]] Result<> sync() const {
        if (::msync(data_, len_, MS_SYNC) != 0) {
            return std::unexpected(fail(1, "shmkv: msync failed"));
        }
        return {};
    }

  private:
    Mapping(unsigned char* data, std::size_t len) noexcept : data_(data), len_(len) {}
    void reset() noexcept {
        if (data_ != nullptr) ::munmap(data_, len_);
        data_ = nullptr;
        len_ = 0;
    }
    unsigned char* data_ = nullptr;
    std::size_t len_ = 0;
};
```

```go
// store owns the fd and the shared mapping; close() unmaps then closes,
// mirroring the RAII types in the C++ and Rust versions.
type store struct {
	fd    int
	data  []byte
	slots uint32
}

func (s *store) close() {
	if s.data != nil {
		_ = unix.Munmap(s.data) // []byte discipline: never touch s.data after this
		s.data = nil
	}
	if s.fd >= 0 {
		_ = unix.Close(s.fd)
		s.fd = -1
	}
}

func mapShared(fd int, size int) ([]byte, error) {
	data, err := unix.Mmap(fd, 0, size, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		return nil, failf(1, fmt.Errorf("mmap: %w", err), "shmkv: mmap failed")
	}
	return data, nil
}
```

```rust
/// Map `file` MAP_SHARED, read/write.
///
/// ---- unsafe boundary -------------------------------------------------
/// `MmapMut::map_mut` is `unsafe` because the compiler cannot see other
/// writers of the underlying file: another process truncating it would
/// turn our in-bounds loads into SIGBUS, and concurrent mutation would be
/// a data race the borrow checker never observed. Why the call is sound
/// here:
///   * every access goes through `&self.map[..]` slices bounded by the
///     length we mapped, never raw pointers;
///   * we size the mapping from ftruncate/fstat in the same process and
///     no shmkv command ever shrinks an existing store file;
///   * the demo runs shmkv commands sequentially, so cross-process writes
///     are ordered by process exit (and each writer msyncs before exit) —
///     the concurrent-writer race the type system cannot rule out is
///     excluded by the tool's usage model, not by the compiler.
/// Everything outside this function stays in safe Rust.
/// ----------------------------------------------------------------------
fn map_shared(file: &std::fs::File) -> Result<MmapMut> {
    unsafe { MmapMut::map_mut(file) }.map_err(|_| Fail::new(1, "shmkv: mmap failed"))
}
```

Read the three as one argument about **who admits the danger**. A shared
mapping is memory whose contents can be changed — or whose backing can be
yanked — by something the compiler cannot see. Rust's `memmap2` makes you sign
for that: `MmapMut::map_mut` is `unsafe`, and the boundary comment is the
signature — the promises (in-bounds slices, nobody shrinks the file, writers
are sequenced) are exactly the conditions under which treating the mapping as
`&mut [u8]` is sound, and everything outside that one function is safe Rust.
C++ silently allows the same thing: `mmap` hands back `void*`, and nothing
distinguishes a pointer into a shared, externally mutable, `SIGBUS`-capable
region from a pointer into a private heap block — the `Mapping` class adds
lifetime discipline (move-only, `munmap` exactly once, in `Store` declared
*after* `Fd` so it unmaps before the fd closes) but the type system never
learns what the pointer means. Go hides it behind familiarity:
`unix.Mmap` returns an ordinary-looking `[]byte` whose backing array is not
GC-managed memory at all; the slice header outlives the mapping's validity the
moment `Munmap` runs, and the one-line discipline comment in `close` is all
that stands between you and a use-after-unmap fault the runtime will report as
a plain `SIGSEGV`/`SIGBUS`.

Above that boundary the three programs are deliberately the same. `create`
runs the truncate-extend-map-stamp ritual and reports
`created FILE: N slots, SIZE bytes`. `open` validates before trusting:
size ≥ 10, magic bytes equal, and `size == 10 + slots*256` exactly — the last
check is what turns "some file that starts with SHKV1" into "a store whose
every slot index is in bounds", so the slot arithmetic
(`offset = 10 + i*256`) can never leave the mapping. `set` is a linear probe
that prefers the slot already holding the key (overwrite burns no new slot),
else the first empty one, else fails with exit 5; it clears the whole 256-byte
slot before copying key and value so a shorter value never leaves a longer
predecessor's tail bytes behind — without that `memset`, the two "identical"
stores in the cross-check would differ in the padding and `cmp` would say so.
`get` and `dump` scan slots through a `field()` helper that stops at the first
NUL, and `dump` sorts pairs bytewise by key so slot order — an artifact of
insertion history — never leaks into output. One fragile bit is worth naming:
C++ flushes stdout before printing `dump`'s stderr summary, because stdout to
a pipe is fully buffered and the combined-stream interleaving must match
Go and Rust byte for byte.

## Errors, three ways

The contract is a fixed stderr line plus a fixed exit code: 1 for
open/mmap/format failures, 2 for usage and oversized keys (>63 bytes) or
values (>191 bytes), 4 for `get` on a missing key, 5 for `set` on a full
store. C++ carries `CliError{code, msg}` through `std::expected`, and `main`
is the single printer. Rust's `Fail` is the same shape with one twist: an
empty message *means* "print the usage line", so `--slots 0`,
`--slots 12x`, and `--slots 99999999999` all funnel through `parse_slots`
(digits only, range [1, u32 max] — identical rules in all three languages)
into the same exit-2 usage path. Go is the most instructive: `cliError` pins
`code` and `msg` but also wraps an underlying cause, so `errors.As` routes it
in `main` while the `%w` chain keeps `mmap: ...` diagnosable — and a sentinel
`errUsage` plays Rust's empty-message role via `errors.Is`. Reproduced this
session: a junk file gets `shmkv: bad.kv: not a shmkv v0 store` and exit 1, a
missing file `shmkv: cannot open missing.kv` and exit 1, the third `set` into
a 2-slot store `shmkv: store full (2 slots)` and exit 5, a 64-byte key
`shmkv: key too long (max 63 bytes)` and exit 2 — identically from all three
binaries, which is what 48 `verify.lua` checks per language assert.

## Concurrency lens

`MAP_SHARED` gives you visibility, and visibility is not synchronization. Two
`set` processes running *concurrently* could both probe, both find slot 2
empty, and both write it — last `memcpy` wins, one key silently lost; worse, a
`get` racing a `set` can read a slot mid-`memset` and see a torn key. Nothing
in this chapter prevents that: `msync` is a durability barrier, not a memory
barrier, and the kernel orders nothing between mappers. shmkv v0's defense is
its usage model — commands are whole processes run sequentially, so every
write is ordered by process exit — and the Rust unsafe-boundary comment is
where that assumption is written down rather than left implicit. This is the
cliff-hanger by design: chapter 20 puts atomics and futexes *inside* a shared
mapping to make concurrent access actually safe. Two language-specific hazards
round it out. Go's `[]byte` view means the race detector and GC offer no help
across processes — and the slice must never be touched after `Munmap`, which
`close` enforces by nil-ing it. And any mapper can be killed by a peer that
truncates the file — in-bounds loads become `SIGBUS` — which is exactly the
hole `F_SEAL_SHRINK` sealed in the memfd aside.

## Build, run, observe

```bash
[host]$ cd examples/16-mmap-and-shared-mappings && ./demo.sh
```

The full matrix is one runner line — from this session:

```console
[host]$ python3 scripts/test-all-examples.py --only 16-mmap-and-shared-mappings
example                      cpp   go    rust
16-mmap-and-shared-mappings  PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

Driving the interop by hand is more instructive. Rust creates and writes, Go
writes into the same store, C++ reads everything back:

```bash
[host]$ ./rust/target/release/app create demo.kv --slots 4
created demo.kv: 4 slots, 1034 bytes
[host]$ od -An -tx1 -N10 demo.kv
 53 48 4b 56 31 00 04 00 00 00
[host]$ ./rust/target/release/app set demo.kv zeta z1 && ./rust/target/release/app set demo.kv alpha a1
set zeta
set alpha
[host]$ ./go/bin/app set demo.kv gadd g1
set gadd
[host]$ ./cpp/build/release/app get demo.kv gadd
g1
[host]$ ./go/bin/app set demo.kv alpha a2 && ./cpp/build/release/app get demo.kv alpha
set alpha
a2
[host]$ ./rust/target/release/app dump demo.kv
alpha=a2
gadd=g1
zeta=z1
shmkv: 3/4 slots used
```

The `od` line is Figure 16.2 in the flesh: `53 48 4b 56 31 00` is
`SHKV1\0`, and `04 00 00 00` is 4 as a u32 LE. The overwrite of `alpha`
reused its slot (3/4 used, not 4/4), and the dump is key-sorted regardless of
which language inserted what.

## Cross-check, three ways

**The syscalls, under `strace`.** One C++ `set`, filtered to the calls the
source claims (loader noise trimmed):

```console
[host]$ strace -e trace=openat,mmap,msync,munmap ./cpp/build/release/app set demo.kv alpha a2
openat(AT_FDCWD, "demo.kv", O_RDWR)     = 3
mmap(NULL, 1034, PROT_READ|PROT_WRITE, MAP_SHARED, 3, 0) = 0x7ff82e437000
msync(0x7ff82e437000, 1034, MS_SYNC)    = 0
munmap(0x7ff82e437000, 1034)            = 0
```

Exactly the lifecycle the code promises: map 1034 bytes `MAP_SHARED`, one
`MS_SYNC` over the whole store before success is reported, unmap before the
fd closes. The Go binary's trace shows the identical four lines (its fd is 4 —
the runtime holds extras).

**The mapping, in `/proc`.** `set` exits too fast to inspect, so inject a 3 s
delay into `msync` and read the paused process's map table:

```console
[host]$ strace -o /dev/null -e trace=msync -e inject=msync:delay_enter=3000000 \
        ./cpp/build/release/app set demo.kv alpha a2 &
[host]$ grep demo.kv /proc/<pid>/maps
7ff342434000-7ff342435000 rw-s 00000000 00:24 24538261   .../demo.kv
```

The permissions column is the whole chapter in four characters: `rw-s` —
readable, writable, **s**hared. A `MAP_PRIVATE` mapping shows `p` in that
position (look at your own binary's lines in the same file). The 1034-byte
store occupies one 4096-byte page range: mappings are page-granular, and the
file's last partial page is zero-padded in memory.

**The format, by `cmp`.** A second store built by the *C++* binary replaying
the identical operations (`create --slots 4`, then the same four `set`s) is
byte-identical to the Rust-created, Go-amended one — `cmp demo.kv twin.kv`
prints nothing and exits 0, zero padding included. That, plus the three
byte-identical `dump` outputs, is what `verify.lua` mechanizes: 48 checks per
language, `PASS 48 / FAIL 0` for cpp, go, and rust in this session's run.

> **On the lab VM** <span class="status status--unverified">unverified</span> —
> the eBPF view of this chapter is watching page faults and writeback
> fleet-wide (`bpftrace` on the `page-faults` and `writeback` tracepoints)
> instead of per-process `strace`; bcc-tools are not runnable on this host
> without privileges, and the Debugging part exercises exactly that on the
> `systems-target` VM.

## What you learned

- `mmap` is a 2×2 — file-backed vs anonymous, `MAP_SHARED` vs `MAP_PRIVATE` —
  and `MAP_PRIVATE`'s copy-on-write means your first store to a page forks it:
  the file keeps `SHKV1` while your view says `XXXXX`.
- Visibility and durability are separate: shared mappers see each other's
  stores instantly through common page-cache pages, but only
  `msync(MS_SYNC)` — visible in the strace as one call over the store —
  guarantees the bytes reached disk before success is reported.
- The `ftruncate`-then-`mmap` ritual sizes the mapping, gets zero-filled
  "empty" slots for free, and avoids `SIGBUS` past EOF; `memfd_create` plus
  `F_SEAL_SHRINK`/`F_SEAL_GROW`/`F_SEAL_FUTURE_WRITE` (seals read back as 22)
  makes an anonymous, truncation-proof variant.
- Cross-language interop comes from defining bytes, not structs: stated
  offsets, NUL padding, and a u32 LE assembled bytewise dodge every repr,
  padding, and endianness rule — and `cmp` proving two stores byte-identical
  is the test that format survived three implementations.
- The unsafe boundary is the same fact three ways: Rust makes you write down
  the promises (`unsafe MmapMut::map_mut`), C++ lets a `void*` into shared
  memory look like any other pointer, and Go disguises the mapping as a
  `[]byte` you must never touch after `Munmap`.

Next, **allocators and GC runtimes**: what actually happens between your
`new`/`make`/`Box` and the anonymous mappings this chapter taught you to see.

---

<p><span class="status status--verified">verified</span> — every number and
output excerpt above was produced on the Fedora 44 reference host this
session: the runner printed <code>16-mmap-and-shared-mappings  PASS  PASS
PASS</code> (3 passed, 0 failed, 0 skipped) and <code>verify.lua</code>
logged <code>PASS 48 / FAIL 0</code> for each language; the interop
transcript is real (<code>created demo.kv: 4 slots, 1034 bytes</code>, the
<code>od</code> header <code>53 48 4b 56 31 00 04 00 00 00</code>, Go's
<code>gadd</code> read back as <code>g1</code> by C++, the key-sorted dump
with <code>3/4 slots used</code>); <code>cmp</code> confirmed the Rust-built
and C++-built store files byte-identical; the strace excerpt shows the real
<code>mmap(..., MAP_SHARED, 3, 0)</code> / <code>msync(..., MS_SYNC)</code>
pair; the delay-injected run yielded the live <code>rw-s</code> line from
<code>/proc/&lt;pid&gt;/maps</code>; and both asides (MAP_PRIVATE COW,
memfd_create + sealing with seals 22 and EPERM on a new writable map) ran
live under python3. The "On the lab VM" eBPF callout is unverified as
marked, as are writeback-timing defaults on other kernels.</p>
