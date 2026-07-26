#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

BIN=target/release/app

build() {
  cargo build --release
}

run() {
  if [[ ! -x "$BIN" ]]; then build; fi
  "./$BIN" "$@"
}

# miri runs the app under the interpreter that checks for undefined behaviour.
# It needs nightly + the miri component; the CI gate does NOT depend on it
# (verify.lua exercises the stable build only). Used by the chapter.
miri() {
  cargo +nightly miri run --quiet "$@"
}

case "${1:-}" in
  build)    build ;;
  run)      shift; run "$@" ;;
  miri)     shift; miri "$@" ;;
  "")       build; run ;;
  *)        echo "usage: $0 [build|run [args]|miri [args]]" >&2; exit 2 ;;
esac
