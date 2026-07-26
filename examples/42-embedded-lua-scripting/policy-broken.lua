-- policy-broken.lua -- a policy with a syntax error.  [validate: expect-syntax-error]
--
-- The host loads policies with a protected load, so a syntax error surfaces as
-- data (a returned error), not a crash. `pmon check --policy policy-broken.lua`
-- reports the parse failure and exits non-zero; the running supervisor would
-- keep its previous good policy. This is the load-time half of "Errors, three
-- ways" -- the runtime half is policy-evil.lua.

POLICY_VERSION = "broken"

function on_exit(info)
  -- Deliberate syntax error: `end` is missing to close the `if`.
  if info.exit_code == 0 then
    return { action = "stop", delay_ms = 0, reason = "clean" }
  -- (no `end` here) -- parser reports "'end' expected"
  return { action = "restart", delay_ms = 0, reason = "crash" }
end
