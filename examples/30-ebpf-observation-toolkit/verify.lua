-- Verify the eBPF observation toolkit (chapter 30) — a VM example. This is
-- TOOLING (bcc-tools + bpftrace) observing our userspace binary via uprobes
-- and tracepoints; we write no kernel-side eBPF program of our own.
--
-- The workload (`app work --seconds N`) is deployed to the lab guest, then
-- observe.sh (staged alongside it, run as root via SUDO) starts the workload
-- in the background and points five separate tools at it in turn. Each
-- assertion below is the OBSERVABLE proof that a specific tool really saw a
-- real event our program produced, not just that the tool ran without error:
--
--   1. opensnoop  — shows our bait file's path opened by comm "app".
--   2. execsnoop  — shows a "true" child with PPID == our workload's pid.
--   3. funccount  — a nonzero per-call count for busy_hash() via uprobe.
--   4. offcputime — a stack naming comm "app" (the sleep between iterations
--                   put it off-CPU during the trace window).
--   5. bpftrace   — a one-liner uprobe on busy_hash() reports @calls > 0.
--
-- funccount/bpftrace both target the pattern "<bin>:*busy_hash*", which
-- matches the plain symbol "busy_hash" (cpp/rust) and the Go runtime's
-- "main.busy_hash" without needing a different pattern per language.
--
-- Runs under the runner's vm mode (TARGET=systems-target). Staging goes
-- straight through ssh/scp (not deploy-to-vm.sh, which only shells out a
-- single binary+args — this needs a custom root-run driver script too).

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
local local_observe = example_dir .. "/observe.sh"

-- Resolve the guest IP (same helper deploy-to-vm.sh uses).
local ipf = io.popen(repo_root .. "/scripts/lab/vm-ip.sh '" .. target .. "' 2>/dev/null")
local ip = ipf and ipf:read("*l") or nil
if ipf then ipf:close() end
if not ip or ip == "" then
  checks.skip("cannot resolve IP for VM '" .. target .. "' (lab down?)")
end

local ssh_opts =
  "-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=" .. home .. "/.ssh/known_hosts"

-- Stage the binary under a per-language directory so the deployed file's
-- basename — and therefore its /proc comm — is exactly "app" regardless of
-- which language built it, matching the book-wide "binary stays app" rule.
local remote_dir = "/tmp/lsp30-" .. lang
local remote_bin = remote_dir .. "/app"
local remote_observe = "/tmp/lsp30-observe.sh"

local stage_cmd = string.format(
  "ssh %s 'fedora@%s' 'mkdir -p %s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "ssh %s 'fedora@%s' 'chmod 755 %s %s'",
  ssh_opts, ip, remote_dir,
  ssh_opts, local_bin, ip, remote_bin,
  ssh_opts, local_observe, ip, remote_observe,
  ssh_opts, ip, remote_bin, remote_observe)
local st = checks.run(stage_cmd)
checks.expect_exit(st, 0, lang .. ": stage app binary + observe.sh on guest")

-- Run the observation driver as root; 60s of workload comfortably covers the
-- ~34s of sequential tool budgets (opensnoop 6s, execsnoop 6s, funccount 7s,
-- offcputime 8s, bpftrace 7s) plus ssh/bcc-compile overhead.
local observe = checks.run(string.format(
  "ssh %s 'fedora@%s' 'sudo bash %s %s 60'",
  ssh_opts, ip, remote_observe, remote_bin))
checks.expect_exit(observe, 0, lang .. ": observe.sh exits 0")
if observe.exit ~= 0 then
  print(observe.out)
end
local out = observe.out

checks.expect_match(out, "work: start seconds=60 pid=%d+ bait=/tmp/lsp%-ebpf%-work%-bait%.txt",
  lang .. ": workload prints its start line")
checks.expect_match(out,
  "work: done seconds=60 iters=%d+ opens=%d+ execs=%d+ busy_calls=%d+ busy_hash=%d+",
  lang .. ": workload prints its done/summary line")

-- 1. opensnoop saw our bait file opened by comm "app".
checks.expect_match(out, "app%s+%d+%s+%d+%s+/tmp/lsp%-ebpf%-work%-bait%.txt",
  lang .. ": opensnoop shows app opening the bait file")

-- 2. execsnoop saw the "true" child, parented by our workload's pid.
local app_pid = out:match("workload pid=(%d+)")
if app_pid then
  checks.expect_match(out, "true%s+%d+%s+" .. app_pid .. "%s+0%s+/usr/[a-z]*/?true",
    lang .. ": execsnoop shows a true child with PPID=" .. app_pid)
else
  checks.expect_match(out, "\1IMPOSSIBLE\1",
    lang .. ": could not read workload pid from observe.sh output")
end

-- 3. funccount counted real calls to busy_hash() via uprobe (nonzero count).
checks.expect_match(out, "busy_hash%s+[1-9]%d*",
  lang .. ": funccount reports a nonzero busy_hash call count")

-- 4. offcputime attributed off-CPU time to a stack naming comm "app".
checks.expect_match(out, "app %(%d+%)", lang .. ": offcputime shows a stack for comm \"app\"")

-- 5. bpftrace's uprobe on busy_hash fired (nonzero @calls).
checks.expect_match(out, "@calls:%s*[1-9]%d*",
  lang .. ": bpftrace uprobe busy_hash calls is nonzero")

checks.finish()
