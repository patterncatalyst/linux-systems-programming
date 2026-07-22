-- Verify loadmix (37-use-method-60s-analysis) for LSP_LANG. A vm-mode
-- example: driven directly against systems-target via plain scp/ssh to
-- /tmp/lsp37-<lang>/app, never `demo.sh <lang> run` under TARGET --
-- deploy-to-vm.sh's `ssh -t` injects CRLF that breaks Lua patterns, and its
-- fixed /home/fedora/app path can be poisoned by a stale binary left over
-- from an unrelated example (this happened once during golden capture).
-- Every remote invocation below uses the full absolute remote_bin path --
-- never a leading `cd` that a later `;`-separated command could silently
-- fail to inherit.
--
-- Asserts observable behavior, identical in shape across cpp/go/rust:
--   1. build, then stage to /tmp/lsp37-<lang>/app -- both must exit 0.
--   2. error shapes: no args / bad --resource / --seconds 0 / bad analyze
--      flag all exit 2 with the two-line usage on stderr.
--   3. saturate cpu/io/net (and mem) at a short --seconds: exit 0, the
--      start/done line shapes, the documented worker-count formula against
--      the guest's real nproc, ops>0 (mem excepted -- the golden capture
--      found ops legitimately 0 at a short duration, since allocation alone
--      can consume the whole budget), and the elapsed wall time actually
--      tracks --seconds for cpu/io/net -- this is exactly what would have
--      caught the pre-fix saturator bug where sat_* exited in milliseconds
--      regardless of the requested duration. mem gets a generous (x5)
--      ceiling instead of a tight window, since it can legitimately
--      *overrun* --seconds by several seconds under real swap pressure
--      (the inner touch loop only checks the stop flag between passes, per
--      the golden-capture calibration notes).
--   4. idle `analyze --seconds N`: the exact skeleton -- 1 start line, 5
--      tool lines, 8 metric lines (each %d+%.%d%d), 12 signal lines in
--      cpu/mem/io/net x Utilization/Saturation/Errors order, 1 verdict
--      line, and "analyze: done" as the last line of the app's own output.
--   5. load attribution: a background `--resource cpu` saturator makes a
--      concurrent `analyze` see busy_pct and run_queue rise, Utilization
--      fire, and the verdict land on cpu -- retried up to 3 attempts
--      against a generous margin (calibrated from 3 real captures showing
--      busy_pct 1.34->100.00 and run_queue 3.00->7.00-8.00, both clearing
--      the gate by a large margin).
--   6. wall-clock sanity: `analyze --seconds N` finishes in close to N
--      seconds, not 5N -- proving the five interval samplers
--      (vmstat/mpstat/iostat/sar/pidstat) run concurrently, not serially.
--
-- Never compares formatted decimals across languages (C++, Go, and Rust
-- disagree on halfway rounding) -- only shapes (%d+%.%d%d) and numeric
-- ranges/ratios are asserted.
--
-- Invoked from the example directory with LSP_LANG, TARGET, and REPO_ROOT
-- set (EXAMPLE_DIR optional, defaults to ".").

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local target = os.getenv("TARGET")
if not target or target == "" then
  checks.skip("no TARGET set -- this is a vm example (run with --mode vm)")
end

local example_dir = os.getenv("EXAMPLE_DIR") or "."
local home = os.getenv("HOME") or "/root"

-- Local path of the freshly built binary for this language.
local bin_rel = ({
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
})[lang]
local local_bin = example_dir .. "/" .. bin_rel

-- Resolve the guest IP (same helper deploy-to-vm.sh uses).
local ipf = io.popen(repo_root .. "/scripts/lab/vm-ip.sh '" .. target .. "' 2>/dev/null")
local ip = ipf and ipf:read("*l") or nil
if ipf then ipf:close() end
if not ip or ip == "" then
  checks.skip("cannot resolve IP for VM '" .. target .. "' (lab down?)")
end

