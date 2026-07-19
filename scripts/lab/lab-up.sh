#!/usr/bin/env bash
# lab-up.sh — start the lab guests and print their IPs.
#   export LAB_PREFIX=myproject
#   ./lab-up.sh
#   eval "$(./lab-up.sh --export)"   # also export TARGET_IP / PEER_IP into your shell
#
# Starts <prefix>-target and <prefix>-peer if defined but not running, then waits
# for DHCP leases. If a guest isn't defined yet, it tells you to provision it.
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LAB_PREFIX="${LAB_PREFIX:-systems}"
TARGET="${TARGET_VM:-${LAB_PREFIX}-target}"; PEER="${PEER_VM:-${LAB_PREFIX}-peer}"

for VM in "$TARGET" "$PEER"; do
  if ! virsh dominfo "$VM" >/dev/null 2>&1; then
    echo "skip $VM (not defined — provision it: $SCRIPT_DIR/provision-vm.sh $VM)" >&2
    continue
  fi
  state="$(virsh domstate "$VM" 2>/dev/null || echo unknown)"
  if [ "$state" = "running" ]; then echo "$VM already running"; else echo "starting $VM"; virsh start "$VM" >/dev/null 2>&1 || true; fi
done

echo "waiting for DHCP leases ..." >&2
for _ in $(seq 1 30); do
  ti="$("$SCRIPT_DIR/vm-ip.sh" "$TARGET" 2>/dev/null || true)"
  [ -n "$ti" ] && break
  sleep 2
done

if [ "${1:-}" = "--export" ]; then
  exec "$SCRIPT_DIR/lab-ips.sh"
fi
"$SCRIPT_DIR/vm-ip.sh" "$TARGET" >/dev/null 2>&1 && echo "$TARGET: $("$SCRIPT_DIR/vm-ip.sh" "$TARGET")" || echo "$TARGET: no lease yet"
"$SCRIPT_DIR/vm-ip.sh" "$PEER"   >/dev/null 2>&1 && echo "$PEER:   $("$SCRIPT_DIR/vm-ip.sh" "$PEER")"   || echo "$PEER:   no lease / not provisioned"
