---
title: "r26 — the full-matrix gate run (internal)"
layout: plan
render_with_liquid: false
published: false
---

# r26 — the authoritative gate, run across the finished book

`CLAUDE.md` names this run as the authoritative gate:

> CI (`validate.yml`) checks site integrity + build smoke only; the authoritative
> gate is a host run of the runner with the lab up, recorded in `_plans/`.

**It had never been run across the whole book.** Every iteration from r14 onward
ran `--only <example>`, which verifies the chapter being written and nothing
else. This is that run, and it found two real environment defects that per-example
runs structurally cannot.

## Result

```
python3 scripts/test-all-examples.py --mode all --jobs 4
137 passed, 0 failed, 0 skipped
```

**137 example-language combinations across 56 examples, zero skips.** The lab was
up (`systems-target` + `systems-peer`) and the LGTM stack was healthy, so nothing
degraded to SKIP — the VM examples and the two LGTM-dependent ones (38, 41) all
ran for real. JUnit at `build-logs/r26-full-matrix.xml`.

## Environment

| | |
| --- | --- |
| host kernel | 7.1.8-200.fc44 |
| host glibc / libstdc++ | 2.43 / **16.2.1-2.fc44** (`libstdc++.so.6.0.36`) |
| host toolchain | g++ 16.1.1, clang 22.1.8, CMake 4.3.0, Ninja 1.13.0 (at `~/.local/bin/ninja`), Conan 2.30.0, Lua 5.4.8 |
| guest kernel | 6.19.10-300.fc44 |
| guest libstdc++ | **16.2.1-2.fc44 after the fix below** (was 16.1.1-2) |
| LGTM | `lsp-lgtm` container, healthcheck `healthy` |

## Defect 1 — stale CMake caches (41 failures, first run)

Every C++ example failed to configure:

```
CMake Error at CMakeLists.txt:2 (project):
  Running '/usr/bin/ninja-build' '--version' failed with: no such file or directory
```

Fifty `examples/*/cpp/build/` trees held caches pinning
`CMAKE_MAKE_PROGRAM=/usr/bin/ninja-build`, a path that no longer exists on this
host — ninja now lives at `~/.local/bin/ninja`. ch55 and ch56 were unaffected
because they had been configured fresh that session.

**Not a repository defect**: `build/` is gitignored, and a clean checkout (what
CI does) configures correctly. It is a defect of *this working tree*, and the
kind that a per-example run hides indefinitely because the example you are
writing is always freshly configured.

**Fixed** by deleting all 50 gitignored build trees and rebuilding from clean —
which is also the stronger gate, since it proves every example configures from
nothing.

## Defect 2 — the lab guest could not run host-built C++ (6 failures, second run)

After the clean rebuild, six VM examples failed — 14, 30, 32, 33, 37, 40 — all
`verify exit 1`, **all C++ only, with Go and Rust passing on the same guest in the
same run.** That asymmetry is the whole diagnosis: Go and Rust link statically and
do not care what the guest ships.

The verify output blamed the demo (`port_never_opened`, missing Landlock lines,
an OOM that did not happen). Running the binary by hand gave the real error:

