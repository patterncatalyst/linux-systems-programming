-- Verify pmon v1: assert supervisor BEHAVIOR, not exit-0.
--   * v0 `run` still propagates child exit status and signal deaths
--   * crash loop: exact restart lines, doubling backoff values, distinct
--     child pids, giving-up line, exit 1, and real elapsed sleep time
--   * SIGTERM/SIGINT: clean shutdown line, exit 0, child actually reaped
--   * SIGHUP: reload line, replacement child with a new pid, old pid dead
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set. Plain Lua 5.1+ (works under luajit).

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local bins = {
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
}
local bin = bins[lang]

local function demo(args)
  return checks.run("./demo.sh " .. lang .. " run " .. args)
end

-- Boolean assertion via the checks pattern API.
local function expect_true(cond, label)
  checks.expect_match(cond and "yes" or "no", "yes", label)
end

local ws = checks.run("mktemp -d").out:gsub("%s+$", "")
assert(ws:match("^/"), "mktemp -d must yield an absolute path")

-- Ensure the binary exists (the orchestrator builds before verifying; a
-- standalone run should fail loudly here instead of deep in a scenario).
local r = checks.run("test -x " .. bin)
checks.expect_exit(r, 0, lang .. ": built binary " .. bin .. " exists")

-- 1. Usage: bare invocation and a missing "--" both refuse with exit 2.
r = demo("")
checks.expect_exit(r, 2, lang .. ": bare invocation exits 2")
checks.expect_match(r.out, "usage: pmon run %-%- CMD", lang .. ": prints usage")
r = demo("run echo hi")
checks.expect_exit(r, 2, lang .. ": run without -- exits 2")

-- 2. v0 `run`: exit status propagates; signal deaths become 128+N.
r = demo("run -- sh -c 'exit 3'")
checks.expect_exit(r, 3, lang .. ": run propagates child exit 3")
checks.expect_match(r.out, "pmon: child %d+ exited status=3",
  lang .. ": run reports child status")
r = demo("run -- sh -c 'kill -9 $$'")
checks.expect_exit(r, 137, lang .. ": run maps SIGKILL death to exit 137")
checks.expect_match(r.out, "pmon: child %d+ killed signal=9",
  lang .. ": run reports the killing signal")

-- 3. supervise with a clean child: no restart, exit 0.
r = demo("supervise --max-restarts 3 -- true")
checks.expect_exit(r, 0, lang .. ": clean child ends supervision with 0")
checks.expect_match(r.out, "pmon: child %d+ exited status=0",
  lang .. ": clean exit reported")
expect_true(not r.out:find("pmon: restart"),
  lang .. ": clean exit triggers no restart")

-- 4. Crash loop: two restarts with doubling backoff, then give up.
r = checks.run(string.format([[
  start=$(date +%%s%%N)
  ./demo.sh %s run supervise --max-restarts 2 --backoff-ms 120 -- sh -c 'exit 1'
  code=$?
  end=$(date +%%s%%N)
  echo "code=$code elapsed_ms=$(( (end - start) / 1000000 ))"
]], lang))
checks.expect_match(r.out, "pmon: restart #1 %(backoff 120ms%)",
  lang .. ": first restart uses the base backoff")
checks.expect_match(r.out, "pmon: restart #2 %(backoff 240ms%)",
  lang .. ": second backoff doubles")
checks.expect_match(r.out, "pmon: giving up after 2 restarts",
  lang .. ": gives up when the budget is spent")
checks.expect_match(r.out, "code=1 ", lang .. ": crash loop exits 1")
local pids = {}
for pid in r.out:gmatch("pmon: started pid (%d+)") do pids[#pids + 1] = pid end
expect_true(#pids == 3, lang .. ": three children were started (got " .. #pids .. ")")
expect_true(#pids == 3 and pids[1] ~= pids[2] and pids[2] ~= pids[3]
  and pids[1] ~= pids[3], lang .. ": each restart is a fresh pid")
local elapsed = tonumber(r.out:match("elapsed_ms=(%d+)"))
expect_true(elapsed ~= nil and elapsed >= 340,
  lang .. ": backoffs really slept 120+240ms (elapsed " ..
  tostring(elapsed) .. "ms)")

-- 5. SIGTERM and SIGINT: forward SIGTERM, reap the child, exit 0.
for _, sig in ipairs({ "TERM", "INT" }) do
  r = checks.run(string.format([[
    log=%s/sig%s.log
    ./%s supervise -- sleep 30 > "$log" 2>&1 &
    pid=$!
    i=0; while [ $i -lt 50 ]; do
      grep -q "started pid" "$log" && break; sleep 0.1; i=$((i+1)); done
    child=$(awk '/started pid/{print $4}' "$log" | head -1)
    kill -%s $pid
    wait $pid; echo "supervisor_exit=$?"
    sleep 0.1
    if kill -0 "$child" 2>/dev/null; then echo "child=alive"; else echo "child=gone"; fi
    cat "$log"
  ]], ws, sig, bin, sig))
  checks.expect_match(r.out, "pmon: shutting down %(SIG" .. sig .. "%)",
    lang .. ": SIG" .. sig .. " prints the shutdown line")
  checks.expect_match(r.out, "supervisor_exit=0",
    lang .. ": SIG" .. sig .. " shuts down with exit 0")
  checks.expect_match(r.out, "child=gone",
    lang .. ": SIG" .. sig .. " leaves no child process behind")
end

-- 6. SIGHUP: reload line, replacement child, backoff/restart state reset.
r = checks.run(string.format([[
  log=%s/hup.log
  ./%s supervise -- sleep 30 > "$log" 2>&1 &
  pid=$!
  i=0; while [ $i -lt 50 ]; do
    grep -q "started pid" "$log" && break; sleep 0.1; i=$((i+1)); done
  first=$(awk '/started pid/{print $4}' "$log" | head -1)
  kill -HUP $pid
  i=0; while [ $i -lt 50 ]; do
    [ "$(grep -c "started pid" "$log")" -ge 2 ] && break; sleep 0.1; i=$((i+1)); done
  second=$(awk '/started pid/{print $4}' "$log" | sed -n 2p)
  sleep 0.2
  if kill -0 "$first" 2>/dev/null; then echo "first=alive"; else echo "first=gone"; fi
  if kill -0 "$second" 2>/dev/null; then echo "second=alive"; else echo "second=gone"; fi
  [ -n "$first" ] && [ -n "$second" ] && [ "$first" != "$second" ] \
    && echo "distinct=yes" || echo "distinct=no"
  kill -TERM $pid
  wait $pid; echo "supervisor_exit=$?"
  sleep 0.1
  if kill -0 "$second" 2>/dev/null; then echo "final=alive"; else echo "final=gone"; fi
  cat "$log"
]], ws, bin))
checks.expect_match(r.out, "pmon: reload requested",
  lang .. ": SIGHUP prints the reload line")
checks.expect_match(r.out, "distinct=yes",
  lang .. ": reload starts a replacement child with a new pid")
checks.expect_match(r.out, "first=gone",
  lang .. ": reload terminates the old child")
checks.expect_match(r.out, "second=alive",
  lang .. ": replacement child is running after reload")
checks.expect_match(r.out, "supervisor_exit=0",
  lang .. ": post-reload SIGTERM still shuts down cleanly")
checks.expect_match(r.out, "final=gone",
  lang .. ": shutdown reaps the replacement child too")
expect_true(not r.out:find("pmon: restart #"),
  lang .. ": reload is not counted as a crash restart")

checks.run("rm -rf " .. ws)
checks.finish()
