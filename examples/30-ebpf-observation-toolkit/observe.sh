#!/usr/bin/env bash
# observe.sh — the actual demo of this chapter. Runs ON THE LAB GUEST AS ROOT
# (bcc-tools and bpftrace both need CAP_BPF/CAP_PERFMON, which on this lab
# image means root). It starts `app work --seconds N` in the background and
# then points five separate observation tools at it in turn, proving each one
# saw a real event our program produced:
#
#   opensnoop   -> the bait file open(2)
#   execsnoop   -> the fork/exec of "true"
#   funccount   -> a per-call count of busy_hash()
#   offcputime  -> the sleep between iterations (off-CPU stacks)
#   bpftrace    -> a uprobe on busy_hash(), one-liner style
#
# This is tooling observing our userspace binary via uprobes/tracepoints; it
# writes no kernel-side eBPF program of its own.
#
# Usage: observe.sh <path-to-app-binary-on-this-host> [seconds]
#
# Not `set -e`: a bcc tool killed by its own `timeout` wrapper is expected,
# not a driver failure — each step is checked/reported on its own.
set -uo pipefail

BIN="${1:?usage: observe.sh <path-to-app-binary> [seconds]}"
WORK_SECONDS="${2:-60}"
BCC=/usr/share/bcc/tools
BAIT=/tmp/lsp-ebpf-work-bait.txt
OUT=/tmp/lsp-ebpf-observe
COMM="$(basename "$BIN")" # /proc comm the kernel records for this binary (<=15 chars: "app")
PATTERN="${BIN}:*busy_hash*" # matches busy_hash (cpp/rust) and main.busy_hash (go)

if [[ $EUID -ne 0 ]]; then
    echo "observe.sh must run as root (bcc-tools/bpftrace need it)" >&2
    exit 1
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.out "$BAIT"
pkill -x "$COMM" 2>/dev/null || true
sleep 0.5

echo "=== workload ==="
"$BIN" work --seconds "$WORK_SECONDS" >"$OUT/app.log" 2>&1 &
APP_PID=$!
sleep 0.7
if ! kill -0 "$APP_PID" 2>/dev/null; then
    echo "workload failed to start:" >&2
    cat "$OUT/app.log" >&2
    exit 1
fi
echo "workload pid=$APP_PID"
head -1 "$OUT/app.log"

echo
echo "=== opensnoop (open() bait) ==="
timeout 6 "$BCC/opensnoop" -p "$APP_PID" >"$OUT/opensnoop.out" 2>&1 || true
tail -n +1 "$OUT/opensnoop.out"

echo
echo "=== execsnoop (fork/exec bait) ==="
timeout 6 "$BCC/execsnoop" -P "$APP_PID" >"$OUT/execsnoop.out" 2>&1 || true
tail -n +1 "$OUT/execsnoop.out"

echo
echo "=== funccount busy_hash (uprobe/funccount bait) ==="
timeout 7 "$BCC/funccount" -p "$APP_PID" -d 5 "$PATTERN" >"$OUT/funccount.out" 2>&1 || true
cat "$OUT/funccount.out"

echo
echo "=== offcputime (sleep off-CPU bait) ==="
timeout 8 "$BCC/offcputime" -p "$APP_PID" 6 >"$OUT/offcputime.out" 2>&1 || true
echo "($(wc -l <"$OUT/offcputime.out") lines captured; frames naming our binary/function:)"
grep -E "busy_hash|nanosleep|${COMM} \(" "$OUT/offcputime.out" 2>/dev/null | sort -u

echo
echo "=== bpftrace uprobe:busy_hash ==="
timeout 7 bpftrace -e "uprobe:${BIN}:*busy_hash* { @calls = count(); } interval:s:5 { print(@calls); exit(); }" \
    >"$OUT/bpftrace.out" 2>&1 || true
cat "$OUT/bpftrace.out"

# Let the workload finish on its own — it's bounded by its own --seconds
# deadline, budgeted comfortably longer than the tool sequence above. Poll
# rather than a bare `wait`, so a shell job-control quirk can't cause a
# spurious early kill of a workload that's still legitimately running; only
# force it if it somehow overruns its own budget by a wide margin.
deadline_ts=$(($(date +%s) + WORK_SECONDS + 20))
while kill -0 "$APP_PID" 2>/dev/null; do
    if [[ $(date +%s) -ge $deadline_ts ]]; then
        echo "workload overran its budget; stopping it" >&2
        kill "$APP_PID" 2>/dev/null || true
        break
    fi
    sleep 0.5
done
wait "$APP_PID" 2>/dev/null || true

echo
echo "=== summary ==="
opens=$(grep -c "$BAIT" "$OUT/opensnoop.out" 2>/dev/null || true)
execs=$(grep -c '^true' "$OUT/execsnoop.out" 2>/dev/null || true)
funccalls=$(grep -oE 'busy_hash[[:space:]]+[0-9]+' "$OUT/funccount.out" 2>/dev/null | awk '{print $2}')
offcpu_hits=$(grep -c "${COMM} (" "$OUT/offcputime.out" 2>/dev/null || true)
bt_calls=$(grep -oE '@calls: [0-9]+' "$OUT/bpftrace.out" 2>/dev/null | tail -1 | awk '{print $2}')
echo "opensnoop_opens=${opens:-0}"
echo "execsnoop_execs=${execs:-0}"
echo "funccount_calls=${funccalls:-0}"
echo "offcputime_hits=${offcpu_hits:-0}"
echo "bpftrace_calls=${bt_calls:-0}"
echo "app_summary: $(tail -1 "$OUT/app.log")"
