-- Verify 52-boost-thread (C++ only): what is left in Boost.Thread that the
-- C++ standard did not take.
--
-- Boost.Thread is where std::thread came from -- C++11 standardized it almost
-- wholesale -- so the question worth asking in 2026 is what survived the
-- standardization. Every gate below asserts a capability std:: does not have
-- on this toolchain, verified rather than claimed.
--
-- NOTHING HERE IS TIMED (see ch39). No root, no VM, no network, no Conan:
-- system Boost only.
--
-- Hard-gated (A-F):
--   A. Every pillar computes the same digest, 0x481984990deee5ff -- the value
--      ch46's C++, ch47's Go, ch48's Rust, ch49's conc, ch50's pthreads, and
--      ch51's stdthread all produce.
--   B. Continuations: boost::future::then() chains async(21) into 42, and
--      when_all joins two futures into 1 + 2. std::future can do neither.
--   C. The contrast is COMPILE-TIME, not editorial: a concept is evaluated
--      against std::future<int> and boost::future<int> and the two answers
--      are printed. Asserting on that output means the claim "std::future has
--      no .then()" is checked by the compiler on every run, not by the author
--      once.
--   D. Upgrade lock: a thread holding boost::upgrade_lock promotes to unique
--      WITHOUT releasing, while a concurrent reader could still take a shared
--      lock beforehand. std::shared_mutex has no upgrade path at all -- the
--      name std::upgrade_lock does not exist, which is why this gate has no
--      std-side half to compare against.
--   E. Interruption -- THE THREE-WAY PAYOFF. boost::thread::interrupt()
--      throws an ordinary C++ exception at a defined interruption point, so:
--        * a worker catches boost::thread_interrupted and joins normally, AND
--        * a worker that swallows it with catch (...) and does not rethrow
--          leaves the process ALIVE, exit 0.
--      The identical mistake against ch50's pthread_cancel aborted the
--      process with "FATAL: exception not rethrown", exit 134. Boost sits
--      between ch50's forced unwind and ch51's flag, and this gate is what
--      proves it rather than asserting it.
--   F. Attributes: a Boost thread created with a 256 KiB attribute observes
--      at least that much via pthread_getattr_np -- the same 262144 ch50
--      measured through the POSIX API directly.
--
-- Gated-if-present (G): clang++ parity on the digest.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("52-boost-thread is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
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

local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: builds against system Boost (no Conan, no network)")

-- ── A. one digest, every pillar ──────────────────────────────────────────

-- interrupt-busy is run separately below: it leaves via _exit() with a
-- detached thread still spinning, so it is not part of this loop's
-- "runs and returns cleanly" shape.
local pillars = { "versions", "continuations", "when-all", "upgrade", "interrupt",
                  "interrupt-swallow", "attributes" }
local out = {}
for _, p in ipairs(pillars) do
  local r = checks.run("./demo.sh cpp run " .. p)
  checks.expect_exit(r, 0, "cpp: pillar " .. p .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: pillar " .. p .. " computes the expected digest (== ch46-ch51's)")
  out[p] = r.out
end

-- ── C. the compile-time contrast (checked first: B depends on it) ────────

checks.expect_match(out["versions"], "std::future_has_then=no",
  "cpp: std::future has NO .then() -- a concept the compiler evaluated, not a claim")
checks.expect_match(out["versions"], "boost::future_has_then=yes",
  "cpp: boost::future does have .then()")
checks.expect_match(out["versions"], "BOOST_THREAD_VERSION=5",
  "cpp: the version-5 interface is active (set in CMakeLists, before any Boost header)")
checks.expect_match(out["versions"], "Boost 1%.%d+%.%d+",
  "cpp: BOOST_VERSION is reported in the output, so the footer can state what was measured")

-- ── B. continuations ─────────────────────────────────────────────────────

checks.expect_match(out["continuations"], "async%(21%)%.then%(%*2%) = 42",
  "cpp: boost::future::then() chained a continuation onto a running async")
checks.expect_match(out["continuations"], "chained=yes",
  "cpp: and the continuation produced the composed value rather than blocking for it")
checks.expect_match(out["when-all"], "joined two futures %-> 1 %+ 2 = 3",
  "cpp: boost::when_all joined two independent futures into one")

-- ── D. the upgrade lock ──────────────────────────────────────────────────

checks.expect_match(out["upgrade"], "reader_saw_old_value=yes",
  "cpp: a concurrent reader could still take a shared lock while the upgrade lock was held")
checks.expect_match(out["upgrade"], "value_after_promotion=2",
  "cpp: the promoted lock then wrote the new value")
checks.expect_match(out["upgrade"], "promoted shared%->unique without releasing",
  "cpp: upgrade_to_unique_lock promotes in place -- no window for another writer, " ..
  "which is exactly what std::shared_mutex cannot offer")

-- ── E. cancellation, three ways -- the payoff ────────────────────────────

checks.expect_match(out["interrupt"], "caught boost::thread_interrupted",
  "cpp: interrupt() delivered an ordinary C++ exception at a defined interruption point")
checks.expect_match(out["interrupt"], "caught=yes joined_normally=yes",
  "cpp: and the worker returned normally afterwards")

-- The decisive one. In ch50 this exact shape -- catch everything, rethrow
-- nothing -- killed the process. Here it must NOT.
local swallow = checks.run("./demo.sh cpp run interrupt-swallow 2>&1; echo exit=$?")
checks.expect_match(swallow.out, "exit=0",
  "cpp: swallowing a Boost interruption leaves the process ALIVE (exit 0) -- " ..
  "the identical mistake against ch50's pthread_cancel aborted with exit 134")
checks.expect_match(swallow.out, "caught the interruption and did NOT rethrow",
  "cpp: and the catch-all really did run, so this is not a case of never being interrupted")
checks.expect_match(swallow.out, "process survived",
  "cpp: execution continued past the join -- boost::thread_interrupted is an ordinary " ..
  "exception, not glibc's forced unwind")

-- The other half of "defined interruption points": a thread that never
-- reaches one cannot be interrupted. ch50's pthread_cancel CAN yank a thread
-- out of a compute loop because it is asynchronous; Boost trades that reach
-- for predictability, and this asserts the trade rather than describing it.
local busy = checks.run("./demo.sh cpp run interrupt-busy 2>&1")
checks.expect_match(busy.out, lit(EXPECTED_DIGEST),
  "cpp: pillar interrupt-busy computes the expected digest")
checks.expect_match(busy.out, "worker_stopped=no",
  "cpp: a busy loop with no interruption point ignores interrupt() entirely -- " ..
  "Boost interrupts at defined points, it does not preempt")

-- ── F. attributes: ch50's stack control, portably ────────────────────────

local attrs = out["attributes"]
local requested = field(attrs, "requested")
local observed = field(attrs, "observed")
checks.expect_match(attrs, "at_least_requested=yes",
  "cpp: a Boost thread created with a 256 KiB attribute got at least that much " ..
  "(requested=" .. tostring(requested) .. " observed=" .. tostring(observed) .. ")")
checks.expect_match(tostring(observed ~= nil and observed == 262144), "true",
  "cpp: and the number is 262144 -- byte-identical to what ch50 measured through " ..
  "pthread_attr_setstacksize directly, because on Linux this is that call")

-- ── G. clang parity (skip-if-absent) ─────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target boostthread >/dev/null 2>&1 && " ..
    "./build/release-clang/boostthread continuations")
  checks.expect_exit(cl, 0, "cpp: clang-built boostthread runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest")
  checks.expect_match(cl.out, "chained=yes",
    "cpp: and clang's build chains the same continuation")
else
  print("SKIP: clang++ not found on PATH -- gate G not asserted")
end

-- ── not gated ────────────────────────────────────────────────────────────

print("info: Boost's experimental executors are not gated -- this chapter covers what " ..
  "the standard did NOT take from Boost.Thread, and executors are the part C++26's " ..
  "std::execution is actively replacing (see ch49's forward-looking section and ch56)")

checks.finish()
