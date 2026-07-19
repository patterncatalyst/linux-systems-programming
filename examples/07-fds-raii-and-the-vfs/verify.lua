-- Verify fwatch v0: build a fixture tree with pinned mtimes, assert the exact
-- snapshot lines, mutate the tree (add/remove/append+retouch), and assert the
-- exact diff lines and summary. Also checks usage and error-path behavior.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run "

-- Make sure the binary exists so `run` output is only the program's output.
local build = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(build, 0, lang .. ": builds")

-- Fixture: four regular files across nested directories, mtimes pinned with
-- touch -d @EPOCH so the snapshot bytes are fully deterministic.
local tmp = checks.run("mktemp -d").out:gsub("%s+$", "")
if tmp == "" then checks.skip("mktemp failed") end
local tree = tmp .. "/tree"
local snap = tmp .. "/snap.txt"

local setup = checks.run(table.concat({
  "mkdir -p '" .. tree .. "/sub/deep'",
  "printf hello > '" .. tree .. "/a.txt'",              -- 5 bytes
  "printf bye > '" .. tree .. "/gone.txt'",             -- 3 bytes
  "printf worldwide > '" .. tree .. "/sub/b.txt'",      -- 9 bytes
  "printf abc > '" .. tree .. "/sub/deep/c.bin'",       -- 3 bytes
  "touch -d @1000000000 '" .. tree .. "/a.txt'",
  "touch -d @1000000300 '" .. tree .. "/gone.txt'",
  "touch -d @1000000100 '" .. tree .. "/sub/b.txt'",
  "touch -d @1000000200 '" .. tree .. "/sub/deep/c.bin'",
}, " && "))
checks.expect_exit(setup, 0, lang .. ": fixture tree created")

-- snapshot: exact sorted "<relpath> <size> <mtime>" lines.
local r = checks.run(demo .. "snapshot '" .. tree .. "' > '" .. snap .. "'")
checks.expect_exit(r, 0, lang .. ": snapshot exits 0")
local written = checks.run("cat '" .. snap .. "'")
checks.expect_match(written.out,
  "^a%.txt 5 1000000000\n" ..
  "gone%.txt 3 1000000300\n" ..
  "sub/b%.txt 9 1000000100\n" ..
  "sub/deep/c%.bin 3 1000000200\n$",
  lang .. ": snapshot is exactly the four sorted entry lines")

-- diff against an unchanged tree: nothing but the zero summary.
r = checks.run(demo .. "diff '" .. tree .. "' '" .. snap .. "'")
checks.expect_exit(r, 0, lang .. ": no-change diff exits 0")
checks.expect_match(r.out, "^fwatch: 0 added, 0 removed, 0 modified\n$",
  lang .. ": no-change diff prints only the zero summary")

-- Mutate: create new.txt, delete gone.txt, grow a.txt and move its mtime.
local mutate = checks.run(table.concat({
  "printf newfile > '" .. tree .. "/new.txt'",
  "rm '" .. tree .. "/gone.txt'",
  "printf ' again' >> '" .. tree .. "/a.txt'",          -- 5 -> 11 bytes
  "touch -d @1000000500 '" .. tree .. "/a.txt'",
}, " && "))
checks.expect_exit(mutate, 0, lang .. ": tree mutated")

-- diff: exactly one ~, one -, one + (in sorted path order) plus the summary.
r = checks.run(demo .. "diff '" .. tree .. "' '" .. snap .. "'")
checks.expect_exit(r, 0, lang .. ": diff exits 0")
checks.expect_match(r.out,
  "^~ a%.txt\n" ..
  "%- gone%.txt\n" ..
  "%+ new%.txt\n" ..
  "fwatch: 1 added, 1 removed, 1 modified\n$",
  lang .. ": diff shows ~/-/+ lines and the 1/1/1 summary, nothing else")

-- The modified file keeps its identity in a fresh snapshot (size 11, new mtime).
r = checks.run(demo .. "snapshot '" .. tree .. "'")
checks.expect_exit(r, 0, lang .. ": re-snapshot exits 0")
checks.expect_match(r.out, "^a%.txt 11 1000000500\n",
  lang .. ": re-snapshot sees the new size and mtime")

-- Error paths: missing directory (errno 2), missing snapshot file, bad usage.
local tmp_pat = tmp:gsub("%-", "%%-"):gsub("%.", "%%.")
r = checks.run(demo .. "snapshot '" .. tmp .. "/nope'")
checks.expect_exit(r, 1, lang .. ": snapshot of missing dir exits 1")
checks.expect_match(r.out,
  "^fwatch: error: cannot open directory '" .. tmp_pat .. "/nope' %(errno 2%)\n$",
  lang .. ": missing dir reports errno 2 on stderr")

r = checks.run(demo .. "diff '" .. tree .. "' '" .. tmp .. "/nosnap'")
checks.expect_exit(r, 1, lang .. ": diff with missing snapshot exits 1")
checks.expect_match(r.out, "cannot read snapshot '" .. tmp_pat .. "/nosnap' %(errno 2%)",
  lang .. ": missing snapshot reports errno 2")

r = checks.run(demo .. "snapshot")
checks.expect_exit(r, 2, lang .. ": missing args exit 2")
checks.expect_match(r.out, "usage: fwatch snapshot DIR", lang .. ": usage on stderr")

checks.run("rm -rf '" .. tmp .. "'")
checks.finish()
