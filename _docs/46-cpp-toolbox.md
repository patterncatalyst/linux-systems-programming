---
title: "The C++ toolbox: CMake presets, Conan lockfiles, and GCC-vs-clang parity"
order: 46
part: "Appendices: Tooling"
description: "cpp-toolbox is one C++23 binary and the full toolchain wrapped around it -- seven CMakePresets.json configurations sharing one _base, a committed Conan 2 lockfile for an isolated fmt sub-target, and a GCC-vs-clang parity gate on toolbox report -- read by clang-tidy, clang-format, and a custom gdb Python pretty-printer, with ccache and mold demonstrated but absent on the reference host. Verified on the Fedora 44 host: verify.lua PASS 20/FAIL 0, GCC and clang release builds byte-identical (digest=0x481984990deee5ff, diff empty), UBSan catching a seeded signed-integer overflow at toolbox.cpp:43, and gdb rendering Digest(0x481984990deee5ff) through toolbox-printers.py."
duration: "50 minutes"
---

Chapter 45 opened this part with three suites that watch a whole fleet from
outside — Cockpit's console, SystemTap's kernel modules, PCP's pull-based
metrics — and none of them cared what language produced the process they
were watching. This chapter turns all the way around: one language, one
small program, and every tool that touches it before it ever runs. Chapter
31 already gave C++ one paragraph of toolbelt each — `perf`, `clang-tidy`,
GCC's `-fanalyzer` — on the way to covering Go and Rust too. This chapter
reopens exactly that toolchain and does not move on. `toolbox` is a single
C++23 binary, engineered so that every tool pointed at it — two compilers,
a build-system generator, a package manager, a static analyzer, a
formatter, a debugger that needs help understanding a custom type — has
something concrete to catch or confirm, not just a clean exit.

The shape of the problem is specific to C++. Go ships one command
(`go build`) and Rust ships one command (`cargo build`); C++ ships a
language standard and leaves the rest — which compiler, which generator,
which package manager, whether the debugger understands your types out of
the box — as choices a project has to make explicit. `toolbox` makes seven
of those choices as seven named `CMakePresets.json` configurations, pins a
Conan 2 dependency to an exact, committed lockfile, and prints one
deterministic digest so that GCC and clang can be asked, byte for byte,
whether they agree.

{% include excalidraw.html
   file="46-cmake-preset-graph"
   alt="A tree diagram. At the top, a hidden _base configure preset (generator Ninja, binaryDir sourceDir/build/presetName) fans out to six visible presets: release (RelWithDebInfo, GCC), debug (Debug, GCC), asan (Debug + -fsanitize=address,undefined), and ubsan (Debug + -fsanitize=undefined -fno-sanitize-recover=undefined) sit in one column; release-clang and debug-clang sit beside them, each drawn inheriting from release/debug respectively with an arrow labeled inherits plus a CMAKE_CXX_COMPILER=clang++ override. Each of the six feeds its own build/<presetName> directory box below it. A seventh preset, conan, also inherits _base but is drawn held apart in a dashed box labeled isolated -- only reachable when TOOLBOX_ENABLE_CONAN=ON, needed by no other preset, so the default build never touches Conan. A caption note names which preset the parity gate uses (release vs release-clang), which the UBSan gate uses (ubsan), and which gdb uses (debug)."
   caption="Figure 46.1 — the CMakePresets inheritance graph: one _base config fanning out to release/debug/asan/ubsan and their clang twins, each into build/${presetName}, with the Conan sub-target preset held apart so the default build needs no Conan" %}

> **Tools used** — `cmake` and `ninja` (host; both are hard requirements in
> `scripts/check-host.sh`), `g++` and `clang++` (host; both are hard
> requirements in `scripts/check-host.sh`), `clang-tidy` and `clang-format`
> (host; ship from the same LLVM toolchain `check-host.sh` verifies via
> `clang`, but are not separately gated by it), `gdb` plus its Python
> pretty-printer extension (host; `gdb` is a hard requirement in
> `scripts/check-host.sh`), `conan` (host; a soft/warn entry in
> `scripts/check-host.sh` — a missing Conan degrades that section's gate to
> an informational `SKIP:`, it never fails the example), and `ccache`/`mold`
> (host; demonstrated in this chapter's recipes but absent on the reference
> host, so those two sections are marked unverified below). No VM, no root,
> no LGTM stack — `examples/46-cpp-toolbox` is `mode: local` in
> `examples/manifest.yaml`.

Because the C++ toolchain is itself the subject, `examples/46-cpp-toolbox/`
ships `langs: [cpp]` only — a Go or Rust build would have nothing new to
say about a `CMakePresets.json` file or a `.clang-tidy` config. Every code
block below is a plain fenced `cpp`/`console`/`json` block, the way Chapter
44's Go runtime chapter presented Go alone; there is no language-tab
include to reach for when there is only one language in the room.

