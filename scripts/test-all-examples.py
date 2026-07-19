#!/usr/bin/env python3
"""Test orchestrator for the book's runnable examples.

Reads examples/manifest.yaml, builds each selected example x language with
"./demo.sh <lang> build" (optionally in parallel), then verifies sequentially
with "lua verify.lua" (env LSP_LANG / EXAMPLE_DIR / REPO_ROOT; exit 77 = SKIP).

VM-mode examples (mode: vm | vm-peer) verify STRICTLY serially against the lab
guests; the lab is preflighted with scripts/lab/vm-ip.sh and an unreachable
guest turns the example into a SKIP ("lab down"), never a FAIL. Examples with
requires: [lgtm] are skipped when the local OTLP endpoint (localhost:4318)
does not answer. Stdlib only.

Usage:
  scripts/test-all-examples.py [--only ID]... [--lang cpp|go|rust]...
      [--mode local|vm|all] [--revert-between] [--junit PATH] [--jobs N]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "examples" / "manifest.yaml"
LOG_DIR = REPO_ROOT / "build-logs"  # gitignored scratch output

LANGS = ("cpp", "go", "rust")
LAB_PREFIX = os.environ.get("LAB_PREFIX", "systems")
TARGET_VM = f"{LAB_PREFIX}-target"
PEER_VM = f"{LAB_PREFIX}-peer"
LGTM_PROBE_URL = "http://localhost:4318"
# Lua interpreter for verify.lua: $LUA overrides; else lua, else luajit.
LUA = os.environ.get("LUA") or shutil.which("lua") or shutil.which("luajit") or "lua"

BUILD_TIMEOUT = 900  # seconds per example-lang build
VERIFY_TIMEOUT = 300  # seconds per verify.lua run


# ---------------------------------------------------------------------------
# Manifest parsing
# ---------------------------------------------------------------------------

class ManifestError(Exception):
    pass


def parse_manifest_subset(text: str) -> list[dict]:
    """Strict-subset YAML parser used when PyYAML is unavailable.

    Supports exactly the documented manifest schema and nothing more:
      * full-line comments (#...) and blank lines
      * one top-level key "examples:" introducing a list
      * list items "- key: value" with further "key: value" lines indented
        to the same column
      * values are plain/quoted scalars or flow lists like [cpp, go]
    No nested mappings, block lists, multiline scalars, anchors, or tags.
    """
    item_re = re.compile(r"^(\s*)-\s+([A-Za-z0-9_-]+):\s*(.*)$")
    kv_re = re.compile(r"^(\s*)([A-Za-z0-9_-]+):\s*(.*)$")

    def unquote(s: str) -> str:
        if len(s) >= 2 and s[0] == s[-1] and s[0] in "'\"":
            return s[1:-1]
        return s

    def parse_value(v: str, lineno: int):
        v = v.strip()
        if v.startswith("[") and v.endswith("]"):
            inner = v[1:-1].strip()
            return [unquote(p.strip()) for p in inner.split(",")] if inner else []
        if not v:
            raise ManifestError(f"manifest line {lineno}: empty value (block collections unsupported)")
        return unquote(v)

    examples: list[dict] = []
    current: dict | None = None
    in_examples = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not raw[0].isspace() and stripped != "examples:" and not stripped.startswith("- "):
            in_examples = False  # some other top-level key: ignore its content
            continue
        if stripped == "examples:" and not raw[0].isspace():
            in_examples = True
            current = None
            continue
        if not in_examples:
            continue
        m = item_re.match(raw)
        if m:
            current = {m.group(2): parse_value(m.group(3), lineno)}
            examples.append(current)
            continue
        m = kv_re.match(raw)
        if m and current is not None:
            current[m.group(2)] = parse_value(m.group(3), lineno)
            continue
        raise ManifestError(f"manifest line {lineno}: unsupported syntax: {stripped!r}")
    return examples


def load_manifest() -> list[dict]:
    if not MANIFEST.is_file():
        sys.exit(f"error: {MANIFEST} not found")
    text = MANIFEST.read_text(encoding="utf-8")
    try:
        import yaml  # type: ignore
        data = yaml.safe_load(text) or {}
        raw = data.get("examples") or []
    except ImportError:
        raw = parse_manifest_subset(text)

    examples = []
    for i, ex in enumerate(raw):
        if not isinstance(ex, dict) or "id" not in ex:
            raise ManifestError(f"examples[{i}]: each entry needs at least 'id'")
        langs = ex.get("langs", list(LANGS))
        if isinstance(langs, str):
            langs = [langs]
        requires = ex.get("requires", [])
        if isinstance(requires, str):
            requires = [requires]
        examples.append({
            "id": str(ex["id"]),
            "dir": str(ex.get("dir", f"examples/{ex['id']}")),
            "langs": [str(l) for l in langs],
            "mode": str(ex.get("mode", "local")),
            "requires": [str(r) for r in requires],
        })
    return examples


# ---------------------------------------------------------------------------
# Result bookkeeping
# ---------------------------------------------------------------------------

class Result:
    def __init__(self) -> None:
        self.status = "PASS"  # PASS | FAIL | SKIP
        self.reason = ""
        self.seconds = 0.0
        self.log: Path | None = None

    def mark(self, status: str, reason: str = "") -> None:
        self.status = status
        self.reason = reason


def log_tail(path: Path | None, max_chars: int = 4000) -> str:
    if path is None or not path.is_file():
        return ""
    text = path.read_text(encoding="utf-8", errors="replace")
    return text[-max_chars:]


# ---------------------------------------------------------------------------
# Build phase
# ---------------------------------------------------------------------------

def build_one(ex: dict, lang: str, res: Result) -> None:
    ex_dir = REPO_ROOT / ex["dir"]
    log = LOG_DIR / f"{ex['id']}-{lang}.log"
    res.log = log
    start = time.monotonic()
    try:
        p = subprocess.run(
            ["./demo.sh", lang, "build"], cwd=ex_dir,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=BUILD_TIMEOUT,
        )
        log.write_bytes(p.stdout)
        if p.returncode != 0:
            res.mark("FAIL", f"build exit {p.returncode}")
    except subprocess.TimeoutExpired as e:
        log.write_bytes(e.stdout or b"")
        res.mark("FAIL", f"build timed out after {BUILD_TIMEOUT}s")
    except OSError as e:
        log.write_text(str(e), encoding="utf-8")
        res.mark("FAIL", f"build could not start: {e}")
    finally:
        res.seconds = time.monotonic() - start


# ---------------------------------------------------------------------------
# Verify phase helpers
# ---------------------------------------------------------------------------

_vm_cache: dict[str, bool] = {}


def vm_reachable(vm: str) -> bool:
    if vm not in _vm_cache:
        try:
            p = subprocess.run(
                [str(REPO_ROOT / "scripts" / "lab" / "vm-ip.sh"), vm],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
            )
            _vm_cache[vm] = p.returncode == 0 and bool(p.stdout.strip())
        except (OSError, subprocess.TimeoutExpired):
            _vm_cache[vm] = False
    return _vm_cache[vm]


_lgtm_up: bool | None = None


def lgtm_up() -> bool:
    global _lgtm_up
    if _lgtm_up is None:
        try:
            urllib.request.urlopen(LGTM_PROBE_URL, timeout=3)
            _lgtm_up = True
        except urllib.error.HTTPError:
            _lgtm_up = True  # any HTTP response (incl. 4xx) means it is listening
        except (urllib.error.URLError, OSError):
            _lgtm_up = False
    return _lgtm_up


def revert_target() -> bool:
    p = subprocess.run(
        [str(REPO_ROOT / "scripts" / "lab" / "revert-vm.sh"), TARGET_VM, "lab-ready"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if p.returncode != 0:
        print(f"warning: revert-vm.sh failed:\n{p.stdout.decode(errors='replace')}", file=sys.stderr)
    return p.returncode == 0


def verify_one(ex: dict, lang: str, res: Result) -> None:
    ex_dir = REPO_ROOT / ex["dir"]
    env = os.environ.copy()
    env.update(LSP_LANG=lang, EXAMPLE_DIR=str(ex_dir), REPO_ROOT=str(REPO_ROOT))
    if ex["mode"] in ("vm", "vm-peer"):
        env["TARGET"] = TARGET_VM  # demo.sh run deploys via deploy-to-vm.sh
    start = time.monotonic()
    try:
        p = subprocess.run(
            [LUA, "verify.lua"], cwd=ex_dir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=VERIFY_TIMEOUT,
        )
        out = p.stdout.decode(errors="replace")
        if res.log:
            with res.log.open("a", encoding="utf-8") as f:
                f.write(f"\n--- verify ({lang}) exit {p.returncode} ---\n{out}")
        if p.returncode == 77:
            m = re.search(r"SKIP:\s*(.+)", out)
            res.mark("SKIP", m.group(1).strip() if m else "verify skipped")
        elif p.returncode != 0:
            res.mark("FAIL", f"verify exit {p.returncode}")
    except subprocess.TimeoutExpired:
        res.mark("FAIL", f"verify timed out after {VERIFY_TIMEOUT}s")
    except OSError as e:
        res.mark("FAIL", f"verify could not start: {e}")
    finally:
        res.seconds += time.monotonic() - start


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", action="append", metavar="ID", default=[],
                    help="run only this example id (repeatable)")
    ap.add_argument("--lang", action="append", choices=LANGS, default=[],
                    help="restrict to this language (repeatable)")
    ap.add_argument("--mode", choices=("local", "vm", "all"), default="local",
                    help="which examples to run (default: local)")
    ap.add_argument("--revert-between", action="store_true",
                    help="revert the target VM to 'lab-ready' between VM examples")
    ap.add_argument("--junit", metavar="PATH", help="write JUnit XML report")
    ap.add_argument("--jobs", type=int, default=1, metavar="N",
                    help="parallel builds (build phase only; verify is sequential)")
    return ap.parse_args(argv)


def select(examples: list[dict], args: argparse.Namespace) -> list[dict]:
    known = {ex["id"] for ex in examples}
    for only in args.only:
        if only not in known:
            sys.exit(f"error: --only {only}: no such example in manifest")
    out = []
    for ex in examples:
        if args.only and ex["id"] not in args.only:
            continue
        if args.mode == "local" and ex["mode"] != "local":
            continue
        if args.mode == "vm" and ex["mode"] not in ("vm", "vm-peer"):
            continue
        langs = [l for l in ex["langs"] if not args.lang or l in args.lang]
        if not langs:
            continue
        ex = dict(ex, langs=langs)
        out.append(ex)
    return out


def print_table(selected: list[dict], results: dict) -> None:
    cols = [l for l in LANGS if any(l in ex["langs"] for ex in selected)]
    id_w = max([len("example")] + [len(ex["id"]) for ex in selected])
    print()
    print("  ".join(["example".ljust(id_w)] + [c.ljust(4) for c in cols]))
    for ex in selected:
        cells = [(results[(ex["id"], l)].status if l in ex["langs"] else "-").ljust(4)
                 for l in cols]
        print("  ".join([ex["id"].ljust(id_w)] + cells))
    reasons = [(ex["id"], l, results[(ex["id"], l)])
               for ex in selected for l in ex["langs"]
               if results[(ex["id"], l)].reason]
    if reasons:
        print()
        for ex_id, lang, r in reasons:
            print(f"  {r.status} {ex_id} [{lang}]: {r.reason}")


def write_junit(path: str, selected: list[dict], results: dict) -> None:
    cases = [(ex, l, results[(ex["id"], l)]) for ex in selected for l in ex["langs"]]
    suite = ET.Element("testsuite", {
        "name": "examples",
        "tests": str(len(cases)),
        "failures": str(sum(1 for _, _, r in cases if r.status == "FAIL")),
        "skipped": str(sum(1 for _, _, r in cases if r.status == "SKIP")),
        "time": f"{sum(r.seconds for _, _, r in cases):.1f}",
    })
    for ex, lang, r in cases:
        tc = ET.SubElement(suite, "testcase", {
            "classname": ex["id"], "name": lang, "time": f"{r.seconds:.1f}",
        })
        if r.status == "FAIL":
            fail = ET.SubElement(tc, "failure", {"message": r.reason})
            fail.text = log_tail(r.log)
        elif r.status == "SKIP":
            ET.SubElement(tc, "skipped", {"message": r.reason})
    ET.indent(suite)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        examples = load_manifest()
    except ManifestError as e:
        sys.exit(f"error: {e}")
    selected = select(examples, args)
    if not selected:
        print("nothing selected")
        return 0

    LOG_DIR.mkdir(exist_ok=True)
    results = {(ex["id"], l): Result() for ex in selected for l in ex["langs"]}

    # Build phase (parallelizable).
    jobs = [(ex, l) for ex in selected for l in ex["langs"]]
    print(f"building {len(jobs)} example-lang combinations (jobs={args.jobs})...")
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futs = {pool.submit(build_one, ex, l, results[(ex["id"], l)]): (ex, l)
                for ex, l in jobs}
        for fut in concurrent.futures.as_completed(futs):
            ex, l = futs[fut]
            fut.result()
            r = results[(ex["id"], l)]
            state = "ok" if r.status == "PASS" else "FAILED"
            print(f"  build {ex['id']} [{l}]: {state}")

    # Verify phase — strictly sequential; VM examples serial by construction.
    print("\nverifying...")
    vm_examples_run = 0
    for ex in selected:
        langs = [l for l in ex["langs"] if results[(ex["id"], l)].status == "PASS"]
        if not langs:
            continue

        if "lgtm" in ex["requires"] and not lgtm_up():
            for l in langs:
                results[(ex["id"], l)].mark("SKIP", "lgtm down")
            print(f"  {ex['id']}: SKIP (lgtm down)")
            continue

        if ex["mode"] in ("vm", "vm-peer"):
            needed = [TARGET_VM] + ([PEER_VM] if ex["mode"] == "vm-peer" else [])
            if not all(vm_reachable(vm) for vm in needed):
                for l in langs:
                    results[(ex["id"], l)].mark("SKIP", "lab down")
                print(f"  {ex['id']}: SKIP (lab down)")
                continue
            if args.revert_between and vm_examples_run > 0 and not revert_target():
                for l in langs:
                    results[(ex["id"], l)].mark("SKIP", "revert failed")
                continue
            vm_examples_run += 1

        for l in langs:
            r = results[(ex["id"], l)]
            verify_one(ex, l, r)
            print(f"  verify {ex['id']} [{l}]: {r.status}"
                  + (f" ({r.reason})" if r.reason else ""))

    print_table(selected, results)
    all_r = [results[(ex["id"], l)] for ex in selected for l in ex["langs"]]
    passed = sum(1 for r in all_r if r.status == "PASS")
    failed = sum(1 for r in all_r if r.status == "FAIL")
    skipped = sum(1 for r in all_r if r.status == "SKIP")
    print(f"\n{passed} passed, {failed} failed, {skipped} skipped"
          f" (logs in {LOG_DIR.relative_to(REPO_ROOT)}/)")

    if args.junit:
        write_junit(args.junit, selected, results)
        print(f"junit report: {args.junit}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
