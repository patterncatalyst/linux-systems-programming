---
title: "Prerequisites and toolchains"
order: 1
part: "Setting Up"
description: "Prepare a Fedora 44 host for systems programming in C++23, Go, and Rust — pinned toolchains, one check script that verifies everything, and a first tri-language example that asks the kernel who you are."
duration: 60 minutes
---

Every chapter in this book ends with something you can run, and almost every
one of them shows the same program three times — in C++23, Go, and Rust. That
only works if all three toolchains on your machine agree with the ones the
book was written against, which is why this chapter is less "install these
packages" and more "here is *why* each pin exists, and here is one script that
tells you whether your host matches." By the end you will have run the book's
first program, `examples/01-hello-syscalls`, in all three languages, and
cross-checked with `strace` that each of them really made the syscalls the
source code claims.

The code is in `examples/01-hello-syscalls/`. `./demo.sh` there builds and
runs all three implementations; its `README.md` covers the run-script contract
every later example reuses.

{% include excalidraw.html
   file="01-toolchain-landscape"
   alt="Three toolchain stacks — GCC/CMake/gdb for C++, go1.26.5/go build/dlv for Go, rustc/cargo/clippy for Rust — each converging through its own demo.sh onto one shared demo.sh runner contract."
   caption="Figure 1.1 — three toolchains, one demo.sh contract shared by every example in the book" %}

## One host, three toolchains

The book targets **Fedora 44** deliberately. Systems programming is the one
domain where "any recent Linux" is not good enough: chapters later in the book
read kernel tracepoints, load eBPF programs, and poke at cgroup files whose
layout shifts between kernel versions. Fedora ships close to upstream — the
reference host here runs kernel `7.1.3-200.fc44` — so what you observe matches
what current kernel documentation describes. It also ships **GCC 16.1.1** as
the system compiler, which matters immediately: the C++ examples use C++23's
`std::println` and `std::expected`, and those need GCC 14 or newer. On Fedora
44 you get that for free; on an LTS distribution you would be building a
compiler first.

Go and Rust are pinned differently, and the difference is worth understanding
because it decides *where* the pin lives:

- **Go** pins inside the module. `examples/01-hello-syscalls/go/go.mod`
  declares `toolchain go1.26.5`, so any reasonably modern `go` binary —
  including Fedora's packaged one — will download and use exactly go1.26.5 on
  first build. The pin travels with the code; your system Go only bootstraps
  it.
- **Rust** pins via `rust-toolchain.toml` (`channel = "1.97.1"`), but that
  file is honored by **rustup**, not by Fedora's packaged `rustc`. Install
  Rust through rustup or the pin silently does nothing — which is exactly the
  kind of drift that later makes an example "work on my machine" only.

C++ has no equivalent in-tree pin mechanism, so it pins by *floor* instead:
the check script requires GCC ≥ 14 and CMake ≥ 3.25 (the first version with
the `CMakePresets.json` v6 schema the examples use). Clang is installed as a
first-class alternate — every C++ example carries `release-clang` /
`debug-clang` presets, because two compilers disagreeing about your code is
the cheapest static analysis you will ever get.

## What `scripts/check-host.sh` verifies — and why

Rather than a page of `dnf install` lines to transcribe, the repo ships
`scripts/check-host.sh`: run it, read the table, fix the `[fail]` rows. Two
design choices in the script are worth noticing before the package list.

First, it deliberately does **not** use `set -e`:

```bash
# Deliberately no -e: failing checks are recorded in the table, not
# fatal on the spot.
set -uo pipefail
```

A host-check script that dies on the first missing tool tells you one problem
per run; this one accumulates every result into a table and exits non-zero
only at the end, so one pass gives you the full shopping list. Second, it
distinguishes **hard requirements** (`fail` — gates the exit code) from
**soft** ones (`warn` — informational). That split encodes what the book
actually needs versus what is merely convenient.

The hard requirements, and the reason each is hard:

