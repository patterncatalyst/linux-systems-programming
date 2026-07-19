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

# Local walkthrough of the child fates pmon distinguishes: clean exit,
# nonzero exit, death by signal, exec failure. Runs the binary directly.
demo() {
  local rc
  echo "== pmon run -- /bin/true (expect exit 0) =="
  rc=0; "./$BIN" run -- /bin/true || rc=$?
  echo "exit=$rc"

  echo "== pmon run -- sh -c 'exit 42' (expect exit 42) =="
  rc=0; "./$BIN" run -- sh -c 'exit 42' || rc=$?
  echo "exit=$rc"

  echo "== pmon run -- sh -c 'kill -TERM \$\$' (expect exit 143) =="
  rc=0; "./$BIN" run -- sh -c 'kill -TERM $$' || rc=$?
  echo "exit=$rc"

  echo "== pmon run -- ./no-such-binary (expect exit 127) =="
  rc=0; "./$BIN" run -- ./no-such-binary || rc=$?
  echo "exit=$rc"
}

case "${1:-}" in
  build) build ;;
  run) shift; run "$@" ;;
  "") build; demo ;;
  *) echo "usage: $0 [build|run [args...]]" >&2; exit 2 ;;
esac
