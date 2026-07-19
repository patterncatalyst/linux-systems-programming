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
  run)   shift; run "$@" ;;
  "")    build; demo ;;
  *)     echo "usage: $0 [build|run [args...]]" >&2; exit 2 ;;
esac
