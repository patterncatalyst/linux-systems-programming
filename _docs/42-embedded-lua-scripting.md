---
title: "Embedding Lua as a policy engine: one policy.lua, three host runtimes"
order: 42
part: "Deep Dives"
description: "lua-policy moves pmon's restart/backoff decision out of the compiled binary and into a single sandboxed policy.lua, embedded three structurally different ways -- C++23 via sol2, Go via gopher-lua (Lua 5.1), Rust via mlua (Lua 5.4) -- and reloaded live on a real SIGHUP with no process restart. Verified on the Fedora 44 host: byte-identical decisions across all three languages (diff-confirmed), PASS 19/FAIL 0 per language, and three genuinely different sandbox-rejection diagnostics for the same malicious policy."
duration: "45 minutes"
---

Chapter 12 taught `pmon` to treat a signal as data instead of an
interruption: block `SIGCHLD` process-wide, then read it off a `signalfd` on
the next turn of an ordinary `poll` loop. Chapter 41's capstone fleet leaned
on the same discipline for `SIGTERM`/`SIGINT`. This chapter reaches for a
different signal — `SIGHUP`, "please re-read your configuration" — to
reload something `pmon` has never had before: a restart policy that is not
compiled in.

Every earlier `pmon` shipped its restart/backoff logic as C++, Go, or Rust
source, rebuilt whenever the policy changed. This chapter's `pmon` keeps the
supervisor's *mechanism* — track a child, replay its exit — in the host
language, and moves the restart *decision* into `policy.lua`, one file with
one contract: `on_exit(info) -> decision`. Embedding Lua is not new. What is
worth a chapter is embedding the *same* file into three sandboxed
interpreters built three structurally different ways — sol2's C++ DSL over
PUC-Rio 5.4, gopher-lua's pure-Go 5.1 VM, mlua's Rust bindings over a
vendored PUC-Rio 5.4 — and proving all three reach byte-identical restart
decisions from it, live reload included.

{% include excalidraw.html
   file="42-host-script-boundary"
   alt="Three stacked host-runtime boxes on the left -- C++23/sol2 against PUC-Rio Lua 5.4, Go/gopher-lua against Lua 5.1, Rust/mlua against a vendored PUC-Rio Lua 5.4 -- each with a bidirectional arrow into a center box labeled policy.lua, one file, marked on_exit(info) going in and decision{action,delay_ms,reason} coming back. The policy.lua box sits inside a shaded sandbox-boundary band with a ghost box reading blocked: os, io, package, debug (never opened); nilled: load/loadfile/dofile/require/collectgarbage/loadstring. A host table box on the right connects to policy.lua with a bidirectional arrow labeled host.log / host.now_ms. A caption note reads Lua 5.1 (gopher-lua) vs Lua 5.4 (sol2, mlua) forces the shared subset -- one policy.lua drives all three."
   caption="Figure 42.1 — the host↔script boundary: three differently-built sandboxed Lua states, one shared policy.lua, and the host table it calls back into" %}

> **Tools used** — `lua` (host, drives `verify.lua` and is the PUC-Rio 5.4
> reference interpreter `policy.lua` was checked against), `kill` (host,
> sends the real `SIGHUP` that triggers the live reload — a shell builtin /
> coreutils command), and the three build toolchains `cmake`+`conan` / `go`
> / `cargo` (host). `lua`, `cmake`, `conan`, `go`, and `cargo` all appear in
> `scripts/check-host.sh`; `kill` ships in Fedora's base install.

## The host↔script boundary: `on_exit(info) -> decision`

`pmon`'s contract with `policy.lua` is four callbacks wide (Figure 42.1),
and every one crosses the boundary as plain data — numbers, strings, and
tables, never a function pointer or closure the host has to reason about:
`on_exit(info)` in, a `{action, delay_ms, reason}` decision back, plus two
callbacks the policy can call into the host, `host.log(msg)` and
`host.now_ms()`.

