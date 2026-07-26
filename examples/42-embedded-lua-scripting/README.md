# 42 — embedded-lua-scripting

`pmon` lifts its restart/backoff decision out of the host binary and into a
shared **`policy.lua`**. Each host — C++ (sol2), Go (gopher-lua), Rust (mlua) —
embeds a Lua interpreter, sandboxes it, and calls `on_exit(info)` after each
supervised child dies. Lua returns a `{ action, delay_ms, reason }` decision the
host obeys. Edit `policy.lua`, send **SIGHUP**, and the running supervisor
retunes with no rebuild and no restart of `pmon` itself.

One script, three host runtimes, identical observable behaviour.

## The host ↔ script boundary

```
host  --info-->  on_exit(info)   -- {name, exit_code|signal, restarts,
                                  --  consecutive_failures, uptime_ms,
                                  --  last_backoff_ms}
on_exit --decision--> host       -- {action="restart"|"stop"|"escalate",
                                  --  delay_ms, reason}
policy  --host.log(msg)--> host  -- structured log from inside the policy
policy  --host.now_ms()--> host  -- monotonic-ish clock, for time-aware policies
```

The sandbox opens only `base`, `string`, `table`, `math`, and removes
`load`/`loadfile`/`dofile`/`require`; `os`, `io`, `package` and `debug` are
never loaded. A policy therefore cannot touch the filesystem, spawn processes,
or pull in new code — `policy-evil.lua` proves it.

## Policy files

| file | role |
|---|---|
| `policy.lua` | v1 — base backoff 500ms, give up after 8 restarts |
| `policy-v2.lua` | v2 — base backoff 100ms, give up after 2 restarts (the SIGHUP edit) |
| `policy-evil.lua` | calls `os.execute` — rejected by the sandbox at runtime |
| `policy-broken.lua` | a syntax error — rejected at load |

`policy.lua` uses only the intersection of Lua 5.1 (gopher-lua) and Lua 5.4
(sol2/mlua): no `//`, no bitwise operators, no `<close>`/`<const>`, no
5.2+-only stdlib. That is what lets one file drive all three hosts.

## Run it

```bash
# full scenario + live SIGHUP reload, one language:
./demo.sh cpp run
./demo.sh go run
./demo.sh rust run

# load-check a single policy under the sandbox:
./demo.sh rust check ../policy-evil.lua     # -> pmon: policy error: ... 'os'
./demo.sh rust check ../policy.lua          # -> pmon: policy ok version=1.0.0

# try the reload by hand: run the binary, edit the policy, signal it
cp policy.lua /tmp/p.lua
./cpp/build/release/app run --policy /tmp/p.lua &
cp policy-v2.lua /tmp/p.lua && kill -HUP %1
```

## Verify

`verify.lua` asserts the *same* decision lines across cpp/go/rust: the v1 load
banner, the 500ms→1000ms backoff doubling, the clean-exit `stop`, the SIGHUP
reload to v1.1.0, the post-reload 100ms backoff and early `escalate`, plus the
sandbox and syntax rejections from `check`. Behaviour, not exit codes.

```bash
LSP_LANG=cpp lua verify.lua      # (run from this directory)
```

Mode: `local`. Libraries: `sol2/3.5.0`+`lua/5.4.6` (Conan), `gopher-lua v1.1.2`,
`mlua 0.12` (`lua54`+`vendored`).
