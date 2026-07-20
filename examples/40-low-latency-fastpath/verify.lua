-- Verify chatterd-fastpath (40-low-latency-fastpath) for LSP_LANG. A vm-mode
-- example: driven directly against systems-target via plain scp/ssh to
-- /tmp/lsp40-<lang>/, never `demo.sh <lang> run` under TARGET --
-- deploy-to-vm.sh's `ssh -t` injects CRLF that breaks Lua patterns, and its
-- fixed /home/fedora/app path can be poisoned by a stale binary left over
-- from an unrelated example (this happened once during golden capture).
-- Every remote invocation below uses the full absolute remote_bin /
-- remote_driver path -- never a leading `cd` relied on to apply across a
-- later `;`-separated command in the same string. Every multi-statement
-- remote script here is joined with "\n", never "; " -- a line ending in
-- `&` (backgrounding) followed by `; ` produces `cmd & ; next`, a bash
-- syntax error that silently makes the whole remote script a no-op parse
-- (a real bug hit while writing example 37's verify.lua).
--
-- Asserts observable behavior, identical in shape across cpp/go/rust:
--   1. build, then stage app + run-latency-bench.sh to /tmp/lsp40-<lang>/,
--      chmod 755 both -- both must exit 0.
--   2. error shapes: no args / frobnicate / fastpath without --pin / naive
--      without --port / measure without --n all exit 2 with the 3-line
--      "usage: app fastpath ..." usage text.
--   3. fastpath --pin <nproc+50> (out of range): exit 1,
--      "app: error: cpu N out of range".
--   4. measure connect-refused (127.0.0.1:1) and measure against a non-IPv4
--      host: both exit 1, with "app: error: connect" and
--      "not an IPv4 address" respectively.
--   5. the real run-latency-bench.sh (N=20000, WARMUP=500): exit 0, and its
--      output proves --pin actually restricts scheduling AT THE KERNEL
--      LEVEL (read from /proc/<pid>/status by the driver script, not the
--      program's own claim) -- fastpath-cpus-allowed is exactly the single
--      pinned CPU while naive-cpus-allowed is the full online set.
--   6. all six percentiles_ns lines (naive-1..3, fastpath-1..3) parse, each
--      with n=20000, a monotonic min<=p50<=p90<=p99<=p99.9<=max, a mean that
--      parses as a float, and every value > 0.
--   7. median-of-three per variant: median(fastpath p50) < median(naive p50)
--      * 0.90 -- a conservative 10% requirement against an observed ~30-33%
--      margin across three independent golden-capture bench runs (1.425x -
--      1.488x, never marginal or inverted) -- both medians are always
--      printed so a marginal run is diagnosable from the log even when this
--      assertion passes.
--   8. SIGINT shutdown, tested separately per server with a bounded
--      (10s) wait via `timeout 10 tail --pid=$PID -f /dev/null` so a hung
--      server surfaces as a clear FAIL rather than a runner-level timeout --
--      this is the assertion Go is most likely to fail, since Go's runtime
--      installs SA_RESTART handlers and a blocking read will not see EINTR
--      on its own. naive --port P: "app: naive shutting down", exit 0.
--      fastpath --port P --pin 0 --busy-poll: the exact listening line
--      "app: fastpath listening on 0.0.0.0:P pinned-cpu=0 busy-poll=on",
--      "app: fastpath shutting down", exit 0.
--
-- Never compares formatted decimals across languages (C++, Go, and Rust
-- disagree on halfway rounding) -- only shapes and numeric ranges/ratios are
-- asserted; percentile values (p50 etc.) are plain integers so they ARE
-- compared numerically across the naive/fastpath medians, but never against
-- a fixed expected number.
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
local local_driver = example_dir .. "/run-latency-bench.sh"

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
-- when false -- same helper as examples 37/39's verify.lua.
local function expect_true(cond, label, detail)
  if cond then
    checks.expect_match("ok", "ok", label)
  else
    local msg = label
    if detail ~= nil then msg = msg .. " (" .. tostring(detail) .. ")" end
    checks.expect_match("no", "yes", msg)
  end
end

local remote_dir = "/tmp/lsp40-" .. lang
local remote_bin = remote_dir .. "/app"
local remote_driver = remote_dir .. "/run-latency-bench.sh"

-- ---------------------------------------------------------------------------
-- 1. build, then stage app + run-latency-bench.sh to /tmp/lsp40-<lang>/,
--    chmod 755 both.
-- ---------------------------------------------------------------------------

local build = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(build, 0, lang .. ": demo.sh build exits 0")

local stage_cmd = string.format(
  "ssh %s 'fedora@%s' 'mkdir -p %s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "scp %s '%s' 'fedora@%s:%s' && " ..
  "ssh %s 'fedora@%s' 'chmod 755 %s %s'",
  ssh_opts, ip, remote_dir,
  ssh_opts, local_bin, ip, remote_bin,
  ssh_opts, local_driver, ip, remote_driver,
  ssh_opts, ip, remote_bin, remote_driver)
local staged = checks.run(stage_cmd)
checks.expect_exit(staged, 0, lang .. ": stage app + run-latency-bench.sh to " .. remote_dir)

-- The guest's real core count drives the out-of-range --pin value below.
local nproc_r = ssh_run("nproc")
local nproc = tonumber(nproc_r.out:match("%d+")) or 1

-- ---------------------------------------------------------------------------
-- 2. error shapes -- exit 2, "usage: app fastpath ..." on stderr.
-- ---------------------------------------------------------------------------

local function expect_usage(args, label)
  local r = ssh_run(remote_bin .. " " .. args)
  checks.expect_exit(r, 2, lang .. ": " .. label .. " exits 2")
  checks.expect_match(r.out, "usage: app fastpath", lang .. ": " .. label .. " prints usage")
end

expect_usage("", "no args")
expect_usage("frobnicate", "frobnicate")
expect_usage("fastpath --port 9999", "fastpath --port 9999 (no --pin)")
expect_usage("naive", "naive (no port)")
expect_usage("measure --target 127.0.0.1:9999", "measure --target ... (no --n)")

-- ---------------------------------------------------------------------------
-- 3. fastpath --pin out of range -- exit 1, cpu N out of range.
-- ---------------------------------------------------------------------------

local bad_cpu = nproc + 50
local pin_r = ssh_run(remote_bin .. " fastpath --port 19700 --pin " .. bad_cpu)
checks.expect_exit(pin_r, 1, lang .. ": fastpath --pin " .. bad_cpu .. " (out of range) exits 1")
checks.expect_match(pin_r.out, "app: error: cpu %d+ out of range",
  lang .. ": out-of-range --pin prints the cpu-out-of-range error")

-- ---------------------------------------------------------------------------
-- 4. measure error shapes -- connect refused, non-IPv4 host.
-- ---------------------------------------------------------------------------

local connect_r = ssh_run(remote_bin .. " measure --target 127.0.0.1:1 --n 10")
checks.expect_exit(connect_r, 1, lang .. ": measure --target 127.0.0.1:1 (connect refused) exits 1")
checks.expect_match(connect_r.out, "app: error: connect",
  lang .. ": measure connect-refused prints the connect error")

local ipv4_r = ssh_run(remote_bin .. " measure --target nothost:9999 --n 5")
checks.expect_exit(ipv4_r, 1, lang .. ": measure --target nothost:9999 (not IPv4) exits 1")
checks.expect_match(ipv4_r.out, "not an IPv4 address",
  lang .. ": measure against a non-IPv4 host reports not-an-IPv4-address")

-- ---------------------------------------------------------------------------
-- 5, 6, 7. the real run-latency-bench.sh: kernel-level pinning proof, all
-- six percentiles_ns lines parse, and median(fastpath) beats median(naive).
-- ---------------------------------------------------------------------------

local bench_script = table.concat({
  "OUT=/tmp/lsp40_bench_" .. lang .. ".$$",
  "bash " .. remote_driver .. " " .. remote_bin .. " 20000 500 >$OUT 2>&1",
  "RC=$?",
  "cat $OUT",
  "rm -f $OUT",
  "echo BENCH_EXIT=$RC",
}, "\n")
local br = ssh_run(bench_script)
local bench_exit = tonumber(br.out:match("BENCH_EXIT=(%d+)"))
expect_true(bench_exit == 0,
  lang .. ": run-latency-bench.sh /tmp/lsp40-" .. lang .. "/app 20000 500 exits 0",
  tostring(bench_exit))

-- 5. Kernel-level pinning proof (phase A of the driver script), read from
-- /proc/<pid>/status by the driver -- not the program's own printed claim.
local naive_allowed = br.out:match("app: naive%-cpus%-allowed (%S+)")
local fast_allowed = br.out:match("app: fastpath%-cpus%-allowed (%S+)")

local want_naive_allowed = (nproc > 1) and ("0-" .. (nproc - 1)) or "0"
expect_true(naive_allowed == want_naive_allowed,
  lang .. ": naive-cpus-allowed is the full online CPU set (untouched affinity mask)",
  string.format("got=%s want=%s", tostring(naive_allowed), want_naive_allowed))
expect_true(fast_allowed ~= nil and fast_allowed:match("^%d+$") ~= nil,
  lang .. ": fastpath-cpus-allowed is a single CPU value (no range, no comma) -- " ..
    "the Go port is most likely to fail this",
  tostring(fast_allowed))

-- 6. All six percentiles_ns lines: n=20000, monotonic percentiles, mean
-- parses as a float, every value > 0.
local samples = { naive = {}, fastpath = {} }
local pct_pattern =
  "app: percentiles_ns tag=(%a+)%-(%d+) p50=(%d+) p90=(%d+) p99=(%d+) p99%.9=(%d+) " ..
  "min=(%d+) max=(%d+) mean=(%d+%.%d+) n=(%d+)"
local matched = 0
for variant, trial, p50, p90, p99, p999, minv, maxv, mean, n in br.out:gmatch(pct_pattern) do
  matched = matched + 1
  p50, p90, p99, p999 = tonumber(p50), tonumber(p90), tonumber(p99), tonumber(p999)
  minv, maxv, n = tonumber(minv), tonumber(maxv), tonumber(n)
  local mean_n = tonumber(mean)
  local label = lang .. ": " .. variant .. "-" .. trial .. " percentiles_ns"

  expect_true(n == 20000, label .. " has n=20000", "n=" .. tostring(n))
  expect_true(minv ~= nil and minv > 0 and p50 >= minv and p90 >= p50 and p99 >= p90 and
      p999 >= p99 and maxv >= p999,
    label .. " is monotonic (min<=p50<=p90<=p99<=p99.9<=max)",
    string.format("min=%s p50=%s p90=%s p99=%s p99.9=%s max=%s",
      tostring(minv), tostring(p50), tostring(p90), tostring(p99), tostring(p999), tostring(maxv)))
  expect_true(mean_n ~= nil and mean_n > 0, label .. " mean parses as a positive float", tostring(mean))

  if variant == "naive" or variant == "fastpath" then
    table.insert(samples[variant], p50)
  end
end
expect_true(matched == 6,
  lang .. ": exactly 6 percentiles_ns lines present (naive-1..3, fastpath-1..3)",
  "matched=" .. tostring(matched))

-- 7. Median-of-three per variant, conservative threshold, both medians
-- always printed (per the golden-capture calibration: observed ratio
-- 1.425x-1.488x across 3 independent runs, so a 0.90 factor -- a 10%
-- requirement -- has comfortable headroom against a noisier CI run without
-- being a tautology).
local function median3(t)
  if #t ~= 3 then return nil end
  local s = { t[1], t[2], t[3] }
  table.sort(s)
  return s[2]
end
local naive_med = median3(samples.naive)
local fast_med = median3(samples.fastpath)
print(string.format("%s: median(naive p50)=%s ns  median(fastpath p50)=%s ns",
  lang, tostring(naive_med), tostring(fast_med)))
expect_true(naive_med ~= nil and fast_med ~= nil and fast_med < naive_med * 0.90,
  lang .. ": median(fastpath p50) < median(naive p50) * 0.90",
  string.format("naive_median=%s fastpath_median=%s", tostring(naive_med), tostring(fast_med)))

-- ---------------------------------------------------------------------------
-- 8. SIGINT shutdown, tested separately per server, with a bounded (10s)
-- wait so a Go server that hangs on a blocking read (SA_RESTART, no EINTR)
-- surfaces as a clear FAIL rather than a runner-level timeout.
-- ---------------------------------------------------------------------------

local function sigint_test(args, port)
  local script = table.concat({
    "OUT=/tmp/lsp40_sigint_" .. lang .. "_" .. port .. ".$$",
    remote_bin .. " " .. args .. " >$OUT 2>&1 &",
    "PID=$!",
    "FOUND=0",
    "for i in $(seq 1 50); do",
    "  if (exec 3<>\"/dev/tcp/127.0.0.1/" .. port .. "\") 2>/dev/null; then",
    "    exec 3<&- 2>/dev/null",
    "    exec 3>&- 2>/dev/null",
    "    FOUND=1",
    "    break",
    "  fi",
    "  sleep 0.1",
    "done",
    "if [ \"$FOUND\" != \"1\" ]; then",
    "  kill -9 $PID 2>/dev/null",
    "  cat $OUT",
    "  rm -f $OUT",
    "  echo SIGINT_EXIT=port_never_opened",
    "  exit 0",
    "fi",
    "kill -INT $PID",
    "if timeout 10 tail --pid=$PID -f /dev/null 2>/dev/null; then",
    "  wait $PID",
    "  RC=$?",
    "else",
    "  RC=124",
    "  kill -9 $PID 2>/dev/null",
    "  wait $PID 2>/dev/null",
    "fi",
    "cat $OUT",
    "rm -f $OUT",
    "echo SIGINT_EXIT=$RC",
  }, "\n")
  return ssh_run(script)
end

-- naive: "app: naive shutting down", exit 0.
do
  local r = sigint_test("naive --port 19801", 19801)
  local rc = r.out:match("SIGINT_EXIT=(%S+)")
  expect_true(rc == "0",
    lang .. ": naive exits 0 after SIGINT (bounded 10s wait, catches a Go SA_RESTART hang)",
    "SIGINT_EXIT=" .. tostring(rc))
  checks.expect_match(r.out, "app: naive shutting down",
    lang .. ": naive prints the shutdown line on SIGINT")
end

-- fastpath --busy-poll: the exact listening line, then
-- "app: fastpath shutting down", exit 0.
do
  local r = sigint_test("fastpath --port 19802 --pin 0 --busy-poll", 19802)
  local rc = r.out:match("SIGINT_EXIT=(%S+)")
  expect_true(rc == "0",
    lang .. ": fastpath --busy-poll exits 0 after SIGINT (bounded 10s wait, catches a Go SA_RESTART hang)",
    "SIGINT_EXIT=" .. tostring(rc))
  checks.expect_match(r.out, "app: fastpath listening on 0%.0%.0%.0:%d+ pinned%-cpu=0 busy%-poll=on",
    lang .. ": fastpath prints the exact listening line (pinned-cpu=0 busy-poll=on)")
  checks.expect_match(r.out, "app: fastpath shutting down",
    lang .. ": fastpath prints the shutdown line on SIGINT")
end

-- ---------------------------------------------------------------------------
-- 9. Cleanup.
-- ---------------------------------------------------------------------------

checks.run(string.format("ssh %s 'fedora@%s' 'rm -rf %s'", ssh_opts, ip, remote_dir))

checks.finish()
