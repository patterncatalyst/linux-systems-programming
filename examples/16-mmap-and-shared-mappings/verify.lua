-- Verify 16-mmap-and-shared-mappings: shmkv v0, an mmap-backed KV store with
-- ONE on-disk format shared by all three languages. Asserts behavior: exact
-- header bytes (od), file sizes (stat), CLI stdout/stderr shapes and exit
-- codes — and, the point of the example, CROSS-LANGUAGE INTEROP: a store
-- created and written by the Rust binary is read by the Go and C++ binaries
-- (byte-identical dumps), then the reverse (created by C++, read by Rust),
-- with cmp(1) proving the store files themselves are byte-identical.
-- Invoked from the example directory with LSP_LANG (cpp|go|rust) and EXAMPLE_DIR set.

local script_dir = arg[0]:match("(.*/)") or "./"
local repo_root = os.getenv("REPO_ROOT") or (script_dir .. "../..")
package.path = package.path .. ";" .. repo_root .. "/scripts/lib/?.lua"
local checks = require("checks")

local lang = os.getenv("LSP_LANG")
if lang ~= "cpp" and lang ~= "go" and lang ~= "rust" then
  checks.skip("LSP_LANG must be cpp|go|rust (got " .. tostring(lang) .. ")")
end

-- Absolute example dir: each language's demo.sh runs the binary from its own
-- subdirectory, so relative FILE arguments would land there instead of here.
local dir = os.getenv("EXAMPLE_DIR")
if not dir or dir == "" or dir == "." then
  dir = checks.run("pwd").out:gsub("%s+$", "")
end

local demo = {
  cpp = "./demo.sh cpp run",
  go = "./demo.sh go run",
  rust = "./demo.sh rust run",
}
local bins = {
  cpp = "cpp/build/release/app",
  go = "go/bin/app",
  rust = "rust/target/release/app",
}

-- The interop checks drive all three binaries no matter which LSP_LANG this
-- pass is for; build any that the orchestrator has not built yet.
for l, bin in pairs(bins) do
  local f = io.open(dir .. "/" .. bin, "r")
  if f then f:close() else checks.run("./" .. l .. "/demo.sh build") end
end

local D = demo[lang]
local store = dir .. "/verify-" .. lang .. ".kv"
local scratch = {
  store, dir .. "/verify-" .. lang .. "-full.kv", dir .. "/verify-" .. lang .. "-bad.kv",
  dir .. "/verify-x.kv", dir .. "/verify-y.kv",
  dir .. "/verify-dump-cpp.txt", dir .. "/verify-dump-go.txt",
  dir .. "/verify-dump-rust.txt", dir .. "/verify-dump-y.txt",
}
for _, f in ipairs(scratch) do os.remove(f) end

-- ---------------------------------------------------------------- CLI shape
local r = checks.run(D)
checks.expect_exit(r, 2, lang .. ": no args exits 2")
checks.expect_match(r.out, "usage: shmkv create FILE %-%-slots N | set FILE KEY VALUE",
  lang .. ": no args prints usage")

r = checks.run(D .. " frobnicate " .. store)
checks.expect_exit(r, 2, lang .. ": unknown subcommand exits 2")

-- ------------------------------------------------- create: format on disk
r = checks.run(D .. " create " .. store .. " --slots 8")
checks.expect_exit(r, 0, lang .. ": create exits 0")
checks.expect_match(r.out, "created .*verify%-" .. lang .. "%.kv: 8 slots, 2058 bytes",
  lang .. ": create reports slots and bytes")

r = checks.run("stat -c %s " .. store)
checks.expect_match(r.out, "^2058%s*$", lang .. ": file is exactly 10 + 8*256 bytes")

-- Header, byte for byte: magic "SHKV1\0" then u32 LE slot_count == 8.
r = checks.run("od -An -tx1 -N10 " .. store)
checks.expect_match(r.out, "53 48 4b 56 31 00 08 00 00 00",
  lang .. ": header bytes are SHKV1\\0 + u32le 8")

-- ------------------------------------------------------- set / get / dump
r = checks.run(D .. " set " .. store .. " alpha one")
checks.expect_exit(r, 0, lang .. ": set exits 0")
checks.expect_match(r.out, "set alpha", lang .. ": set echoes the key")
checks.run(D .. " set " .. store .. " beta two")
r = checks.run(D .. " set " .. store .. " alpha uno") -- overwrite in place
checks.expect_exit(r, 0, lang .. ": overwrite set exits 0")

r = checks.run(D .. " get " .. store .. " alpha")
checks.expect_exit(r, 0, lang .. ": get exits 0")
checks.expect_match(r.out, "^uno\n$", lang .. ": get returns the overwritten value")

r = checks.run(D .. " get " .. store .. " gamma")
checks.expect_exit(r, 4, lang .. ": missing key exits 4")
checks.expect_match(r.out, "shmkv: key not found", lang .. ": missing key message")

r = checks.run(D .. " dump " .. store)
checks.expect_exit(r, 0, lang .. ": dump exits 0")
checks.expect_match(r.out, "alpha=uno\nbeta=two\n", lang .. ": dump sorted pairs")
checks.expect_match(r.out, "shmkv: 2/8 slots used",
  lang .. ": overwrite burned no extra slot (2/8 used)")

