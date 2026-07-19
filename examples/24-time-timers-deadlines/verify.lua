-- Verify chatterd v3 (chapter 24: time, timers, deadlines). Asserts OBSERVABLE
-- behavior, not exit-0:
--   * clockprobe prints a monotonic-clock line with three parseable fields
--     (res ns, nanosleep 1ms us, timerfd 1ms us) and names CLOCK_MONOTONIC,
--     never CLOCK_REALTIME (deadlines must be measured on a steady clock).
--   * two peers link over loopback and deliver a chat message each way.
--   * killing the listener makes the connector declare "peer A timed out"
--     (the deadline firing on silence) then reconnect with a jittered,
--     growing backoff ("reconnecting to A in <ms>ms").
--   * a SIGTERM shuts the connector down cleanly (exit 0).
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

local function expect_true(cond, label)
  checks.expect_match(cond and "yes" or "no", "yes", label)
end

-- The orchestrator builds before verifying; fail loudly here if it did not.
local r = checks.run("test -x " .. bin)
checks.expect_exit(r, 0, lang .. ": built binary " .. bin .. " exists")

-- ---------------------------------------------------------------------------
-- 1. Usage / arg handling.
-- ---------------------------------------------------------------------------
r = checks.run("./demo.sh " .. lang .. " run")
checks.expect_exit(r, 2, lang .. ": bare invocation exits 2")
checks.expect_match(r.out, "usage: chatterd", lang .. ": prints usage")

r = checks.run("./" .. bin .. " bogus-subcommand")
checks.expect_exit(r, 2, lang .. ": unknown subcommand exits 2")

-- ---------------------------------------------------------------------------
-- 2. clockprobe: three parseable fields on a monotonic-clock line.
-- ---------------------------------------------------------------------------
r = checks.run("./demo.sh " .. lang .. " run clockprobe")
checks.expect_exit(r, 0, lang .. ": clockprobe exits 0")
checks.expect_match(r.out, "clockprobe: CLOCK_MONOTONIC res=%d+ns",
  lang .. ": clockprobe reports CLOCK_MONOTONIC resolution in ns")
checks.expect_match(r.out, "nanosleep%(1ms%) actual=%d+us",
  lang .. ": clockprobe reports nanosleep(1ms) actual in us")
checks.expect_match(r.out, "timerfd%(1ms%) actual=%d+us",
  lang .. ": clockprobe reports timerfd(1ms) actual in us")
expect_true(not r.out:find("CLOCK_REALTIME"),
  lang .. ": clockprobe uses a monotonic clock, not the wall clock")
-- The two 1ms sleepers should each land in a sane [1ms, 100ms] window.
local ns_us = tonumber(r.out:match("nanosleep%(1ms%) actual=(%d+)us"))
local tfd_us = tonumber(r.out:match("timerfd%(1ms%) actual=(%d+)us"))
expect_true(ns_us ~= nil and ns_us >= 1000 and ns_us <= 100000,
  lang .. ": nanosleep(1ms) actually slept ~1ms (got " .. tostring(ns_us) .. "us)")
expect_true(tfd_us ~= nil and tfd_us >= 1000 and tfd_us <= 100000,
  lang .. ": timerfd(1ms) actually waited ~1ms (got " .. tostring(tfd_us) .. "us)")

-- ---------------------------------------------------------------------------
-- 3. Heartbeat + deadline + reconnect: two peers, kill one, drive the other.
-- ---------------------------------------------------------------------------
r = checks.run(string.format([[
  set -u
  bin=%s
  work=$(mktemp -d)
  alog=$work/a.log; blog=$work/b.log
  "./$bin" listen --name A --addr 127.0.0.1:0 \
    --heartbeat-ms 80 --timeout-ms 320 --message "hi-from-A" >"$alog" 2>&1 &
  apid=$!
  port=""
  for i in $(seq 1 100); do
    port=$(sed -n 's/.*listening on 127.0.0.1:\([0-9]*\).*/\1/p' "$alog")
    [ -n "$port" ] && break
    sleep 0.05
  done
  echo "port=$port"
  "./$bin" connect --name B --peer "A@127.0.0.1:$port" \
    --heartbeat-ms 80 --timeout-ms 320 --backoff-ms 120 --max-backoff-ms 2000 \
    --message "hi-from-B" --seed 42 >"$blog" 2>&1 &
  bpid=$!
  trap 'kill -9 $apid $bpid 2>/dev/null; rm -rf "$work"' EXIT
  # wait for both directions of the chat to land
  for i in $(seq 1 100); do
    grep -q "message from A" "$blog" 2>/dev/null \
      && grep -q "message from B" "$alog" 2>/dev/null && break
    sleep 0.05
  done
  echo "=== A LOG (link phase) ==="; cat "$alog"
  echo "=== B LOG (link phase) ==="; cat "$blog"
  # kill the listener; the connector must notice the silence and reconnect
  kill -9 $apid 2>/dev/null
  for i in $(seq 1 160); do
    n=$(grep -c "reconnecting to A in" "$blog" 2>/dev/null || echo 0)
    [ "$n" -ge 3 ] && break
    sleep 0.05
  done
  # clean shutdown of the connector
  kill -TERM $bpid 2>/dev/null
  wait $bpid; echo "b_exit=$?"
  echo "=== B LOG (final) ==="; cat "$blog"
]], bin))

checks.expect_match(r.out, "port=%d+", lang .. ": listener bound an ephemeral port")
checks.expect_match(r.out, "chatterd: A linked with B", lang .. ": listener links with connector")
checks.expect_match(r.out, "chatterd: B linked with A", lang .. ": connector links with listener")
checks.expect_match(r.out, "chatterd: A message from B: hi%-from%-B",
  lang .. ": message delivered connector -> listener")
checks.expect_match(r.out, "chatterd: B message from A: hi%-from%-A",
  lang .. ": message delivered listener -> connector")
checks.expect_match(r.out, "chatterd: peer A timed out",
  lang .. ": connector declares the listener timed out after silence")

-- Count and inspect the jittered reconnect lines.
local delays = {}
for ms in r.out:gmatch("chatterd: reconnecting to A in (%d+)ms") do
  delays[#delays + 1] = tonumber(ms)
end
expect_true(#delays >= 2,
  lang .. ": at least two reconnect attempts (got " .. #delays .. ")")
-- Equal-jitter backoff from base 120ms: first delay in [60,120], and the
-- window grows (later attempts can exceed the first base ceiling).
expect_true(#delays >= 1 and delays[1] >= 60 and delays[1] <= 120,
  lang .. ": first backoff is jittered within [60,120]ms (got " ..
  tostring(delays[1]) .. ")")
local grew = false
for _, d in ipairs(delays) do
  if d > 120 then grew = true end
end
expect_true(grew,
  lang .. ": backoff grows past the base ceiling across attempts")

checks.expect_match(r.out, "b_exit=0", lang .. ": SIGTERM shuts the connector down with exit 0")
checks.expect_match(r.out, "chatterd: B shutting down",
  lang .. ": connector prints its clean-shutdown line")

checks.finish()
