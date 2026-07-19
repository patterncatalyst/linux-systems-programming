-- Verify spscring for LSP_LANG: the SPSC lock-free ring delivers every item
-- exactly once (sum == N*(N-1)/2) under real producer/consumer concurrency,
-- with both cache-line-padding modes, at a healthy ring size and at capacity 1
-- (maximum head/tail contention); the throughput line is well formed; and a
-- missing argument prints the usage line and exits 2.
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

-- N*(N-1)/2 for the values 0..N-1 the producer pushes.
local function triangular(n)
  -- n <= 10^6 here, so this stays well inside Lua 5.4 integer range.
  return (n * (n - 1)) // 2
end

-- Run spscring and assert the output line is byte-shaped as documented and
-- carries the expected items / sum / pad token, with a parseable throughput.
local function check_run(capacity, items, pad_flag, pad_token, label)
  local cmd = demo .. " --capacity " .. capacity .. " --items " .. items
  if pad_flag then
    cmd = cmd .. " --pad " .. pad_flag
  end
  local r = checks.run(cmd)
  checks.expect_exit(r, 0, label .. ": exits 0")

  local want_sum = triangular(items)
  -- Full line shape: everything is pinned except the throughput number.
  local pat = "^spscring: items=" .. items ..
              " sum=" .. want_sum ..
              " throughput_mops=%d+%.%d+ pad=" .. pad_token .. "%s*$"
  checks.expect_match(r.out, pat,
    label .. ": line is 'items=" .. items .. " sum=" .. want_sum ..
    " throughput_mops=<float> pad=" .. pad_token .. "'")

  -- Independently confirm the throughput field parses as a positive number.
  local mops = r.out:match("throughput_mops=([%d%.]+)")
  if mops and tonumber(mops) and tonumber(mops) > 0 then
    checks.expect_match("ok", "ok", label .. ": throughput_mops parses as a positive number")
  else
    checks.expect_match("no", "yes",
      label .. ": throughput_mops parses as a positive number (got " .. tostring(mops) .. ")")
  end
end

-- 1. Healthy ring, no padding: correctness of the acquire/release protocol.
check_run(1024, 1000000, "off", "off", lang .. " cap=1024 pad=off")

-- 2. Healthy ring, cache-line padding on: same delivered sum.
check_run(1024, 1000000, "on", "on", lang .. " cap=1024 pad=on")

-- 3. Capacity 1: producer and consumer hand off one slot at a time, the
--    worst case for the head/tail handshake; sum must still be exact.
check_run(1, 1000, nil, "off", lang .. " cap=1 default-pad")

-- 4. CLI contract: no arguments -> usage on stderr, exit 2.
local u = checks.run(demo)
checks.expect_exit(u, 2, lang .. ": missing args exits 2")
checks.expect_match(u.out, "usage: spscring %-%-capacity K %-%-items N",
  lang .. ": missing args prints usage")

checks.finish()
