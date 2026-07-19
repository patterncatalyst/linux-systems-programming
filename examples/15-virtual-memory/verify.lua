-- Verify 15-virtual-memory: run memmap in every mode and assert the
-- observable behavior — header shapes, the annotated /proc/self/maps
-- excerpt (region range, permissions, backing path), real VmRSS growth
-- (heap/mmap-anon must gain at least half the touched allocation), nonzero
-- minor-fault deltas, the per-step fault-walk table shape, exit codes for
-- usage/runtime errors, and that the fault-walk temp file is unlinked.
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

local demo = "./demo.sh " .. lang .. " run"
-- Absolute path: each language's demo.sh runs the binary from its own
-- subdirectory, so a relative FILE would land there instead of here.
local example_dir = os.getenv("EXAMPLE_DIR") or "."
local map_file = example_dir .. "/verify-" .. lang .. "-map.bin"
local empty_file = example_dir .. "/verify-" .. lang .. "-empty.bin"

local function num(out, pat)
  return tonumber(string.match(out, pat) or "")
end

-- Asserts on the common report tail; returns rss delta and minor faults.
local function check_report(out, mode_label, label_prefix)
  checks.expect_match(out, "memmap: maps excerpt",
    label_prefix .. ": maps excerpt header")
  checks.expect_match(out,
    "memmap:%s+%x+%-%x+ [rw%-][w%-][x%-][ps][^\n]*<%-%- target %(mode=" ..
      mode_label .. "%)",
    label_prefix .. ": annotated target region in maps excerpt")
  checks.expect_match(out, "memmap: vmrss_before=%d+KB vmrss_after=%d+KB",
    label_prefix .. ": vmrss line")
  checks.expect_match(out, "memmap: faults minor=%d+ major=%d+",
    label_prefix .. ": faults line")
  local before = num(out, "vmrss_before=(%d+)KB")
  local after = num(out, "vmrss_after=(%d+)KB")
  local minor = num(out, "faults minor=(%d+) major=")
  local delta = (before and after) and (after - before) or nil
  return delta, minor
end

-- 1. No arguments: usage on stderr, exit 2.
local r = checks.run(demo)
checks.expect_exit(r, 2, lang .. ": no args exits 2")
checks.expect_match(r.out,
  "usage: memmap %-%-mode stack|heap|mmap%-anon|mmap%-file <FILE>|fault%-walk %[%-%-mb N%]",
  lang .. ": no args prints usage")

-- 2. Unknown mode: usage, exit 2.
r = checks.run(demo .. " --mode sideways")
checks.expect_exit(r, 2, lang .. ": bad mode exits 2")

-- 3. Heap, default 64 MiB: header, labeled rw-p region, RSS grows by at
--    least half the allocation, nonzero minor fault delta.
r = checks.run(demo .. " --mode heap")
checks.expect_exit(r, 0, lang .. ": heap exits 0")
checks.expect_match(r.out, "memmap: mode=heap bytes=67108864 pages=16384",
  lang .. ": heap header line")
checks.expect_match(r.out,
  "memmap:%s+%x+%-%x+ rw%-p[^\n]*<%-%- target %(mode=heap%)",
  lang .. ": heap target region is rw-p")
local delta, minor = check_report(r.out, "heap", lang .. ": heap")
checks.expect_match(tostring(delta ~= nil and delta >= 32768), "true",
  lang .. ": heap RSS delta >= 32768 KB (got " .. tostring(delta) .. ")")
checks.expect_match(tostring(minor ~= nil and minor > 0), "true",
  lang .. ": heap minor faults > 0 (got " .. tostring(minor) .. ")")

-- 4. Stack: clamped to 4 MiB, labeled region, nonzero minor faults.
r = checks.run(demo .. " --mode stack")
checks.expect_exit(r, 0, lang .. ": stack exits 0")
checks.expect_match(r.out, "memmap: mode=stack bytes=4194304 pages=1024",
  lang .. ": stack header line (clamped to 4 MiB)")
delta, minor = check_report(r.out, "stack", lang .. ": stack")
checks.expect_match(tostring(minor ~= nil and minor > 0), "true",
  lang .. ": stack minor faults > 0 (got " .. tostring(minor) .. ")")

-- 5. mmap-anon, 32 MiB: rw-p region, RSS grows by at least half.
r = checks.run(demo .. " --mode mmap-anon --mb 32")
checks.expect_exit(r, 0, lang .. ": mmap-anon exits 0")
checks.expect_match(r.out, "memmap: mode=mmap%-anon bytes=33554432 pages=8192",
  lang .. ": mmap-anon header line")
