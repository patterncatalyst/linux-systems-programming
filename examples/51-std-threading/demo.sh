#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (C++ only), like ch49 and ch50. This chapter measures
# what the C++ standard library's synchronization primitives cost on this
# kernel; Go and Rust have their own primitives over the same futex, and ch56
# is where the cross-language comparison belongs.
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
