#!/usr/bin/env bash
# lab-down.sh — gracefully shut down the lab guests so nothing is left running.
#   ./lab-down.sh            # ACPI shutdown of <prefix>-target and <prefix>-peer
#   FORCE=1 ./lab-down.sh    # hard power-off any guest that ignores the ACPI request
#
# Run this when you're done. It does NOT delete anything — disks and snapshots
# survive; bring the lab back with ./lab-up.sh.
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
LAB_PREFIX="${LAB_PREFIX:-systems}"
TARGET="${TARGET_VM:-${LAB_PREFIX}-target}"; PEER="${PEER_VM:-${LAB_PREFIX}-peer}"

for VM in "$TARGET" "$PEER"; do
  virsh dominfo "$VM" >/dev/null 2>&1 || { echo "skip $VM (not defined)"; continue; }
  state="$(virsh domstate "$VM" 2>/dev/null || echo unknown)"
  if [ "$state" != "running" ]; then echo "$VM already $state"; continue; fi
  echo "shutting down $VM ..."
  virsh shutdown "$VM" >/dev/null 2>&1 || true
done

echo "waiting up to 60s for graceful shutdown ..."
for _ in $(seq 1 60); do
  running=0
  for VM in "$TARGET" "$PEER"; do
    [ "$(virsh domstate "$VM" 2>/dev/null || true)" = "running" ] && running=1
  done
  [ "$running" = 0 ] && break
  sleep 1
done

for VM in "$TARGET" "$PEER"; do
  [ "$(virsh domstate "$VM" 2>/dev/null || true)" = "running" ] || continue
  if [ "${FORCE:-0}" = "1" ]; then
    echo "forcing off $VM"; virsh destroy "$VM" >/dev/null 2>&1 || true
  else
    echo "WARNING: $VM still running (guest ignored ACPI). Re-run with FORCE=1 to power it off." >&2
  fi
done

virsh list --all
echo
echo "note: if you run an observability stack (or any other container) on the host,"
echo "      it is separate from these guests — stop it however you started it."
