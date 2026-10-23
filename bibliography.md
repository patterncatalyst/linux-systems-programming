---
title: "Bibliography"
layout: plan
permalink: /bibliography/
description: "The reference works and primary documentation this tutorial's coverage and rigor draw on."
---

This tutorial teaches Linux systems programming in C++23, Go, and Rust against
the kernel's own interfaces. The works below are the field's standard references
and the authoritative documentation it follows. They are reference works, not
sources: no text or code from any of them is reproduced here. Where you want to
go deeper than a chapter does, these are where to go.

| Title | Author(s) | Publisher | Year | Focus |
|---|---|---|---|---|
| [*The Linux Programming Interface*](https://man7.org/tlpi/) | Michael Kerrisk | No Starch Press | 2010 | The comprehensive reference for the Linux and POSIX system-call API |
| [*Advanced Programming in the UNIX Environment*, 3rd ed.](https://www.oreilly.com/library/view/advanced-programming-in/9780321638014/) | W. Richard Stevens, Stephen A. Rago | Addison-Wesley | 2013 | Files, processes, signals, and IPC at the UNIX system-call layer |
| [*Linux Kernel Development*, 3rd ed.](https://www.oreilly.com/library/view/linux-kernel-development/9780768696974/) | Robert Love | Addison-Wesley | 2010 | How the kernel schedules, manages memory, and services system calls |
| [*Systems Performance*, 2nd ed.](https://www.brendangregg.com/systems-performance-2nd-edition-book.html) | Brendan Gregg | Addison-Wesley | 2020 | Methodologies and tools for observing and tuning Linux performance |

## Primary references

The tutorial follows the current specifications and documentation for the
interfaces and tools it uses. These are the authoritative sources to check
against:

- [Linux man-pages project](https://man7.org/linux/man-pages/): the system-call and library-function reference (sections 2 and 3).
- [Linux kernel documentation](https://docs.kernel.org/): the kernel's own docs, including the io_uring, cgroup, and namespaces references.
- [liburing](https://github.com/axboe/liburing): the io_uring user-space library and examples.
- [cppreference](https://en.cppreference.com/) (C++23), [Go documentation](https://go.dev/doc/), and [The Rust Reference](https://doc.rust-lang.org/reference/): the three language standards the examples are written against.

Nothing in these chapters or the runnable examples copies text or code from
these works.
