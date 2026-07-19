-- Verify workq (25-shared-state-and-the-futex) for LSP_LANG.
--
-- Asserts the CORRECT path's observable behavior, identical across cpp/go/rust
-- (the --buggy path is a sanitizer/race-detector chapter demo, deliberately
-- NOT exercised here):
--   1. error contract: no args / unknown flag / missing required / bad integer
--      / missing value all print the usage line and exit 2
--   2. a fixed run (P=4 C=4 N=100000, default seed) prints
--        workq: produced=100000 consumed=100000 checksum=42b3746c6cee7465 ms=<t>
--      the checksum is the SAME 64-bit constant for all three languages
--   3. determinism: the checksum does not depend on P, C, or the queue cap —
--      four different shapes over the same (N, seed) all yield that constant,
--      and every run reports produced==consumed==N
--   4. a different seed yields a different-but-shared constant, and N=0 yields
--      produced==consumed==0 with an all-zero checksum
--
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and
-- EXAMPLE_DIR set; the interpreter may be lua or luajit.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local demo = "./demo.sh " .. lang .. " run"

-- Precomputed cross-language constants (see README "Wire/value contract").
local CK_DEFAULT = "42b3746c6cee7465"  -- N=100000, default seed 0x0123456789ABCDEF
local CK_SEED42  = "fceb5608a4b73dc0"  -- N=50000,  seed 42
local N = 100000

-- expect_true: route a computed boolean through expect_exit so it lands in the
-- same pass/fail accounting as everything else.
local function expect_true(cond, label)
  checks.expect_exit({ exit = cond and 0 or 1 }, 0, label)
end

local USAGE = "usage: workq %-%-producers P %-%-consumers C %-%-items N"

-- ---------------------------------------------------------------------------
-- 1. error contract
-- ---------------------------------------------------------------------------

local noargs = checks.run(demo)
checks.expect_exit(noargs, 2, lang .. ": no arguments exits 2")
checks.expect_match(noargs.out, USAGE, lang .. ": no-args prints the usage line")

local unknown = checks.run(demo .. " --producers 1 --consumers 1 --items 4 --frob")
checks.expect_exit(unknown, 2, lang .. ": unknown flag exits 2")
checks.expect_match(unknown.out, "workq: unknown flag: %-%-frob", lang .. ": names the unknown flag")

local missing = checks.run(demo .. " --producers 2 --items 4")
checks.expect_exit(missing, 2, lang .. ": missing --consumers exits 2")
checks.expect_match(missing.out, USAGE, lang .. ": missing-required prints usage")

local badint = checks.run(demo .. " --producers x --consumers 1 --items 4")
checks.expect_exit(badint, 2, lang .. ": non-integer value exits 2")
checks.expect_match(badint.out, "workq: not an integer: x", lang .. ": names the bad integer")

local noval = checks.run(demo .. " --producers")
checks.expect_exit(noval, 2, lang .. ": flag without a value exits 2")
checks.expect_match(noval.out, "workq: %-%-producers needs a value", lang .. ": names the value-less flag")

-- ---------------------------------------------------------------------------
-- 2. fixed run: shared checksum constant, produced==consumed==N
-- ---------------------------------------------------------------------------

local base = checks.run(demo .. " --producers 4 --consumers 4 --items " .. N)
checks.expect_exit(base, 0, lang .. ": correct run exits 0")
checks.expect_match(base.out,
  "workq: produced=" .. N .. " consumed=" .. N .. " checksum=" .. CK_DEFAULT .. " ms=%d+",
  lang .. ": produced==consumed==N and checksum matches the cross-language constant")

-- ---------------------------------------------------------------------------
-- 3. determinism: checksum independent of P, C, cap
-- ---------------------------------------------------------------------------

local shapes = {
  "--producers 1 --consumers 1 --items " .. N .. " --cap 8",
  "--producers 8 --consumers 3 --items " .. N .. " --cap 1024",
  "--producers 2 --consumers 16 --items " .. N .. " --cap 4",
}
for _, shape in ipairs(shapes) do
  local r = checks.run(demo .. " " .. shape)
  checks.expect_exit(r, 0, lang .. ": shape [" .. shape .. "] exits 0")
  local produced = r.out:match("produced=(%d+)")
  local consumed = r.out:match("consumed=(%d+)")
  local checksum = r.out:match("checksum=(%x+)")
  expect_true(produced == tostring(N) and consumed == tostring(N),
    lang .. ": [" .. shape .. "] produced==consumed==N (got p=" ..
    tostring(produced) .. " c=" .. tostring(consumed) .. ")")
  expect_true(checksum == CK_DEFAULT,
    lang .. ": [" .. shape .. "] checksum is order-independent (" ..
    tostring(checksum) .. ")")
end

-- ---------------------------------------------------------------------------
-- 4. a different seed => different shared constant; N=0 => zero checksum
-- ---------------------------------------------------------------------------

local seeded = checks.run(demo .. " --producers 3 --consumers 3 --items 50000 --seed 42")
checks.expect_exit(seeded, 0, lang .. ": seeded run exits 0")
checks.expect_match(seeded.out,
  "workq: produced=50000 consumed=50000 checksum=" .. CK_SEED42 .. " ms=%d+",
  lang .. ": seed 42 yields its own cross-language constant")

local empty = checks.run(demo .. " --producers 3 --consumers 2 --items 0")
checks.expect_exit(empty, 0, lang .. ": N=0 run exits 0")
checks.expect_match(empty.out,
  "workq: produced=0 consumed=0 checksum=0000000000000000 ms=%d+",
  lang .. ": empty queue drains to an all-zero checksum")

checks.finish()