## CMake presets in depth

`cpp/CMakePresets.json` declares schema `"version": 6` and one hidden base
every other preset inherits from:

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "_base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
```

`hidden: true` keeps `_base` out of every preset-selection UI and off
`cmake --list-presets` — it exists only to be inherited from, never chosen
directly. The one line worth reading twice is `binaryDir`:
{% raw %}`"${sourceDir}/build/${presetName}"`{% endraw %} means every preset
that inherits `_base` gets its own build directory for free, named after
itself — `build/release`, `build/debug`, `build/asan`, and so on — with no
preset having to repeat the path. `{% raw %}${presetName}{% endraw %}` is
CMake Presets' own variable, resolved at configure time to whichever
preset's name you passed to `--preset`; it is not something this chapter's
own tooling or Jekyll ever expands.

Six more presets inherit `_base` (a seventh, `conan`, is covered on its
own below):

| Preset | Inherits | Compiler | Build type / flags |
|---|---|---|---|
| `release` | `_base` | GCC (default) | `RelWithDebInfo` (`-O2 -g`) |
| `debug` | `_base` | GCC (default) | `Debug` |
| `asan` | `_base` | GCC (default) | `Debug`, `-fsanitize=address,undefined` |
| `ubsan` | `_base` | GCC (default) | `Debug`, `-fsanitize=undefined -fno-sanitize-recover=undefined` |
| `release-clang` | `release` | clang | same as `release`, compiler overridden |
| `debug-clang` | `debug` | clang | same as `debug`, compiler overridden |

`release-clang` and `debug-clang` show the other half of `inherits`: they
inherit from `release`/`debug` — not from `_base` — and add exactly two
cache variables on top:

```json
    {
      "name": "release-clang",
      "inherits": "release",
      "displayName": "Release (clang)",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++"
      }
    },
```

Everything else — the `RelWithDebInfo` build type, `binaryDir`, compile
commands export — comes along from `release` unchanged; only the compiler
differs. That is the whole mechanism the parity gate below leans on: two
presets that are identical except for which compiler builds them. `asan`
and `ubsan` look similar to each other but serve different jobs on
purpose: `asan` combines `-fsanitize=address,undefined` with no
`-fno-sanitize-recover` — a general bug-hunting build that keeps running
and reports every violation it finds — while `ubsan` adds
`-fno-sanitize-recover=undefined`, so the *first* undefined-behavior
violation aborts the process immediately with a nonzero exit. That
determinism is exactly what a gate that has to produce a fixed exit code
needs, which is why "UBSan recipe" below reaches for `ubsan`, not `asan`.

`buildPresets` is a flat, 1:1 mirror of the seven configure presets — each
build preset just names the configure preset it drives:

```json
  "buildPresets": [
    { "name": "release",       "configurePreset": "release" },
    { "name": "debug",         "configurePreset": "debug" },
    { "name": "asan",          "configurePreset": "asan" },
    { "name": "ubsan",         "configurePreset": "ubsan" },
    { "name": "release-clang", "configurePreset": "release-clang" },
    { "name": "debug-clang",   "configurePreset": "debug-clang" },
    { "name": "conan",         "configurePreset": "conan" }
  ]
```

Once a configure preset exists, its build preset is `cmake --build
--preset <name>` and nothing more — no per-preset generator flags, no
target list, because `binaryDir` and `generator` already came from
`_base`.

## Conan 2 lockfiles

The seventh preset, `conan`, is the odd one out on purpose:

```json
    {
      "name": "conan",
      "inherits": "_base",
      "displayName": "Conan 2 sub-target (fmt, isolated)",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/build/conan/generators/conan_toolchain.cmake",
        "TOOLBOX_ENABLE_CONAN": "ON"
      }
    }
```

`TOOLBOX_ENABLE_CONAN` is a CMake `option()` defined in
`cpp/CMakeLists.txt`, off by default:

```
option(TOOLBOX_ENABLE_CONAN "Build the Conan-provided fmt sub-target" OFF)
if(TOOLBOX_ENABLE_CONAN)
    add_subdirectory(conan)
