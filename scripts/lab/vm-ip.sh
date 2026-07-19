#!/usr/bin/env bash
# vm-ip.sh — print the IPv4 address libvirt leased to a guest.
#   ./vm-ip.sh myproject-target
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
VM="${1:?usage: vm-ip.sh <vm-name>}"
# Try the DHCP lease via the domain's interface; fall back to guest agent.
ip=$(virsh -q domifaddr "$VM" 2>/dev/null | awk '/ipv4/ {sub(/\/.*/,"",$4); print $4; exit}')
if [[ -z "${ip:-}" ]]; then
  ip=$(virsh -q domifaddr "$VM" --source agent 2>/dev/null | awk '/ipv4/ {sub(/\/.*/,"",$4); print $4; exit}')
fi
[[ -n "${ip:-}" ]] || { echo "no lease yet for $VM — wait for cloud-init and retry" >&2; exit 1; }
echo "$ip"
