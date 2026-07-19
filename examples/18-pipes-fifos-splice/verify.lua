-- Verify pmon v4 (18-pipes-fifos-splice) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. usage errors exit 2 with the usage text
--   2. v0 `run` still mirrors exits — status 3 stays 3, SIGKILL becomes 137
--      with "killed signal=9"
--   3. v2 pidfd engine + v4 capture: a child that fails once and then
--      succeeds yields two "engine=pidfd child=... pidfd=..." spawn lines,
--      "restart 1/3", exit 0 — and the log holds the child's stdout AND
--      stderr as "[out] ..."/"[err] ..." lines from both runs
--   4. v1 sigchld engine still works with the capture attached: same restart
--      shape, no "pidfd=" anywhere, "giving up" exit 1, log filled
--   5. v2 stop path survives the pipes: --timeout-ms TERMs the child
--      ("killed signal=15"), prints "exiting (timeout)", exit 0
--   6. v4 tail: the FIFO is created; a cat reader receives the log backlog
--      plus lines appended by a live supervise run; killing the cat makes
--      tail print "pmon: tail reader detached" ONCE and survive (kill -0);
--      a reattached cat receives the line appended while detached AND later
--      appends (nothing lost); SIGTERM ends tail with "exiting (signal)",
--      exit 0
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

-- Scratch area for logs, FIFOs, and reader captures.
local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

local function sh(script)
  return checks.run(script:gsub("@WD@", wd))
end

-- ---------------------------------------------------------------------------
-- 1. usage shapes
-- ---------------------------------------------------------------------------

local bogus = checks.run(demo .. " bogus")
checks.expect_exit(bogus, 2, lang .. ": unknown subcommand exits 2")
checks.expect_match(bogus.out, "usage: pmon", lang .. ": unknown subcommand prints usage")

local nofifo = checks.run(demo .. " tail --log x.log")
checks.expect_exit(nofifo, 2, lang .. ": tail without --fifo exits 2")

-- ---------------------------------------------------------------------------
-- 2. v0 run: exit mirroring + stdout passthrough
-- ---------------------------------------------------------------------------

local r = sh(demo .. [[ run -- /bin/sh -c 'echo out-line; echo err-line >&2; exit 3']])
checks.expect_exit(r, 3, lang .. ": run mirrors the child's exit status 3")
checks.expect_match(r.out, "pmon: run child=%d+", lang .. ": run announces the child pid")
checks.expect_match(r.out, "out%-line", lang .. ": run passes child stdout through")
checks.expect_match(r.out, "pmon: child=%d+ exited status=3", lang .. ": run reports the exit status")

local sigd = sh(demo .. [[ run -- /bin/sh -c 'kill -KILL $$']])
checks.expect_exit(sigd, 137, lang .. ": run exits 128+9 for a SIGKILLed child")
checks.expect_match(sigd.out, "pmon: child=%d+ killed signal=9", lang .. ": run reports the signal")

-- ---------------------------------------------------------------------------
-- 3. pidfd engine + capture: two pipes into the log, restart on failure
-- ---------------------------------------------------------------------------

local sup = sh([[
set -e
rm -f @WD@/ok @WD@/pmon.log
]] .. demo .. [[ supervise --max-restarts 3 --log @WD@/pmon.log -- /bin/sh -c \
  'echo run-out; echo run-err >&2; if [ -f @WD@/ok ]; then exit 0; else touch @WD@/ok; exit 1; fi'
]])
checks.expect_exit(sup, 0, lang .. ": supervise exits 0 once the child succeeds")
checks.expect_match(sup.out, "pmon: engine=pidfd child=%d+ pidfd=%d+",
  lang .. ": pidfd engine announces child and pidfd")
checks.expect_match(sup.out, "pmon: child=%d+ exited status=1", lang .. ": failing run reported")
checks.expect_match(sup.out, "pmon: restart 1/3", lang .. ": supervise restarts after failure")
checks.expect_match(sup.out, "pmon: child=%d+ exited status=0", lang .. ": clean run reported")
local _, starts = sup.out:gsub("pmon: engine=pidfd child=%d+ pidfd=%d+", "")
checks.expect_match("starts=" .. starts, "^starts=2$", lang .. ": exactly two child launches")

local log = sh("cat @WD@/pmon.log")
checks.expect_exit(log, 0, lang .. ": supervise created the log file")
local _, outs = log.out:gsub("%[out%] run%-out", "")
local _, errs = log.out:gsub("%[err%] run%-err", "")
checks.expect_match(string.format("out=%d err=%d", outs, errs), "^out=2 err=2$",
  lang .. ": log has [out]/[err] prefixed lines from both streams of both runs")

-- ---------------------------------------------------------------------------
-- 4. sigchld engine still works with the capture attached
-- ---------------------------------------------------------------------------

local sig = sh(demo ..
  [[ supervise --engine sigchld --max-restarts 1 --log @WD@/sigchld.log -- /bin/sh -c 'echo sig-out; exit 5']])
