-- Verify 56-capstone-comparison (C++ only): Part 14's closing comparison.
--
-- ONE workload, SIX models, TWO uniform instruments. ch50-ch55 each measured
-- their own model with their own instrument on their own workload, so no two
-- numbers in the compendium were strictly comparable. That is what this example
-- fixes, and what these gates check.
--
-- NOTHING HERE IS TIMED (ch39) and nothing binds a port.
--
-- Hard-gated (A-D, F):
--   A. Every arm computes the digest 0x481984990deee5ff -- ch46-ch55's.
--   B. THE CAPSTONE GATE. Every model folds the identical workload to the
--      IDENTICAL total. This runs before any comparison is drawn, because a
--      model that produced a different total would not be a differently-priced
--      answer to the same question -- it would be a wrong one, and the whole
--      chapter would be comparing incomparable things.
--   C. Instrument 1 (ch50's gettid, ch49's vocabulary): the three single-
--      threaded models report exactly 1 tid, and the four multi-threaded ones
--      report more than 1. `sequential`, `coroutine` and `fiber` occupy one CPU
--      while holding eight tasks in flight -- ch49's "concurrent, not parallel"
--      as a count rather than an assertion.
--   D. Instrument 2 (ch51's strace method): the single-threaded models never
--      reach the futex at all; the lock-based models reach it hundreds of
--      times; and Asio's STRAND costs less than half what std::mutex costs for
--      byte-identical work. Gated as signs and one ratio, never as magnitudes.
--      Degrades to an informational SKIP if strace cannot attach.
--   F. clang parity on the digest and on the total.
--
-- Gated-if-present (E): the seventh model, P2300 senders. Requires Conan AND
-- (on a cold cache) the network, so it is skip-if-absent -- a machine with
-- neither still passes A-D and F. NOTE it links the NVIDIA stdexec REFERENCE
-- IMPLEMENTATION: __cpp_lib_senders is still undefined in this toolchain's
-- standard library, which ch49 measured and the `versions` arm re-reports.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("56-capstone-comparison is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
end

local function lit(s)
  return (s:gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1"))
end

local function tool_present(cmd)
  return checks.run("command -v " .. cmd .. " >/dev/null 2>&1").exit == 0
end

local function field(out, name)
  local v = out:match(name .. "=(%-?%d+)")
  return v and tonumber(v) or nil
end

local BIN = "cpp/build/release/capstone"
local SENDERS_BIN = "cpp/build/conan/capstone-senders"
local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: the six-model core builds against SYSTEM Boost only -- no Conan, " ..
  "no network, no third-party package")

-- ── A. one digest, every arm ─────────────────────────────────────────────

local arms = { "versions", "sequential", "pthreads", "std-thread", "boost-thread", "coroutine",
               "fiber", "asio", "table" }
local out = {}
for _, a in ipairs(arms) do
  local r = checks.run("timeout 300 " .. BIN .. " " .. a)
  checks.expect_exit(r, 0, "cpp: arm " .. a .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: arm " .. a .. " computes the expected digest (== ch46-ch55's)")
  out[a] = r.out
end

-- ── B. THE CAPSTONE GATE: one workload, one answer ───────────────────────

local models = { "sequential", "pthreads", "std-thread", "boost-thread", "coroutine", "fiber",
                 "asio" }
local reference = out["sequential"]:match("total=(0x%x+)")
checks.expect_match(tostring(reference ~= nil), "true",
  "cpp: the sequential control produced a total to compare against (" .. tostring(reference) .. ")")

for _, m in ipairs(models) do
  checks.expect_match(out[m], "correct=yes",
    "cpp: " .. m .. " folded all 40000 units correctly")
  local total = out[m]:match("total=(0x%x+)")
  checks.expect_match(tostring(total), tostring(reference),
    "cpp: " .. m .. " folded to the IDENTICAL total (" .. tostring(total) ..
    ") -- six models, one workload, one answer, so the comparison compares like with like")
end

-- ── C. instrument 1: who ran it ──────────────────────────────────────────

for _, m in ipairs({ "sequential", "coroutine", "fiber" }) do
  checks.expect_match(out[m], "distinct_tids=1",
    "cpp: " .. m .. " held 8 tasks in flight on exactly ONE OS thread -- ch49's " ..
    "'concurrent, not parallel' as a count")
end

for _, m in ipairs({ "pthreads", "std-thread", "boost-thread", "asio" }) do
  local n = field(out[m], "distinct_tids")
  checks.expect_match(tostring(n ~= nil and n > 1), "true",
    "cpp: " .. m .. " spread the SAME work across " .. tostring(n) .. " OS threads")
end

-- ── D. instrument 2: what synchronization cost, counted from outside ─────

local function futex_count(bin, a)
  local r = checks.run("strace -f -c -e trace=futex " .. bin .. " " .. a ..
                       " 2>&1 | awk '/futex/{print $4}' | head -1")
  return tonumber((r.out:gsub("%s", "")))
end

if not tool_present("strace") then
  print("SKIP: strace not found -- gate D (the futex axis) not asserted")
else
  local n = {}
  for _, m in ipairs(models) do
    n[m] = futex_count(BIN, m)
  end

  for _, m in ipairs({ "sequential", "coroutine", "fiber" }) do
    checks.expect_match(tostring(n[m] ~= nil and n[m] < 10), "true",
      "cpp: " .. m .. " never reached the futex (" .. tostring(n[m]) .. " calls) -- one thread " ..
      "means nothing to synchronize, whatever the model")
  end

  for _, m in ipairs({ "pthreads", "std-thread", "boost-thread" }) do
    checks.expect_match(tostring(n[m] ~= nil and n[m] > 100), "true",
      "cpp: " .. m .. " reached the futex " .. tostring(n[m]) .. " times for the same 40000 " ..
      "folds -- a blocked thread is a thread the kernel has to park and wake (ch51)")
  end

  checks.expect_match(tostring(n["asio"] ~= nil and n["std-thread"] ~= nil and
                               n["asio"] * 2 < n["std-thread"]), "true",
    "cpp: Asio's STRAND cost less than half what std::mutex cost for byte-identical work " ..
    "(strand=" .. tostring(n["asio"]) .. ", mutex=" .. tostring(n["std-thread"]) ..
    ") -- a mutex blocks a thread, a strand defers a handler (ch55)")

  print("info: futex magnitudes are NOT gated -- they move with scheduling on every run. " ..
    "What is gated is the three-tier SIGN: one thread reaches the kernel not at all, a lock " ..
    "reaches it hundreds of times, and a strand lands between them.")
end

-- ── E. the seventh model: P2300 senders (skip-if-absent) ─────────────────
--
-- Isolated exactly as ch46's Conan sub-target is: ./demo.sh cpp build never
-- evaluates it, so everything above passed without Conan, without the network,
-- and without stdexec present at all.

if not tool_present("conan") then
  print("SKIP: conan not found -- gate E (the P2300 senders arm) not asserted. The six-model " ..
    "core above is unaffected, which is the point of keeping this arm isolated.")
else
  local export = checks.run("conan export cpp/conan/recipe >/dev/null 2>&1")
  local install = checks.run(
    "conan install cpp/conan --output-folder=cpp/build/conan --build=missing " ..
    "--lockfile=cpp/conan/conan.lock -s compiler.cppstd=23 >/dev/null 2>&1")
  if export.exit ~= 0 or install.exit ~= 0 then
    print("SKIP: conan could not provide stdexec (offline with a cold cache?) -- gate E not " ..
      "asserted")
  else
    -- The build type MUST match the Conan profile's. CMakeDeps gates the
    -- include dirs behind $<$<CONFIG:Release>:...>, so a RelWithDebInfo
    -- configure silently drops them and the arm fails to find its headers.
    local built = checks.run("cd cpp && cmake --preset conan >/dev/null 2>&1 && " ..
                             "cmake --build --preset conan >/dev/null 2>&1")
    checks.expect_exit(built, 0, "cpp: the P2300 arm builds against the pinned stdexec " ..
      "(nvhpc-26.05, sha256-verified by the recipe)")

    local s = checks.run("timeout 300 " .. SENDERS_BIN .. " senders")
    checks.expect_exit(s, 0, "cpp: the senders arm runs")
    checks.expect_match(s.out, lit(EXPECTED_DIGEST),
      "cpp: the senders arm computes the expected digest too")
    local total = s.out:match("total=(0x%x+)")
    checks.expect_match(tostring(total), tostring(reference),
      "cpp: and folds to the IDENTICAL total (" .. tostring(total) .. ") -- a SEVENTH model, " ..
      "same workload, same answer")
    checks.expect_match(s.out, "stdlib_p2300=no",
      "cpp: reported as the NVIDIA stdexec REFERENCE implementation -- ch49 measured " ..
      "__cpp_lib_senders as undefined and it still is, so this is not the standard library")
  end
end

-- The same claim, from the core binary, gated whether or not Conan exists.
checks.expect_match(out["versions"], "stdlib_p2300=no",
  "cpp: the standard library still has no P2300 (__cpp_lib_senders undefined) -- ch49's " ..
  "measurement, re-reported here rather than quietly dropped")

-- ── F. clang parity ──────────────────────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target capstone >/dev/null 2>&1 && " ..
    "timeout 300 ./build/release-clang/capstone std-thread")
  checks.expect_exit(cl, 0, "cpp: clang-built capstone runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST), "cpp: clang and GCC agree on the digest")
  local total = cl.out:match("total=(0x%x+)")
  checks.expect_match(tostring(total), tostring(reference),
    "cpp: and clang folds to the same total as GCC (" .. tostring(total) .. ")")
else
  print("SKIP: clang++ not found on PATH -- gate F not asserted")
end

checks.finish()
