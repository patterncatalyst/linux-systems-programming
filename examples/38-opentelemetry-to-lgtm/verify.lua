-- Verify lsp-otel (38-opentelemetry-to-lgtm) for LSP_LANG. A LOCAL-mode
-- example that requires the LGTM stack (podman container `lsp-lgtm`, OTLP on
-- :4318, Grafana on :3000). Unlike the vm examples this runs entirely on the
-- host: it builds the binary, runs `serve` in the background, drives N traced
-- requests through it, sends SIGINT so the SDK flushes and shuts down, then
-- proves the telemetry actually LANDED IN THE STACK by querying Prometheus
-- (metrics) and Tempo (traces) through Grafana's datasource proxy -- not by
-- trusting the program's own "I exported" claim.
--
-- The query surface: only Grafana (:3000, admin/admin) is published off the
-- otel-lgtm container; it proxies to the internal Prometheus (uid=prometheus,
-- metrics) and Tempo (uid=tempo, traces). Each language tags its telemetry
-- with a distinct service.name -- lsp-otel-cpp / lsp-otel-go / lsp-otel-rust
-- -- so the three languages' signals never collide in the shared stack and
-- each run is queried by its own service_name.
--
-- Asserts observable behavior, identical in shape across cpp/go/rust:
--   1. build exits 0.
--   2. error shapes: no args, `serve` without --port, `drive` --n 0 all exit
--      2 with the two-line usage; `drive` against a dead address exits 1 with
--      `app: error:`.
--   3. serve prints `app: serve listening on 127.0.0.1:PORT`; drive prints
--      `app: drive sent N/N ok` and exits 0; SIGINT makes serve print
--      `app: shutting down` and exit 0 (the SDK flush-then-shutdown path).
--   4. metrics reached Prometheus: requests_total{service_name=...} is >= N
--      (the counter the parent span increments once per request) and
--      request_duration_milliseconds_count{service_name=...} is >= N (the
--      histogram records one observation per request).
--   5. traces reached Tempo: at least one trace with rootServiceName=... and
--      rootTraceName=request (the parent span wrapping work+respond).
-- Metrics/traces are polled with a bounded retry because the SDK batches on a
-- ~2s interval and the collector/backends add their own ingest lag.
--
-- If the LGTM stack is not reachable the whole example SKIPs (per the runner
-- convention: a down stack is not a failing example), matching manifest.yaml
-- `requires: [lgtm]`.
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local example_dir = os.getenv("EXAMPLE_DIR") or "."
local grafana = "http://localhost:3000"
local svc = "lsp-otel-" .. lang
local port = 47380
local n = 10