local ssh_opts =
  "-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=" .. home .. "/.ssh/known_hosts"

-- Run a (single-quote-free) remote script through ssh.
local function ssh_run(remote_script)
  return checks.run(string.format("ssh %s 'fedora@%s' '%s'", ssh_opts, ip, remote_script))
end

-- Boolean assertion via the checks.lua pattern-match API: match "ok" against
-- "ok" when true, or force a mismatch (with the real detail in the label)
-- when false -- same helper as example 39's verify.lua.
local function expect_true(cond, label, detail)
  if cond then
    checks.expect_match("ok", "ok", label)
  else
    local msg = label
    if detail ~= nil then msg = msg .. " (" .. tostring(detail) .. ")" end
    checks.expect_match("no", "yes", msg)
  end
end

local remote_dir = "/tmp/lsp37-" .. lang
local remote_bin = remote_dir .. "/app"

-- ---------------------------------------------------------------------------
-- 1. build, then stage to /tmp/lsp37-<lang>/app -- named "app" so /proc comm
--    is "app", the book-wide rule.
-- ---------------------------------------------------------------------------

local build = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(build, 0, lang .. ": demo.sh build exits 0")

local stage_cmd = string.format(
  "ssh %s 'fedora@%s' 'mkdir -p %s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "ssh %s 'fedora@%s' 'chmod 755 %s'",
  ssh_opts, ip, remote_dir,
  ssh_opts, local_bin, ip, remote_bin,
  ssh_opts, ip, remote_bin)
local staged = checks.run(stage_cmd)
checks.expect_exit(staged, 0, lang .. ": stage app binary to " .. remote_dir .. " on the guest")

-- The guest's real core count drives the worker-count formulas below.
local nproc_r = ssh_run("nproc")
local nproc = tonumber(nproc_r.out:match("%d+")) or 1

-- ---------------------------------------------------------------------------
-- 2. error shapes -- exit 2, usage on stderr.
-- ---------------------------------------------------------------------------

local function expect_usage(args, label)
  local r = ssh_run(remote_bin .. " " .. args)
  checks.expect_exit(r, 2, lang .. ": " .. label .. " exits 2")
  checks.expect_match(r.out, "usage: app %-%-resource", lang .. ": " .. label .. " prints usage")
end

expect_usage("", "no args")
expect_usage("--resource bogus --seconds 5", "--resource bogus --seconds 5")
expect_usage("--resource cpu --seconds 0", "--resource cpu --seconds 0")
expect_usage("analyze --bogus", "analyze --bogus")

-- ---------------------------------------------------------------------------
-- 3. saturate mode -- cpu/io/net (tight elapsed window) plus mem (generous
--    ceiling, no ops>0 requirement), per the golden-capture calibration.
-- ---------------------------------------------------------------------------

local function workers_for(res, cpus)
  if res == "mem" then return math.max(2, cpus) end
  if res == "cpu" then return math.max(4, cpus * 3) end
  return math.max(4, cpus * 4) -- io, net
end

-- Run `--resource <res> --seconds <secs>` remotely, timed with date +%s%N
-- wrapped tightly around just the app invocation (not the ssh handshake).
local function saturate(res, secs)
  local script = table.concat({
    "OUT=/tmp/lsp37_sat_" .. res .. ".$$",
    "t0=$(date +%s%N)",
    remote_bin .. " --resource " .. res .. " --seconds " .. secs .. " >$OUT 2>&1",
    "RC=$?",
    "t1=$(date +%s%N)",
    "cat $OUT",
    "rm -f $OUT",
    "echo SAT_EXIT=$RC",
    "echo SAT_ELAPSED_MS=$(( (t1 - t0) / 1000000 ))",
  }, "\n")
  return ssh_run(script)
end

