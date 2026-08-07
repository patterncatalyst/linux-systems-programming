#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd ../../.. && pwd)"

BIN=target/release/toolbox

# --offline on purpose: toolbox is a zero-dependency crate, so a correct
# build never needs the network. Passing --offline turns "cargo quietly
# reached crates.io" from an invisible event into a hard build failure.
build() {
  cargo build --offline --release
}

run() {
  if [[ -n "${TARGET:-}" ]]; then
    exec "$REPO_ROOT/scripts/lab/deploy-to-vm.sh" "$TARGET" "$BIN" -- "$@"
  fi
  exec "./$BIN" "$@"
}

case "${1:-}" in
  build) build ;;
  run) shift; run "$@" ;;
  "") build; run report ;;
  *) echo "usage: $0 [build|run [report]]" >&2; exit 2 ;;
esac
