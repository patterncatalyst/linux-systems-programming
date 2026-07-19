-- Verify toolbelt: both targets (hot/alloc) produce the exact single-line
-- report with numbers that parse to sane values, bad usage exits 2, and the
-- default sizes (3,000,000 / 200,000) are unchanged. Then each language's
-- native profiler is actually run against the binary and asserted to name
-- the hot function:
--   cpp/rust: perf record -> perf report --stdio names spin_hot
--   go:       runtime/pprof CPU profile -> go tool pprof --top names
--             main.hotSpin; heap profile -> go tool pprof --top --cum
--             --alloc_space names main.allocChurn
-- plus a plain "perf stat" run on `app hot` parsing cycles/instructions,
-- which works identically for all three (perf attaches to any ELF binary).
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local bin = ({ cpp = "cpp/build/release/app", go = "go/bin/app", rust = "rust/target/release/app" })[lang]

if checks.run("test -x " .. bin).exit ~= 0 then
  checks.skip(bin .. " missing — run './demo.sh " .. lang .. " build' first")
end

if checks.run("command -v perf >/dev/null 2>&1").exit ~= 0 then
  checks.skip("perf not found on PATH")
end

local function demo(args)
  return checks.run("./demo.sh " .. lang .. " run " .. args)
end

-- Bad usage: missing/unknown mode, non-numeric --n, unknown flag -> usage on
-- stderr, exit 2. The diagnostic text and the shared "<hot|alloc> [--n N]"
-- prefix of the usage banner are identical across all three languages (Go's
-- banner has extra profiling flags appended, which the plain substring
-- match below tolerates).
local usage_pat = "usage: app <hot|alloc> %[%-%-n N%]"

local r = demo("")
checks.expect_exit(r, 2, lang .. ": missing mode exits 2")
checks.expect_match(r.out, "missing mode", lang .. ": missing mode diagnostic")
checks.expect_match(r.out, usage_pat, lang .. ": missing mode prints usage")

r = demo("bogus")
checks.expect_exit(r, 2, lang .. ": unknown mode exits 2")
checks.expect_match(r.out, "unknown mode: bogus", lang .. ": unknown mode diagnostic")

r = demo("hot --n zero")
checks.expect_exit(r, 2, lang .. ": non-numeric --n exits 2")
checks.expect_match(r.out, "not a positive integer: zero", lang .. ": non-numeric --n diagnostic")

r = demo("hot --bogus")
checks.expect_exit(r, 2, lang .. ": unknown flag exits 2")
checks.expect_match(r.out, "unknown argument: %-%-bogus", lang .. ": unknown flag diagnostic")

-- The report line, parsed strictly.
local line_pat = "^app: mode=(%a+) n=(%d+) result=(%d+) ms=(%d+)\n$"

local function check_run(args, want_mode, want_n, want_result)
  r = demo(args)
  checks.expect_exit(r, 0, lang .. ": '" .. args .. "' exits 0")
  checks.expect_match(r.out, line_pat, lang .. ": '" .. args .. "' report is the exact single line")
  local mode, n, result, ms = r.out:match(line_pat)
  if not mode then
    return
  end
  checks.expect_match(mode, "^" .. want_mode .. "$", lang .. ": '" .. args .. "' mode=" .. want_mode)
  checks.expect_match(n, "^" .. want_n .. "$", lang .. ": '" .. args .. "' echoes n=" .. want_n)
  checks.expect_match(result, "^" .. want_result .. "$", lang .. ": '" .. args .. "' result=" .. want_result)
  local ms_n = tonumber(ms)
  if ms_n and ms_n >= 0 and ms_n < 300000 then
    checks.expect_match("ok", "ok", lang .. ": '" .. args .. "' ms sane (" .. ms .. ")")
  else
    checks.expect_match(ms or "", "impossible-ms", lang .. ": '" .. args .. "' ms sane")
  end
end

-- Defaults: identical across all three languages, including the numeric
-- result — same algorithm, same input, same answer.
check_run("hot", "hot", "3000000", "216816")
check_run("alloc", "alloc", "200000", "5000")

-- Small sizes still work (n below the 1000-entry keyspace, and a tiny hot
-- count) — the arithmetic isn't hardcoded to the defaults.
check_run("hot --n 10", "hot", "10", "4")
check_run("alloc --n 7", "alloc", "7", "7")

-- perf stat on `app hot`: works identically for any ELF binary. Parses two
-- counters; values are printed for the log but not bounded (host load
-- varies), only their presence and shape are asserted.
r = checks.run("perf stat -e cycles,instructions -- ./" .. bin .. " hot --n 500000 2>&1")
checks.expect_exit(r, 0, lang .. ": perf stat run exits 0")
local cycles = r.out:match("([%d,]+)%s+cycles")
local instrs = r.out:match("([%d,]+)%s+instructions")
if cycles and tonumber((cycles:gsub(",", ""))) then
  checks.expect_match("ok", "ok", lang .. ": perf stat parses cycles (" .. cycles .. ")")
else
  checks.expect_match(r.out, "cycles:?u?%s", lang .. ": perf stat reports cycles")
end
if instrs and tonumber((instrs:gsub(",", ""))) then
  checks.expect_match("ok", "ok", lang .. ": perf stat parses instructions (" .. instrs .. ")")
else
  checks.expect_match(r.out, "instructions:?u?%s", lang .. ": perf stat reports instructions")
end

-- Per-language native profiler: run it for real, on the real binary, and
-- assert it names the hot/alloc-heavy function.
if lang == "cpp" or lang == "rust" then
  local perf_data = os.tmpname() .. "-" .. lang .. ".perf.data"
  r = checks.run(string.format(
    "perf record -o %s --call-graph fp -- ./%s hot --n 1500000", perf_data, bin))
  checks.expect_exit(r, 0, lang .. ": perf record exits 0")

  r = checks.run(string.format("perf report -i %s --stdio", perf_data))
  checks.expect_exit(r, 0, lang .. ": perf report --stdio exits 0")
  checks.expect_match(r.out, "spin_hot", lang .. ": perf report --stdio names spin_hot")
  os.remove(perf_data)
elseif lang == "go" then
  local cpu_prof = os.tmpname() .. "-go.cpu.prof"
  r = checks.run(string.format("./%s hot --n 1500000 --cpuprofile %s", bin, cpu_prof))
  checks.expect_exit(r, 0, "go: --cpuprofile run exits 0")

  r = checks.run(string.format("go tool pprof --top --nodecount=5 %s %s", bin, cpu_prof))
  checks.expect_exit(r, 0, "go: go tool pprof --top (cpu) exits 0")
  checks.expect_match(r.out, "main%.hotSpin", "go: cpu profile names main.hotSpin")
  os.remove(cpu_prof)

  local mem_prof = os.tmpname() .. "-go.mem.prof"
  r = checks.run(string.format("./%s alloc --n 200000 --memprofile %s", bin, mem_prof))
  checks.expect_exit(r, 0, "go: --memprofile run exits 0")

  r = checks.run(string.format(
    "go tool pprof --top --cum --alloc_space --nodecount=5 %s %s", bin, mem_prof))
  checks.expect_exit(r, 0, "go: go tool pprof --top --cum --alloc_space (heap) exits 0")
  checks.expect_match(r.out, "main%.allocChurn", "go: heap profile names main.allocChurn")
  os.remove(mem_prof)
end

checks.finish()
