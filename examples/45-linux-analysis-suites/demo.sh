#!/usr/bin/env bash
# demo.sh [cpp|go|rust] [build|run]
#
# No per-language source here: PCP watches a system, not a language --
# there's nothing C++/Go/Rust-specific to build. The [cpp|go|rust] argument
# only satisfies the runner's contract (scripts/test-all-examples.py loops
# langs x {build,verify}); the image built and run here is identical no
# matter which is passed.
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=lsp45-pcp
CONTAINER=lsp45-pcp-demo

case "${1:-}" in
  cpp|go|rust) shift ;;
  *) echo "usage: $0 [cpp|go|rust] [build|run]" >&2; exit 2 ;;
esac

build() {
  podman build -t "$IMAGE" -f Containerfile .
}

run() {
  podman rm -f "$CONTAINER" >/dev/null 2>&1 || true

  # If a rootless podman.sock is up for this user, mount it so the layered
  # podman PMDA sees REAL running containers (including this one) instead
  # of just registering with zero values. This is a genuine, verified
  # result: pmda-podman's own Install script only checks for
  # /run/podman/podman.sock to decide whether to prompt about starting the
  # podman service -- it works identically well whether the socket behind
  # that path belongs to root or, as here, an unprivileged user.
  local sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/podman/podman.sock"
  local mount_args=()
  if [ -S "$sock" ]; then
    mount_args=(-v "$sock:/run/podman/podman.sock")
  else
    echo "note: no rootless podman.sock at $sock -- podman PMDA will" \
         "register but report zero container values" >&2
  fi

  podman run -d --name "$CONTAINER" --cpus=2 --memory=256m \
    "${mount_args[@]}" "$IMAGE"

  for _ in $(seq 1 20); do
    podman exec "$CONTAINER" pminfo -f pmcd.version >/dev/null 2>&1 && break
    sleep 1
  done

  echo "--- pcp: suite summary (openmetrics/podman should both be listed) ---"
  podman exec "$CONTAINER" pcp

  echo "--- pminfo: the two layered PMDAs' namespaces ---"
  podman exec "$CONTAINER" pminfo openmetrics podman

  echo "--- pminfo -f: a real scraped openmetrics value (file:// source, no network) ---"
  podman exec "$CONTAINER" pminfo -f openmetrics.lsp45.lsp45_answer

  if [ -S "$sock" ]; then
    echo "--- pminfo -f: real podman container visibility (via the mounted socket) ---"
    podman exec "$CONTAINER" pminfo -f podman.container.name podman.container.running
  fi

  echo "--- pmrep: one live sample of a core, always-present metric ---"
  podman exec "$CONTAINER" pmrep -t 1 -s 1 kernel.all.load

  echo "container left running: podman exec/logs it, or podman rm -f $CONTAINER"
}

case "${1:-}" in
  build) build ;;
  run)   build; run ;;
  "")    build; run ;;
  *)     echo "usage: $0 [cpp|go|rust] [build|run]" >&2; exit 2 ;;
esac
