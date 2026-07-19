-- Verify pmon (11-process-lifecycle): behavioral checks for the process
-- lifecycle monitor. Invoked from the example directory with LSP_LANG
-- (cpp|go|rust) and EXAMPLE_DIR set.
--
-- What a passing run proves: pmon reports the child's REAL pid (matched
-- against $$ echoed by the child itself); exit statuses 0/1/42 are reported
-- and mirrored in pmon's own exit code; SIGTERM/SIGKILL deaths are reported
-- as "killed by signal <n> (<NAME>)" with exit 128+n; the rusage line has the
-- exact maxrss/user/sys/wall shape with plausible values (wall covers a real
-- 200 ms sleep, maxrss is nonzero, user CPU registers for a busy loop); and
-- an exec failure prints only "pmon: exec <cmd>: <reason>" on stderr with
-- exit 127 — no fate or rusage lines for a child that never ran.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"
-- The demo contract forwards args verbatim, so the binary's own CLI
-- ("run -- CMD [ARGS...]") appears after the demo.sh "run" verb.
local pmon = demo .. " run --"
-- maxrss=<kb>KB user=<s>.<ms>s sys=<s>.<ms>s wall=<ms>ms, ms always 3 digits.
local rusage_pat =
  "pmon: rusage maxrss=%d+KB user=%d+%.%d%d%ds sys=%d+%.%d%d%ds wall=%d+ms"

-- Boolean assertions routed through expect_exit so they are counted.
local function expect_true(cond, label)
  checks.expect_exit({ exit = cond and 0 or 1 }, 0, label)
end

-- 1. Clean exit: reported and mirrored, rusage line in exact shape.
local r = checks.run(pmon .. " /bin/true")
checks.expect_exit(r, 0, lang .. ": /bin/true mirrors exit 0")
checks.expect_match(r.out, "pmon: pid %d+ exited status 0",
  lang .. ": reports clean exit")
checks.expect_match(r.out, rusage_pat, lang .. ": rusage line has exact shape")

-- 2. Nonzero exit: /bin/false -> 1.
r = checks.run(pmon .. " /bin/false")
checks.expect_exit(r, 1, lang .. ": /bin/false mirrors exit 1")
checks.expect_match(r.out, "pmon: pid %d+ exited status 1",
  lang .. ": reports status 1")

-- 3. Arbitrary status is mirrored verbatim.
r = checks.run(pmon .. " sh -c 'exit 42'")
checks.expect_exit(r, 42, lang .. ": exit 42 is mirrored")
checks.expect_match(r.out, "pmon: pid %d+ exited status 42",
  lang .. ": reports status 42")

-- 4. The reported pid is the actual child pid: the child prints its own $$
--    and pmon's fate line must name the same process.
r = checks.run(pmon .. " sh -c 'echo childpid=$$'")
checks.expect_exit(r, 0, lang .. ": child echoing its pid exits 0")
local child_pid = r.out:match("childpid=(%d+)")
expect_true(child_pid ~= nil, lang .. ": child stdout passes through pmon")
if child_pid then
  checks.expect_match(r.out, "pmon: pid " .. child_pid .. " exited status 0",
    lang .. ": fate line names the real child pid")
end

-- 5. Death by SIGTERM: named, numbered, mirrored as 128+15.
r = checks.run(pmon .. " sh -c 'kill -TERM $$'")
checks.expect_exit(r, 143, lang .. ": SIGTERM death mirrors exit 143")
checks.expect_match(r.out, "pmon: pid %d+ killed by signal 15 %(TERM%)",
  lang .. ": reports killed by signal 15 (TERM)")
checks.expect_match(r.out, rusage_pat,
  lang .. ": rusage still reported for a signaled child")

-- 6. Death by SIGKILL: 128+9.
r = checks.run(pmon .. " sh -c 'kill -KILL $$'")
checks.expect_exit(r, 137, lang .. ": SIGKILL death mirrors exit 137")
checks.expect_match(r.out, "pmon: pid %d+ killed by signal 9 %(KILL%)",
  lang .. ": reports killed by signal 9 (KILL)")

-- 7. Exec failure: strerror-shaped reason on stderr, exit 127, and no
--    fate/rusage lines (there was no child run to report on).
r = checks.run(pmon .. " ./nonexistent")
checks.expect_exit(r, 127, lang .. ": exec failure exits 127")
checks.expect_match(r.out, "pmon: exec %./nonexistent: No such file or directory",
  lang .. ": exec failure names cmd and strerror reason")
expect_true(r.out:find("exited status") == nil,
  lang .. ": no fate line on exec failure")
expect_true(r.out:find("rusage") == nil,
  lang .. ": no rusage line on exec failure")

-- 8. Wall time is real: a 200 ms sleep must show up in wall=<ms>ms.
r = checks.run(pmon .. " sh -c 'sleep 0.2'")
checks.expect_exit(r, 0, lang .. ": sleep 0.2 exits 0")
local wall = tonumber(r.out:match("wall=(%d+)ms"))
expect_true(wall ~= nil and wall >= 150,
  lang .. ": wall time covers the 200 ms sleep (got " .. tostring(wall) .. "ms)")
expect_true(wall ~= nil and wall < 10000,
  lang .. ": wall time is not absurd for a 200 ms sleep")

-- 9. rusage values are the child's, and real: maxrss nonzero, and a shell
--    busy loop registers user CPU (sec*1000+ms > 20ms).
r = checks.run(pmon .. " sh -c 'i=0; while [ $i -lt 200000 ]; do i=$((i+1)); done'")
checks.expect_exit(r, 0, lang .. ": busy loop exits 0")
local rss = tonumber(r.out:match("maxrss=(%d+)KB"))
expect_true(rss ~= nil and rss > 0, lang .. ": maxrss is nonzero")
local usec, ums = r.out:match("user=(%d+)%.(%d+)s")
local user_ms = usec and (tonumber(usec) * 1000 + tonumber(ums)) or nil
expect_true(user_ms ~= nil and user_ms > 20,
  lang .. ": busy loop registers child user CPU (got " ..
  tostring(user_ms) .. "ms)")

-- 10. Bad usage: exit 2 with the usage line.
r = checks.run(demo)
checks.expect_exit(r, 2, lang .. ": missing subcommand exits 2")
checks.expect_match(r.out, "usage: pmon run %-%- CMD %[ARGS%.%.%.%]",
  lang .. ": prints usage line")

checks.finish()
