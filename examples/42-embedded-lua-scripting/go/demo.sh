#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
BIN=bin/app

build() {
    go build -o "$BIN" .
}

# run: replay the two-phase exit scenario, hot-reload the policy on a real
# SIGHUP between phases, and stream the supervisor's output. The reload is
# race-free: we wait for the "awaiting SIGHUP" sentinel before editing the
# watched policy file and signalling, so there is no fixed sleep to tune.
run() {
    if [[ ! -x "$BIN" ]]; then build; fi
    local work; work="$(mktemp -d)"
    trap 'rm -rf "$work"' RETURN
    cp ../policy.lua "$work/policy.lua"
    "./$BIN" run --policy "$work/policy.lua" > "$work/out" 2>&1 &
    local pid=$!
    local i
    for i in $(seq 1 400); do
        if grep -q 'awaiting SIGHUP' "$work/out" 2>/dev/null; then break; fi
        sleep 0.025
    done
    cp ../policy-v2.lua "$work/policy.lua"
    kill -HUP "$pid"
    local rc=0
    wait "$pid" || rc=$?
    cat "$work/out"
    return "$rc"
}

# check: load one policy under the sandbox and report ok / rejected.
check() {
    if [[ ! -x "$BIN" ]]; then build; fi
    exec "./$BIN" check --policy "$@"
}

case "${1:-}" in
    build) build ;;
    run)   shift; run ;;
    check) shift; check "$@" ;;
    "")    build; run ;;
    *)     echo "usage: $0 [build|run|check <policy>]" >&2; exit 2 ;;
esac
