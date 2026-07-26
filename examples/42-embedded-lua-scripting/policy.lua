-- policy.lua -- pmon restart/backoff policy.
--
-- Loaded once at startup and again on every SIGHUP. The host (C++/sol2 and
-- Rust/mlua on Lua 5.4, Go/gopher-lua on Lua 5.1) calls on_exit() after each
-- supervised child exits and acts on the table it returns. Nothing here is
-- language-specific: it uses only features present in BOTH Lua 5.1 and 5.4
-- (no //, no goto, no <close>, no bitwise operators, no 5.2+-only stdlib) so
-- the exact same file drives all three hosts.
--
-- Contract:
--   on_exit(info) -> decision      -- required
--   on_load()                      -- optional, called once right after (re)load
--   host.log(msg)                  -- host callback, available inside this file
--   host.now_ms()                  -- host callback, monotonic-ish wall clock ms
--
-- The host sandboxes this script: os, io, package and debug are NOT loaded,
-- and load/loadfile/dofile/require are removed, so a policy cannot touch the
-- filesystem, spawn processes, or pull in new code -- see the chapter.

-- Tunables. Nothing outside this table needs to change to retune the policy,
-- which is the whole point of shipping it as data-shaped Lua instead of a
-- recompiled host.
local CONFIG = {
  base_backoff_ms   = 500,    -- first restart waits this long
  max_backoff_ms    = 30000,  -- backoff never grows past this
  max_restarts      = 8,      -- give up for good after this many lifetime restarts
  fast_crash_ms     = 2000,   -- an exit faster than this "didn't really start"
  fast_crash_streak = 3,      -- this many fast failures in a row -> escalate now
}

POLICY_VERSION = "1.0.0"

-- Compute the next backoff delay from the delay the host actually used last
-- time (info.last_backoff_ms), not from a restart counter -- the host doesn't
-- need to know the policy's math, and the policy doesn't need the host's
-- counters to stay in lockstep with its own. It just doubles whatever it is
-- handed back, with a floor and a cap.
local function next_backoff_ms(last_backoff_ms)
  local delay
  if last_backoff_ms == nil or last_backoff_ms <= 0 then
    delay = CONFIG.base_backoff_ms
  else
    delay = last_backoff_ms * 2
  end
  if delay > CONFIG.max_backoff_ms then
    delay = CONFIG.max_backoff_ms
  end
  return delay
end

local function safe_log(msg)
  if host ~= nil and host.log ~= nil then
    host.log(msg)
  end
end

-- Called once, right after the host loads or reloads this file. Purely for
-- observability: it is how "SIGHUP picked up the new policy" becomes a line
-- in the log instead of an assertion about internal state.
function on_load()
  safe_log(string.format(
    "policy loaded: version=%s base_backoff_ms=%d max_backoff_ms=%d max_restarts=%d",
    POLICY_VERSION, CONFIG.base_backoff_ms, CONFIG.max_backoff_ms, CONFIG.max_restarts))
end

-- The one function the host requires. info is a plain table of primitives; the
-- return value is a plain table with at least action, delay_ms and reason set.
function on_exit(info)
  local name = info.name or "?"
  local exit_code = info.exit_code             -- integer or nil (signal death)
  local signal = info.signal                   -- string or nil (e.g. "SIGSEGV")
  local restarts = info.restarts or 0
  local consecutive = info.consecutive_failures or 0
  local uptime_ms = info.uptime_ms or -1
  local last_backoff_ms = info.last_backoff_ms or 0

  -- Clean exit: the child chose to stop. Do not second-guess it.
  if signal == nil and exit_code == 0 then
    return {
      action = "stop",
      delay_ms = 0,
      reason = string.format("%s exited 0: clean shutdown, not restarting", name),
    }
  end

  local consecutive_failures = consecutive + 1
  local how
  if signal ~= nil then
    how = "signal " .. signal
  else
    how = "exit code " .. tostring(exit_code)
  end

  -- Crash-looping: several failures in a row, each dying almost immediately.
  -- This is the case a flat restart counter misses -- 8 restarts spread over
  -- an hour is normal churn, 3 restarts in 3 seconds is a broken binary.
  local fast = uptime_ms >= 0 and uptime_ms < CONFIG.fast_crash_ms
  if fast and consecutive_failures >= CONFIG.fast_crash_streak then
    return {
      action = "escalate",
      delay_ms = 0,
      reason = string.format(
        "%s: %d fast failures in a row (last %s after %dms uptime) -- crash-looping, paging",
        name, consecutive_failures, how, uptime_ms),
    }
  end

  -- Lifetime give-up: failing steadily, not in a tight loop, but it has burned
  -- through its restart budget.
  if restarts >= CONFIG.max_restarts then
    return {
      action = "escalate",
      delay_ms = 0,
      reason = string.format(
        "%s: giving up after %d restarts (last %s)", name, restarts, how),
    }
  end

  local delay_ms = next_backoff_ms(last_backoff_ms)
  return {
    action = "restart",
    delay_ms = delay_ms,
    reason = string.format(
      "%s: restart %d after %s (uptime %dms), backing off %dms",
      name, restarts + 1, how, uptime_ms, delay_ms),
  }
end
