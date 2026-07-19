#!/usr/bin/env bash
# _demo-bg.sh — auto-cleanup for guest-side background jobs a demo starts.
#
# Demos sometimes drive a workload on the lab VM(s) in the background (nohup
# load-generators, ncat listeners, churn loops). Those outlive the demo's
# foreground process, so a plain Ctrl-C leaves them running and pinning the
# VM's CPU. Source this and call `reap <ssh-target> <pattern>` once per
# background workload the demo launches; on exit (normal or Ctrl-C) every
# registered pattern is killed on its host.
#
#   reap "fedora@$TIP" target-app          # a named binary  -> pkill -x
#   reap "fedora@$TIP" 'while true; do curl -s http://127.0.0.1:8080/'   # a loop -> pkill -f
#
# `reap` tries both `pkill -x` (exact process name) and `pkill -f` (command-line
# substring), so either style of pattern works. The launch lines themselves stay
# exactly as they are — this only registers what to clean up.

_REAP_LIST=()
reap() { _REAP_LIST+=("$1"$'\t'"$2"); }

_reap_all() {
    [ "${_REAPED:-0}" = 1 ] && return; _REAPED=1
    [ ${#_REAP_LIST[@]} -eq 0 ] && return
    local entry host pat ssho="-o StrictHostKeyChecking=accept-new -o ConnectTimeout=8"
    for entry in "${_REAP_LIST[@]}"; do
        host="${entry%%$'\t'*}"; pat="${entry#*$'\t'}"
        ssh $ssho "$host" "pkill -f -- \"$pat\" 2>/dev/null; pkill -x -- \"$pat\" 2>/dev/null; true" \
            </dev/null >/dev/null 2>&1 || true
    done
}

# EXIT covers normal exit and `set -e` failures; INT/TERM route through exit so
# the EXIT trap fires exactly once (guarded by _REAPED).
trap _reap_all EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