for _, res in ipairs({ "cpu", "io", "net" }) do
  local secs = 4
  local r = saturate(res, secs)

  local sat_exit = tonumber(r.out:match("SAT_EXIT=(%d+)"))
  expect_true(sat_exit == 0, lang .. ": --resource " .. res .. " --seconds " .. secs .. " exits 0",
    tostring(sat_exit))

  local workers = tonumber(r.out:match(
    "loadmix: start resource=" .. res .. " seconds=" .. secs .. " workers=(%d+)"))
  expect_true(workers ~= nil, lang .. ": " .. res .. " prints loadmix: start with workers=N")

  local want_workers = workers_for(res, nproc)
  expect_true(workers == want_workers,
    lang .. ": " .. res .. " workers matches the documented formula for nproc=" .. nproc,
    string.format("got workers=%s want=%d", tostring(workers), want_workers))

  local done_ops, done_bytes = r.out:match(
    "loadmix: done resource=" .. res .. " seconds=" .. secs .. " workers=%d+ ops=(%d+) bytes=(%d+)")
  done_ops, done_bytes = tonumber(done_ops), tonumber(done_bytes)
  expect_true(done_ops ~= nil and done_bytes ~= nil,
    lang .. ": " .. res .. " prints loadmix: done with ops= and bytes=")
  expect_true(done_ops ~= nil and done_ops > 0,
    lang .. ": " .. res .. " ops > 0", tostring(done_ops))

  local elapsed_ms = tonumber(r.out:match("SAT_ELAPSED_MS=(%d+)"))
  -- cpu/io/net track --seconds tightly on this guest (within ~0.2s per the
  -- golden capture); a +/-2-4s window keeps this tight enough to have
  -- caught the pre-fix bug (which exited in single-digit milliseconds)
  -- while tolerating a slightly noisier guest.
  expect_true(elapsed_ms ~= nil and elapsed_ms >= (secs * 1000 - 1500) and elapsed_ms <= (secs * 1000 + 4000),
    lang .. ": " .. res .. " elapsed tracks --seconds=" .. secs ..
      " (catches the pre-fix instant-exit saturator bug)",
    tostring(elapsed_ms) .. "ms")
end

do
  local mem_secs = 3
  local r = saturate("mem", mem_secs)

  local sat_exit = tonumber(r.out:match("SAT_EXIT=(%d+)"))
  expect_true(sat_exit == 0,
    lang .. ": --resource mem --seconds " .. mem_secs .. " exits 0 (does not OOM the guest)",
    tostring(sat_exit))

  local workers = tonumber(r.out:match(
    "loadmix: start resource=mem seconds=" .. mem_secs .. " workers=(%d+)"))
  local want_workers = workers_for("mem", nproc)
  expect_true(workers == want_workers,
    lang .. ": mem workers matches the documented formula (max(2, nproc))",
    string.format("got workers=%s want=%d", tostring(workers), want_workers))

  local mem_ops, mem_bytes = r.out:match(
    "loadmix: done resource=mem seconds=" .. mem_secs .. " workers=%d+ ops=(%d+) bytes=(%d+)")
  mem_ops, mem_bytes = tonumber(mem_ops), tonumber(mem_bytes)
  expect_true(mem_ops ~= nil and mem_bytes ~= nil,
    lang .. ": mem prints loadmix: done with ops= and bytes=")
  -- ops is deliberately NOT required to be > 0 here: the golden capture
  -- found ops legitimately 0 at a short --seconds, since the initial buffer
  -- allocation alone can consume the whole budget before any touch pass
  -- completes. bytes (the ~1.35xMemTotal target) is the real sanity check.
  expect_true(mem_bytes ~= nil and mem_bytes > 1024 * 1024 * 1024,
    lang .. ": mem bytes= is the ~1.35xMemTotal target (>1GiB)", tostring(mem_bytes))

  local elapsed_ms = tonumber(r.out:match("SAT_ELAPSED_MS=(%d+)"))
  -- mem can OVERRUN --seconds by several seconds under real swap pressure
  -- (a 12.6s actual for an 8s request was observed) -- a generous x5
  -- ceiling, not the tight cpu/io/net window.
  expect_true(elapsed_ms ~= nil and elapsed_ms <= mem_secs * 5000,
    lang .. ": mem finishes within a generous ceiling (secondsx5), tolerating the known swap overrun",
    tostring(elapsed_ms) .. "ms")
