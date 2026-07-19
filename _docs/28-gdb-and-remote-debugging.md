---
title: "gdb and Remote Debugging"
order: 28
part: "Debugging"
description: "app crashes on purpose, three ways: a real SIGSEGV and a systemd-coredump core in C++, a runtime-caught panic in Go, an Option::unwrap() panic in Rust — and gdb, rust-gdb, coredumpctl, readelf, and objdump read the same crash from four directions, then gdbserver and target remote carry the session from the host to a VM."
duration: "55 minutes"
---

Every debugger so far in this book has been a `println` you had to recompile
to move. This chapter replaces that habit with the real tool: a crash that
already happened, frozen in a core file or a live process, that you can
walk backwards through without changing a line of code. `app` is this
chapter's target — a small pmon-like supervisor that behaves identically in
`run` mode across C++, Go, and Rust, then diverges completely in `crash`
mode, because the *entire point* of this chapter is that "the same bug" does
not mean "the same crash": C++ gets a hardware `SIGSEGV` and a core file,
Go's runtime intercepts the fault and turns it into a panic, and Rust's
`Option::unwrap()` never touches invalid memory at all. One call chain, three
debugging stories.

The code is in `examples/28-gdb-and-remote-debugging/`. `./demo.sh build`
builds all three; each language directory's `demo.sh run [crash]` runs it,
and the top-level `README.md` documents the full reproduction and
verification contract.

{% include excalidraw.html
   file="28-stack-frame-anatomy"
   alt="A column of five stacked frames from a real gdb bt full on app's core file, growing toward lower addresses as they go down: frame 0 std::println&lt;int,int&gt; at print colon 164, the actual crash PC; frame 1 deref at main.cpp:52 showing child equals pid 4242 exit_slot 0x0; frame 2 handle_child at main.cpp:56 with return address 0x4016b2; frame 3 run_supervisor at main.cpp:65 with return address 0x40177c and locals child, guard equals optimized out; frame 4 main at main.cpp:104 with return address 0x400964. Beside it, a DWARF cross-check column shows readelf/objdump decodedline results: main.cpp:52 maps to 0x401680, an exact match to gdb's own breakpoint address; main.cpp:56 maps to 0x4016b2, an exact match to frame 2's return address; main.cpp:65 maps to 0x401772, close to but not exactly frame 3's return address of 0x40177c since that is a post-call return address, not a statement start. A footer note says every frame function is noinline so -O2 cannot fold the call chain away, and ShutdownGuard's destructor never runs because SIGSEGV does not unwind the stack."
   caption="Figure 28.1 — the five real frames gdb names on app's core, cross-checked address for address against the DWARF line table readelf and objdump report independently" %}

> **Tools used** — `gdb` 17.2, `rust-gdb`, `coredumpctl`, `readelf`, `objdump`
> (host); `gdb-gdbserver` (preinstalled on the `systems-target` VM image via
> cloud-init, documented but not exercised live in this local lane — no
> `systems-target` VM was up this session); `dlv` (delve) is referenced for
> contrast but is a soft/optional host tool per `scripts/check-host.sh` and
> was not installed or run here. Everything else appears in
> `scripts/check-host.sh`.

## Reading a crash: core, gdb, and the languages that don't need one

A **core dump** is the kernel's own crash-scene photograph: on an unhandled
fatal signal (`SIGSEGV`, `SIGABRT`, `SIGILL`, ...), the default disposition
is to write the process's entire address space, register state, and open-fd
table to disk before terminating it. On a plain workstation that used to
mean a `core` file dropped in the crashing process's working directory; on
this host, and on any modern Fedora/systemd machine,
`/proc/sys/kernel/core_pattern` instead reads
`|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h %d %F` — the
leading `|` means the kernel *pipes* the core to that program rather than
writing a file itself, so "the crash dropped a core" means
`coredumpctl` has a compressed copy indexed by pid, not that a bare `core`
file appeared anywhere. `coredumpctl info <pid>` confirms one exists and
prints its own compact backtrace; `coredumpctl dump <pid> -o core.app`
extracts it as an ordinary file gdb can open directly.

