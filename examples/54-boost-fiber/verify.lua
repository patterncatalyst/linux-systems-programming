-- Verify 54-boost-fiber (C++ only): what a real stack buys, and what it costs.
--
-- ch53 measured a C++20 coroutine frame at 32 bytes -- stackless, holding only
-- the locals that cross a suspension. A Boost fiber is STACKFULL: it owns a
-- machine stack switched by boost::context. That single difference produces
-- both its price and its capability, and both are measured here.
--
-- ch44 owns Go's GMP scheduler (M:N decided automatically by a runtime); this
-- example shows C++'s version, where the scheduler is an explicit choice and
-- the choice is observable in /proc via gettid().
--
-- NOTHING HERE IS TIMED (see ch39). Every observable is a byte count, a tid
-- count, a call depth, or an exit signal. No root, no VM, no network, no
-- Conan: system Boost only.
--
-- Hard-gated (A-F):
--   A. Every non-faulting case computes the same digest, 0x481984990deee5ff --
--      ch46 through ch53.
--   B. Stacks: a fixedsize_stack returns EXACTLY what was asked for, and
--      protected_fixedsize_stack returns exactly one page more. The guard-page
--      DELTA is gated against stack_traits::page_size(), never an absolute
--      byte count, so this holds on a host with different page sizes.
--   C. The three-way memory table: a fiber stack sits strictly between ch53's
--      32-byte frame and ch50's 8388608-byte thread stack, and both ratios are
--      computed by the program from those measured numbers.
--   D. Deep suspend -- the capability the stack buys. A fiber yields from
--      inside an ORDINARY function six frames below the fiber body and resumes
--      at the same depth. A stackless coroutine cannot do this at any price:
--      co_await is only valid in a coroutine body, so every frame in the
--      suspend path would have to be a coroutine too.
--   E. M:N, and the scheduler is a choice. The SAME 16 fibers doing the SAME
--      work report exactly 1 distinct tid under the default round_robin
--      scheduler and more than 1 under shared_work. One call to
--      use_scheduling_algorithm is the only difference -- ch49's grid, applied
--      to fibers.
--   F. The guard page, both directions. A fiber that stays inside its 64 KiB
--      stack exits 0; the same fiber recursing 60x past it dies of a SIGNAL.
--      The gate asserts the exit signal and NEVER anything the faulting run
--      printed -- output after a stack overflow is undefined, and gating on it
--      would be gating on UB (the rule ch49 established for oob_unchecked).
--
-- Gated-if-present (G): clang++ parity on the digest.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("54-boost-fiber is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
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

local BIN = "cpp/build/release/fiberdemo"
local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: builds against system Boost.Fiber (no Conan, no network)")

-- ── A. one digest, every non-faulting case ───────────────────────────────
--
-- guard-overflow is deliberately absent: it dies of SIGSEGV by design and
-- never reaches its report line. Gate F handles it.

local cases = { "versions", "stacks", "versus", "deep", "roundrobin", "sharedwork", "guard-ok" }
local out = {}
for _, c in ipairs(cases) do
  local r = checks.run("timeout 120 " .. BIN .. " " .. c)
  checks.expect_exit(r, 0, "cpp: case " .. c .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: case " .. c .. " computes the expected digest (== ch46-ch53's)")
  out[c] = r.out
end

-- ── B. stacks, and the guard page measured as a delta ────────────────────

local st = out["stacks"]
local page = field(st, "page")
local requested = field(st, "requested")
local fixed = field(st, "fixedsize")
local protected_size = field(st, "protected")
local delta = field(st, "guard_delta")

checks.expect_match(st, "exact_request=yes",
  "cpp: fixedsize_stack allocated exactly what was asked (requested=" .. tostring(requested) ..
  " got=" .. tostring(fixed) .. ")")
checks.expect_match(st, "guard_is_one_page=yes",
  "cpp: protected_fixedsize_stack costs exactly one page more (delta=" .. tostring(delta) ..
  " page_size=" .. tostring(page) .. ") -- that page is the guard")
checks.expect_match(tostring(delta ~= nil and page ~= nil and delta == page), "true",
  "cpp: and the delta is gated against page_size(), not a hardcoded 4096")

-- ── C. the three-way memory table ────────────────────────────────────────

local vs = out["versus"]
local thread_b = field(vs, "thread")
local fiber_b = field(vs, "fiber")
local frame_b = field(vs, "coroutine_frame")
local per_thread = field(vs, "fibers_per_thread_stack")
local per_fiber = field(vs, "frames_per_fiber_stack")

checks.expect_match(tostring(thread_b == 8388608), "true",
  "cpp: the table cites ch50's actually-measured thread stack (8388608)")
checks.expect_match(tostring(frame_b == 32), "true",
  "cpp: and ch53's actually-measured coroutine frame (32)")
checks.expect_match(vs, "fiber_between_the_two=yes",
  "cpp: a fiber stack (" .. tostring(fiber_b) .. ") sits strictly between them -- it has a " ..
  "stack, so it costs more than a frame; the kernel never sees it, so it costs less " ..
  "than a thread")
checks.expect_match(tostring(per_thread ~= nil and fiber_b ~= nil and thread_b ~= nil
  and per_thread == math.floor(thread_b / fiber_b)), "true",
  "cpp: fibers_per_thread_stack=" .. tostring(per_thread) .. " is arithmetic on two " ..
  "measured numbers, not a quoted figure")
checks.expect_match(tostring(per_fiber ~= nil and fiber_b ~= nil and frame_b ~= nil
  and per_fiber == math.floor(fiber_b / frame_b)), "true",
  "cpp: frames_per_fiber_stack=" .. tostring(per_fiber) .. " likewise")

-- ── D. the capability, not the cost ──────────────────────────────────────

local deep = out["deep"]
local yield_depth = field(deep, "yielded_at_depth")
checks.expect_match(deep, "same=yes",
  "cpp: the fiber resumed at exactly the depth it suspended from")
checks.expect_match(tostring(yield_depth ~= nil and yield_depth > 1), "true",
  "cpp: and it suspended from " .. tostring(yield_depth) .. " frames deep inside ORDINARY " ..
  "functions -- no co_await, no coroutine, no function colouring")

-- ── E. M:N: same fibers, different scheduler, visible difference ─────────

checks.expect_match(out["roundrobin"], "distinct_tids=1",
  "cpp: 16 fibers under the default round_robin scheduler ran on exactly ONE OS thread -- " ..
  "concurrency with no parallelism, in ch49's terms")

local sw = out["sharedwork"]
local sw_tids = field(sw, "distinct_tids")
-- Scheduler-dependent, so retried a bounded number of times rather than
-- asserted on a single run: "at least one of K runs saw migration" is a real
-- effect where a single run on a loaded host might legitimately not. Measured
-- 4 of 4 on five consecutive runs here, so the retry is insurance.
if sw_tids ~= nil and sw_tids <= 1 then
  for _ = 1, 4 do
    local r = checks.run("timeout 120 " .. BIN .. " sharedwork")
    local n = field(r.out, "distinct_tids")
    if n ~= nil and n > 1 then
      sw_tids = n
      break
    end
  end
end
checks.expect_match(tostring(sw_tids ~= nil and sw_tids > 1), "true",
  "cpp: the SAME 16 fibers under shared_work migrated across " .. tostring(sw_tids) ..
  " OS threads -- one call to use_scheduling_algorithm is the only difference")

-- ── F. the guard page, both directions ───────────────────────────────────

local ok = checks.run("timeout 60 " .. BIN .. " guard-ok >/dev/null 2>&1; echo exit=$?")
checks.expect_match(ok.out, "exit=0",
  "cpp: a fiber that stays inside its 64 KiB protected stack completes normally")

local overflow = checks.run("timeout 60 " .. BIN .. " guard-overflow >/dev/null 2>&1; echo exit=$?")
local code = tonumber(overflow.out:match("exit=(%d+)") or "0")
-- Asserted as "died of a signal" (128 + signo), never as a specific output.
-- What a process prints after running off its stack is undefined; ch49
-- established that gating on the output of UB is unsound, so this gates the
-- exit status alone. 124 would mean timeout(1) killed it -- i.e. it hung
-- rather than faulting -- and must NOT pass.
checks.expect_match(tostring(code ~= nil and code >= 128 and code ~= 124), "true",
  "cpp: recursing 60x past that stack dies of a signal (exit=" .. tostring(code) ..
  "; 139 = SIGSEGV on the guard page) rather than corrupting the next mapping")

-- ── G. clang parity (skip-if-absent) ─────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target fiberdemo >/dev/null 2>&1 && " ..
    "timeout 120 ./build/release-clang/fiberdemo versus")
  checks.expect_exit(cl, 0, "cpp: clang-built fiberdemo runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest")
  checks.expect_match(cl.out, "fiber_between_the_two=yes",
    "cpp: and clang reports the same three-way ordering")
else
  print("SKIP: clang++ not found on PATH -- gate G not asserted")
end

print("info: context-switch timings are NOT gated -- this book does not gate on " ..
  "durations (ch39). The fiber-vs-thread claim here is about MEMORY and about where a " ..
  "suspension may occur, both of which are counts rather than clocks.")

checks.finish()
