-- Verify 44-go-runtime-for-systems (Go only).
--
-- Asserts the observable behaviour of the four Go-runtime themes: scheduler
-- (bounded-pool rendezvous, work distribution, async-preemption under
-- GOMAXPROCS=1), GC pacing (an exact forced-GC delta, then the GOGC/GOMEMLIMIT
-- effect on collection count), netpoller (thousands of goroutines blocked on
-- pipe reads with the OS thread count bounded), and knobs (effective
-- GOMAXPROCS/GOGC/GOMEMLIMIT/GODEBUG echo). The program computes every
-- comparison itself and prints a literal "... ok" / "... FAIL" verdict token;
-- this script matches the fixed label text plus the literal "ok", never a raw
-- fluctuating number. GODEBUG=gctrace/schedtrace are shown by the chapter,
-- not asserted here -- they are per-line noisy and not meant to be matched.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "go" then
  checks.skip("44-go-runtime-for-systems is Go-only (LSP_LANG=" .. tostring(lang) .. ")")
end

local b = checks.run("./demo.sh go build")
checks.expect_exit(b, 0, "go: builds")

local r = checks.run("./demo.sh go run all")
checks.expect_exit(r, 0, "go: run all exits 0")

-- (1) scheduler
checks.expect_match(r.out,
  "sched: rendezvous workers=8 baseline=%d+ during=%d+ during>=baseline%+workers ok",
  "sched: rendezvous proves >=8 concurrently-alive goroutines")
checks.expect_match(r.out,
  "sched: workpool workers=8 jobs=64 done=64 done==jobs ok",
  "sched: worker pool processes every job exactly once")
checks.expect_match(r.out,
  "sched: preempt forced_gomaxprocs=1 spinner_ops=2000000000 ticker_incr=%d+ ticker_incr>0 ok",
  "sched: async preemption lets the ticker goroutine progress under GOMAXPROCS=1")
checks.expect_match(r.out, "sched: PASS", "sched: theme verdict PASS")

-- (2) GC pacing
checks.expect_match(r.out,
  "gc: forced numgc_before=%d+ numgc_after=%d+ delta=1 delta==1 ok",
  "gc: one explicit runtime.GC() call advances NumGC by exactly 1")
checks.expect_match(r.out,
  "gc: gogc100_cycles=%d+ gogc800_cycles=%d+ gogc800<gogc100 ok",
  "gc: looser GOGC=800 triggers fewer cycles than GOGC=100 for the same churn")
checks.expect_match(r.out,
  "gc: memlimit_unset_cycles=%d+ memlimit_tight_cycles=%d+ memlimit_tight>memlimit_unset ok",
  "gc: an 8MiB GOMEMLIMIT triggers more cycles than no limit")
checks.expect_match(r.out, "gc: PASS", "gc: theme verdict PASS")

-- (3) netpoller
checks.expect_match(r.out,
  "netpoll: parked goroutines=2000 baseline=%d+ total=%d+ total==baseline%+2000 ok",
  "netpoll: 2000 goroutines blocked on pipe reads all exist concurrently")
checks.expect_match(r.out,
  "netpoll: parked threads=%d+ bound=%d+ threads<=bound ok",
  "netpoll: OS thread count stays bounded while 2000 goroutines are parked")
checks.expect_match(r.out, "netpoll: PASS", "netpoll: theme verdict PASS")

-- (4) knobs (structural, not value-based)
checks.expect_match(r.out, "knobs: GOMAXPROCS=%d+ NumCPU=%d+",
  "knobs: reports effective GOMAXPROCS/NumCPU")
checks.expect_match(r.out, "knobs: PASS", "knobs: theme verdict PASS")

checks.expect_match(r.out, "ALL: PASS", "overall verdict PASS")

checks.finish()
