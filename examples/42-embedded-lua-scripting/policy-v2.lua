-- policy-v2.lua -- the "edited" policy the demo drops in before sending SIGHUP.
--
-- Identical in shape to policy.lua; only CONFIG and POLICY_VERSION change. The
-- host re-reads its --policy path on SIGHUP, so swapping this file in and
-- signalling the running supervisor retunes restart behaviour with no rebuild
-- and no restart of pmon itself. That is the whole point of the chapter.

local CONFIG = {
  base_backoff_ms   = 100,    -- much tighter first backoff than v1's 500ms
  max_backoff_ms    = 30000,
  max_restarts      = 2,      -- give up far sooner than v1's 8
  fast_crash_ms     = 2000,
  fast_crash_streak = 3,
}

POLICY_VERSION = "1.1.0"

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

function on_load()
  safe_log(string.format(
    "policy loaded: version=%s base_backoff_ms=%d max_backoff_ms=%d max_restarts=%d",
    POLICY_VERSION, CONFIG.base_backoff_ms, CONFIG.max_backoff_ms, CONFIG.max_restarts))
end

function on_exit(info)
  local name = info.name or "?"
  local exit_code = info.exit_code
  local signal = info.signal
  local restarts = info.restarts or 0
  local consecutive = info.consecutive_failures or 0
  local uptime_ms = info.uptime_ms or -1
  local last_backoff_ms = info.last_backoff_ms or 0

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
