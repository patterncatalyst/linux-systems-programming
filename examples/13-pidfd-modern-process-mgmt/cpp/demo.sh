#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd ../../.. && pwd)"
BIN=build/release/app

build() {
    cmake --preset release
    cmake --build --preset release
}

run() {
    if [[ ! -x "$BIN" ]]; then
        build
    fi
    if [[ -n "${TARGET:-}" ]]; then
        "$REPO_ROOT/scripts/lab/deploy-to-vm.sh" "$TARGET" "$BIN" -- "$@"
    else
        # exec so a backgrounded `run supervise ...` can be signalled directly
        exec "./$BIN" "$@"
    fi
}

case "${1:-}" in
    build)
        build
        ;;
    run)
        shift
        run "$@"
        ;;
    "")
        build
        run supervise --timeout-ms 2000 -- sh -c 'exit 0'
        ;;
    *)
        echo "usage: $0 [build|run [args]]" >&2
        exit 2
        ;;
esac
