-- Verify copyx (05-errors-three-ways): behavioral checks for the error taxonomy.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.
--
-- What a passing run proves: a real copy produced a byte-identical file and
-- reported the exact byte count; a missing source exited 2 with a reason on
-- stderr; ENOSPC via /dev/full exited 3 with a reason naming the space problem.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

local m = checks.run("mktemp -d")
local tmp = m.out:gsub("%s+$", "")
if m.exit ~= 0 or tmp == "" then
  checks.skip("mktemp -d failed")
end
local function p(name) return "'" .. tmp .. "/" .. name .. "'" end

-- Seed a source whose size is not a multiple of the 64 KiB copy buffer,
-- so the loop exercises a short final read.
checks.run("head -c 200000 /dev/urandom > " .. p("src.bin"))

-- 1. Real copy: exact byte count on stdout, exit 0, dst byte-identical to src.
local r = checks.run(demo .. " " .. p("src.bin") .. " " .. p("dst.bin"))
checks.expect_exit(r, 0, lang .. ": successful copy exits 0")
checks.expect_match(r.out, "copied 200000 bytes", lang .. ": reports exact byte count")
r = checks.run("cmp " .. p("src.bin") .. " " .. p("dst.bin"))
checks.expect_exit(r, 0, lang .. ": cmp: dst is byte-identical to src")

-- 2. Empty source: still success, zero bytes, empty dst created.
checks.run(": > " .. p("empty.bin"))
r = checks.run(demo .. " " .. p("empty.bin") .. " " .. p("empty-out.bin"))
checks.expect_exit(r, 0, lang .. ": empty-file copy exits 0")
checks.expect_match(r.out, "copied 0 bytes", lang .. ": empty file reports 0 bytes")
r = checks.run("test -f " .. p("empty-out.bin") .. " && test ! -s " .. p("empty-out.bin"))
checks.expect_exit(r, 0, lang .. ": empty dst exists and is empty")

-- 3. Missing source: taxonomy exit 2, "copyx: <reason>" on stderr.
r = checks.run(demo .. " " .. p("no-such-file") .. " " .. p("x.bin"))
checks.expect_exit(r, 2, lang .. ": missing source exits 2")
checks.expect_match(r.out, "copyx: .*No such file or directory",
  lang .. ": missing source prints copyx: reason")

-- 4. Write failure (/dev/full -> ENOSPC): taxonomy exit 3, reason mentions space.
r = checks.run(demo .. " " .. p("src.bin") .. " /dev/full")
checks.expect_exit(r, 3, lang .. ": ENOSPC write exits 3")
checks.expect_match(r.out, "copyx: write /dev/full", lang .. ": names the failing op and path")
checks.expect_match(r.out, "space", lang .. ": reason mentions space")

checks.run("rm -rf '" .. tmp .. "'")
checks.finish()
