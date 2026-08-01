# 46 — cpp-toolbox

A **single-language** example (C++ only): the C++ toolchain itself is the
subject, so a Go or Rust variant would have nothing to show. One `toolbox`
binary plus a full set of toolchain artifacts, engineered so every tool
produces a checkable effect, not just a clean exit.

- **`toolbox report`** — an FNV-1a digest over a fixed embedded byte array,
  printed via a custom `Digest { uint64_t fnv; }` type. Integer/string
  output only (no floats, addresses, timing, or unordered-container
  iteration), so a GCC build and a clang build print byte-identical output.
- **`toolbox defect overflow`** — deliberately triggers signed-integer
  overflow, caught by the `ubsan` CMake preset.
- `src/smell.cpp` — one deliberate, behavior-preserving clang-tidy smell
  (`performance-unnecessary-value-param`), compiled into the same binary so
  clang-tidy sees it through the compile-commands database.

## Layout

```
46-cpp-toolbox/
├── demo.sh              # dispatcher: ./demo.sh [cpp|all|build] [args...]
├── verify.lua           # automated check driven by CI
└── cpp/
    ├── demo.sh          # release preset build/run
    ├── CMakeLists.txt
    ├── CMakePresets.json    # release/debug/asan/ubsan/release-clang/debug-clang/conan
    ├── .clang-tidy
    ├── .clang-format
    ├── .gdbinit             # loads toolbox-printers.py
    ├── toolbox-printers.py  # gdb pretty-printer for Digest
    ├── src/
    │   ├── toolbox.cpp      # subcommands: report, defect overflow
    │   ├── digest.{hpp,cpp} # Digest type + FNV-1a
    │   └── smell.{hpp,cpp}  # the tidy-smell TU
    ├── fixtures/
    │   └── misformatted.cpp # NOT built; clang-format nonzero-exit demo
    └── conan/
        ├── conanfile.py     # isolated sub-target, pulls fmt
        ├── conan.lock       # committed lockfile
        ├── CMakeLists.txt
        └── src/main.cpp
```

## The demo contract

- `./demo.sh cpp` — build then run (`release` preset)
- `./demo.sh cpp build` — build only
- `./demo.sh cpp run [args]` — run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

The default build (`release` preset, what `./demo.sh cpp build` and CI
build-smoke use) never needs Conan, clang-tidy, clang-format, or gdb -- those
are exercised directly by `verify.lua` and the chapter's hand-run recipes.

## Verify

```bash
LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
```

Hard-gated, always asserted:

- **A.** `release` (GCC) vs `release-clang` builds of `toolbox report` are
  byte-identical and equal a known literal.
- **B.** The `ubsan` preset catches the seeded overflow and prints
  `runtime error: signed integer overflow`.
- **C.** `gdb -batch -x cpp/.gdbinit ...` renders the pretty-printed
  `Digest(0x...)`.

Gated only if the tool is present on `PATH` (never fails the example if
absent -- degrades to an informational `SKIP:` line):

- **D.** clang-tidy flags `performance-unnecessary-value-param` at
  `src/smell.cpp:3`.
- **E.** `clang-format --dry-run --Werror` is clean on the tracked sources,
  nonzero on `fixtures/misformatted.cpp`.
- **F.** `conan install` against the committed lockfile + the `conan`
  preset build + run prints the expected line.

ccache and mold are checked opportunistically and printed for information
only -- both are expected absent on the reference host and are never part
of the pass/fail count.

Mode: `local`. No root, no VM, no LGTM stack.
