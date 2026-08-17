-- Verify 50-pthreads (C++ only): what POSIX threads expose that std::thread
-- does not, and what each control does that the kernel or glibc can be asked
-- to confirm. ch49 measured threads from above with std::thread; this chapter
-- goes under it to the pthread, and every assertion below checks a real,
-- program-observed effect -- a tid, a procfs string, an installed stack size,
-- a CPU id, a scheduling policy, an errno, or an abort message.
--
-- NOTHING HERE IS TIMED (see ch39). Nothing here needs root, a VM, a network,
-- or a second host.
--
-- Hard-gated (A-G):
--   A. Every facet computes the same digest, 0x481984990deee5ff -- the value
--      ch46's C++, ch47's Go, ch48's Rust, and ch49's conc all produce over
--      the same 16 payload bytes.
--   B. identity: the main thread's tid EQUALS the process pid (a process is
--      its first thread), worker tids are distinct from it and each other,
--      and /proc/self/task/<tid> exists.
--   C. naming: pthread_setname_np's name is readable back through BOTH
--      pthread_getname_np and /proc/self/task/<tid>/comm, and they agree; a
--      16-character name is refused with ERANGE and leaves the old name in
--      place (the kernel's comm field is 16 bytes INCLUDING the NUL).
--   D. stack: a thread created with a 256 KiB attr observes at least that
--      much and less than the 8 MiB default, and a request below
--      PTHREAD_STACK_MIN is refused with EINVAL rather than rounded up.
--   E. affinity: two threads pinned with pthread_setaffinity_np report two
--      DIFFERENT sched_getcpu() values while the calling thread's own mask is
--      unchanged. This is the ch49 contrast -- that chapter moved the whole
--      process, this one moves one thread. Skipped with an informational
--      SKIP (never a FAIL, never a false PASS) on a host allowed <2 CPUs.
--   F. sched: the default policy is SCHED_OTHER at priority 0, and an
--      unprivileged SCHED_FIFO request is refused with EPERM leaving the
--      policy unchanged. The REFUSAL is the gate -- asserting a successful
--      SCHED_FIFO would require root and would not run on a reader's laptop.
--   G. cancel, both directions:
--        - pthread_cancel unwinds: the C++ destructor AND the
--          pthread_cleanup_push handler both run, and pthread_join reports
--          PTHREAD_CANCELED.
--        - swallowing that unwind with a catch (...) that does not rethrow
--          aborts the whole process with "exception not rethrown". This is
--          why C++20 standardized cooperative std::stop_token instead.
--   H. bridge: std::thread::native_handle() is a pthread_t (checked by
--      static_assert at compile time) and naming a std::thread through it
--      lands in that thread's procfs comm.
--
-- Gated-if-present (I): clang++ parity on the digest, as ch49's gate G.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("50-pthreads is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
end

-- Escape a literal string for use inside a Lua pattern.
local function lit(s)
  return (s:gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1"))
end

local function tool_present(cmd)
  return checks.run("command -v " .. cmd .. " >/dev/null 2>&1").exit == 0
end

-- Pull a named integer field out of a facet's output.
local function field(out, name)
  local v = out:match(name .. "=(%-?%d+)")
  return v and tonumber(v) or nil
end

local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: builds (stdlib + pthreads only, no Boost, no network)")

-- ── A. one digest, every facet ───────────────────────────────────────────

local facets = { "identity", "naming", "stack", "affinity", "sched", "cancel", "bridge" }
local out = {}
for _, f in ipairs(facets) do
  local r = checks.run("./demo.sh cpp run " .. f)
  checks.expect_exit(r, 0, "cpp: facet " .. f .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: facet " .. f .. " computes the expected digest (== ch46/47/48/49's)")
  out[f] = r.out
end

-- ── B. identity: pthread_t is not a tid, and a process is its first thread ─

checks.expect_match(out["identity"], "same=yes",
  "cpp: the main thread's tid equals the process pid -- a process is its first thread")
checks.expect_match(out["identity"], "worker_tids_distinct=yes",
  "cpp: every worker got a distinct kernel tid, none of them the pid")
checks.expect_match(out["identity"], "main_task_dir=present",
  "cpp: the tid names a real directory under /proc/self/task")

-- ── C. naming: the name reaches the kernel, and 16 chars is one too many ──

checks.expect_match(out["naming"], "agree=yes",
  "cpp: pthread_getname_np and /proc/self/task/<tid>/comm report the same name")
checks.expect_match(out["naming"], "via_proc='ch50%-worker'",
  "cpp: the name set with pthread_setname_np is the one procfs reports")
checks.expect_match(out["naming"], "16char_rc=34 %(ERANGE%)",
  "cpp: a 16-character name is refused with ERANGE (comm is 16 bytes incl. NUL)")
checks.expect_match(out["naming"], "unchanged=yes",
  "cpp: the refused rename left the previous name in place -- a silent no-op if unchecked")

-- ── D. stack: std::thread has no say; pthreads does ───────────────────────

local st = out["stack"]
local custom = field(st, "custom")
local default_worker = field(st, "default_worker")
local requested = field(st, "requested")
checks.expect_match(st, "custom_at_least_requested=yes",
  "cpp: the thread created with a 256 KiB attr got at least 256 KiB (custom=" ..
    tostring(custom) .. ")")
checks.expect_match(st, "custom_smaller_than_default=yes",
  "cpp: and materially less than the default (default_worker=" ..
    tostring(default_worker) .. ")")
checks.expect_match(st, "below_min_rc=22 %(EINVAL%)",
  "cpp: a stack below PTHREAD_STACK_MIN is refused with EINVAL, not rounded up")

-- ── E. affinity: per THREAD, not per process (skip if <2 CPUs allowed) ────

local aff = out["affinity"]
local aff_allowed = field(aff, "main_cpus_allowed")
if aff:find("need at least 2 to pin two threads apart") then
  print("SKIP: fewer than 2 CPUs allowed -- gate E not asserted")
else
  checks.expect_match(aff, "workers_on_different_cpus=yes",
    "cpp: two threads pinned with pthread_setaffinity_np ran on two different CPUs")
  checks.expect_match(aff, "unchanged=yes",
    "cpp: pinning those threads left the caller's own affinity mask alone " ..
      "(main_cpus_allowed=" .. tostring(aff_allowed) .. ") -- unlike ch49's process-wide pin")
  local rc_a = aff:match("worker_a asked=%d+ rc=(%-?%d+)")
  checks.expect_match(tostring(rc_a), "^0$",
    "cpp: pthread_setaffinity_np itself succeeded for worker A")
end

-- ── F. sched: the refusal is the observable ──────────────────────────────

checks.expect_match(out["sched"], "is_other=yes",
  "cpp: the default scheduling policy is SCHED_OTHER")
checks.expect_match(out["sched"], "priority=0",
  "cpp: SCHED_OTHER requires sched_priority 0 -- 'priority' there means nice")
checks.expect_match(out["sched"], "SCHED_FIFO prio 50 %-> rc=1 %(EPERM%)",
  "cpp: an unprivileged SCHED_FIFO request is refused with EPERM")
checks.expect_match(out["sched"], "unchanged=yes",
  "cpp: the refused policy change left the thread on SCHED_OTHER")

-- ── G. cancel: it unwinds, and swallowing the unwind is fatal ─────────────

checks.expect_match(out["cancel"], "~Guard%(raii%) ran during unwinding",
  "cpp: pthread_cancel unwinds the stack -- the C++ destructor really ran")
checks.expect_match(out["cancel"], "pthread_cleanup handler ran",
  "cpp: the pthread_cleanup_push handler ran too")
checks.expect_match(out["cancel"], "joined_retval_is_canceled=yes",
  "cpp: pthread_join reported PTHREAD_CANCELED rather than a normal return")

-- The second case aborts on purpose, which is why it is its own subcommand.
local swallow = checks.run("./demo.sh cpp run cancel-swallow 2>&1")
checks.expect_match(tostring(swallow.exit ~= 0), "true",
  "cpp: swallowing the forced unwind kills the process (exit=" .. tostring(swallow.exit) .. ")")
checks.expect_match(swallow.out, "exception not rethrown",
  "cpp: glibc names the reason -- a catch (...) that does not rethrow is fatal here")
checks.expect_match(swallow.out, "caught the forced unwind and did not rethrow",
  "cpp: and the catch block really did run first, so this is not a crash before the catch")
checks.expect_match(tostring(swallow.out:find("still alive after swallowing") == nil), "true",
  "cpp: execution never reached the line after the join -- the abort is not recoverable")

-- ── H. the bridge: std::thread IS a pthread here ─────────────────────────

checks.expect_match(out["bridge"], "native_handle_is_pthread_t=yes",
  "cpp: std::thread::native_handle_type is pthread_t (static_assert, compile time)")
checks.expect_match(out["bridge"], "setname_rc=0",
  "cpp: naming a std::thread through its native handle succeeded")
checks.expect_match(out["bridge"], "proc_comm='std%-thread%-x'",
  "cpp: and procfs shows the name on the std::thread's own task")

-- ── I. clang parity (skip-if-absent) ─────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target pthreads >/dev/null 2>&1 && " ..
    "./build/release-clang/pthreads identity")
  checks.expect_exit(cl, 0, "cpp: clang-built pthreads runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest")
  checks.expect_match(cl.out, "same=yes",
    "cpp: and clang's build sees the same tid/pid identity")
else
  print("SKIP: clang++ not found on PATH -- gate I not asserted")
end

-- ── not gated: anything needing privilege ────────────────────────────────

print("info: a SUCCESSFUL SCHED_FIFO change is not gated -- it needs CAP_SYS_NICE or an" ..
  " RLIMIT_RTPRIO allowance; the EPERM refusal above is what an unprivileged run can prove")

checks.finish()
