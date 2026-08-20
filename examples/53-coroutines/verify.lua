-- Verify 53-coroutines (C++ only): what a suspended computation COSTS.
--
-- ch27 already built a working coroutine engine -- promise_type, the awaitable
-- protocol, symmetric transfer, an epoll reactor that parks and resumes
-- handles. That chapter owns the MECHANISM and nothing here repeats it.
--
-- This example asks the question ch27 never did, and it is a systems question:
-- a coroutine frame and a thread both hold one paused computation, so what do
-- they each cost? ch50 measured a thread's stack at 8388608 bytes. The frames
-- below are measured by overloading operator new inside the promise type,
-- which is the only portable way to see a size the language hides.
--
-- NOTHING HERE IS TIMED (see ch39). Every observable is a byte count, an
-- allocation count, or a computed value. No threads, no Boost, no network.
--
-- Hard-gated (A-E):
--   A. Every case computes the same digest, 0x481984990deee5ff -- ch46's C++,
--      ch47's Go, ch48's Rust, ch49's conc, ch50's pthreads, ch51's stdthread,
--      ch52's boostthread.
--   B. Frames track what is LIVE ACROSS A SUSPENSION: a trivial coroutine's
--      frame is overhead only, one carrying a char[4096] exceeds 4096, and the
--      middle case sits between them. The RELATIONSHIPS are gated, never the
--      exact byte counts -- those are ABI- and compiler-dependent, and the
--      chapter prints them rather than asserting them.
--   C. HALO is MEASURED, not assumed. Heap allocation elision is PERMITTED by
--      the standard and required by nothing, and on this host GCC 16 elides
--      none while clang 22 elides all of them from -O1 up. So this gate
--      asserts only that the measurement is internally consistent -- the count
--      never exceeds the number of calls, and the work still happened. It
--      deliberately does NOT assert that elision does or does not occur, so a
--      future GCC that starts eliding will not fail it. The clang contrast is
--      gate F.
--   D. std::generator (C++23) is EXERCISED: an infinite Fibonacci coroutine
--      consumed finitely produces 4181 and is then destroyed mid-suspension.
--   E. The comparison this chapter exists for: a coroutine frame is at least
--      three orders of magnitude smaller than ch50's measured thread stack.
--      Gated as a ratio, so it holds on any host where the relationship holds.
--   Plus: the lifetime trap -- an abandoned frame is allocated and never
--      freed, while an owned one is freed exactly once. Counted through
--      operator delete, because "did anyone call destroy()" is the real
--      question and allocation counts cannot answer it.
--
-- Gated-if-present (F): clang++ parity on the digest, AND the HALO contrast --
-- if clang is installed, its elision count must be no greater than GCC's.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("53-coroutines is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
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
checks.expect_exit(b, 0, "cpp: builds (C++23 stdlib only -- no threads, no Boost, no network)")

-- ── A. one digest, every case ────────────────────────────────────────────

