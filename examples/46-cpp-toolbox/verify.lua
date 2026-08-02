-- Verify 46-cpp-toolbox (C++ only): the C++ toolchain itself is the subject
-- -- CMake presets, Conan lockfiles, GCC/clang parity, gdb pretty-printers,
-- clang-tidy/clang-format. Every assertion below checks a real, tool-produced
-- effect, never merely a process exit code.
--
-- Hard-gated (A-C): deterministic on the pinned C++23 toolchain, always run.
--   A. `toolbox report` (FNV-1a digest, integer/string output only) is
--      byte-identical between a GCC build and a clang build, AND equals a
--      known literal.
--   B. The "ubsan" preset catches a deliberately seeded signed-integer
--      overflow (`toolbox defect overflow`) and prints UBSan's
--      "runtime error: signed integer overflow" line.
--   C. gdb, loaded with cpp/.gdbinit's Digest pretty-printer, renders the
--      custom Digest type as "Digest(0x<16 hex digits>)" instead of the raw
--      aggregate.
--
-- Gated-if-present (D-F): each tool's absence degrades to an informational
-- "SKIP:" print (not a checks.skip(), which would abort the whole script --
-- A-C must still run even if clang-tidy/clang-format/conan happen to be
-- missing). On the reference host all three are present, so these do run
-- and do count toward PASS/FAIL.
--   D. clang-tidy, via the compile-commands DB, flags src/smell.cpp's
--      deliberate performance-unnecessary-value-param smell.
--   E. clang-format --dry-run --Werror exits 0 on the tracked sources and
--      nonzero on the deliberately misformatted fixture.
--   F. `conan install` (against the committed lockfile) + the "conan" preset
--      build + run produces the expected line.
--
-- ccache/mold: opportunistic presence check only, printed but never
-- asserted -- both are expected absent on the reference host.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("46-cpp-toolbox is C++-only (LSP_LANG=" .. tostring(lang) .. ")")
end

-- Escape a literal string for use inside a Lua pattern (checks.expect_match
-- takes a pattern, not a plain substring).
local function lit(s)
  return (s:gsub("([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1"))
end

local function tool_present(cmd)
  return checks.run("command -v " .. cmd .. " >/dev/null 2>&1").exit == 0
end

-- ── A. GCC-vs-clang byte parity ──────────────────────────────────────────

local b = checks.run("./demo.sh cpp build")
checks.expect_exit(b, 0, "cpp: release (GCC) builds via ./demo.sh cpp build")

local rclang = checks.run(
  "cd cpp && cmake --preset release-clang >/dev/null && cmake --build --preset release-clang >/dev/null")
checks.expect_exit(rclang, 0, "cpp: release-clang builds")

local EXPECTED_REPORT =
  "toolbox report: label=[toolbox] report payload_len=16 digest=0x481984990deee5ff"

local gcc_report = checks.run("./cpp/build/release/toolbox report")
checks.expect_exit(gcc_report, 0, "cpp: GCC-built toolbox report runs")
checks.expect_match(gcc_report.out, lit(EXPECTED_REPORT),
  "cpp: GCC-built report equals the expected literal")

local clang_report = checks.run("./cpp/build/release-clang/toolbox report")
checks.expect_exit(clang_report, 0, "cpp: clang-built toolbox report runs")
checks.expect_match(clang_report.out, lit(EXPECTED_REPORT),
  "cpp: clang-built report equals the expected literal")

local parity = checks.run(
  "bash -c 'diff <(./cpp/build/release/toolbox report) <(./cpp/build/release-clang/toolbox report)'")
checks.expect_exit(parity, 0,
  "cpp: GCC vs clang report output is byte-identical (empty diff)")

-- ── B. UBSan recipe ───────────────────────────────────────────────────────

local ubsan_build = checks.run(
  "cd cpp && cmake --preset ubsan >/dev/null && cmake --build --preset ubsan >/dev/null")
