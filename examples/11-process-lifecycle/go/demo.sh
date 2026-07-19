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
  run)   shift; run "$@" ;;
  "")    build; demo ;;
  *)     echo "usage: $0 [build|run [args...]]" >&2; exit 2 ;;
esac