local cases = { "versions", "frames", "halo", "generator", "versus-thread", "lifetime" }
local out = {}
for _, c in ipairs(cases) do
  local r = checks.run("./demo.sh cpp run " .. c)
  checks.expect_exit(r, 0, "cpp: case " .. c .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: case " .. c .. " computes the expected digest (== ch46-ch52's)")
  out[c] = r.out
end

checks.expect_match(out["versions"], "__cpp_lib_generator=%d+",
  "cpp: C++23 <generator> is present on this toolchain")

-- ── B. the frame holds what crosses the suspension ───────────────────────

local frames = out["frames"]
local trivial = field(frames, "trivial")
local small = field(frames, "small")
local large = field(frames, "large")

checks.expect_match(frames, "overhead_only=yes",
  "cpp: a trivial coroutine's frame is bookkeeping only (trivial=" .. tostring(trivial) ..
  " bytes) -- promise, resume/destroy pointers, state index")
checks.expect_match(frames, "tracks_live_locals=yes",
  "cpp: frame size tracks the locals live across the suspend (small=" .. tostring(small) ..
  " sits between trivial and large)")
checks.expect_match(frames, "carries_4k_buffer=yes",
  "cpp: a coroutine holding a char[4096] across a suspend has a frame larger than 4096 " ..
  "(large=" .. tostring(large) .. ") -- the buffer really is in there")
checks.expect_match(tostring(trivial ~= nil and large ~= nil and large > trivial * 10), "true",
  "cpp: and the two differ by more than an order of magnitude -- frame cost is what you " ..
  "carry, not a flat overhead")

-- ── C. HALO: measured, never assumed ─────────────────────────────────────

local halo = out["halo"]
local calls = field(halo, "calls")
local allocs = field(halo, "allocations")
local elided = field(halo, "elided")

checks.expect_match(tostring(allocs ~= nil and calls ~= nil and allocs <= calls), "true",
  "cpp: the allocation count is internally consistent (allocations=" .. tostring(allocs) ..
  " <= calls=" .. tostring(calls) .. ")")
checks.expect_match(tostring(elided ~= nil and allocs ~= nil and calls ~= nil
  and elided == calls - allocs), "true",
  "cpp: elided is reported as calls minus allocations (elided=" .. tostring(elided) .. ")")
checks.expect_match(halo, "work_done=1000",
  "cpp: all 1000 coroutine bodies ran regardless of whether their frames were heap-allocated")
-- Deliberately NOT asserted: that elision does or does not happen. It is
-- permitted and not required, GCC 16 does none of it and clang 22 does all of
-- it, and a gate either way would encode one compiler's choice as a rule.
print("info: whether HALO fires is NOT gated -- elision is permitted by " ..
  "[dcl.fct.def.coroutine] and required by nothing. Measured on this host: g++ 16.1.1 " ..
  "elides none at any -O level, clang++ 22.1.8 elides all from -O1 up. Gate F compares them.")

-- ── D. std::generator, exercised ─────────────────────────────────────────

checks.expect_match(out["generator"], "twentieth_fib=4181",
  "cpp: C++23 std::generator produced the correct 20th Fibonacci term")
checks.expect_match(out["generator"], "correct=yes",
  "cpp: an infinite generator consumed finitely, then destroyed mid-suspension")

-- ── E. the number this chapter exists for ────────────────────────────────

local vt = out["versus-thread"]
local frame_bytes = field(vt, "coroutine_frame")
local stack_bytes = field(vt, "thread_stack")
local ratio = field(vt, "ratio")

checks.expect_match(tostring(stack_bytes == 8388608), "true",
  "cpp: the comparison cites ch50's actually-measured thread stack (8388608 bytes)")
checks.expect_match(vt, "three_orders_of_magnitude=yes",
  "cpp: a coroutine frame (" .. tostring(frame_bytes) .. " bytes) is at least 1000x smaller " ..
  "than a thread stack -- ratio=" .. tostring(ratio) .. "x")
checks.expect_match(tostring(ratio ~= nil and frame_bytes ~= nil and stack_bytes ~= nil
  and ratio == math.floor(stack_bytes / frame_bytes)), "true",
  "cpp: and the ratio is the arithmetic of the two measured numbers, not a quoted figure")

-- ── the lifetime trap ────────────────────────────────────────────────────
--
-- Counted through operator delete rather than operator new: both halves
-- allocate one frame, so allocation counts cannot tell them apart. The
-- question is whether anyone called destroy().

checks.expect_match(out["lifetime"], "abandoned alloc=1 free=0 leaked=yes",
  "cpp: an abandoned coroutine_handle leaks its frame -- allocated once, freed never")
checks.expect_match(out["lifetime"], "owned     alloc=1 free=1 leaked=no",
  "cpp: an owning wrapper calling destroy() in its destructor frees it exactly once")

-- ── F. clang parity, and the HALO contrast ───────────────────────────────

if tool_present("clang++") then
  local built = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target coro >/dev/null 2>&1 && echo built")
  checks.expect_match(built.out, "built", "cpp: clang builds the example")

  local cl_frames = checks.run("./cpp/build/release-clang/coro frames")
  checks.expect_exit(cl_frames, 0, "cpp: clang-built coro runs")
  checks.expect_match(cl_frames.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest")

  local cl_halo = checks.run("./cpp/build/release-clang/coro halo")
  local cl_allocs = field(cl_halo.out, "allocations")
  checks.expect_match(tostring(cl_allocs ~= nil and allocs ~= nil and cl_allocs <= allocs), "true",
    "cpp: clang elides at least as many frame allocations as GCC (clang=" ..
    tostring(cl_allocs) .. " gcc=" .. tostring(allocs) .. ") -- same source, same standard, " ..
    "different answer to a question the standard leaves open")
else
  print("SKIP: clang++ not found on PATH -- gate F not asserted")
end

checks.finish()