checks.expect_exit(ubsan_build, 0, "cpp: ubsan preset builds")

local ubsan_run = checks.run("./cpp/build/ubsan/toolbox defect overflow")
checks.expect_exit(ubsan_run, 1, "cpp: ubsan build exits 1 on the seeded overflow")
checks.expect_match(ubsan_run.out, "runtime error: signed integer overflow",
  "cpp: UBSan reports 'runtime error: signed integer overflow'")

-- ── C. gdb pretty-printer ─────────────────────────────────────────────────

local debug_build = checks.run(
  "cd cpp && cmake --preset debug >/dev/null && cmake --build --preset debug >/dev/null")
checks.expect_exit(debug_build, 0, "cpp: debug preset builds")

local gdb_run = checks.run(
  "gdb -batch -x cpp/.gdbinit -ex 'b toolbox.cpp:30' -ex run -ex 'p d' " ..
  "--args cpp/build/debug/toolbox report")
checks.expect_exit(gdb_run, 0, "cpp: gdb batch run exits 0")
checks.expect_match(gdb_run.out, lit("Digest(0x481984990deee5ff)"),
  "cpp: gdb pretty-printer renders the Digest custom type")

-- ── D. clang-tidy (skip-if-absent) ───────────────────────────────────────

if tool_present("clang-tidy") then
  local tidy = checks.run("cd cpp && clang-tidy -p build/release src/smell.cpp 2>&1")
  checks.expect_match(tidy.out, "smell%.cpp:3.-performance%-unnecessary%-value%-param",
    "cpp: clang-tidy flags performance-unnecessary-value-param at smell.cpp:3")
else
  print("SKIP: clang-tidy not found on PATH -- gate D not asserted")
end

-- ── E. clang-format (skip-if-absent) ─────────────────────────────────────

if tool_present("clang-format") then
  local fmt_tracked = checks.run(
    "cd cpp && clang-format --dry-run --Werror src/*.cpp src/*.hpp conan/src/*.cpp")
  checks.expect_exit(fmt_tracked, 0, "cpp: clang-format --dry-run --Werror is clean on tracked sources")

  local fmt_fixture = checks.run(
    "cd cpp && clang-format --dry-run --Werror fixtures/misformatted.cpp")
  checks.expect_exit(fmt_fixture, 1, "cpp: clang-format flags the deliberately misformatted fixture")
else
  print("SKIP: clang-format not found on PATH -- gate E not asserted")
end

-- ── F. Conan 2 sub-target (skip-if-absent) ───────────────────────────────

if tool_present("conan") then
  local install = checks.run(
    "cd cpp && conan install conan --output-folder=build/conan --build=missing " ..
    "--lockfile=conan/conan.lock >/dev/null 2>&1")
  checks.expect_exit(install, 0, "cpp: conan install (against the committed lockfile) succeeds")

  local conan_build = checks.run(
    "cd cpp && cmake --preset conan >/dev/null && cmake --build --preset conan >/dev/null")
  checks.expect_exit(conan_build, 0, "cpp: conan preset configures and builds")

  local conan_run = checks.run("./cpp/build/conan/conan/toolbox-conan-demo")
  checks.expect_exit(conan_run, 0, "cpp: toolbox-conan-demo runs")
  checks.expect_match(conan_run.out, lit("toolbox-conan: fmt sub-target ok"),
    "cpp: toolbox-conan-demo prints the expected line")
else
  print("SKIP: conan not found on PATH -- gate F not asserted")
end

-- ── ccache/mold: opportunistic, informational only, never gated ─────────

if tool_present("ccache") then
  print("info: ccache present on PATH (see chapter for the cache-hit recipe)")
else
  print("info: ccache absent on this host -- ccache recipe shown in the chapter but not asserted")
end

if tool_present("mold") then
  print("info: mold present on PATH (see chapter for the parallel-linker recipe)")
else
  print("info: mold absent on this host -- mold recipe shown in the chapter but not asserted")
end

checks.finish()
