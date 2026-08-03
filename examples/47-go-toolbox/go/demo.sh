#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../../.. && pwd)"
BIN=bin/toolbox

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
  "")    build; run report ;;
  *)     echo "usage: $0 [build|run [report|tools|defect divzero]]" >&2; exit 2 ;;
esac
