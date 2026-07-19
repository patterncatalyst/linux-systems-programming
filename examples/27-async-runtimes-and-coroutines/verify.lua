-- Verify chatterd v4 for LSP_LANG: the async engine (C++ coroutines / tokio /
-- Go goroutines) delivers a broadcast to 20 clients, the thread engine still
-- delivers it too (prior engine kept working), the daemon shuts down cleanly on
-- SIGTERM, and the CLI contract holds. Invoked from the example directory with
-- LSP_LANG (cpp|go|rust) and REPO_ROOT set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

-- Scratch area for the background-daemon driver script.
local tmp = checks.run("mktemp -d").out:gsub("%s+$", "")
if tmp == "" or not tmp:match("^/") then
  checks.skip("mktemp -d failed")
end
local function cleanup()
  os.execute("rm -rf '" .. tmp .. "'")
end

-- ---------------------------------------------------------------------------
-- Driver: start chatterd in the background on a chosen engine/port, wait for
-- the port, run the 20-client loadtest, then SIGTERM the daemon and report the
-- daemon's exit code and stderr. All output is echoed on stdout so a single
-- checks.run() captures the whole transcript. Runs under bash for /dev/tcp.
-- ---------------------------------------------------------------------------
local driver = tmp .. "/drive.sh"
do
  local f = assert(io.open(driver, "w"))
  f:write([[
#!/usr/bin/env bash
set -u
LANG_SEL="$1"; ENGINE="$2"; PORT="$3"
SRVLOG="$(mktemp)"
./demo.sh "$LANG_SEL" run serve --engine "$ENGINE" --port "$PORT" >/dev/null 2>"$SRVLOG" &
sv=$!
up=0
for _ in $(seq 1 200); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
    exec 3>&- 3<&-; up=1; break
  fi
  sleep 0.05
done
[ "$up" = 1 ] || echo "SERVER_NEVER_UP"
./demo.sh "$LANG_SEL" run loadtest --port "$PORT" --clients 20
echo "LOADTEST_EXIT=$?"
kill -TERM "$sv" 2>/dev/null
wait "$sv"; echo "SERVER_EXIT=$?"
echo "----SERVERLOG----"
cat "$SRVLOG"
rm -f "$SRVLOG"
exit 0
]])
  f:close()
end

-- Run one engine's broadcast test and assert the observable outcome.
local function broadcast_test(engine, port)
  local r = checks.run("bash " .. driver .. " " .. lang .. " " .. engine .. " " .. port)
  local tag = lang .. "/" .. engine .. ": "
  checks.expect_match(r.out, "chatterd: listening on 127%.0%.0%.1:" .. port ..
    " engine=" .. engine, tag .. "daemon announces the listening port and engine")
  checks.expect_match(r.out, "loadtest: delivered 20/20",
    tag .. "broadcast reaches all 20 clients")
  checks.expect_match(r.out, "LOADTEST_EXIT=0", tag .. "loadtest exits 0")
  checks.expect_match(r.out, "SERVER_EXIT=0", tag .. "daemon exits 0 on SIGTERM")
  checks.expect_match(r.out, "chatterd: shutdown", tag .. "daemon logs a clean shutdown")
  if not r.out:find("delivered 20/20") then
    print(r.out) -- surface the transcript on failure
  end
end

-- 1. The async engine (this chapter's rewrite) delivers to all 20 clients.
broadcast_test("async", 47180)

-- 2. The prior thread engine still delivers identically.
broadcast_test("thread", 47181)

-- ---------------------------------------------------------------------------
-- 3. CLI contract: no subcommand -> usage on stderr, exit 2.
-- ---------------------------------------------------------------------------
local u = checks.run(demo)
checks.expect_exit(u, 2, lang .. ": missing subcommand exits 2")
checks.expect_match(u.out, "usage:", lang .. ": missing subcommand prints usage")

-- 4. Unknown engine -> exit 2 with a diagnostic (no port bound).
local e = checks.run(demo .. " serve --engine bogus")
checks.expect_exit(e, 2, lang .. ": unknown engine exits 2")
checks.expect_match(e.out, "unknown engine 'bogus'", lang .. ": unknown engine is diagnosed")

cleanup()
checks.finish()