Every simulated child exit becomes one call across that boundary. The host
builds an `info` table from a hardcoded `Event`/`event`/`ExitEvent` struct,
calls `on_exit` under a **protected** call — so a bug in the policy becomes
a returned error, never a crash — and reads back three fields from the
table Lua handed back. That marshaling looks remarkably similar across all
three despite three unrelated Lua C APIs underneath:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
[[nodiscard]] std::expected<Decision, std::string> decide(sol::state& lua, const Event& ev) {
    sol::table info = lua.create_table();
    info["name"] = std::string{ev.name};
    info["exit_code"] = ev.exit_code;
    info["restarts"] = ev.restarts;
    info["consecutive_failures"] = ev.consecutive_failures;
    info["uptime_ms"] = ev.uptime_ms;
    info["last_backoff_ms"] = ev.last_backoff_ms;

    sol::protected_function fn = lua["on_exit"];
    if (!fn.valid()) {
        return std::unexpected(std::string{"policy defines no on_exit"});
    }
    sol::protected_function_result result = fn(info);
    if (!result.valid()) {
        sol::error err = result;
        return std::unexpected(err.what());
    }
    sol::table decision = result;
    return Decision{
        .action = decision["action"],
        .delay_ms = decision["delay_ms"],
        .reason = decision["reason"],
    };
}
```

```go
func callOnExit(L *lua.LState, ev event) (decision, error) {
	info := L.NewTable()
	info.RawSetString("name", lua.LString(ev.name))
	if ev.exitCode != nil {
		info.RawSetString("exit_code", lua.LNumber(*ev.exitCode))
	}
	if ev.signal != nil {
		info.RawSetString("signal", lua.LString(*ev.signal))
	}
	info.RawSetString("restarts", lua.LNumber(ev.restarts))
	info.RawSetString("consecutive_failures", lua.LNumber(ev.consecutiveFailures))
	info.RawSetString("uptime_ms", lua.LNumber(ev.uptimeMs))
	info.RawSetString("last_backoff_ms", lua.LNumber(ev.lastBackoffMs))

	fn, ok := L.GetGlobal("on_exit").(*lua.LFunction)
	if !ok {
		return decision{}, fmt.Errorf("policy defines no on_exit function")
	}
	if err := L.CallByParam(lua.P{Fn: fn, NRet: 1, Protect: true}, info); err != nil {
		return decision{}, fmt.Errorf("on_exit: %w", luaErr(err))
	}
	ret := L.Get(-1)
	L.Pop(1)
	tbl, ok := ret.(*lua.LTable)
	if !ok {
		return decision{}, fmt.Errorf("on_exit returned %s, want table", ret.Type())
	}
	return decision{
		action:  tbl.RawGetString("action").String(),
		delayMs: int64(lua.LVAsNumber(tbl.RawGetString("delay_ms"))),
		reason:  tbl.RawGetString("reason").String(),
	}, nil
}
```

```rust
fn info_table(lua: &Lua, ev: &ExitEvent) -> mlua::Result<Table> {
    let info = lua.create_table()?;
    info.set("name", ev.name)?;
    info.set("exit_code", ev.exit_code)?;
    info.set("restarts", ev.restarts)?;
    info.set("consecutive_failures", ev.consecutive_failures)?;
    info.set("uptime_ms", ev.uptime_ms)?;
    info.set("last_backoff_ms", ev.last_backoff_ms)?;
    Ok(info)
}

