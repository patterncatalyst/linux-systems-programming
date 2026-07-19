#!/usr/bin/env python3
"""Static site validator for linux-systems-programming.

Checks (each finding printed as "file: message"; exit 1 if any):
  1. _docs/*.md and _parts/*.md front matter parses (simple YAML subset).
  2. every _docs "part:" matches some _parts "part_name:".
  3. no unguarded literal {{ / {% in prose (outside fences and {% raw %}).
  4. codetabs contract: include marker followed by exactly N fenced blocks.
  5. assets/diagrams: *.svg parse as XML, *.excalidraw parse as JSON;
     every excalidraw include in _docs has both NAME.svg and NAME.excalidraw.
  6. bash -n every *.sh under scripts/ and examples/.
  7. if lua/luac is on PATH, syntax-check every *.lua there too.

Stdlib only. Run from anywhere: paths resolve relative to the repo root.
"""

import json
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

findings: list[str] = []


def flag(path: Path, message: str) -> None:
    try:
        rel = path.resolve().relative_to(ROOT)
    except ValueError:
        rel = path
    findings.append(f"{rel}: {message}")


# ---------------------------------------------------------------- front matter

KEY_RE = re.compile(r"^([A-Za-z0-9_-]+):(?:\s+(.*))?$")


def parse_front_matter(path: Path, lines: list[str]) -> tuple[dict[str, str], int]:
    """Return ({key: unquoted value}, index of line after closing ---).

    Flags findings for malformed front matter. Body starts at the returned
    index even on error (0 meaning: treat the whole file as body).
    """
    if not lines or lines[0].strip() != "---":
        flag(path, "front matter: missing opening --- fence")
        return {}, 0
    fm: dict[str, str] = {}
    for i in range(1, len(lines)):
        line = lines[i].rstrip("\n")
        if line.strip() == "---":
            return fm, i + 1
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = KEY_RE.match(line)
        if not m:
            flag(path, f"front matter line {i + 1}: not a 'key: value' line: {line.strip()!r}")
            continue
        key, value = m.group(1), (m.group(2) or "").strip()
        quoted = len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'"
        if quoted:
            value = value[1:-1]
        elif ": " in value:
            flag(path, f"front matter line {i + 1}: unquoted value for {key!r} contains ': ' — quote it")
        fm[key] = value
    flag(path, "front matter: unterminated (no closing --- fence)")
    return fm, 0


# --------------------------------------------------- body scanning (fences/raw)

FENCE_RE = re.compile(r"^(\s*)(`{3,}|~{3,})")
RAW_OPEN_RE = re.compile(r"\{%-?\s*raw\s*-?%\}")
RAW_CLOSE_RE = re.compile(r"\{%-?\s*endraw\s*-?%\}")


def annotate_body(lines: list[str], start: int) -> tuple[list[bool], list[bool]]:
    """Per line: (guarded, fence_open) — guarded means inside a fence or raw
    region (delimiter lines count as guarded); fence_open marks lines that
    open a fenced code block outside raw."""
    n = len(lines)
    guarded = [False] * n
    fence_open = [False] * n
    fence: tuple[str, int] | None = None  # (char, length)
    in_raw = False
    for i in range(start, n):
        line = lines[i]
        if fence:
            guarded[i] = True
            m = FENCE_RE.match(line)
            if m and m.group(2)[0] == fence[0] and len(m.group(2)) >= fence[1]:
                fence = None
            continue
        if in_raw:
            guarded[i] = True
            if RAW_CLOSE_RE.search(line):
                in_raw = False
            continue
        if RAW_OPEN_RE.search(line):
            guarded[i] = True
            if not RAW_CLOSE_RE.search(line[RAW_OPEN_RE.search(line).end():]):
                in_raw = True
            continue
        m = FENCE_RE.match(line)
        if m:
            guarded[i] = True
            fence_open[i] = True
            fence = (m.group(2)[0], len(m.group(2)))
    return guarded, fence_open


# ---------------------------------------------------------- check 3: liquid

TAG_ALLOWED_RE = re.compile(r"\{%-?\s*(include|assign|raw|endraw)\b")
VAR_ALLOWED_RE = re.compile(r"\{\{-?\s*(site|page|part|include)\.")


def check_liquid(path: Path, lines: list[str], start: int, guarded: list[bool]) -> None:
    for i in range(start, len(lines)):
        if guarded[i]:
            continue
        line = lines[i]
        for m in re.finditer(r"\{\{|\{%", line):
            tok = m.group(0)
            if tok == "{%":
                if TAG_ALLOWED_RE.match(line, m.start()):
                    continue
            else:
                if VAR_ALLOWED_RE.match(line, m.start()):
                    continue
                end = line.find("}}", m.end())
                if end != -1 and "| relative_url" in line[m.start():end]:
                    continue
            flag(path, f"line {i + 1}: unguarded literal {tok!r} — fence it, {{% raw %}}-guard it, or use an allowed form")


# -------------------------------------------------------- check 4: codetabs

CODETABS_RE = re.compile(r'\{%-?\s*include\s+codetabs\.html\s+langs="([^"]*)"')


