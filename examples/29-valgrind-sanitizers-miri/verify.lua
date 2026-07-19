-- Verify bugfarm (chapter 29: valgrind, sanitizers, and miri).
--
-- "app <leak|uaf|uninit|overflow|race>" runs one seeded defect. This isn't
-- a normal program with an observable happy-path output -- the thing to
-- observe is a DETECTING TOOL catching the defect, so most of this script
-- builds an instrumented variant and runs the tool directly against the
-- binary (not through demo.sh, which only knows the plain build).
--
-- Per language, what's actually expressible differs on purpose:
--   cpp  -- all five: valgrind (leak, uninit), ASan (uaf, overflow),
--           TSan (race).
--   go   -- leak (self-detected via runtime.NumGoroutine(), no external
--           tool watches Go goroutine leaks the way valgrind watches
--           malloc) and race (-race build, "DATA RACE", exit 66). uaf/
--           uninit/overflow are not expressible in safe Go: bounds/nil
--           checks and the GC rule them out at runtime, so those exit 64
--           with a fixed note.
--   rust -- leak only (Box::leak, a real unfreed allocation valgrind
--           reports as "definitely lost"; miri additionally confirms it
--           when available -- informational, since the pinned 1.97.1
--           toolchain does not ship miri). uaf/uninit/overflow/race are
--           rejected by the borrow checker / bounds checks at compile
--           time or by construction, so those exit 64 with a fixed note.
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR/REPO_ROOT set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local USAGE = "usage: app <leak|uaf|uninit|overflow|race>"

-- Defensive: the CI harness always runs "./demo.sh LANG build" before this
-- script, but make verify.lua correct when run standalone too.
local r = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(r, 0, lang .. ": plain build compiles")

-- Bad usage, identical wording across all three languages.
r = checks.run("./demo.sh " .. lang .. " run")
checks.expect_exit(r, 2, lang .. ": no subcommand exits 2")
checks.expect_match(r.out, USAGE, lang .. ": no subcommand prints usage")

r = checks.run("./demo.sh " .. lang .. " run bogus")
checks.expect_exit(r, 2, lang .. ": unknown bug exits 2")
checks.expect_match(r.out, "unknown bug: bogus", lang .. ": unknown bug diagnostic")

-- A "prevented" subcommand: language runtime/compiler rules the bug out,
-- so it must print the fixed note and exit 64 (sysexits EX_USAGE) rather
-- than attempt anything.
local function expect_prevented(bug, phrase)
  local pr = checks.run("./demo.sh " .. lang .. " run " .. bug)
  checks.expect_exit(pr, 64, lang .. ": " .. bug .. " prevented exits 64")
  checks.expect_match(pr.out, phrase, lang .. ": " .. bug .. " prints '" .. phrase .. "'")
end