-- ---------------------------------------------------------- failure modes
local full = dir .. "/verify-" .. lang .. "-full.kv"
checks.run(D .. " create " .. full .. " --slots 2")
checks.run(D .. " set " .. full .. " k1 v1")
checks.run(D .. " set " .. full .. " k2 v2")
r = checks.run(D .. " set " .. full .. " k3 v3")
checks.expect_exit(r, 5, lang .. ": set on a full store exits 5")
checks.expect_match(r.out, "shmkv: store full %(2 slots%)", lang .. ": store-full message")

r = checks.run(D .. " set " .. full .. " " .. string.rep("k", 64) .. " v")
checks.expect_exit(r, 2, lang .. ": 64-byte key exits 2")
checks.expect_match(r.out, "shmkv: key too long %(max 63 bytes%)", lang .. ": key-too-long message")

r = checks.run(D .. " set " .. full .. " k " .. string.rep("v", 192))
checks.expect_exit(r, 2, lang .. ": 192-byte value exits 2")
checks.expect_match(r.out, "shmkv: value too long %(max 191 bytes%)",
  lang .. ": value-too-long message")

local badf = dir .. "/verify-" .. lang .. "-bad.kv"
checks.run("printf 'this is not a shmkv store, not even close' > " .. badf)
r = checks.run(D .. " get " .. badf .. " k")
checks.expect_exit(r, 1, lang .. ": non-store file exits 1")
checks.expect_match(r.out, "not a shmkv v0 store", lang .. ": bad-magic message")

r = checks.run(D .. " get " .. dir .. "/verify-does-not-exist.kv k")
checks.expect_exit(r, 1, lang .. ": missing file exits 1")
checks.expect_match(r.out, "shmkv: cannot open", lang .. ": cannot-open message")

r = checks.run(D .. " create " .. badf .. " --slots 0")
checks.expect_exit(r, 2, lang .. ": --slots 0 exits 2 with usage")

-- ------------------------------------------------- CROSS-LANGUAGE INTEROP
-- One store, written by Rust AND Go, read by everyone. This only works if
-- all three binaries agree on every byte of the format.
local x = dir .. "/verify-x.kv"
checks.run(demo.rust .. " create " .. x .. " --slots 6")
checks.run(demo.rust .. " set " .. x .. " zeta z1")
checks.run(demo.rust .. " set " .. x .. " alpha a1")
checks.run(demo.rust .. " set " .. x .. " mid m1")

r = checks.run(demo.go .. " get " .. x .. " alpha")
checks.expect_exit(r, 0, "interop: go reads rust-created store")
checks.expect_match(r.out, "^a1\n$", "interop: go gets rust-written value")

r = checks.run(demo.cpp .. " get " .. x .. " zeta")
checks.expect_exit(r, 0, "interop: cpp reads rust-created store")
checks.expect_match(r.out, "^z1\n$", "interop: cpp gets rust-written value")

r = checks.run(demo.go .. " set " .. x .. " gadd g1")
checks.expect_exit(r, 0, "interop: go writes into rust-created store")
r = checks.run(demo.cpp .. " get " .. x .. " gadd")
checks.expect_match(r.out, "^g1\n$", "interop: cpp sees the go-written key")

for l, d in pairs(demo) do
  r = checks.run(d .. " dump " .. x .. " > " .. dir .. "/verify-dump-" .. l .. ".txt")
  checks.expect_exit(r, 0, "interop: " .. l .. " dump exits 0")
  checks.expect_match(r.out, "shmkv: 4/6 slots used", "interop: " .. l .. " counts 4/6 used")
end
r = checks.run("cmp -s " .. dir .. "/verify-dump-cpp.txt " .. dir .. "/verify-dump-go.txt")
checks.expect_exit(r, 0, "interop: cpp and go dumps are byte-identical")
r = checks.run("cmp -s " .. dir .. "/verify-dump-cpp.txt " .. dir .. "/verify-dump-rust.txt")
checks.expect_exit(r, 0, "interop: cpp and rust dumps are byte-identical")
r = checks.run("cat " .. dir .. "/verify-dump-rust.txt")
checks.expect_match(r.out, "^alpha=a1\ngadd=g1\nmid=m1\nzeta=z1\n$",
  "interop: dump is the four pairs, key-sorted")

-- Reverse direction: C++ creates and replays the identical operations; Rust
-- reads it back; and the two store FILES must be byte-identical, since the
-- format (zero padding included) is fully deterministic.
local y = dir .. "/verify-y.kv"
checks.run(demo.cpp .. " create " .. y .. " --slots 6")
checks.run(demo.cpp .. " set " .. y .. " zeta z1")
checks.run(demo.cpp .. " set " .. y .. " alpha a1")
checks.run(demo.cpp .. " set " .. y .. " mid m1")
checks.run(demo.cpp .. " set " .. y .. " gadd g1")

r = checks.run(demo.rust .. " get " .. y .. " mid")
checks.expect_exit(r, 0, "interop: rust reads cpp-created store")
checks.expect_match(r.out, "^m1\n$", "interop: rust gets cpp-written value")

r = checks.run(demo.rust .. " dump " .. y .. " > " .. dir .. "/verify-dump-y.txt")
checks.expect_exit(r, 0, "interop: rust dump of cpp store exits 0")
r = checks.run("cmp -s " .. dir .. "/verify-dump-y.txt " .. dir .. "/verify-dump-rust.txt")
checks.expect_exit(r, 0, "interop: rust dump of cpp store matches its dump of the rust store")

r = checks.run("cmp -s " .. x .. " " .. y)
checks.expect_exit(r, 0, "interop: rust-built and cpp-built store files are byte-identical")

for _, f in ipairs(scratch) do os.remove(f) end
checks.finish()