/// Call `on_exit(info)` under a protected call and print the resulting
/// decision. The `reason` text is entirely Lua's (built with
/// `string.format` in policy.lua) -- this only formats the wrapping
/// `pmon: decision ...` line around whatever the policy returned.
fn decide(lua: &Lua, ev: &ExitEvent) -> mlua::Result<()> {
    let info = info_table(lua, ev)?;
    let on_exit: Function = lua.globals().get("on_exit")?;
    let decision: Table = on_exit.call(info)?;

    let action: String = decision.get("action")?;
    let delay_ms: i64 = decision.get("delay_ms")?;
    let reason: String = decision.get("reason")?;

    println!(
        "pmon: decision child={} action={} delay_ms={} reason=\"{}\"",
        ev.name, action, delay_ms, reason
    );
    Ok(())
}
```

`sol::protected_function`, `L.CallByParam{Protect:true}`, and a plain
`Function::call` returning `mlua::Result` are three names for the same idea:
a Lua-side error inside `on_exit` comes back to the host as data, not a
`longjmp` or a process-ending panic. `signal` is the one field every binding
treats as *conditionally present*: this scenario never kills a child with a
signal, so C++'s `info["signal"]` is simply never set, and Go's
`event.signal` being a `*string` makes that "absent, not nil" contract
explicit in the host's own type.

That marshaling is scaffolding around the one function that makes the
restart decision, quoted here once because it is one file, not a codetab:

```lua
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
```

Four rules, checked in order, and every one is a decision the *host* never
makes: a clean exit always `stop`s; a tight streak of fast failures (three
inside `fast_crash_ms`, 2000 ms) `escalate`s regardless of lifetime restart
count — the case a flat counter misses, since 8 restarts over an hour is
normal churn and 3 in 3 seconds is a broken binary; burning through the
lifetime `max_restarts` budget also `escalate`s; anything else `restart`s
with a backoff doubled from whatever delay the host last used, floored at
`base_backoff_ms` and capped at `max_backoff_ms`. The whole restart policy
is under 130 lines of Lua, none of it duplicated in any of the three hosts.

## One `policy.lua`, two Lua versions

`policy.lua` is a single 128-line file, and it is not just shared by
convention — it is written to run correctly under **two different Lua
language versions** at once. sol2 and mlua both build against PUC-Rio **Lua
5.4.6**; gopher-lua is a pure-Go reimplementation of **Lua 5.1**. Lua 5.2
through 5.4 added integer division (`//`), bitwise operators, `goto`, and
to-be-closed variables (`<close>`); 5.1 has none of them. `policy.lua`'s own
header comment states the constraint plainly: it "uses only features
present in BOTH Lua 5.1 and 5.4 ... so the exact same file drives all three
hosts." Concretely, the backoff doubling reads `delay = last_backoff_ms *
2`, not a bitwise left-shift, and every formatted string goes through
`string.format`, identical in both versions. This is the same lesson the
book has repeated elsewhere — a `hardware_concurrency()` call lying about a
container, a `runtime.NumCPU()` lying about an affinity mask — reached here
through language-version drift instead of kernel drift.

## The sandbox: what `policy.lua` cannot touch

`policy.lua` never sees `os`, `io`, `package`, or `debug` — those libraries
are simply never opened, in any binding — and `load`, `loadfile`, `dofile`,
`require`, `collectgarbage`, and `loadstring` are explicitly nilled out
afterward, even though their *owning* libraries were never opened either.
That belt-and-suspenders removal matters because those six globals are
reachable straight from `base`, the one library every binding does open; a
global with the same name could otherwise be reintroduced by a careless
policy. The three bindings build that sandbox with three different APIs,
but the shape is identical — open four libraries, remove six globals,
register one `host` table:

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
sol::state make_sandbox() {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);
    lua["load"] = sol::lua_nil;
    lua["loadfile"] = sol::lua_nil;
    lua["dofile"] = sol::lua_nil;
    lua["require"] = sol::lua_nil;
    lua["collectgarbage"] = sol::lua_nil;
    lua["loadstring"] = sol::lua_nil;

    sol::table host = lua.create_named_table("host");
    host.set_function("log", [](const std::string& msg) { say("pmon: policy: " + msg); });
    host.set_function("now_ms", []() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   steady_clock::now().time_since_epoch())
            .count();
    });
    return lua;
}
```

```go
func newSandbox() *lua.LState {
	L := lua.NewState(lua.Options{SkipOpenLibs: true})

	for _, lib := range []struct {
		name string
		open lua.LGFunction
	}{
		{lua.BaseLibName, lua.OpenBase},
		{lua.StringLibName, lua.OpenString},
		{lua.TabLibName, lua.OpenTable},
		{lua.MathLibName, lua.OpenMath},
	} {
		L.Push(L.NewFunction(lib.open))
		L.Push(lua.LString(lib.name))
		L.Call(1, 0)
	}

	for _, g := range []string{"load", "loadfile", "dofile", "require", "collectgarbage", "loadstring"} {
		L.SetGlobal(g, lua.LNil)
	}

	// The unopened libraries (os, io, package, debug, coroutine) are left as
	// plain nils by SkipOpenLibs, which gopher-lua's VM reports generically
	// ("attempt to index a non-table object(nil) with key 'execute'") -- it
	// never names the global the way PUC-Rio Lua does. Trade a nil for a
	// guard table whose __index raises a message that names the blocked
	// global explicitly: same sandboxing (the real library is still never
	// opened), a clearer error for whoever runs `check`.
	for _, blocked := range []string{"os", "io", "package", "debug", "coroutine"} {
		name := blocked
		guard := L.NewTable()
		mt := L.NewTable()
		mt.RawSetString("__index", L.NewFunction(func(L *lua.LState) int {
			key := L.CheckString(2)
			L.RaiseError("sandbox: global '%s' is not available (blocked field '%s')", name, key)
			return 0
		}))
		L.SetMetatable(guard, mt)
		L.SetGlobal(name, guard)
	}

	host := L.NewTable()
	L.SetFuncs(host, map[string]lua.LGFunction{
		"log": func(L *lua.LState) int {
			fmt.Printf("pmon: policy: %s\n", L.CheckString(1))
			return 0
		},
		"now_ms": func(L *lua.LState) int {
			L.Push(lua.LNumber(time.Now().UnixMilli()))
			return 1
		},
	})
	L.SetGlobal("host", host)

	return L
}
```

