// pmon -- a restart/backoff supervisor whose policy lives in Lua, not in this
// binary. `policy.lua` (and its SIGHUP-swapped sibling `policy-v2.lua`) is
// loaded into a *sandboxed* mlua state: only base/string/table/math are
// opened, and the loader globals that could read files or eval new code from
// inside the sandbox (`load`, `loadfile`, `dofile`, `require`,
// `collectgarbage`, `loadstring`) are removed after the fact. `os`, `io`,
// `package` and `debug` are simply never opened, so a policy that reaches for
// `os.execute` hits a nil global -- see `run_check` and `policy-evil.lua`.
//
// Errors, three ways: CLI parse failures `bail!` out of `parse_args`;
// fallible host-side operations return `anyhow::Result` composed with
// `.context()`; and every boundary crossing into Lua (`exec`, `.call`, table
// `get`) is an `mlua::Result` that a protected load/call turns into data
// instead of a panic, so a bad policy degrades to a reported error, not a
// crash of the supervisor.

use std::env;
use std::fs;
use std::process::exit;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};
use mlua::{Function, Lua, LuaOptions, StdLib, Table, Value};

/// Wall-clock milliseconds since the UNIX epoch, exposed to policies as
/// `host.now_ms()`. "Monotonic-ish" is all the contract asks for -- policies
/// only use it for human-readable timestamps, never for scheduling.
fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as i64
}

/// Build a fresh, sandboxed Lua state: only base/string/table/math opened
/// (`os`/`io`/`package`/`debug` are never opened at all), then strip the
/// loader globals that could otherwise read files or evaluate fresh source
/// from inside the sandbox even without a filesystem library.
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

/// Load and execute the policy chunk under the sandbox (a protected load, so
/// a syntax error comes back as `Err` instead of aborting), then call its
/// optional `on_load()` hook. Both steps are `mlua::Result` all the way
/// through, so callers decide how to surface a failure.
fn load_policy(lua: &Lua, path: &str) -> mlua::Result<()> {
    let src = fs::read_to_string(path)
        .map_err(|e| mlua::Error::RuntimeError(format!("reading {path}: {e}")))?;
    lua.load(&src).set_name(path).exec()?;

    if let Ok(on_load) = lua.globals().get::<Function>("on_load") {
        on_load.call::<()>(())?;
    }
    Ok(())
}

/// One supervised-child exit event, exactly as the shared scenario spec
/// describes it. `signal` is never set in this chapter's scenario (every
/// event here is an exit-code death), so the `info` table simply never gets
/// a `signal` field -- matching "a nil field is absent from the table".
struct ExitEvent {
    name: &'static str,
    exit_code: i64,
    restarts: i64,
    consecutive_failures: i64,
    uptime_ms: i64,
    last_backoff_ms: i64,
}

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

fn phase1(lua: &Lua) -> mlua::Result<()> {
    decide(
        lua,
        &ExitEvent {
            name: "web",
            exit_code: 0,
            restarts: 0,
            consecutive_failures: 0,
            uptime_ms: 120_000,
            last_backoff_ms: 0,
        },
    )?;
    decide(
        lua,
        &ExitEvent {
            name: "worker",
            exit_code: 1,
            restarts: 0,
            consecutive_failures: 0,
            uptime_ms: 9_000,
            last_backoff_ms: 0,
        },
    )?;
    decide(
        lua,
        &ExitEvent {
            name: "worker",
            exit_code: 1,
            restarts: 1,
            consecutive_failures: 1,
            uptime_ms: 9_000,
            last_backoff_ms: 500,
        },
    )?;
    Ok(())
}

fn phase2(lua: &Lua) -> mlua::Result<()> {
    decide(
        lua,
        &ExitEvent {
            name: "api",
            exit_code: 1,
            restarts: 0,
            consecutive_failures: 0,
            uptime_ms: 5_000,
            last_backoff_ms: 0,
        },
    )?;
    decide(
        lua,
        &ExitEvent {
            name: "worker",
            exit_code: 1,
            restarts: 2,
            consecutive_failures: 2,
            uptime_ms: 9_000,
            last_backoff_ms: 1_000,
        },
    )?;
    Ok(())
}

