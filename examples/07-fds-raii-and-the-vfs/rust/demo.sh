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

# Self-contained fixture demo: fwatch needs a directory to watch, so the bare
# invocation builds one, snapshots it, mutates it, and diffs.
demo() {
  local tmp
  tmp=$(mktemp -d)
  trap "rm -rf '$tmp'" EXIT
  mkdir -p "$tmp/tree/sub"
  printf 'one\n' >"$tmp/tree/a.txt"
  printf 'nested\n' >"$tmp/tree/sub/b.txt"
  echo "# fwatch snapshot"
  "./$BIN" snapshot "$tmp/tree" | tee "$tmp/snap.txt"
  printf 'grown\n' >>"$tmp/tree/a.txt"
  rm "$tmp/tree/sub/b.txt"
  printf 'new\n' >"$tmp/tree/c.txt"
  echo "# fwatch diff (after append/remove/create)"
  "./$BIN" diff "$tmp/tree" "$tmp/snap.txt"
}

case "${1:-}" in
  build) build ;;
  run) shift; run "$@" ;;
  "") build; demo ;;
  *) echo "usage: $0 [build|run [args...]]" >&2; exit 2 ;;
esac
