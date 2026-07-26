#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (Rust only): proc-macros need their own crate, so this
# is a cargo workspace. The dispatcher still follows the book's demo contract.
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
