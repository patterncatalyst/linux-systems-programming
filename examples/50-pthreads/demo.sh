#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Single-language example (C++ only), like ch49. This chapter is about what
# POSIX threads expose that std::thread does not; Go and Rust wrap the same
# pthreads and deliberately hide exactly these controls, so a three-language
# version would have two directories with nothing to say.
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