checks.expect_match(r.out,
  "memmap:%s+%x+%-%x+ rw%-p[^\n]*<%-%- target %(mode=mmap%-anon%)",
  lang .. ": mmap-anon target region is rw-p")
delta, minor = check_report(r.out, "mmap%-anon", lang .. ": mmap-anon")
checks.expect_match(tostring(delta ~= nil and delta >= 16384), "true",
  lang .. ": mmap-anon RSS delta >= 16384 KB (got " .. tostring(delta) .. ")")
checks.expect_match(tostring(minor ~= nil and minor > 0), "true",
  lang .. ": mmap-anon minor faults > 0 (got " .. tostring(minor) .. ")")

-- 6. mmap-file on a real 2 MiB file: size from the file, r--p region whose
--    maps line names the file, nonzero minor faults.
checks.run("dd if=/dev/zero of=" .. map_file .. " bs=1M count=2 status=none")
r = checks.run(demo .. " --mode mmap-file " .. map_file)
checks.expect_exit(r, 0, lang .. ": mmap-file exits 0")
checks.expect_match(r.out, "memmap: mode=mmap%-file bytes=2097152 pages=512",
  lang .. ": mmap-file header sized from the file")
checks.expect_match(r.out,
  "memmap:%s+%x+%-%x+ r%-%-p[^\n]*verify%-" .. lang ..
    "%-map%.bin[^\n]*<%-%- target %(mode=mmap%-file%)",
  lang .. ": mmap-file target line is r--p and names the file")
delta, minor = check_report(r.out, "mmap%-file", lang .. ": mmap-file")
checks.expect_match(tostring(minor ~= nil and minor > 0), "true",
  lang .. ": mmap-file minor faults > 0 (got " .. tostring(minor) .. ")")
os.remove(map_file)

-- 7. mmap-file runtime errors: missing file and empty file both exit 1.
r = checks.run(demo .. " --mode mmap-file /nonexistent/memmap-missing.bin")
checks.expect_exit(r, 1, lang .. ": missing file exits 1")
checks.expect_match(r.out, "memmap: error:", lang .. ": missing file error line")
checks.run(": > " .. empty_file)
r = checks.run(demo .. " --mode mmap-file " .. empty_file)
checks.expect_exit(r, 1, lang .. ": empty file exits 1")
checks.expect_match(r.out, "memmap: error: [^\n]*file is empty",
  lang .. ": empty file error line")
os.remove(empty_file)

-- 8. fault-walk, 16 MiB: 8 step lines of 512 pages each with growing fault
--    counts (fault-around batches them, so per-step minors are small but
--    must be nonzero), then the summary tail, then the temp file is gone.
r = checks.run(demo .. " --mode fault-walk --mb 16")
checks.expect_exit(r, 0, lang .. ": fault-walk exits 0")
checks.expect_match(r.out, "memmap: mode=fault%-walk bytes=16777216 pages=4096",
  lang .. ": fault-walk header line")
checks.expect_match(r.out, "memmap: walk file=[^\n]*memmap%-walk%-%d+%.bin steps=8",
  lang .. ": fault-walk announces file and steps")
local step_minor_total = 0
for i = 1, 8 do
  checks.expect_match(r.out,
    "memmap: step=" .. i .. "/8 pages=512 minor=%d+ major=%d+",
    lang .. ": fault-walk step " .. i .. "/8 line")
  local m = num(r.out, "step=" .. i .. "/8 pages=512 minor=(%d+) major=")
  step_minor_total = step_minor_total + (m or 0)
end
checks.expect_match(tostring(step_minor_total >= 8), "true",
  lang .. ": fault-walk steps accumulate minor faults (got " ..
    step_minor_total .. ")")
delta, minor = check_report(r.out, "fault%-walk", lang .. ": fault-walk")
checks.expect_match(tostring(minor ~= nil and minor > 0), "true",
  lang .. ": fault-walk total minor faults > 0 (got " .. tostring(minor) .. ")")
local walk_file = string.match(r.out, "walk file=(%S+) steps=8")
if walk_file then
  local probe = checks.run("test -e " .. walk_file)
  checks.expect_match(tostring(probe.exit ~= 0), "true",
    lang .. ": fault-walk temp file was unlinked")
else
  checks.expect_match("missing", "present", lang .. ": walk file path parsed")
end

checks.finish()
