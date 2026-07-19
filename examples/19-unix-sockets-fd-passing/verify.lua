-- Verify pmon v5 (19-unix-sockets-fd-passing) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. usage errors exit 2; pmctl against a missing socket exits 1
--   2. supervise: control socket appears; status reports child/uptime/restarts
--      and the reported child pid is a live process running the supervised
--      command (process-tree check via /proc)
--   3. SO_PEERCRED: the supervisor logs the CALLING uid for every ctl connect
--   4. logfd: pmctl's "via-fd:" output equals `tail -3` of the log file byte
--      for byte — proof the fd crossed the socket and was read directly
--   5. restarts: a short-lived child drives restarts=N>0 in status, restart
--      lines on stderr, and one "pmon: start child" log line per (re)start
--   6. stop: reply is exactly "stopping"; supervisor exits, removes the
--      socket, and the child process group is gone
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

-- Scratch area. NB: sun_path caps UNIX socket paths at ~107 bytes, so this
-- must stay short — mktemp -d under /tmp is fine, deep CI workspaces are not.
local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end
local uid = checks.run("id -u").out:gsub("%s+$", "")

local function sh(script)
  return checks.run(script:gsub("@WD@", wd))
end

-- ---------------------------------------------------------------------------
-- 1. error shapes
-- ---------------------------------------------------------------------------

local noargs = checks.run(demo)
checks.expect_exit(noargs, 2, lang .. ": no arguments exits 2")
checks.expect_match(noargs.out, "usage: pmon", lang .. ": no arguments prints usage")

local badact = sh(demo .. " pmctl --ctl @WD@/x.sock frobnicate")
checks.expect_exit(badact, 2, lang .. ": unknown pmctl action exits 2")

local nosock = sh(demo .. " pmctl --ctl @WD@/absent.sock status")
checks.expect_exit(nosock, 1, lang .. ": pmctl against a missing socket exits 1")
checks.expect_match(nosock.out, "pmctl: error:", lang .. ": connect failure is reported")

-- ---------------------------------------------------------------------------
-- 2. supervise + status (long-lived quiet child)
-- ---------------------------------------------------------------------------

local start = sh([[
]] .. demo .. [[ supervise --ctl @WD@/ctl.sock --log @WD@/pmon.log -- \
  /bin/sh -c 'echo alpha; echo beta; echo gamma; exec sleep 30' \
  > @WD@/sup.out 2> @WD@/sup.err &
echo $! > @WD@/sup.pid
for i in $(seq 1 100); do [ -S @WD@/ctl.sock ] && break; sleep 0.05; done
sleep 0.3
test -S @WD@/ctl.sock
]])
checks.expect_exit(start, 0, lang .. ": control socket appears after supervise starts")

local st = sh(demo .. " pmctl --ctl @WD@/ctl.sock status")
checks.expect_exit(st, 0, lang .. ": status exits 0")
checks.expect_match(st.out, "^child=%d+ uptime=%d+s restarts=0\n$",
  lang .. ": status is exactly 'child=<pid> uptime=<s>s restarts=<n>'")

local child = st.out:match("child=(%d+)") or "0"
local tree = checks.run("tr '\\0' ' ' < /proc/" .. child .. "/cmdline")
checks.expect_exit(tree, 0, lang .. ": status child pid is a live process")
checks.expect_match(tree.out, "sleep 30", lang .. ": child pid is running the supervised command")

-- ---------------------------------------------------------------------------
-- 3. SO_PEERCRED
-- ---------------------------------------------------------------------------

local creds = sh("cat @WD@/sup.err")
checks.expect_match(creds.out, "pmon: listening on " .. wd .. "/ctl%.sock",
  lang .. ": supervisor announces the ctl socket")
checks.expect_match(creds.out, "pmon: started child pid=" .. child,
  lang .. ": supervisor announces the child pid")
checks.expect_match(creds.out, "pmon: ctl connect uid=" .. uid .. " pid=%d+",
  lang .. ": SO_PEERCRED line shows the calling uid")

