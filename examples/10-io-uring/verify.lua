-- Verify fwatch v2: build a fixture tree, then assert observable behavior of
-- every subcommand — exact scan/sync counters, byte-equal copies (diff -r),
-- inotify event lines from watch, and the Go uring refusal (exit 64).
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

local function demo(args)
  return checks.run("./demo.sh " .. lang .. " run " .. args)
end

-- Fixture: 5 regular files (1348634 bytes total) across nested dirs, plus an
-- empty directory and an empty file. Sizes straddle the engines' 128 KiB
-- chunk: b.bin needs 3 chunks, d.bin needs 9 (i.e. two 8-pair uring batches).
local ws = checks.run("mktemp -d").out:gsub("%s+$", "")
assert(ws:match("^/"), "mktemp -d must yield an absolute path")
local src = ws .. "/src"
checks.run(string.format([[
  mkdir -p %s/sub/deep %s/emptydir
  printf 'hello fwatch\n' > %s/a.txt
  head -c 300000 /dev/urandom > %s/sub/b.bin
  head -c 1048581 /dev/urandom > %s/sub/deep/d.bin
  printf '0123456789012345678901234567890123456789' > %s/sub/c.txt
  : > %s/empty.dat
]], src, src, src, src, src, src, src))

-- No arguments: usage on stderr, exit 2.
local r = demo("")
checks.expect_exit(r, 2, lang .. ": bare invocation exits 2")
checks.expect_match(r.out, "usage: fwatch", lang .. ": prints usage")

-- v0: scan reports the exact file and byte totals.
r = demo("scan " .. src)
checks.expect_exit(r, 0, lang .. ": scan exits 0")
checks.expect_match(r.out, "scanned 5 files 1348634 bytes", lang .. ": scan totals")

-- v2: sync with the rw engine — counters plus a byte-identical tree.
local dst_rw = ws .. "/dst-rw"
r = demo("sync " .. src .. " " .. dst_rw .. " --engine rw")
checks.expect_exit(r, 0, lang .. ": sync rw exits 0")
checks.expect_match(r.out, "synced 5 files 1348634 bytes engine=rw ms=%d+",
  lang .. ": sync rw summary")
r = checks.run("diff -r " .. src .. " " .. dst_rw)
checks.expect_exit(r, 0, lang .. ": rw copy is byte-equal (diff -r)")

-- v2: the uring engine. Go refuses by design (exit 64); C++ (raw syscalls +
-- mmap'd rings) and Rust (io-uring crate) must produce an identical tree.
local dst_uring = ws .. "/dst-uring"
r = demo("sync " .. src .. " " .. dst_uring .. " --engine uring")
if lang == "go" then
  checks.expect_exit(r, 64, "go: engine=uring exits 64")
  checks.expect_match(r.out, "engine=uring: unsupported in Go %(see chapter 10%)",
    "go: uring refusal message")
else
  checks.expect_exit(r, 0, lang .. ": sync uring exits 0")
  checks.expect_match(r.out, "synced 5 files 1348634 bytes engine=uring ms=%d+",
    lang .. ": sync uring summary")
  r = checks.run("diff -r " .. src .. " " .. dst_uring)
  checks.expect_exit(r, 0, lang .. ": uring copy is byte-equal (diff -r)")
end

-- v1: watch emits inotify event lines. Create a file 0.4 s into the watch;
-- creating-then-writing yields CREATE (and usually MODIFY) events.
local watched = ws .. "/watched"
checks.run("mkdir -p " .. watched)
r = checks.run(string.format(
  "( sleep 0.4; printf x > %s/newfile.txt ) & ./demo.sh %s run watch %s --timeout 1500",
  watched, lang, watched))
checks.expect_exit(r, 0, lang .. ": watch exits 0 after its timeout")
checks.expect_match(r.out, "event CREATE newfile%.txt", lang .. ": CREATE event line")
checks.expect_match(r.out, "watched %d+ events", lang .. ": watch summary line")

checks.run("rm -rf " .. ws)
checks.finish()