-- Boolean assertion via the checks.lua pattern-match API (same helper as the
-- other chapters' verify.lua): match on success, force a labelled mismatch on
-- failure so it surfaces exactly like an expect_match failure.
local function expect_true(cond, label, detail)
  if cond then
    checks.expect_match("ok", "ok", label)
  else
    local msg = label
    if detail ~= nil then msg = msg .. " (" .. tostring(detail) .. ")" end
    checks.expect_match("no", "yes", msg)
  end
end

-- LGTM reachability: a down stack SKIPs the whole example, it does not fail.
local health = checks.run(
  "curl -s -o /dev/null -w '%{http_code}' -u admin:admin " .. grafana .. "/api/health")
if not health.out:match("200") then
  checks.skip("LGTM stack not reachable at " .. grafana .. " (requires the lsp-lgtm container)")
end

-- Local path of the freshly built binary for this language.
local bin_rel = ({
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
})[lang]
local bin = example_dir .. "/" .. bin_rel

-- ---------------------------------------------------------------------------
-- 1. build.
-- ---------------------------------------------------------------------------
local build = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(build, 0, lang .. ": demo.sh build exits 0")

-- ---------------------------------------------------------------------------
-- 2. error shapes.
-- ---------------------------------------------------------------------------
local no_args = checks.run(bin)
checks.expect_exit(no_args, 2, lang .. ": no args exits 2")
checks.expect_match(no_args.out, "usage: app serve", lang .. ": no args prints usage")

local serve_noport = checks.run(bin .. " serve")
checks.expect_exit(serve_noport, 2, lang .. ": serve without --port exits 2")

local drive_zero = checks.run(bin .. " drive --n 0 --addr 127.0.0.1:1")
checks.expect_exit(drive_zero, 2, lang .. ": drive --n 0 exits 2")

local drive_dead = checks.run(bin .. " drive --n 5 --addr 127.0.0.1:1")
checks.expect_exit(drive_dead, 1, lang .. ": drive against a dead address exits 1")
checks.expect_match(drive_dead.out, "app: error:", lang .. ": dead-address drive prints app: error:")

-- ---------------------------------------------------------------------------
-- 3-5. run serve+drive+flush, then confirm telemetry landed in the stack.
-- One shell script so the background serve, the drive, the SIGINT flush, and
-- the bounded metric/trace polling all share process state; it echoes simple
-- KEY=value tokens this file then parses.
-- ---------------------------------------------------------------------------
local q = grafana .. "/api/datasources/proxy/uid/prometheus/api/v1/query?query="
local tempo = grafana .. "/api/datasources/proxy/uid/tempo/api/search?tags=service.name%3D" .. svc .. "&limit=1"

-- URL-encoded PromQL: metric{service_name="lsp-otel-<lang>"}
local prom_req = "requests_total%7Bservice_name%3D%22" .. svc .. "%22%7D"
local prom_hist = "request_duration_milliseconds_count%7Bservice_name%3D%22" .. svc .. "%22%7D"

local script = table.concat({
  "set -u",
  "OUT=/tmp/lsp38_" .. lang .. ".$$",
  "SRV=$OUT.serve",
  bin .. " serve --port " .. port .. " >$SRV 2>&1 &",
  "SPID=$!",
  "for i in $(seq 1 50); do grep -q 'serve listening' $SRV 2>/dev/null && break; sleep 0.1; done",
  bin .. " drive --n " .. n .. " --addr 127.0.0.1:" .. port .. " >$OUT.drive 2>&1",
  "echo DRIVE_EXIT=$?",
  "cat $OUT.drive",
  "kill -INT $SPID 2>/dev/null",
  "if timeout 10 tail --pid=$SPID -f /dev/null 2>/dev/null; then wait $SPID; echo SERVE_EXIT=$?; else echo SERVE_EXIT=124; kill -9 $SPID 2>/dev/null; fi",
  "cat $SRV",
  -- poll Prometheus for the counter (bounded ~15s for batch + ingest lag)
  "REQ=; for i in $(seq 1 30); do",
  "  REQ=$(curl -s -u admin:admin '" .. q .. prom_req .. "' | python3 -c 'import sys,json;r=json.load(sys.stdin)[\"data\"][\"result\"];print(int(float(r[0][\"value\"][1])) if r else \"\")' 2>/dev/null)",
  "  [ -n \"$REQ\" ] && break; sleep 0.5; done",
  "echo PROM_REQUESTS=$REQ",
  "HCNT=$(curl -s -u admin:admin '" .. q .. prom_hist .. "' | python3 -c 'import sys,json;r=json.load(sys.stdin)[\"data\"][\"result\"];print(int(float(r[0][\"value\"][1])) if r else \"\")' 2>/dev/null)",
  "echo PROM_HIST_COUNT=$HCNT",
  -- poll Tempo for a trace
  "TR=0; for i in $(seq 1 20); do",
  "  TR=$(curl -s -u admin:admin '" .. tempo .. "' | python3 -c 'import sys,json;print(len(json.load(sys.stdin).get(\"traces\",[])))' 2>/dev/null)",
  "  [ \"$TR\" != \"0\" ] && [ -n \"$TR\" ] && break; sleep 0.5; done",
  "echo TEMPO_TRACES=$TR",
  "rm -f $OUT $OUT.serve $OUT.drive",
}, "\n")

local r = checks.run(script)

-- serve/drive observable behavior
checks.expect_match(r.out, "app: serve listening on 127%.0%.0%.1:" .. port,
  lang .. ": serve prints the listening line")
checks.expect_match(r.out, "app: drive sent " .. n .. "/" .. n .. " ok",
  lang .. ": drive reports N/N ok")
local drive_exit = r.out:match("DRIVE_EXIT=(%d+)")
expect_true(drive_exit == "0", lang .. ": drive exits 0", "DRIVE_EXIT=" .. tostring(drive_exit))
local serve_exit = r.out:match("SERVE_EXIT=(%d+)")
expect_true(serve_exit == "0",
  lang .. ": serve exits 0 after SIGINT (SDK flush-then-shutdown)", "SERVE_EXIT=" .. tostring(serve_exit))
checks.expect_match(r.out, "app: shutting down", lang .. ": serve prints the shutdown line")

-- metrics landed in Prometheus
local prom_req_v = tonumber(r.out:match("PROM_REQUESTS=(%d+)"))
expect_true(prom_req_v ~= nil and prom_req_v >= n,
  lang .. ": requests_total{service_name=" .. svc .. "} reached Prometheus and is >= " .. n,
  "PROM_REQUESTS=" .. tostring(r.out:match("PROM_REQUESTS=(%S*)")))
local prom_hist_v = tonumber(r.out:match("PROM_HIST_COUNT=(%d+)"))
expect_true(prom_hist_v ~= nil and prom_hist_v >= n,
  lang .. ": request_duration_milliseconds_count{service_name=" .. svc .. "} is >= " .. n,
  "PROM_HIST_COUNT=" .. tostring(r.out:match("PROM_HIST_COUNT=(%S*)")))

-- traces landed in Tempo
local tempo_v = tonumber(r.out:match("TEMPO_TRACES=(%d+)"))
expect_true(tempo_v ~= nil and tempo_v >= 1,
  lang .. ": at least one trace with service.name=" .. svc .. " reached Tempo",
  "TEMPO_TRACES=" .. tostring(r.out:match("TEMPO_TRACES=(%S*)")))

checks.finish()
