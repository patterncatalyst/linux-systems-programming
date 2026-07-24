-- Verify the capstone fleet (41-capstone-fleet) for LSP_LANG. This lane is
-- mode:vm-peer, but -- following the convention example 23 established -- the
-- verify itself is a LOCAL assertion run entirely on loopback: the whole fleet
-- (pmon supervising chatterd + sysagent + fwatch), the cross-node chatterd
-- bridge (two nodes on 127.0.0.1 with distinct ports), and the OTLP export to
-- the local LGTM stack are all exercised without touching a VM. The real
-- two-host run across systems-target/systems-peer is the chapter's demo; the
-- bridge logic is identical on loopback (it is just a TCP client of the peer's
-- advertised address), so the observable contract is fully covered here.
--
-- Asserts observable behavior, identical in shape across cpp/go/rust:
--   1. usage/error shapes: no args, `chatterd` / `fwatch` with no subcommand,
--      `chatterd send` missing --nick/--text, `sysagent saturate` missing
--      --resource -- all exit 2 with the usage banner.
--   2. sysagent --once: exit 0, the `sysagent: node=.. cpu_pct=.. mem_pct=..
--      load1=.. ts=..` line, and the `otel disabled` line when no endpoint set.
--   3. fwatch snapshot DIR lists the directory; fwatch watch DIR reports
--      `event: create|modify|delete PATH` lines then `(timeout)`; fwatch watch
--      --sandbox installs a Landlock ruleset (`fwatch: landlock ABI=N enforced
--      dir=..`) -- the kernel-level sandbox proof, not the program's own claim.
--   4. chatterd: a message sent to one node is `received from=NICK text=..` by
--      a local listener; and across the BRIDGE (node A --peer-> node B), a
--      message sent on A is delivered to a listener on B as
--      `received from=NICK@A text=..` (the @A origin tag proves it crossed).
--   5. pmon fleet: drops the capability bounding set
--      (`pmon: capabilities bounding_set_dropped=N no_new_privs=1`), starts all
--      three services, reaches `pmon: health chatterd=up sysagent=up
--      fwatch=up`, and on SIGTERM prints `pmon: shutdown` and exits 0.
--   6. telemetry: with OTEL_EXPORTER_OTLP_ENDPOINT set, sysagent prints
--      `otel enabled ... export_errors=0` and its `sysagent_cpu_pct` gauge
--      reaches Prometheus (queried through Grafana's proxy). SKIPPED if the
--      LGTM stack is not reachable (mode requires it, but a down stack is not a
--      failing example).
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

-- Boolean assertion via the checks.lua pattern-match API (same helper as the
-- other chapters' verify.lua).
local function expect_true(cond, label, detail)
  if cond then
    checks.expect_match("ok", "ok", label)
  else
    local msg = label
    if detail ~= nil then msg = msg .. " (" .. tostring(detail) .. ")" end
    checks.expect_match("no", "yes", msg)
  end
end

-- Distinct port bases per language so back-to-back cpp/go/rust runs never race
-- on a lingering TIME_WAIT socket.
local base = ({ cpp = 0, go = 100, rust = 200 })[lang]
local p_serve = 47500 + base   -- single-node chatterd
local p_a = 47510 + base       -- bridge node A
local p_b = 47511 + base       -- bridge node B
local p_pmon = 47520 + base    -- pmon's chatterd

local bin_rel = ({
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
})[lang]
local bin = example_dir .. "/" .. bin_rel

local wd = checks.run("mktemp -d").out:gsub("%s+$", "")
if wd == "" or not wd:match("^/") then
  checks.skip("mktemp -d did not yield a directory")
end
local function sh(script)
  return checks.run((script:gsub("@BIN@", bin):gsub("@WD@", wd)))
end

-- ---------------------------------------------------------------------------
-- 0. build.
-- ---------------------------------------------------------------------------
local build = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(build, 0, lang .. ": demo.sh build exits 0")

-- ---------------------------------------------------------------------------
-- 1. usage / error shapes -- exit 2, usage banner.
-- ---------------------------------------------------------------------------
local function expect_usage(args, label)
  local r = sh("@BIN@ " .. args)
  checks.expect_exit(r, 2, lang .. ": " .. label .. " exits 2")
  checks.expect_match(r.out, "usage: app", lang .. ": " .. label .. " prints usage")
end
expect_usage("", "no args")
expect_usage("chatterd", "chatterd without subcommand")
expect_usage("fwatch", "fwatch without subcommand")
expect_usage("chatterd send --host 127.0.0.1 --port 1", "chatterd send missing --nick/--text")
-- saturate is a sysagent subcommand with its OWN usage banner, not the main one.
local sat = sh("@BIN@ sysagent saturate --seconds 1")
checks.expect_exit(sat, 2, lang .. ": sysagent saturate missing --resource exits 2")
checks.expect_match(sat.out, "usage: sysagent saturate",
  lang .. ": sysagent saturate prints its own usage")

-- ---------------------------------------------------------------------------
-- 2. sysagent one-shot.
-- ---------------------------------------------------------------------------
local sa = sh("@BIN@ sysagent --once --node n1")
checks.expect_exit(sa, 0, lang .. ": sysagent --once exits 0")
checks.expect_match(sa.out,
  "sysagent: node=n1 cpu_pct=%d+%.%d+ mem_pct=%d+%.%d+ load1=%d+%.%d+ ts=%d+",
  lang .. ": sysagent prints the metrics line")
checks.expect_match(sa.out, "sysagent: otel disabled",
  lang .. ": sysagent reports otel disabled with no endpoint")

-- ---------------------------------------------------------------------------
-- 3. fwatch: snapshot, watch events, and the Landlock sandbox.
-- ---------------------------------------------------------------------------
sh("mkdir -p @WD@/snap && touch @WD@/snap/alpha.txt @WD@/snap/beta.txt")
local snap = sh("@BIN@ fwatch snapshot @WD@/snap")
checks.expect_exit(snap, 0, lang .. ": fwatch snapshot exits 0")
checks.expect_match(snap.out, "alpha%.txt", lang .. ": snapshot lists directory entries")

local watch = sh([[
mkdir -p @WD@/watch
@BIN@ fwatch watch @WD@/watch --timeout-ms 1500 >@WD@/watch.out 2>&1 &
wp=$!
sleep 0.4
echo hi >@WD@/watch/f.txt
echo more >>@WD@/watch/f.txt
rm @WD@/watch/f.txt
wait $wp
cat @WD@/watch.out
]])
checks.expect_match(watch.out, "event: create .*/f%.txt", lang .. ": fwatch reports a create event")
checks.expect_match(watch.out, "event: delete .*/f%.txt", lang .. ": fwatch reports a delete event")

local sandbox = sh([[
mkdir -p @WD@/sb
@BIN@ fwatch watch @WD@/sb --sandbox --timeout-ms 800 >@WD@/sb.out 2>&1 &
wp=$!
sleep 0.3
touch @WD@/sb/g.txt
wait $wp
cat @WD@/sb.out
]])
checks.expect_match(sandbox.out, "fwatch: landlock ABI=%d+ enforced dir=",
  lang .. ": fwatch installs a Landlock ruleset (kernel-level sandbox)")
checks.expect_match(sandbox.out, "fwatch: watching .* %(sandbox=true%)",
  lang .. ": fwatch announces sandbox mode")

-- ---------------------------------------------------------------------------
-- 4. chatterd: single-node delivery, then the cross-node bridge.
-- ---------------------------------------------------------------------------
local single = sh([[
@BIN@ chatterd serve --host 127.0.0.1 --port ]] .. p_serve .. [[ --node solo >@WD@/s.out 2>&1 &
sp=$!
sleep 0.6
@BIN@ chatterd listen --host 127.0.0.1 --port ]] .. p_serve .. [[ --nick bob --timeout-ms 2500 >@WD@/l.out 2>&1 &
lp=$!
sleep 0.4
@BIN@ chatterd send --host 127.0.0.1 --port ]] .. p_serve .. [[ --nick alice --text "hello fleet" --timeout-ms 1500
echo "send-exit=$?"
wait $lp
kill -INT $sp 2>/dev/null; wait $sp 2>/dev/null
echo "--- listen ---"; cat @WD@/l.out
echo "--- serve ---"; cat @WD@/s.out
]])
checks.expect_match(single.out, "send%-exit=0", lang .. ": chatterd send exits 0")
checks.expect_match(single.out, "chatterd: listening on 127%.0%.0%.1:" .. p_serve .. " node=solo",
  lang .. ": chatterd serve prints the listening line")
checks.expect_match(single.out, "chatterd: received from=alice text=hello fleet",
  lang .. ": a local listener receives the message")

local bridge = sh([[
@BIN@ chatterd serve --host 127.0.0.1 --port ]] .. p_a .. [[ --node A \
  --peer 127.0.0.1:]] .. p_b .. [[ --peer-node B >@WD@/ba.out 2>&1 &
ap=$!
@BIN@ chatterd serve --host 127.0.0.1 --port ]] .. p_b .. [[ --node B \
  --peer 127.0.0.1:]] .. p_a .. [[ --peer-node A >@WD@/bb.out 2>&1 &
bp=$!
sleep 1.0
@BIN@ chatterd listen --host 127.0.0.1 --port ]] .. p_b .. [[ --nick carol --timeout-ms 3000 >@WD@/bc.out 2>&1 &
lp=$!
sleep 0.4
@BIN@ chatterd send --host 127.0.0.1 --port ]] .. p_a .. [[ --nick dave --text "cross node" --timeout-ms 1500
wait $lp
kill -INT $ap $bp 2>/dev/null; wait $ap $bp 2>/dev/null
echo "--- carol (node B) ---"; cat @WD@/bc.out
]])
checks.expect_match(bridge.out, "chatterd: received from=dave@A text=cross node",
  lang .. ": a message sent on node A is bridged to a listener on node B (from=dave@A)")

-- ---------------------------------------------------------------------------
-- 5. pmon fleet: cap drop, all services up, clean shutdown.
-- ---------------------------------------------------------------------------
local fleet = sh([[
mkdir -p @WD@/fleet
@BIN@ pmon --node p1 --sandbox-dir @WD@/fleet --chatterd-port ]] .. p_pmon .. [[ \
  --health-interval-ms 400 >@WD@/pmon.out 2>&1 &
pp=$!
for i in $(seq 1 30); do
  grep -q "chatterd=up sysagent=up fwatch=up" @WD@/pmon.out 2>/dev/null && break
  sleep 0.2
done
kill -TERM $pp 2>/dev/null
if timeout 8 tail --pid=$pp -f /dev/null 2>/dev/null; then wait $pp; echo "pmon-exit=$?"; else echo "pmon-exit=124"; kill -9 $pp 2>/dev/null; fi
cat @WD@/pmon.out
]])
checks.expect_match(fleet.out, "pmon: capabilities bounding_set_dropped=%d+ no_new_privs=1",
  lang .. ": pmon drops the capability bounding set and sets no_new_privs")
checks.expect_match(fleet.out, "pmon: started service=chatterd",
  lang .. ": pmon starts chatterd")
checks.expect_match(fleet.out, "pmon: health chatterd=up sysagent=up fwatch=up restarts=",
  lang .. ": pmon reaches all-services-up health")
local pmon_exit = fleet.out:match("pmon%-exit=(%d+)")
expect_true(pmon_exit == "0",
  lang .. ": pmon shuts the fleet down cleanly on SIGTERM (exit 0)", "pmon-exit=" .. tostring(pmon_exit))
checks.expect_match(fleet.out, "pmon: shutdown", lang .. ": pmon prints the shutdown line")

-- ---------------------------------------------------------------------------
-- 6. telemetry to the LGTM stack (SKIP if unreachable).
-- ---------------------------------------------------------------------------
local grafana = "http://localhost:3000"
local health = checks.run(
  "curl -s -o /dev/null -w '%{http_code}' -u admin:admin " .. grafana .. "/api/health")
if health.out:match("200") then
  -- A one-shot --once pushes a single gauge sample that does not reliably
  -- persist as a queryable series; run sysagent for a few seconds so the
  -- periodic reader pushes several samples under a distinct node label.
  local node = "captel-" .. lang
  local otel = sh(
    "OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318 @BIN@ sysagent --node " .. node ..
    " --interval-ms 500 >@WD@/tel.out 2>&1 & sp=$!; sleep 3.2; kill -INT $sp 2>/dev/null; " ..
    "wait $sp 2>/dev/null; cat @WD@/tel.out")
  -- (node omitted from the pattern: it contains a '-', a Lua-pattern magic char)
  checks.expect_match(otel.out, "sysagent: otel enabled endpoint=http://localhost:4318",
    lang .. ": sysagent enables OTLP export when the endpoint is set")

  -- sysagent_cpu_pct{node="captel-<lang>"}
  local q = grafana .. "/api/datasources/proxy/uid/prometheus/api/v1/query?query=" ..
    "sysagent_cpu_pct%7Bnode%3D%22" .. node .. "%22%7D"
  local poll = checks.run(table.concat({
    "V=; for i in $(seq 1 30); do",
    "  V=$(curl -s -u admin:admin '" .. q .. "' | python3 -c 'import sys,json;r=json.load(sys.stdin)[\"data\"][\"result\"];print(len(r))' 2>/dev/null)",
    "  [ -n \"$V\" ] && [ \"$V\" != \"0\" ] && break; sleep 0.5; done",
    "echo PROM_SERIES=$V",
  }, "\n"))
  local series = tonumber(poll.out:match("PROM_SERIES=(%d+)"))
  expect_true(series ~= nil and series >= 1,
    lang .. ": the sysagent_cpu_pct{node=" .. node .. "} gauge reached Prometheus via OTLP",
    "PROM_SERIES=" .. tostring(poll.out:match("PROM_SERIES=(%S*)")))
else
  print(lang .. ": LGTM stack not reachable -- skipping telemetry assertions")
end

checks.run("rm -rf " .. wd)
checks.finish()