```rust
fn build_sandbox() -> mlua::Result<Lua> {
    let lua = Lua::new_with(
        StdLib::STRING | StdLib::TABLE | StdLib::MATH,
        LuaOptions::default(),
    )?;

    for g in [
        "load",
        "loadfile",
        "dofile",
        "require",
        "collectgarbage",
        "loadstring",
    ] {
        lua.globals().set(g, Value::Nil)?;
    }

    let host = lua.create_table()?;
    host.set(
        "log",
        lua.create_function(|_, msg: String| {
            println!("pmon: policy: {msg}");
            Ok(())
        })?,
    )?;
    host.set("now_ms", lua.create_function(|_, ()| Ok(now_ms()))?)?;
    lua.globals().set("host", host)?;

    Ok(lua)
}
```

The Go tab is longest for a real reason: `gopher-lua`'s `SkipOpenLibs` leaves
`os`/`io`/`package`/`debug`/`coroutine` as ordinary `nil` globals, and its
own nil-index error is generic — it never names *which* global was nil.
sol2 and mlua get PUC-Rio's actual error message for free; gopher-lua's host
installs a **guard table** per blocked library instead, with a metamethod
that names the field explicitly — a strictly more informative error for
whoever runs `check`. This split shows up again, unprompted, in "Errors,
three ways" below.

## How the code works

Both subcommands — `run` and `check` — funnel through the same three
functions: `make_sandbox`/`newSandbox`/`build_sandbox` builds the
interpreter, `load_policy`/`loadPolicy` runs the chunk under a protected
load and calls the policy's optional `on_load()` hook (only for the
`policy loaded: version=...` banner), and `decide`/`callOnExit`/`decide`
drives one `on_exit` call. `run` replays a fixed two-phase scenario — three
simulated exits against v1, a real `SIGHUP`, three more against whatever the
policy file now contains — hardcoded because the point here is the policy
engine, not a process reaper. `check` loads one policy path, probes
`on_exit` once, and reports `pmon: policy ok version=...` or `pmon: policy
error: ...`, exiting 0 or 1 — the "will this file behave" gate `demo.sh`
calls before ever running a policy for real. Every print in the C++ port
routes through a `say()` helper that calls `std::fflush(stdout)`: `demo.sh`
polls the redirected stdout file for the `awaiting SIGHUP` sentinel, and an
unflushed write would leave it staring at a stale, empty file.

## Errors, three ways

Each host's own source comments name its own "errors, three ways" split —
three unrelated idioms for the identical job, turning an interpreter
failure into data instead of a crash. **C++**: fallible operations return
`std::expected<T, std::string>`, converting a failed
`sol::protected_function_result` into its `sol::error` message; `die()` is
reserved for CLI-usage/OS-resource failures with no data-driven recovery.
**Go**: `die()` prints the fully `%w`-wrapped error chain and exits 1;
`luaErr()` unwraps gopher-lua's `*lua.ApiError` via `errors.As` — never a
bare type assertion — so a caller sees `attempt to index a nil value
(global 'os')` rather than a Go-shaped wrapper string. **Rust**: CLI parse
failures `bail!` out of `parse_args`; host-side operations return
`anyhow::Result` composed with `.context()`; every boundary crossing into
Lua is an `mlua::Result` that a protected load/call turns into data instead
of a panic.

