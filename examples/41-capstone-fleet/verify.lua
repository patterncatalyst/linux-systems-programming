-- Verify the hello-syscall template: run the demo for LSP_LANG and check its output.
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
checks.expect_match(r.out, "pid %d+ on Linux", lang .. ": prints pid/uname line")
checks.finish()
