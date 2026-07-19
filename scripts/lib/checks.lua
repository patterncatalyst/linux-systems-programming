-- checks.lua — tiny assertion helpers for verify.lua scripts (Lua 5.4, no deps).
-- Failures print "FAIL: label" and are counted; nothing is fatal until finish().
--
--   local checks = require("checks")
--   local r = checks.run("./demo.sh cpp run")
--   checks.expect_exit(r, 0, "demo exits cleanly")
--   checks.expect_match(r.out, "pid %d+ on Linux", "prints pid/uname line")
--   checks.finish()

local M = {}

local npass, nfail = 0, 0

local function ok(label)
  npass = npass + 1
  print("ok: " .. label)
end

local function bad(label, detail)
  nfail = nfail + 1
  print("FAIL: " .. label)
  if detail then print("  " .. detail) end
end

-- Run a shell command; return {exit=<int>, out=<combined stdout+stderr>}.
function M.run(cmd)
  -- Subshell so the redirect covers every command in a compound cmd string.
  local f = assert(io.popen("( " .. cmd .. " ) 2>&1", "r"))
  local out = f:read("*a") or ""
  local _, how, code = f:close()
  local exit
  if how == "exit" then
    exit = code
  elseif how == "signal" then
    exit = 128 + code
  else
    exit = 0
  end
  return { exit = exit, out = out }
end

function M.expect_exit(r, code, label)
  if r.exit == code then
    ok(label)
  else
    bad(label, string.format("expected exit %d, got %d", code, r.exit))
  end
end

-- pattern is a Lua pattern, not a regex.
function M.expect_match(out, pattern, label)
  if string.find(out, pattern) then
    ok(label)
  else
    bad(label, "output did not match pattern: " .. pattern)
  end
end

-- Probe a TCP port via nc when available, else bash's /dev/tcp.
function M.expect_port_open(host, port, label)
  local probe = string.format(
    "if command -v nc >/dev/null 2>&1; then nc -z -w 2 '%s' %d; " ..
    "else bash -c 'exec 3<>\"/dev/tcp/%s/%d\"'; fi",
    host, port, host, port)
  local r = M.run(probe)
  if r.exit == 0 then
    ok(label)
  else
    bad(label, string.format("port %s:%d not reachable", host, port))
  end
end

-- Exit 77 (automake convention) so the aggregator can distinguish skips.
function M.skip(reason)
  print("SKIP: " .. reason)
  os.exit(77)
end

function M.finish()
  print(string.format("PASS %d / FAIL %d", npass, nfail))
  os.exit(nfail > 0 and 1 or 0)
end

return M