gdb's job starts where that photograph does. Opened against a binary that
still carries debug info — this chapter's C++ build uses the `release`
CMake preset, which is `RelWithDebInfo` (`-O2 -g`, optimized *and*
debuggable) — gdb reconstructs source-level frames, local variables, and
even C++ types from the DWARF the compiler embedded, not from guesswork.
That last part is what "pretty-printers" buy you: without them, a
`std::string_view` is a two-word struct (`{ _M_str, _M_len }`) and you'd read
its text by hand out of memory; with libstdc++'s printers loaded (they ship
with the distro's gdb and register themselves automatically), the same
value prints as the string itself. Breaking at `parse_mode` and feeding it a
bad argument shows exactly this:

```console
[host]$ gdb --batch -ex 'set pagination off' -ex 'set debuginfod enabled off' \
  -ex 'break parse_mode' -ex 'run bogus-thing' -ex 'print sub' -ex 'whatis sub' \
  ./app
Breakpoint 1, (anonymous namespace)::parse_mode (sub="bogus-thing") at src/main.cpp:77
$1 = "bogus-thing"
type = std::string_view
```

`sub="bogus-thing"` in the frame header and `$1 = "bogus-thing"` are both the
pretty-printer at work — `info pretty-printer` confirms the object file
registering it: `objfile /lib64/libstdc++.so.6 pretty-printers:
libstdc++-v6`. The same mechanism turns a struct reference into a readable
value: breaking at `deref` and printing `child` (a `const ChildRecord&`)
gives `$1 = (const (anonymous namespace)::ChildRecord &) @0x7fff...: {pid =
4242, exit_slot = 0x0}` — field names, not offsets. And because gdb reads
target memory rather than executing your code's checks, you can trigger the
exact fault *before* the program does: `print *child.exit_slot` at that
breakpoint returns `Cannot access memory at address 0x0` — gdb hit the same
null pointer the CPU is about to, just without a signal.

`rust-gdb` is the same gdb binary wrapped with a small Python autoload script
that ships with `rustc` and teaches it Rust's type layout — enum
discriminants, `Option`, slices. Breaking at `app::deref` on the Rust build
and printing the child record shows the payoff directly:

```console
[host]$ rust-gdb --batch -ex 'break app::deref' -ex 'run crash' -ex 'print *child' ./app
Breakpoint 1, app::deref (child=0x7fff...) at src/main.rs:38
$1 = app::ChildRecord {pid: 4242, exit_slot: core::option::Option<i32>::None}
```

`exit_slot: core::option::Option<i32>::None` is the variant *name*, not the
tag byte a plain gdb without the Rust autoload would show. Plain `gdb` (no
`rust-gdb` wrapper) can still open the same binary — Rust's DWARF is real
DWARF — but you lose this layout-aware rendering and read raw enum
representations by hand instead.

