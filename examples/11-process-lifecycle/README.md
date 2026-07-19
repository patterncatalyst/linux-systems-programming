# 11-process-lifecycle — pmon

A minimal process monitor, written three times — C++23, Go, and Rust — for
the process lifecycle chapter: spawn a command, wait for it, and report the
two things a supervisor always needs, *how the child died* and *what it cost*.

```
pmon run -- CMD [ARGS...]
```

## Behavior contract (identical in all three languages)

| Outcome | Output | Exit |
| --- | --- | --- |
| Child exits `s` | stdout: `pmon: pid <p> exited status <s>` + rusage line | `s` |
| Child killed by signal `n` | stdout: `pmon: pid <p> killed by signal <n> (<NAME>)` + rusage line | `128+n` |
| exec fails | stderr: `pmon: exec <cmd>: <reason>` (no report lines) | 127 |
| Bad usage | stderr: `usage: pmon run -- CMD [ARGS...]` | 2 |

The rusage line has one fixed shape (milliseconds always three digits):

```
pmon: rusage maxrss=<kb>KB user=<s>.<ms>s sys=<s>.<ms>s wall=<ms>ms
```

Sample session (any of the three binaries):

```
$ pmon run -- sh -c 'kill -TERM $$'
pmon: pid 181042 killed by signal 15 (TERM)
pmon: rusage maxrss=3864KB user=0.000s sys=0.001s wall=1ms
$ echo $?
143
$ pmon run -- ./nonexistent
pmon: exec ./nonexistent: No such file or directory
$ echo $?
127
```

Design decisions shared by all three:

- **The exit code mirrors the child** — the shell convention (`status`, or
  `128+signal`, or `127` for "could not exec"), so pmon composes with `&&`,
  `make`, and CI the same way the shell itself would.
- **Signal names come from a hand-rolled table** (`15 -> TERM`, `9 -> KILL`,
  ..., `SIG<n>` beyond 31), not `strsignal(3)`, whose text is
  locale-dependent prose ("Terminated") and differs across runtimes.
- **`<reason>` is `strerror(3)`-shaped** (`No such file or directory`). Go's
  errno strings are lowercase, so the first letter is capitalized, and a PATH
  miss (`exec.ErrNotFound`) is rendered as ENOENT — exactly what `execvp(3)`
  reports in the other two.
- **Child stdio is inherited**, so the child's own output interleaves
  naturally and the report lines land after the wait.

### How each language spells the same lifecycle

- **C++23** (`cpp/`): the chapter's two-step model spelled out —
  `fork(2)` + `execvp(3)` in the child, `waitpid(2)` +
  `getrusage(RUSAGE_CHILDREN)` in the parent (`posix_spawn(3)` is discussed
  in the chapter; fork is used here for teaching). Exec failure travels over
  a self-pipe: both ends `O_CLOEXEC`, so a successful exec closes the child's
  copy and the parent reads EOF, while a failed exec ships `errno` up before
  `_exit(127)`. A move-only RAII `Fd` owns each pipe end; fallible steps
  return `std::expected`; output is `std::println`.
- **Go** (`go/`): `os/exec` — whose `Start` performs the same
  fork/exec-with-status-pipe dance internally, surfacing exec failure as an
  error before any child ran. The wait-side facts come from
  `os.ProcessState`: `Sys()` is the raw `syscall.WaitStatus`, `SysUsage()`
  the `*syscall.Rusage` that `wait4(2)` filled in for exactly this child.
  Errors are wrapped (`fmt.Errorf` + `%w`) and inspected with
  `errors.As`/`errors.Is`.
- **Rust 2024** (`rust/`): `std::process::Command::spawn` (again the same
  CLOEXEC status-pipe trick under the hood, hence `Err` on exec failure),
  then `wait4(2)` via `libc` (re-exported by nix) so a single call yields
  both the status and the rusage; `nix::sys::wait::WaitStatus::from_raw`
  decodes the raw status and `nix::errno::Errno::desc` renders the
  `strerror(3)` text. The wait wrapper is `io::Result` + `?`, with EINTR
  retried.

One semantic wrinkle worth noticing: C++ asks for `RUSAGE_CHILDREN` (all
waited-for children — equivalent here because there is exactly one), while Go
and Rust read the per-child rusage that `wait4(2)` returns. Same numbers, two
kernel interfaces.

## The demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary with `args`
  (with env `TARGET` set, deploys to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally)
- `./demo.sh` — build, then a local walkthrough of the four child fates:
  clean exit, `exit 42`, death by SIGTERM (exit 143), exec failure (127)

The top-level `demo.sh` dispatches: `./demo.sh cpp run -- /bin/true`,
`./demo.sh all`, `./demo.sh build`.

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts observable
behavior, not exit-0:

- `/bin/true` / `/bin/false` / `sh -c 'exit 42'` report the status and
  mirror it in pmon's own exit code;
- the pid in the fate line is the *real* child pid — the child echoes `$$`
  and the two must agree;
- `kill -TERM $$` reports `killed by signal 15 (TERM)` with exit 143,
  `kill -KILL $$` reports `signal 9 (KILL)` with exit 137;
- `./nonexistent` exits 127 with `pmon: exec ./nonexistent: No such file or
  directory` and *no* fate or rusage lines;
- the rusage line matches the exact shape, wall time covers a real 200 ms
  sleep, maxrss is nonzero, and a shell busy loop registers child user CPU;
- no arguments prints the usage line and exits 2.
