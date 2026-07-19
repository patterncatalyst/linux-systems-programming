-- Verify shmkv v1 (20-shared-memory-coordination) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. usage errors exit 2; a missing file exits 1; a non-SHKV2 file exits 1
--   2. serve + watch as two REAL processes (shell background jobs): while
--      both run, exactly one "/app serve" and one "/app watch" process
--      exists; every one of the N updates is observed with a seqlock-
--      consistent key=value pair; both exit 0
--   3. file contents after serve: SHKV2 magic, futex word and u64 update
--      counter equal to N, last value string present in a slot
--   4. bench prints one parseable row per channel (futex, mq, poll) with
--      integer microsecond percentiles; futex p50 beats poll p50, and poll
--      p50 >= 500 us (the 1 ms sleep-poll pays for itself by construction)
--   5. no POSIX message queue name is left behind in /dev/mqueue
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

local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

local function sh(script)
  return checks.run(script:gsub("@WD@", wd))
end

-- expect_true: route a computed boolean through expect_exit so it lands in
-- the same pass/fail accounting as everything else.
local function expect_true(cond, label)
  checks.expect_exit({ exit = cond and 0 or 1 }, 0, label)
end

-- ---------------------------------------------------------------------------
-- 1. error contract
-- ---------------------------------------------------------------------------

local noargs = sh(demo)
checks.expect_exit(noargs, 2, lang .. ": no arguments exits 2")
checks.expect_match(noargs.out, "usage: shmkv serve FILE", lang .. ": usage names serve")
checks.expect_match(noargs.out, "bench FILE %[%-%-rounds N%] %[%-%-channel futex|mq|poll%]",
  lang .. ": usage names the bench channels")

local badchan = sh(demo .. " bench @WD@/x.shm --rounds 10 --channel pigeon")
checks.expect_exit(badchan, 2, lang .. ": unknown bench channel exits 2")

local missing = sh(demo .. " watch @WD@/missing.shm --events 1")
checks.expect_exit(missing, 1, lang .. ": watch on missing file exits 1")
checks.expect_match(missing.out, "shmkv: ", lang .. ": missing-file error has shmkv prefix")
checks.expect_match(missing.out, "[Nn]o such file", lang .. ": missing-file error names ENOENT")

local badmagic = sh([[
printf 'garbage' > @WD@/bad.shm
]] .. demo .. [[ watch @WD@/bad.shm --events 1
]])
checks.expect_exit(badmagic, 1, lang .. ": watch on non-SHKV2 file exits 1")
checks.expect_match(badmagic.out, "bad magic %(want SHKV2%)", lang .. ": bad-magic error names SHKV2")

-- ---------------------------------------------------------------------------
-- 2. serve + watch across two real processes
-- ---------------------------------------------------------------------------

local updates = 6
local sw = sh([[
]] .. demo .. [[ serve @WD@/kv.shm --updates ]] .. updates .. [[ --interval-ms 80 \
    > @WD@/serve.out 2>&1 &
spid=$!
# Attach the watcher only once the header is initialized (magic in place).
for i in $(seq 1 100); do
  [ "$(head -c 5 @WD@/kv.shm 2>/dev/null)" = "SHKV2" ] && break
  sleep 0.02
done
]] .. demo .. [[ watch @WD@/kv.shm --events ]] .. updates .. [[ \
    > @WD@/watch.out 2>&1 &
wpid=$!
sleep 0.25
# Bracket trick so this shell's own command line never matches the pattern.
sprocs=$(pgrep -c -f "/ap[p] serve" || true)
wprocs=$(pgrep -c -f "/ap[p] watch" || true)
s_rc=0; wait $spid || s_rc=$?
w_rc=0; wait $wpid || w_rc=$?
echo "sprocs=$sprocs wprocs=$wprocs s_rc=$s_rc w_rc=$w_rc"
echo "--- serve ---"; cat @WD@/serve.out
echo "--- watch ---"; cat @WD@/watch.out
]])
checks.expect_exit(sw, 0, lang .. ": serve+watch harness exits 0")
checks.expect_match(sw.out, "sprocs=1 wprocs=1", lang .. ": one serve and one watch process alive mid-run")
checks.expect_match(sw.out, "s_rc=0 w_rc=0", lang .. ": both processes exit 0")
checks.expect_match(sw.out, "serve: file=" .. wd .. "/kv%.shm updates=6 interval_ms=80",
  lang .. ": serve announces its parameters")
