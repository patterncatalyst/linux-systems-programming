#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (C++ only): this opens Part 14, the C++ concurrency
# compendium. A Go or Rust variant would answer a different question -- each
# has one blessed concurrency model, where the whole point here is that C++
# offers several and you have to choose.
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
