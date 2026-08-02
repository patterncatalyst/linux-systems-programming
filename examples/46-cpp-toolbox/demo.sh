#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (C++ only): the whole point is the C++ toolchain
# itself (CMake presets, Conan, GCC/clang parity, gdb printers, clang-tidy/
# clang-format, ccache/mold) -- a Go or Rust variant would have nothing to
# show.
langs=(cpp)

case "${1:-all}" in
  cpp)
    shift
    exec "./cpp/demo.sh" "$@"
    ;;
  all)
    for lang in "${langs[@]}"; do "./$lang/demo.sh"; done
    ;;
  build)
    for lang in "${langs[@]}"; do "./$lang/demo.sh" build; done
    ;;
  *)
    echo "usage: $0 [cpp|all|build] [args...]" >&2
    exit 2
    ;;
esac