endif()
```

Only the `conan` preset turns it `ON`; every other preset — including
`release`, what `./demo.sh cpp build` and fedora:44's CI build-smoke both
use — never evaluates `cpp/conan/` and never needs Conan installed at all.
That isolation is the whole design decision this section is about: a
tutorial reader without Conan on `PATH` still gets a clean default build,
and CI never has to install a package manager it doesn't otherwise need.

`cpp/conan/conanfile.py` is a small, self-contained Conan 2 consumer that
pulls exactly one dependency:

```python
class ToolboxConanDemo(ConanFile):
    name = "toolbox-conan-demo"
    version = "1.0.0"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("fmt/11.0.2")

    def layout(self):
        # Generated files land in build/conan/generators/ (relative to the
        # --output-folder passed on the CLI), matching the "conan"
        # CMakePresets.json preset's CMAKE_TOOLCHAIN_FILE path.
        self.folders.generators = "generators"

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()
```

`self.folders.generators = "generators"` is the line that ties this file
to the preset above: it makes Conan write `conan_toolchain.cmake` into
`build/conan/generators/`, exactly where the `conan` preset's
`CMAKE_TOOLCHAIN_FILE` looks for it. Get that path wrong in either file and
`cmake --preset conan` fails to configure before a single line of C++ ever
compiles.

The lockfile is what makes this reproducible rather than "whatever `fmt`
version happens to resolve today": `cpp/conan/conan.lock` is committed and
pins one exact package revision, timestamp included:

```json
{
    "version": "0.5",
    "requires": [
        "fmt/11.0.2#7b5e2770c1ccb3d4af9b4f6762135645%1778145873.016"
    ],
    "build_requires": [],
    "python_requires": [],
    "config_requires": []
}
```

`conan install conan --output-folder=build/conan --build=missing
--lockfile=conan/conan.lock` resolves against that exact revision, not
whatever `fmt/11.0.2` happens to mean on the day someone else runs it — the
same discipline `go.sum`/`Cargo.lock` give the other two languages,
expressed in Conan's own lockfile format. On this host, that install, the
`conan` preset's configure and build, and the resulting binary all ran for
real:

```console
$ conan install conan -of build/conan --build=missing --lockfile=conan/conan.lock   # fmt/11.0.2 from lockfile
$ cmake --preset conan && cmake --build --preset conan                              # builds toolbox-conan-demo
$ ./build/conan/toolbox-conan-demo  →  toolbox-conan: fmt sub-target ok   (exit 0)
```

`toolbox-conan-demo`'s own source is three lines that only exist to prove
the link succeeded — it never touches `toolbox`'s digest, so nothing about
Conan can affect the GCC-vs-clang parity story below:

```cpp
#include <fmt/core.h>

int main() {
    fmt::print("toolbox-conan: fmt sub-target ok\n");
    return 0;
}
```

## GCC-vs-clang parity — and where the two compilers disagree

`toolbox report` exists to make a hard question answerable with a `diff`:
does the *exact same source*, built by two different compilers, produce
the exact same bytes? Making that question answerable at all takes care —
floats round differently, pointer values differ between runs, timestamps
differ by definition, and unordered-container iteration order is
compiler- and library-defined. `toolbox report` avoids every one of those
by construction: it hashes a fixed embedded payload with FNV-1a and prints
only integers and strings.

```cpp
struct Digest {
    std::uint64_t fnv;
};

