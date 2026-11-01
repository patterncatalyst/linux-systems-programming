#!/usr/bin/env python3
"""Find and remove details that identify the development machine or its owner.

A public repository must not name the machine it was built on: home paths, the
account name, the hostname, the laptop and CPU model, the exact kernel build, the
machine-id, MAC addresses, the root filesystem UUID. This tool finds them and
replaces them, without ever writing them down itself: this machine's own values
are read at run time, everything else is matched by generic patterns.

    host_ids.py check   [--repo DIR]           tracked files; exit 1 on a hit (CI-safe)
    host_ids.py scrub   [--repo DIR] [PATH...]  rewrite files in place (default: tracked)
    host_ids.py scrub --dry-run ...             change nothing; exit 1 if a file would change
    host_ids.py filter                          scrub stdin to stdout (captured logs)
    host_ids.py history [--repo DIR] [--emit-rules FILE]
                                                scan every blob and message in all refs;
                                                optionally write git filter-repo rules
    host_ids.py local   [--repo DIR]            inventory of local copies of old history
    host_ids.py self-test                       planted cases in a throwaway repo

Hits are reported as `path:line: category`, never with the matched value, so a CI
log does not republish what it found. The rules file written by --emit-rules does
hold the values: it must live outside the repository and be deleted after use.

Optional per-repo config `.host-scrub.toml` (committed; holds no identifiers):

    skip = ["scripts/validate.py"]               # files never checked or scrubbed
    cpu_class = "Intel 8-core/16-thread laptop CPU"   # override the derived class
    [allow]                                      # path globs exempt from a category
    kernel-build = ["scripts/lab/*.sh"]
    [allow_match]                                # regexes for public values, any file
    kernel-build = ['^8[.]4[.]2-200[.]fc98']     # e.g. the lab guest's pinned kernel
    home-path = ['^/home/fedora$']               # the guest's cloud-init account

Extra literals that identify the machine but are not discoverable (an old hostname,
a former account) go in an untracked file, one per line, `value` or
`value==>replacement`: $HOST_SCRUB_EXTRA, else ~/.config/host-scrub/extra.txt.

Stdlib only; Python 3.11 or newer.
"""

from __future__ import annotations

import argparse
import fnmatch
import getpass
import gzip
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

SELF = Path(__file__).resolve()
CONFIG_NAME = ".host-scrub.toml"
# Names too common to search for as words; a CI runner's account is "runner".
COMMON_NAMES = {
    "root", "user", "runner", "fedora", "ubuntu", "debian", "admin", "localhost",
    "github", "build", "builder", "docker", "vagrant", "ec2-user", "cloud-user",
}  # fmt: skip
# Placeholders that are fine after /home/ or /Users/.
HOME_PLACEHOLDERS = {"user", "you", "username", "me", "name", "runner"}
PLACEHOLDER_ALT = "|".join(sorted(HOME_PLACEHOLDERS))


# ----------------------------------------------------------------- the machine
@dataclass
class Runtime:
    """This machine's identifying values, read now and never written to disk."""

    account: list[str] = field(default_factory=list)
    hostname: list[str] = field(default_factory=list)
    home: str = ""
    repo_path: str = ""
    cpu_model: str = ""
    cpu_class: str = "CPU"
    products: list[str] = field(default_factory=list)
    kernel_release: str = ""
    kernel_version: str = ""
    machine_ids: list[str] = field(default_factory=list)
    macs: list[str] = field(default_factory=list)
    boot_uuids: list[str] = field(default_factory=list)
    extra: list[tuple[str, str]] = field(default_factory=list)


def _read(path: str) -> str:
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""


