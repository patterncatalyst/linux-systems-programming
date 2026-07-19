#!/usr/bin/env bash
# run-latency-bench.sh — staged onto the lab guest by verify.lua and run there
# (never on the host: this book's vm-mode examples never ssh from the local
# machine into anything but the lab guest itself, and this script IS what runs
# on that guest).
#
#   ./run-latency-bench.sh /path/to/app [N] [WARMUP]
#
# Two independent phases, both against a fresh pair of server processes so
# neither leaves state the other depends on:
#
#   Phase A — kernel-level proof that --pin actually restricts scheduling.
#   Neither server is touched by an external `taskset`; naive keeps whatever
#   affinity mask the shell handed it (every online CPU) while fastpath's own
#   sched_setaffinity(2) call (inside `app fastpath --pin`) narrows its own
#   mask down to exactly one CPU. Reading /proc/<pid>/status confirms this at
#   the kernel level rather than trusting the program's own printed line.
#
#   Phase B — the timing comparison. A naive-vs-fastpath latency race is only
#   meaningful if everything about connection placement is identical between
#   the two runs; on this lab's 2-vCPU guest, whether the client and server
#   land on the SAME virtual CPU or DIFFERENT ones dominates the result (a
#   same-vCPU handoff needs no cross-vCPU wakeup; a cross-vCPU one does, and
#   under nested KVM that IPI is expensive). So here BOTH servers are
#   externally `taskset`-pinned to the same CPU fastpath will end up on
#   internally, and the client is pinned to the other CPU for both
#   measurements — holding placement constant so the only thing that differs
#   is the allocation/scheduling discipline inside each server.
set -uo pipefail

APP="${1:?usage: run-latency-bench.sh /path/to/app [N] [WARMUP]}"
N="${2:-20000}"
WARMUP="${3:-500}"

NPROC=$(nproc)
if (( NPROC >= 2 )); then
  PIN_CPU=1
  CLIENT_CPU=0
else
  PIN_CPU=0
  CLIENT_CPU=0
fi

PORT_NAIVE_A=19501
PORT_FAST_A=19502
PORT_NAIVE_B=19503
PORT_FAST_B=19504

echo "app: nproc=$NPROC pin_cpu=$PIN_CPU client_cpu=$CLIENT_CPU"

wait_for_port() {
  local host="$1" port="$2"
  for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then
      exec 3<&- 2>/dev/null
      exec 3>&- 2>/dev/null
      return 0
    fi
    sleep 0.1
  done
  return 1
}

cpus_allowed() {
  awk '/Cpus_allowed_list/ {print $2}' "/proc/$1/status" 2>/dev/null
}

# ---------------------------------------------------------------------------
# Phase A: kernel-level pinning proof.
# ---------------------------------------------------------------------------
"$APP" naive --port "$PORT_NAIVE_A" >/tmp/lsp40-naiveA.log 2>&1 &
naive_a_pid=$!
"$APP" fastpath --port "$PORT_FAST_A" --pin "$PIN_CPU" >/tmp/lsp40-fastA.log 2>&1 &
fast_a_pid=$!

if ! wait_for_port 127.0.0.1 "$PORT_NAIVE_A" || ! wait_for_port 127.0.0.1 "$PORT_FAST_A"; then
  echo "app: error: phase A servers did not come up"
  cat /tmp/lsp40-naiveA.log /tmp/lsp40-fastA.log 2>/dev/null
  kill "$naive_a_pid" "$fast_a_pid" 2>/dev/null
  exit 1
fi

echo "app: naive-cpus-allowed $(cpus_allowed "$naive_a_pid")"
echo "app: fastpath-cpus-allowed $(cpus_allowed "$fast_a_pid")"

kill "$naive_a_pid" "$fast_a_pid" 2>/dev/null
wait "$naive_a_pid" 2>/dev/null
wait "$fast_a_pid" 2>/dev/null

# ---------------------------------------------------------------------------
# Phase B: confound-controlled timing comparison.
# ---------------------------------------------------------------------------
taskset -c "$PIN_CPU" "$APP" naive --port "$PORT_NAIVE_B" >/tmp/lsp40-naiveB.log 2>&1 &
naive_b_pid=$!
taskset -c "$PIN_CPU" "$APP" fastpath --port "$PORT_FAST_B" --pin "$PIN_CPU" --busy-poll \
  >/tmp/lsp40-fastB.log 2>&1 &
fast_b_pid=$!

if ! wait_for_port 127.0.0.1 "$PORT_NAIVE_B" || ! wait_for_port 127.0.0.1 "$PORT_FAST_B"; then
  echo "app: error: phase B servers did not come up"
  cat /tmp/lsp40-naiveB.log /tmp/lsp40-fastB.log 2>/dev/null
  kill "$naive_b_pid" "$fast_b_pid" 2>/dev/null
  exit 1
fi

# Three interleaved trials per variant (naive1, fastpath1, naive2, ...) rather
# than three-then-three: interleaving keeps any slow drift in host load from
# landing entirely on one side of the comparison. Each trial is tagged so
# verify.lua can group them by variant regardless of order; the pass/fail
# gate downstream compares the MEDIAN of the three trials per variant, not a
# single sample — this lab guest is a shared, nested-KVM host, and a lone
# run occasionally hits a multi-hundred-microsecond scheduling stall
# unrelated to which variant is being measured (see README).
status=0
for trial in 1 2 3; do
  echo "app: === naive measure trial $trial/3 ==="
  taskset -c "$CLIENT_CPU" "$APP" measure --target "127.0.0.1:$PORT_NAIVE_B" --n "$N" \
    --warmup "$WARMUP" --tag "naive-$trial"
  (( $? == 0 )) || status=1

  echo "app: === fastpath measure trial $trial/3 ==="
  taskset -c "$CLIENT_CPU" "$APP" measure --target "127.0.0.1:$PORT_FAST_B" --n "$N" \
    --warmup "$WARMUP" --tag "fastpath-$trial"
  (( $? == 0 )) || status=1
done
naive_status=$status
fast_status=$status

kill "$naive_b_pid" "$fast_b_pid" 2>/dev/null
wait "$naive_b_pid" 2>/dev/null
wait "$fast_b_pid" 2>/dev/null

if [[ "$naive_status" -ne 0 || "$fast_status" -ne 0 ]]; then
  echo "app: error: measure exited nonzero (naive=$naive_status fastpath=$fast_status)"
  exit 1
fi
exit 0
