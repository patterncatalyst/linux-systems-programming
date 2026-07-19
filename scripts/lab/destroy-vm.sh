#!/usr/bin/env bash
# destroy-vm.sh — tear a guest all the way down, including its disks.
#   ./destroy-vm.sh myproject-target
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
LAB_PREFIX="${LAB_PREFIX:-systems}"
CACHE_DIR="${LAB_CACHE:-$HOME/.cache/${LAB_PREFIX}-lab}"
VM="${1:?usage: destroy-vm.sh <vm-name>}"
virsh destroy "$VM" 2>/dev/null || true
virsh undefine "$VM" --remove-all-storage --nvram 2>/dev/null \
  || virsh undefine "$VM" 2>/dev/null || true
rm -f "$CACHE_DIR/${VM}.qcow2" "$CACHE_DIR/${VM}-seed.img"
echo "destroyed $VM"
