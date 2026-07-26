-- Verify the embedded-Lua policy engine for LSP_LANG (cpp|go|rust).
--
-- The C++/Go/Rust hosts all embed the SAME policy.lua and must produce the
-- SAME observable decisions -- that identity is what this script asserts, not
-- merely a zero exit. Every assertion is a checks.expect_match against the
-- host's fixed decision-line format, so it is deterministic and language-
-- neutral (no wall-clock timing, no process introspection).

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

-- Build once up front so a compile failure is a distinct, obvious failure.
local b = checks.run("./demo.sh " .. lang .. " build")
checks.expect_exit(b, 0, lang .. ": builds")

-- The main scenario: replay a two-phase exit sequence through policy.lua,
-- reload policy-v2.lua on a real SIGHUP between phases, replay the rest.
local r = checks.run("./demo.sh " .. lang .. " run")
checks.expect_exit(r, 0, lang .. ": run exits 0")

-- v1 policy loads at startup and announces its tunables (proves on_load fired
-- and host.log crossed the boundary Lua->host).
checks.expect_match(r.out, "policy: policy loaded: version=1%.0%.0",
  lang .. ": v1 policy loads at startup")
checks.expect_match(r.out, "base_backoff_ms=500 max_backoff_ms=30000 max_restarts=8",
  lang .. ": v1 tunables reported from Lua")

-- Clean exit -> stop, never restarted.
checks.expect_match(r.out, "child=web action=stop",
  lang .. ": clean-exit child gets action=stop")

-- Exponential backoff is computed in Lua, not hardcoded in the host: the first
-- restart backs off the base 500ms, the next doubles to 1000ms.
checks.expect_match(r.out, "child=worker action=restart delay_ms=500",
  lang .. ": first backoff is base 500ms")
checks.expect_match(r.out, "child=worker action=restart delay_ms=1000",
  lang .. ": backoff doubles to 1000ms")

-- Hot reload: real SIGHUP re-reads the --policy path, now holding v2.
checks.expect_match(r.out, "awaiting SIGHUP", lang .. ": supervisor waits for SIGHUP")
checks.expect_match(r.out, "reload requested", lang .. ": SIGHUP handled")
checks.expect_match(r.out, "policy: policy loaded: version=1%.1%.0",
  lang .. ": SIGHUP reloads to v2 policy")

-- Post-reload decisions use v2's numbers: smaller base backoff, and the lower
-- max_restarts turns a would-be restart into an escalate.
checks.expect_match(r.out, "action=restart delay_ms=100",
  lang .. ": post-reload decision uses v2's smaller base backoff")
checks.expect_match(r.out, "child=worker action=escalate",
  lang .. ": v2's lower max_restarts triggers escalate")

-- Sandbox + error handling via `check`: a good policy loads clean...
local ok = checks.run("./demo.sh " .. lang .. " check ../policy.lua")
checks.expect_exit(ok, 0, lang .. ": check accepts a good policy")
checks.expect_match(ok.out, "policy ok version=1%.0%.0",
  lang .. ": good policy reports ok")

-- ...a policy that reaches for os.execute is rejected by the sandbox...
local evil = checks.run("./demo.sh " .. lang .. " check ../policy-evil.lua")
checks.expect_exit(evil, 1, lang .. ": check rejects a sandbox-escaping policy")
checks.expect_match(evil.out, "policy error:", lang .. ": sandbox rejection reported")
checks.expect_match(evil.out, "os", lang .. ": rejection names the blocked global")

-- ...and a syntax error is caught at load, not crashed on.
local broken = checks.run("./demo.sh " .. lang .. " check ../policy-broken.lua")
checks.expect_exit(broken, 1, lang .. ": check rejects a syntax-broken policy")
checks.expect_match(broken.out, "policy error:", lang .. ": parse error reported")

checks.finish()
