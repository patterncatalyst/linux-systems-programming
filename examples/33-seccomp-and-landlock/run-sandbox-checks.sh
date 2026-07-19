#!/usr/bin/env bash
# run-sandbox-checks.sh APP_BIN — staged on the lab guest and run as root
# (SUDO=1) by verify.lua. Drives all three behavioral assertions in one ssh
# round trip and prints a line per result the caller can pattern-match:
#
#   forbidden-syscall-exit=<n>   (20 = confirmed EPERM, the passing case)
#   outside-exit=<n>             (20 = confirmed EACCES, the passing case)
#   watch-sandbox-exit=<n>       (0  = the positive control ran cleanly)
#
# Deliberately not `set -e`: a probe's nonzero exit is the expected result
# we're capturing, not a driver failure.
set -uo pipefail

APP="${1:?usage: run-sandbox-checks.sh APP_BIN}"

OUTSIDE_SANDBOX=/tmp/lsp33-outside-sandbox
OUTSIDE_TARGET_DIR=/tmp/lsp33-outside-target
OUTSIDE_TARGET="$OUTSIDE_TARGET_DIR/secret.txt"
WATCH_SANDBOX=/tmp/lsp33-watch-sandbox

rm -rf "$OUTSIDE_SANDBOX" "$OUTSIDE_TARGET_DIR" "$WATCH_SANDBOX"
mkdir -p "$OUTSIDE_SANDBOX" "$OUTSIDE_TARGET_DIR" "$WATCH_SANDBOX"
echo "top secret" > "$OUTSIDE_TARGET"

echo "=== forbidden-syscall ==="
"$APP" probe --forbidden-syscall
echo "forbidden-syscall-exit=$?"

echo "=== outside ==="
"$APP" probe --sandbox "$OUTSIDE_SANDBOX" --outside "$OUTSIDE_TARGET"
echo "outside-exit=$?"

echo "=== watch-sandbox (positive control: watching inside the tree still works) ==="
"$APP" watch --sandbox "$WATCH_SANDBOX" --timeout-ms 4000 &
WATCH_PID=$!
sleep 0.5
echo hello > "$WATCH_SANDBOX/inside.txt"
echo more >> "$WATCH_SANDBOX/inside.txt"
rm -f "$WATCH_SANDBOX/inside.txt"
wait "$WATCH_PID"
echo "watch-sandbox-exit=$?"

rm -rf "$OUTSIDE_SANDBOX" "$OUTSIDE_TARGET_DIR" "$WATCH_SANDBOX"
exit 0
