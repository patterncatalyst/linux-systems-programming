---
title: "r15 / ch46 — cpp-toolbox — plan (internal)"
published: false
---

# r15 ch46 — example `46-cpp-toolbox` + chapter + 2 diagrams (lgtm-relay Phase 1)

Model relay: Opus plan (this file) → Sonnet execute → Opus validate.
First C++ toolchain reference-depth appendix (Part 13 "Appendices: Tooling").
Single-language `langs: [cpp]`, `mode: local`, no VM/root/podman/LGTM.

## Approach
One deterministic C++23 `toolbox` binary + toolchain artifacts, engineered so every
tool produces a CHECKABLE effect (not exit-0). Subcommands + auxiliary depth payload
(`.gdbinit`, gdb Python printer, `.clang-tidy`, `.clang-format`, Conan sub-target +
lockfile, misformatted fixture).

**Hard-gated (tools check-host.sh guarantees; deterministic on pinned toolchain):**
- **A. Dual-compiler byte parity.** `toolbox report` prints an integer/string-only FNV-1a
  digest (no floats/addresses/timing/unordered iteration). verify builds `release` (GCC)
  + `release-clang`, runs both, asserts byte-identical AND equals an expected literal.
  Chapter separately SHOWS GCC-vs-clang diagnostic differences (shown, not gated — wording drifts).
- **B. UBSan recipe.** `toolbox defect overflow` seeds signed-int overflow; `ubsan` preset
  catches it; verify asserts exact `runtime error: signed integer overflow`. Defers full
  matrix to ch29.
- **C. gdb pretty-printer.** custom `Digest{uint64_t}` + Python printer; verify runs
  `gdb -batch -x cpp/.gdbinit ...` and asserts the pretty form.

**Gated-if-present (checks.skip-guarded):** D clang-tidy (assert pinned check-name TOKEN,
not message text) · E clang-format `--dry-run --Werror` 0 on tracked, nonzero on fixture ·
F Conan 2 with committed lockfile, isolated sub-target (NOT in default build → CI stays green).

**Shown-not-gated (absent even on reference host):** ccache second-build cache-hit; mold in
`.comment`. Footer marks unverified. Accurate host state.

