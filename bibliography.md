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

<table class="bib bib--books">
  <colgroup><col style="width:62%"><col style="width:24%"><col style="width:14%"></colgroup>
  <thead><tr><th>Title</th><th>Author(s)</th><th>Year</th></tr></thead>
  <tbody>
    <tr><td><a href="https://man7.org/tlpi/" rel="noopener"><em>The Linux Programming Interface</em></a><br><small>No Starch Press. The comprehensive reference for the Linux and POSIX system-call API</small></td><td>Michael Kerrisk</td><td>2010</td></tr>
    <tr><td><a href="https://www.oreilly.com/library/view/advanced-programming-in/9780321638014/" rel="noopener"><em>Advanced Programming in the UNIX Environment</em>, 3rd ed.</a><br><small>Addison-Wesley. Files, processes, signals, and IPC at the UNIX system-call layer</small></td><td>W. Richard Stevens, Stephen A. Rago</td><td>2013</td></tr>
    <tr><td><a href="https://www.oreilly.com/library/view/linux-kernel-development/9780768696974/" rel="noopener"><em>Linux Kernel Development</em>, 3rd ed.</a><br><small>Addison-Wesley. How the kernel schedules, manages memory, and services system calls</small></td><td>Robert Love</td><td>2010</td></tr>
    <tr><td><a href="https://www.brendangregg.com/systems-performance-2nd-edition-book.html" rel="noopener"><em>Systems Performance</em>, 2nd ed.</a><br><small>Addison-Wesley. Methodologies and tools for observing and tuning Linux performance</small></td><td>Brendan Gregg</td><td>2020</td></tr>
  </tbody>
</table>

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