Go breaks this whole story on purpose. There is no core file to open in the
first place: the Go runtime installs its own handler for the fault-causing
signals and, on recognizing that the fault came from ordinary user code (not
a bug inside the runtime itself), converts it into a normal — if
unrecovered — `panic`, never re-raising `SIGSEGV` with the default
disposition that would dump core. `gdb` *can* still attach to a running Go
binary (Fedora's gdb ships "Loading Go Runtime support" goroutine helpers),
and on this exact crash it stops cleanly at the fault and even walks a
correct four-frame backtrace, because the whole program is one goroutine
blocking on one OS thread when it dies:

```console
[host]$ gdb --batch -ex 'run crash' -ex bt --args ./app crash
Thread 1 "app" received signal SIGSEGV, Segmentation fault.
0x000000000049fd45 in main.deref (child=...) at main.go:42
#0  main.deref (child=...) at main.go:42
#1  main.handleChild (child=...) at main.go:47
#2  main.runSupervisor (injectBug=true, ~r0=<optimized out>) at main.go:56
#3  main.main () at main.go:79
```

That result is flattering and it hides the real problem. gdb's thread list
maps to **OS threads** (the `M`s in Go's `G`/`M`/`P` scheduler), not to
**goroutines** (`G`s) — a goroutine that is parked on a channel, blocked in a
`select`, or simply not currently scheduled onto any `M` has no OS thread for
gdb to show you, and gdb has no notion of "goroutine 41" as a selectable
frame context the way it has "thread 3". The moment your program has more
than a handful of concurrent goroutines, `info threads` and `bt` stop mapping
onto your program's logical concurrency at all. Go's own tooling doesn't have
this blind spot because it was built with the scheduler in mind: `dlv exec
./app`, then `goroutines` to list every logical goroutine (parked or
running) and `goroutine <n> bt` to backtrace one specifically — `delve`
walks the runtime's own goroutine table, not the OS thread list. This
host does not have `dlv` installed (it is a soft, optional entry in
`scripts/check-host.sh`, not a hard requirement), so that specific
`goroutines`/`goroutine <n> bt` session is <span class="status
status--unverified">unverified</span> here — the point stands on the gdb
transcript above being accidentally complete only because this crash never
gets past goroutine 1.

Rust needs neither gdb nor a core for this particular bug class either.
`.unwrap()` on a `None` calls `panic!` directly — no signal is raised at
all, so there is nothing for gdb to catch even if you attached. The
idiomatic tool is `RUST_BACKTRACE=1`, which walks the unwinder's own frame
records and prints every one by name, entirely in userspace:

```console
[host]$ RUST_BACKTRACE=1 ./app crash
thread 'main' panicked at src/main.rs:38:25:
called `Option::unwrap()` on a `None` value
   5: app::deref
             at ./src/main.rs:38:25
   6: app::handle_child
             at ./src/main.rs:44:5
   7: app::run_supervisor
             at ./src/main.rs:59:9
   8: app::main
             at ./src/main.rs:102:16
exit=101
```

Without `RUST_BACKTRACE=1` the same panic and the same `exit=101` still
happen, but the frame names are gone — the backtrace is opt-in, the crash
is not.

## Remote debugging: gdbserver, target remote, and CLion

Everything above assumes gdb and the crashing binary share a filesystem.
That stops being true the moment the thing you need to debug lives on a
different machine — this book's `systems-target` lab VM, a container, a
device you cannot install a full toolchain on. `gdbserver` solves this by
splitting gdb in two: a thin stub (`gdbserver`) runs *next to* the target
process and does nothing but single-step it, read its memory, and set
breakpoints on request; your full gdb runs on the host, keeps all its
symbol tables and pretty-printers locally, and drives the stub over gdb's
**remote serial protocol**, typically carried over TCP. Only the protocol's
small binary command set crosses the wire — source, DWARF, and the
pretty-printer scripts never leave the host.

{% include excalidraw.html
   file="28-remote-debug-topology"
   alt="Two stacked bands. The upper host band, marked verified this session, shows the local core-dump path: app crash producing a SIGSEGV, an arrow into systemd-coredump, coredumpctl dump extracting core.app, and gdb app core.app doing a full bt. Beside it, a second host-side node reads gdb (client), target remote systems-target:2345, with an amber bidirectional arrow labeled gdb remote serial protocol, TCP:2345 descending into the lower systems-target VM band, marked documented, not run this session. That band holds a gdbserver :2345 --once app crash node feeding an app (crash target) child process node below it, plus a dashed ghost box off to the side reading CLion, Remote GDB Server run config, unverified. A footer note says gdbserver is preinstalled on the lab VM image via cloud-init but not on this host, and no systems-target VM was up this session."
   caption="Figure 28.2 — the local core-dump path this chapter's evidence is built on, beside the gdbserver/target-remote/CLion path documented for the systems-target VM" %}

The session looks like this in practice:

```console
[vm]$ gdbserver --once :2345 ./app crash
Process ./app created; pid = 4183
Listening on port 2345
```

```console
[host]$ gdb ./cpp/build/release/app
(gdb) target remote systems-target:2345
Remote debugging using systems-target:2345
(gdb) continue
```

`--once` tells `gdbserver` to serve exactly one debug session and exit
afterward — the right default for a scripted reproduction, since a stray
`gdbserver` left listening on a lab VM is a debug backdoor with no
authentication. `target remote` is the one gdb command that changes
everything downstream of it: every `break`, `run` (really `continue`, since
the process is already started), `print`, and `bt` you already know behaves
identically, dispatched over the wire instead of a local `ptrace(2)`. The
binary path you pass to gdb on the host must be the *unstripped*, debug-info
copy — gdbserver on the VM only needs the raw executable, gdb on the host
supplies all the symbols.

This exact transcript is <span class="status
status--unverified">unverified</span> in this chapter: no `systems-target`
VM was up during this session (`virsh list --all` shows no defined guests
in this local lane), so the local core-dump path above carries this
chapter's verified evidence, and the remote path is documented from the lab
image's cloud-init (`gdb-gdbserver` is provisioned there) and the command
shapes gdb itself specifies, not from a live run. Confirm it for real by
bringing the lab up (`scripts/lab/lab-up.sh`) and repeating the two
transcripts above against the deployed binary.

CLion's remote debug story is the same protocol wearing an IDE: a **"Remote
GDB Server" run configuration** points at a `target-name` (an SSH-reachable
host, here `systems-target`), a path to the *local* copy of the binary with
debug info, and a `gdbserver` command line to run on that target — CLion
then opens the SSH session, launches `gdbserver` remotely, and drives it
with its own bundled gdb, giving you the IDE's breakpoint gutter and
variable inspector over the identical remote-serial link. This
configuration is <span class="status status--unverified">unverified</span>
in this chapter — it is real, documented CLion functionality (Settings →
Build, Execution, Deployment → Toolchains → a remote toolchain over SSH,
then a Remote GDB Server run configuration referencing it), not something
run here.

## How the code works

`app` walks the identical four-function call chain in every language —
`main -> run_supervisor -> handle_child -> deref` — and it is written to
resist the optimizer erasing that chain: every frame function is marked
noinline (`[[gnu::noinline]]` / `//go:noinline` / `#[inline(never)]`), and
all three builds keep debug info in an otherwise optimized binary (C++'s
`release` preset is `RelWithDebInfo`; Go's toolchain always emits DWARF;
Rust's `Cargo.toml` sets `[profile.release] debug = true`). Rust needed one
more trick: `handle_child`'s call to `deref` is its last statement, so
without a following `std::hint::black_box(())`, LLVM turned it into a tail
call and reused `handle_child`'s own frame for `deref` — erasing
`handle_child` from every backtrace. Verified directly: removing the
`black_box` line made `RUST_BACKTRACE=1`'s trace jump straight from
`app::deref` to `app::run_supervisor`.

The data shape is one `ChildRecord` — a pid plus a slot for the child's exit
status that a supervisor's reaper is supposed to fill in before anything
reads it. In `run` mode that slot is populated first; in `crash` mode
`handle_child` runs *as if it fired before the reaper got there* — the
exact race a real signal-driven supervisor (this book's `pmon`, from Part 3
onward) can hit if a `SIGCHLD` notification and the code consuming it get
out of step. Each language expresses "not populated yet" the way its type
system prefers, and that choice is the whole reason the crash differs:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
struct ChildRecord {
    int pid = 0;
    const int* exit_slot = nullptr;
};

[[gnu::noinline]] void deref(const ChildRecord& child) {
    // BUG: no null check before the read. In "crash" mode exit_slot is
    // nullptr because handle_child ran before the reaper recorded the exit
    // status -- exactly the kind of race a real supervisor can hit.
    std::println("pmon: child {} exited status={}", child.pid,
                 *child.exit_slot);
}

[[gnu::noinline]] void handle_child(const ChildRecord& child) { deref(child); }
```

```go
type childRecord struct {
	pid      int
	exitSlot *int
}

//go:noinline
func deref(child childRecord) {
	// BUG: no nil check before the read. In "crash" mode exitSlot is nil
	// because handleChild ran before the reaper recorded the exit status --
	// exactly the kind of race a real supervisor can hit.
	fmt.Printf("pmon: child %d exited status=%d\n", child.pid, *child.exitSlot)
}

//go:noinline
func handleChild(child childRecord) {
	deref(child)
}
```

```rust
#[derive(Clone, Copy)]
struct ChildRecord {
    pid: u32,
    exit_slot: Option<i32>,
}

#[inline(never)]
fn deref(child: &ChildRecord) {
    // BUG: unwrap() with no check. In "crash" mode exit_slot is None because
    // handle_child ran before the reaper recorded the exit status -- exactly
    // the kind of race a real supervisor can hit.
    println!(
        "pmon: child {} exited status={}",
        child.pid,
        child.exit_slot.unwrap()
    );
}

#[inline(never)]
fn handle_child(child: &ChildRecord) {
    deref(child);
    // A no-op after the call: without it, `deref` is the last thing this
    // function does and the optimizer turns the call into a tail jump,
    // reusing this frame and erasing `handle_child` from the backtrace.
    std::hint::black_box(());
}
```

C++'s `const int* exit_slot = nullptr` is the raw pointer the whole bug class
is named after — `*child.exit_slot` reads through address `0x0`, the MMU
raises a page fault on an unmapped page, and the kernel delivers `SIGSEGV`
with no C++-level participation at all. Go's `*int` is the same shape at the
machine level, but the runtime stands between the fault and the process:
its signal handler recognizes a `nil`-pointer-shaped fault from user code
and converts it into `runtime.Error`, which surfaces as an ordinary panic.
Rust rejected the raw-pointer option outright — safe Rust has no way to
leave a pointer dangling like this — so the equivalent "not filled in yet"
state is `Option<i32>::None`, and `.unwrap()` is an explicit, checked
operation that panics on `None` rather than reading through anything.
`run_supervisor` is the wiring in all three: it builds one `ChildRecord`,
chooses `nullptr`/`nil`/`None` versus a real value based on `inject_bug`, and
calls `handle_child` either way — the crash path and the clean path are
*the same code*, differing only in the data, which is the point: this bug
is not a different code path, it's a value that should have arrived and
didn't.

## Errors, three ways

The subcommand parsing is this book's usual three-way split and is
identical output across languages: no argument or an unrecognized one
prints the two-line `usage: app run` / `app crash` block to stderr and
exits 2. `app run` prints three fixed lines and exits 0 in every language —
byte-identical, dual-use, unremarkable. `app crash` is where the three
languages part ways, and the table is the chapter in miniature:

| | mechanism | exit | core? | names the frame |
|---|---|---|---|---|
| C++ | null pointer deref → `SIGSEGV` | 139 | yes, via `systemd-coredump` | `gdb --batch -ex run -ex bt` |
| Go | nil deref → runtime catches `SIGSEGV`, converts to panic | 2 | no | the goroutine trace in the panic output |
| Rust | `Option::unwrap()` on `None` → panic | 101 | no | `RUST_BACKTRACE=1` |

One more thing worth reading off the C++ trace directly: the `ShutdownGuard`
RAII type in `run_supervisor`'s clean path prints "supervisor exiting
cleanly" from its destructor, and that line never appears on the crash path
— confirmed in every transcript above. RAII buys you cleanup on every
*C++-level* exit (`return`, a thrown exception, `std::exit` via a registered
handler); it buys you nothing against a signal, because a signal does not
unwind the stack — the kernel simply stops the process where it stands.

## What each language's tooling gives you

This chapter is tooling-heavy rather than concurrency-heavy, so the lens
here is what each debugger sees, not a data race: gdb sees raw memory,
registers, and DWARF — it is precise but blind to any concept a language
runtime invented for itself. That is exactly why it is the *only* tool for
the C++ crash (there is no higher-level runtime to ask) and only an
*accidentally* adequate one for Go (it happened to catch this crash while
the program was still single-goroutine) — `delve` exists specifically to
close that gap by understanding the `G`/`M`/`P` scheduler gdb has no model
of. Rust splits the difference: `rust-gdb` gives gdb's raw power *plus*
enum/`Option`-aware rendering, but for an ordinary panic-and-unwind exit
(no signal, no fault), the unwinder's own `RUST_BACKTRACE=1` output is
faster to reach for and needs no debugger session at all.

## Build, run, observe

```console
[host]$ cd examples/28-gdb-and-remote-debugging && ./demo.sh build
```

```console
[host]$ ./cpp/build/release/app run
pmon: supervisor starting
pmon: child 4242 exited status=0
pmon: supervisor exiting cleanly
```

Now reproduce the crash and pull the core by hand, exactly as `verify.lua`
does it:

```console
[host]$ ws=$(mktemp -d); cd "$ws"
[host]$ ( ulimit -c unlimited; /path/to/cpp/build/release/app crash & echo $! >pid; wait $! )
pmon: supervisor starting
[host]$ coredumpctl info "$(cat pid)"
Signal: 11 (SEGV)
Storage: /var/lib/systemd/coredump/core.app.1000...zst (present)
Stack trace of thread 2906298:
  #0  ... _ZSt7printlnIJRKiS1_EEvSt19basic_format_stringIcJDpNSt13type_identityIT_E4typeEEEDpOS4_ (app + 0x169a)
  #1  ... run_supervisor (app + 0x177c)
  #2  ... main (app + 0x964)
```

Notice `coredumpctl`'s own quick trace, which is itself worth reading
carefully: it names only three frames — `println`'s formatting internals,
`run_supervisor`, `main` — silently skipping `deref` and `handle_child`.
That trace is a lightweight summary, not a full DWARF-driven unwind; it is
*not* the tool to trust for a real backtrace, and gdb's full walk against
the extracted core proves it:

```console
[host]$ coredumpctl dump "$(cat pid)" -o core.app
[host]$ gdb --batch -ex 'set debuginfod enabled off' -ex 'bt full' \
    /path/to/cpp/build/release/app core.app
#0  0x000000000040169a in std::println<int const&, int const&> (__fmt=...) at /usr/include/c++/16/print:164
#1  (anonymous namespace)::deref (child=...) at src/main.cpp:52
#2  0x00000000004016b2 in (anonymous namespace)::handle_child (child=...) at src/main.cpp:56
#3  0x000000000040177c in (anonymous namespace)::run_supervisor (inject_bug=true) at src/main.cpp:65
        child = {pid = 4242, exit_slot = 0x0}
#4  0x0000000000400964 in main (argc=<optimized out>, argv=<optimized out>) at src/main.cpp:104
```

All five frames, `deref` and `handle_child` included, with the exact
`exit_slot = 0x0` that caused the fault sitting right there in frame #3's
locals. The runner agrees end to end: `python3 scripts/test-all-examples.py
--only 28-gdb-and-remote-debugging` reports `PASS PASS PASS`, and each
language's `verify.lua` run separately confirms its own crash contract —
`cpp`'s scripted `gdb --batch -ex run -ex bt` names `deref`,
`handle_child`, and `run_supervisor`; `go`'s exit code is 2 with the
standard nil-deref panic text; `rust`'s exit code is 101 with
`RUST_BACKTRACE=1` naming all four Rust frames and no frame names at all
without it.

## Cross-check: gdb's addresses against the DWARF line table directly

Every address gdb reported above is derived from the same `.debug_line`
section `readelf` and `objdump` can decode independently, with no gdb
involved at all — if they agree, gdb isn't inventing anything. `readelf -S`
first confirms the binary actually carries full DWARF (it is optimized, not
stripped):

```console
[host]$ readelf -S ./app | grep -i debug
  [31] .debug_aranges    ...
  [32] .debug_info       ...
  [34] .debug_line       ...
  [35] .debug_str        ...
```

`objdump --dwarf=decodedline` decodes that section into source-line ->
address rows directly:

```console
[host]$ objdump --dwarf=decodedline ./app | grep -E "main.cpp\s+(52|56|65)\b"
main.cpp    52    0x401680
main.cpp    56    0x4016b0
main.cpp    56    0x4016b2
main.cpp    65    0x401772
```

Two of these match gdb's own numbers exactly, independently derived: gdb's
breakpoint on line 52 (`Breakpoint 1 at 0x401680: file .../main.cpp, line
52`) is the identical address `objdump` reports for that line, and the core
backtrace's frame #2 return address (`0x00000000004016b2`) is the identical
address `objdump` reports for line 56. Line 65's row (`0x401772`) is close
to but not identical to frame #3's return address (`0x40177c`) — expected,
since a frame's *return* address sits a few bytes past a `call`
instruction's own encoding, not at the statement's first byte the way a
breakpoint address does. `readelf -n` supplies one more independent
identity check, the build ID, which ties this exact binary to this exact
debug-info walk:

```console
[host]$ readelf -n ./app | grep "Build ID"
    Build ID: 6c7d2d0445c2e75e68424a4c7c2564d2902c6f12
```

`readelf -S` on the Go and Rust binaries confirms the same discipline holds
across all three languages: Go's toolchain always emits a `.debug_info` /
`.debug_line` pair (Go tooling doesn't offer a way to omit DWARF from a
default build), and Rust's release binary carries the same sections because
of this crate's `[profile.release] debug = true` — three different
compilers, one DWARF-based line-number contract that `readelf`, `objdump`,
and gdb all read the same way.

## What you learned

- A core dump on a modern systemd host is not a bare `core` file — it's a
  `systemd-coredump` entry `coredumpctl` finds by pid and extracts on
  request; opening that extracted core in gdb (`gdb app core.app`) gives a
  full DWARF-driven backtrace that a lightweight summary trace (like
  `coredumpctl info`'s own) can silently omit frames from.
- gdb's pretty-printers turn `std::string_view`/`ChildRecord`/etc. into
  readable values instead of raw offsets, and `rust-gdb` extends the same
  idea to Rust's `Option`/enum layout (`exit_slot:
  core::option::Option<i32>::None`, not a bare discriminant byte).
- gdb's thread model is OS threads, not goroutines — it can look complete on
  a single-goroutine crash and still be blind to the concurrency that
  matters; `delve`'s `goroutines`/`goroutine <n> bt` exists specifically to
  close that gap.
- `gdbserver` + `target remote` split gdb across a network boundary without
  losing any capability — only the remote serial protocol crosses the wire,
  every symbol and pretty-printer stays on the host — and `readelf`/`objdump`
  on `.debug_line` are the independent cross-check that confirms gdb's own
  addresses rather than trusting them blind.

Next, **valgrind, sanitizers, and Miri**: three very different ways of
catching the memory bugs a debugger only shows you *after* they've already
happened.

---

<p><span class="status status--verified">verified</span> — every transcript
above except the two spans marked <span class="status
status--unverified">unverified</span> (the `systems-target` gdbserver/target
remote session, the CLion remote-debug configuration, and delve's
`goroutines`/`goroutine <n> bt`) was produced on the Fedora 44 reference host
(kernel 7.1.3-200.fc44, gdb 17.2, gcc 16.1.1, go1.26.5, rustc 1.97.1, readelf/
objdump 2.46.1) this session: <code>app crash</code> under
<code>ulimit -c unlimited</code> exited 139 and <code>coredumpctl</code>
recorded and extracted a real core; a scripted
<code>gdb --batch -ex run -ex bt</code> and a separate <code>gdb ... bt
full</code> against that extracted core both named
<code>deref</code>/<code>handle_child</code>/<code>run_supervisor</code>/<code>main</code>,
with frame addresses <code>0x4016b2</code> (handle_child) and
<code>0x400964</code> (main); <code>rust-gdb</code> broke at
<code>app::deref</code> and printed
<code>ChildRecord {pid: 4242, exit_slot: core::option::Option&lt;i32&gt;::None}</code>;
Go's <code>./app crash</code> exited 2 with the standard nil-pointer-deref
panic text and a goroutine trace naming <code>main.deref</code>,
<code>main.handleChild</code>, <code>main.runSupervisor</code>; a direct
<code>gdb ... bt</code> on the running Go binary independently named the
same four frames; Rust's <code>RUST_BACKTRACE=1 ./app crash</code> exited
101 and named <code>app::deref</code> through <code>app::main</code>, while
a bare <code>./app crash</code> exited 101 with no frame names;
<code>readelf -S</code> confirmed <code>.debug_info</code>/<code>.debug_line</code>
present and unstripped in all three binaries; <code>objdump
--dwarf=decodedline</code> reported <code>main.cpp:52 -&gt; 0x401680</code>
(matching gdb's own breakpoint address) and <code>main.cpp:56 -&gt;
0x4016b2</code> (matching the core backtrace's frame #2 return address)
exactly; and the runner printed
<code>28-gdb-and-remote-debugging  PASS  PASS  PASS</code>. The gdbserver/
target-remote path is documented from the lab VM's cloud-init
(<code>gdb-gdbserver</code> is provisioned there) and gdb's own command
semantics, not from a live session — confirm it by bringing
<code>systems-target</code> up and repeating the two-line transcript in
"Remote debugging" against the deployed binary.</p>
