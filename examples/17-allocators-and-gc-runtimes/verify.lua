-- Verify allocbench: both variants must produce the exact report line with
-- numbers that parse to sane values, bad usage must exit 2, and the default
-- --allocs must be 200000. Go alone must additionally report gc_cycles=<n>
-- (its collector is the chapter's point; C++/Rust must NOT print it).
-- Timing/RSS ordering between variants is measured and printed for the log
-- but deliberately not asserted — it can flake on a loaded host.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local function demo(args)
  return checks.run("./demo.sh " .. lang .. " run " .. args)
end

-- Bad usage: unknown flag / bad N / bad variant -> usage on stderr, exit 2.
local r = demo("--bogus")
checks.expect_exit(r, 2, lang .. ": unknown flag exits 2")
checks.expect_match(r.out, "usage: allocbench %[%-%-allocs N%] %[%-%-variant default|arena%]",
  lang .. ": unknown flag prints usage")

r = demo("--allocs zero")
checks.expect_exit(r, 2, lang .. ": non-numeric --allocs exits 2")
checks.expect_match(r.out, "not a positive integer", lang .. ": non-numeric --allocs diagnostic")

r = demo("--variant marble")
checks.expect_exit(r, 2, lang .. ": unknown variant exits 2")
checks.expect_match(r.out, "unknown variant: marble", lang .. ": unknown variant diagnostic")

-- The report line, parsed strictly. Go carries the extra gc_cycles field;
-- C++ and Rust must not (no collector to count).
local base = "allocbench: variant=(%a+) allocs=(%d+) peak_rss=(%d+)KB ms=(%d+)"
local line_pat
if lang == "go" then
  line_pat = "^" .. base .. " gc_cycles=(%d+)\n$"
else
  line_pat = "^" .. base .. "\n$"
end

-- Run one variant and return its parsed peak_rss (KB) after asserting the
-- full line shape and that every number parses to a sane value.
local function check_variant(variant, allocs)
  local args = "--allocs " .. allocs
  if variant ~= "default" then
    args = args .. " --variant " .. variant
  end
  r = demo(args)
  checks.expect_exit(r, 0, lang .. ": " .. variant .. " run exits 0")
  checks.expect_match(r.out, line_pat, lang .. ": " .. variant .. " report is the exact single line")
  local v, n, rss, ms, gc = r.out:match(base .. (lang == "go" and " gc_cycles=(%d+)" or ""))
  if not v then
    checks.expect_match(r.out, base, lang .. ": " .. variant .. " line parses")  -- records the FAIL
    return nil
  end
  checks.expect_match(v, "^" .. variant .. "$", lang .. ": reports variant=" .. variant)
  checks.expect_match(n, "^" .. allocs .. "$", lang .. ": echoes allocs=" .. allocs)
  local rss_n, ms_n = tonumber(rss), tonumber(ms)
  if rss_n and rss_n >= 1000 then
    checks.expect_match("ok", "ok", lang .. ": " .. variant .. " peak_rss parses and is >= 1000 KB (" .. rss .. ")")
  else
    checks.expect_match(rss or "", "impossible-peak-rss", lang .. ": " .. variant .. " peak_rss sane")
  end
  if ms_n and ms_n >= 0 and ms_n < 300000 then
    checks.expect_match("ok", "ok", lang .. ": " .. variant .. " ms parses (" .. ms .. ")")
  else
    checks.expect_match(ms or "", "impossible-ms", lang .. ": " .. variant .. " ms sane")
  end
  if lang == "go" then
    if tonumber(gc) then
      checks.expect_match("ok", "ok", lang .. ": " .. variant .. " gc_cycles parses (" .. gc .. ")")
    else
      checks.expect_match(gc or "", "impossible-gc", lang .. ": " .. variant .. " gc_cycles sane")
    end
  end
  return rss_n
end

local rss_default = check_variant("default", 200000)
local rss_arena = check_variant("arena", 200000)
if rss_default and rss_arena then
  -- Informational only: ordering is workload truth, not an assertion.
  print(string.format("info: %s peak_rss default=%dKB arena=%dKB (delta %+dKB)",
    lang, rss_default, rss_arena, rss_arena - rss_default))
end

-- Defaults: bare run must use allocs=200000 and variant=default.
r = demo("")
checks.expect_exit(r, 0, lang .. ": bare run exits 0")
checks.expect_match(r.out, "^allocbench: variant=default allocs=200000 peak_rss=%d+KB ms=%d+",
  lang .. ": bare run defaults to variant=default allocs=200000")

-- A tiny run still works (fewer allocs than the 1000-key keyspace).
r = demo("--allocs 7 --variant arena")
checks.expect_exit(r, 0, lang .. ": --allocs 7 arena run exits 0")
checks.expect_match(r.out, "^allocbench: variant=arena allocs=7 peak_rss=%d+KB ms=%d+",
  lang .. ": --allocs 7 arena report line")

checks.finish()
