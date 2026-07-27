#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../../.. && pwd)"
BIN=bin/app

build() {
  go build -o "$BIN" .
}

run() {
  if [[ -n "${TARGET:-}" ]]; then
    exec "$REPO_ROOT/scripts/lab/deploy-to-vm.sh" "$TARGET" "$BIN" -- "$@"
  fi
  exec "./$BIN" "$@"
}

case "${1:-}" in
  build) build ;;
  run)   shift; run "$@" ;;
  "")    build; run ;;
  *)     echo "usage: $0 [build|run [args...]]" >&2; exit 2 ;;
esac
