# 28-gdb-and-remote-debugging — bugfarm scenario 1: a crashing pmon

Chapter 28 is the first of the "bugfarm" examples: `app` is deliberately
dual-use. `app run` is a normal, well-behaved program. `app crash` is a
debugging TARGET — it walks the identical three/four-frame call chain but
feeds it a value that was never filled in, and goes straight into the bug
class this chapter is about: reading through a pointer/reference/`Option`
that a concurrent-looking event handler assumed — wrongly — was already
populated.

```
app run                                          # normal operation, exit 0
app crash                                         # deliberate crash, debugging target
```

## Behavior

Both subcommands walk the same call chain:

```
main -> run_supervisor -> handle_child -> deref
```

`run_supervisor` fakes a supervisor that just reaped child pid 4242. In
`run` mode the reaper filled in the child's exit-status slot before
`handle_child` looks at it. In `crash` mode, `handle_child` runs as if it
fired *before* the reaper got there — the slot was never populated — which
is exactly the shape of race a real signal-driven supervisor (see chapter
12's `pmon`) can hit if a `SIGCHLD` notification and the code that consumes
it get out of step.

- `app run` prints:
  ```
  pmon: supervisor starting
  pmon: child 4242 exited status=0
  pmon: supervisor exiting cleanly
  ```
  and exits 0.
- `app crash` prints only `pmon: supervisor starting`, then hits the bug.
  **What happens next is different in every language — that difference is
  the point of this chapter:**

| | mechanism | exit | core? | name the frame with |
|---|---|---|---|---|
| C++ | null pointer deref → **SIGSEGV** | 139 | yes (via systemd-coredump) | `gdb --batch -ex run -ex bt` |
| Go | nil pointer deref → runtime catches SIGSEGV, converts to **panic** | 2 | no | the goroutine trace in the panic output |
| Rust | `Option::unwrap()` on `None` → **panic** | 101 | no | `RUST_BACKTRACE=1` |

- No/unknown subcommand prints a two-line usage block on stderr and exits 2
  (identical across all three).

### Why C++ gets a core and Go/Rust don't

C++ has no runtime standing between your bug and the kernel: a null-pointer
read simply generates a hardware page fault, the kernel delivers `SIGSEGV`,
and the default disposition for an unhandled `SIGSEGV` is "dump core, then
terminate." Go and Rust both interpose a runtime between your code and the
raw fault. Go's runtime installs its own `SIGSEGV` handler, recognizes the
fault came from ordinary code (not from inside the runtime itself), and
converts it into a normal — if unrecovered — panic; there is no
`sigaction(SA_RESETHAND)` back to the default disposition, so no core.
Rust's `.unwrap()` calls `panic!` directly: no signal is even involved,
just the ordinary unwind-and-abort (here, unwind-and-`exit(101)`) machinery.
`gdb` still attaches and runs both binaries fine, but there is no signal to
catch — asking gdb for a backtrace after the fact gets you nothing useful,
which is why the Go/Rust checks below read the runtime's own trace instead.

One more thing worth noticing once you're inside gdb on the C++ build: the
`ShutdownGuard` RAII type in `cpp/src/main.cpp`, whose destructor prints the
"exiting cleanly" line, never fires on the crash path. RAII buys you
cleanup on every C++-level exit (`return`, an exception, `std::exit` via a
`terminate` handler) — it buys you nothing against a hardware fault, because
a signal doesn't unwind the stack. Frame `#0` in the release backtrace below
even lands a level *inside* `std::println`'s own formatting machinery,
rather than exactly on `*child.exit_slot`: at `-O2`, forming the reference
argument doesn't fault, only reading through it during formatting does.

## Demo contract

Each language directory has a `demo.sh` with the book-wide interface:

- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary (builds first if needed)
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh` instead of running locally.

All three builds keep debug info in an otherwise optimized binary: the C++
`release` CMake preset is `RelWithDebInfo` (`-O2 -g`), Go's toolchain always
emits DWARF, and Rust's `Cargo.toml` sets `[profile.release] debug = true`.
Every frame function (`deref`, `handle_child`, `run_supervisor`) is marked
noinline (`[[gnu::noinline]]` / `//go:noinline` / `#[inline(never)]`) so
optimization can't fold the call chain away — Rust's `handle_child` also
needs a `std::hint::black_box(())` after the call, or the optimizer turns it
into a tail call and erases its own frame from the backtrace.

Try it:

```sh
./demo.sh build
./demo.sh cpp run crash          # exit 139, no output about "exiting cleanly"
./demo.sh go  run crash          # exit 2, a goroutine trace
RUST_BACKTRACE=1 ./demo.sh rust run crash   # exit 101, a frame-named backtrace
```

## Reproducing the debugging session by hand

```sh
cd cpp && cmake --preset release && cmake --build --preset release

# a) it crashes, and drops a core (this host routes core_pattern through
#    systemd-coredump, so "the core file" means coredumpctl's copy, not a
#    bare `core` in the cwd):
ws=$(mktemp -d); cd "$ws"
( ulimit -c unlimited; /path/to/cpp/build/release/app crash & echo $! >pid; wait $! )
coredumpctl info "$(cat pid)"                 # confirms SIGSEGV + a stored core
coredumpctl dump "$(cat pid)" -o core.app      # extracts it as a real file

# b) a scripted gdb session names the frame:
gdb --batch -ex 'set pagination off' -ex 'set debuginfod enabled off' \
    -ex run -ex bt --args /path/to/cpp/build/release/app crash
#   ...
#   Program received signal SIGSEGV, Segmentation fault.
#   #1  (anonymous namespace)::deref (child=...) at src/main.cpp:52
#   #2  (anonymous namespace)::handle_child (child=...) at src/main.cpp:56
#   #3  (anonymous namespace)::run_supervisor (inject_bug=true) at src/main.cpp:65
#   #4  main (argc=..., argv=...) at src/main.cpp:104
```

```sh
cd go && go build -o bin/app .
./bin/app crash
#   panic: runtime error: invalid memory address or nil pointer dereference
#   [signal SIGSEGV: segmentation violation code=0x1 addr=0x0 pc=...]
#   goroutine 1 [running]:
#   main.deref(...)
#   main.handleChild(...)
#   main.runSupervisor(0x1)
#   main.main()
```

```sh
cd rust && cargo build --release
RUST_BACKTRACE=1 ./target/release/app crash
#   thread 'main' panicked at src/main.rs:38:25:
#   called `Option::unwrap()` on a `None` value
#   ...
#   5: app::deref
#   6: app::handle_child
#   7: app::run_supervisor
#   8: app::main
```

## Verification

`verify.lua` (run per language with `LSP_LANG` set) asserts:

- `run` is identical across languages: exit 0, the three status lines.
- bare / unknown subcommand: exit 2, usage block — identical across languages.
- `crash`, per language:
  - **cpp** — exit 139 (SIGSEGV); the RAII cleanup line never appears; under
    `ulimit -c unlimited` in a scratch temp dir, `coredumpctl` finds and
    extracts a core for the crashed pid; a scripted
    `gdb --batch -ex run -ex bt` names `deref`, `handle_child`, and
    `run_supervisor` in the backtrace.
  - **go** — exit 2 (not 139, no core); the panic message names the
    standard nil-pointer-dereference text; the goroutine trace names
    `main.deref`, `main.handleChild`, `main.runSupervisor`.
  - **rust** — exit 101 (not 139, no core) either way; with
    `RUST_BACKTRACE=1`, the backtrace names `app::deref`,
    `app::handle_child`, `app::run_supervisor`; without it, those names are
    absent (the backtrace is opt-in) but the exit code is unchanged.

Run it standalone from this directory:

```sh
LSP_LANG=cpp EXAMPLE_DIR="$(pwd)" REPO_ROOT=../.. lua verify.lua
```

or through the orchestrator:

```sh
python3 ../../scripts/test-all-examples.py --only 28-gdb-and-remote-debugging
```
