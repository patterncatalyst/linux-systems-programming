-- Verify pmon v6 (namespaces & cgroups) — a VM example. Behavioral assertions:
--
--   1. PID NAMESPACE — `containerize -- /usr/bin/echo cmd-ran` must print
--      "pmon: child sees pid 1": the child only sees itself as pid 1 because
--      it is the first (and only) task in a fresh PID namespace.
--   2. UTS NAMESPACE — the same run must print "pmon: hostname=<name>" with
--      the hostname we asked for, and that name must be DIFFERENT from the
--      guest's real hostname (fetched independently over ssh) — proof the
--      change landed in a private UTS namespace, not the host's.
--   3. CGROUP MEMORY LIMIT (negative control) — a real memory hog started
--      under `--mem-max` far below what it tries to allocate must be
--      OOM-killed (SIGKILL, exit 137), not merely slowed down. Without the
--      cgroup limit doing real work, the same hog would run to completion;
--      the kill is the proof memory.max is live, not decorative.
--
-- Runs under the runner's vm mode (TARGET=systems-target, deploy via
-- deploy-to-vm.sh with SUDO=1). deploy-to-vm.sh flattens CMD's argv with a
-- bare `$*` on the remote side, so any CMD token containing spaces/shell
-- metacharacters (a `bash -c '...'` one-liner, say) gets corrupted in
-- transit. We route around that: the pid/hostname check uses a CMD of
-- plain, space-free tokens (`/usr/bin/echo cmd-ran`), and the memory hog is
-- staged as a real script file on the guest first, then invoked as
-- `sh /tmp/pmon-hog.sh` — two more plain tokens.

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

local home = os.getenv("HOME") or "/root"
local ssh_opts =
  "-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=" .. home .. "/.ssh/known_hosts"

-- Resolve the guest IP (same helper deploy-to-vm.sh uses).
local ipf = io.popen(repo_root .. "/scripts/lab/vm-ip.sh '" .. target .. "' 2>/dev/null")
local ip = ipf and ipf:read("*l") or nil
if ipf then ipf:close() end
if not ip or ip == "" then
  checks.skip("cannot resolve IP for VM '" .. target .. "' (lab down?)")
end

-- ---------------------------------------------------------------------------
-- 1 & 2: pid namespace + uts namespace, via a trivial CMD.
-- ---------------------------------------------------------------------------

local ns_hostname = "pmon-vm-check"
local pos = checks.run(string.format(
  "SUDO=1 ./demo.sh %s run containerize --hostname %s -- /usr/bin/echo cmd-ran",
  lang, ns_hostname))
checks.expect_exit(pos, 0, lang .. ": containerize (echo) exits 0")
checks.expect_match(pos.out, "pmon: child sees pid 1",
  lang .. ": child observes itself as pid 1 (pid namespace)")
-- ns_hostname contains literal '-', a Lua pattern quantifier magic char, so
-- it must be escaped (%-) before use as a pattern, not just concatenated.
checks.expect_match(pos.out, "pmon: hostname=" .. ns_hostname:gsub("%-", "%%-"),
  lang .. ": child observes the requested hostname (uts namespace)")
checks.expect_match(pos.out, "cmd%-ran",
  lang .. ": CMD itself ran (echo output present)")
checks.expect_match(pos.out, "pmon: cgroup mem%.pressure some=%d+%.%d+",
  lang .. ": cgroup PSI memory.pressure was read")
checks.expect_match(pos.out, "pmon: child exited status=0",
  lang .. ": clean exit mirrored")

-- The uts namespace only proves anything if the name we set is not the
-- guest's own hostname to begin with (fetched independently, not from pmon).
local hn = checks.run(string.format("ssh %s 'fedora@%s' hostname", ssh_opts, ip))
checks.expect_exit(hn, 0, "fetch guest's real hostname over ssh")
local host_hostname = hn.out:match("^%s*(.-)%s*$")
if host_hostname == ns_hostname then
  checks.expect_match("mismatch-forced", "never-matches",
    lang .. ": test hostname '" .. ns_hostname .. "' collided with the real guest hostname")
else
  checks.expect_match("uts namespace isolated the hostname change", "isolated",
    lang .. ": containerized hostname (" .. ns_hostname .. ") differs from guest's real hostname (" ..
      host_hostname .. ")")
end

-- ---------------------------------------------------------------------------
-- 3: cgroup memory.max negative control — a real hog gets OOM-killed.
-- ---------------------------------------------------------------------------

-- Stage the hog as a file (not a `bash -c '...'` argument — see file header)
-- so deploy-to-vm.sh's flattening never sees its internal spaces/pipe/`;`.
local hog_path = "/tmp/pmon-hog.sh"
local stage = checks.run(string.format(
  [[ssh %s 'fedora@%s' "cat > %s && chmod +x %s" <<'HOGEOF'
#!/bin/sh
a=$(head -c 200000000 /dev/zero | tr '\0' x)
sleep 2
echo should-not-print
HOGEOF
]], ssh_opts, ip, hog_path, hog_path))
checks.expect_exit(stage, 0, lang .. ": stage memory-hog script on guest")

-- 64 MiB ceiling, no swap (containerize disables memory.swap.max itself):
-- comfortably above any of the three runtimes' own baseline RSS, but far
-- below the ~190 MiB the hog tries to hold, so the hog — not pmon itself —
-- is what the cgroup OOM killer picks.
local neg = checks.run(string.format(
  "SUDO=1 ./demo.sh %s run containerize --mem-max 67108864 --cgroup pmon-hog-%s -- sh %s",
  lang, lang, hog_path))
checks.expect_exit(neg, 137, lang .. ": memory hog under memory.max is OOM-killed (exit 137)")
checks.expect_match(neg.out, "pmon: child sees pid 1",
  lang .. ": hog run still entered the pid namespace before being killed")
checks.expect_match(neg.out, "pmon: child killed signal=9 %(KILL%)",
  lang .. ": kill is reported as SIGKILL, not a graceful exit")
if neg.out:find("should%-not%-print") then
  checks.expect_match("hog completed", "never-matches",
    lang .. ": hog ran to completion — memory.max did not constrain it")
else
  checks.expect_match("hog did not complete", "did not complete",
    lang .. ": hog never reached its post-allocation echo — killed mid-allocation")
end

checks.finish()
