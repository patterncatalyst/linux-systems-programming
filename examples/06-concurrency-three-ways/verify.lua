-- Verify parhash for LSP_LANG: fixed fixture -> byte-identical output across
-- all three languages, wrong usage -> exit 2, and SIGINT on a big tree ->
-- partial results + "parhash: interrupted" + exit 130.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

-- Scratch area (fixture, big tree, captured streams); removed at the end.
local tmp = checks.run("mktemp -d").out:gsub("%s+$", "")
if tmp == "" or not tmp:match("^/") then
  checks.skip("mktemp -d failed")
end
local function cleanup()
  os.execute("rm -rf '" .. tmp .. "'")
end

-- ---------------------------------------------------------------------------
-- 1. Known fixture: 5 files (nested dirs, binary, empty) with precomputed
--    FNV-1a 64 sums. The output must be byte-identical for every language.
-- ---------------------------------------------------------------------------
local fix = tmp .. "/fixture"
checks.run(table.concat({
  "mkdir -p " .. fix .. "/sub/deep",
  "printf 'alpha\\n'   > " .. fix .. "/a.txt",
  "printf 'bravo\\n'   > " .. fix .. "/b.txt",
  "printf 'charlie\\n' > " .. fix .. "/sub/c.txt",
  "head -c 1024 /dev/zero > " .. fix .. "/sub/deep/d.bin",
  ": > " .. fix .. "/zz-empty.dat",
}, " && "))

local expected_file = tmp .. "/expected.txt"
do
  local f = assert(io.open(expected_file, "w"))
  f:write("bbd23ea491ed9813  a.txt\n")
  f:write("4d038ef62a7d9c0b  b.txt\n")
  f:write("f839d20567c0a911  sub/c.txt\n")
  f:write("51d88627df287325  sub/deep/d.bin\n")
  f:write("cbf29ce484222325  zz-empty.dat\n")
  f:write("parhash: 5 files, 4 workers\n")
  f:close()
end

local actual = tmp .. "/actual.txt"
local r = checks.run(demo .. " " .. fix .. " > " .. actual)
checks.expect_exit(r, 0, lang .. ": fixture run exits 0")

local d = checks.run("diff -u " .. expected_file .. " " .. actual)
checks.expect_exit(d, 0, lang .. ": stdout is byte-identical to the expected 5 sorted sums + summary")
if d.exit ~= 0 then
  print(d.out) -- show the diff on failure
end

-- ---------------------------------------------------------------------------
-- 2. CLI contract: no argument -> usage on stderr, exit 2.
-- ---------------------------------------------------------------------------
local u = checks.run(demo)
checks.expect_exit(u, 2, lang .. ": missing DIR exits 2")
checks.expect_match(u.out, "usage: parhash DIR", lang .. ": missing DIR prints usage")

-- ---------------------------------------------------------------------------
-- 3. SIGINT drain: a big sparse tree (16 small + 96 x 64 MiB files, ~6 GiB
--    logical, ~0 on disk) takes seconds to hash; INT at 100 ms must yield
--    only completed hash lines, "parhash: interrupted" on stderr, exit 130.
-- ---------------------------------------------------------------------------
local big = tmp .. "/big"
checks.run(table.concat({
  "mkdir -p " .. big,
  "for i in $(seq -w 1 16); do printf 'small %s\\n' \"$i\" > " .. big .. "/aa-$i.txt; done",
  "for i in $(seq -w 1 96); do truncate -s 64M " .. big .. "/big-$i.bin; done",
}, " && "))

local int_out = tmp .. "/int-out.txt"
local int_err = tmp .. "/int-err.txt"
local i = checks.run(demo .. " " .. big .. " > " .. int_out .. " 2> " .. int_err ..
                     " & pid=$!; sleep 0.1; kill -INT $pid; wait $pid")
checks.expect_exit(i, 130, lang .. ": SIGINT exits 130")

local err_text = checks.run("cat " .. int_err).out
checks.expect_match(err_text, "parhash: interrupted", lang .. ": SIGINT prints 'parhash: interrupted' on stderr")

-- Partial stdout: every line is a completed "<16 hex>  <path>" entry and the
-- normal-completion summary never appears.
local bad_lines = checks.run("grep -cEv '^[0-9a-f]{16}  ' " .. int_out .. " || true").out
checks.expect_match(bad_lines, "^0%s*$", lang .. ": interrupted stdout holds only hash lines (no summary)")
local summary = checks.run("grep -c 'files, 4 workers' " .. int_out .. " || true").out
checks.expect_match(summary, "^0%s*$", lang .. ": interrupted run prints no summary line")

cleanup()
checks.finish()
