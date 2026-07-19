-- Verify fwatch v1 (09-event-loops-epoll-inotify) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. v0 regression — snapshot format and diff created/modified/deleted lines
--   2. watch: background run, mutate the directory after ~400 ms, expect one
--      debounced event line per path and a clean "(timeout)" exit, exit 0
--   3. watch: SIGTERM produces the "(signal)" exit line, exit 0
--   4. usage errors exit 2; watching a missing directory exits 1
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set; the interpreter may be lua or luajit.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

-- Scratch area for watched dirs and snapshot files.
local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

local function sh(script)
  return checks.run(script:gsub("@WD@", wd))
end

-- ---------------------------------------------------------------------------
-- 1. v0 regression: snapshot + diff
-- ---------------------------------------------------------------------------

local snap = sh([[
set -e
mkdir @WD@/w
printf 'one\n'  > @WD@/w/a.txt
printf 'two\n'  > @WD@/w/b.txt
]] .. demo .. [[ snapshot @WD@/w | tee @WD@/s1.txt
]])
checks.expect_exit(snap, 0, lang .. ": snapshot exits 0")
checks.expect_match(snap.out, "a%.txt\t4\t%d+", lang .. ": snapshot lists a.txt with size 4 and mtime_ns")
checks.expect_match(snap.out, "b%.txt\t4\t%d+", lang .. ": snapshot lists b.txt")

local diff = sh([[
set -e
sleep 0.05
printf 'more\n' >> @WD@/w/a.txt
rm @WD@/w/b.txt
printf 'new\n'  > @WD@/w/c.txt
]] .. demo .. [[ snapshot @WD@/w > @WD@/s2.txt
]] .. demo .. [[ diff @WD@/s1.txt @WD@/s2.txt
]])
checks.expect_exit(diff, 0, lang .. ": diff exits 0")
checks.expect_match(diff.out, "modified a%.txt\ndeleted b%.txt\ncreated c%.txt",
  lang .. ": diff reports modified/deleted/created in name order")

-- ---------------------------------------------------------------------------
-- 2. watch: debounced events, then a clean timeout exit
-- ---------------------------------------------------------------------------

local watch = sh([[
set -e
]] .. demo .. [[ watch @WD@/w --timeout-ms 2000 > @WD@/watch.out 2> @WD@/watch.err &
pid=$!
sleep 0.4
printf 'hello\n'  > @WD@/w/n1.txt
printf 'again\n' >> @WD@/w/a.txt
rm @WD@/w/c.txt
wait $pid
cat @WD@/watch.err @WD@/watch.out
]])
checks.expect_exit(watch, 0, lang .. ": watch exits 0 after timeout")
checks.expect_match(watch.out, "fwatch: watching ", lang .. ": watch announces the directory on stderr")
checks.expect_match(watch.out, "event: created n1%.txt", lang .. ": created event for new file")
checks.expect_match(watch.out, "event: modified a%.txt", lang .. ": modified event for appended file")
checks.expect_match(watch.out, "event: deleted c%.txt", lang .. ": deleted event for removed file")
checks.expect_match(watch.out, "fwatch: exiting %(timeout%)", lang .. ": clean timeout exit line")

-- Debounce check: creating n1.txt fires IN_CREATE + IN_MODIFY back to back,
-- but the 100 ms window must coalesce them into exactly ONE event line.
local _, n1_lines = watch.out:gsub("event: %a+ n1%.txt", "")
checks.expect_match("n1-event-lines=" .. n1_lines, "^n1%-event%-lines=1$",
  lang .. ": create+write within 100 ms debounce to one event line")

-- ---------------------------------------------------------------------------
-- 3. watch: SIGTERM -> "(signal)" exit line, still exit 0
-- ---------------------------------------------------------------------------

local sig = sh([[
set -e
]] .. demo .. [[ watch @WD@/w --timeout-ms 10000 > @WD@/sig.out 2>/dev/null &
pid=$!
sleep 0.5
kill -TERM $pid
wait $pid
cat @WD@/sig.out
]])
checks.expect_exit(sig, 0, lang .. ": watch exits 0 on SIGTERM")
checks.expect_match(sig.out, "fwatch: exiting %(signal%)", lang .. ": signal exit line")

-- ---------------------------------------------------------------------------
-- 4. error shapes
-- ---------------------------------------------------------------------------

local bogus = checks.run(demo .. " bogus")
checks.expect_exit(bogus, 2, lang .. ": unknown subcommand exits 2")
checks.expect_match(bogus.out, "usage: fwatch", lang .. ": unknown subcommand prints usage")

local missing = sh(demo .. " watch @WD@/no-such-dir")
checks.expect_exit(missing, 1, lang .. ": watching a missing directory exits 1")

checks.run("rm -rf " .. wd)
checks.finish()
