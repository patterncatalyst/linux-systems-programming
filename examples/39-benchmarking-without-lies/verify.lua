-- Verify benchlab (39-benchmarking-without-lies) for LSP_LANG.
--
-- Asserts observable behavior, identical in shape across cpp/go/rust:
--   1. for each op (encode/decode/roundtrip), a real run (--iters/--warmup)
--      emits a well-formed percentile table with sane monotonic ordering
--      (min <= median <= p99 <= max), a coordinated-omission-corrected p99
--      that is never below the raw p99, n == the requested iters, and a
--      checksum proving the codec actually ran (not skipped/optimized away)
--   2. the same fixed workload run twice produces the same checksum both
--      times (the codec's output is deterministic even though the ns
--      timings are not) — the "repeatable-order" contract
--   3. --lie runs, exits 0, and its output contains no percentile table at
--      all — that absence is the whole point of the contrast
--   4. error shapes: no flags, an unknown --op, and --warmup >= --iters all
--      exit 2 with a usage line
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

-- Boolean assertion via the checks.lua pattern-match API: match "ok" against
-- "ok" when true, or force a mismatch (with the real detail in the label)
-- when false, so failures show up the same way expect_match failures do.
local function expect_true(cond, label, detail)
  if cond then
    checks.expect_match("ok", "ok", label)
  else
    local msg = label
    if detail ~= nil then
      msg = msg .. " (" .. tostring(detail) .. ")"
    end
    checks.expect_match("no", "yes", msg)
  end
end

local function parse_bench(out)
  local op, iters, warmup = out:match("benchlab: op=(%a+) iters=(%d+) warmup=(%d+)")
  local n, min_ns, median_ns, p99_ns, max_ns = out:match(
    "benchlab: n=(%d+) min_ns=(%d+) median_ns=(%d+) p99_ns=(%d+) max_ns=(%d+)")
  local co_p99_ns, expected_interval_ns, co_n = out:match(
    "benchlab: co_p99_ns=(%d+) expected_interval_ns=(%d+) co_n=(%d+)")
  local checksum = out:match("benchlab: checksum=(%x+)")
  return {
    op = op,
    iters = tonumber(iters), warmup = tonumber(warmup),
    n = tonumber(n), min_ns = tonumber(min_ns), median_ns = tonumber(median_ns),
    p99_ns = tonumber(p99_ns), max_ns = tonumber(max_ns),
    co_p99_ns = tonumber(co_p99_ns), expected_interval_ns = tonumber(expected_interval_ns),
    co_n = tonumber(co_n),
    checksum = checksum,
  }
end

local ITERS = 20000
local WARMUP = 1000

-- ---------------------------------------------------------------------------
-- 1 & 2. Each op: shape, monotonicity, and repeatable checksum across two runs.
-- ---------------------------------------------------------------------------

for _, op in ipairs({ "encode", "decode", "roundtrip" }) do
  local cmd = demo .. " --op " .. op .. " --iters " .. ITERS .. " --warmup " .. WARMUP
  local tag = lang .. " " .. op

  local r1 = checks.run(cmd)
  checks.expect_exit(r1, 0, tag .. ": exits 0")
  checks.expect_match(r1.out,
    "benchlab: op=" .. op .. " iters=" .. ITERS .. " warmup=" .. WARMUP,
    tag .. ": header line echoes op/iters/warmup")
  checks.expect_match(r1.out, "benchlab: checksum=%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x",
    tag .. ": prints a 64-bit hex checksum (proves the codec actually ran)")

  local f1 = parse_bench(r1.out)
  expect_true(f1.n == ITERS, tag .. ": n equals the requested iters", f1.n)
  expect_true(f1.min_ns ~= nil and f1.median_ns ~= nil and f1.p99_ns ~= nil and f1.max_ns ~= nil,
    tag .. ": min/median/p99/max all parse as numbers")
  if f1.min_ns then
    expect_true(f1.min_ns <= f1.median_ns and f1.median_ns <= f1.p99_ns and f1.p99_ns <= f1.max_ns,
      tag .. ": percentiles are monotonic (min <= median <= p99 <= max)",
      string.format("min=%d median=%d p99=%d max=%d", f1.min_ns, f1.median_ns, f1.p99_ns, f1.max_ns))
    expect_true(f1.co_p99_ns >= f1.p99_ns,
      tag .. ": coordinated-omission-corrected p99 is never below the raw p99",
      string.format("co_p99=%d p99=%d", f1.co_p99_ns, f1.p99_ns))
    expect_true(f1.co_n >= f1.n,
      tag .. ": the CO-corrected sample count is at least the raw sample count",
      string.format("co_n=%d n=%d", f1.co_n, f1.n))
    local want_interval = f1.median_ns > 0 and f1.median_ns or 1
    expect_true(f1.expected_interval_ns == want_interval,
      tag .. ": expected_interval_ns matches the documented derivation (raw median, floor 1)",
      string.format("expected_interval_ns=%d median_ns=%d", f1.expected_interval_ns, f1.median_ns))
  end

  -- Repeatable-order: a second run of the identical fixed workload must
  -- produce the identical checksum (deterministic codec output), even though
  -- the ns timings themselves are expected to vary run to run.
  local r2 = checks.run(cmd)
  checks.expect_exit(r2, 0, tag .. ": second run also exits 0")
  local f2 = parse_bench(r2.out)
  expect_true(f1.checksum ~= nil and f1.checksum == f2.checksum,
    tag .. ": checksum is identical across two runs of the same fixed workload",
    tostring(f1.checksum) .. " vs " .. tostring(f2.checksum))
end

-- ---------------------------------------------------------------------------
-- 3. --lie: runs, exits 0, and — the entire point — never emits a percentile
--    table, only a single untrusted wall-clock sample.
-- ---------------------------------------------------------------------------

local lie = checks.run(demo .. " --lie --op roundtrip")
checks.expect_exit(lie, 0, lang .. ": --lie exits 0")
checks.expect_match(lie.out, "benchlab: lie op=roundtrip",
  lang .. ": --lie header names itself as the lie")
checks.expect_match(lie.out, "benchlab: lie elapsed_ns=%d+ sink=%d+",
  lang .. ": --lie reports exactly one elapsed_ns sample")
expect_true(not lie.out:match("p99_ns="),
  lang .. ": --lie output has no percentile table at all")
expect_true(not lie.out:match("median_ns="),
  lang .. ": --lie output has no median either — one sample, no warmup, no variance")

-- ---------------------------------------------------------------------------
-- 4. Error shapes: usage, exit 2.
-- ---------------------------------------------------------------------------

local noargs = checks.run(demo)
checks.expect_exit(noargs, 2, lang .. ": no flags exits 2")
checks.expect_match(noargs.out, "usage: benchlab", lang .. ": usage line is printed")

local badop = checks.run(demo .. " --op bogus --iters 1000")
checks.expect_exit(badop, 2, lang .. ": unknown --op exits 2")

local badrange = checks.run(demo .. " --op encode --iters 100 --warmup 500")
checks.expect_exit(badrange, 2, lang .. ": --warmup >= --iters exits 2")

checks.finish()
