---
title: "Prerequisites and toolchains"
order: 1
part: "Setting Up"
description: "Fedora 44 packages, the C++/Go/Rust toolchains, CLion and terminal workflows — and a first tri-language example to prove the setup."
duration: 60 minutes
---

> **Note** — this chapter is a **preview stub**: it exists so you can see the
> site's look and feel, including the multi-language code tabs below. The full
> chapter (package lists, toolchain pins, CLion setup, and the
> `examples/01-hello-syscalls` walkthrough) lands in iteration r03.

Every chapter in this book shows the same program in all three languages.
Click a tab once and the whole site follows you — your choice is remembered
across pages. Here is the flavor of it, with the book's first program: ask the
kernel who you are.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// C++23 — no raw printf, no C-style error handling
#include <expected>
#include <print>
#include <system_error>
#include <sys/utsname.h>
#include <unistd.h>

std::expected<utsname, std::error_code> identify() {
    utsname u{};
    if (uname(&u) == -1)
        return std::unexpected(std::error_code(errno, std::system_category()));
    return u;
}

int main() {
    if (auto u = identify()) {
        std::println("pid {} on {} {}", getpid(), u->sysname, u->release);
        return 0;
    } else {
        std::println(stderr, "uname: {}", u.error().message());
        return 1;
    }
}
```

```go
// Go — errors are values; syscalls go through golang.org/x/sys/unix
package main

import (
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

func identify() (unix.Utsname, error) {
	var u unix.Utsname
	if err := unix.Uname(&u); err != nil {
		return u, fmt.Errorf("uname: %w", err)
	}
	return u, nil
}

func main() {
	u, err := identify()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Printf("pid %d on %s %s\n", os.Getpid(),
		unix.ByteSliceToString(u.Sysname[:]),
		unix.ByteSliceToString(u.Release[:]))
}
```

```rust
// Rust — Result all the way down; rustix gives safe, ergonomic syscalls
use rustix::system::uname;
use std::process;

fn main() {
    let u = uname(); // infallible on Linux: the kernel fills a struct we own
    println!(
        "pid {} on {} {}",
        process::id(),
        u.sysname().to_string_lossy(),
        u.release().to_string_lossy()
    );
}
```

The same program, three worldviews: C++ hands you `errno` and lets you decide
how to dress it (`std::expected` here — exceptions are reserved for subsystem
boundaries); Go wraps the error and returns it up the call chain; Rust's
binding layer has already decided which calls can fail at all. Chapter 5 turns
this observation into the error-handling policy the rest of the book applies.

## What this chapter will cover

- The Fedora 44 package set for all three toolchains, the debuggers
  (gdb, valgrind, delve), the eBPF observation tools (bcc-tools, bpftrace,
  bpftool), and the analysis suites (Cockpit, SystemTap, PCP via Podman).
- Pinned toolchains: GCC (with clang as a first-class alternate), CMake presets
  and Conan 2 for C++; Go 1.26.5; Rust 1.97.1 via rustup.
- CLion with the Rust and Go plugins — and the terminal equivalent for every
  build, run, and debug step in the book.
- `scripts/check-host.sh`, which verifies the whole setup in one shot.

<p><span class="status status--unverified">unverified</span> — preview stub; the runnable example and verification land in r03.</p>
