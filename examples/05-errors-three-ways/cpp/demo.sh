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
        "./$BIN" "$@"
    fi
}

# Local walkthrough of the error taxonomy: one success, one exit-2 path,
# one exit-3 path. Runs the binary directly (not via TARGET deploy) because
# the scratch files live on this host.
demo() {
    local tmp
    tmp="$(mktemp -d)"
    trap "rm -rf '$tmp'" EXIT
    head -c 200000 /dev/urandom > "$tmp/src.bin"

    echo "== copy 200000 random bytes =="
    "./$BIN" "$tmp/src.bin" "$tmp/dst.bin"
    cmp "$tmp/src.bin" "$tmp/dst.bin" && echo "cmp: src and dst identical"

    echo "== missing source (expect exit 2) =="
    local rc=0
    "./$BIN" "$tmp/missing.bin" "$tmp/x.bin" || rc=$?
    echo "exit=$rc"

    echo "== write to /dev/full (expect exit 3) =="
    rc=0
    "./$BIN" "$tmp/src.bin" /dev/full || rc=$?
    echo "exit=$rc"
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
        demo
        ;;
    *)
        echo "usage: $0 [build|run [args]]" >&2
        exit 2
        ;;
esac
