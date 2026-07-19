-- Verify fwatch v3 (seccomp + Landlock) — a VM example. Behavioral
-- assertions, driven by run-sandbox-checks.sh staged and run as root
-- (SUDO=1) on the lab guest:
--
--   1. NEGATIVE CONTROL — `probe --forbidden-syscall` installs a seccomp
--      allowlist that omits socket(2), then calls it. The bare exit code
--      alone would prove nothing (a program that always exits nonzero would
--      "pass"); the assertion is that socket(2) specifically returned EPERM,
--      confirmed both by the process's own exit code (20, this program's
--      "confirmed denied" convention) AND the printed errno text.
--   2. NEGATIVE CONTROL — `probe --sandbox DIR --outside PATH` restricts
--      Landlock to DIR, then opens a file under a DIFFERENT directory.
--      Same shape: exit 20 plus an explicit EACCES line.
--   3. POSITIVE CONTROL — `watch --sandbox DIR` with both layers applied
--      still sees create/modify/delete events for a file created INSIDE
--      DIR and exits 0 on timeout. This is what makes 1 and 2 meaningful:
--      the sandbox denies specific things without breaking the watcher's
--      actual job.
--
-- Runs under the runner's vm mode (TARGET=systems-target, deploy via a
-- custom root-run driver script — deploy-to-vm.sh only shells out a single
-- binary+args, and this needs mkdir/rm scaffolding around three invocations
-- of the same binary in one ssh round trip, same shape as example 30's
-- observe.sh).

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local target = os.getenv("TARGET")
if not target or target == "" then
  checks.skip("no TARGET set — this is a vm example (run with --mode vm)")
end

local example_dir = os.getenv("EXAMPLE_DIR") or "."
local home = os.getenv("HOME") or "/root"

-- Local path of the freshly built binary for this language.
local bin_rel = ({
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
})[lang]
local local_bin = example_dir .. "/" .. bin_rel
local local_driver = example_dir .. "/run-sandbox-checks.sh"

-- Resolve the guest IP (same helper deploy-to-vm.sh uses).
local ipf = io.popen(repo_root .. "/scripts/lab/vm-ip.sh '" .. target .. "' 2>/dev/null")
local ip = ipf and ipf:read("*l") or nil
if ipf then ipf:close() end
if not ip or ip == "" then
  checks.skip("cannot resolve IP for VM '" .. target .. "' (lab down?)")
end

local ssh_opts =
  "-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=" .. home .. "/.ssh/known_hosts"

-- Stage the binary under a per-language directory (so /proc comm is always
-- "app", the book-wide rule) alongside the driver script.
local remote_dir = "/tmp/lsp33-" .. lang
local remote_bin = remote_dir .. "/app"
local remote_driver = "/tmp/lsp33-run-sandbox-checks.sh"

local stage_cmd = string.format(
  "ssh %s 'fedora@%s' 'mkdir -p %s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "ssh %s 'fedora@%s' 'chmod 755 %s %s'",
  ssh_opts, ip, remote_dir,
  ssh_opts, local_bin, ip, remote_bin,
  ssh_opts, local_driver, ip, remote_driver,
  ssh_opts, ip, remote_bin, remote_driver)
local st = checks.run(stage_cmd)
checks.expect_exit(st, 0, lang .. ": stage app binary + run-sandbox-checks.sh on guest")

-- Run the driver as root; Landlock/seccomp are unprivileged mechanisms, but
-- this book's convention keeps every namespace/cgroup/seccomp/Landlock demo
-- on the VM path (SUDO=1) for consistency with its siblings.
local driven = checks.run(string.format(
  "ssh %s 'fedora@%s' 'sudo bash %s %s'",
  ssh_opts, ip, remote_driver, remote_bin))
checks.expect_exit(driven, 0, lang .. ": run-sandbox-checks.sh exits 0")
if driven.exit ~= 0 then
  print(driven.out)
end
local out = driven.out

-- 1. seccomp negative control: socket(2) denied with EPERM.
checks.expect_match(out, "fwatch: seccomp filter installed %(%d+ syscalls allowed%)",
  lang .. ": seccomp filter installed line printed")
checks.expect_match(out, "fwatch: probe forbidden%-syscall: EPERM",
  lang .. ": forbidden syscall (socket) denied with EPERM")
checks.expect_match(out, "forbidden%-syscall%-exit=20",
  lang .. ": probe --forbidden-syscall confirms denial (exit 20)")

-- 2. Landlock negative control: a read outside the sandboxed tree denied
-- with EACCES.
checks.expect_match(out, "fwatch: landlock ABI=%d+ enforced",
  lang .. ": landlock ABI enforced line printed")
checks.expect_match(out, "fwatch: probe outside /tmp/lsp33%-outside%-target/secret%.txt: EACCES",
  lang .. ": read outside the Landlock tree denied with EACCES")
checks.expect_match(out, "outside%-exit=20",
  lang .. ": probe --outside confirms denial (exit 20)")

-- 3. Positive control: the sandboxed watch loop still sees events inside
-- the tree and exits cleanly on timeout.
checks.expect_match(out, "event: %a+ inside%.txt",
  lang .. ": sandboxed watch still reports events for a file inside the tree")
checks.expect_match(out, "fwatch: exiting %(timeout%)",
  lang .. ": sandboxed watch exits cleanly on timeout")
checks.expect_match(out, "watch%-sandbox%-exit=0",
  lang .. ": watch --sandbox exits 0 (positive control passes)")

checks.finish()
