# 56 — the capstone: one workload, seven models, two instruments

Chapters 50 through 55 each measured one concurrency model. Each used its own
instrument, on its own workload, to answer its own question — so **no two
numbers in the compendium were strictly comparable.**

That is what this example fixes. One workload runs through every model, and
every model is measured the same two ways.

## The workload

`cpp/src/workload.hpp` is shared verbatim by all seven arms: **8 tasks × 5000
rounds = 40000 folds** into one accumulator. The fold is addition on `uint64_t`,
which is commutative and associative, so the total does not depend on the order a
model happens to run in — and the models genuinely differ in that order.

```
[host]$ ./demo.sh cpp run sequential
sequential: tasks=8 rounds=5000 folds=40000 total=0xb75768f6610642a0 correct=yes
sequential: distinct_tids=1
```

**Every model produces `0xb75768f6610642a0`.** That is gate B, and it runs before
any comparison is drawn: a model that produced a different total would not be a
differently-priced answer to the same question, it would be a wrong one.

## Instrument 1 — who ran it (`gettid()`, ch50's instrument)

| arm | model | `distinct_tids` |
| --- | --- | --- |
| `sequential` | the control | **1** |
| `coroutine` | C++20 coroutines (ch53) | **1** |
| `fiber` | Boost.Fiber (ch54) | **1** |
| `pthreads` | pthreads (ch50) | **8** |
| `std-thread` | `std::jthread` (ch51) | **8** |
| `boost-thread` | Boost.Thread (ch52) | **8** |
| `asio` | Boost.Asio strand (ch55) | **8** |
| `senders` | P2300 via stdexec (opt-in) | **8** |

The first three hold **eight tasks in flight on one CPU** — Chapter 49's
"concurrent, not parallel", as a count rather than an assertion.

## Instrument 2 — what synchronization cost (`strace -f -c -e trace=futex`)

Three tiers, stable across runs:

| tier | arms | futex calls |
| --- | --- | --- |
| one thread, any model | `sequential`, `coroutine`, `fiber` | **1, 1, 1** |
| many threads + a **lock** | `pthreads` / `std-thread` / `boost-thread` | **230 / 427 / 451** |
| many threads + a **strand** | `asio` | **51** |

Byte-identical work in every row. A lock **blocks a thread**, which the kernel
then has to park and wake; a strand **defers a handler** and hands the thread
back. And a single-threaded model reaches the futex not at all, whatever its
suspension machinery costs elsewhere.

**Magnitudes are never gated** — they move on every run. The three tiers are.

## The two assembled axes

The `table` arm prints two more axes. Neither is measured here; both are carried
in as named constants beside the instrument that produced them, so the table
cites real prior work instead of restating it from memory:

```
[host]$ ./demo.sh cpp run table
table: thread=8388608 (ch50, pthread_getattr_np)
table: fiber=131072 (ch54, stack_traits::default_size)
table: coroutine_frame=32 (ch53, operator new in promise_type)
table: ch50 pthread_cancel=forced-unwind (swallowing it aborts the process)
table: ch51 stop_token=flag (nothing is thrown; the target polls)
table: ch52 thread::interrupt=exception (ordinary, at defined points)
table: ch55 cancellation_signal=completion (operation_aborted, handler still runs)
```

## The seventh model — P2300 senders, and what it is not

Chapter 49 measured that the standard library has no P2300 and said the live
comparison went into Chapter 56 *if and only if the toolchain had caught up*. It
has not:

```
[host]$ ./demo.sh cpp run versions
versions: senders_macro=undefined stdlib_p2300=no
versions: execution_macro=201902 (201902 = the C++17 parallel algorithms header)
```

So the senders arm links **NVIDIA's stdexec, the reference implementation**, and
says so everywhere it reports:

```
[host]$ ./cpp/build/conan/capstone-senders senders
senders: tasks=8 rounds=5000 folds=40000 total=0xb75768f6610642a0 correct=yes
senders: distinct_tids=8
senders: implementation=NVIDIA-stdexec (reference), stdlib_p2300=no
```

stdexec is **not in ConanCenter** and not packaged by Fedora, and upstream
publishes no releases — only `nvhpc-*` snapshot tags. So
`cpp/conan/recipe/conanfile.py` pins tag **`nvhpc-26.05`** by tarball **sha256**,
which is what makes the build reproducible; a tag alone can be moved.

## Layout

```
56-capstone-comparison/
├── demo.sh          # dispatcher (C++ only)
├── verify.lua       # gates A-D and F hard, E skip-if-absent
└── cpp/
    ├── CMakeLists.txt
    ├── CMakePresets.json     # `release`, plus the opt-in `conan` preset
    ├── demo.sh
    ├── conan/
    │   ├── conanfile.py      # consumer (ch46's isolated-sub-target pattern)
    │   ├── conan.lock        # committed
    │   └── recipe/
    │       └── conanfile.py  # stdexec pinned by tag + sha256
    └── src/
        ├── workload.hpp      # THE workload, shared by every arm
        ├── capstone.cpp      # six models + the control + the table
        └── senders.cpp       # the seventh, built only by the conan preset
```

**The six-model core needs no Conan, no network, and no third-party package** —
system Boost only. That isolation is deliberate and is what keeps `validate.yml`
green on fedora:44, which has no Conan.

## Build and run

```
[host]$ ./demo.sh cpp build          # six models; never looks for stdexec
[host]$ for a in versions sequential pthreads std-thread boost-thread \
                 coroutine fiber asio table; do ./demo.sh cpp run "$a"; done
```

The opt-in seventh arm, which needs Conan and (on a cold cache) the network:

```
[host]$ conan export cpp/conan/recipe
[host]$ conan install cpp/conan --output-folder=cpp/build/conan --build=missing \
          --lockfile=cpp/conan/conan.lock -s compiler.cppstd=23
[host]$ cd cpp && cmake --preset conan && cmake --build --preset conan
[host]$ ./cpp/build/conan/capstone-senders senders
```

> **The `conan` preset builds `Release`, not `RelWithDebInfo`, on purpose.**
> CMakeDeps gates the include directories behind `$<$<CONFIG:Release>:...>`, so a
> mismatched build type silently drops them — `find_package(stdexec)` still
> *succeeds*, and the compile then fails on a missing header.

## Verification

```
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

- **A** — all nine cases compute `digest=0x481984990deee5ff` (ch46–ch55's).
- **B** — the capstone gate: every model folds to the identical total.
- **C** — instrument 1: exactly 1 tid for the three single-threaded models,
  more than 1 for the four multi-threaded ones. Signs, never magnitudes.
- **D** — instrument 2: single-threaded models under 10 futex calls, lock-based
  models over 100, and Asio's strand under **half** what `std::mutex` cost.
  Degrades to an informational SKIP if `strace` cannot attach.
- **F** — clang parity on the digest and on the total.

**E** (the senders arm) runs only if `conan` is on `PATH` and can supply stdexec;
otherwise it prints a SKIP and everything else still passes. Both paths were
exercised: **PASS 57 / FAIL 0** with Conan available, and **PASS 52 / FAIL 0**
with `conan` hidden from `PATH` and the Conan build tree deleted.

**Not gated:** timings (ch39), futex magnitudes, and the ranking of models by
futex count beyond the three-tier split.
