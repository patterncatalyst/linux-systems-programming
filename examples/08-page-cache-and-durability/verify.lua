-- Verify 08-page-cache-and-durability: run iobench in buffered, fsync-every,
-- and direct modes and assert the observable behavior — report line shapes,
-- exact byte counts, the on-disk file size (stat), and exit codes. Timing
-- values are asserted for shape only (numbers flake).
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
-- Absolute path: each language's demo.sh runs the binary from its own
-- subdirectory, so a relative FILE would land there instead of here.
local example_dir = os.getenv("EXAMPLE_DIR") or "."
local out_file = example_dir .. "/verify-" .. lang .. ".bin"
os.remove(out_file)

local function expect_file_size(path, size, label)
  local r = checks.run("stat -c %s " .. path)
  checks.expect_match(r.out, "^" .. size .. "%s*$", label)
end

-- 1. No arguments: usage on stderr, exit 2.
local r = checks.run(demo)
checks.expect_exit(r, 2, lang .. ": no args exits 2")
checks.expect_match(r.out, "usage: iobench %-%-mode buffered|fsync%-every|direct",
  lang .. ": no args prints usage")

-- 2. Unknown mode: usage, exit 2, and no file gets created.
r = checks.run(demo .. " --mode sideways " .. out_file)
checks.expect_exit(r, 2, lang .. ": bad mode exits 2")
local stat = checks.run("stat -c %s " .. out_file)
checks.expect_match(tostring(stat.exit ~= 0), "true", lang .. ": bad mode creates no file")

-- 3. Buffered: 8 MiB on disk, report line + separate fsync_ms line, in order.
r = checks.run(demo .. " --mode buffered --size-mb 8 " .. out_file)
checks.expect_exit(r, 0, lang .. ": buffered exits 0")
checks.expect_match(r.out, "mode=buffered bytes=8388608 ms=%d+ MiB/s=%d+%.%d",
  lang .. ": buffered report line")
checks.expect_match(r.out, "MiB/s=%d+%.%d\nfsync_ms=%d+",
  lang .. ": fsync_ms line follows the report line")
expect_file_size(out_file, 8388608, lang .. ": buffered file is 8 MiB on disk")

-- 4. fsync-every: 4 MiB, sync every 4 blocks, single report line.
r = checks.run(demo .. " --mode fsync-every --every 4 --size-mb 4 " .. out_file)
checks.expect_exit(r, 0, lang .. ": fsync-every exits 0")
checks.expect_match(r.out, "mode=fsync%-every bytes=4194304 ms=%d+ MiB/s=%d+%.%d",
  lang .. ": fsync-every report line")
checks.expect_match(tostring(r.out:find("fsync_ms") == nil), "true",
  lang .. ": fsync-every prints no fsync_ms line")
expect_file_size(out_file, 4194304, lang .. ": fsync-every file is 4 MiB on disk")

-- 5. Direct: either O_DIRECT works here (report + size) or the filesystem
--    rejects it with the documented message and exit 4.
r = checks.run(demo .. " --mode direct --size-mb 4 " .. out_file)
if r.exit == 4 then
  checks.expect_match(r.out, "direct: unsupported on this filesystem",
    lang .. ": direct EINVAL message with exit 4")
else
  checks.expect_exit(r, 0, lang .. ": direct exits 0")
  checks.expect_match(r.out, "mode=direct bytes=4194304 ms=%d+ MiB/s=%d+%.%d",
    lang .. ": direct report line")
  expect_file_size(out_file, 4194304, lang .. ": direct file is 4 MiB on disk")
end

os.remove(out_file)
checks.finish()
