#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (Go only): the program observes the Go runtime itself
# (scheduler, GC, netpoller), so C++/Rust variants would have nothing to show.
langs=(go)

case "${1:-all}" in
  go)
    shift
    exec "./go/demo.sh" "$@"
    ;;
  all)
    for lang in "${langs[@]}"; do "./$lang/demo.sh"; done
    ;;
  build)
    for lang in "${langs[@]}"; do "./$lang/demo.sh" build; done
    ;;
  *)
    echo "usage: $0 [go|all|build] [args...]" >&2
    exit 2
    ;;
esac