if lang == "cpp" then
  -- C++ expresses all five defects; every one needs a tool that watches
  -- memory or threads, not a plain run, to produce its finding.

  -- ASan+UBSan and TSan builds aren't part of the plain demo.sh contract
  -- (build/run/TARGET deploy) -- build them directly via the CMake
  -- presets that ship alongside the release preset.
  r = checks.run("cd cpp && cmake --preset asan >/dev/null && cmake --build --preset asan >/dev/null")
  checks.expect_exit(r, 0, "cpp: ASan+UBSan build configures and compiles")

  r = checks.run("cd cpp && cmake --preset tsan >/dev/null && cmake --build --preset tsan >/dev/null")
  checks.expect_exit(r, 0, "cpp: TSan build configures and compiles")

  -- leak -> valgrind memcheck, "definitely lost".
  r = checks.run("valgrind --leak-check=full --error-exitcode=1 ./cpp/build/release/app leak")
  checks.expect_exit(r, 1, "cpp: valgrind flags the leak (nonzero exit)")
  checks.expect_match(r.out, "definitely lost", "cpp: valgrind report says 'definitely lost'")

  -- uninit -> valgrind memcheck, "uninitialised value(s)".
  r = checks.run("valgrind --error-exitcode=1 ./cpp/build/release/app uninit")
  checks.expect_exit(r, 1, "cpp: valgrind flags the uninitialised read (nonzero exit)")
  checks.expect_match(r.out, "ninitialised value", "cpp: valgrind report cites an uninitialised value")

  -- uaf -> ASan, "heap-use-after-free".
  r = checks.run("./cpp/build/asan/app uaf")
  checks.expect_exit(r, 1, "cpp: ASan build exits nonzero on uaf")
  checks.expect_match(r.out, "heap%-use%-after%-free", "cpp: ASan report says 'heap-use-after-free'")

  -- overflow -> ASan, "heap-buffer-overflow".
  r = checks.run("./cpp/build/asan/app overflow")
  checks.expect_exit(r, 1, "cpp: ASan build exits nonzero on overflow")
  checks.expect_match(r.out, "heap%-buffer%-overflow", "cpp: ASan report says 'heap-buffer-overflow'")

  -- race -> TSan, "data race", exit 66 (TSan's default halt_on_error exitcode).
  r = checks.run("./cpp/build/tsan/app race")
  checks.expect_exit(r, 66, "cpp: TSan build exits 66 on race")
  checks.expect_match(r.out, "[Dd]ata race", "cpp: TSan report says 'data race'")

elseif lang == "go" then
  -- leak: self-detected. There is no valgrind-for-goroutines here; the
  -- program itself compares runtime.NumGoroutine() before/after and
  -- reports the delta, which is the idiomatic way this gets noticed.
  r = checks.run("./demo.sh go run leak")
  checks.expect_exit(r, 0, "go: leak run exits 0")
  checks.expect_match(r.out, "leaked=%d+", "go: leak report has a leaked=<n> field")
  local leaked = tonumber(r.out:match("leaked=(%d+)"))
  if leaked and leaked >= 1 then
    checks.expect_match("ok", "ok", "go: at least one goroutine leaked (" .. leaked .. ")")
  else
    checks.expect_match(r.out, "leaked=[1-9]", "go: leaked count is >= 1")
  end

  -- race: needs a -race build, which is not what the plain demo.sh build
  -- produces for this chapter (unlike ch06, this chapter is not
  -- exclusively about concurrency) -- build the instrumented binary
  -- directly.
  r = checks.run("cd go && go build -race -o bin/app-race .")
  checks.expect_exit(r, 0, "go: race-instrumented build compiles")

  r = checks.run("./go/bin/app-race race")
  checks.expect_exit(r, 66, "go: race build exits 66 (race detector default)")
  checks.expect_match(r.out, "DATA RACE", "go: race detector report says 'DATA RACE'")

  -- uaf/uninit/overflow: not expressible in safe Go.
  expect_prevented("uaf", "prevented by the runtime")
  expect_prevented("uninit", "prevented by the runtime")
  expect_prevented("overflow", "prevented by the runtime")

else -- rust
  -- leak: Box::leak is a safe API whose entire point is to opt an
  -- allocation out of reclamation. --errors-for-leak-kinds=all makes
  -- valgrind treat "still reachable" as an error too (the classification
  -- a leaked-but-recently-live pointer gets depends on stray bit patterns
  -- left on the stack, which is not something to pin a test on); the
  -- byte count and "definitely lost" tag are what we actually assert on
  -- this host.
  r = checks.run(
    "valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all " ..
    "--error-exitcode=1 ./rust/target/release/app leak")
  checks.expect_exit(r, 1, "rust: valgrind flags the Box::leak allocation (nonzero exit)")
  checks.expect_match(r.out, "definitely lost", "rust: valgrind report says 'definitely lost'")
  checks.expect_match(r.out, "65,536 bytes", "rust: valgrind report names the leaked size")

  -- miri: informational only. The pinned toolchain (rust-toolchain.toml,
  -- 1.97.1) does not ship a miri component; a nightly toolchain with miri
  -- installed is a bonus double-check, never a requirement -- absence
  -- must degrade gracefully, not fail the example.
  local probe = checks.run("rustup which --toolchain nightly miri")
  if probe.exit == 0 then
    local mr = checks.run("cd rust && cargo +nightly miri run --quiet -- leak 2>&1")
    if mr.out:find("memory leaked") then
      print("info: rust: miri (nightly) also reports 'memory leaked' for the Box::leak allocation")
    else
      print("info: rust: miri ran on nightly but did not report the expected leak text (not asserted)")
    end
  else
    print("info: rust: miri unavailable for the pinned 1.97.1 toolchain and no nightly+miri found " ..
          "-- skipping the bonus check (valgrind above already covers the leak finding)")
  end

  -- uaf/uninit/overflow/race: rejected by the compiler (or, for race,
  -- have no data race to report because ownership rules prevent the
  -- unsynchronized shared access in the first place).
  expect_prevented("uaf", "prevented at compile time")
  expect_prevented("uninit", "prevented at compile time")
  expect_prevented("overflow", "prevented at compile time")
  expect_prevented("race", "prevented at compile time")
end

checks.finish()
