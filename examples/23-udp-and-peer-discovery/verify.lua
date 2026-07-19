-- Verify chatterd v2 (23-udp-and-peer-discovery) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust. This lane is
-- mode:vm-peer, but the verify itself is the LOCAL two-process assertion: two
-- chatterd instances with distinct names run on loopback multicast, discover
-- each other, and exchange one canonical chatterd TCP chat frame (JOIN then
-- DELIVER — the same frame ch21/22/24/27 share). (The real two-host demo
-- across systems-target/systems-peer is driven separately by the chapter's
-- demo; see README.) demo.sh run is forced local here by clearing TARGET, so
-- the assertion never touches a VM.
--
-- Checks:
--   1. usage errors exit 2 (no subcommand, unknown flag, malformed --group);
--      an unassignable --iface fails at bind time with exit 1
--   2. two instances (alice, bob) each print exactly one
--      "discovered peer <other> at 127.0.0.1:<port>" line
--   3. after discovery each opens a TCP connection to the peer's advertised
--      port and prints "peer <other> says: hello from <other>"
--   4. both processes exit 0 and announce themselves on stderr
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

-- Force demo.sh run to execute locally even when the runner set TARGET for the
-- vm-peer lane: an empty TARGET makes demo.sh take its loopback branch.
local demo = "TARGET= ./demo.sh " .. lang .. " run discover"

-- Distinct ports per language so back-to-back cpp/go/rust runs never collide on
-- a lingering TIME_WAIT socket.
local base = ({ cpp = 0, go = 100, rust = 200 })[lang]
local group = "239.23.7.1"
local uport = 51823 + base
local aport = 9231 + base
local bport = 9232 + base

local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

local function sh(script)
  return checks.run((script:gsub("@WD@", wd):gsub("@DEMO@", demo)))
end

-- ---------------------------------------------------------------------------
-- 1. error shapes
-- ---------------------------------------------------------------------------

local noargs = checks.run("TARGET= ./demo.sh " .. lang .. " run")
checks.expect_exit(noargs, 2, lang .. ": no subcommand exits 2")
checks.expect_match(noargs.out, "usage: chatterd discover", lang .. ": prints usage")

local unknown = sh("@DEMO@ --group " .. group .. " --port " .. uport ..
  " --name x --frobnicate")
checks.expect_exit(unknown, 2, lang .. ": unknown flag exits 2")

local badgroup = sh("@DEMO@ --group 999.1.1.1 --port " .. uport .. " --name x")
checks.expect_exit(badgroup, 2, lang .. ": malformed --group exits 2")

-- 192.0.2.0/24 is TEST-NET-1: valid syntax, never assigned to this host, so the
-- TCP bind fails at runtime -> exit 1 (not a usage error).
local badiface = sh("@DEMO@ --group " .. group .. " --port " .. uport ..
  " --name x --iface 192.0.2.1 --tcp-port " .. aport .. " --rounds 1")
checks.expect_exit(badiface, 1, lang .. ": unassignable --iface exits 1")
checks.expect_match(badiface.out, "chatterd: error:", lang .. ": bind failure is reported")

-- ---------------------------------------------------------------------------
-- 2 + 3. two instances discover each other and exchange a message
-- ---------------------------------------------------------------------------

local params = "--group " .. group .. " --port " .. uport ..
  " --iface 127.0.0.1 --announce-ms 150 --rounds 12"

local run = sh([[
@DEMO@ ]] .. params .. [[ --name alice --tcp-port ]] .. aport .. [[ \
  > @WD@/alice.out 2> @WD@/alice.err & apid=$!
@DEMO@ ]] .. params .. [[ --name bob --tcp-port ]] .. bport .. [[ \
  > @WD@/bob.out 2> @WD@/bob.err & bpid=$!
wait $apid; echo "alice-exit=$?" >> @WD@/exits
wait $bpid; echo "bob-exit=$?"   >> @WD@/exits
]])
checks.expect_exit(run, 0, lang .. ": both instances run to completion")

local exits = sh("cat @WD@/exits").out
checks.expect_match(exits, "alice%-exit=0", lang .. ": alice exits 0")
checks.expect_match(exits, "bob%-exit=0", lang .. ": bob exits 0")

local aout = sh("cat @WD@/alice.out").out
local bout = sh("cat @WD@/bob.out").out
local aerr = sh("cat @WD@/alice.err").out
local berr = sh("cat @WD@/bob.err").out

checks.expect_match(aout, "discovered peer bob at 127%.0%.0%.1:" .. bport,
  lang .. ": alice discovers bob at his advertised tcp port")
checks.expect_match(bout, "discovered peer alice at 127%.0%.0%.1:" .. aport,
  lang .. ": bob discovers alice at her advertised tcp port")

checks.expect_match(aout, "peer bob says: hello from bob",
  lang .. ": alice receives bob's chat frame over TCP")
checks.expect_match(bout, "peer alice says: hello from alice",
  lang .. ": bob receives alice's chat frame over TCP")

-- discovery is deduped: exactly one "discovered peer" line per process.
local _, an = aout:gsub("discovered peer ", "")
local _, bn = bout:gsub("discovered peer ", "")
checks.expect_match("count=" .. an, "^count=1$",
  lang .. ": alice discovers bob exactly once (dedup)")
checks.expect_match("count=" .. bn, "^count=1$",
  lang .. ": bob discovers alice exactly once (dedup)")

checks.expect_match(aerr, "chatterd: announcing as alice on " .. group,
  lang .. ": alice announces on stderr")
checks.expect_match(berr, "chatterd: announcing as bob on " .. group,
  lang .. ": bob announces on stderr")

checks.run("rm -rf " .. wd)
checks.finish()
