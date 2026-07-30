---
title: "r14 / ch45 — plan (internal)"
published: false
---

# r14 ch45 — `_docs/45-linux-analysis-suites.md` + 2 diagrams (lgtm-relay Phase 1)

Model relay: Opus plan (this file) → Sonnet execute → Opus validate.
Example already built + verified: `examples/45-linux-analysis-suites/` (podman PASS 10/0, 265 MB).
Evidence: `scratchpad/EVIDENCE45-podman.md`, `scratchpad/EVIDENCE45-vm.md`, `scratchpad/spec45/*`.

## Approach
Appendices-part reference chapter. Throughline: prior tools watched ONE process
(strace/gdb/bcc, ch30); ch45 pivots to three fleet-wide *suites* with three
philosophies — Cockpit (zero-scripting web console), SystemTap (bcc's
kernel-module-compiling sibling), PCP (pluggable metric collector pulling from
podman/ch34 + OpenMetrics/ch38). Runnable artifact = PCP container ONLY; Cockpit
+ SystemTap demonstrated on systems-target. Adapts (never silently drops) the
per-language spine, as ch44 did for single-language Go. Footer carries BOTH a
`status--verified` span (podman run + VM SystemTap + Cockpit socket) AND a
narrowly-scoped `status--unverified` span (Cockpit web-UI panels: described, not
browser-automated). No `status--verified-with-caveats` class exists — two-span
mix is the established equivalent.

Rejected: codetabs include (single-lang, follow ch44 plain fences); rewriting the
superseded design-doc §1/§2 (UBI/pmda-bcc — describe the BUILT artifact); splitting
prose into parallel writers (one file → collision).

## Steps (Phase 2 fan-out)
- **S1 — Diagram A** `assets/diagrams/45-analysis-suite-landscape.{svg,excalidraw}` (parallel). Three lanes: Cockpit (browser→cockpit-ws→systemd/podman; socket VERIFIED, panels described); SystemTap (.stp→stap→C→kernel module, amber = VERIFIED stap-module-ok, vs ch30 dyninst; needs kernel-devel); PCP (pmcd center, two amber arrows in — pmda-openmetrics from `file:///etc/lsp45/lsp45.prom`, pmda-podman from `podman.sock (ch34)`; dashed ghost `pmda-bcc — NOT shipped, super-privileged, ch30 on VM`). Caption Figure 45.1.
- **S2 — Diagram B** `assets/diagrams/45-pcp-build-gate.{svg,excalidraw}` (parallel). builder→smoke→runtime; highlight `FROM smoke` edge (forces gate to build); dashed side box Cockpit+SystemTap on systems-target. Caption Figure 45.2.
- **S3 — Chapter prose** `_docs/45-linux-analysis-suites.md` (parallel; sole editor of this file). Front matter part="Appendices: Tooling" (VERIFIED exact), order 45, quoted description. Full spine. "Why Fedora not UBI" paragraph. Both real bugs (FROM builder stage-skip; ./Install systemd-revert fixed by pmcd-first) in How-it-works/Errors. Verbatim excerpts from Containerfile/config/demo.sh/verify.lua. Tools-used box. All run-numbers trace to EVIDENCE files.
- **S4 — Catalogue** `assets/diagrams/README.md` — 2 new rows (after S1+S2; sole editor of README).
- **S5 — Manifest** — CONFIRM ONLY, no edit (id 45 already correct).

Parallel: S1, S2, S3 independent (distinct files). S4 after S1+S2.

## Acceptance criteria
1. part == "Appendices: Tooling" (exact); order 45; description quoted.
2. YAML parses; full spine in order.
3. Tools-used box right after intro/figure; every named tool exercised.
4. Every example fenced block = byte-for-byte substring of its named file.
5. Two excalidraw embeds file="45-analysis-suite-landscape" / "45-pcp-build-gate", alt + Figure 45.1/45.2; both pairs exist.
6. README.md +2 chapter-45 rows.
7. pmda-bcc stated excluded (super-privileged), tied to ch30; not shown shipped in either diagram.
8. Both real bugs present (FROM builder stage-skip + fix; ./Install revert + pmcd-first fix).
9. "Why Fedora not UBI" paragraph present.
10. Footer: status--verified span (podman+VM SystemTap+Cockpit socket) AND status--unverified span naming Cockpit web-UI panels as described.
11. Every "from a run" number matches an EVIDENCE file exactly (265 MB, PASS 10/0, value 42, 7.1.5-3/11 agents, 0.970/0.540/0.420, "lsp45-pcp-demo", HTTP 200, stap-module-ok, 155/43 openat). Nothing else.
12. Zero honest/honestly/to-be-honest; each lie/lying (if any) machine-subject only. ("Honest:" from evidence must not leak.)
13. manifest.yaml unchanged, id 45 correct.
14. No literal {{ }}/{% %} in prose outside includes/relative_url/raw; SVGs well-formed; excalidraw valid JSON.

## Risks
Non-verbatim excerpts (crit 4 grep); fabricated plausible number (crit 11 grep); over-claiming
Cockpit UI verified (crit 10 + footer read); diagram shows pmda-bcc shipped (crit 7); embed/file/catalogue
name mismatch (crit 5-6); invented CSS class (crit 10 two-span mix); "Honest:" leak (crit 12); diagram
edge-into-space / hidden label (band for container boxes, edges terminate on box edge).

## Status
- [ ] S1 diagram A
- [ ] S2 diagram B
- [ ] S3 prose
- [ ] S4 catalogue
- [ ] S5 manifest confirm
- [ ] Phase 3 validate
