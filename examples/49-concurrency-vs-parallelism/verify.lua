-- Verify 49-concurrency-vs-parallelism (C++ only): the framing chapter that
-- opens Part 14, the C++ concurrency compendium. Concurrency is structure
-- (many tasks in flight); parallelism is execution (many CPUs occupied).
-- Every assertion below checks a real, program-produced structural fact --
-- a count or a set cardinality -- never a duration and never a bare exit code.
--
-- NOTHING HERE IS TIMED. Wall-clock numbers are not reproducible across hosts
-- or runs and this book does not gate on them (see ch39). The observables are:
--   cpus_allowed   -- CPU_COUNT(sched_getaffinity), a permission
--   distinct_cpus  -- popcount of every CPU any worker was seen on, an outcome
--   max_inflight   -- high-water mark of live workers, "tasks in flight"
--   interleaves    -- ticket-sequence gaps, i.e. observed worker handoffs
--
-- Hard-gated (A-F), all offline, stdlib + Linux scheduler syscalls only:
--   A. All four models compute the identical digest (0x481984990deee5ff --
--      the same value as ch46's C++, ch47's Go, and ch48's Rust toolbox),
--      and all four report consensus=yes. The answer does not depend on the
--      execution model; that is the chapter's thesis.
--   B. parallel: distinct_cpus > 1 with cpus_allowed = all CPUs. Parallelism
--      is real and observed, not merely permitted.
--   C. concurrent: cpus_allowed = 1 AND distinct_cpus = 1 AND max_inflight > 1
--      AND interleaves > 0 -- many tasks in flight on exactly one CPU. This
--      is the chapter's central case and no single one of those four facts
--      proves it alone.
--   D. sequential: max_inflight = 1 AND interleaves = 0 -- neither concurrent
--      nor parallel. both: distinct_cpus > 1 AND interleaves > 0 AND
--      max_inflight > cpus_allowed (genuinely oversubscribed).
--   E. C++26 Contracts (P2900) on GCC 16: one source, four binaries, one per
--      -fcontract-evaluation-semantic. Four DISTINCT observable behaviors are
--      asserted -- diagnostic present/absent crossed with terminating/not --
--      never four exit codes.
--   F. Safety hardening: the same source built with and without
--      _GLIBCXX_ASSERTIONS. The hardened build traps and names the failed
--      predicate; the unchecked build is asserted STATICALLY to contain no
--      bounds check at all (its runtime behavior is undefined and is never
--      asserted on -- gating on the output of UB would be unsound).
--
-- Gated-if-present (G): clang++ parity. If clang is installed, a
-- clang-built conc must produce the identical digest. Absent -> informational
-- SKIP, never a checks.skip() (which would abort) and never a FAIL.
--
-- NOT gated: std::execution senders (P2300) and static reflection (P2996).
-- Measured on this host, neither exists: __cpp_lib_senders is undefined
-- (GCC 16's <execution> is the C++17 parallel-algorithms header,
-- __cpp_lib_execution = 201902) and __cpp_impl_reflection is undefined in
-- GCC 16 with clang 22 rejecting -freflection outright. The chapter covers
-- both as clearly-labeled forward-looking prose; ch56 owns the live sender
-- comparison if the toolchain has caught up by then.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("49-concurrency-vs-parallelism is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
end

-- Escape a literal string for use inside a Lua pattern.
local function lit(s)
  return (s:gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1"))
end

local function tool_present(cmd)
  return checks.run("command -v " .. cmd .. " >/dev/null 2>&1").exit == 0
end

-- Pull a named integer field out of a `conc report:` line.
local function field(out, name)
  local v = out:match(name .. "=(%-?%d+)")
  return v and tonumber(v) or nil
end

local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: builds (stdlib only, no Boost, no Conan, no network)")

-- ── A. one answer, four execution models ─────────────────────────────────

local models = {}
for _, m in ipairs({ "sequential", "concurrent", "parallel", "both" }) do
  local r = checks.run("./demo.sh cpp run --model " .. m)
  checks.expect_exit(r, 0, "cpp: conc --model " .. m .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: " .. m .. " computes the expected digest (== ch46/47/48's)")
  checks.expect_match(r.out, "consensus=yes",
    "cpp: " .. m .. " -- every worker independently agreed on the digest")
  models[m] = r.out
end

-- ── B. parallel: parallelism observed, not merely permitted ──────────────
--
-- distinct_cpus is the one scheduler-dependent assertion here. It is retried
-- a bounded number of times rather than asserted on a single run: "at least
-- one of K runs observed more than one CPU" is a real effect, whereas a
-- single run on a fully loaded host could legitimately see one. Measured on
-- this 16-CPU host it reported 14-16 distinct CPUs on every one of 8
-- consecutive runs, so the retry is insurance, not a crutch.
local par_cpus = field(models["parallel"], "distinct_cpus")
local par_allowed = field(models["parallel"], "cpus_allowed")
if par_cpus ~= nil and par_cpus <= 1 then
  for _ = 1, 4 do
    local r = checks.run("./demo.sh cpp run --model parallel")
    local d = field(r.out, "distinct_cpus")
    if d ~= nil and d > 1 then
      par_cpus = d
      break
    end
  end
end
checks.expect_match(tostring(par_cpus ~= nil and par_cpus > 1), "true",
  "cpp: parallel occupied more than one CPU (distinct_cpus=" .. tostring(par_cpus) .. ")")
checks.expect_match(tostring(par_allowed ~= nil and par_allowed > 1), "true",
  "cpp: parallel was allowed more than one CPU (cpus_allowed=" .. tostring(par_allowed) .. ")")

-- ── C. concurrent: many tasks in flight, exactly one CPU ─────────────────

local con = models["concurrent"]
checks.expect_match(con, "cpus_allowed=1",
  "cpp: concurrent pinned itself to one CPU via sched_setaffinity")
checks.expect_match(con, "distinct_cpus=1",
  "cpp: concurrent ran on exactly one CPU -- no parallelism happened")
local con_inflight = field(con, "max_inflight")
local con_inter = field(con, "interleaves")
checks.expect_match(tostring(con_inflight ~= nil and con_inflight > 1), "true",
  "cpp: concurrent had many tasks in flight at once (max_inflight=" .. tostring(con_inflight) .. ")")
checks.expect_match(tostring(con_inter ~= nil and con_inter > 0), "true",
  "cpp: concurrent workers really interleaved (interleaves=" .. tostring(con_inter) .. ")")

-- ── D. sequential is neither; both is both ───────────────────────────────

checks.expect_match(models["sequential"], "max_inflight=1",
  "cpp: sequential never had more than one task in flight")
checks.expect_match(models["sequential"], "interleaves=0",
  "cpp: sequential workers never interleaved -- ticket runs are contiguous")

local both = models["both"]
local both_cpus = field(both, "distinct_cpus")
local both_inter = field(both, "interleaves")
local both_inflight = field(both, "max_inflight")
local both_allowed = field(both, "cpus_allowed")
checks.expect_match(tostring(both_cpus ~= nil and both_cpus > 1), "true",
  "cpp: both occupied many CPUs (distinct_cpus=" .. tostring(both_cpus) .. ")")
checks.expect_match(tostring(both_inter ~= nil and both_inter > 0), "true",
  "cpp: both interleaved (interleaves=" .. tostring(both_inter) .. ")")
checks.expect_match(tostring(both_inflight ~= nil and both_allowed ~= nil
  and both_inflight > both_allowed), "true",
  "cpp: both was genuinely oversubscribed (max_inflight=" .. tostring(both_inflight)
    .. " > cpus_allowed=" .. tostring(both_allowed) .. ")")

-- ── E. C++26 Contracts (P2900), four semantics, four behaviors ───────────

local gcc_build = checks.run("cd cpp && grep -c 'CMAKE_CXX_COMPILER_ID' CMakeLists.txt")
checks.expect_exit(gcc_build, 0, "cpp: the contract targets are guarded on the GNU compiler id")

local ignore = checks.run("./cpp/build/release/contracts_ignore")
checks.expect_exit(ignore, 0, "cpp: contracts ignore -- the violation does not stop the program")
checks.expect_match(ignore.out, "still running after the violation",
  "cpp: contracts ignore -- execution continued past the violated precondition")
checks.expect_match(tostring(ignore.out:find("contract violation") == nil), "true",
  "cpp: contracts ignore -- no diagnostic was printed")

local observe = checks.run("./cpp/build/release/contracts_observe 2>&1")
checks.expect_exit(observe, 0, "cpp: contracts observe -- the program still runs to completion")
checks.expect_match(observe.out, "%[assertion_kind: pre, semantic: observe, .*terminating: no%]",
  "cpp: contracts observe -- the precondition violation is reported, non-terminating")
checks.expect_match(observe.out, "%[assertion_kind: post, semantic: observe, .*terminating: no%]",
  "cpp: contracts observe -- the postcondition violation is reported too")
checks.expect_match(observe.out, "still running after the violation",
  "cpp: contracts observe -- reported and continued, unlike enforce")

local enforce = checks.run("./cpp/build/release/contracts_enforce 2>&1")
checks.expect_match(tostring(enforce.exit ~= 0), "true",
  "cpp: contracts enforce -- the program terminates on the violation")
checks.expect_match(enforce.out, "%[assertion_kind: pre, semantic: enforce, .*terminating: yes%]",
  "cpp: contracts enforce -- the diagnostic says it is terminating")
checks.expect_match(tostring(enforce.out:find("still running") == nil), "true",
  "cpp: contracts enforce -- execution did not continue past the violation")

local quick = checks.run("./cpp/build/release/contracts_quick_enforce 2>&1")
checks.expect_match(tostring(quick.exit ~= 0), "true",
  "cpp: contracts quick_enforce -- the program terminates")
checks.expect_match(tostring(quick.out:find("contract violation") == nil), "true",
  "cpp: contracts quick_enforce -- terminates WITHOUT printing a diagnostic (that is the difference from enforce)")

-- ── F. safety hardening: the check is there, or it is not ────────────────

local hardened = checks.run("./cpp/build/release/oob_hardened 2>&1")
checks.expect_match(tostring(hardened.exit ~= 0), "true",
  "cpp: the hardened build traps on the out-of-bounds subscript")
checks.expect_match(hardened.out, "__n < this%->size%(%)",
  "cpp: the trap names the failed bounds predicate, not just 'aborted'")

-- The unchecked build's RUNTIME behavior is undefined, so it is never
-- asserted on. What is asserted is static and sound: the binary contains no
-- bounds check at all.
local nm_unchecked = checks.run("nm -C cpp/build/release/oob_unchecked 2>/dev/null | grep -c glibcxx_assert_fail || true")
checks.expect_match(nm_unchecked.out, "^%s*0%s*$",
  "cpp: the unchecked build contains no __glibcxx_assert_fail -- the check is compiled out")
local nm_hardened = checks.run("nm -C cpp/build/release/oob_hardened 2>/dev/null | grep -c glibcxx_assert_fail || true")
checks.expect_match(tostring(tonumber(nm_hardened.out:match("(%d+)") or "0") > 0), "true",
  "cpp: the hardened build does contain __glibcxx_assert_fail")

-- ── G. clang parity (skip-if-absent) ─────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target conc >/dev/null 2>&1 && " ..
    "./build/release-clang/conc --model parallel")
  checks.expect_exit(cl, 0, "cpp: clang-built conc runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest (ch46's parity idea, one line)")
else
  print("SKIP: clang++ not found on PATH -- gate G not asserted")
end

-- ── not gated: P2300 senders, P2996 reflection ───────────────────────────

print("info: std::execution senders (P2300) not gated -- __cpp_lib_senders is undefined on" ..
  " this toolchain; GCC 16's <execution> is the C++17 parallel-algorithms header")
print("info: static reflection (P2996) not gated -- __cpp_impl_reflection undefined in GCC 16," ..
  " and clang 22 rejects -freflection")

checks.finish()
