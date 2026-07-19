-- Verify sysprobe: run the demo for LSP_LANG and assert the labeled syscall
-- sequence — four "step=<name> ok" lines plus the summary, in order.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local r = checks.run("./demo.sh " .. lang .. " run")
checks.expect_exit(r, 0, lang .. ": demo exits 0")

-- Every step line and the summary must be present...
for _, step in ipairs({ "open", "write", "sleep", "random" }) do
  checks.expect_match(r.out, "step=" .. step .. " ok", lang .. ": step=" .. step .. " ok")
end
checks.expect_match(r.out, "sysprobe: 4 steps ok", lang .. ": summary line")

-- ...and in order, as one contiguous five-line block.
checks.expect_match(r.out,
  "step=open ok\nstep=write ok\nstep=sleep ok\nstep=random ok\nsysprobe: 4 steps ok",
  lang .. ": steps appear in order")

checks.finish()