## Chapter
Single-language spine (ch44 precedent, plain fenced cpp blocks, no codetabs include):
hook → Fig 46.1 → Tools-used box → CMake presets in depth → Conan 2 lockfiles → GCC-vs-clang
parity+diagnostics → How the code works → Errors-three-ways (compile diagnostic / tidy finding /
UBSan abort) → Concurrency lens (build-graph parallelism: Ninja -j, mold parallel linker) →
gdb printers/.gdbinit → lint/format/sanitize/ccache/mold recipes → build/run/observe → cross-check
(the parity diff IS the cross-check) → what you learned → status footer.
Differentiate (cross-ref, don't re-teach): vs ch31 (3-lang profiling breadth), ch28 (gdb remote/core),
ch29 (sanitizer matrix). ch46 = C++ build/debug DEPTH: preset internals, Conan lockfiles, cross-compiler
parity, gdb Python printers, .clang-tidy/.clang-format authoring, ccache/mold.

Rejected: reuse ch29 bugfarm (re-teaches matrix); Conan mandatory in default build (breaks fedora:44
CI smoke); hard-gate ccache/mold (absent on host); gate exact tidy/GCC message text (drifts).

## Steps (Phase-2 parallelism)
1. Mint+strip scaffold `examples/46-cpp-toolbox/` (new-example.sh, delete go/rust, single-lang demo.sh). BLOCKING. Not parallel.
2. Example sources+artifacts: cpp/src/toolbox.cpp (+ custom-type header, tidy-smell TU, defect path), CMakeLists.txt, CMakePresets.json (release/debug/asan/ubsan/release-clang/debug-clang/conan), .clang-tidy, .clang-format, .gdbinit, toolbox-printers.py, fixtures/misformatted.cpp, conan/conanfile.py + lockfile, cpp/demo.sh, README.md. Depends 1.
3. verify.lua (gates A-C hard, D-F skip-if-absent, ccache/mold opportunistic) + single-lang LSP_LANG guard. Depends 2.
4. Local build+verify+transcript capture on host (all presets; hand-run gdb/tidy/format/UBSan/Conan/ccache/mold for REAL output; confirm tidy token fires on host clang-tidy, swap smell if not). Depends 2+3. Gates prose.
5. Chapter `_docs/46-cpp-toolbox.md`. Depends 4. Parallel with 6.
6. 2 diagrams (46-cmake-preset-graph, 46-toolbox-pipeline) + 2 README rows. Depends 2. Parallel with 5.
7. manifest.yaml +entry (langs:[cpp], mode:local, timeout 420). After 1.

Collisions: assets/diagrams/README.md (step 6) + examples/manifest.yaml (step 7) are the only shared files; one edit each.

## Acceptance criteria
1. No go/rust dir; manifest langs:[cpp], mode:local, no requires.
2. ./demo.sh cpp build exits 0 (release + release-clang).
3. diff release vs release-clang `report` empty AND == asserted literal.
4. LSP_LANG=cpp lua verify.lua → PASS N/FAIL 0; hard asserts A (parity), B (UBSan signed overflow), C (gdb printer) — none exit-0.
5. clang-tidy: output has pinned check-name token on expected line (skip if absent).
6. clang-format --dry-run --Werror 0 on tracked, nonzero on fixture (skip if absent).
7. Conan install+preset build+run 0, prints expected line (skip if absent); lockfile committed.
8. ccache/mold in chapter, opportunistic only, never in hard PASS, footer unverified.
9. scripts/validate.py → validate: OK.
10. banned-words grep clean (or machine-subject only).
11. every chapter cpp block = verbatim substring of example source.
12. test-all-examples.py --only 46-cpp-toolbox → PASS; footer status--verified with behavioral evidence.

## Risks
tidy/format version drift (assert token only, confirm on host step 4); parity break from float/addr/timing
(integer/string-only digest); gdb auto-load/ptrace (explicit -x, trivial printer, batch debugs own child);
ccache/mold absent (skip/opportunistic); Conan in CI (isolated sub-target); ninja PATH (sandbox artifact,
check-host gates it).

## Verification outlook
CLEAN — cleaner than ch45. Local/no-VM/no-root. Hard core A-C deterministic on pinned toolchain, passes on
any conforming host, richer on reference host. Only unverified corners: ccache/mold (absent) + diagnostic-wording
asides — never gated. Should merge on standard host-gate + CI-green, no mixed caveat.

## Status — DONE
- [x] S1-S4 example (scaffold→sources→verify.lua→build+capture): PASS 20/0, all tools present+firing
- [x] S5 chapter `_docs/46-cpp-toolbox.md` (815 lines)
- [x] S6 diagrams (46-cmake-preset-graph, 46-toolbox-pipeline) + README rows
- [x] S7 manifest (langs:[cpp], mode:local, timeout 420)
- [x] Phase 3 validate (Opus): all 12 criteria MET, verdict **SHIP**, zero MUST-FIX (fixed one `cmd`→`cmake` typo)

## Gate (host run, 2026-08-01/02)
- `verify.lua` PASS 20/0 · `validate.py` → validate: OK · `test-all-examples --only 46-cpp-toolbox` PASS
- Verification CLEAN (local, no VM). Only ccache/mold shown-not-gated (absent on host); GCC-vs-clang diagnostic asides framed as illustrative live-session, not gated.
- Reference toolchain: GCC 16.1.1, clang 22.1.8, CMake 4.3.0, gdb 17.2, Conan 2.30.0, kernel 7.1.4-204.fc44.

## Merge: AUTONOMOUS (user authorized for r15).