The highlight, though, is what those three idioms produce for the *same*
bad input. `policy-evil.lua` reaches for `os.execute("id > /tmp/pwned")`
inside `on_exit`; `policy-broken.lua` is missing an `end`. Running
`./demo.sh <lang> check ../policy-evil.lua` against all three:

```console
[host]$ ./demo.sh cpp check ../policy-evil.lua
pmon: policy error: ../policy-evil.lua:21: attempt to index a nil value (global 'os')
stack traceback:
        [C]: in metamethod 'index'
        ../policy-evil.lua:21: in function 'base.on_exit'
        ...
[host]$ ./demo.sh go check ../policy-evil.lua
pmon: policy error: on_exit: ../policy-evil.lua:21: sandbox: global 'os' is not available (blocked field 'execute')
[host]$ ./demo.sh rust check ../policy-evil.lua
pmon: policy error: runtime error: [string "../policy-evil.lua"]:21: attempt to index a nil value (global 'os')
stack traceback:
        [C]: in metamethod 'index'
        ...
```

sol2 and mlua report the *identical* diagnostic shape — PUC-Rio's own
"attempt to index a nil value (global 'os')" plus a native stack traceback —
because both are the real reference interpreter underneath, reached through
different Rust/C++ FFI. Go's line differs by design: it's `newSandbox`'s
guard-table metamethod firing, and it names something PUC-Rio's message
doesn't — *which field* (`execute`) the policy tried to reach. Same
rejection, same line number, three genuinely different diagnostics.

`policy-broken.lua`'s missing `end` is a **load**-time failure — caught
before `on_exit` is ever called — and it shows the same split one syntax
error later:

```console
[host]$ ./demo.sh cpp check ../policy-broken.lua
pmon: policy error: ../policy-broken.lua:16: 'end' expected (to close 'if' at line 13) near 'return'
[host]$ ./demo.sh go check ../policy-broken.lua
pmon: policy error: loading ../policy-broken.lua: ../policy-broken.lua line:16(column:8) near 'return':   syntax error
[host]$ ./demo.sh rust check ../policy-broken.lua
pmon: policy error: syntax error: [string "../policy-broken.lua"]:16: 'end' expected (to close 'if' at line 13) near 'return'
```

Once again cpp and rust agree almost to the byte — PUC-Rio's own parser
message — and gopher-lua's parser reports the same line and near-token in
its own format. In every case the running supervisor is unaffected: `check`
loads the bad file in isolation, reports the rejection, and exits 1; a
`run`-time `SIGHUP` reload that hits a bad policy would report the same
error and simply keep serving whatever policy was already loaded — a
`policy-*.lua` typo is a reported problem, never a crash of `pmon` itself.

## Concurrency lens

Every host consumes the *same* `SIGHUP` through three different primitives —
the same divergence Chapter 41 mapped for `SIGTERM`, reached here through a
reload path with one new wrinkle: after the signal lands, **every** binding
discards the whole interpreter and builds a brand-new one.

{% include codetabs.html langs="C++|Go|Rust" %}

```cpp
// Block until SIGHUP arrives on `sigfd`, draining exactly one signalfd_siginfo.
void wait_for_sighup(int sigfd) {
    pollfd pfd{.fd = sigfd, .events = POLLIN, .revents = 0};
    for (;;) {
        const int n = ::poll(&pfd, 1, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("poll: " + last_error().message());
        }
        signalfd_siginfo info{};
        if (::read(sigfd, &info, sizeof info) == sizeof info && info.ssi_signo == SIGHUP) {
            return;
        }
    }
}

// Fresh sandbox + host table + protected load + on_load(), all rebuilt from
// scratch — the simplest correct way to guarantee reload leaves no state
// from the previous policy behind.
[[nodiscard]] std::expected<sol::state, std::string> reload(const std::string& policy_path) {
    sol::state lua = make_sandbox();
    if (auto loaded = load_policy(lua, policy_path); !loaded) {
        return std::unexpected(loaded.error());
    }
    return lua;
}
```

