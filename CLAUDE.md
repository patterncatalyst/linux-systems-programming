# Linux Systems Programming — project conventions

Tutorial site: Linux systems programming in C++23, Go, and Rust for
intermediate→advanced developers. Jekyll/GitHub Pages (built in CI — no local
Ruby), examples in `examples/NN-slug/{cpp,go,rust}/`, chapters in
`_docs/NN-slug.md` with the same NN.

## Scope rules

- **eBPF is tooling only** (bcc-tools, bpftrace, bpftool) — we observe our own
  userspace programs; we never write kernel eBPF.
- Examples run **locally** unless they need root, kernel observation, or a
  second host — then the KVM lab: `systems-target` / `systems-peer`
  (`scripts/lab/`, LAB_PREFIX=systems, Fedora 44 guests, snapshot `lab-ready`).
- Containers: multi-stage **Containerfile** (never "Dockerfile"), **UBI 10**
  bases, rootless podman.

## Toolchain pins

- C++23 — system GCC (floor 14) primary, clang first-class alternate presets;
  CMake ≥ 3.25, Presets v6, Ninja, `build/${presetName}`; Conan 2 only where
  third-party deps exist.
- Go — `go.mod` `go 1.26` + `toolchain go1.26.5`.
- Rust — `rust-toolchain.toml` channel 1.97.1, edition 2024.
- Lua 5.4 (LuaJIT accepted locally) drives `verify.lua`; Python 3 drives
  `scripts/test-all-examples.py` and `scripts/validate.py`.

## Chapter conventions

- Front matter: `title/order/part/description/duration`; `part:` must equal a
  `_parts` `part_name` exactly; quote values containing colons.
- Spine: hook → figure → concept → "How the code works" → "Errors, three ways"
  → "Concurrency lens" → "Build, run, observe" → cross-check → "What you
  learned" → status footer.
- **Tools used box (required)**: immediately after the intro/figure, a callout
  listing every tool the chapter invokes and where it runs, e.g.
  `> **Tools used** — `strace` (host), `gdbserver` (systems-target VM),
  `podman` (host). All appear in `scripts/check-host.sh` or the VM cloud-init.`
  Every tool named must actually be exercised in the chapter.
- Code tabs: `{% include codetabs.html langs="C++|Go|Rust" %}` + exactly one
  fenced block per label, in order, each a **verbatim excerpt** of the real
  example source.
- Command prefixes: `[host]$`, `[vm]$`, `[peer]$`.
- Wrap literal `{{ }}`/`{% %}` in `{% raw %}…{% endraw %}` in prose.
- Diagrams: paired SVG+Excalidraw via `scripts/generate_diagram.py`
  (`g.OUT="assets/diagrams"`); catalogue every diagram in
  `assets/diagrams/README.md`; embed via the `excalidraw.html` include with a
  `Figure N.x` caption.
- **Verification discipline**: a demo is *verified* only when it produced its
  claimed observable effect on the stated environment; footers
  (`status--verified` / `status--unverified`) state the behavioral evidence.
  Every number quoted as "from a run" must come from a real run.
- **Banned words**: never use "honest", "honestly", "to be honest" — write
  "real", "practical", "accurate" instead.

## Examples

- Mint new examples with `scripts/new-example.sh NN-slug`; register in
  `examples/manifest.yaml` (`mode: local|vm|vm-peer`, `requires: [lgtm]`).
- Identical observable behavior across the three languages — one `verify.lua`
  (checks.lua API) asserts behavior, never merely exit 0.
- Demo contract: `./demo.sh [cpp|go|rust] [build|run]`; `TARGET=<vm>` deploys
  via `scripts/lab/deploy-to-vm.sh` (`SUDO=1` for root demos).
- Runner: `python3 scripts/test-all-examples.py` — VM examples strictly
  serial; SKIP (not FAIL) when lab/LGTM is down.

## Git

- One branch per iteration (`docs/rNN-…`), PR, squash merge to main, delete
  branch. Never commit directly to main. Conventional Commits:
  types docs/site/demo/ci/chore/fix/feat/refactor/style; scopes `§N`,
  `demo-NN`, `rNN`.
- CI (`validate.yml`) checks site integrity + build smoke only; the
  authoritative gate is a host run of the runner with the lab up, recorded in
  `_plans/`.
