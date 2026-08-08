# _template — hello-syscall

The seed for every runnable example in the book. It implements the same tiny
program three times — C++23, Go, and Rust — each printing exactly one line:

```
pid <PID> on <sysname> <release> (<machine>)
```

via `getpid(2)` + `uname(2)` equivalents, with each language's idiomatic error
handling (`std::expected` / wrapped errors via `%w` / `rustix`).

## Layout

```
_template/
├── demo.sh      # dispatcher: ./demo.sh [cpp|go|rust|all|build] [args...]
├── verify.lua   # automated check driven by CI
├── cpp/         # CMake preset build, demo.sh
├── go/          # go build, demo.sh
└── rust/        # cargo build, demo.sh
```

## The demo contract

Every language directory has a `demo.sh` with the same interface, identical
across the whole book:

- `./demo.sh` — build then run
- `./demo.sh build` — build only
- `./demo.sh run [args]` — run the built binary
- With env `TARGET` set, `run` deploys the binary to that lab VM via
  `scripts/lab/deploy-to-vm.sh "$TARGET" <binary> -- [args]` instead of
  running locally.

The top-level `demo.sh` here is only a dispatcher: `./demo.sh cpp run` execs
`cpp/demo.sh run`; bare `./demo.sh` (or `all`) builds and runs all three.

## Verification

`verify.lua` is run from this directory with `LSP_LANG` set to one language at
a time. It uses `scripts/lib/checks.lua` to run `./demo.sh $LSP_LANG run` and
asserts the output matches `pid %d+ on Linux`. Exit 0 = pass, 1 = fail,
77 = skip.

## Mapping to a chapter

Each chapter's example is a copy of this template (`scripts/new-example.sh
NN-slug`) with the program body replaced by that chapter's code. Keep the
demo contract and `verify.lua` shape intact so CI and the manifest
(`examples/manifest.yaml`) keep working.
