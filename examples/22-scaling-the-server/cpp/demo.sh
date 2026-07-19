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
        # exec so a backgrounded `run serve ...` can be signalled directly
        exec "./$BIN" "$@"
    fi
}

demo() {
    build
    local tmp
    tmp=$(mktemp -d)
    "./$BIN" serve --engine epoll --port 0 2>"$tmp/serve.err" &
    local sp=$! port=""
    for _ in $(seq 1 100); do
        port=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' "$tmp/serve.err")
        [[ -n "$port" ]] && break
        sleep 0.05
    done
    echo "chatterd (epoll) listening on 127.0.0.1:$port"
    "./$BIN" listen --port "$port" bob --count 1 >"$tmp/bob.out" &
    local lp=$!
    sleep 0.2
    "./$BIN" send --port "$port" alice "hello from alice"
    wait "$lp"
    echo "bob received: $(cat "$tmp/bob.out")"
    "./$BIN" flood --port "$port" 50
    kill -TERM "$sp"
    wait "$sp" 2>/dev/null || true
    cat "$tmp/serve.err"
    rm -rf "$tmp"
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
        demo
        ;;
    *)
        echo "usage: $0 [build|run [args]]" >&2
        exit 2
        ;;
esac
