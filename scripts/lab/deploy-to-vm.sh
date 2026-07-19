#!/usr/bin/env bash
# deploy-to-vm.sh — copy a compiled binary to a lab guest and run it.
#
#   ./deploy-to-vm.sh systems-target ./build/release/app
#   ./deploy-to-vm.sh systems-target ./build/release/app -- --port 9000
#   SUDO=1 ./deploy-to-vm.sh systems-target ./build/release/pmon   # run as root
#
# Everything after `--` is passed to the binary on the guest. Most demos in
# this book run unprivileged; set SUDO=1 for the ones that genuinely need root
# (capabilities, namespaces, cgroup writes). The lab user has passwordless sudo.
set -euo pipefail
VM="${1:?usage: deploy-to-vm.sh <vm-name> <local-binary> [-- args...]}"
BIN="${2:?need a local binary path}"
shift 2
[[ "${1:-}" == "--" ]] && shift || true

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IP="$("$SCRIPT_DIR/vm-ip.sh" "$VM")"
SSH_OPTS="-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$HOME/.ssh/known_hosts"
REMOTE="/home/fedora/$(basename "$BIN")"

echo "→ copying $(basename "$BIN") to fedora@$IP:$REMOTE"
scp $SSH_OPTS "$BIN" "fedora@$IP:$REMOTE"
ssh $SSH_OPTS "fedora@$IP" "chmod +x '$REMOTE'"

# Demos set OTEL_ENDPOINT to the host stack (http://<gateway>:4318). sudo strips
# the environment, so forward it explicitly as OTEL_EXPORTER_OTLP_ENDPOINT via
# `env` — otherwise the guest binary falls back to its own localhost and no
# telemetry reaches the host stack.
RENV=""
[[ -n "${OTEL_ENDPOINT:-}" ]] && RENV="OTEL_EXPORTER_OTLP_ENDPOINT='$OTEL_ENDPOINT'"

echo "→ running on $VM (Ctrl-C to stop):"
if [[ "${SUDO:-0}" == "1" ]]; then
  exec ssh -t $SSH_OPTS "fedora@$IP" "sudo env $RENV '$REMOTE' $*"
else
  exec ssh -t $SSH_OPTS "fedora@$IP" "env $RENV '$REMOTE' $*"
fi
