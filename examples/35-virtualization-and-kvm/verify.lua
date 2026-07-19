-- Verify overheadbench (chapter 35: virtualization and KVM). This example
-- itself is host-local: it asserts the three microbenchmarks actually run
-- on THIS host and emit parseable, sane-range numeric rows.
--
--   * "--bench all" (the default) prints exactly the three rows, in order
--     syscall/mem/io, each "bench=<name> metric=<v> unit=<u>" with <v>
--     parsing as a finite non-negative number and <u> the expected unit.
--   * Each metric lands in a generous but real sanity window — wide enough
--     to tolerate a loaded CI host, narrow enough to catch "printed zero"
--     or "printed a bogus huge/negative number" bugs.
--   * Selecting a single bench via --bench prints only that bench's row.
--   * --iters overrides the loop count for whichever bench(es) run, and a
--     small explicit value still produces a sane row (guards against a
--     divide-by-zero / elapsed==0 edge case at low iteration counts).
--   * Bad usage (unknown --bench value, missing flag value, unknown flag,
--     non-numeric --iters) exits 2 and prints the usage line — never a
--     silent wrong answer.
--
-- The chapter separately deploys this same binary to a lab VM
-- (systems-target) and into a rootless podman container on the host, and
-- tabulates host vs VM vs container numbers for the overhead discussion;
-- that comparison is authored by hand, not asserted here.
--
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
  if cond then
    checks.expect_match("yes", "yes", label)
  else
    checks.expect_match("no", "yes", label)
  end
end

-- Sane ranges: wide enough for a loaded CI host or older hardware, narrow
-- enough to catch a broken/zero/negative measurement.
local RANGES = {
  syscall = { unit = "ns/call", lo = 1,    hi = 5e6 },   -- 1ns .. 5ms/call
  mem     = { unit = "GB/s",    lo = 0.01, hi = 1000 },  -- 10MB/s .. 1TB/s
  io      = { unit = "ops/s",   lo = 0.1,  hi = 5e6 },   -- down to one op per 10s
}

local function check_row(out, name, label_prefix)
  local pattern = "bench=" .. name .. " metric=(%-?%d+%.%d+) unit=(%S+)"
  local metric_s, unit = out:match(pattern)
  expect_true(metric_s ~= nil, label_prefix .. ": " .. name .. " row parses (bench=" .. name .. " metric=<num> unit=<u>)")
  if metric_s == nil then return end
  local metric = tonumber(metric_s)
  local r = RANGES[name]
  expect_true(unit == r.unit, label_prefix .. ": " .. name .. " unit is " .. r.unit .. " (got " .. tostring(unit) .. ")")
  expect_true(metric ~= nil and metric >= r.lo and metric <= r.hi,
    label_prefix .. ": " .. name .. " metric " .. tostring(metric_s) .. " " .. tostring(unit) ..
    " is in sane range [" .. r.lo .. ", " .. r.hi .. "]")
end

-- ---------------------------------------------------------------------------
-- 1. Built binary exists (the orchestrator builds before verifying).
-- ---------------------------------------------------------------------------
local r = checks.run("test -x " .. bin)
checks.expect_exit(r, 0, lang .. ": built binary " .. bin .. " exists")

-- ---------------------------------------------------------------------------
-- 2. Default run (--bench all): three rows, syscall/mem/io, in order, each
--    parseable and in a sane range.
-- ---------------------------------------------------------------------------
r = checks.run("./demo.sh " .. lang .. " run")
checks.expect_exit(r, 0, lang .. ": default run (--bench all) exits 0")

local si = r.out:find("bench=syscall")
local mi = r.out:find("bench=mem")
local ii = r.out:find("bench=io")
expect_true(si ~= nil and mi ~= nil and ii ~= nil, lang .. ": all three bench rows are present")
expect_true(si ~= nil and mi ~= nil and ii ~= nil and si < mi and mi < ii,
  lang .. ": rows print in order syscall, mem, io")

check_row(r.out, "syscall", lang)
check_row(r.out, "mem", lang)
check_row(r.out, "io", lang)

-- ---------------------------------------------------------------------------
-- 3. Selecting a single bench prints only that row.
-- ---------------------------------------------------------------------------
for _, name in ipairs({ "syscall", "mem", "io" }) do
  r = checks.run("./demo.sh " .. lang .. " run --bench " .. name)
  checks.expect_exit(r, 0, lang .. ": --bench " .. name .. " exits 0")
  check_row(r.out, name, lang)
  for _, other in ipairs({ "syscall", "mem", "io" }) do
    if other ~= name then
      expect_true(not r.out:find("bench=" .. other), lang .. ": --bench " .. name .. " does not print the " .. other .. " row")
    end
  end
end

-- ---------------------------------------------------------------------------
-- 4. --iters overrides the loop count; a small explicit value still yields
--    a sane, parseable row (no divide-by-zero at low iteration counts).
-- ---------------------------------------------------------------------------
r = checks.run("./demo.sh " .. lang .. " run --bench syscall --iters 500")
checks.expect_exit(r, 0, lang .. ": --bench syscall --iters 500 exits 0")
check_row(r.out, "syscall", lang)

r = checks.run("./demo.sh " .. lang .. " run --bench io --iters 10")
checks.expect_exit(r, 0, lang .. ": --bench io --iters 10 exits 0")
check_row(r.out, "io", lang)

-- ---------------------------------------------------------------------------
-- 5. Bad usage: unknown --bench value, missing flag value, unknown flag,
--    non-numeric --iters all exit 2 with the usage line -- never a silent
--    wrong answer.
-- ---------------------------------------------------------------------------
r = checks.run("./demo.sh " .. lang .. " run --bench bogus")
checks.expect_exit(r, 2, lang .. ": unknown --bench value exits 2")
checks.expect_match(r.out, "usage: overheadbench", lang .. ": unknown --bench value prints usage")

r = checks.run("./demo.sh " .. lang .. " run --bench")
checks.expect_exit(r, 2, lang .. ": --bench with no value exits 2")
checks.expect_match(r.out, "usage: overheadbench", lang .. ": --bench with no value prints usage")

r = checks.run("./demo.sh " .. lang .. " run --iters abc")
checks.expect_exit(r, 2, lang .. ": non-numeric --iters exits 2")
checks.expect_match(r.out, "usage: overheadbench", lang .. ": non-numeric --iters prints usage")

r = checks.run("./demo.sh " .. lang .. " run --iters 0")
checks.expect_exit(r, 2, lang .. ": --iters 0 exits 2")

r = checks.run("./demo.sh " .. lang .. " run --nonsense")
checks.expect_exit(r, 2, lang .. ": unknown flag exits 2")
checks.expect_match(r.out, "usage: overheadbench", lang .. ": unknown flag prints usage")

checks.finish()