def _run(*args: str) -> str:
    try:
        return subprocess.run(
            args, capture_output=True, text=True, check=True
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def _cpu_class() -> str:
    info = _read("/proc/cpuinfo")
    vendor = {"GenuineIntel": "Intel", "AuthenticAMD": "AMD"}
    v = next((vendor.get(m) for m in re.findall(r"vendor_id\s*:\s*(\S+)", info)), None)
    if not v and platform.system() == "Darwin":
        brand = _run("sysctl", "-n", "machdep.cpu.brand_string")
        v = "Apple" if brand.startswith("Apple") else "Intel"
    cores = {
        (p, c)
        for p, c in re.findall(
            r"physical id\s*:\s*(\d+).*?core id\s*:\s*(\d+)", info, re.DOTALL
        )
    }
    threads = os.cpu_count() or 0
    chassis = _read("/sys/class/dmi/id/chassis_type")
    kind = "laptop " if chassis in {"8", "9", "10", "14", "31", "32"} else ""
    shape = f"{len(cores)}-core/{threads}-thread " if cores and threads else ""
    return f"{v or 'x86'} {shape}{kind}CPU".replace("  ", " ")


def _extra_literals() -> list[tuple[str, str]]:
    path = os.environ.get("HOST_SCRUB_EXTRA") or str(
        Path.home() / ".config/host-scrub/extra.txt"
    )
    out = []
    for line in _read(path).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        value, _, repl = line.partition("==>")
        out.append((value, repl or "redacted"))
    return out


def gather(repo: Path | None) -> Runtime:
    rt = Runtime()
    home = os.environ.get("HOME", "")
    names = set()
    try:
        names.add(getpass.getuser())
    except (OSError, KeyError):
        pass
    if m := re.match(r"/(?:home|Users)/([^/]+)", home):
        names.add(m.group(1))
        rt.home = m.group(0)
    rt.account = sorted(n for n in names if n)
    host = socket.gethostname()
    rt.hostname = sorted({h for h in (host, host.split(".")[0]) if h})
    if repo is not None:
        rt.repo_path = str(repo.resolve())
    for line in _read("/proc/cpuinfo").splitlines():
        if line.startswith("model name"):
            rt.cpu_model = line.split(":", 1)[1].strip()
            break
    if not rt.cpu_model and platform.system() == "Darwin":
        rt.cpu_model = _run("sysctl", "-n", "machdep.cpu.brand_string")
    rt.cpu_class = _cpu_class()
    for f in ("product_name", "product_version", "product_family", "board_name"):
        v = _read(f"/sys/class/dmi/id/{f}")
        if (
            len(v) >= 4
            and not re.fullmatch(r"(?i)none|default string|to be filled.*|\d+", v)
            and not GENERIC_VM_PRODUCT.search(v)
        ):
            rt.products.append(v)
    if platform.system() == "Linux":
        rt.kernel_release = _run("uname", "-r")
        rt.kernel_version = _run("uname", "-v")
    rt.machine_ids = sorted(
        {v for v in (_read("/etc/machine-id"), _read("/var/lib/dbus/machine-id")) if v}
    )
    for d in Path("/sys/class/net").glob("*"):
        mac = _read(str(d / "address")).lower()
        if mac and d.name != "lo" and mac != "00:00:00:00:00:00":
            rt.macs.append(mac)
    rt.macs = sorted(set(rt.macs))
    rt.boot_uuids = re.findall(r"UUID=([0-9a-fA-F-]{36})", _read("/proc/cmdline"))
    rt.extra = _extra_literals()
    return rt


# ----------------------------------------------------------------- the rules
@dataclass(frozen=True)
class Rule:
    category: str
    pattern: re.Pattern[str]
    replacement: str  # a re.sub template
    literal: str = ""  # set for a runtime literal; used for filter-repo rules


def _lit(
    category: str, value: str, repl: str, word: bool = False, icase: bool = False
) -> Rule:
    pat = re.escape(value)
    if word:
        pat = rf"(?<![A-Za-z0-9_]){pat}(?![A-Za-z0-9_])"
    flags = re.IGNORECASE if icase else 0
    return Rule(
        category, re.compile(pat, flags), repl.replace("\\", "\\\\"), literal=value
    )


def _major_minor(release: str) -> str:
    m = re.match(r"(\d+\.\d+)", release)
    return m.group(1) if m else "redacted"


def _preempt(version: str) -> str:
    m = re.search(r"PREEMPT_RT|PREEMPT_DYNAMIC|PREEMPT\b", version)
    return m.group(0) if m else "SMP"


INTEL_PART = r"i[3579]-\d{4,5}[A-Z]{0,2}"
# DMI product strings of hypervisors and cloud VMs: generic, never identifying.
GENERIC_VM_PRODUCT = re.compile(
    r"(?i)virtual machine|standard pc|kvm|qemu|vmware|virtualbox|bochs|hvm domu|"
    r"google compute engine|openstack|amazon ec2|droplet|cloud server"
)
KERNEL_BUILD = (
    r"(?<!kernel-)(?<![\w.-])(\d+\.\d+)\.\d+-\d+(?:\.\w+)*?\.(?:fc|el)\d+(?:_\d+)?"
    r"\.(?:x86_64|aarch64)(?!\.rpm)\b"
    r"|(?<![\w.-])(\d+\.\d+)\.\d+-\d+-(?:generic|lowlatency|amd64|arm64|cloud-amd64)\b"
)
# The same release written without its architecture ("kernel 8.4.2-200.fc98"). A bare
# name-version-release would also match every Fedora package version, so this form is
# matched after a word that says it is a kernel (allowing quotes, markup or a line break
# in between), or by the shape of a Fedora kernel release: a three-digit release number
# in the 100s to 300s, which ordinary packages do not use.
KERNEL_BUILD_NOARCH = (
    r"\b((?:[Kk]ernel|Linux|vmlinuz-|uname -r)\W{0,8}?(?:<code>)?)"
    r"(\d+\.\d+)\.\d+-\d+(?:\.\w+)*?\.(?:fc|el)\d+(?:_\d+)?\b"
    r"(?!\.(?:x86_64|aarch64|src|rpm))"
    r"|(?<![\w.-])()([4-9]\.\d+)\.\d+-[1-3]\d\d\.fc\d+\b(?!\.(?:x86_64|aarch64|src|rpm))"
)


def generic_rules(cpu_class: str) -> list[Rule]:
    """Patterns that catch identifiers from any machine, not just this one.

    A match becomes this machine's CPU class when the vendor agrees, else the bare vendor.
    """

    def cls(vendor: str) -> str:
        c = cpu_class if cpu_class.startswith(vendor) else f"{vendor} CPU"
        return c.replace("\\", "\\\\")

    cpu, amd = cls("Intel"), cls("AMD")
    return [
        Rule(
            "cpu-model",
            re.compile(
                rf"(?:\d+(?:st|nd|rd|th) Gen )?Intel\(R\) Core\(TM\) (?:{INTEL_PART}|Ultra \d \d{{3}}[A-Z]?)"
                r"(?: (?:CPU )?@ [\d.]+GHz)?"
            ),
            cpu,
        ),
        Rule(
            "cpu-model",
            re.compile(
                rf"(?<![A-Za-z0-9+/=-])(?:\d+(?:st|nd|rd|th)[- ][Gg]en )?(?:Intel(?: Core)? )?{INTEL_PART}\b"
            ),
            cpu,
        ),
        Rule("cpu-model", re.compile(r"\bCore Ultra [579] \d{3}[A-Z]{0,2}\b"), cpu),
        Rule(
            "cpu-model",
            re.compile(r"\bAMD Ryzen(?: AI)?(?: PRO)? \d+ \w*\d{3,4}\w*"),
            amd,
        ),
        Rule(
            "cpu-model", re.compile(r"\bRyzen(?: AI)?(?: PRO)? \d+ \w*\d{3,4}\w*"), amd
        ),
        Rule(
            "cpu-model",
            re.compile(r"Intel\(R\) Xeon\(R\)[^\"\n]{0,40}?(?=\"|$|\n|  )"),
            cpu,
        ),
        Rule("cpu-model", re.compile(r"\bXeon (?:[A-Z]{1,2}-?)?\d{4}[A-Z]?\b"), cpu),
        Rule(
            "product-name",
            re.compile(
                r'("product_(?:name|version|family)"\s*:\s*)"(?!redacted")[^"]+"'
            ),
            r'\1"redacted"',
        ),
        Rule("kernel-build", re.compile(KERNEL_BUILD), r"\1\2"),
        Rule("kernel-build", re.compile(KERNEL_BUILD_NOARCH), r"\1\2\3\4"),
        Rule(
            "kernel-version",
            re.compile(
                r"#\d+(?: SMP)?(?: (PREEMPT_DYNAMIC|PREEMPT_RT|PREEMPT))? \w{3} \w{3} +\d{1,2} "
                r"\d\d:\d\d:\d\d (?:UTC|[A-Z]{3,4}) \d{4}"
            ),
            r"\1",
        ),
        Rule(
            "boot-uuid", re.compile(r"root=UUID=[0-9a-fA-F-]{36}"), "root=UUID=<uuid>"
        ),
        Rule("boot-uuid", re.compile(r"BOOT_IMAGE=\S+"), "BOOT_IMAGE=<redacted>"),
        Rule(
            "home-path",
            re.compile(
                rf"/(?:home|Users)/(?!(?:{PLACEHOLDER_ALT})(?:/|\b))[A-Za-z0-9._-]+(?=/|\b)"
            ),
            "~",
        ),
    ]


def runtime_rules(rt: Runtime) -> list[Rule]:
    """Literal rules for this machine's values. Longest and most specific first."""
    rules: list[Rule] = []
    if rt.repo_path and rt.home and rt.repo_path.startswith(rt.home):
        rules.append(_lit("home-path", rt.repo_path, "<repo>"))
    for value, repl in rt.extra:
        rules.append(_lit("extra", value, repl))
    if rt.home:
        rules.append(_lit("home-path", rt.home, "~"))
    if rt.cpu_model:
        rules.append(_lit("cpu-model", rt.cpu_model, rt.cpu_class))
    for p in sorted(rt.products, key=len, reverse=True):
        rules.append(_lit("product-name", p, "redacted"))
    if rt.kernel_version and "PREEMPT_RT" not in rt.kernel_version:
        rules.append(
            _lit("kernel-version", rt.kernel_version, _preempt(rt.kernel_version))
        )
    if rt.kernel_release:
        rules.append(
            _lit("kernel-build", rt.kernel_release, _major_minor(rt.kernel_release))
        )
    for mid in rt.machine_ids:
        rules.append(_lit("machine-id", mid, "<machine-id>", word=True))
    for mac in rt.macs:
        rules.append(_lit("mac", mac, "00:00:00:00:00:00", icase=True))
    for u in rt.boot_uuids:
        rules.append(_lit("boot-uuid", u, "<uuid>", icase=True))
    for h in sorted(rt.hostname, key=len, reverse=True):
        if len(h) >= 4 and h.lower() not in COMMON_NAMES:
            rules.append(_lit("hostname", h, "host", word=True))
    for a in rt.account:
        if len(a) >= 3 and a.lower() not in COMMON_NAMES:
            rules.append(_lit("account", a, "user", word=True))
    return rules


def on_ci_runner() -> bool:
    """True on a hosted CI runner, whose own values say nothing about the developer's machine."""
    return os.environ.get("CI", "").lower() == "true" or "GITHUB_ACTIONS" in os.environ


def all_rules(rt: Runtime) -> list[Rule]:
    """This machine's literal values, then the generic patterns. On a CI runner only the
    generic patterns apply: the runner's hostname, DMI and kernel are a cloud VM's, and
    matching them would flag ordinary words ("Virtual Machine") in the repo."""
    if on_ci_runner():
        return generic_rules(rt.cpu_class)
    return runtime_rules(rt) + generic_rules(rt.cpu_class)


# ----------------------------------------------------------------- config
@dataclass
class Config:
    skip: list[str] = field(default_factory=list)
    allow: dict[str, list[str]] = field(default_factory=dict)
    allow_match: dict[str, list[re.Pattern[str]]] = field(default_factory=dict)
    cpu_class: str = ""

    def skipped(self, rel: str) -> bool:
        return any(fnmatch.fnmatch(rel, g) for g in self.skip)

    def allowed(self, category: str, rel: str) -> bool:
        return any(fnmatch.fnmatch(rel, g) for g in self.allow.get(category, []))

    def value_allowed(self, category: str, value: str) -> bool:
        """True when a matched value is a known public string, such as a guest's pin."""
        return any(p.search(value) for p in self.allow_match.get(category, []))


def load_config(repo: Path) -> Config:
    path = repo / CONFIG_NAME
    if path.is_file():
        text = path.read_text(encoding="utf-8")
    else:  # a bare mirror: read the committed config from HEAD
        text = _run("git", "-C", str(repo), "show", f"HEAD:{CONFIG_NAME}")
    if not text:
        return Config()
    data = tomllib.loads(text)
    return Config(
        skip=list(data.get("skip", [])),
        allow={k: list(v) for k, v in data.get("allow", {}).items()},
        allow_match={
            k: [re.compile(x) for x in v]
            for k, v in data.get("allow_match", {}).items()
        },
        cpu_class=str(data.get("cpu_class", "")),
    )


# ----------------------------------------------------------------- files
def repo_root(start: Path) -> Path:
    """The work tree root, or the repository itself when it is bare (a mirror clone)."""
    out = _run("git", "-C", str(start), "rev-parse", "--show-toplevel")
    if out:
        return Path(out)
    if _run("git", "-C", str(start), "rev-parse", "--is-bare-repository") == "true":
        return start.resolve()
    sys.exit(f"host_ids: {start} is not a git repository")


def tracked(repo: Path) -> list[str]:
    res = subprocess.run(
        ["git", "-C", str(repo), "ls-files", "-z"], capture_output=True, check=True
    )
    return [n for n in res.stdout.decode().split("\0") if n]


def decode(raw: bytes) -> str | None:
    if raw[:2] == b"\x1f\x8b":
        try:
            raw = gzip.decompress(raw)
        except (OSError, EOFError):
            return None
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return None


def gzip_n(data: bytes) -> bytes:
    """Compress like `gzip -n`: no stored file name, mtime 0."""
    return gzip.compress(data, mtime=0)


def scrub_text(text: str, rules: list[Rule], cfg: Config, rel: str) -> str:
    for r in rules:
        if cfg.allowed(r.category, rel):
            continue

        def sub(m: re.Match[str], r: Rule = r) -> str:
            if cfg.value_allowed(r.category, m.group(0)):
                return m.group(0)
            return m.expand(r.replacement)

        text = r.pattern.sub(sub, text)
    return text


def hits(text: str, rules: list[Rule], cfg: Config, rel: str) -> list[tuple[int, str]]:
    found = []
    for r in rules:
        if cfg.allowed(r.category, rel):
            continue
        for m in r.pattern.finditer(text):
            if r.category == "home-path" and m.group(0) in ("~", "<repo>"):
                continue
            if cfg.value_allowed(r.category, m.group(0)):
                continue
            found.append((text.count("\n", 0, m.end() - 1) + 1, r.category))
    return sorted(set(found))


def is_self(repo: Path, rel: str) -> bool:
    """The tool itself, or the config that lists allowed patterns."""
    if rel == CONFIG_NAME:
        return True
    try:
        return (repo / rel).resolve() == SELF
    except OSError:
        return False


# ----------------------------------------------------------------- commands
def setup(repo_arg: str | None) -> tuple[Path, Config, list[Rule]]:
    repo = repo_root(Path(repo_arg or "."))
    cfg = load_config(repo)
    rt = gather(repo)
    if cfg.cpu_class:
        rt.cpu_class = cfg.cpu_class
    return repo, cfg, all_rules(rt)


def cmd_check(repo: Path, cfg: Config, rules: list[Rule]) -> int:
    n_files = n_hits = 0
    literal = [r for r in rules if r.literal]
    for rel in tracked(repo):
        if cfg.skipped(rel):
            continue
        # The tool and its config carry fake examples of every pattern by design, so they
        # are checked for this machine's own values only.
        file_rules = literal if is_self(repo, rel) else rules
        try:
            text = decode((repo / rel).read_bytes())
        except OSError:
            continue
        if text is None:
            continue
        n_files += 1
        for line, cat in hits(text, file_rules, cfg, rel):
            print(f"{rel}:{line}: {cat}")
            n_hits += 1
    print(f"host_ids check: {n_files} text file(s), {n_hits} hit(s)", file=sys.stderr)
    return 1 if n_hits else 0


def cmd_scrub(
    repo: Path, cfg: Config, rules: list[Rule], paths: list[str], dry_run: bool
) -> int:
    if paths:
        files = []
        for p in map(Path, paths):
            if p.is_dir():
                files += [
                    f
                    for f in sorted(p.rglob("*"))
                    if f.is_file() and ".git" not in f.parts
                ]
            elif p.is_file():
                files.append(p)
    else:
        files = [repo / rel for rel in tracked(repo)]
    changed = []
    for f in files:
        try:
            rel = str(f.resolve().relative_to(repo))
        except ValueError:
            rel = str(f)
        if cfg.skipped(rel) or rel == CONFIG_NAME or f.resolve() == SELF:
            continue
        try:
            raw = f.read_bytes()
        except OSError:
            continue
        text = decode(raw)
        if text is None:
            continue
        new = scrub_text(text, rules, cfg, rel)
        is_gz = raw[:2] == b"\x1f\x8b"
        clean_header = not is_gz or (raw[3] & 0x08 == 0 and raw[4:8] == b"\0\0\0\0")
        if new == text and clean_header:
            continue
        changed.append(rel)
        if not dry_run:
            data = new.encode("utf-8")
            f.write_bytes(gzip_n(data) if is_gz else data)
    verb = "would change" if dry_run else "changed"
    print(f"host_ids scrub: {verb} {len(changed)} file(s)", file=sys.stderr)
    for rel in changed[:50]:
        print(f"  {rel}", file=sys.stderr)
    return 1 if (dry_run and changed) else 0


def cmd_filter(rules: list[Rule]) -> int:
    sys.stdout.write(scrub_text(sys.stdin.read(), rules, Config(), "-"))
    return 0


def _replacement_for(rule: Rule, value: str) -> str:
    """The text a matched value becomes, computed with the rule's own template."""
    return rule.pattern.sub(rule.replacement, value, count=1)


def cmd_history(repo: Path, cfg: Config, rules: list[Rule], emit: str | None) -> int:
    if emit and Path(emit).resolve().is_relative_to(repo):
        sys.exit(
            "host_ids: --emit-rules must be outside the repository (it holds the values)"
        )
    objects = subprocess.run(
        ["git", "-C", str(repo), "rev-list", "--all", "--objects"],
        capture_output=True, text=True, check=True,
    ).stdout.splitlines()  # fmt: skip
    paths: dict[str, str] = {}
    for line in objects:
        sha, _, path = line.partition(" ")
        if path:
            paths.setdefault(sha, path)
    found: dict[str, set[str]] = {}  # category -> paths
    values: dict[str, str] = {}  # value -> replacement
    batch = subprocess.Popen(
        ["git", "-C", str(repo), "cat-file", "--batch"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    )  # fmt: skip
    assert batch.stdin and batch.stdout

    def scan(text: str, rel: str, literal_only: bool = False) -> None:
        for r in rules:
            if cfg.allowed(r.category, rel) or cfg.skipped(rel):
                continue
            if literal_only and not r.literal:
                continue
            for m in r.pattern.finditer(text):
                v = m.group(0)
                if r.category == "home-path" and v in ("~", "<repo>"):
                    continue
                if cfg.value_allowed(r.category, v):
                    continue
                found.setdefault(r.category, set()).add(rel)
                values.setdefault(v, _replacement_for(r, v))

    for sha, rel in paths.items():
        # Every past version of the tool is scanned for this machine's own values: a
        # fixture once copied from a real file is exactly the leak to find.
        own = Path(rel).name in (SELF.name, CONFIG_NAME)
        batch.stdin.write(f"{sha}\n".encode())
        batch.stdin.flush()
        header = batch.stdout.readline().split()
        if len(header) < 3:
            continue
        data = batch.stdout.read(int(header[2]))
        batch.stdout.read(1)
        if header[1] != b"blob":
            continue
        text = decode(data)
        if text is not None:
            scan(text, rel, literal_only=own)
    batch.stdin.close()
    batch.wait()
    messages = subprocess.run(
        ["git", "-C", str(repo), "log", "--all", "--format=%H%n%B%n%x00"],
        capture_output=True, text=True, check=True,
    ).stdout  # fmt: skip
    tags = _run(
        "git", "-C", str(repo), "for-each-ref", "refs/tags", "--format=%(contents)"
    )
    scan(messages + "\n" + tags, "<commit and tag messages>")
    n_commits = _run("git", "-C", str(repo), "rev-list", "--all", "--count")
    print(f"host_ids history: {n_commits} commit(s), {len(paths)} path object(s)")
    for cat in sorted(found):
        sample = sorted(found[cat])
        more = f" (+{len(sample) - 5} more)" if len(sample) > 5 else ""
        print(f"  {cat}: {len(sample)} path(s): {', '.join(sample[:5])}{more}")
    if not found:
        print("  clean: nothing identifying in any reachable blob or message")
    if emit:
        lines, dropped = format_rules(values)
        if dropped:
            print(f"  {dropped} multi-line match(es) could not be written as rules")
        Path(emit).write_text(
            "\n".join(lines) + ("\n" if lines else ""), encoding="utf-8"
        )
        os.chmod(emit, 0o600)
        print(
            f"  wrote {len(lines)} literal rule(s) to {emit}: review it, then delete it"
        )
    return 1 if found else 0


def format_rules(values: dict[str, str]) -> tuple[list[str], int]:
    """`git filter-repo --replace-text` lines, longest value first.

    The format is one rule per line, so a match that spans a line break (a version on
    the line after the word "kernel") is reduced to its last line when the replacement
    keeps the same leading lines; otherwise it is dropped and counted. Every line is
    prefixed `literal:` so a value can never be read as a regex or glob rule.
    """
    rules: dict[str, str] = {}
    dropped = 0
    for v, r in values.items():
        while "\n" in v and "\n" in r and v.split("\n", 1)[0] == r.split("\n", 1)[0]:
            v, r = v.split("\n", 1)[1], r.split("\n", 1)[1]
        if "\n" in v or "\n" in r or "==>" in v or not v.strip():
            dropped += 1
            continue
        if v != r:
            rules.setdefault(v, r)
    ordered = sorted(rules.items(), key=lambda kv: len(kv[0]), reverse=True)
    return [f"literal:{v}==>{r}" for v, r in ordered], dropped


def cmd_local(repo: Path) -> int:
    g = ["git", "-C", str(repo)]
    print("worktrees:")
    print("  " + (_run(*g, "worktree", "list") or "(none)").replace("\n", "\n  "))
    remote_tips = set(
        _run(*g, "for-each-ref", "--format=%(objectname)", "refs/remotes").split()
    )
    print("local branches not contained in any remote-tracking branch:")
    stray = 0
    for line in _run(
        *g, "for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads"
    ).splitlines():
        name, sha = line.split()
        contained = sha in remote_tips or bool(
            _run(*g, "for-each-ref", "--contains", sha, "--format=x", "refs/remotes")
        )
        if not contained:
            stray += 1
            print(f"  {name}")
    if not stray:
        print("  (none)")
    stashes = _run(*g, "stash", "list")
    print(f"stashes: {len(stashes.splitlines()) if stashes else 0}")
    reflog = _run(*g, "reflog", "--all", "--format=%H")
    print(f"reflog entries: {len(reflog.splitlines()) if reflog else 0}")
    print("objects:", _run(*g, "count-objects", "-v").replace("\n", ", "))
    return 0


# ----------------------------------------------------------------- self-test
def self_test() -> int:
    fake = Runtime(
        account=["planted-acct"],
        hostname=["planted-host.example", "planted-host"],
        home="/home/planted-acct",
        cpu_model="Intel(R) Core(TM) i9-99999X CPU @ 9.90GHz",
        cpu_class="Intel 4-core/8-thread laptop CPU",
        products=["Planted Laptop 9000"],
        kernel_release="9.9.9-100.fc99.x86_64",
        kernel_version="#1 SMP PREEMPT_DYNAMIC Mon Jan  1 00:00:00 UTC 2099",
        machine_ids=["0123456789abcdef0123456789abcdef"],
        macs=["02:00:5e:10:20:30"],
        boot_uuids=["11111111-2222-3333-4444-555555555555"],
    )
    cases = {
        "a.md": "built in /home/planted-acct/Dev/proj by planted-acct\n",
        "b.json": '{"product_name": "Planted Laptop 9000", "model": "i9-99999X"}\n',
        "c.txt": "Linux planted-host 9.9.9-100.fc99.x86_64 #1 SMP PREEMPT_DYNAMIC "
        "Mon Jan  1 00:00:00 UTC 2099\n",
        "d.txt": "id 0123456789abcdef0123456789abcdef mac 02:00:5E:10:20:30 "
        "root=UUID=11111111-2222-3333-4444-555555555555\n",
        "e.txt": "other machine: /home/someone/x, an AMD Ryzen 7 5800X, 6.8.0-45-generic\n",
        "f.md": "an 11th-gen Intel i7-99999X laptop, an Intel Core i5-99999U\n",
        "pins.sh": "SRPM=kernel-8.4.2-200.fc98.src.rpm\nGUEST=8.4.2-200.fc98.x86_64\n",
        "noarch.md": "the host ran kernel 6.9.4-200.fc40 that day\n",
        "noarch2.md": "kernel `6.9.4-200.fc40`, kernel\n<code>6.8.1-100.fc40</code>\n"
        "| 6.7.2-300.fc39 | host |\n",
        "mixed.md": "guest 7.3.1-100.fc45.x86_64, host 6.9.4-200.fc40.x86_64, /home/fedora/app\n",
        "pkgs.md": "pcp 8.1.5-3.fc98 and kernel-devel-8.4.2-200.fc98 are pinned\n",
        "clean.md": "a ~/x path, <repo>/y, /home/user/z, kernel 7.2 PREEMPT_DYNAMIC\n",
    }
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        for name, body in cases.items():
            (repo / name).write_text(body, encoding="utf-8")
        (repo / "g.log.gz").write_bytes(
            gzip.compress(b"/home/planted-acct/log\n", mtime=123)
        )
        (repo / CONFIG_NAME).write_text(
            '[allow]\nkernel-build = ["pins.sh"]\n'
            "[allow_match]\nkernel-build = ['^7[.]3[.]1-100[.]fc45']\n"
            "home-path = ['^/home/fedora$']\n",
            encoding="utf-8",
        )
        subprocess.run(["git", "-C", str(repo), "add", "-A"], check=True)
        fake.repo_path = str(repo)
        rules = runtime_rules(fake) + generic_rules(fake.cpu_class)
        cfg = load_config(repo)

        def expect(cond: bool, what: str) -> None:
            if not cond:
                failures.append(what)

        for name in ("a.md", "b.json", "c.txt", "d.txt", "e.txt"):
            expect(bool(hits(cases[name], rules, cfg, name)), f"check flags {name}")
        expect(not hits(cases["clean.md"], rules, cfg, "clean.md"), "clean.md passes")
        expect(
            bool(hits(cases["noarch.md"], rules, cfg, "noarch.md")), "archless flagged"
        )
        expect(
            not hits(cases["pkgs.md"], rules, cfg, "pkgs.md"), "package versions pass"
        )
        expect(
            len(hits(cases["noarch2.md"], rules, cfg, "noarch2.md")) == 3,
            "quoted, next-line and table-cell kernels flagged",
        )
        expect(
            [c for _, c in hits(cases["mixed.md"], rules, cfg, "mixed.md")]
            == ["kernel-build"],
            "mixed.md: only the host kernel is flagged",
        )
        expect(
            not hits(cases["pins.sh"], rules, cfg, "pins.sh"),
            "allowed kernel pin passes",
        )
        expect(
            bool(hits(cases["pins.sh"], rules, Config(), "pins.sh")),
            "pin flagged without allow",
        )
        rc = cmd_scrub(repo, cfg, rules, [], dry_run=False)
        expect(rc == 0, "scrub exits 0")
        for name in cases:
            text = (repo / name).read_text(encoding="utf-8")
            expect(not hits(text, rules, cfg, name), f"{name} clean after scrub")
        out = (repo / "a.md").read_text(encoding="utf-8")
        expect(out == "built in ~/Dev/proj by user\n", f"a.md exact: {out!r}")
        out = (repo / "c.txt").read_text(encoding="utf-8")
        expect(out == "Linux host 9.9 PREEMPT_DYNAMIC\n", f"c.txt exact: {out!r}")
        out = (repo / "b.json").read_text(encoding="utf-8")
        expect(
            '"redacted"' in out and "Intel 4-core/8-thread laptop CPU" in out, "b.json"
        )
        out = (repo / "mixed.md").read_text(encoding="utf-8")
        want = "guest 7.3.1-100.fc45.x86_64, host 6.9, /home/fedora/app\n"
        expect(out == want, f"mixed.md exact: {out!r}")
        out = (repo / "noarch.md").read_text(encoding="utf-8")
        expect(out == "the host ran kernel 6.9 that day\n", f"noarch.md exact: {out!r}")
        out = (repo / "f.md").read_text(encoding="utf-8")
        cls = "Intel 4-core/8-thread laptop CPU"
        expect(out == f"an {cls} laptop, an {cls}\n", f"f.md exact: {out!r}")
        out = (repo / "noarch2.md").read_text(encoding="utf-8")
        want = "kernel `6.9`, kernel\n<code>6.8</code>\n| 6.7 | host |\n"
        expect(out == want, f"noarch2.md exact: {out!r}")
        out = (repo / "pins.sh").read_text(encoding="utf-8")
        expect(out == cases["pins.sh"], "allowed pin untouched by scrub")
        gz = (repo / "g.log.gz").read_bytes()
        expect(
            gz[4:8] == b"\0\0\0\0" and gzip.decompress(gz) == b"~/log\n",
            "gz rewritten -n",
        )
        expect(
            cmd_scrub(repo, cfg, rules, [], dry_run=True) == 0, "scrub is idempotent"
        )
    lines, dropped = format_rules(
        {
            "kernel\n`8.4.2-200.fc98": "kernel\n`8.4",
            "8.4.2-200.fc98.x86_64": "8.4",
            "a\nb": "c\nd",
        }
    )
    expect(
        lines
        == ["literal:8.4.2-200.fc98.x86_64==>8.4", "literal:`8.4.2-200.fc98==>`8.4"]
        and dropped == 1,
        f"format_rules: {lines!r} dropped={dropped}",
    )
    saved = {k: os.environ.get(k) for k in ("CI", "GITHUB_ACTIONS")}
    os.environ["CI"] = "true"
    expect(all(not r.literal for r in all_rules(fake)), "CI runner: generic rules only")
    for k, v in saved.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    expect(
        not GENERIC_VM_PRODUCT.search("Planted Laptop 9000"),
        "laptop is not a VM product",
    )
    expect(
        bool(GENERIC_VM_PRODUCT.search("Virtual Machine")), "Virtual Machine is generic"
    )
    # The tool must never carry this machine's own values, not even in a test fixture.
    own = SELF.read_text(encoding="utf-8")
    for r in runtime_rules(gather(None)):
        if r.pattern.search(own):
            failures.append(f"host_ids.py itself holds this machine's {r.category}")
    for f in failures:
        print(f"FAIL {f}")
    print(
        f"host_ids self-test: {'FAILED' if failures else 'passed'} ({len(failures)} failure(s))"
    )
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("check", help="tracked files; exit 1 on a hit")
    p.add_argument("--repo")
    p = sub.add_parser("scrub", help="rewrite files in place")
    p.add_argument("--repo")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("paths", nargs="*")
    sub.add_parser("filter", help="scrub stdin to stdout")
    p = sub.add_parser("history", help="scan all reachable blobs and messages")
    p.add_argument("--repo")
    p.add_argument("--emit-rules", metavar="FILE")
    p = sub.add_parser("local", help="inventory of local copies of old history")
    p.add_argument("--repo")
    sub.add_parser("self-test", help="planted cases in a throwaway repo")
    args = ap.parse_args()
    if args.cmd == "self-test":
        return self_test()
    if args.cmd == "filter":
        rt = gather(None)
        return cmd_filter(all_rules(rt))
    repo, cfg, rules = setup(getattr(args, "repo", None))
    if args.cmd == "check":
        return cmd_check(repo, cfg, rules)
    if args.cmd == "scrub":
        return cmd_scrub(repo, cfg, rules, args.paths, args.dry_run)
    if args.cmd == "history":
        return cmd_history(repo, cfg, rules, args.emit_rules)
    return cmd_local(repo)


if __name__ == "__main__":
    sys.exit(main())