- **`gcc`/`g++` ≥ 14, `cmake` ≥ 3.25, `clang`, `ninja`** — the C++ floor
  described above. Ninja is required because every preset in
  `CMakePresets.json` sets `"generator": "Ninja"`.
- **`go`** — any version, because of the toolchain-directive trick; the
  script only *warns* if it is not already 1.26.x, noting the directive
  "auto-downloads go1.26.5 on first build."
- **`rustup` + `rustc` + `cargo`** — a hard fail if rustup is absent, with
  the hint "do not use Fedora's system rust". If rustup is present but on a
  different version, it is only a warning: `rust-toolchain.toml` installs
  1.97.1 on first build anyway.
- **`lua` ≥ 5.4 (or LuaJIT) and `python3`** — Lua drives the per-example
  `verify.lua` checks that CI runs; the harness accepts LuaJIT locally (the
  check script records a warning recommending lua 5.4 for parity with CI).
  Python drives the site's tooling (including the diagram generator that
  produced Figure 1.1).
- **`podman` ≥ 5.0** — the observability chapters run the LGTM stack
  (Grafana, Loki, Tempo, Mimir) as containers; the reference host has 5.8.4.
- **`virsh`, `virt-install`, `qemu-img`, `cloud-localds`, and `/dev/kvm`** —
  the KVM lab that the next chapter provisions. `/dev/kvm` missing means
  virtualization is disabled in firmware, which no package can fix, so the
  script checks the device node itself.
- **`git` and `gh`** — the repo workflow.

The soft rows: **Conan 2.x** (only some C++ demos pull dependencies through
it), **ruby + bundler** (local Jekyll preview of this site only), and the Go
lint/debug trio **golangci-lint, staticcheck, dlv** (CI runs them; locally
they are nice-to-have). Membership in the `libvirt` group is also warn-only —
everything works with `sudo`, it is just tedious without the group.

## The demo contract

Every example in the book is driven the same way, and
`examples/01-hello-syscalls` is where the contract is established. The
top-level `demo.sh` is nothing but a dispatcher:

```bash
case "${1:-all}" in
  cpp|go|rust)
    lang="$1"; shift
    exec "./$lang/demo.sh" "$@"
    ;;
  all)
    for lang in "${langs[@]}"; do "./$lang/demo.sh"; done
    ;;
  build)
    for lang in "${langs[@]}"; do "./$lang/demo.sh" build; done
    ;;
esac
```

Each language directory owns a `demo.sh` with an identical interface:
`./demo.sh` builds then runs, `./demo.sh build` builds only, `./demo.sh run
[args]` runs the built binary. The C++ variant shows the shape:

```bash
build() {
    cmake --preset release
    cmake --build --preset release
}

run() {
    if [[ ! -x "$BIN" ]]; then
        build
    fi
    if [[ -n "${TARGET:-}" ]]; then
        "$REPO_ROOT/scripts/lab/deploy-to-vm.sh" "$TARGET" "$BIN" -- "$@"
    else
        "./$BIN" "$@"
    fi
}
```

The `TARGET` branch is the part that pays off later: set `TARGET` to a lab VM
name and `run` deploys the host-built binary to the guest via
`scripts/lab/deploy-to-vm.sh` instead of running it locally. Privileged
examples in later chapters need exactly this — build on the host, execute on
a disposable kernel — and because the contract is uniform, `TARGET=… ./demo.sh
go run` means the same thing in every chapter. Alongside the demo sits
`verify.lua`, which CI runs once per language: it executes `./demo.sh $LSP_LANG
run` and asserts the output matches the Lua pattern `pid %d+ on Linux`. That
is the book's definition of *verified*: the observable effect, not a clean
compile.

## How the code works

The program itself is three files, one per language, each printing exactly one
line — `pid <PID> on <sysname> <release> (<machine>)` — using two of the
oldest syscalls in the catalog: `getpid(2)` and `uname(2)`.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// hello-syscall: print "pid <PID> on <sysname> <release> (<machine>)" via getpid(2) + uname(2).

#include <cerrno>
#include <cstdio>
#include <expected>
#include <print>
#include <system_error>

