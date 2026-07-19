#!/usr/bin/env bash
# Shared helpers for example demo scripts and the aggregator. Source this;
# don't execute it directly. Handles colors, repo-root resolution, and
# HTTP readiness waiting.

if [[ -t 1 ]]; then
    GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    GREEN=''; RED=''; YELLOW=''; CYAN=''; BOLD=''; NC=''
fi

step()  { echo -e "${CYAN}━━ $*${NC}"; }
pass()  { echo -e "${GREEN}✓ $*${NC}"; }
fail()  { echo -e "${RED}✗ $*${NC}" >&2; exit 1; }
info()  { echo -e "${YELLOW}  $*${NC}"; }

repo_root() {
    if git rev-parse --show-toplevel >/dev/null 2>&1; then
        git rev-parse --show-toplevel
    else
        cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
    fi
}

# Wait up to N seconds for an HTTP endpoint. Use 127.0.0.1 not localhost.
wait_for_http() {
    local url="$1" timeout="${2:-30}" i
    for ((i = 0; i < timeout; i++)); do
        curl -fsS "$url" >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}