def check_codetabs(path: Path, lines: list[str], start: int,
                   guarded: list[bool], fence_open: list[bool]) -> None:
    n = len(lines)
    for i in range(start, n):
        if guarded[i]:
            continue
        m = CODETABS_RE.search(lines[i])
        if not m:
            continue
        labels = [s for s in m.group(1).split("|") if s.strip()]
        want = len(labels)
        if want == 0:
            flag(path, f"line {i + 1}: codetabs include has empty langs=")
            continue
        got = 0
        j = i + 1
        while j < n:
            if not lines[j].strip():
                j += 1
                continue
            if fence_open[j]:
                got += 1
                j += 1
                while j < n and guarded[j] and not fence_open[j]:
                    j += 1
                if got == want:
                    # exactly N: an immediately adjacent extra fence would be
                    # absorbed into the tab group too
                    k = j
                    while k < n and not lines[k].strip():
                        k += 1
                    if k < n and fence_open[k]:
                        flag(path, f"line {i + 1}: codetabs langs={want} but an extra adjacent code block follows (line {k + 1})")
                    break
                continue
            flag(path, f"line {i + 1}: codetabs langs={want} but only {got} code block(s) before prose resumes (line {j + 1})")
            break
        else:
            if got < want:
                flag(path, f"line {i + 1}: codetabs langs={want} but only {got} code block(s) before end of file")


# -------------------------------------------------------- checks 1-4 driver

def md_files(dirname: str) -> list[Path]:
    d = ROOT / dirname
    return sorted(d.glob("*.md")) if d.is_dir() else []


def check_markdown() -> None:
    part_names: set[str] = set()
    doc_parts: list[tuple[Path, str]] = []

    for path in md_files("_parts") + md_files("_docs"):
        lines = path.read_text(encoding="utf-8").splitlines()
        fm, body_start = parse_front_matter(path, lines)
        if path.parent.name == "_parts":
            if "part_name" in fm:
                part_names.add(fm["part_name"])
            else:
                flag(path, "front matter: missing 'part_name:'")
        else:
            if "part" in fm:
                doc_parts.append((path, fm["part"]))
            else:
                flag(path, "front matter: missing 'part:'")
        guarded, fence_open = annotate_body(lines, body_start)
        check_liquid(path, lines, body_start, guarded)
        check_codetabs(path, lines, body_start, guarded, fence_open)

    for path, part in doc_parts:
        if part not in part_names:
            flag(path, f"part: {part!r} matches no _parts part_name: (known: {', '.join(sorted(part_names)) or 'none'})")


# ------------------------------------------------------- check 5: diagrams

EXCALIDRAW_INCLUDE_RE = re.compile(r"\{%-?\s*include\s+excalidraw\.html(.*?)-?%\}", re.DOTALL)
FILE_PARAM_RE = re.compile(r'file="([^"]+)"')


def check_diagrams() -> None:
    diagrams = ROOT / "assets" / "diagrams"
    if diagrams.is_dir():
        for svg in sorted(diagrams.glob("*.svg")):
            try:
                ET.parse(svg)
            except ET.ParseError as e:
                flag(svg, f"SVG does not parse: {e}")
        for exc in sorted(diagrams.glob("*.excalidraw")):
            try:
                json.loads(exc.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError) as e:
                flag(exc, f"excalidraw is not valid JSON: {e}")

    for path in md_files("_docs"):
        text = path.read_text(encoding="utf-8")
        for m in EXCALIDRAW_INCLUDE_RE.finditer(text):
            fp = FILE_PARAM_RE.search(m.group(1))
            line = text.count("\n", 0, m.start()) + 1
            if not fp:
                flag(path, f"line {line}: excalidraw include without file=\"NAME\"")
                continue
            name = fp.group(1)
            for ext in (".svg", ".excalidraw"):
                if not (diagrams / f"{name}{ext}").is_file():
                    flag(path, f"line {line}: excalidraw include references missing assets/diagrams/{name}{ext}")


# ----------------------------------------------------- checks 6-7: scripts

def script_files(pattern: str) -> list[Path]:
    out: list[Path] = []
    for dirname in ("scripts", "examples"):
        d = ROOT / dirname
        if d.is_dir():
            out.extend(sorted(d.rglob(pattern)))
    return out


def first_line(text: str) -> str:
    return text.strip().splitlines()[0] if text.strip() else "syntax error"


def check_shell() -> None:
    for sh in script_files("*.sh"):
        r = subprocess.run(["bash", "-n", str(sh)], capture_output=True, text=True)
        if r.returncode != 0:
            flag(sh, f"bash -n: {first_line(r.stderr)}")


def check_lua() -> None:
    luac = shutil.which("luac")
    lua = shutil.which("lua")
    if not luac and not lua:
        print("note: lua not on PATH; skipping .lua syntax checks", file=sys.stderr)
        return
    stdin_check = "local f, e = loadfile(arg[1]) if not f then io.stderr:write(tostring(e) .. '\\n') os.exit(1) end"
    for luafile in script_files("*.lua"):
        if luac:
            r = subprocess.run([luac, "-p", str(luafile)], capture_output=True, text=True)
        else:
            r = subprocess.run([lua, "-", str(luafile)], input=stdin_check,
                               capture_output=True, text=True)
        if r.returncode != 0:
            flag(luafile, f"lua syntax: {first_line(r.stderr)}")


# ----------------------------------------------------------------------- main

def main() -> int:
    check_markdown()
    check_diagrams()
    check_shell()
    check_lua()
    for f in findings:
        print(f)
    if findings:
        print(f"validate: {len(findings)} finding(s)", file=sys.stderr)
        return 1
    print("validate: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