end

-- ---------------------------------------------------------------------------
-- 4 & 6. idle `analyze --seconds N`: exact line skeleton, plus wall-clock
--        sanity (samplers run concurrently, not serially).
-- ---------------------------------------------------------------------------

local analyze_secs = 8
local analyze_script = table.concat({
  "OUT=/tmp/lsp37_analyze.$$",
  "t0=$(date +%s%N)",
  remote_bin .. " analyze --seconds " .. analyze_secs .. " >$OUT 2>&1",
  "RC=$?",
  "t1=$(date +%s%N)",
  "cat $OUT",
  "rm -f $OUT",
  "echo AN_EXIT=$RC",
  "echo AN_ELAPSED_MS=$(( (t1 - t0) / 1000000 ))",
}, "\n")
local ar = ssh_run(analyze_script)

local an_exit = tonumber(ar.out:match("AN_EXIT=(%d+)"))
expect_true(an_exit == 0, lang .. ": analyze --seconds " .. analyze_secs .. " exits 0", tostring(an_exit))

checks.expect_match(ar.out, "analyze: start seconds=" .. analyze_secs .. " cpus=%d+",
  lang .. ": analyze prints the start line")
checks.expect_match(ar.out, "analyze: tool pidstat active_processes=%d+",
  lang .. ": analyze prints the pidstat tool line")
checks.expect_match(ar.out, "analyze: tool dmesg status=%a+",
  lang .. ": analyze prints the dmesg status line")

for _, m in ipairs({
  "resource=cpu name=busy_pct value=%d+%.%d%d unit=pct",
  "resource=cpu name=run_queue value=%d+%.%d%d unit=procs",
  "resource=mem name=used_pct value=%d+%.%d%d unit=pct",
  "resource=mem name=swap_io value=%d+%.%d%d unit=kbps",
  "resource=io name=util_pct value=%d+%.%d%d unit=pct",
  "resource=io name=iowait_pct value=%d+%.%d%d unit=pct",
  "resource=io name=await_ms value=%d+%.%d%d unit=ms",
  "resource=net name=pkts value=%d+%.%d%d unit=per_s",
}) do
  checks.expect_match(ar.out, "analyze: metric " .. m, lang .. ": analyze metric " .. m)
end

-- All 12 signal lines, in cpu/mem/io/net x Utilization/Saturation/Errors
-- order -- one pattern enforces both presence and order via lazy ".-" gaps.
local signal_order = {}
for _, res in ipairs({ "cpu", "mem", "io", "net" }) do
  table.insert(signal_order,
    "analyze: signal resource=" .. res .. " type=Utilization fired=%a+ value=%d+%.%d%d threshold=%d+%.%d%d")
  table.insert(signal_order,
    "analyze: signal resource=" .. res .. " type=Saturation fired=%a+ value=%d+%.%d%d threshold=%d+%.%d%d")
  table.insert(signal_order,
    "analyze: signal resource=" .. res .. " type=Errors fired=false")
end
checks.expect_match(ar.out, table.concat(signal_order, ".-"),
  lang .. ": all 12 signal lines present, in cpu/mem/io/net x Utilization/Saturation/Errors order")

checks.expect_match(ar.out, "analyze: verdict resource=%a+ ratio=%d+%.%d%d",
  lang .. ": analyze prints the verdict line")
-- "analyze: done" is the last line of the app's OWN output (our injected
-- AN_EXIT=/AN_ELAPSED_MS= lines follow it in the captured buffer).
checks.expect_match(ar.out, "analyze: done\nAN_EXIT=%d+",
  lang .. ": analyze: done is the last line of the app's output")

