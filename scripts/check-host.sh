#!/usr/bin/env bash
# Verify the Fedora 44 host has every prerequisite the demos and the
# KVM lab need. Prints a table of PASS / WARN / FAIL lines and exits
# non-zero if any hard requirement fails.
#
# Run this once after following §2's installation steps, and re-run
# any time you change toolchains.
#
# Deliberately no -e: failing checks are recorded in the table, not
# fatal on the spot.
set -uo pipefail

# ── Colors ───────────────────────────────────────────────────────────────
# Honor NO_COLOR if set or if stdout is not a tty.
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_RESET=$'\033[0m'
else
    C_RED=""
    C_GREEN=""
    C_YELLOW=""
    C_RESET=""
fi

log_ok()   { printf '%s[ ok ]%s  %s\n' "$C_GREEN"  "$C_RESET" "$*"; }
log_warn() { printf '%s[warn]%s  %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
log_err()  { printf '%s[fail]%s  %s\n' "$C_RED"    "$C_RESET" "$*" >&2; }

# ── Status table accumulator ─────────────────────────────────────────────
# Status values:
#   ok    — hard requirement satisfied
#   fail  — hard requirement missing; gates exit code
#   warn  — soft requirement missing; informational, doesn't gate exit
PASS=0; FAIL=0; WARN=0
TABLE=()

record() {
    local status="$1"; shift
    local label="$1"; shift
    local detail="$1"; shift
    local hint="${1:-}"
    case "$status" in
        ok)
            PASS=$((PASS + 1))
            TABLE+=("$(printf '%s[ ok ]%s  %-32s  %s' "$C_GREEN" "$C_RESET" "$label" "$detail")")
            ;;
        warn)
            WARN=$((WARN + 1))
            TABLE+=("$(printf '%s[warn]%s  %-32s  %s' "$C_YELLOW" "$C_RESET" "$label" "$detail")")
            if [[ -n "$hint" ]]; then
                TABLE+=("        ↳ $hint")
            fi
            ;;
        *)
            FAIL=$((FAIL + 1))
            TABLE+=("$(printf '%s[fail]%s  %-32s  %s' "$C_RED" "$C_RESET" "$label" "$detail")")
            if [[ -n "$hint" ]]; then
                TABLE+=("        ↳ $hint")
            fi
            ;;
    esac
}

# check_version <label> <cmd> <version_arg> <min_major> <min_minor> [pkg]
# Hard requirement: cmd exists and reports >= min_major.min_minor.
check_version() {
    local label="$1" cmd="$2" version_arg="$3" min_major="$4" min_minor="$5"
    local pkg="${6:-$2}"
    local found
    if ! command -v "$cmd" >/dev/null 2>&1; then
        record fail "$label" "not installed" "sudo dnf install -y $pkg"
        return
    fi
    found="$($cmd $version_arg 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    if [[ -z "$found" ]]; then
        record fail "$label" "unknown version" "verify with: $cmd $version_arg"
        return
    fi
    local fmaj="${found%%.*}"
    local fmin="${found#*.}"; fmin="${fmin%%.*}"
    if (( fmaj > min_major )) || { (( fmaj == min_major )) && (( fmin >= min_minor )); }; then
        record ok "$label" "$found"
    else
        record fail "$label" "$found" "Need >= $min_major.$min_minor; upgrade $pkg."
    fi
}

# ── Distro ───────────────────────────────────────────────────────────────
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ "${ID:-}" == fedora ]]; then
        record ok "fedora baseline" "$PRETTY_NAME"
    else
        record warn "fedora baseline" \
            "$PRETTY_NAME" \
            "The book targets Fedora 44; expect package-name differences."
    fi
fi

# ── C/C++ toolchain ──────────────────────────────────────────────────────
check_version "gcc >= 14"    gcc   --version 14 0  gcc
check_version "g++ >= 14"    g++   --version 14 0  gcc-c++
check_version "cmake >= 3.25" cmake --version 3 25 cmake

if command -v clang >/dev/null 2>&1; then
    record ok "clang" "$(clang --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
else
    record fail "clang" "not installed" "sudo dnf install -y clang"
fi

if command -v ninja >/dev/null 2>&1; then
    record ok "ninja" "$(ninja --version 2>/dev/null | head -1)"
else
    record fail "ninja" "not installed" "sudo dnf install -y ninja-build"
fi

# ── Go ───────────────────────────────────────────────────────────────────
# Any modern go works to bootstrap: the go.mod toolchain directive
# auto-downloads go1.26.5 on first build.
if command -v go >/dev/null 2>&1; then
    GO_VER="$(go version 2>/dev/null | grep -oE 'go[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    if [[ "$GO_VER" == go1.26* ]]; then
        record ok "go 1.26.x" "$GO_VER"
    else
        record warn "go 1.26.x" \
            "${GO_VER:-unknown}" \
            "toolchain directive auto-downloads go1.26.5 on first build."
    fi
else
    record fail "go 1.26.x" "not installed" "sudo dnf install -y golang"
fi

