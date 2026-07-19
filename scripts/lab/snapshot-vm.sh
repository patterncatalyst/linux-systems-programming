#!/usr/bin/env bash
# snapshot-vm.sh — capture a named snapshot of a lab guest (default: lab-ready).
#   ./snapshot-vm.sh myproject-target                 # snapshot named "lab-ready"
#   ./snapshot-vm.sh myproject-target clean-updated   # a differently-named snapshot
#
# Take a snapshot after the guest is fully provisioned + tooled (cloud-init done,
# your extra packages installed) so you can revert to a known-good state in
# seconds instead of re-provisioning. Snapshots of a running guest capture RAM +
# disk, so a revert restores it running.
#
# Uses the system libvirt instance; if your login shell isn't in the `libvirt`
# group yet, run via: sg libvirt -c '...'.
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
VM="${1:?usage: snapshot-vm.sh <vm-name> [snapshot-name]}"
SNAP="${2:-lab-ready}"
virsh dominfo "$VM" >/dev/null 2>&1 || { echo "no such domain: $VM" >&2; exit 1; }
# Replace an existing snapshot of the same name so re-running is idempotent.
virsh snapshot-delete "$VM" "$SNAP" >/dev/null 2>&1 || true
virsh snapshot-create-as "$VM" "$SNAP" \
  --description "lab snapshot taken $(date -u +%FT%TZ)" --atomic
echo "snapshot '$SNAP' created for $VM"
virsh snapshot-list "$VM"
