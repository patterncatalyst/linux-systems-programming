#!/usr/bin/env bash
# revert-vm.sh — restore a lab guest to a snapshot (default: lab-ready).
#   ./revert-vm.sh myproject-target                 # revert to "lab-ready"
#   ./revert-vm.sh myproject-target clean-updated   # revert to a named snapshot
#
# Use this to undo destructive/offensive demos, or to get back to a clean tooled
# baseline. Reverting to a running-state snapshot restores it running.
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
VM="${1:?usage: revert-vm.sh <vm-name> [snapshot-name]}"
SNAP="${2:-lab-ready}"
virsh snapshot-info "$VM" "$SNAP" >/dev/null 2>&1 \
  || { echo "no snapshot '$SNAP' on $VM — list with: virsh snapshot-list $VM" >&2; exit 1; }
virsh snapshot-revert "$VM" "$SNAP"
echo "$VM reverted to snapshot '$SNAP'"
