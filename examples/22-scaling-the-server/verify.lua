-- Verify chatterd v1 (22-scaling-the-server) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. epoll engine, two-party transcript — a listener receives the exact
--      "alice: hello world" line the sender broadcast; server stop summary
--      reports messages=1 peak_conns=2; clean exit
--   2. epoll engine, load — `flood 50` opens 50 concurrent connections and one
--      broadcast is DELIVERED to all 50 (delivered==connected==50, no dropped
--      frames), exit 0; server stop summary reports messages=1 peak_conns=50
--   3. threaded engine — the same two-party transcript and 50-client flood
--      still work and shut down cleanly
--   4. error shapes — no subcommand and bad flags exit 2; a client against a
--      dead port exits 1
--
-- Each scenario runs its own fresh daemon so the message and peak counts are
-- deterministic. Invoked from the example directory with LSP_LANG (cpp|go|rust)
-- and EXAMPLE_DIR set; the interpreter may be lua or luajit.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

-- Run a fresh daemon of ENGINE, discover its ephemeral port, execute BODY
-- (which may use $port and @WD@/@DEMO@), then SIGTERM the daemon and dump its
-- stderr. Returns the combined transcript + exit code.
local function scenario(engine, body)
  local template = [[
set -e
@DEMO@ serve --engine @ENGINE@ --port 0 2>@WD@/serve.err &
spid=$!
port=""
for _ in $(seq 1 200); do
  port=$(sed -n 's/.*on 127\.0\.0\.1:\([0-9]*\).*/\1/p' @WD@/serve.err)
  [ -n "$port" ] && break
  sleep 0.05
done
[ -n "$port" ] || { echo NO_PORT; kill $spid 2>/dev/null; exit 3; }
@BODY@
kill -TERM $spid 2>/dev/null || true
for _ in $(seq 1 200); do kill -0 $spid 2>/dev/null || break; sleep 0.05; done
echo "=== serve.err ==="
cat @WD@/serve.err
]]
  local script = template
    :gsub("@ENGINE@", engine)
    :gsub("@BODY@", body)
    :gsub("@DEMO@", demo)
    :gsub("@WD@", wd)
  return checks.run(script)
end

local two_party = [[
@DEMO@ listen --port $port bob --count 1 > @WD@/bob.out 2>/dev/null &
lp=$!
sleep 0.3
if @DEMO@ send --port $port alice "hello world" > @WD@/alice.out 2>/dev/null; then se=0; else se=$?; fi
echo "SEND_EXIT=$se"
if wait $lp; then le=0; else le=$?; fi
echo "LISTEN_EXIT=$le"
echo "ALICE=$(cat @WD@/alice.out)"
echo "BOB=$(cat @WD@/bob.out)"
]]

local flood = [[
if @DEMO@ flood --port $port 50 > @WD@/flood.out 2>/dev/null; then fe=0; else fe=$?; fi
echo "FLOOD_EXIT=$fe"
cat @WD@/flood.out
]]

-- ---------------------------------------------------------------------------
-- 1. epoll engine — two-party transcript
-- ---------------------------------------------------------------------------

local e1 = scenario("epoll", two_party)
checks.expect_exit(e1, 0, lang .. ": epoll two-party scenario exits 0")
checks.expect_match(e1.out, "chatterd: serving engine=epoll on 127%.0%.0%.1:%d+",
  lang .. ": epoll daemon announces its bound loopback port")
checks.expect_match(e1.out, "SEND_EXIT=0", lang .. ": epoll send exits 0")
checks.expect_match(e1.out, "LISTEN_EXIT=0", lang .. ": epoll listen exits 0")
checks.expect_match(e1.out, "ALICE=alice: hello world",
  lang .. ": epoll sender sees its own message echoed back")
checks.expect_match(e1.out, "BOB=alice: hello world",
  lang .. ": epoll listener receives the broadcast verbatim")
checks.expect_match(e1.out, "chatterd: stopped engine=epoll messages=1 peak_conns=2",
  lang .. ": epoll stop summary reports 1 message across 2 peak connections")

-- ---------------------------------------------------------------------------
-- 2. epoll engine — 50-connection flood, broadcast delivered to all
-- ---------------------------------------------------------------------------

local e2 = scenario("epoll", flood)
checks.expect_exit(e2, 0, lang .. ": epoll flood scenario exits 0")
checks.expect_match(e2.out, "flood: connected 50",
  lang .. ": epoll accepts all 50 concurrent flood connections")
checks.expect_match(e2.out, "flood: delivered 50",
  lang .. ": epoll broadcast is delivered to all 50 connections (no dropped frames)")
checks.expect_match(e2.out, "FLOOD_EXIT=0", lang .. ": epoll flood exits 0")
checks.expect_match(e2.out, "chatterd: stopped engine=epoll messages=1 peak_conns=50",
  lang .. ": epoll stop summary reports 1 message across 50 peak connections")

-- ---------------------------------------------------------------------------
-- 3. threaded engine — same transcript and flood still work
-- ---------------------------------------------------------------------------

local t1 = scenario("threaded", two_party)
checks.expect_exit(t1, 0, lang .. ": threaded two-party scenario exits 0")
checks.expect_match(t1.out, "BOB=alice: hello world",
  lang .. ": threaded listener receives the broadcast verbatim")
checks.expect_match(t1.out, "chatterd: stopped engine=threaded messages=1 peak_conns=2",
  lang .. ": threaded stop summary reports 1 message across 2 peak connections")

local t2 = scenario("threaded", flood)
checks.expect_exit(t2, 0, lang .. ": threaded flood scenario exits 0")
checks.expect_match(t2.out, "flood: delivered 50",
  lang .. ": threaded broadcast is delivered to all 50 connections")
checks.expect_match(t2.out, "chatterd: stopped engine=threaded messages=1 peak_conns=50",
  lang .. ": threaded stop summary reports 1 message across 50 peak connections")

-- ---------------------------------------------------------------------------
-- 4. error shapes
-- ---------------------------------------------------------------------------

local noargs = checks.run(demo)
checks.expect_exit(noargs, 2, lang .. ": no subcommand exits 2")
checks.expect_match(noargs.out, "usage: chatterd", lang .. ": no subcommand prints usage")

local badeng = checks.run(demo .. " serve --engine bogus --port 0")
checks.expect_exit(badeng, 2, lang .. ": unknown engine exits 2")

local badargs = checks.run(demo .. " flood --port 0")
checks.expect_exit(badargs, 2, lang .. ": flood without a count exits 2")

local dead = checks.run(demo .. " send --port 1 alice hi")
checks.expect_exit(dead, 1, lang .. ": a client against a dead port exits 1")
checks.expect_match(dead.out, "chatctl: cannot connect",
  lang .. ": the connect failure is reported")

checks.run("rm -rf " .. wd)
checks.finish()