```go
	fmt.Println("pmon: awaiting SIGHUP to reload policy")
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGHUP)
	<-sigCh
	fmt.Println("pmon: reload requested")

	// Re-read the same --policy path (its content has since changed) into a
	// brand-new sandboxed state; the old one is discarded, not mutated.
	L.Close()
	L = newSandbox()
	if err := loadPolicy(L, policyPath); err != nil {
		L.Close()
		return err
	}
```

```rust
    let reload = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal_hook::consts::SIGHUP, reload.clone())
        .context("registering SIGHUP handler")?;

    println!("pmon: awaiting SIGHUP to reload policy");
    while !reload.load(Ordering::Relaxed) {
        thread::sleep(Duration::from_millis(20));
    }
    println!("pmon: reload requested");

    // Rebuild the whole Lua state from scratch on reload, rather than mutate
    // the old one in place: a fresh sandbox guarantees nothing from the old
    // policy (globals, closures) survives into the new one.
    let lua = build_sandbox()
        .map_err(|e| anyhow::anyhow!("{e}"))
        .context("rebuilding sandbox for reload")?;
    load_policy(&lua, policy_path)
        .map_err(|e| anyhow::anyhow!("{e}"))
        .context("reloading policy")?;
```

C++ reuses ch12/ch13's pattern outright: `SIGHUP` is blocked process-wide
with `sigprocmask` up front (in `make_sighup_fd`, not shown), then consumed
as ordinary readability on a `signalfd` inside a `poll` loop — no signal
handler runs at all. Go reaches for the idiomatic equivalent: `signal.Notify`
hands a buffered channel to the Go runtime's own signal-handling machinery —
delivery happens on a dedicated runtime thread, never in user code — and
`cmdRun` simply blocks on `<-sigCh`. Rust's `signal-hook` sits between those
two: it installs a real, async-signal-safe `sigaction` handler, but that
handler is only permitted to flip an `AtomicBool` — no allocation, no Lua
call, no I/O — and `cmd_run`'s own thread polls that flag every 20 ms. The
flag is registered **before** the "awaiting SIGHUP" line prints, closing a
race a naïve implementation would have: a `SIGHUP` landing the instant after
the sentinel appears is never missed.

All three converge on the same rebuild discipline once the signal lands:
throw the whole interpreter away and construct a fresh one — the
correctness argument for reload, not a code-simplicity shortcut. A policy
can define arbitrary local state, upvalues, and closures; mutating a live
state in place would leave open the chance the *old* policy's state
survives into the *new* one's behavior. Building fresh guarantees the
opposite. One asymmetry worth naming even though this chapter never
exercises it: Go's `*lua.LState` is **not** goroutine-safe — a real `pmon`
wanting concurrent `on_exit` calls against a `gopher-lua` policy would need
its own serialization, where sol2's and mlua's states carry the identical
single-threaded constraint under a different name.

## Build, run, observe

```bash
[host]$ cd examples/42-embedded-lua-scripting && ./demo.sh build
```

`./demo.sh <lang> run` copies `policy.lua` into a scratch file, starts `pmon
run --policy <scratch>`, waits for the `awaiting SIGHUP` sentinel, copies
`policy-v2.lua` over the same path, and sends a real `kill -HUP` — no fixed
sleep, no guesswork. The output is identical, byte for byte, across all
three languages (diff-confirmed this session):

```console
[host]$ ./demo.sh cpp run
pmon: policy: policy loaded: version=1.0.0 base_backoff_ms=500 max_backoff_ms=30000 max_restarts=8
pmon: decision child=web action=stop delay_ms=0 reason="web exited 0: clean shutdown, not restarting"
pmon: decision child=worker action=restart delay_ms=500 reason="worker: restart 1 after exit code 1 (uptime 9000ms), backing off 500ms"
pmon: decision child=worker action=restart delay_ms=1000 reason="worker: restart 2 after exit code 1 (uptime 9000ms), backing off 1000ms"
pmon: awaiting SIGHUP to reload policy
pmon: reload requested
pmon: policy: policy loaded: version=1.1.0 base_backoff_ms=100 max_backoff_ms=30000 max_restarts=2
pmon: decision child=api action=restart delay_ms=100 reason="api: restart 1 after exit code 1 (uptime 5000ms), backing off 100ms"
pmon: decision child=worker action=escalate delay_ms=0 reason="worker: giving up after 2 restarts (last exit code 1)"
```

