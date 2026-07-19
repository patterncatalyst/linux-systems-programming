-- Verify chatterd v0 (21-tcp-sockets) for LSP_LANG.
--
-- Asserts observable behavior, identical across cpp/go/rust:
--   1. usage errors exit 2; chatctl against a closed port exits 1 and reports
--      the failure on stderr.
--   2. A real two-client session over loopback TCP: start `serve` in the
--      background, connect two chatctl clients (bob, then alice) through
--      scripted stdin fifos, and assert the actually delivered lines —
--        * alice's message reaches bob as "alice: hello"
--        * alice never sees her own message echoed back (messages go to
--          OTHER clients only)
--        * both clients see a join notice from the reserved "server" sender
--        * the server logs each join on stderr
--   3. SIGINT tears the listener down and the server exits 0.
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

-- Pick a free loopback port (bound and released; the server sets SO_REUSEADDR
-- so the immediate rebind succeeds even against a lingering TIME_WAIT).
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

-- ---------------------------------------------------------------------------
-- 1. error shapes
-- ---------------------------------------------------------------------------

local noargs = checks.run(demo)
checks.expect_exit(noargs, 2, lang .. ": no arguments exits 2")
checks.expect_match(noargs.out, "usage: chatterd", lang .. ": no arguments prints usage")

local badsub = checks.run(demo .. " frobnicate")
checks.expect_exit(badsub, 2, lang .. ": unknown subcommand exits 2")

local noport = checks.run(demo .. " serve")
checks.expect_exit(noport, 2, lang .. ": serve without --port exits 2")

local noname = sh(demo .. " chatctl --port @PORT@")
checks.expect_exit(noname, 2, lang .. ": chatctl without --name exits 2")

-- Nothing is listening yet: connect must fail with a runtime error, exit 1.
local refused = sh(demo .. " chatctl --port @PORT@ --name x < /dev/null")
checks.expect_exit(refused, 1, lang .. ": chatctl against a closed port exits 1")
checks.expect_match(refused.out, "chatctl: error:", lang .. ": connect failure is reported")

-- ---------------------------------------------------------------------------
-- 2 + 3. two-client session, then a clean SIGINT shutdown
-- ---------------------------------------------------------------------------

local session = sh([[
set -u
wait_for() {  # wait_for FILE PATTERN
  for _ in $(seq 1 200); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.05; done
  return 1
}

@DEMO@ serve --port @PORT@ --host 127.0.0.1 > @WD@/srv.out 2>&1 &
SRV=$!
for _ in $(seq 1 200); do ss -ltn 2>/dev/null | grep -q ":@PORT@ " && break; sleep 0.05; done

mkfifo @WD@/afifo @WD@/bfifo

# bob joins first, then alice — so bob is present to witness alice's join.
@DEMO@ chatctl --port @PORT@ --name bob   < @WD@/bfifo > @WD@/b.out 2>&1 &
B=$!
exec 4> @WD@/bfifo
wait_for @WD@/srv.out "bob joined"

@DEMO@ chatctl --port @PORT@ --name alice < @WD@/afifo > @WD@/a.out 2>&1 &
A=$!
exec 3> @WD@/afifo
wait_for @WD@/a.out "server: alice joined"
wait_for @WD@/b.out "server: alice joined"

# alice speaks; bob must receive it.
printf 'hello\n' >&3
wait_for @WD@/b.out "alice: hello"

# Close alice first and let her exit fully, so a.out is frozen before we look.
exec 3>&-
wait "$A"
exec 4>&-
wait "$B"

# Per-file observations (avoids cross-section ambiguity in the transcript).
grep -q "^alice: hello$"          @WD@/b.out && echo "BOB_GOT_HELLO=yes"    || echo "BOB_GOT_HELLO=no"
grep -q "^server: alice joined$"  @WD@/b.out && echo "BOB_SAW_JOIN=yes"     || echo "BOB_SAW_JOIN=no"
grep -q "^server: bob joined$"    @WD@/b.out && echo "BOB_SAW_OWN_JOIN=yes" || echo "BOB_SAW_OWN_JOIN=no"
grep -q "joined$"                 @WD@/a.out && echo "ALICE_SAW_JOIN=yes"   || echo "ALICE_SAW_JOIN=no"
grep -q "^alice: hello$"          @WD@/a.out && echo "ALICE_SELF_ECHO=yes"  || echo "ALICE_SELF_ECHO=no"

echo "===ALICE===";  cat @WD@/a.out
echo "===BOB===";    cat @WD@/b.out
echo "===END==="

# SIGINT the listener; it must close and exit 0.
kill -INT "$SRV"
for _ in $(seq 1 200); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.05; done
if kill -0 "$SRV" 2>/dev/null; then kill -9 "$SRV"; echo "server_exit=timeout"; else
  wait "$SRV"; echo "server_exit=$?"
fi

# The server log now includes the shutdown line printed on SIGINT.
echo "===SERVER==="; cat @WD@/srv.out
]])

-- Message delivery to the OTHER client.
checks.expect_match(session.out, "BOB_GOT_HELLO=yes",
  lang .. ": alice's message is delivered to bob as 'alice: hello'")
checks.expect_match(session.out, "ALICE_SELF_ECHO=no",
  lang .. ": a message is not echoed back to its own sender")

-- Both clients see a join.
checks.expect_match(session.out, "ALICE_SAW_JOIN=yes", lang .. ": alice sees a join notice")
checks.expect_match(session.out, "BOB_SAW_JOIN=yes", lang .. ": bob sees alice's join notice")
checks.expect_match(session.out, "BOB_SAW_OWN_JOIN=yes",
  lang .. ": a joiner is told of its own join (join goes to all members)")

-- The delivered lines really appear in the transcript.
checks.expect_match(session.out, "\nalice: hello\n",
  lang .. ": 'alice: hello' is present in bob's transcript")
checks.expect_match(session.out, "server: alice joined",
  lang .. ": the join notice carries the reserved 'server' sender")

-- Server-side observations.
checks.expect_match(session.out, "chatterd: listening on 127%.0%.0%.1:" .. port,
  lang .. ": server announces the listen address")
checks.expect_match(session.out, "chatterd: bob joined", lang .. ": server logs bob's join")
checks.expect_match(session.out, "chatterd: alice joined", lang .. ": server logs alice's join")
checks.expect_match(session.out, "chatterd: shutting down", lang .. ": server logs the shutdown")

-- Clean SIGINT teardown.
checks.expect_match(session.out, "server_exit=0", lang .. ": SIGINT shuts the listener and exits 0")

checks.run("rm -rf " .. wd)
checks.finish()