#include <sys/utsname.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::expected<utsname, std::error_code> system_info() {
    utsname info{};
    if (::uname(&info) != 0) {
        return std::unexpected(std::error_code{errno, std::system_category()});
    }
    return info;
}

} // namespace

int main() {
    const auto info = system_info();
    if (!info) {
        std::println(stderr, "uname failed: {}", info.error().message());
        return 1;
    }
    std::println("pid {} on {} {} ({})", ::getpid(), info->sysname, info->release, info->machine);
    return 0;
}
```

```go
// hello-syscall: print "pid <PID> on <sysname> <release> (<machine>)".
package main

import (
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

func helloLine() (string, error) {
	var uts unix.Utsname
	if err := unix.Uname(&uts); err != nil {
		return "", fmt.Errorf("uname: %w", err)
	}
	return fmt.Sprintf("pid %d on %s %s (%s)",
		unix.Getpid(),
		unix.ByteSliceToString(uts.Sysname[:]),
		unix.ByteSliceToString(uts.Release[:]),
		unix.ByteSliceToString(uts.Machine[:])), nil
}

func main() {
	line, err := helloLine()
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	fmt.Println(line)
}
```

```rust
use rustix::system::uname;

fn main() {
    let info = uname();
    println!(
        "pid {} on {} {} ({})",
        std::process::id(),
        info.sysname().to_string_lossy(),
        info.release().to_string_lossy(),
        info.machine().to_string_lossy(),
    );
}
```

All three call `uname(2)`, which fills a caller-supplied `utsname` struct with
fixed-size, NUL-terminated byte arrays — the kernel writes into *your* memory;
there is no allocation and no pointer handed back. Each language then has to
answer the same two questions: *how do I get at those bytes?* and *what do I
do if the call fails?* — and the three answers are a preview of the error-
handling policy the whole book runs on.

**C++** wraps the raw call in `system_info()`, which returns
`std::expected<utsname, std::error_code>`. The kernel reports failure as `-1`
plus `errno`; the wrapper immediately converts that into
`std::error_code{errno, std::system_category()}` so the error is a value, not
ambient global state that the next libc call could overwrite. `[[nodiscard]]`
makes ignoring the result a warning, and `main` branches on the `expected`
rather than on `errno`. The `::uname` scope qualifier is not decoration — the
struct and the function are *both* named `uname`, and `::` disambiguates the
global C function from the type. The struct's `char[]` fields print directly
because `std::println` treats them as C strings.

**Go** goes through `golang.org/x/sys/unix` (v0.47.0) — the maintained home
for raw syscall bindings since the standard `syscall` package was frozen.
`unix.Uname` returns an ordinary `error`, which `helloLine` wraps with `%w` so
callers can still `errors.Is`/`errors.As` through the added context. The
`Utsname` fields arrive as `[65]byte` arrays, and `unix.ByteSliceToString`
does the careful conversion: scan to the first NUL, slice, convert — without it
you would print 65 bytes of name plus garbage.

**Rust** uses `rustix` (1.1.4), a safe binding layer that makes syscalls
directly rather than through libc. Note what is *missing*: no `Result`, no
`unwrap`. `rustix::system::uname()` is typed as infallible because on Linux
`uname` cannot fail when handed a valid struct the caller owns — the binding
encodes the manpage's failure contract in the signature. The returned fields
are `CStr`-like views, and `to_string_lossy()` handles the (theoretical)
non-UTF-8 case instead of panicking. Three languages, three defaults: C++
makes you construct the error discipline yourself, Go hands you an error value
and conventions, and Rust moves part of the manpage into the type system.
Chapter 5 turns this comparison into policy.

## Build, run, observe

Check the host first, then run the whole example:

```bash
[host]$ ./scripts/check-host.sh
[host]$ cd examples/01-hello-syscalls && ./demo.sh
```

The check script prints its `[ ok ]`/`[warn]`/`[fail]` table and ends with a
summary line; on the reference host every required check passes. The demo then
builds and runs each language in turn — CMake configures the `release` preset
into `cpp/build/release/`, `go build` resolves go1.26.5 and `x/sys` on first
run, `cargo` installs the pinned 1.97.1 toolchain if needed — and each binary
prints one line of the same shape:

```
pid 2520094 on Linux 7.1.3-200.fc44.x86_64 (x86_64)
```

Three lines, three different PIDs, identical formatting: that is the whole
point. If any language prints something else — or the pattern
`pid %d+ on Linux` fails to match — your toolchain differs from the pin, and
`check-host.sh` will usually tell you where.

## Cross-check: watch the syscalls happen

The source *claims* it calls `uname(2)` and `getpid(2)`. `strace` — which
intercepts every syscall a process makes — lets you verify that independently
of any language runtime (`sudo dnf install -y strace` if you lack it). On the
C++ binary, trimmed to the `uname` call:

```bash
[host]$ strace -f -e trace=uname ./cpp/build/release/app
uname({sysname="Linux", nodename="Lemuria", ...}) = 0
pid 2520274 on Linux 7.1.3-200.fc44.x86_64 (x86_64)
+++ exited with 0 +++
```

One process, one `uname`, exit 0. Now the Go binary, same filter:

```bash
[host]$ strace -f -e trace=uname ./go/bin/app
strace: Process 2520268 attached
strace: Process 2520269 attached
strace: Process 2520270 attached
strace: Process 2520271 attached
[pid 2520267] uname({sysname="Linux", nodename="Lemuria", ...}) = 0
pid 2520267 on Linux 7.1.3-200.fc44.x86_64 (x86_64)
+++ exited with 0 +++
```

Same single `uname`, but four extra threads attached before `main` even ran —
the Go scheduler's worker threads, created with `clone(2)`. Run `strace -c`
on the C++ binary and `strace -cf` on the Go one (the `-f` follows those
threads) and the difference gets numeric: on this host the C++ binary made 64
syscalls total, while the Go binary made 217 across all threads, including
114 `rt_sigaction` calls (the runtime installing its signal handlers) and 4
`clone`s. Neither number is a problem — but it is the first concrete sighting
of a theme that recurs all book long: a language runtime is itself a systems
program, and `strace` does not let it hide.

## CLion (optional)

If you use CLion, opening `examples/01-hello-syscalls/cpp` should auto-import
the `CMakePresets.json` presets (release, debug, asan, and the clang pair) as
build configurations, and the Rust and Go plugins cover the other two
directories. Everything in the book, though, is driven from the terminal — no
step ever requires the IDE.

<p><span class="status status--unverified">unverified</span> — the CLion preset auto-import and plugin flow has not yet been exercised for this repo; confirm the five presets appear and that the Rust plugin picks up <code>rust-toolchain.toml</code>.</p>

## What you learned

- Fedora 44 gives current-kernel observability and a C++23-capable system GCC;
  Go pins itself via the `go.mod` toolchain directive, Rust via
  `rust-toolchain.toml` — but only under rustup.
- `scripts/check-host.sh` separates hard requirements from soft ones and
  reports all failures in one pass instead of dying on the first.
- Every example follows one `demo.sh` contract — `build`, `run`, and
  `TARGET=<vm>` for deploy-to-guest — and is *verified* only by its observable
  output, which `verify.lua` pattern-matches.
- `strace` confirms what a binary actually asks the kernel for, and already
  reveals runtime differences: 64 syscalls for the C++ hello, 217 for Go's.

Next, the **KVM lab**: provisioning the `systems-target` and `systems-peer`
guests so that everything privileged in later chapters runs on a disposable,
snapshotted kernel instead of yours.

---

<p><span class="status status--verified">verified</span> — runner table PASS for cpp, go, and rust on the Fedora 44 host (kernel 7.1.3-200.fc44, GCC 16.1.1, go1.26.5, rustc 1.97.1): each binary printed <code>pid &lt;PID&gt; on Linux 7.1.3-200.fc44.x86_64 (x86_64)</code>, and the strace excerpts above are real trimmed output from those binaries. The CLion section remains unverified as noted.</p>
