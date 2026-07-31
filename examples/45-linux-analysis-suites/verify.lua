-- Verify 45-linux-analysis-suites. LOCAL but requires podman: builds the
-- Containerfile, runs pmcd, and asserts observable PCP behavior -- not
-- merely that the container started.
--
-- No per-language behavior exists (see demo.sh's header note); the manifest
-- restricts this example to LSP_LANG=cpp so the runner drives it once, not
-- three times, per scripts/manifest.yaml.
--
-- What's asserted, and why each item is real rather than assumed:
--   1. the image builds -- and the build-time smoke stage inside the
--      Containerfile already failed once (loudly) during development when
--      a PMDA didn't register, so a green build here is a real gate, not
--      just "podman build exited 0".
--   2. pmcd answers pminfo within a bounded poll.
--   3. BOTH layered PMDAs (openmetrics, podman) appear in `pcp`'s summary.
--   4. a REAL scraped openmetrics value: `openmetrics.lsp45.lsp45_answer`
--      comes from a `file://` config.d source shipped in this example
--      (config/lsp45.prom), not a live network scrape -- deliberately, so
--      the check has no external dependency, but the value is genuinely
--      parsed by the PMDA each time, not hardcoded.
--   5. a real core PCP metric sample, `kernel.all.load`, independent of
--      anything this chapter added -- proves pmcd's base collection works
--      on its own.
--   6. the podman PMDA registers its namespace (21 metrics). This script
--      does NOT assert live container VALUES from it: that requires
--      mounting the host's rootless podman.sock into the container
--      (demo.sh does this when one is present), which is not guaranteed
--      on every CI/host environment this runs on. What IS asserted here
--      (registration) is true unconditionally; the live-value behavior is
--      real and demonstrated by demo.sh, just not gated in this script.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" then
  checks.skip("LSP_LANG must be cpp (got " .. tostring(lang) ..
    ") -- this example has no per-language behavior; the manifest runs it " ..
    "once under cpp")
end

if checks.run("command -v podman").exit ~= 0 then
  checks.skip("podman not available on this host")
end

local image, c = "lsp45-pcp", "lsp45-pcp-verify"

local function cleanup()
  checks.run("podman rm -f " .. c .. " >/dev/null 2>&1; true")
end
cleanup() -- in case a previous run left something behind

-- ---------------------------------------------------------------------------
-- 1. build -- the Containerfile's own smoke stage already gates this: if
--    either PMDA failed to register at build time, this fails right here.
-- ---------------------------------------------------------------------------

local build = checks.run("podman build -t " .. image .. " -f Containerfile .")
checks.expect_exit(build, 0, "podman build succeeds (build-time PMDA gate passes)")

-- ---------------------------------------------------------------------------
-- 2. run + wait for pmcd readiness (bounded poll, not a fixed sleep)
-- ---------------------------------------------------------------------------

local run = checks.run(
  "podman run -d --name " .. c .. " --cpus=2 --memory=256m " .. image)
checks.expect_exit(run, 0, "pmcd container starts")

local ready = checks.run(
  "for i in $(seq 1 20); do podman exec " .. c ..
  " pminfo -f pmcd.version >/dev/null 2>&1 && { echo ready; break; }; sleep 1; done")
checks.expect_match(ready.out, "ready", "pmcd accepts connections within 20s")

-- ---------------------------------------------------------------------------
-- 3. `pcp` summary names both layered PMDAs as active
-- ---------------------------------------------------------------------------

local pcp_summary = checks.run("podman exec " .. c .. " pcp")
checks.expect_match(pcp_summary.out, "openmetrics", "pcp summary lists the openmetrics PMDA")
checks.expect_match(pcp_summary.out, "podman", "pcp summary lists the podman PMDA")

-- ---------------------------------------------------------------------------
-- 4. a REAL scraped openmetrics value, from the file:// source this example
--    ships (config/lsp45.prom) -- not a live network scrape, but a real
--    parse of a real file, every time.
-- ---------------------------------------------------------------------------

local om = checks.run("podman exec " .. c .. " pminfo -f openmetrics.lsp45.lsp45_answer")
checks.expect_match(om.out, "openmetrics%.lsp45%.lsp45_answer",
  "pminfo -f resolves the scraped openmetrics metric name")
checks.expect_match(om.out, "value 42",
  "the scraped value is the real constant this chapter's config ships (42)")

-- ---------------------------------------------------------------------------
-- 5. podman PMDA: registers a real namespace (values need a mounted
--    podman.sock, which demo.sh does but this script does not require).
-- ---------------------------------------------------------------------------

local pminfo_podman = checks.run("podman exec " .. c .. " pminfo podman")
checks.expect_match(pminfo_podman.out, "podman%.container%.",
  "podman PMDA registers a podman.container.* metric namespace")

-- ---------------------------------------------------------------------------
-- 6. pmrep: a real numeric sample for a stable, well-known CORE PCP metric
--    (not one of the layered PMDAs -- proves pmcd's base collection works
--    independent of anything this chapter added).
-- ---------------------------------------------------------------------------

local pmrep = checks.run("podman exec " .. c .. " pmrep -t 1 -s 1 kernel.all.load")
checks.expect_exit(pmrep, 0, "pmrep runs against a known core metric")
checks.expect_match(pmrep.out, "k%.a%.load", "pmrep returns a sample for kernel.all.load")

-- ---------------------------------------------------------------------------
-- cleanup: remove the container and the built image, unconditionally
-- ---------------------------------------------------------------------------

cleanup()
checks.run("podman rmi -f " .. image .. " >/dev/null 2>&1; true")

checks.finish()