Read as a story: v1 loads (500 ms base, give up after 8). `web` exits 0 —
`stop`. `worker` crashes twice — `restart` at 500 ms, then the doubled
1000 ms backoff. `pmon` parks on `SIGHUP`; `demo.sh` swaps in
`policy-v2.lua` and signals the same, never-restarted PID. Reload picks up
v1.1.0 (100 ms base, give up after only 2). `api`'s fresh crash backs off
100 ms, v2's tighter base; `worker`, already carrying 2 lifetime restarts,
immediately hits v2's `max_restarts=2` and `escalate`s — the same event
that would have been a normal `restart` under v1's `max_restarts=8`. The
policy changed behavior with zero rebuild and zero process restart.

`./demo.sh <lang> check` is the load-and-probe path from "Errors, three
ways" above, and it rejects a hostile policy the same way in every language:

```console
[host]$ ./demo.sh rust check ../policy-evil.lua
pmon: policy error: runtime error: [string "../policy-evil.lua"]:21: attempt to index a nil value (global 'os')
```

(exit 1, full traceback above). And the whole matrix, through the shared harness:

```console
[host]$ python3 scripts/test-all-examples.py --only 42-embedded-lua-scripting
verifying...
  verify 42-embedded-lua-scripting [cpp]: PASS
  verify 42-embedded-lua-scripting [go]: PASS
  verify 42-embedded-lua-scripting [rust]: PASS

example                       cpp   go    rust
42-embedded-lua-scripting     PASS  PASS  PASS
3 passed, 0 failed, 0 skipped
```

Each language's `verify.lua` reported `PASS 19 / FAIL 0`.

{% include excalidraw.html
   file="42-sighup-reload-lifecycle"
   alt="A horizontal sequence of six boxes inside a band labeled one pmon process -- PID never changes: load v1 (base=500ms max_restarts=8), phase-1 decisions (web: stop; worker: restart 500ms to 1000ms), awaiting SIGHUP (a dashed ghost box), reload (fresh Lua state rebuilt, on_load logs v1.1.0), phase-2 decisions (api: restart 100ms; worker: escalate after 2 restarts), and exit 0. Above the awaiting-SIGHUP box, a ghost box shows the operator action -- edit the policy file, then kill -HUP $(pgrep app) -- with an arrow into the flow. Below it, three sub-boxes show the three per-language consumption mechanisms: C++ signalfd + poll, Go signal.Notify(ch), Rust signal_hook flag (AtomicBool + 20ms poll). A bottom note reads: the process is NOT restarted -- only a fresh, sandboxed Lua state is built in place and the decision logic changes underneath it."
   caption="Figure 42.2 — the SIGHUP reload lifecycle: v1's decisions, a real SIGHUP consumed three different ways, a fresh Lua state built in place, and v1.1.0's tighter decisions — same process throughout" %}

## Cross-check: one script, three interpreters, byte-identical decisions

The claim this chapter makes is falsifiable and was actually falsified
against: `policy.lua`, unmodified, drives sol2/PUC-Lua 5.4, gopher-lua's
pure-Go 5.1 VM, and mlua/PUC-Lua 5.4 to the *exact same sequence of restart
decisions*, including the exact backoff numbers and the exact `reason`
strings Lua's `string.format` produced. That is not "the same shape of
output" — it is a byte-for-byte `diff` of three processes built from three
different compilers, three different runtimes, and (for Go) a genuinely
different Lua language version, running the identical two-phase scenario
with the identical `SIGHUP` reload in between. This is possible because
every value crossing the boundary is host-primitive data, never a Lua value
the host interprets loosely; it is *correct* because of the shared-subset
discipline above, which guarantees `policy.lua` compiles to the same
observable program on Lua 5.1 and 5.4 alike. `test-all-examples.py --only
42-embedded-lua-scripting` passing 3/0 and each `verify.lua` reporting
`PASS 19 / FAIL 0` are the gate; the diff-confirmed identical `./demo.sh
<lang> run` transcript above is the proof the gate is checking behavior,
not three green exit codes agreeing by coincidence.

## What you learned

- **The host↔script boundary is four callbacks, all plain data**:
  `on_exit(info) -> decision` plus `host.log`/`host.now_ms`, crossed as
  tables of numbers and strings, never a closure — what makes a
  diff-identical run across three unrelated Lua bindings possible.
