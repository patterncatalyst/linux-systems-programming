-- Verify sysagent v0 (36-proc-sys-and-the-agent) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. usage errors: no args / unknown subcommand / serve without --port all
--      exit 2 with a usage line on stderr.
--   2. `sample --json` emits every expected key with a numeric value, in the
--      deterministic cross-language schema (README.md), with sane ranges.
--   3. A cpu-busy child measurably raises cpu_util_pct vs an idle baseline —
--      compared with a robust margin and a couple of retries, since this
--      runs on a real, possibly-noisy host rather than an isolated lab VM.
--   4. `serve --port P` answers GET /metrics with the same JSON shape, 404s
--      an unknown path, and its mem_total_kb agrees exactly with a direct
--      /proc/meminfo read (a static fact untouched by system load); SIGINT
--      shuts it down cleanly (exit 0).
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set; the interpreter may be lua or luajit.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

-- ---------------------------------------------------------------------------
-- 1. error shapes
-- ---------------------------------------------------------------------------

local noargs = checks.run(demo)
checks.expect_exit(noargs, 2, lang .. ": no arguments exits 2")
checks.expect_match(noargs.out, "usage: sysagent", lang .. ": no arguments prints usage")

local badsub = checks.run(demo .. " frobnicate")
checks.expect_exit(badsub, 2, lang .. ": unknown subcommand exits 2")

local noport = checks.run(demo .. " serve")
checks.expect_exit(noport, 2, lang .. ": serve without --port exits 2")

-- ---------------------------------------------------------------------------
-- 2. `sample --json` schema + sane ranges
-- ---------------------------------------------------------------------------

local sample = checks.run(demo .. " sample --json --interval-ms 200")
checks.expect_exit(sample, 0, lang .. ": sample --json exits 0")

local function expect_number_key(key, label)
  local v = sample.out:match('"' .. key .. '":(%-?%d+%.?%d*)')
  if v == nil then
    checks.expect_match(sample.out, '"' .. key .. '":NEVER_MATCHES', label .. " (key present)")
  else
    checks.expect_match(tostring(tonumber(v) ~= nil), "true", label .. " (numeric)")
  end
end

for _, key in ipairs({
  "cpu_util_pct", "cpu_user_pct", "cpu_system_pct",
  "load1", "load5", "load15", "runnable", "total_threads",
  "mem_total_kb", "mem_available_kb", "mem_used_kb",
  "psi_cpu_some_avg10", "psi_mem_some_avg10", "psi_io_some_avg10",
}) do
  expect_number_key(key, lang .. ": sample --json has \"" .. key .. "\"")
end

checks.expect_match(sample.out, '"psi_available":%a+', lang .. ": sample --json has psi_available bool")
checks.expect_match(sample.out, '"disks":%[.*"name":"[%w_%-/]+".-"reads":%d+.-"writes":%d+',
  lang .. ": disks array has a named entry with reads/writes")
checks.expect_match(sample.out, '"net":%[.-"iface":"lo".-"rx_bytes":%d+.-"tx_bytes":%d+',
  lang .. ": net array includes loopback rx/tx")

local mem_total = tonumber(sample.out:match('"mem_total_kb":(%d+)'))
local util_pct = tonumber(sample.out:match('"cpu_util_pct":([%d%.]+)'))
checks.expect_match(tostring(mem_total ~= nil and mem_total > 0), "true",
  lang .. ": mem_total_kb is a positive number")
checks.expect_match(tostring(util_pct ~= nil and util_pct >= 0 and util_pct <= 100.01), "true",
  lang .. ": cpu_util_pct is within [0,100]")

-- ---------------------------------------------------------------------------
-- 3. a cpu-busy child raises cpu_util_pct vs an idle baseline
-- ---------------------------------------------------------------------------

local ncpu = tonumber(checks.run("nproc").out:match("%d+")) or 4

local function busy_vs_idle()
  local script = string.format([[
IDLE=$(%s sample --json --interval-ms 300 | grep -o '"cpu_util_pct":[0-9.]*' | cut -d: -f2)
echo "IDLE_PCT=$IDLE"
for i in $(seq %d); do
  timeout 2 bash -c 'while :; do :; done' &
done
sleep 0.3
BUSY=$(%s sample --json --interval-ms 500 | grep -o '"cpu_util_pct":[0-9.]*' | cut -d: -f2)
echo "BUSY_PCT=$BUSY"
wait 2>/dev/null
]], demo, ncpu, demo)
  local r = checks.run(script)
  return tonumber(r.out:match("IDLE_PCT=([%d%.]+)")), tonumber(r.out:match("BUSY_PCT=([%d%.]+)"))
