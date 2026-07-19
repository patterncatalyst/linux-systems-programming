#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

langs=(cpp go rust)

case "${1:-all}" in
  cpp|go|rust)
    lang="$1"
    shift
    exec "./$lang/demo.sh" "$@"
    ;;
  all)
    for lang in "${langs[@]}"; do "./$lang/demo.sh"; done
    ;;
  build)
    for lang in "${langs[@]}"; do "./$lang/demo.sh" build; done
    ;;
  *)
    echo "usage: $0 [cpp|go|rust|all|build] [args...]" >&2
    exit 2
    ;;
esac