- **A shared-subset policy file survives a language-version gap, not just a
  binding-API gap.** `policy.lua` runs identically on PUC-Rio Lua 5.4 (sol2,
  mlua) and gopher-lua's pure-Go Lua 5.1 because it never uses `//`, `goto`,
  `<close>`, bitwise operators, or 5.2+-only stdlib.
- **A protected call turns an interpreter bug into a return value.**
  `sol::protected_function`, `L.CallByParam{Protect:true}`, and
  `mlua::Result` all guarantee the same thing: a bad `on_exit` degrades to
  `pmon: policy error: ...` and exit 1, never a crash of the supervisor.
- **The sandbox-rejection diagnostic differs by binding, and that is
  informative, not a bug.** sol2 and mlua share PUC-Rio's native `attempt
  to index a nil value (global 'os')`; gopher-lua's guard table reports
  `sandbox: global 'os' is not available (blocked field 'execute')` —
  strictly more specific, since gopher-lua's own nil-index error never
  names the global at all.
- **`SIGHUP` reload rebuilds the interpreter, never restarts the process.**
  `pmon`'s PID and child-tracking state survive untouched; only the Lua
  state is thrown away and rebuilt from `policy-v2.lua`, so no closure or
  global from the old policy leaks into the new one.
- **Three signals, one outcome, three primitives.** C++ reuses ch12's
  `sigprocmask` + `signalfd` + `poll`; Go hands the signal to the runtime's
  own machinery via `signal.Notify` and blocks on a channel; Rust's
  `signal-hook` flips an `AtomicBool` from a minimal `sigaction` handler a
  separate thread polls every 20 ms, armed before the "awaiting" line prints
  to close the missed-signal race.

Two deep dives remain in this part — Rust macros for systems code, and the
Go runtime from a systems programmer's seat — the same instinct turned on a
single language's own machinery.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session: <code>python3 scripts/test-all-examples.py
--only 42-embedded-lua-scripting</code> printed <code>3 passed, 0
failed</code> (build cpp/go/rust: ok; verify cpp/go/rust: PASS), with each
language's own <code>verify.lua</code> reporting <code>PASS 19 / FAIL 0</code>.
A full local regression (<code>--mode local</code>) gave <code>96 passed, 0
failed, 3 skipped</code> (the 3 skips are the unrelated <code>38-otel</code>
example, LGTM unreachable) — no regressions from this chapter's example.
<code>./demo.sh &lt;lang&gt; run</code> produced byte-identical output across
cpp/go/rust (diff-confirmed): v1.0.0 (base 500ms, max_restarts=8) driving a
`stop` for a clean exit and a `restart` backoff that doubled 500ms → 1000ms,
then a real <code>kill -HUP</code> reload to v1.1.0 (base 100ms,
max_restarts=2) driving a 100ms `restart` and an `escalate` once the
worker's lifetime restart count (2, carried over from phase one) hit v2's
lower ceiling — the same process throughout, PID unchanged. The sandbox
rejection diagnostics for <code>policy-evil.lua</code> (os.execute) and
<code>policy-broken.lua</code> (missing `end`) were captured verbatim per
language via <code>./demo.sh &lt;lang&gt; check ../policy-evil.lua</code>
and the `-broken` equivalent, confirming sol2/mlua share PUC-Lua 5.4's
native "attempt to index a nil value (global 'os')" text while gopher-lua's
guard table reports "sandbox: global 'os' is not available (blocked field
'execute')". Library pins: <code>sol2/3.5.0</code> +
<code>lua/5.4.6</code> via Conan 2 (C++), <code>gopher-lua v1.1.2</code>
(Go, Lua 5.1 semantics), <code>mlua 0.12</code> with <code>lua54</code> +
<code>vendored</code> features plus <code>signal-hook 0.3</code> and
<code>anyhow 1</code> (Rust); the host's own <code>lua</code> is 5.4.8
(PUC-Rio), driving <code>verify.lua</code> itself. Not exercised: this
example is <code>mode: local</code> per <code>examples/manifest.yaml</code> —
there is no VM or LGTM path for it, so no lab-guest or telemetry run
applies here.</p>