checks.expect_exit(sig, 1, lang .. ": sigchld engine exits 1 when restarts are exhausted")
checks.expect_match(sig.out, "pmon: engine=sigchld child=%d+", lang .. ": sigchld spawn line")
checks.expect_match(sig.out, "pmon: restart 1/1", lang .. ": sigchld restart line shape")
checks.expect_match(sig.out, "pmon: giving up after 1 restarts", lang .. ": sigchld gives up")
local _, npidfd = sig.out:gsub("pidfd=", "")
checks.expect_match("npidfd=" .. npidfd, "^npidfd=0$",
  lang .. ": sigchld output has no pidfd= field")
local slog = sh("cat @WD@/sigchld.log")
local _, souts = slog.out:gsub("%[out%] sig%-out", "")
checks.expect_match("souts=" .. souts, "^souts=2$",
  lang .. ": sigchld engine captured both runs into the log")

-- ---------------------------------------------------------------------------
-- 5. the v2 stop path survives the pipes: timeout TERMs the child
-- ---------------------------------------------------------------------------

local timeo = sh(demo .. [[ supervise --timeout-ms 500 --log @WD@/timeo.log -- sleep 30]])
checks.expect_exit(timeo, 0, lang .. ": timeout stop path exits 0")
checks.expect_match(timeo.out, "pmon: child=%d+ killed signal=15",
  lang .. ": supervised child is TERMed on timeout")
checks.expect_match(timeo.out, "pmon: exiting %(timeout%)", lang .. ": timeout exit line")

-- ---------------------------------------------------------------------------
-- 6. tail: FIFO relay, reader detach survival, reattach without loss
-- ---------------------------------------------------------------------------

-- The FIFO is fed from the pmon.log written by step 3; live appends flow
-- through a second real supervise run, then direct appends around the
-- detach/reattach churn. Every phase transition is polled, not slept blind.
local tail = sh([[
wait_for() { i=0; while [ $i -lt 100 ]; do grep -qF "$2" "$1" 2>/dev/null && return 0; sleep 0.1; i=$((i+1)); done; return 1; }
rm -f @WD@/t.fifo @WD@/cat1.out @WD@/cat2.out @WD@/tail.out @WD@/tail.err
timeout 120 ]] .. demo .. [[ tail --log @WD@/pmon.log --fifo @WD@/t.fifo > @WD@/tail.out 2> @WD@/tail.err &
tpid=$!
i=0; while [ $i -lt 100 ]; do [ -p @WD@/t.fifo ] && break; sleep 0.1; i=$((i+1)); done
[ -p @WD@/t.fifo ] && echo "fifo-created"
cat @WD@/t.fifo > @WD@/cat1.out &
c1=$!
wait_for @WD@/cat1.out "[out] run-out" && echo "cat1-got-backlog"
rm -f @WD@/ok
]] .. demo .. [[ supervise --max-restarts 0 --log @WD@/pmon.log -- /bin/sh -c 'echo live-out; echo live-err >&2' >/dev/null 2>&1
wait_for @WD@/cat1.out "[out] live-out" && wait_for @WD@/cat1.out "[err] live-err" && echo "cat1-got-live"
kill "$c1"; wait "$c1" 2>/dev/null
printf '[out] while-detached\n' >> @WD@/pmon.log
wait_for @WD@/tail.out "pmon: tail reader detached" && echo "tail-reported-detach"
if kill -0 "$tpid" 2>/dev/null; then echo "tail-survived"; fi
cat @WD@/t.fifo > @WD@/cat2.out &
c2=$!
wait_for @WD@/cat2.out "[out] while-detached" && echo "cat2-got-missed-line"
printf '[err] after-reattach\n' >> @WD@/pmon.log
wait_for @WD@/cat2.out "[err] after-reattach" && echo "cat2-got-new-line"
kill -TERM "$tpid"
wait "$tpid"; echo "tail-exit=$?"
wait "$c2" 2>/dev/null
echo "detach-count=$(grep -cF 'pmon: tail reader detached' @WD@/tail.out)"
echo "== tail.out =="; cat @WD@/tail.out
echo "== tail.err =="; cat @WD@/tail.err
]])
checks.expect_exit(tail, 0, lang .. ": tail scenario script completes")
checks.expect_match(tail.out, "fifo%-created", lang .. ": tail creates the FIFO")
checks.expect_match(tail.out, "pmon: tail ready %(fifo ", lang .. ": tail announces readiness")
checks.expect_match(tail.out, "cat1%-got%-backlog", lang .. ": first reader receives the existing log lines")
checks.expect_match(tail.out, "cat1%-got%-live", lang ..
  ": lines from a live supervise run flow supervise -> log -> tail -> FIFO -> cat")
checks.expect_match(tail.out, "tail%-reported%-detach", lang .. ": EPIPE is reported as reader detached")
checks.expect_match(tail.out, "tail%-survived", lang .. ": tail keeps running after the reader dies")
checks.expect_match(tail.out, "cat2%-got%-missed%-line", lang ..
  ": line appended while detached reaches the reattached reader (no loss)")
checks.expect_match(tail.out, "cat2%-got%-new%-line", lang .. ": reattached reader receives new lines")
checks.expect_match(tail.out, "detach%-count=1", lang .. ": exactly one detach report")
checks.expect_match(tail.out, "tail%-exit=0", lang .. ": SIGTERM ends tail with exit 0")
checks.expect_match(tail.out, "pmon: exiting %(signal%)", lang .. ": tail prints the signal exit line")

checks.run("rm -rf " .. wd)
checks.finish()