```
/home/fedora/app: /lib64/libstdc++.so.6: version `GLIBCXX_3.4.36' not found
```

| | libstdc++ | soname | max GLIBCXX |
| --- | --- | --- | --- |
| host | 16.2.1-2 | `.so.6.0.36` | **3.4.36** |
| guest (before) | 16.1.1-2 | `.so.6.0.35` | 3.4.35 |

The host took a `libstdc++` update on 2026-08-18 that the lab guest never
received. Binaries built on the host after that date require a symbol the guest
does not have, so **every C++ VM demo had been silently broken for ten days** and
no per-example run would have said so unless it happened to be a VM example.

Note the trap in the diagnosis: `rpm -q libstdc++` on the guest reported
`16.1.1-2` while `g++ --version` on the *host* also reports 16.1.1 — the compiler
package and the runtime package are versioned separately, and it is the runtime
that moved. Comparing compiler versions would have shown a match and proved
nothing.

**Fixed** with `sudo dnf -y update libstdc++ libgcc` on `systems-target`; the
guest now provides `GLIBCXX_3.4.36` and all seven VM cpp examples pass.

### RESOLVED (2026-08-28) — both snapshots refreshed

The snapshots were dated **2026-07-18** and predated the host's update, so any
revert (`scripts/lab/revert-vm.sh`, or the runner's `--revert-between`) restored
libstdc++ 16.1.1 and re-broke every C++ VM demo.

`systems-peer` was found to carry **the same drift** (16.1.1-2, max
`GLIBCXX_3.4.35`). The full matrix had not caught it because no C++ binary is
deployed to the peer, but snapshotting it unchanged would have baked the same
latent defect back in.

Both guests were updated (`dnf -y update libstdc++ libgcc` → 16.2.1-2,
`GLIBCXX_3.4.36`), their accumulated run artifacts removed — `~/app`, `~/JSON`,
`/tmp/lsp*` on the target; `~/chatterd`, `~/beacons.txt`, `~/peer.{out,err}`,
`~/tcpdump.err` on the peer, all of which postdated the July snapshot and would
have made the new baseline dirtier than the old one — and both re-snapshotted:

```
lab-ready   2026-08-28 21:20:04 -0400   running   (systems-target)
lab-ready   2026-08-28 21:20:10 -0400   running   (systems-peer)
```

**Verified by reverting, not by assuming.** After
`revert-vm.sh systems-target lab-ready`, the guest reported
`libstdc++-16.2.1-2.fc44` / `GLIBCXX_3.4.36`, and:

```
python3 scripts/test-all-examples.py --mode vm --lang cpp
8 passed, 0 failed, 0 skipped
```

All eight VM cpp examples, including `41-capstone-fleet`, which spans both guests
and LGTM. The fix survives a revert, which is what the defect required.

Still worth doing: a **ch02 troubleshooting note**, since this is reader-facing.
Any reader whose host outpaces their lab guest hits it, and it presents as a
broken demo rather than a version problem.

## Also found, not fixed — ch39 violates the project's own banned-words rule

`CLAUDE.md` says, in the banned-words section:

> Same rule for "lie"/"lies"/"lying" **when the subject is the work, the method,
> or the reader** ("benchmarking without lies", "the lie this chapter corrects")
> […] Name a deliberately-bad variant after its defect (`--naive`,
> `--unwarmed`), never `--lie`: flag names propagate into all three languages,
> `verify.lua`, the recorded chapter output, and the example slug.

The rule quotes this chapter verbatim as its example of what not to do. Chapter 39
is titled **"Benchmarking without lies"**, lives at
`_docs/39-benchmarking-without-lies.md`, and its example still ships a `--lie`
flag in all three languages. The rule was evidently written from this experience
and never applied back to it.

Blast radius, measured: the `--lie` flag appears in five files (`cpp/src/main.cpp`,
`go/main.go`, `rust/src/main.rs`, `verify.lua`, `README.md`); the slug appears in
the chapter, the manifest, the example's `verify.lua` and `README.md`, and
`_plans/r09-batch1-stub-completion.md`. **No other chapter cites ch39 by slug or
title** — the references to it elsewhere (ch54, ch55, ch56) are all "Chapter 39"
by number, so a rename would not break cross-references. It would change a
reader-facing URL.

Left alone deliberately: renaming a published chapter is the user's call.

## What this run argues for

Per-example verification is necessary and is not sufficient. Both defects here
were invisible to `--only` runs by construction — one lived in build trees that
the example under test always rebuilds, the other in a guest that only VM examples
touch. A full-matrix run belongs at the end of every iteration that touches the
toolchain, or on a schedule.
