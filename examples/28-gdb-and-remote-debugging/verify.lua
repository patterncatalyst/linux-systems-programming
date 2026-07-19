-- Verify bugfarm scenario 1: `app run` is a normal, dual-use program;
-- `app crash` is a debugging TARGET whose call chain is identical across
-- languages (main -> run_supervisor -> handle_child -> deref) but whose
-- observable crash mechanism is NOT -- that difference is the chapter:
--   * C++: a real SIGSEGV, exit 139, a systemd-coredump core recoverable
--     with coredumpctl, and a scripted `gdb --batch -ex run -ex bt` backtrace
--     naming every frame.
--   * Go: the runtime turns the same class of bug into a panic, not a core.
--     Exit 2, and the goroutine trace (not gdb) names every frame.
--   * Rust: `.unwrap()` on a `None` panics too. Exit 101, and
--     RUST_BACKTRACE=1 (not gdb) names every frame.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set. Plain Lua 5.4 (checks.lua has no deps).

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local bins = {
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
}
local bin = bins[lang]

local example_dir = os.getenv("EXAMPLE_DIR")
if example_dir == nil or example_dir == "" then
  example_dir = checks.run("pwd").out:gsub("%s+$", "")
end
local abs_bin = example_dir .. "/" .. bin

-- Boolean assertion via the checks pattern API (same trick as ch12's verify).
local function expect_true(cond, label)
  checks.expect_match(cond and "yes" or "no", "yes", label)
end

-- Ensure the binary exists (the orchestrator builds before verifying; a
-- standalone run should fail loudly here instead of deep in a scenario).
local r = checks.run("test -x " .. bin)
checks.expect_exit(r, 0, lang .. ": built binary " .. bin .. " exists")

-- 1. Usage: bare invocation and an unknown subcommand both exit 2 with the
--    identical two-line usage block, across all three languages.
r = checks.run("./" .. bin)
checks.expect_exit(r, 2, lang .. ": bare invocation exits 2")
checks.expect_match(r.out, "usage: app run", lang .. ": prints usage")
r = checks.run("./" .. bin .. " bogus")
checks.expect_exit(r, 2, lang .. ": unknown subcommand exits 2")

-- 2. "run" is the normal program: identical dual-use output, exit 0.
r = checks.run("./" .. bin .. " run")
checks.expect_exit(r, 0, lang .. ": run exits 0")
checks.expect_match(r.out, "pmon: supervisor starting", lang .. ": run starts")
checks.expect_match(r.out, "pmon: child 4242 exited status=0",
  lang .. ": run reports the reaped child")
checks.expect_match(r.out, "pmon: supervisor exiting cleanly",
  lang .. ": run reaches its normal cleanup line")

-- 3. "crash" -- each language's real crash story.
if lang == "cpp" then
  -- 3a. A real SIGSEGV: exit 139, and the RAII ShutdownGuard's destructor
  --     never runs (a signal is not stack unwinding).
  r = checks.run("./" .. bin .. " crash")
  checks.expect_exit(r, 139, lang .. ": crash dies to SIGSEGV (exit 139)")
  checks.expect_match(r.out, "pmon: supervisor starting",
    lang .. ": crash gets as far as starting")
  expect_true(not r.out:find("supervisor exiting cleanly", 1, true),
    lang .. ": RAII guard's destructor does not run on SIGSEGV")

  -- 3b. Core-enabling wrapper: ulimit -c unlimited, run in a temp dir, and
  --     confirm a core was actually dropped. This host's core_pattern pipes
  --     through systemd-coredump (`cat /proc/sys/kernel/core_pattern`
  --     starts with '|'), so "drops a core file" means coredumpctl can find
  --     and extract one into the temp dir -- not a bare `core` file
  --     appearing there on its own.
  local ws = checks.run("mktemp -d").out:gsub("%s+$", "")
  assert(ws:match("^/"), "mktemp -d must yield an absolute path")
  r = checks.run(string.format([[
    cd %s
    ( ulimit -c unlimited; %s crash >/dev/null 2>&1 & echo $! >pidfile; wait $! )
    code=$?
    pid=$(cat pidfile)
    echo "child_exit=$code"
    found=0
    for i in $(seq 1 100); do
      if coredumpctl info "$pid" >/dev/null 2>&1; then found=1; break; fi
      sleep 0.1
    done
    echo "coredump_found=$found"
    if [ "$found" = "1" ] && coredumpctl dump "$pid" -o core.app >/dev/null 2>&1 \
        && [ -s core.app ]; then
      echo "core_file=yes"
    else
      echo "core_file=no"
    fi
  ]], ws, abs_bin))
  checks.expect_match(r.out, "child_exit=139",
    lang .. ": crash under ulimit -c unlimited still dies to SIGSEGV")
  checks.expect_match(r.out, "coredump_found=1",
    lang .. ": systemd-coredump recorded the crash")
  checks.expect_match(r.out, "core_file=yes",
    lang .. ": a core file was dropped in the temp dir")
  checks.run("rm -rf " .. ws)

  -- 3c. A scripted gdb session on the crash names the expected frames.
  r = checks.run(string.format(
    "gdb --batch -ex 'set pagination off' -ex 'set debuginfod enabled off' " ..
    "-ex run -ex bt --args %s crash", abs_bin))
  checks.expect_match(r.out, "SIGSEGV", lang .. ": gdb reports the SIGSEGV")
  checks.expect_match(r.out, "deref", lang .. ": gdb backtrace names deref")
  checks.expect_match(r.out, "handle_child",
    lang .. ": gdb backtrace names handle_child")
  checks.expect_match(r.out, "run_supervisor",
    lang .. ": gdb backtrace names run_supervisor")
elseif lang == "go" then
  -- Go does not core the same way: the runtime catches the hardware SIGSEGV
  -- itself and turns it into an ordinary (unrecovered) panic. Assert the
  -- panic's goroutine trace names every frame instead of a gdb backtrace.
  r = checks.run("./" .. bin .. " crash")
  checks.expect_exit(r, 2, lang .. ": nil-deref panic exits 2 (not 139, no core)")
  checks.expect_match(r.out, "pmon: supervisor starting",
    lang .. ": crash gets as far as starting")
  checks.expect_match(r.out,
    "panic: runtime error: invalid memory address or nil pointer dereference",
    lang .. ": panics with the standard nil-deref message")
  checks.expect_match(r.out, "main%.deref",
    lang .. ": goroutine trace names deref")
  checks.expect_match(r.out, "main%.handleChild",
    lang .. ": goroutine trace names handleChild")
  checks.expect_match(r.out, "main%.runSupervisor",
    lang .. ": goroutine trace names runSupervisor")
  expect_true(not r.out:find("supervisor exiting cleanly", 1, true),
    lang .. ": no code past the panic point runs")
elseif lang == "rust" then
  -- Rust's equivalent bug class has no raw pointer to leave dangling: it is
  -- an Option that should have been filled in and was not. unwrap() panics
  -- (exit 101, no core); RUST_BACKTRACE=1 names every frame, opt-in.
  r = checks.run("RUST_BACKTRACE=1 ./" .. bin .. " crash")
  checks.expect_exit(r, 101, lang .. ": unwrap-on-None panics, exit 101 (not 139, no core)")
  checks.expect_match(r.out, "pmon: supervisor starting",
    lang .. ": crash gets as far as starting")
  checks.expect_match(r.out, "panicked at src/main.rs",
    lang .. ": panics with a source location")
  checks.expect_match(r.out, "app::deref", lang .. ": backtrace names deref")
  checks.expect_match(r.out, "app::handle_child",
    lang .. ": backtrace names handle_child")
  checks.expect_match(r.out, "app::run_supervisor",
    lang .. ": backtrace names run_supervisor")
  expect_true(not r.out:find("supervisor exiting cleanly", 1, true),
    lang .. ": no code past the panic point runs")

  -- Without RUST_BACKTRACE, the same panic and exit code happen, but no
  -- frame names are printed -- documents that the backtrace is opt-in.
  r = checks.run("./" .. bin .. " crash")
  checks.expect_exit(r, 101,
    lang .. ": panic exit code is unchanged without a backtrace")
  expect_true(not r.out:find("app::deref", 1, true),
    lang .. ": RUST_BACKTRACE=1 is required to see frame names")
end

checks.finish()
