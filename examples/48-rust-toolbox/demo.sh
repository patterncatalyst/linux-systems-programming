#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (Rust only): the Rust toolchain itself is the
# subject (rustup/rust-toolchain.toml pinning, rustfmt, clippy, cargo test,
# and the skip-if-present nextest/deny/llvm-cov/audit accelerators) -- a
# C++ or Go variant would have nothing to show.
langs=(rust)

case "${1:-all}" in
  rust)
    shift
    exec "./rust/demo.sh" "$@"
    ;;
  all)
    for lang in "${langs[@]}"; do "./$lang/demo.sh"; done
    ;;
  build)
    for lang in "${langs[@]}"; do "./$lang/demo.sh" build; done
    ;;
  *)
    echo "usage: $0 [rust|all|build] [args...]" >&2
    exit 2
    ;;
esac