# ── Rust ─────────────────────────────────────────────────────────────────
# rustc must come from rustup so the pinned channel in rust-toolchain.toml
# takes effect (Fedora's system rustc ignores it).
if command -v rustup >/dev/null 2>&1 && command -v rustc >/dev/null 2>&1; then
    RUSTC_VER="$(rustc --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    if [[ "$RUSTC_VER" == "1.97.1" ]]; then
        record ok "rustc via rustup" "$RUSTC_VER"
    else
        record warn "rustc via rustup" \
            "${RUSTC_VER:-unknown} (book pins 1.97.1)" \
            "rust-toolchain.toml installs 1.97.1 on first build; or: rustup toolchain install 1.97.1"
    fi
else
    record fail "rustc via rustup" \
        "rustup and/or rustc not installed" \
        "curl https://sh.rustup.rs -sSf | sh   (do not use Fedora's system rust)"
fi

if command -v cargo >/dev/null 2>&1; then
    record ok "cargo" "$(cargo --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
else
    record fail "cargo" "not installed" "installed by rustup alongside rustc"
fi

# ── Lua / Python ─────────────────────────────────────────────────────────
check_version "lua >= 5.4" lua -v 5 4 lua

if command -v python3 >/dev/null 2>&1; then
    record ok "python3" "$(python3 --version 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
else
    record fail "python3" "not installed" "sudo dnf install -y python3"
fi

# ── Containers ───────────────────────────────────────────────────────────
check_version "podman >= 5.0" podman --version 5 0

# ── KVM lab (libvirt) ────────────────────────────────────────────────────
for pair in virsh:libvirt-client virt-install:virt-install qemu-img:qemu-img cloud-localds:cloud-utils; do
    cmd="${pair%%:*}"; pkg="${pair##*:}"
    if command -v "$cmd" >/dev/null 2>&1; then
        record ok "$cmd" "$(command -v "$cmd")"
    else
        record fail "$cmd" "not installed" "sudo dnf install -y $pkg"
    fi
done

if [[ -e /dev/kvm ]]; then
    record ok "/dev/kvm" "present"
else
    record fail "/dev/kvm" \
        "missing" \
        "Enable VT-x/AMD-V in firmware; on a VM host, enable nested virt."
fi

# Warn-only: without libvirt group membership every virsh call needs sudo.
if id -nG 2>/dev/null | tr ' ' '\n' | grep -qx libvirt; then
    record ok "libvirt group" "$(id -un) is a member"
else
    record warn "libvirt group" \
        "$(id -un) not a member" \
        "sudo usermod -aG libvirt $(id -un) && re-login."
fi

# ── Misc tooling ─────────────────────────────────────────────────────────
for tool in git gh; do
    if command -v "$tool" >/dev/null 2>&1; then
        record ok "$tool" "$(command -v "$tool")"
    else
        record fail "$tool" "not installed" "sudo dnf install -y $tool"
    fi
done

# ── Soft requirements (warn only) ────────────────────────────────────────
# Conan 2.x — only some C++ demos use it.
if command -v conan >/dev/null 2>&1; then
    CONAN_VER="$(conan --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    if [[ "${CONAN_VER%%.*}" == "2" ]]; then
        record ok "conan 2.x" "$CONAN_VER"
    else
        record warn "conan 2.x" "${CONAN_VER:-unknown} (need 2.x)" \
            "pip install --user 'conan>=2.0,<3.0'"
    fi
else
    record warn "conan 2.x" "not installed" "pip install --user 'conan>=2.0,<3.0'"
fi

# ruby + bundler — only for previewing the Jekyll site locally.
if command -v ruby >/dev/null 2>&1 && command -v bundler >/dev/null 2>&1; then
    record ok "ruby + bundler" "$(ruby --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
else
    record warn "ruby + bundler" "not installed" \
        "sudo dnf install -y ruby rubygem-bundler   (local Jekyll preview only)"
fi

# Go linters / debugger — used by CI; nice to have locally.
for pair in "golangci-lint:see golangci-lint.run/docs/welcome/install" \
            "staticcheck:go install honnef.co/go/tools/cmd/staticcheck@latest" \
            "dlv:go install github.com/go-delve/delve/cmd/dlv@latest"; do
    cmd="${pair%%:*}"; hint="${pair#*:}"
    if command -v "$cmd" >/dev/null 2>&1; then
        record ok "$cmd" "$(command -v "$cmd")"
    else
        record warn "$cmd" "not installed" "$hint"
    fi
done

# ── Print the table ──────────────────────────────────────────────────────
echo
printf '%s\n' "${TABLE[@]}"
echo
TOTAL=$((PASS + FAIL))
if (( FAIL == 0 )); then
    log_ok "All ${TOTAL} required checks passed."
    if (( WARN > 0 )); then
        log_warn "${WARN} optional check(s) flagged warnings — review the table."
    fi
    exit 0
else
    log_err "${FAIL} of ${TOTAL} required checks failed. See messages above."
    if (( WARN > 0 )); then
        log_warn "Plus ${WARN} optional warning(s)."
    fi
    exit 1
fi
