#!/usr/bin/env bash
# lab-ips.sh — print the IPs of the two lab guests, for tests that drive traffic
# between them.
#   eval "$(./lab-ips.sh)"   # exports TARGET_IP and PEER_IP
set -euo pipefail
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LAB_PREFIX="${LAB_PREFIX:-systems}"
TARGET="${TARGET_VM:-${LAB_PREFIX}-target}"; PEER="${PEER_VM:-${LAB_PREFIX}-peer}"
ti="$("$SCRIPT_DIR/vm-ip.sh" "$TARGET" 2>/dev/null || true)"
pi="$("$SCRIPT_DIR/vm-ip.sh" "$PEER" 2>/dev/null || true)"
echo "export TARGET_IP=${ti}"
echo "export PEER_IP=${pi}"
[ -z "$pi" ] && echo "# peer '$PEER' has no IP — provision it: ./provision-vm.sh $PEER" >&2 || true