-- ---------------------------------------------------------------------------
-- 4. logfd: the tail arrives through the passed descriptor
-- ---------------------------------------------------------------------------

local lf = sh(demo .. " pmctl --ctl @WD@/ctl.sock logfd | tee @WD@/viafd.out")
checks.expect_exit(lf, 0, lang .. ": logfd exits 0")
checks.expect_match(lf.out, "via%-fd: gamma", lang .. ": log tail read via the received fd")
local _, nlines = lf.out:gsub("via%-fd: [^\n]*\n", "")
checks.expect_match("lines=" .. nlines, "^lines=3$", lang .. ": logfd prints exactly 3 via-fd lines")

local cmp = sh([[
tail -n 3 @WD@/pmon.log | sed 's/^/via-fd: /' > @WD@/expect.out
diff -u @WD@/expect.out @WD@/viafd.out
]])
checks.expect_exit(cmp, 0, lang .. ": via-fd output equals tail -3 of the log file byte for byte")

-- ---------------------------------------------------------------------------
-- 5. restarts (short-lived child)
-- ---------------------------------------------------------------------------

local r2 = sh([[
]] .. demo .. [[ supervise --ctl @WD@/ctl2.sock --log @WD@/pmon2.log -- \
  /bin/sh -c 'exec sleep 0.2' > /dev/null 2> @WD@/sup2.err &
for i in $(seq 1 100); do [ -S @WD@/ctl2.sock ] && break; sleep 0.05; done
sleep 1.3
]] .. demo .. [[ pmctl --ctl @WD@/ctl2.sock status
]])
checks.expect_exit(r2, 0, lang .. ": status works while the child is being restarted")
checks.expect_match(r2.out, "child=%d+ uptime=%d+s restarts=[1-9]%d*",
  lang .. ": a dying child drives restarts > 0")

local r2err = sh("cat @WD@/sup2.err")
checks.expect_match(r2err.out, "pmon: child pid=%d+ exited status=0",
  lang .. ": each child exit is reported with its status")
checks.expect_match(r2err.out, "pmon: restart 1 child pid=%d+",
  lang .. ": restarts are numbered on stderr")

-- One "pmon: start child" log line per (re)start. Another restart may land
-- between the status read and this grep, so assert >= rather than ==.
local restarts = tonumber(r2.out:match("restarts=(%d+)")) or 0
local starts = tonumber(sh("grep -c '^pmon: start child pid=' @WD@/pmon2.log").out:match("%d+")) or 0
local label = lang .. ": log has one 'pmon: start child' line per (re)start"
if starts >= restarts + 1 and starts >= 2 then
  checks.expect_match("ok", "ok", label)
else
  checks.expect_match(string.format("starts=%d restarts=%d", starts, restarts), "^impossible$", label)
end

local stop2 = sh(demo .. " pmctl --ctl @WD@/ctl2.sock stop")
checks.expect_exit(stop2, 0, lang .. ": stop of the restarting supervisor exits 0")

-- ---------------------------------------------------------------------------
-- 6. stop: graceful teardown of the first supervisor
-- ---------------------------------------------------------------------------

local sp = sh(demo .. " pmctl --ctl @WD@/ctl.sock stop")
checks.expect_exit(sp, 0, lang .. ": stop exits 0")
checks.expect_match(sp.out, "^stopping\n$", lang .. ": stop reply is exactly 'stopping'")

local gone = sh([[
for i in $(seq 1 100); do kill -0 $(cat @WD@/sup.pid) 2>/dev/null || break; sleep 0.05; done
kill -0 $(cat @WD@/sup.pid) 2>/dev/null && exit 1
test -e @WD@/ctl.sock && exit 2
kill -0 ]] .. child .. [[ 2>/dev/null && exit 3
exit 0
]])
checks.expect_exit(gone, 0,
  lang .. ": after stop the supervisor exits, unlinks the socket, and the child group is gone")
checks.expect_match(sh("cat @WD@/sup.err").out, "pmon: stopping",
  lang .. ": supervisor logs the stop")

checks.run("rm -rf " .. wd)
checks.finish()
