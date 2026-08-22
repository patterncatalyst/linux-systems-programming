-- Verify 55-boost-asio (C++ only): what a library adds on top of a reactor.
--
-- ch09 built an epoll loop by hand, ch22 wrote the nonblocking/EAGAIN state
-- machine, ch27 drove a hand-rolled coroutine reactor over epoll. Those three
-- own the readiness model and NOTHING here repeats it. This example is about
-- the concepts Asio has that a hand-written loop does not.
--
-- NOTHING HERE IS TIMED (see ch39) and nothing binds a port -- the I/O cases
-- use an AF_UNIX socketpair, so the example is local, offline, unprivileged.
--
-- Hard-gated (A-F):
--   A. Every case computes the same digest, 0x481984990deee5ff -- ch46-ch54.
--   B. Work guard: a fresh io_context::run() returns after ZERO handlers,
--      because run() returns on "no outstanding work" rather than on
--      completion. With an executor_work_guard held it runs the posted
--      handler instead. The single most common Asio surprise, as two counts.
--   C. THE STRAND. The same handler, the same 8 threads, posted two ways:
--        * through a strand -> the unsynchronized counter is EXACTLY right,
--          max_inflight is 1, and overlaps is 0
--        * straight to the io_context -> handlers demonstrably overlap
--      Note carefully what is and is not asserted on the unguarded side. The
--      counter there is a data race, which is UB, and the number of lost
--      updates is not reproducible -- a run that loses nothing is legal. So
--      the gate asserts OVERLAP, which is measured with atomics and is well
--      defined, and merely REPORTS the counter. (ch49's rule: never gate on
--      the output of undefined behavior.)
--   D. A strand is not a mutex. Both produce the correct 20000, but the mutex
--      BLOCKS a thread on contention while the strand DEFERS a handler and
--      returns the thread to the pool. Counted with ch51's method
--      (strace -f -c -e trace=futex). Gated as a ratio, never as magnitudes.
--      Degrades to an informational SKIP if strace cannot attach.
--   E. Scatter-gather: four buffers written as one SEQUENCE cost exactly one
--      socket write-family syscall; the same four written separately cost
--      four. Deterministic counts, and the same readv/writev machinery Part 5
--      covered. The gate counts sendmsg+sendto+writev and ignores plain
--      write(2), which is this program's own stdout.
--   F. Topologies: who actually runs the handlers, by gettid() -- ch50's
--      instrument and ch49's vocabulary applied to an I/O runtime. One thread
--      on one context reports exactly 1 tid; the three multi-threaded shapes
--      report more than 1.
--
-- Gated-if-present (G): clang++ parity on the digest.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("55-boost-asio is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
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

local BIN = "cpp/build/release/asiodemo"
local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: builds against system Boost.Asio (header-only + boost_system)")

-- ── A. one digest, every case ────────────────────────────────────────────

local cases = { "versions", "work-guard", "strand", "nostrand", "strand-cost", "mutex-cost",
                "gather", "separate", "cancel", "topology-one", "topology-pool",
                "topology-percore", "topology-threadpool" }
local out = {}
for _, c in ipairs(cases) do
  local r = checks.run("timeout 180 " .. BIN .. " " .. c)
  checks.expect_exit(r, 0, "cpp: case " .. c .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: case " .. c .. " computes the expected digest (== ch46-ch54's)")
  out[c] = r.out
end

-- ── B. the work guard ────────────────────────────────────────────────────

checks.expect_match(out["work-guard"], "without_work_handlers=0",
  "cpp: a fresh io_context::run() returns after ZERO handlers -- it returns on no " ..
  "outstanding work, not on completion")
checks.expect_match(out["work-guard"], "guard_kept_it_alive=yes",
  "cpp: an executor_work_guard keeps run() alive until the guard is released")

-- ── C. the strand ────────────────────────────────────────────────────────

local st = out["strand"]
checks.expect_match(st, "correct=yes",
  "cpp: through a strand, an UNSYNCHRONIZED counter incremented by 8 threads is exactly " ..
  "right (counter=" .. tostring(field(st, "counter")) .. ")")
checks.expect_match(st, "max_inflight=1",
  "cpp: never more than one strand handler in flight, across 8 threads calling run()")
checks.expect_match(st, "overlaps=0",
  "cpp: and zero overlapping entries -- serialization without a mutex")

local ns = out["nostrand"]
local ns_overlaps = field(ns, "overlaps")
local ns_inflight = field(ns, "max_inflight")
checks.expect_match(tostring(ns_overlaps ~= nil and ns_overlaps > 0), "true",
  "cpp: the SAME handler posted straight to the io_context overlaps (" ..
  tostring(ns_overlaps) .. " overlapping entries, max_inflight=" .. tostring(ns_inflight) ..
  ") -- only the executor changed")
-- The lost-increment count is REPORTED by the program and deliberately not
-- asserted here: an unsynchronized increment is a data race, so losing zero on
-- some run is legal and gating on the magnitude would be gating on UB.
print("info: the unguarded counter's lost-update count is NOT gated -- an unsynchronized " ..
  "increment is a data race and therefore UB, so a run that loses nothing is legal. " ..
  "The gate asserts OVERLAP, which is measured with atomics and well defined.")

-- ── D. a strand is not a mutex ───────────────────────────────────────────

checks.expect_match(out["strand-cost"], "correct=yes",
  "cpp: the strand arm produced the correct 20000")
checks.expect_match(out["mutex-cost"], "correct=yes",
  "cpp: the mutex arm produced the correct 20000 too -- both are CORRECT")

local function futex_count(case)
  local r = checks.run("strace -f -c -e trace=futex " .. BIN .. " " .. case ..
                       " 2>&1 | awk '/futex/{print $4}' | head -1")
  return tonumber((r.out:gsub("%s", "")))
end

if not tool_present("strace") then
  print("SKIP: strace not found -- gate D (strand vs mutex futex cost) not asserted")
else
  local n_strand = futex_count("strand-cost")
  local n_mutex = futex_count("mutex-cost")
  if n_strand == nil or n_mutex == nil then
    print("SKIP: strace could not count syscalls (ptrace-restricted?) -- gate D not asserted")
  else
    checks.expect_match(tostring(n_strand < n_mutex / 2), "true",
      "cpp: the strand reached the kernel far less often than the mutex for identical work " ..
      "(strand=" .. tostring(n_strand) .. " futex calls, mutex=" .. tostring(n_mutex) ..
      ") -- a mutex BLOCKS a thread, a strand DEFERS a handler")
  end
end

-- ── E. scatter-gather ────────────────────────────────────────────────────
--
-- Count only socket-directed writes. Plain write(2) is this program's own
-- stdout and the runtime's internal pipe, and including it would make the
-- comparison meaningless.

local function socket_writes(case)
  local r = checks.run("strace -f -c -e trace=writev,sendmsg,sendto " .. BIN .. " " .. case ..
                       " 2>&1 | awk '/writev|sendmsg|sendto/{s+=$4} END{print s+0}'")
  return tonumber((r.out:gsub("%s", "")))
end

if not tool_present("strace") then
  print("SKIP: strace not found -- gate E (scatter-gather syscall counts) not asserted")
else
  local n_gather = socket_writes("gather")
  local n_separate = socket_writes("separate")
  checks.expect_match(tostring(n_gather == 1), "true",
    "cpp: four buffers written as ONE sequence cost exactly one socket write-family " ..
    "syscall (got " .. tostring(n_gather) .. ") -- the kernel's iovec form, reached " ..
    "without assembling one by hand")
  checks.expect_match(tostring(n_separate == 4), "true",
    "cpp: the identical four buffers written separately cost four (got " ..
    tostring(n_separate) .. ")")
  checks.expect_match(out["gather"], "intact=yes",
    "cpp: and the peer read back exactly the bytes that were written")
end

-- ── F. topologies ────────────────────────────────────────────────────────

checks.expect_match(out["topology-one"], "distinct_tids=1",
  "cpp: one thread calling run() on one io_context ran every handler itself")

for _, shape in ipairs({ "pool", "percore", "threadpool" }) do
  local o = out["topology-" .. shape]
  local n = field(o, "distinct_tids")
  checks.expect_match(tostring(n ~= nil and n > 1), "true",
    "cpp: topology-" .. shape .. " spread handlers across " .. tostring(n) ..
    " OS threads -- same handlers, different shape of run()")
end

-- ── cancellation: a completion, not an exception or a flag ───────────────

checks.expect_match(out["cancel"], "aborted=yes",
  "cpp: a cancelled operation completes with operation_aborted -- the handler still runs " ..
  "exactly once, so ownership rules never change")
checks.expect_match(out["cancel"], "handlers_run=1",
  "cpp: and a one-hour timer's handler ran immediately rather than being dropped")

-- ── G. clang parity (skip-if-absent) ─────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target asiodemo >/dev/null 2>&1 && " ..
    "timeout 180 ./build/release-clang/asiodemo strand")
  checks.expect_exit(cl, 0, "cpp: clang-built asiodemo runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest")
  checks.expect_match(cl.out, "overlaps=0",
    "cpp: and clang's build serializes the strand identically")
else
  print("SKIP: clang++ not found on PATH -- gate G not asserted")
end

checks.finish()