local an_elapsed_ms = tonumber(ar.out:match("AN_ELAPSED_MS=(%d+)"))
expect_true(an_elapsed_ms ~= nil and an_elapsed_ms < analyze_secs * 5000,
  lang .. ": analyze --seconds " .. analyze_secs .. " finishes well under 5x" .. analyze_secs ..
    "s -- proves the 5 interval samplers run concurrently, not serially",
  tostring(an_elapsed_ms) .. "ms")

-- ---------------------------------------------------------------------------
-- 5. load attribution: a background --resource cpu saturator makes a
--    concurrent analyze attribute the load to cpu. Retried up to 3 attempts
--    (per the plan's own hedge, mirroring example 36's cpu-busy check) --
--    the golden capture cleared this gate by a large margin every time
--    (busy_pct 100.00 vs. 60 threshold; run_queue 7-8 vs. nproc+2=4), so a
--    noisy guest should still pass, not produce a false red.
-- ---------------------------------------------------------------------------

local function load_attribution_attempt()
  local bg_secs, sleep_before, an_secs = 15, 2, 10
  local script = table.concat({
    "BGOUT=/tmp/lsp37_bg.$$",
    "ANOUT=/tmp/lsp37_an.$$",
    "nohup " .. remote_bin .. " --resource cpu --seconds " .. bg_secs .. " >$BGOUT 2>&1 &",
    "BGPID=$!",
    "sleep " .. sleep_before,
    remote_bin .. " analyze --seconds " .. an_secs .. " >$ANOUT 2>&1",
    "ANEXIT=$?",
    "wait $BGPID",
    "BGEXIT=$?",
    "cat $ANOUT",
    "echo AN_EXIT=$ANEXIT",
    "cat $BGOUT",
    "echo BG_EXIT=$BGEXIT",
    "rm -f $BGOUT $ANOUT",
  }, "\n")
  return ssh_run(script)
end

local attributed = false
local last_busy, last_rq, last_util_fired, last_verdict = nil, nil, nil, nil
for attempt = 1, 3 do
  local r = load_attribution_attempt()
  local an_exit2 = tonumber(r.out:match("AN_EXIT=(%d+)"))
  local bg_exit = tonumber(r.out:match("BG_EXIT=(%d+)"))
  local busy = tonumber(r.out:match("resource=cpu name=busy_pct value=(%d+%.%d%d)"))
  local rq = tonumber(r.out:match("resource=cpu name=run_queue value=(%d+%.%d%d)"))
  local util_fired = r.out:match("resource=cpu type=Utilization fired=(%a+)")
  local verdict_res = r.out:match("verdict resource=(%a+)")
  last_busy, last_rq, last_util_fired, last_verdict = busy, rq, util_fired, verdict_res

  print(string.format(
    "%s: load-attribution attempt %d: an_exit=%s bg_exit=%s busy_pct=%s run_queue=%s util_fired=%s verdict=%s",
    lang, attempt, tostring(an_exit2), tostring(bg_exit), tostring(busy), tostring(rq),
    tostring(util_fired), tostring(verdict_res)))

  if an_exit2 == 0 and bg_exit == 0 and busy and rq
     and (busy >= 60 or rq >= nproc + 2)
     and util_fired == "true" and verdict_res == "cpu" then
    attributed = true
    break
  end
end
expect_true(attributed,
  lang .. ": under concurrent --resource cpu load, analyze attributes it " ..
    "(busy_pct>=60 or run_queue>=nproc+2, Utilization fired=true, verdict=cpu) within 3 attempts",
  string.format("last: busy_pct=%s run_queue=%s util_fired=%s verdict=%s",
    tostring(last_busy), tostring(last_rq), tostring(last_util_fired), tostring(last_verdict)))

-- ---------------------------------------------------------------------------
-- Cleanup.
-- ---------------------------------------------------------------------------

checks.run(string.format("ssh %s 'fedora@%s' 'rm -rf %s'", ssh_opts, ip, remote_dir))

checks.finish()
