-- Verify 43-rust-macros-for-systems (Rust only).
--
-- Asserts the observable output of the three techniques the example teaches:
-- a #[derive(SysError)] that generates Display from #[error("...")] strings, an
-- #[instrument] attribute macro that logs a function's entry/exit/timing, and a
-- sound unsafe unaligned read. The gate runs on the pinned STABLE toolchain;
-- miri (nightly) is exercised by the chapter, not here.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "rust" then
  checks.skip("43-rust-macros-for-systems is Rust-only (LSP_LANG=" .. tostring(lang) .. ")")
end

local b = checks.run("./demo.sh rust build")
checks.expect_exit(b, 0, "rust: builds (workspace: proc-macro crate + app)")

local r = checks.run("./demo.sh rust run")
checks.expect_exit(r, 0, "rust: run exits 0")

-- #[derive(SysError)]: Display generated from #[error("...")] with field interpolation.
checks.expect_match(r.out, "error: open /etc/shadow failed: 13",
  "derive: named-field error Display")
checks.expect_match(r.out, "error: child 4242 exited with status 1",
  "derive: multi-field error Display")
checks.expect_match(r.out, "error: policy rejected: sandbox blocked os%.execute",
  "derive: tuple-field error Display")

-- #[instrument]: entry line names the fn + args, exit line carries the return
-- value and a measured duration.
checks.expect_match(r.out, "%-> hash_chunk%(len=4096%)",
  "instrument: entry logs fn + args")
checks.expect_match(r.out, "<%- hash_chunk = %d+ %(%d+us%)",
  "instrument: exit logs return value + duration")

-- Sound unsafe: an unaligned read of a repr(C) record out of a byte slice.
checks.expect_match(r.out, "record: magic=0x4b565053 slots=256",
  "unsafe: unaligned read parses the record")

checks.finish()
