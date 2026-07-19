-- Verify pmon v3 (identity & privilege) — a VM example. Behavioral assertions:
--
--   1. `drop --user nobody --keep-cap net_bind_service -- CMD bindprobe :80`
--      must run CMD as an UNPRIVILEGED uid (65534, not 0) yet still bind :80.
--      That pairing is impossible without the ambient CAP_NET_BIND_SERVICE
--      surviving the setuid + execve — so it is the proof the cap was carried.
--
--   2. NEGATIVE CONTROL — the same drop WITHOUT --keep-cap must fail the bind
--      with "Permission denied" and exit 3. Same uid, cap withheld, port
--      denied: this is what makes assertion 1 meaningful rather than trivially
--      true (a root process, or a lowered ip_unprivileged_port_start, would
--      pass assertion 1 while proving nothing).
--
-- Runs under the runner's vm mode (TARGET=systems-target, deploy via
-- deploy-to-vm.sh with SUDO=1). Because /home/fedora is mode 700, the dropped
-- user cannot reach a binary staged there, so we stage a world-readable copy
-- of the exec target in /tmp and point CMD at it.

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

-- Resolve the guest IP (same helper deploy-to-vm.sh uses).
local ipf = io.popen(repo_root .. "/scripts/lab/vm-ip.sh '" .. target .. "' 2>/dev/null")
local ip = ipf and ipf:read("*l") or nil
if ipf then ipf:close() end
if not ip or ip == "" then
  checks.skip("cannot resolve IP for VM '" .. target .. "' (lab down?)")
end

-- Stage a world-readable copy of the exec target on the guest (/tmp is 1777;
-- /home/fedora is 700 and unreachable by the dropped user).
local ssh_opts =
  "-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=" .. home .. "/.ssh/known_hosts"
local staged = "/tmp/pmon-verify"
local stage_cmd = string.format(
  "scp %s '%s' 'fedora@%s:%s' && ssh %s 'fedora@%s' 'chmod 755 %s'",
  ssh_opts, local_bin, ip, staged, ssh_opts, ip, staged)
local st = checks.run(stage_cmd)
checks.expect_exit(st, 0, lang .. ": stage exec target on guest")

-- Positive: drop to nobody, KEEP the cap, bind :80.
local pos = checks.run(string.format(
  "SUDO=1 ./demo.sh %s run drop --user nobody --keep-cap net_bind_service -- %s bindprobe --port 80",
  lang, staged))
checks.expect_exit(pos, 0, lang .. ": drop+keep-cap bindprobe :80 exits 0")
checks.expect_match(pos.out, "bindprobe: uid=%d+ bound :80",
  lang .. ": prints bound-:80 proof line")
local uid = pos.out:match("bindprobe: uid=(%d+) bound :80")
if uid and tonumber(uid) ~= 0 then
  checks.expect_match(pos.out, "uid=65534 bound :80",
    lang .. ": bound :80 as unprivileged nobody (uid=65534, not 0)")
else
  checks.expect_match("uid=" .. tostring(uid), "never-matches",
    lang .. ": uid must be non-root but was " .. tostring(uid))
end

-- Negative control: same drop, NO cap — bind :80 must be denied, exit 3.
local neg = checks.run(string.format(
  "SUDO=1 ./demo.sh %s run drop --user nobody -- %s bindprobe --port 80",
  lang, staged))
checks.expect_exit(neg, 3, lang .. ": drop WITHOUT keep-cap exits 3")
checks.expect_match(neg.out, "bindprobe: bind :80: Permission denied",
  lang .. ": :80 denied without the ambient cap")

checks.finish()
