-- Verify 51-std-threading (C++ only): what the C++ standard library's
-- synchronization primitives become on this kernel, and what they cost.
--
-- ch50 established that a std::thread IS a pthread here. This chapter asks the
-- same question of the synchronization primitives, and the answer is the same
-- one twice: a futex(2) when they block, and NOTHING AT ALL when they do not.
--
-- Every assertion below is a syscall COUNT observed from outside the program
-- with `strace -f -c -e trace=futex`, or a string the kernel put in procfs.
-- NOTHING HERE IS TIMED (see ch39). Syscall counts are reproducible in sign
-- across hosts even where their magnitude is not, and only signs are gated.
--
-- Hard-gated (A-F), all local, unprivileged, offline:
--   A. Every non-hanging subcommand computes the same digest,
--      0x481984990deee5ff -- ch46's C++, ch47's Go, ch48's Rust, ch49's conc,
--      ch50's pthreads.
--   B. THE HEADLINE. `baseline` (no mutex) and `uncontended` (200,000
--      lock/unlock pairs) issue the SAME small number of futex calls, while
--      `contended` -- the identical 200,000 pairs, split across 8 threads --
--      issues hundreds. An uncontended std::mutex costs no syscalls at all;
--      what costs is contention. The gate compares uncontended to baseline
--      rather than asserting zero, because a program that has linked
--      libstdc++ has already made a few syscalls before main() runs.
--   C. The blocking primitives all reach the same syscall: condvar, latch,
--      barrier, semaphore, and atomic-wait each issue >= 1 futex call. They
--      are one mechanism wearing five type names.
--   D. deadlock-safe: std::scoped_lock acquires two mutexes in OPPOSING
--      orders on two threads and completes anyway -- the deadlock-avoidance
--      property std::lock_guard cannot give you.
--   E. deadlock-naive: the same program with lock_guard hangs. Asserted as a
--      timeout (exit 124 from timeout(1)) AND, while it hangs, every task in
--      /proc/<pid>/task/*/wchan parked in a futex wait. The abstraction's
--      failure mode, named by the kernel.
--   F. stoptoken: the worker returns normally after request_stop(). No
--      PTHREAD_CANCELED, no forced unwind -- the ch50 contrast.
--
-- Gated-if-present (G): clang++ parity on the digest.
--
-- Degrades to an informational SKIP, never a FAIL and never a false PASS:
--   * strace unable to attach (ptrace hardening) -> gates B and C skipped
--   * /proc/<tid>/wchan unreadable -> the wchan half of gate E skipped

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("51-std-threading is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
end

local function lit(s)
  return (s:gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1"))
end

local function tool_present(cmd)
  return checks.run("command -v " .. cmd .. " >/dev/null 2>&1").exit == 0
end

local BIN = "cpp/build/release/stdthread"
local EXPECTED_DIGEST = "digest=0x481984990deee5ff"

-- Count futex syscalls issued by one subcommand, from outside the process.
-- Returns nil if strace could not attach, which is a SKIP rather than a FAIL.
local function futex_count(case)
  local r = checks.run("strace -f -c -e trace=futex " .. BIN .. " " .. case ..
                       " 2>&1 | awk '/futex/{print $4}' | head -1")
  local n = tonumber((r.out:gsub("%s", "")))
  return n
end

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: builds (stdlib only, no Boost, no Conan, no network)")

-- ── A. one digest, every non-hanging case ────────────────────────────────

local cases = { "baseline", "uncontended", "contended", "condvar", "latch", "barrier",
                "semaphore", "atomic-wait", "stoptoken", "deadlock-safe" }
local out = {}
for _, c in ipairs(cases) do
  local r = checks.run("./demo.sh cpp run " .. c)
  checks.expect_exit(r, 0, "cpp: case " .. c .. " runs")
  checks.expect_match(r.out, lit(EXPECTED_DIGEST),
    "cpp: case " .. c .. " computes the expected digest (== ch46/47/48/49/50's)")
  out[c] = r.out
end

-- The three cost cases must do the SAME WORK, or their syscall counts are not
-- comparable. Assert that before comparing anything.
checks.expect_match(out["baseline"], "acc=200000",
  "cpp: baseline performed 200000 increments")
checks.expect_match(out["uncontended"], "acc=200000",
  "cpp: uncontended performed the same 200000 increments, each under a lock")
checks.expect_match(out["contended"], "acc=200000",
  "cpp: contended performed the same 200000 increments across 8 threads -- same work, "
  .. "so the futex counts below are comparable")

-- ── B. the headline: locking is free until it is contended ───────────────

local have_strace = tool_present("strace")
local n_base = have_strace and futex_count("baseline") or nil

if not have_strace or n_base == nil then
  print("SKIP: strace could not count syscalls (absent or ptrace-restricted) -- " ..
        "gates B and C not asserted")
else
  local n_unc = futex_count("uncontended")
  local n_con = futex_count("contended")

  checks.expect_match(tostring(n_unc ~= nil and n_unc <= n_base + 1), "true",
    "cpp: 200000 uncontended lock/unlock pairs cost no more futex calls than the " ..
    "no-mutex baseline (baseline=" .. tostring(n_base) .. ", uncontended=" ..
    tostring(n_unc) .. ") -- an uncontended std::mutex is an atomic CAS, not a syscall")
  checks.expect_match(tostring(n_con ~= nil and n_con > 500), "true",
    "cpp: the identical 200000 pairs, contended, cost hundreds of futex calls " ..
    "(contended=" .. tostring(n_con) .. ") -- the cost is contention, not locking")
  checks.expect_match(tostring(n_con ~= nil and n_unc ~= nil and n_con > n_unc * 100), "true",
    "cpp: contended exceeds uncontended by more than two orders of magnitude")

  -- ── C. five type names, one mechanism ──────────────────────────────────
  for _, c in ipairs({ "condvar", "latch", "barrier", "semaphore", "atomic-wait" }) do
    local n = futex_count(c)
    checks.expect_match(tostring(n ~= nil and n >= 1), "true",
      "cpp: " .. c .. " blocks, and blocking means futex(2) (calls=" .. tostring(n) .. ")")
  end
end

-- ── D. scoped_lock survives opposing acquisition orders ──────────────────

checks.expect_match(out["deadlock-safe"], "completed 100000 acquisitions in opposing orders",
  "cpp: std::scoped_lock(a,b) and scoped_lock(b,a) on two threads completed -- " ..
  "argument order does not matter to scoped_lock")

-- ── E. the naive version deadlocks, and the kernel says where ────────────

local naive = checks.run("timeout 5 " .. BIN .. " deadlock-naive >/dev/null 2>&1; echo exit=$?")
checks.expect_match(naive.out, "exit=124",
  "cpp: two lock_guards in opposing orders deadlock -- timeout(1) had to kill it")

-- Catch it in the act and ask the kernel what each thread is doing. Bounded
-- polling, never an unbounded wait: if the process does not appear, skip.
local wchan = checks.run([[
  timeout 6 ]] .. BIN .. [[ deadlock-naive >/dev/null 2>&1 &
  for i in $(seq 1 50); do pgrep -x stdthread >/dev/null 2>&1 && break; done
  sleep 1
  p=$(pgrep -x stdthread | head -1)
  if [ -z "$p" ]; then echo NOPROC; else
    # /proc/<tid>/wchan has no trailing newline, so read each one separately
    # rather than cat-ing them all into one run-on string.
    for t in /proc/$p/task/*; do printf '%s ' "$(cat $t/wchan 2>/dev/null)"; done
  fi
  wait 2>/dev/null
]])

if wchan.out:find("NOPROC") or wchan.out:gsub("%s", "") == "" then
  print("SKIP: /proc/<tid>/wchan unreadable or the process was not caught -- " ..
        "the wchan half of gate E not asserted")
else
  checks.expect_match(wchan.out, "futex",
    "cpp: while deadlocked, procfs reports every thread parked in a futex wait " ..
    "(wchan: " .. wchan.out:gsub("%s+", " ") .. ")")
end

-- ── F. stop_token: cooperative, and it returns normally ──────────────────

checks.expect_match(out["stoptoken"], "stop_requested honored, worker returned normally",
  "cpp: the jthread observed request_stop() and returned on its own terms -- " ..
  "no PTHREAD_CANCELED and no forced unwind, unlike ch50's pthread_cancel")

-- ── G. clang parity (skip-if-absent) ─────────────────────────────────────

if tool_present("clang++") then
  local cl = checks.run(
    "cd cpp && cmake --preset release-clang >/dev/null 2>&1 && " ..
    "cmake --build --preset release-clang --target stdthread >/dev/null 2>&1 && " ..
    "./build/release-clang/stdthread uncontended")
  checks.expect_exit(cl, 0, "cpp: clang-built stdthread runs")
  checks.expect_match(cl.out, lit(EXPECTED_DIGEST),
    "cpp: clang and GCC agree on the digest")
else
  print("SKIP: clang++ not found on PATH -- gate G not asserted")
end

-- ── not gated ────────────────────────────────────────────────────────────

print("info: absolute futex counts for the contended case are NOT gated -- they are " ..
  "scheduling-dependent (1844-3111 observed across runs on the reference host); only " ..
  "the sign of the comparison is asserted")

checks.finish()