/// The two-phase scenario: load v1, replay phase 1, block for a real SIGHUP,
/// reload the (now v2) policy from the same path, replay phase 2.
///
/// SIGHUP handling is the systems-programming content here: `signal-hook`
/// installs an async-signal-safe handler that only flips an `AtomicBool` (no
/// allocation, no Lua call, no I/O inside the signal handler itself), and
/// this thread polls that flag instead of doing any real work inside signal
/// context -- the same discipline ch12 uses for signalfd/self-pipe. The flag
/// is registered *before* we announce readiness, so a SIGHUP that lands the
/// instant after the "awaiting" line prints is never missed.
fn cmd_run(policy_path: &str) -> Result<()> {
    // mlua::Error is not Send+Sync in this (non-"send"-feature, single
    // threaded) build, so it cannot implement std::error::Error the way
    // anyhow::Context wants; fold it into an anyhow::Error by its Display
    // text instead of trying to attach `.context()` directly to an
    // `mlua::Result`.
    let lua = build_sandbox()
        .map_err(|e| anyhow::anyhow!("{e}"))
        .context("building sandbox")?;
    load_policy(&lua, policy_path)
        .map_err(|e| anyhow::anyhow!("{e}"))
        .context("loading policy")?;
    phase1(&lua)
        .map_err(|e| anyhow::anyhow!("{e}"))
        .context("phase 1 decisions")?;

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
    phase2(&lua)
        .map_err(|e| anyhow::anyhow!("{e}"))
        .context("phase 2 decisions")?;

    Ok(())
}

/// Load `policy_path` under the sandbox and run one probe `on_exit` call.
/// Returns the policy's declared version on success; the `mlua::Error` on
/// failure already carries the Lua-side message (a syntax error's location,
/// or -- for `policy-evil.lua` -- "attempt to index a nil value (global
/// 'os')", which is how the sandbox rejection gets reported without the host
/// knowing anything about what the policy tried to do).
fn run_check(policy_path: &str) -> mlua::Result<String> {
    let lua = build_sandbox()?;
    load_policy(&lua, policy_path)?;

    let version: String = lua.globals().get("POLICY_VERSION").unwrap_or_default();

    let probe = ExitEvent {
        name: "probe",
        exit_code: 1,
        restarts: 0,
        consecutive_failures: 0,
        uptime_ms: 9_000,
        last_backoff_ms: 0,
    };
    let info = info_table(&lua, &probe)?;
    let on_exit: Function = lua.globals().get("on_exit")?;
    let _decision: Table = on_exit.call(info)?;

    Ok(version)
}

fn cmd_check(policy_path: &str) -> ! {
    match run_check(policy_path) {
        Ok(version) => {
            println!("pmon: policy ok version={version}");
            exit(0);
        }
        Err(e) => {
            println!("pmon: policy error: {e}");
            exit(1);
        }
    }
}

const USAGE: &str = "usage: app <run|check> --policy <path>";

/// CLI parse failures are the one class of error that never touches Lua or
/// the filesystem, so they get the plain `bail!` treatment instead of the
/// `mlua::Result` plumbing used everywhere else.
fn parse_args(args: &[String]) -> Result<(String, String)> {
    if args.len() < 2 {
        bail!("{USAGE}");
    }
    let sub = args[1].clone();
    if sub != "run" && sub != "check" {
        bail!("{USAGE}");
    }
    let rest = &args[2..];
    if rest.len() != 2 || rest[0] != "--policy" {
        bail!("{USAGE}");
    }
    Ok((sub, rest[1].clone()))
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let (sub, policy_path) = match parse_args(&args) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("{e}");
            exit(2);
        }
    };

    match sub.as_str() {
        "run" => {
            if let Err(e) = cmd_run(&policy_path) {
                eprintln!("pmon: error: {e:?}");
                exit(1);
            }
        }
        "check" => cmd_check(&policy_path),
        _ => unreachable!("parse_args only returns run|check"),
    }
}