checks.expect_match(sw.out, "serve: complete updates=6", lang .. ": serve completion line")
checks.expect_match(sw.out, "watch: complete events=6", lang .. ": watch completion line")
for k = 1, updates do
  checks.expect_match(sw.out, "published update " .. k .. ": k" .. k .. "=value%-" .. k,
    lang .. ": serve published update " .. k)
  checks.expect_match(sw.out, "observed update " .. k .. ": k" .. k .. "=value%-" .. k,
    lang .. ": watch observed update " .. k .. " with consistent value")
end

-- ---------------------------------------------------------------------------
-- 3. file contents after serve
-- ---------------------------------------------------------------------------

local file = sh([[
echo "magic=$(head -c 5 @WD@/kv.shm)"
echo "futexword=$(od -An -t u4 -j 12 -N 4 @WD@/kv.shm | tr -d ' ')"
echo "counter=$(od -An -t u8 -j 16 -N 8 @WD@/kv.shm | tr -d ' ')"
echo "lastvalue=$(strings @WD@/kv.shm | grep -c '^value-6$')"
echo "size=$(stat -c %s @WD@/kv.shm)"
]])
checks.expect_exit(file, 0, lang .. ": file inspection exits 0")
checks.expect_match(file.out, "magic=SHKV2", lang .. ": file carries SHKV2 magic")
checks.expect_match(file.out, "futexword=6\n", lang .. ": u32 futex word settled at 6")
checks.expect_match(file.out, "counter=6\n", lang .. ": u64 update counter settled at 6")
checks.expect_match(file.out, "lastvalue=1", lang .. ": a slot holds the final value string")
checks.expect_match(file.out, "size=4096", lang .. ": file is exactly one page")

-- ---------------------------------------------------------------------------
-- 4. bench: three channels, parseable latencies, sane ordering
-- ---------------------------------------------------------------------------

local rounds = 60
local p50 = {}
for _, channel in ipairs({ "futex", "mq", "poll" }) do
  local r = sh(demo .. " bench @WD@/bench.shm --rounds " .. rounds .. " --channel " .. channel)
  checks.expect_exit(r, 0, lang .. ": bench " .. channel .. " exits 0")
  checks.expect_match(r.out,
    "bench: channel=" .. channel .. " rounds=" .. rounds .. " p50_us=%d+ p99_us=%d+",
    lang .. ": bench " .. channel .. " row is parseable")
  p50[channel] = tonumber(r.out:match("p50_us=(%d+)"))
  local p99 = tonumber(r.out:match("p99_us=(%d+)"))
  expect_true(p50[channel] ~= nil and p99 ~= nil and p99 >= p50[channel],
    lang .. ": bench " .. channel .. " has p99 >= p50")
end
expect_true(p50.poll ~= nil and p50.poll >= 500,
  lang .. ": poll p50 >= 500us by construction (got " .. tostring(p50.poll) .. ")")
expect_true(p50.futex ~= nil and p50.poll ~= nil and p50.futex < p50.poll,
  lang .. ": futex p50 (" .. tostring(p50.futex) .. "us) beats poll p50 (" ..
  tostring(p50.poll) .. "us)")

-- ---------------------------------------------------------------------------
-- 5. mq cleanup
-- ---------------------------------------------------------------------------

local mqleft = sh([[
echo "leftover=$(ls /dev/mqueue 2>/dev/null | grep -c shmkv-bench || true)"
]])
checks.expect_match(mqleft.out, "leftover=0", lang .. ": no shmkv-bench queue left in /dev/mqueue")

checks.run("rm -rf " .. wd)
checks.finish()
