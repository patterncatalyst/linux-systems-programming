#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

BIN=target/release/app

build() {
  cargo build --release
}

run() {
  if [[ -n "${TARGET:-}" ]]; then
    local repo_root
    repo_root=$(cd ../../.. && pwd)
    "$repo_root/scripts/lab/deploy-to-vm.sh" "$TARGET" "$BIN" -- "$@"
  else
    "./$BIN" "$@"
  fi
}

case "${1:-}" in
  build) build ;;
  run) shift; run "$@" ;;
  "") build; run ;;
  *) echo "usage: $0 [build|run [args...]]" >&2; exit 2 ;;
esac