// FNV-1a over a fixed, embedded byte array. Integer/string-only, no floats,
// no addresses, no timing, no unordered-container iteration -- the same
// input bytes must produce the same digest bit-for-bit on every conforming
// C++23 compiler, which is what the GCC-vs-clang parity gate (ch46 Sec.
// "GCC-vs-clang parity") relies on.
Digest fnv1a(const std::uint8_t* data, std::size_t len);
```

```cpp
int cmd_report() {
    const Digest d = fnv1a(kPayload.data(), kPayload.size());
    const std::string label = decorate_label("report");
    std::printf("toolbox report: label=%s payload_len=%zu digest=0x%016llx\n", label.c_str(),
                kPayload.size(), static_cast<unsigned long long>(d.fnv));
    return 0;
}
```

Built once with the `release` preset (GCC) and once with `release-clang`,
`toolbox report`'s output is identical to the byte, and it matches a fixed
literal this chapter pins in `verify.lua`:

```console
$ ./cpp/build/release/toolbox report
toolbox report: label=[toolbox] report payload_len=16 digest=0x481984990deee5ff
$ ./cpp/build/release-clang/toolbox report
toolbox report: label=[toolbox] report payload_len=16 digest=0x481984990deee5ff
$ diff <(release/toolbox report) <(release-clang/toolbox report)   →  (empty, exit 0)
```

That empty `diff` is the chapter's central claim, made falsifiable: two
independently built binaries, one compiled by GCC 16.1.1, one by clang
22.1.8, from the identical source, land on the identical 64-bit digest,
`0x481984990deee5ff`.

Byte-identical *output* does not mean byte-identical *diagnostics* — GCC
and clang disagree constantly about how to phrase the same finding, which
is worth seeing directly rather than taking on faith. Two comparisons run
this session, against this exact source, make the point at both ends of
the build: compile time and run time.

At compile time, a scratch copy of `toolbox.cpp` with one semicolon
deleted from `cmd_report` (`const std::string label =
decorate_label("report")`, no trailing `;`) gets two differently
*located* errors for the identical mistake:

```console
$ g++ -std=c++23 -c toolbox.cpp -o /dev/null
toolbox.cpp: In function 'int {anonymous}::cmd_report()':
toolbox.cpp:31:5: error: expected ',' or ';' before 'std'
   31 |     std::printf("toolbox report: label=%s payload_len=%zu digest=0x%016llx\n", label.c_str(),
      |     ^~~
$ clang++ -std=c++23 -c toolbox.cpp -o /dev/null
toolbox.cpp:30:55: error: expected ';' at end of declaration
   30 |     const std::string label = decorate_label("report")
      |                                                       ^
      |                                                       ;
1 error generated.
```

GCC keeps parsing past the missing `;` and reports the error where it
first becomes unrecoverable — line 31, the *next* statement — with a
message phrased around what it expected there (`','` or `';'` before
`std`). clang stops exactly where the semicolon should have gone — line
30, column 55, the true end of the broken declaration — and even offers a
fix-it caret pointing at the missing character. Same bug, two real and
differently useful answers to "where is it."

At run time, the seeded overflow this chapter uses for the UBSan gate
(below) shows the same kind of drift. The hard-gated `ubsan` preset builds
with GCC by default; building the identical flags with clang instead (an
ad hoc build, not one of the seven named presets) catches the same defect
one column earlier and adds a line GCC's runtime does not print:

```console
$ ./cpp/build/ubsan/toolbox defect overflow          # ubsan preset, GCC (default compiler)
toolbox defect overflow: max_value=2147483647
.../cpp/src/toolbox.cpp:43:34: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
$ clang++ -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined toolbox.cpp digest.cpp smell.cpp -o /tmp/toolbox-clang-ubsan
$ /tmp/toolbox-clang-ubsan defect overflow            # same flags, clang, ad hoc (not a named preset)
toolbox defect overflow: max_value=2147483647
toolbox.cpp:43:32: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior toolbox.cpp:43:32
```

Column 34 is the literal `1` in `max_value + 1`; column 32 is the `+`
operator itself — GCC's libubsan blames the operand, clang's blames the
operation, and only clang's runtime appends a `SUMMARY:` line by default.
Neither is wrong. Both are real diagnostics for the identical undefined
behavior, and `verify.lua`'s own assertion (quoted in "Errors, three ways"
below) only matches the fixed substring `runtime error: signed integer
overflow`, precisely because the rest of the line is not something either
compiler owes you stability on across versions.

## How the code works

`toolbox`'s whole source is four small translation units. `src/toolbox.cpp`
is the dispatcher: `report` computes the FNV-1a digest and prints it,
`defect overflow` seeds the UBSan trap, anything else prints usage and
exits 2.

```cpp
int cmd_defect_overflow() {
    // `volatile` blocks the compiler from constant-folding the overflow at
    // compile time -- the point is a *runtime* UBSan trap, not a compiler
    // diagnostic. This one statement is the entire seeded defect.
    volatile int max_value = INT_MAX;
    std::printf("toolbox defect overflow: max_value=%d\n", max_value);
    std::fflush(stdout);            // UBSan's trap handler may skip normal atexit flushing.
    int overflowed = max_value + 1; // UB: signed integer overflow.
    std::printf("unreachable: overflowed=%d\n", overflowed);
    return 0;
}
```

`volatile` is doing real work in that function, not decoration: without
it, a compiler is entitled to see `INT_MAX + 1` as a compile-time constant
expression and either fold it away or reject the translation unit outright
— neither of which is the point. Marking `max_value` `volatile` forces the
addition to happen at run time, where only a sanitizer or the CPU's own
overflow flag can catch it.

`src/digest.hpp`/`digest.cpp` hold the `Digest` type and the FNV-1a
implementation quoted above; `src/smell.hpp`/`smell.cpp` hold one
deliberate, behavior-preserving defect for clang-tidy to find (next
section). Nothing in any of the four files depends on which compiler built
it — no `#ifdef __clang__`, no compiler-specific pragma — which is exactly
what makes the parity claim a claim about the *language standard*, not
about code written to please one compiler over another.

{% include excalidraw.html
   file="46-toolbox-pipeline"
   alt="One source box, toolbox.cpp plus digest and smell translation units, splits into two build arrows -- one through GCC, one through clang -- each landing in its own build/<preset> binary. Both binaries feed into a report command box, and their outputs converge on an amber diff box marked empty, the byte-identical parity gate, digest=0x481984990deee5ff. Below the source box, a static-analysis arm branches off into clang-tidy (flagging performance-unnecessary-value-param in smell.cpp) and clang-format --dry-run --Werror (clean on tracked sources, nonzero on a misformatted fixture). A separate runtime arm branches into the ubsan build, which traps the seeded signed-integer overflow with an abort icon, and the debug build, which feeds gdb plus a Python pretty-printer producing Digest(0x481984990deee5ff). Two dashed accelerator boxes, ccache and mold, sit off to the side labeled shown, not verified on this host."
   caption="Figure 46.2 — the toolbox pipeline: one source built by two compilers into byte-identical output (the parity gate), with static tools (clang-tidy, clang-format) and runtime tools (UBSan, gdb pretty-printers) around it, and ccache/mold as optional accelerators" %}

## Errors, three ways

With one language and one binary, "three ways" means three different
*tools* surfacing an error about the same source, not three languages
producing the same message. The compile-time diagnostic above — GCC's
`expected ',' or ';' before 'std'` against clang's `expected ';' at end of
declaration` for an identical missing semicolon — is the first: a mistake
the compiler itself refuses to build past.

The second is a finding a compiler never flags at all, because nothing
about it is *wrong* — only wasteful. `src/smell.hpp` documents its own
defect:

```cpp
// Deliberate, behavior-preserving smell for the clang-tidy gate (ch46 Sec.
// "Errors, three ways" -- the tidy finding). `label` is only ever read, so
// taking it by value forces an avoidable copy on every call; clang-tidy's
// performance-unnecessary-value-param flags this parameter, and the fix is
// `const std::string&`. Left as `std::string` on purpose -- do not "fix"
// this without re-pinning the tidy chapter section.
std::string decorate_label(std::string label);
```

`src/smell.cpp` is the four-line definition the declaration above
describes, and its line 3 is exactly where the warning below lands:

```cpp
std::string decorate_label(std::string label) {
    return "[toolbox] " + label;
}
```

`clang-tidy -p build/release src/smell.cpp` reads the compile-commands
database `CMAKE_EXPORT_COMPILE_COMMANDS: ON` produces for every preset, and
flags exactly the parameter the comment predicts:

```console
$ clang-tidy -p build/release src/smell.cpp
/…/cpp/src/smell.cpp:3:40: warning: the parameter 'label' of type 'std::string' ... [performance-unnecessary-value-param]
```

`.clang-tidy` pins the check name, not the message wording, precisely
because message text drifts across clang-tidy releases the same way the
UBSan line did above:

```yaml
Checks: >
  bugprone-*,
  modernize-*,
  performance-*,
  readability-*,
  -modernize-use-trailing-return-type,
  -readability-identifier-length
WarningsAsErrors: ''
HeaderFilterRegex: '.*'
FormatStyle: file
```

The third is the UBSan runtime abort already introduced above, this time
as the pinned gate `verify.lua` actually runs:

```lua
local ubsan_run = checks.run("./cpp/build/ubsan/toolbox defect overflow")
checks.expect_exit(ubsan_run, 1, "cpp: ubsan build exits 1 on the seeded overflow")
checks.expect_match(ubsan_run.out, "runtime error: signed integer overflow",
  "cpp: UBSan reports 'runtime error: signed integer overflow'")
```

Three surfaces, three different moments a defect becomes visible: a
compiler refusing to finish parsing, a static analyzer reading a
compile-commands database with no execution at all, and a sanitizer
watching one specific arithmetic operation at run time — and every one of
them names a different line, or a different column on the same line, for
a reason grounded in how that particular tool works.

## Concurrency lens: the build graph's own parallelism

Every earlier concurrency lens in this book — Chapter 25's threads,
Chapter 44's GMP scheduler — asked how a *running program* multiplexes
work. This chapter's build has no threads of its own to watch, but the
tool building it does: Ninja's default job count is bounded by the host's
CPU count, and every one of `toolbox`'s translation units that has no
dependency on another can compile in parallel — `cmake --build --preset
release -- -j$(nproc)` (or simply `-j`, Ninja's own auto-detected default)
hands `digest.cpp`, `smell.cpp`, and `toolbox.cpp` to as many parallel
`g++`/`clang++` invocations as there are idle cores, then serializes only
at the final link, the one step that needs every object file to exist
first. Three translation units is too small a graph to *see* that
parallelism move the needle on this chapter's own build times, but the
dependency shape — many independent compiles fanning in to one serial
link — is the same shape a much larger C++ project's build graph has at
any size, just wider.

The link step itself is where a second, differently-parallel tool would
matter more directly: `mold`, "a Modern Linker," parallelizes symbol
resolution and section layout internally, the way a traditional
single-threaded linker (`ld.bfd`) never does — this book's own earlier
concurrency lenses (Chapter 44's M:N scheduler, Chapter 25's thread pool)
are about overlapping *independent* units of work, and `mold`'s internal
design is the identical idea applied to the one step Ninja itself cannot
parallelize away, because a link has to see every object file at once.
`mold` is demonstrated, not verified, in "lint, format, sanitize,
ccache, mold" below — it is absent on the reference host, so its
speed-up is a documented recipe here, not a measured number.

## gdb pretty-printers

`Digest` is a one-field aggregate, and without help gdb prints exactly
that — a raw struct, no more legible than reading the eight bytes by
hand. `cpp/toolbox-printers.py` teaches gdb what the field means:

```python
class DigestPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        fnv = int(self.val["fnv"]) & 0xFFFFFFFFFFFFFFFF
        return "Digest(0x{:016x})".format(fnv)


def _lookup(val):
    t = val.type.unqualified().strip_typedefs()
    if t.tag == "Digest":
        return DigestPrinter(val)
    return None


def register_printers():
    gdb.pretty_printers.append(_lookup)
```

`_lookup` is gdb's pretty-printer registration contract: given a value,
return a printer object if you know how to render its type, or `None` to
let gdb (or the next registered printer) try something else. Matching on
`t.tag == "Digest"` after `strip_typedefs()` means the printer fires
regardless of `const`/reference qualifiers on the value gdb hands it —
`Digest`, `const Digest&`, and `Digest&` all normalize to the same tag.
`cpp/.gdbinit` is what loads it:

```
set debuginfod enabled off
source cpp/toolbox-printers.py
```

Run from the example root against the `debug` preset's build, with a
breakpoint on the line that first constructs a `Digest`:

```console
$ gdb -batch -x cpp/.gdbinit -ex 'b toolbox.cpp:30' -ex run -ex 'p d' --args cpp/build/debug/toolbox report
Breakpoint 1, (anonymous namespace)::cmd_report () at .../toolbox.cpp:30
30      const std::string label = decorate_label("report");
$1 = Digest(0x481984990deee5ff)
```

`$1 = Digest(0x481984990deee5ff)` — the exact same digest the parity gate
printed above, this time read straight out of the stopped process's stack
by a debugger that would otherwise have shown `{fnv = 5202111637775040511}`
(the same 64-bit value, decimal, with no field name to explain what it
is). The pretty-printer changes nothing about what gdb *has*; it only
changes how much of that the type system throws away by default.

## Lint, format, sanitize, ccache, mold — the recipes

**clang-tidy** and **clang-format** were both exercised for real above and
in "Errors, three ways"; the format half of that pair is worth showing on
its own, because — unlike a `warning:` — a clean formatter run is silent,
and its *effect* is only visible in the exit code:

```console
$ clang-format --dry-run --Werror src/*.cpp src/*.hpp conan/src/*.cpp   # tracked sources
$ echo $?
0
$ clang-format --dry-run --Werror fixtures/misformatted.cpp             # deliberately misformatted, NOT built
$ echo $?
1
```

`cpp/fixtures/misformatted.cpp` exists for exactly this: a file
`clang-format --dry-run --Werror` is guaranteed to reject, kept out of
`CMakeLists.txt`'s `add_executable` sources entirely so it never affects a
build, only this one check. `.clang-format` pins the style being checked
against:

```yaml
BasedOnStyle: LLVM
Language: Cpp
Standard: Latest
IndentWidth: 4
ColumnLimit: 100
PointerAlignment: Left
AllowShortFunctionsOnASingleLine: Inline
```

**Sanitizing** is the `asan`/`ubsan` presets already covered above —
`cmake --preset ubsan && cmake --build --preset ubsan` is the exact
recipe `verify.lua` runs for the hard-gated overflow catch; `cmake
--preset asan` is the same idea without `-fno-sanitize-recover`, for
exploratory bug-hunting where you want the process to keep running and
report every violation instead of aborting on the first.

**ccache** and **mold** are the two tools this chapter shows without
being able to verify on the reference host — neither is installed here:

```console
$ command -v ccache; echo "exit=$?"
exit=1
$ command -v mold; echo "exit=$?"
exit=1
```

The recipe for each is real, just not run: adding
`"CMAKE_CXX_COMPILER_LAUNCHER": "ccache"` to a preset's `cacheVariables`
makes every compile invocation go through `ccache` first, which hashes the
preprocessed source plus compiler flags and serves a cache hit instead of
re-invoking the compiler on an unchanged translation unit — the second
build of an unchanged tree turning into a cache lookup instead of a
recompile is the effect a real run would show. `mold` swaps in as the
linker with `"CMAKE_EXE_LINKER_FLAGS": "-fuse-ld=mold"`, and a binary it
actually linked would carry its own name in the `.comment` ELF section,
checkable with `readelf -p .comment build/release/toolbox | grep mold`
with no debugger needed at all. Both recipes are shown here exactly as
they would be run; neither produced output on this host, and the status
footer below marks them accordingly.

## Build, run, observe

```console
[host]$ cd examples/46-cpp-toolbox && ./demo.sh cpp build
```

That builds the `release` preset (GCC). Hand-running the two compilers'
builds side by side is what "GCC-vs-clang parity" above quoted verbatim;
the deliberately seeded defects follow the same shape:

```console
[host]$ ./cpp/build/release/toolbox report
toolbox report: label=[toolbox] report payload_len=16 digest=0x481984990deee5ff
[host]$ cd cpp && cmake --preset ubsan && cmake --build --preset ubsan && cd ..
[host]$ ./cpp/build/ubsan/toolbox defect overflow
toolbox defect overflow: max_value=2147483647
.../cpp/src/toolbox.cpp:43:34: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
```

The full gate, exactly as run this session:

```console
[host]$ LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua
...
PASS 20 / FAIL 0
[host]$ python3 scripts/test-all-examples.py --only 46-cpp-toolbox
...
1 passed, 0 failed, 0 skipped
```

Twenty real assertions, none of them a bare exit code: the GCC/clang
parity chain (build both, run both, diff both, match the literal digest —
seven assertions on its own), the `ubsan` build and its exact runtime-error
substring, the `debug` build and gdb's pretty-printed `Digest`, the
`clang-tidy` check-name token, `clang-format`'s clean-tracked/dirty-fixture
pair, and the Conan install-configure-build-run chain ending in
`toolbox-conan: fmt sub-target ok`.

## Cross-check: two compilers, one digest

The claim this chapter is built around is falsifiable in the strictest
sense — either two independently-built binaries print the same 64-bit
number or they don't — and it was actually checked, not assumed. GCC
16.1.1 and clang 22.1.8 are different compiler front-ends, different
optimizers, and different code generators, sharing nothing but the C++23
standard and the source text `toolbox.cpp`/`digest.hpp`/`digest.cpp`
compile from. `diff <(release/toolbox report) <(release-clang/toolbox
report)` — empty, exit 0 — is the cross-check the parity section already
walked through in detail: it is not "the same shape of output," it is the
identical byte sequence, `digest=0x481984990deee5ff`, arrived at by two
compilers that disagree, demonstrably, about how to phrase both a
compile-time syntax error and a runtime UBSan trap for the same source.
That disagreement on *diagnostics* against agreement on *output* is the
whole point: the C++23 standard constrains what a program must compute,
not what a compiler must say while compiling or running it, and
`toolbox report` was written specifically to only ever exercise the part
both compilers are required to agree on.

`verify.lua`'s own parity assertions are the automated version of the same
check:

```lua
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
```

Three assertions, not one: both binaries individually match a fixed
literal (so neither compiler quietly drifted from the expected digest),
*and* they match each other directly via `diff`. Either check alone would
be suggestive; both together are the proof this section's opening claim
needed.

## Where this sits next to the book's other tooling chapters

Three earlier chapters already covered pieces of this ground, and this
chapter deliberately does not re-teach any of them:

- **Chapter 31** ("Per-Language Toolbelts") gave C++ one column of a
  three-language *profiling* comparison — `perf`, `clang-tidy`,
  `-fanalyzer` — next to Go's `pprof` and Rust's `cargo flamegraph`. This
  chapter is the opposite shape: one language, covered in depth, and it
  never profiles anything.
- **Chapter 28** ("gdb and Remote Debugging") is where `gdb`'s core
  mechanics live — reading a core file, `gdbserver`/`target remote`,
  `rust-gdb`'s type-aware rendering. This chapter assumes all of that and
  adds exactly one new idea: a pretty-printer *you write* for *your own*
  type, not one that ships with libstdc++.
- **Chapter 29** ("valgrind, Sanitizers, and Miri") built the full five-bug
  ASan/UBSan/TSan/valgrind/miri catch-or-miss matrix. This chapter uses
  exactly one corner of that matrix — UBSan on a signed-integer overflow —
  and spends its real depth budget on CMake presets and Conan lockfiles
  instead, ground Chapter 29 never touches.

What is genuinely new here — preset inheritance mechanics, a committed
Conan 2 lockfile, cross-compiler byte parity, a hand-written gdb Python
printer, and `.clang-tidy`/`.clang-format` authored rather than merely
invoked — has no earlier chapter to point back to instead.

## What you learned

- **`CMakePresets.json`'s inheritance is the whole mechanism**: a hidden
  `_base` preset supplies `generator`, {% raw %}`binaryDir:
  "${sourceDir}/build/${presetName}"`{% endraw %}, and compile-commands
  export once; `release-clang`/`debug-clang` inherit from `release`/`debug`
  (not `_base`) and override only the compiler, and `asan` vs `ubsan`
  differ by exactly one flag (`-fno-sanitize-recover=undefined`) that turns
  "report every violation" into "abort deterministically on the first."
- **A Conan sub-target can be real and still not gate the default build**:
  `cpp/conan/`'s `conanfile.py` + committed `conan.lock` (`fmt/11.0.2`,
  exact revision) only builds when the isolated `conan` preset turns
  `TOOLBOX_ENABLE_CONAN` on — the `release` preset, `./demo.sh cpp build`,
  and fedora:44's CI build-smoke never evaluate `cpp/conan/` at all.
- **Byte-identical output and identical diagnostics are different
  claims**: GCC 16.1.1 and clang 22.1.8 produced the exact same
  `digest=0x481984990deee5ff` for `toolbox report` (empty `diff`), while
  disagreeing on where to anchor a missing-semicolon error (line 31 vs.
  line 30, column 55) and how to phrase the identical UBSan trap (column
  34 vs. 32, with or without a `SUMMARY:` line) — the standard fixes the
  first, not the second.
- **A pretty-printer is a lookup function, not magic**: `toolbox-printers.py`'s
  `_lookup` matches on `val.type.unqualified().strip_typedefs().tag ==
  "Digest"` and hands back an object with a `to_string()` method — the same
  three-line contract behind every pretty-printer libstdc++ ships, applied
  here to a type this chapter wrote itself.
- **Not every recipe in a toolchain chapter can be verified on every
  host**: `ccache` and `mold` are real, documented recipes — a
  `CMAKE_CXX_COMPILER_LAUNCHER` cache variable and a `.comment`-section
  check — that this chapter shows without a cache hit or a linked binary
  to point at, because neither tool is installed on the reference host;
  the footer below marks exactly that gap and nothing more.

Two appendices remain in Part 13: the Go and Rust toolchains get the same
reference-depth treatment this chapter gave C++.

---

<p><span class="status status--verified">verified</span> — on the Fedora 44
reference host this session (kernel 7.1.4-204.fc44, GCC 16.1.1, clang
22.1.8, CMake 4.3.0, Ninja, gdb 17.2, Conan 2.30.0):
<code>LSP_LANG=cpp REPO_ROOT=$(cd ../.. && pwd) lua verify.lua</code> reported
<code>PASS 20 / FAIL 0</code>, and
<code>python3 scripts/test-all-examples.py --only 46-cpp-toolbox</code>
reported <code>1 passed, 0 failed, 0 skipped</code>. Confirmed live and
folded into that PASS count: the <code>release</code> (GCC) and
<code>release-clang</code> builds of <code>toolbox report</code> printed
byte-identical output (<code>digest=0x481984990deee5ff</code>) with an
empty <code>diff</code>; the <code>ubsan</code> preset caught the seeded
overflow at <code>toolbox.cpp:43</code> with
<code>runtime error: signed integer overflow</code> and exit 1;
<code>gdb -batch -x cpp/.gdbinit ...</code> rendered
<code>Digest(0x481984990deee5ff)</code> via the Python pretty-printer;
<code>clang-tidy</code> flagged <code>performance-unnecessary-value-param</code>
at <code>smell.cpp:3</code>; <code>clang-format --dry-run --Werror</code>
was clean on the tracked sources and nonzero on
<code>fixtures/misformatted.cpp</code>; and
<code>conan install --lockfile=conan/conan.lock</code> plus the
<code>conan</code> preset built and ran
<code>toolbox-conan: fmt sub-target ok</code>. The GCC-vs-clang
diagnostic-wording comparisons (the missing-semicolon compile error and the
ad hoc clang-built UBSan trap) were captured live this session against this
same source but are illustrative asides, not part of <code>verify.lua</code>'s
gated count — wording like this is expected to drift across compiler point
releases, which is why the gate itself only matches the fixed substring
<code>runtime error: signed integer overflow</code>. Not exercised:
<code>ccache</code> and <code>mold</code> are both <span class="status
status--unverified">unverified</span> — <code>command -v ccache</code> and
<code>command -v mold</code> both exit 1 on this host, so their recipes
above are shown as documented commands, with no cache-hit or linked-binary
output to quote. <code>examples/manifest.yaml</code> marks this example
<code>mode: local</code> — no VM or LGTM path applies.</p>
