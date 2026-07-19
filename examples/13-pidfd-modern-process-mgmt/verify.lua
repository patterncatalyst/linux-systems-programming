-- Verify pmon v2 (13-pidfd-modern-process-mgmt) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. v0 regression — `run` mirrors the child's exit status and signal
--   2. pidfd engine (default) supervises a crashing child: per-spawn
--      "engine=pidfd child=<pid> pidfd=<fd>" lines, restart lines in order,
--      a giving-up line, exit 1
--   3. sigchld engine still works and produces byte-identical restart lines
--   4. clean child exit ends supervision with exit 0 and no restart
--   5. stop paths — SIGTERM to pmon and the overall timeout both terminate
--      the child (killed signal=15), print the exiting line, exit 0, and
--      leave no orphaned child behind (process-tree check via pgrep)
--   6. error shapes — usage errors exit 2, spawn failure exits 1
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set; the interpreter may be lua or luajit.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

local function count(out, pattern)
  local _, n = out:gsub(pattern, "")
  return n
end

-- All restart lines of one run, in order, as a single string.
local function restart_seq(out)
  local t = {}
  for line in out:gmatch("pmon: restart %d+/%d+") do
    t[#t + 1] = line
  end
  return table.concat(t, "\n")
end

-- ---------------------------------------------------------------------------
-- 1. v0 regression: run mirrors exit status and death-by-signal
-- ---------------------------------------------------------------------------

local ok0 = checks.run(demo .. " run -- sh -c 'exit 0'")
checks.expect_exit(ok0, 0, lang .. ": run mirrors a clean exit as 0")
checks.expect_match(ok0.out, "pmon: run child=%d+", lang .. ": run announces the child pid")
checks.expect_match(ok0.out, "pmon: child=%d+ exited status=0", lang .. ": run reports status 0")

local ok3 = checks.run(demo .. " run -- sh -c 'exit 3'")
checks.expect_exit(ok3, 3, lang .. ": run mirrors exit status 3")
checks.expect_match(ok3.out, "pmon: child=%d+ exited status=3", lang .. ": run reports status 3")

local sig9 = checks.run(demo .. " run -- sh -c 'kill -9 $$'")
checks.expect_exit(sig9, 137, lang .. ": run mirrors SIGKILL as 128+9")
checks.expect_match(sig9.out, "pmon: child=%d+ killed signal=9", lang .. ": run reports the killing signal")

-- ---------------------------------------------------------------------------
-- 2. pidfd engine (default): crashing child is restarted, then given up on
-- ---------------------------------------------------------------------------

local crash_pidfd = checks.run(demo .. " supervise --max-restarts 2 -- sh -c 'exit 1'")
checks.expect_exit(crash_pidfd, 1, lang .. ": pidfd engine exits 1 after the restart budget")
checks.expect_match(crash_pidfd.out, "pmon: engine=pidfd child=%d+ pidfd=%d+",
  lang .. ": pidfd engine announces child pid AND pidfd number")
checks.expect_match("spawns=" .. count(crash_pidfd.out, "pmon: engine=pidfd child=%d+ pidfd=%d+"),
  "^spawns=3$", lang .. ": pidfd engine spawns initial child + 2 restarts")
checks.expect_match("exits=" .. count(crash_pidfd.out, "pmon: child=%d+ exited status=1"),
  "^exits=3$", lang .. ": every crash is observed and reaped")
checks.expect_match(crash_pidfd.out,
  "pmon: restart 1/2.*pmon: restart 2/2.*pmon: giving up after 2 restarts",
  lang .. ": restart lines in order, then the giving-up line")

-- ---------------------------------------------------------------------------
-- 3. sigchld engine: same behavior, byte-identical restart lines
-- ---------------------------------------------------------------------------

local crash_sigchld = checks.run(demo ..
  " supervise --engine sigchld --max-restarts 2 -- sh -c 'exit 1'")
checks.expect_exit(crash_sigchld, 1, lang .. ": sigchld engine exits 1 after the restart budget")
checks.expect_match("spawns=" .. count(crash_sigchld.out, "pmon: engine=sigchld child=%d+"),
  "^spawns=3$", lang .. ": sigchld engine announces each spawn without a pidfd")
checks.expect_match("pidfd_lines=" .. count(crash_sigchld.out, "pidfd="),
  "^pidfd_lines=0$", lang .. ": sigchld engine output has no pidfd= field")
checks.expect_match(crash_sigchld.out, "pmon: giving up after 2 restarts",
  lang .. ": sigchld engine gives up identically")

local seq_pidfd, seq_sigchld = restart_seq(crash_pidfd.out), restart_seq(crash_sigchld.out)
checks.expect_match(seq_pidfd, "^pmon: restart 1/2\npmon: restart 2/2$",
  lang .. ": pidfd restart lines are exactly restart 1/2 then 2/2")
checks.expect_match(seq_pidfd == seq_sigchld and "identical" or
  ("pidfd<<" .. seq_pidfd .. ">> sigchld<<" .. seq_sigchld .. ">>"),
  "^identical$", lang .. ": restart lines identical across both engines")

-- ---------------------------------------------------------------------------
-- 4. clean child exit: supervision ends, no restart
-- ---------------------------------------------------------------------------

local clean = checks.run(demo .. " supervise -- sh -c 'exit 0'")
checks.expect_exit(clean, 0, lang .. ": clean child exit ends supervision with 0")
checks.expect_match(clean.out, "pmon: engine=pidfd child=%d+ pidfd=%d+",
  lang .. ": clean run still uses the pidfd engine by default")
checks.expect_match("restarts=" .. count(clean.out, "pmon: restart"),
  "^restarts=0$", lang .. ": a clean exit is never restarted")

-- ---------------------------------------------------------------------------
-- 5. stop paths: SIGTERM to pmon, and the overall timeout
-- ---------------------------------------------------------------------------

local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

for _, engine in ipairs({ "pidfd", "sigchld" }) do
  local stop = checks.run([[
set -e
]] .. demo .. [[ supervise --engine ]] .. engine .. [[ -- sleep 8.88 > ]] .. wd .. [[/stop.out 2>&1 &
pid=$!
sleep 0.5
kill -TERM $pid
wait $pid
cat ]] .. wd .. [[/stop.out
]])
  checks.expect_exit(stop, 0, lang .. ": " .. engine .. " engine exits 0 when pmon gets SIGTERM")
  checks.expect_match(stop.out, "pmon: child=%d+ killed signal=15",
    lang .. ": " .. engine .. " stop path forwards SIGTERM to the child")
  checks.expect_match(stop.out, "pmon: exiting %(signal%)",
    lang .. ": " .. engine .. " stop path prints the signal exit line")
end

local timeout = checks.run(demo .. " supervise --timeout-ms 500 -- sleep 7.77")
checks.expect_exit(timeout, 0, lang .. ": timeout stop exits 0")
checks.expect_match(timeout.out, "pmon: child=%d+ killed signal=15",
  lang .. ": timeout stop terminates the child via the pidfd")
checks.expect_match(timeout.out, "pmon: exiting %(timeout%)",
  lang .. ": timeout stop prints the timeout exit line")

-- Process-tree check: the supervised sleep must NOT survive its supervisor.
-- ([.] keeps this probe's own shell command line from matching itself.)
local orphans = checks.run("pgrep -f 'sleep 7[.]77'; pgrep -f 'sleep 8[.]88'; true")
checks.expect_match("survivors=<" .. orphans.out:gsub("%s+", "") .. ">",
  "^survivors=<>$", lang .. ": no supervised child outlives pmon")

-- ---------------------------------------------------------------------------
-- 6. error shapes
-- ---------------------------------------------------------------------------

local bogus = checks.run(demo .. " bogus -- true")
checks.expect_exit(bogus, 2, lang .. ": unknown subcommand exits 2")
checks.expect_match(bogus.out, "usage: pmon", lang .. ": unknown subcommand prints usage")

local nosep = checks.run(demo .. " supervise sh -c 'exit 0'")
checks.expect_exit(nosep, 2, lang .. ": missing -- separator exits 2")

local badengine = checks.run(demo .. " supervise --engine epoll -- true")
checks.expect_exit(badengine, 2, lang .. ": unknown engine exits 2")

local nospawn = checks.run(demo .. " supervise -- /no/such/binary")
checks.expect_exit(nospawn, 1, lang .. ": unspawnable command exits 1")
checks.expect_match(nospawn.out, "pmon: spawn: /no/such/binary",
  lang .. ": spawn failure names the command")

checks.run("rm -rf " .. wd)
checks.finish()
