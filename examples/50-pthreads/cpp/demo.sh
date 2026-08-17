#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd ../../.. && pwd)"
BIN=build/release/pthreads

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
        "./$BIN" "$@"
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
        run identity
        ;;
    *)
        echo "usage: $0 [build|run <identity|naming|stack|affinity|sched|cancel|cancel-swallow|bridge>]" >&2
        exit 2
        ;;
esac