end

-- Retried because this runs on a real, possibly shared host: the margin is
-- generous (12 points) and there's a high-absolute-utilization escape hatch
-- (55%) for the case where the host is already so busy the idle baseline
-- itself is elevated.
local margin_ok, last_idle, last_busy = false, nil, nil
for _attempt = 1, 3 do
  local idle, busy = busy_vs_idle()
  last_idle, last_busy = idle, busy
  if idle and busy and (busy - idle >= 12 or busy >= 55) then
    margin_ok = true
    break
  end
end
print(string.format("cpu-busy check: idle=%s busy=%s margin_ok=%s",
  tostring(last_idle), tostring(last_busy), tostring(margin_ok)))
checks.expect_match(margin_ok and "yes" or "no", "yes",
  lang .. ": a cpu-busy child raises cpu_util_pct vs idle (margin>=12pp or busy>=55%)")

-- ---------------------------------------------------------------------------
-- 4. `serve` — /metrics answers, 404s elsewhere, agrees with a direct
--    /proc read, and shuts down cleanly on SIGINT.
-- ---------------------------------------------------------------------------

local port = checks.run(
  "python3 -c 'import socket; s=socket.socket(); s.bind((\"127.0.0.1\",0)); " ..
  "print(s.getsockname()[1]); s.close()'").out:gsub("%s+$", "")
if not port:match("^%d+$") then
  checks.skip("could not allocate a TCP port")
end

local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end

local function sh(script)
  return checks.run((script:gsub("@WD@", wd):gsub("@PORT@", port):gsub("@DEMO@", demo)))
end

local session = sh([[
set -u
@DEMO@ serve --port @PORT@ > @WD@/srv.out 2>&1 &
SRV=$!
for _ in $(seq 1 200); do ss -ltn 2>/dev/null | grep -q ":@PORT@ " && break; sleep 0.05; done

echo "===BODY==="
curl -s http://127.0.0.1:@PORT@/metrics
echo
echo "===ENDBODY==="

echo "CODE404=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:@PORT@/nope)"
echo "DIRECT_MEMTOTAL=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)"

kill -INT "$SRV"
for _ in $(seq 1 200); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.05; done
if kill -0 "$SRV" 2>/dev/null; then kill -9 "$SRV"; echo "server_exit=timeout"; else
  wait "$SRV"; echo "server_exit=$?"
fi
echo "===SERVER==="; cat @WD@/srv.out
]])

checks.expect_match(session.out, "CODE404=404", lang .. ": serve 404s an unknown path")

local body = session.out:match("===BODY===\n(.-)\n===ENDBODY===")
checks.expect_match(tostring(body ~= nil and body:match('"cpu_util_pct"') ~= nil), "true",
  lang .. ": /metrics answers with the JSON snapshot")

local served_mem_total = body and tonumber(body:match('"mem_total_kb":(%d+)'))
local direct_mem_total = tonumber(session.out:match("DIRECT_MEMTOTAL=(%d+)"))
checks.expect_match(
  tostring(served_mem_total ~= nil and direct_mem_total ~= nil and served_mem_total == direct_mem_total),
  "true", lang .. ": /metrics mem_total_kb agrees exactly with a direct /proc/meminfo read")

local disk_name = body and body:match('"name":"([%w_%-/]+)"')
if disk_name then
  local found = checks.run("grep -q '" .. disk_name .. "' /proc/diskstats && echo yes || echo no")
  checks.expect_match(found.out, "yes",
    lang .. ": a disk name from /metrics (" .. disk_name .. ") is present in /proc/diskstats")
end

checks.expect_match(session.out, "sysagent: listening on 0%.0%.0%.0:" .. port,
  lang .. ": server announces the listen address")
checks.expect_match(session.out, "sysagent: shutting down",
  lang .. ": server logs the shutdown")
checks.expect_match(session.out, "server_exit=0",
  lang .. ": SIGINT shuts the server down cleanly and it exits 0")

checks.run("rm -rf " .. wd)
checks.finish()
