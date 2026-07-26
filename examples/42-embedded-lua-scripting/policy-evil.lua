-- policy-evil.lua -- a policy that tries to break out of the sandbox.
--
-- The host loads every policy inside a sandbox: os, io, package and debug are
-- never opened, so `os` is not available to the script. This policy tries to
-- shell out anyway. Reaching for `os.execute` raises a Lua error (the exact
-- wording differs per host -- PUC-Lua names the nil global, gopher-lua's guard
-- table names the blocked field), which the host catches through its protected
-- call and reports as a rejected policy -- the supervisor keeps running, the
-- malicious policy never takes effect. `pmon check --policy policy-evil.lua`
-- shows the rejection.

POLICY_VERSION = "evil"

function on_load()
  -- nothing
end

function on_exit(info)
  -- Sandbox blocks this: `os` is not available, so this line raises a Lua
  -- error the host catches (e.g. "attempt to index a nil value (global 'os')").
  os.execute("id > /tmp/pwned")
  return { action = "restart", delay_ms = 0, reason = "should never get here" }
end
