-- Verify 34-our-programs-in-containers for LSP_LANG.
--
-- This example is LOCAL but requires podman: it builds the language's
-- Containerfile, runs the image rootless under real --cpus/--memory limits,
-- and asserts observable effects, not merely exit 0:
--
--   1. container-aware resource detection: the CONSTRAINED cpu.max/mem.max
--      are reported, not the host's -- and specifically that C++'s
--      effective_parallelism is the HOST cpu count (hardware_concurrency's
--      trap) while Go's and Rust's match the cgroup quota.
--   2. this process really is PID 1 of its own namespace (pid=1 ppid=0,
--      /proc/1/status inside the container), and no zombie accumulates
--      while jobs are spawned and reaped.
--   3. debugging across namespaces: podman exec AND a rootless
--      `podman unshare nsenter` from the host both see the same PID 1.
--   4. pid-1 signal duties: `serve` reacts to SIGTERM almost instantly and
--      prints a graceful shutdown line; `naive` never prints that line
--      (proving no graceful path ran) and -- for C++/Rust, whose default
--      SIGTERM disposition is masked out by the kernel's PID-1 special
--      case -- takes the full stop grace period and is SIGKILLed. Go's
--      runtime is a real, verified exception: it installs its own handler
--      for SIGTERM at startup regardless of signal.Notify, so a "naive" Go
--      binary still dies promptly -- just abruptly, with no cleanup.
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set; the interpreter may be lua or luajit. Images and
-- containers are removed unconditionally at the end.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

if checks.run("command -v podman").exit ~= 0 then
  checks.skip("podman not available on this host")
end

local image = "lsp34-" .. lang
local serve_c = "lsp34-" .. lang .. "-serve-verify"
local naive_c = "lsp34-" .. lang .. "-naive-verify"

-- Cleanup helper: safe to call at any point, never fails the run.
local function cleanup()
  checks.run("podman rm -f " .. serve_c .. " " .. naive_c .. " >/dev/null 2>&1; true")
end
cleanup() -- in case a previous run left something behind

-- ---------------------------------------------------------------------------
-- 0. host shape: the cpu-vs-host contrast needs a host with more than the
--    2 cpus this test constrains the container to, or C++'s "wrong" number
--    would coincidentally match the "right" one.
-- ---------------------------------------------------------------------------

local nproc = checks.run("nproc")
local host_nproc = tonumber((nproc.out:gsub("%s+", "")))
if not host_nproc or host_nproc < 3 then
  checks.skip("host has only " .. tostring(host_nproc) ..
    " cpu(s); need >= 3 to demonstrate the cgroup-vs-host mismatch at --cpus=2")
end

-- ---------------------------------------------------------------------------
-- 1. build the image
-- ---------------------------------------------------------------------------

local build = checks.run("podman build -t " .. image .. " -f " .. lang .. "/Containerfile " .. lang)
checks.expect_exit(build, 0, lang .. ": podman build succeeds")

-- ---------------------------------------------------------------------------
-- 2. serve, constrained: resource detection, pid-1 identity, reaping
-- ---------------------------------------------------------------------------

local run_serve = checks.run(
  "podman run -d --name " .. serve_c .. " --cpus=2 --memory=128m " .. image .. " serve")
checks.expect_exit(run_serve, 0, lang .. ": serve container starts")

-- Let a handful of job-spawn/reap cycles happen before reading logs.
checks.run("sleep 5")
local logs = checks.run("podman logs " .. serve_c)

checks.expect_match(logs.out, "container: cpu%.max=200000/100000",
  lang .. ": reports the CONSTRAINED cpu.max (--cpus=2), not the host's")
checks.expect_match(logs.out, "mem%.max=134217728",
  lang .. ": reports the CONSTRAINED mem.max (--memory=128m), not the host's")
checks.expect_match(logs.out, "app: pid=1 ppid=0",
  lang .. ": this process really is PID 1 of its own namespace (ppid=0)")
checks.expect_match(logs.out, "app: worker started pid=%d+",
  lang .. ": serve starts the supervised worker")

local _, reap_count = logs.out:gsub("app: reaped pid=%d+ status=0", "")
checks.expect_match("reaps=" .. reap_count, "^reaps=[2-9]%d*$",
  lang .. ": several jobs were spawned and reaped (no pile-up)")

if lang == "cpp" then
  checks.expect_match(logs.out, "effective_parallelism=" .. host_nproc,
    "cpp: hardware_concurrency() reports the HOST cpu count (" .. host_nproc ..
    ") even though the container is capped at 2 -- the trap")
else
  checks.expect_match(logs.out, "effective_parallelism=2",
    lang .. ": reports the cgroup-constrained parallelism (2), not the host's " ..
    host_nproc)
end

-- ---------------------------------------------------------------------------
-- 3. debugging across namespaces: podman exec + rootless podman-unshare nsenter
-- ---------------------------------------------------------------------------

local status = checks.run("podman exec " .. serve_c .. " cat /proc/1/status")
checks.expect_match(status.out, "Name:%s*app",
  lang .. ": podman exec sees this container's PID 1 as `app`")

local zscript = table.concat({
  "podman exec " .. serve_c .. " bash -c '",
  "z=0; for d in /proc/[0-9]*; do",
  " line=$(cat \"$d/stat\" 2>/dev/null) || continue;",
  " rest=${line##*) }; state=${rest%% *};",
  ' [[ "$state" == "Z" ]] && z=$((z+1));',
  " done; echo zombies=$z'",
}, "\n")
local zombies = checks.run(zscript)
checks.expect_match(zombies.out, "zombies=0",
  lang .. ": no zombie processes accumulate inside the container")

local hostpid_r = checks.run("podman inspect --format '{{.State.Pid}}' " .. serve_c)
local hostpid = (hostpid_r.out:gsub("%s+", ""))
local nsenter = checks.run(
  "podman unshare nsenter --target " .. hostpid .. " --pid --mount -- readlink /proc/1/exe")
checks.expect_match(nsenter.out, "/usr/local/bin/app",
  lang .. ": rootless `podman unshare nsenter` from the HOST sees the same PID 1")

-- ---------------------------------------------------------------------------
-- 4a. serve: SIGTERM stops it almost instantly, with a graceful log line
-- ---------------------------------------------------------------------------

local stop_serve = checks.run(table.concat({
  "t0=$(date +%s%N)",
  "podman stop -t 3 " .. serve_c .. " >/dev/null",
  "t1=$(date +%s%N)",
  "echo elapsed_ms=$(( (t1 - t0) / 1000000 ))",
}, "; "))
local ms_serve = tonumber(stop_serve.out:match("elapsed_ms=(%d+)"))
checks.expect_match("fast=" .. tostring(ms_serve ~= nil and ms_serve < 1500),
  "^fast=true$", lang .. ": serve honors SIGTERM well within the 3s grace (" ..
  tostring(ms_serve) .. "ms)")
local final_serve_logs = checks.run("podman logs " .. serve_c)
checks.expect_match(final_serve_logs.out, "app: shutting down %(SIGTERM%)",
  lang .. ": serve prints its graceful shutdown line")

-- ---------------------------------------------------------------------------
-- 4b. naive: no graceful path ever runs. C++/Rust additionally hang out the
--     full grace period (PID-1 default-action masking); Go's runtime kills
--     it promptly anyway (see the top-of-file note) but just as ungracefully.
-- ---------------------------------------------------------------------------

local run_naive = checks.run("podman run -d --name " .. naive_c .. " " .. image .. " naive")
checks.expect_exit(run_naive, 0, lang .. ": naive container starts")
checks.run("sleep 1")

local stop_naive = checks.run(table.concat({
  "t0=$(date +%s%N)",
  "podman stop -t 2 " .. naive_c .. " >/dev/null 2>/dev/null",
  "t1=$(date +%s%N)",
  "echo elapsed_ms=$(( (t1 - t0) / 1000000 ))",
}, "; "))
local ms_naive = tonumber(stop_naive.out:match("elapsed_ms=(%d+)"))

if lang == "go" then
  checks.expect_match("fast=" .. tostring(ms_naive ~= nil and ms_naive < 1000),
    "^fast=true$",
    "go: even naive dies promptly on SIGTERM -- the runtime installs its own " ..
    "default handler regardless of signal.Notify (" .. tostring(ms_naive) .. "ms)")
else
  checks.expect_match("slow=" .. tostring(ms_naive ~= nil and ms_naive >= 1700),
    "^slow=true$",
    lang .. ": naive ignores SIGTERM as PID 1 -- podman waits out the grace " ..
    "period and falls back to SIGKILL (" .. tostring(ms_naive) .. "ms)")
end

local naive_logs = checks.run("podman logs " .. naive_c .. " 2>&1; true")
checks.expect_match("shutdown_lines=" .. select(2, naive_logs.out:gsub("shutting down", "")),
  "^shutdown_lines=0$",
  lang .. ": naive never runs the graceful shutdown path (no cleanup, no message)")

-- ---------------------------------------------------------------------------
-- cleanup: remove containers and the built image
-- ---------------------------------------------------------------------------

cleanup()
checks.run("podman rmi -f " .. image .. " >/dev/null 2>&1; true")

checks.finish()
